#include <jimple-frontend/AST/jimple_expr.h>
#include <jimple-frontend/AST/jimple_hierarchy.h>
#include <util/arith_tools.h>
#include <util/c_sizeof.h>
#include <util/c_typecast.h>
#include <util/c_types.h>
#include <util/namespace.h>
#include <util/expr_util.h>
#include <util/ieee_float.h>
#include <util/std_code.h>
#include <util/std_expr.h>
#include <util/std_types.h>

// Resolve a symbol that MUST exist by this point in conversion. A missing
// symbol means the input module referenced something it never declared (e.g. a
// producer emitted an invoke to a constructor/method, or a field/local, it did
// not emit). Fail loudly naming the offending symbol instead of dereferencing a
// null pointer -- the latter segfaults the whole frontend and hides the cause.
static symbolt &require_symbol(contextt &ctx, const std::string &name)
{
  symbolt *s = ctx.find_symbol(name);
  if (s == nullptr)
  {
    log_error("Could not find symbol {}", name);
    abort();
  }
  return *s;
}

// True when `recv`'s pointee is a struct that actually carries the runtime
// class-id component. Opaque/unresolved class types resolve to
// pointer_typet(empty_typet()) (jimple_type::get_base_type) and have NO such
// component, so reading/writing `@class_identifier` on them would name a
// non-existent member. Both the dispatch read and the `new` write gate on this.
bool jimple_has_class_id(const exprt &recv)
{
  typet t = recv.type();
  if (t.id() == "pointer")
    t = t.subtype();
  if (t.id() != "struct")
    return false;
  const std::string field =
    "tag-" + std::string(jimple_hierarchy::class_id_field());
  for (const auto &comp : to_struct_type(t).components())
    if (comp.get_name() == field)
      return true;
  return false;
}

// Build the lvalue `recv->@class_identifier` (an int) for a receiver expression
// that is a pointer-to-struct. Used to WRITE the runtime class id at `new` and
// READ it back at a virtual call for dispatch. `recv` is consumed. The caller
// must have verified jimple_has_class_id(recv) first.
exprt jimple_class_id_member(exprt recv)
{
  member_exprt op(
    recv,
    "tag-" + std::string(jimple_hierarchy::class_id_field()),
    signedbv_typet(32));
  exprt &base = op.struct_op();
  if (base.type().is_pointer())
  {
    exprt deref("dereference");
    deref.type() = base.type().subtype();
    deref.move_to_operands(base);
    base.swap(deref);
  }
  return std::move(op);
}

// ---------------------------------------------------------------------------
// Engine-native java.lang.String
//
// The producer ships java.lang.String as a Jimple model with a `char[] value`
// field and routes every method (length/charAt/equals/...) through a backing()
// that returns that array. This is dead cost on the hot path of nearly every
// proof: a constructed-but-never-inspected String (above all an exception
// message) still materialises a char array and, under --unwind, loops over it,
// and a String whose `value` was never written (the producer strips literal
// content) faults on the nondet backing.
//
// Instead we treat String as an opaque reference object carrying a single
// symbolic non-negative `@string_length` int, and lower its methods to sound
// axiomatic expressions WITHOUT touching a char array. Content reasoning stays
// sound because the producer has already discarded the characters: across the
// real proofs the only content a proof inspects is length()/isEmpty() (handled
// exactly) and charAt (bounds-checked, nondet char) -- never specific bytes.
// The struct keeps its `value` component so the StringBuilder/CharSequence
// models that read String.value still type-check (they read nondet, a sound
// over-approximation).
// ---------------------------------------------------------------------------

const char *jimple_string_length_field()
{
  return "@string_length";
}

bool jimple_is_string_class(const std::string &class_name)
{
  return class_name == "java.lang.String";
}

bool jimple_has_string_length(const exprt &recv)
{
  typet t = recv.type();
  if (t.id() == "pointer")
    t = t.subtype();
  if (t.id() != "struct")
    return false;
  const std::string field = "tag-" + std::string(jimple_string_length_field());
  for (const auto &comp : to_struct_type(t).components())
    if (comp.get_name() == field)
      return true;
  return false;
}

exprt jimple_string_length_member(exprt recv)
{
  member_exprt op(
    recv,
    "tag-" + std::string(jimple_string_length_field()),
    signedbv_typet(32));
  exprt &base = op.struct_op();
  if (base.type().is_pointer())
  {
    exprt deref("dereference");
    deref.type() = base.type().subtype();
    deref.move_to_operands(base);
    base.swap(deref);
  }
  return std::move(op);
}

// A fresh nondet 32-bit int (an unconstrained String length / hash result). Not
// constrained `>= 0`: an unknown length reading negative is the SOUND direction
// (at worst a spurious FAILED, never a false VERIFIED). `new String` separately
// assumes its own @string_length >= 0 (jimple_statement.cpp).
static exprt nondet_int(contextt &ctx, const std::string &fn)
{
  jimple_nondet nd;
  exprt v = nd.to_exprt(ctx, "java.lang.String", fn);
  v.type() = signedbv_typet(32);
  return v;
}

exprt jimple_string_lower(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name,
  const std::string &method,
  const exprt &recv,
  const std::vector<exprt> &args)
{
  // method carries the producer's "_<argcount+1>" (instance) suffix; compare on
  // the bare name so dispatch is independent of the arity-hash convention.
  std::string name = method;
  auto us = name.rfind('_');
  if (us != std::string::npos)
    name = name.substr(0, us);

  const bool have_len = !recv.is_nil() && jimple_has_string_length(recv);

  // length(): read the object's @string_length. A String from `new` carries an
  // assumed-non-negative length (see jimple_statement.cpp); one with no
  // @string_length component (a nondet/opaque reference: a parameter, a
  // substring result) falls back to an unconstrained nondet -- sound (an unknown
  // length reading negative can only over-approximate, never false-VERIFY).
  if (name == "length")
  {
    if (have_len)
      return jimple_string_length_member(recv);
    return nondet_int(ctx, function_name);
  }

  // isEmpty(): length == 0.
  if (name == "isEmpty")
  {
    exprt len =
      have_len ? jimple_string_length_member(recv) : nondet_int(ctx, function_name);
    return typecast_exprt(
      equality_exprt(len, from_integer(0, signedbv_typet(32))),
      signedbv_typet(32));
  }

  // charAt(i): a nondet char. The Java StringIndexOutOfBounds bound (i<0 ||
  // i>=length) is NOT modelled -- this is a sound but imprecise over-
  // approximation (a missing bound can only miss a would-be exception, never
  // forge a counterexample). No proof in the corpus inspects character values,
  // so precision here is not load-bearing; a real bounds assertion is a TODO.
  if (name == "charAt" && args.size() == 1)
  {
    jimple_nondet nd;
    exprt c = nd.to_exprt(ctx, class_name, function_name);
    c.type() = unsignedbv_typet(16); // Java char
    return c;
  }

  // hashCode/compareTo/indexOf/lastIndexOf: a nondet int result. Distinct
  // strings may share a hash; compareTo's sign is unconstrained. Sound.
  if (
    name == "hashCode" || name == "compareTo" || name == "indexOf" ||
    name == "lastIndexOf")
  {
    jimple_nondet nd;
    exprt v = nd.to_exprt(ctx, class_name, function_name);
    v.type() = signedbv_typet(32);
    return v;
  }

  // equals/startsWith/endsWith/contains/matches: nondet boolean, EXCEPT
  // reflexive equality (s.equals(s)) is true -- the one identity JBMC also pins
  // and a frequent guard. Returned as an int (Java boolean width).
  if (
    name == "equals" || name == "startsWith" || name == "endsWith" ||
    name == "contains" || name == "matches")
  {
    if (name == "equals" && args.size() == 1 && !recv.is_nil())
    {
      // recv == arg ? 1 : nondet-bool
      jimple_nondet nd;
      exprt nbool = nd.to_exprt(ctx, class_name, function_name);
      nbool.type() = signedbv_typet(32);
      return if_exprt(
        equality_exprt(recv, args[0]),
        from_integer(1, signedbv_typet(32)),
        nbool);
    }
    jimple_nondet nd;
    exprt v = nd.to_exprt(ctx, class_name, function_name);
    v.type() = signedbv_typet(32);
    return v;
  }

  // Methods that RETURN a String (substring/toLowerCase/trim/replace/...): a
  // fresh opaque String reference. Modelled as a nondet reference -- it carries
  // no @string_length, so a later length() on it falls back to a fresh
  // non-negative nondet (sound). Content is unconstrained, which is sound since
  // the producer has discarded the characters and no proof inspects the derived
  // bytes.
  if (
    name == "substring" || name == "toLowerCase" || name == "toUpperCase" ||
    name == "trim" || name == "replace" || name == "concat" ||
    name == "toString" || name == "valueOf" || name == "intern" ||
    name == "strip" || name == "format" || name == "join")
  {
    jimple_nondet nd;
    return nd.to_exprt(ctx, class_name, function_name);
  }

  // Not a recognised String intrinsic.
  return nil_exprt();
}

void jimple_constant::from_json(const json &j)
{
  j.at("value").get_to(value);
  if (j.contains("fpwidth"))
    j.at("fpwidth").get_to(fp_width);
}

exprt jimple_constant::to_exprt(
  contextt &,
  const std::string &,
  const std::string &) const
{
  // A null reference constant (e.g. an optional coroutine/Continuation arg).
  if (value == "null")
    return gen_zero(pointer_typet(empty_typet()));

  // IEEE float/double literal. The producer marks these with fpwidth (32/64) and emits the value as
  // decimal text (or Java's "NaN"/"Infinity"/"-Infinity"); without this the value would fall through
  // to the integer path below and std::stoll would throw on the decimal point. Build the constant at
  // the matching precision so it shares the operand type the jimple frontend assigns floats/doubles.
  if (fp_width != 0)
  {
    ieee_floatt f(
      fp_width == 32 ? ieee_float_spect::single_precision()
                     : ieee_float_spect::double_precision());
    if (value == "NaN")
      f.make_NaN();
    else if (value == "Infinity")
      f.make_plus_infinity();
    else if (value == "-Infinity")
      f.make_minus_infinity();
    else
      f.from_double(std::stod(value));
    return f.to_expr();
  }

  // Integer literal. Parse as 64-bit so long constants (> 2^31) don't overflow,
  // and let from_integer encode the value at the type's full width. The
  // assignment/binop typecasts narrow it to the destination width where needed.
  // The previous code passed 10 as the integer2binary *width* argument (it is a
  // bit width, not a base), so any value >= 2^10 was silently truncated -- e.g.
  // 65536 was encoded as 0, corrupting every large constant.
  BigInt as_number(std::stoll(value));
  return from_integer(as_number, signedbv_typet(64));
};

void jimple_symbol::from_json(const json &j)
{
  j.at("value").get_to(var_name);
}

exprt jimple_symbol::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // 1. Look over the local scope
  auto symbol_name = get_symbol_name(class_name, function_name, var_name);

  // @caughtexception is the catch-handler binding (Soot's JCaughtExceptionRef). The frontend does
  // not yet track try/catch regions, so the symbol is never declared by a parameter/identity pass.
  // Declare it on demand as an unconstrained reference (the caught exception object, contents
  // unmodelled) instead of aborting on the missing symbol -- this lets a method containing a catch
  // handler convert.
  if (var_name == "@caughtexception" && ctx.find_symbol(symbol_name) == nullptr)
  {
    symbolt caught = create_jimple_symbolt(
      pointer_typet(empty_typet()),
      class_name,
      var_name,
      symbol_name,
      function_name);
    caught.lvalue = true;
    caught.file_local = true;
    ctx.move_symbol_to_context(caught);
  }

  symbolt &s = require_symbol(ctx, symbol_name);

  // TODO:
  // 2. Look over the class scope
  // 3. Look over the global scope (possibly don't need)

  return symbol_expr(s);
};

std::shared_ptr<jimple_expr> jimple_expr::get_expression(const json &j)
{
  std::string expr_type;
  if (!j.contains("expr_type"))
  {
    jimple_constant c;
    c.setValue("0");
    return std::make_shared<jimple_constant>(c);
  }

  j.at("expr_type").get_to(expr_type);

  // TODO: hashmap, the standard is not stable enough yet
  // It is still a work in progress in the parser: https://github.com/rafaelsamenezes/jimple_parser
  if (expr_type == "constant")
  {
    jimple_constant c;
    c.from_json(j);
    return std::make_shared<jimple_constant>(c);
  }

  if (expr_type == "string_constant")
  {
    // A string LITERAL. The char-array String model uses these (e.g. append(null) -> "null"); the old
    // code built an EMPTY jimple_constant whose to_exprt ran std::stoll("") and ABORTED the frontend,
    // which is why the String model could not be fed. Model the literal as a nondet String reference
    // for now (no crash) -- a faithful char-array materialisation of the literal's bytes is a TODO.
    return std::make_shared<jimple_nondet>();
  }

  if (expr_type == "class_reference")
  {
    jimple_constant c("-1");
    return std::make_shared<jimple_constant>(c);
  }

  if (expr_type == "symbol")
  {
    jimple_symbol c;
    c.from_json(j);
    return std::make_shared<jimple_symbol>(c);
  }

  if (expr_type == "static_invoke")
  {
    jimple_expr_invoke c;
    c.from_json(j);
    return std::make_shared<jimple_expr_invoke>(c);
  }

  if (expr_type == "virtual_invoke")
  {
    jimple_virtual_invoke c;
    c.from_json(j);
    return std::make_shared<jimple_virtual_invoke>(c);
  }

  if (expr_type == "binop")
  {
    jimple_binop c;
    c.from_json(j);
    return std::make_shared<jimple_binop>(c);
  }

  if (expr_type == "cast")
  {
    jimple_cast c;
    c.from_json(j);
    return std::make_shared<jimple_cast>(c);
  }

  if (expr_type == "lengthof")
  {
    jimple_lengthof c;
    c.from_json(j);
    return std::make_shared<jimple_lengthof>(c);
  }

  if (expr_type == "newarray")
  {
    jimple_newarray c;
    c.from_json(j);
    return std::make_shared<jimple_newarray>(c);
  }

  if (expr_type == "new")
  {
    jimple_new c;
    c.from_json(j);
    return std::make_shared<jimple_new>(c);
  }

  if (expr_type == "array_index")
  {
    jimple_deref c;
    c.from_json(j);
    return std::make_shared<jimple_deref>(c);
  }

  if (expr_type == "nondet")
  {
    jimple_nondet c;
    return std::make_shared<jimple_nondet>(c);
  }

  if (expr_type == "static_member")
  {
    jimple_static_member c;
    c.from_json(j.at("signature"));
    return std::make_shared<jimple_static_member>(c);
  }

  if (expr_type == "local_member")
  {
    jimple_virtual_member c;
    c.from_json(j);
    return std::make_shared<jimple_virtual_member>(c);
  }

  log_error("Unexpected expr type: {}", expr_type);
  abort();
}

void jimple_binop::from_json(const json &j)
{
  j.at("operator").get_to(binop);
  // TODO, make hashmap for each operator
  if (binop == "==")
    binop = "=";
  lhs = get_expression(j.at("lhs"));
  rhs = get_expression(j.at("rhs"));
}

exprt jimple_binop::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto lhs_expr = lhs->to_exprt(ctx, class_name, function_name);
  auto rhs_expr = rhs->to_exprt(ctx, class_name, function_name);

  // Coerce the operands to a common type. Integer literals are encoded 64-bit
  // (see jimple_constant) while locals carry their declared width, so a binop
  // mixing a local with a literal (e.g. `intLocal >= 0`) would otherwise build
  // an expression over mismatched bitvector widths and the solver rejects it.
  if (lhs_expr.type() != rhs_expr.type())
  {
    c_typecastt c_typecast(ctx);
    c_typecast.implicit_typecast(rhs_expr, lhs_expr.type());
  }

  // Jimple 3-way compare: lcmp (long), fcmpl/fcmpg (float), dcmpl/dcmpg (double).
  // Each yields an int -1/0/1: a<b -> -1, a==b -> 0, a>b -> 1. The "l"/"g" forms
  // differ only in NaN handling -- fcmpl/dcmpl yield -1 when either operand is NaN,
  // fcmpg/dcmpg yield +1. gen_binary has no single node for this, so lower it to a
  // nested conditional. (javac emits these for every <, <=, >, >= over long/float/
  // double, then branches on the int result.)
  if (binop == "cmp" || binop == "cmpl" || binop == "cmpg")
  {
    typet i32 = signedbv_typet(32);
    exprt neg1 = from_integer(-1, i32);
    exprt zero = from_integer(0, i32);
    exprt pos1 = from_integer(1, i32);
    exprt lt = gen_binary("<", bool_type(), lhs_expr, rhs_expr);
    exprt gt = gen_binary(">", bool_type(), lhs_expr, rhs_expr);
    exprt result = if_exprt(lt, neg1, if_exprt(gt, pos1, zero));
    // Only the float/double forms can see NaN; long cmp cannot.
    if ((binop == "cmpl" || binop == "cmpg") && lhs_expr.type().is_floatbv())
    {
      exprt isnan_l("isnan", bool_type());
      isnan_l.copy_to_operands(lhs_expr);
      exprt isnan_r("isnan", bool_type());
      isnan_r.copy_to_operands(rhs_expr);
      exprt nan_val = (binop == "cmpl") ? neg1 : pos1;
      result = if_exprt(or_exprt(isnan_l, isnan_r), nan_val, result);
    }
    return result;
  }

  // Relational operators yield a boolean; arithmetic operators yield the
  // (now common) operand type. The solver requires the relation node itself to
  // carry bool, otherwise an assert/assume over the result mismatches sorts.
  bool is_relational = binop == "=" || binop == "notequal" || binop == "<" ||
                       binop == "<=" || binop == ">" || binop == ">=";
  typet result_type = is_relational ? typet("bool") : lhs_expr.type();

  return gen_binary(binop, result_type, lhs_expr, rhs_expr);
};

void jimple_cast::from_json(const json &j)
{
  jimple_type type;
  j.at("to").get_to(type);
  to = std::make_shared<jimple_type>(type);
  from = get_expression(j.at("from"));
}

exprt jimple_cast::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto from_expr = from->to_exprt(ctx, class_name, function_name);
  c_typecastt c_typecast(ctx);

  c_typecast.implicit_typecast(from_expr, to->to_typet(ctx));
  return from_expr;
};

void jimple_lengthof::from_json(const json &j)
{
  from = get_expression(j.at("expression"));
}

exprt jimple_lengthof::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto expr = from->to_exprt(ctx, class_name, function_name);

  // Create a function call for allocation
  code_function_callt call;
  auto alloca_symbol = get_lengthof_function();

  symbolt &added_symbol = *ctx.move_symbol_to_context(alloca_symbol);

  call.function() = symbol_expr(added_symbol);

  call.arguments().push_back(expr);

  // Create a sideffect call to represent the allocation
  side_effect_expr_function_callt sideeffect;
  sideeffect.function() = call.function();
  sideeffect.arguments() = call.arguments();
  sideeffect.location() = call.location();
  sideeffect.type() =
    static_cast<const typet &>(call.function().type().return_type());
  return sideeffect;
};

void jimple_newarray::from_json(const json &j)
{
  size = get_expression(j.at("size"));
  jimple_type t;
  j.at("type").get_to(t);
  type = std::make_shared<jimple_type>(t);
}

void jimple_new::from_json(const json &j)
{
  size = std::make_shared<jimple_constant>("1");
  jimple_type t;
  j.at("type").get_to(t);
  type = std::make_shared<jimple_type>(t);
}

// True if [t] is (a pointer to) an enum's struct -- recognised by the synthesised `__ordinal` field the
// java.lang.Enum model contributes to every enum subclass.
static bool is_enum_alloc(const typet &t)
{
  const typet &st = t.is_pointer() ? t.subtype() : t;
  if (st.id() != "struct")
    return false;
  for (const auto &comp : to_struct_type(st).components())
    if (comp.get_name().as_string().find("__ordinal") != std::string::npos)
      return true;
  return false;
}

// An allocation in a class's STATIC INITIALISER (`<clinit>`/`$values`) escapes into a static field and
// must outlive the call (enum constants -> NAME statics; `$VALUES`; a `when`'s `$SwitchMap` int[]).
static bool in_static_init(const std::string &function_name)
{
  return function_name.find("clinit") != std::string::npos ||
         function_name.find("$values") != std::string::npos;
}

// MODEL classes (the bundled JDK/Kotlin/bmc4j operational models) allocate heavily in their own
// initialisers; heap-allocating all of those explodes the dynamic-memory VC count. Their static state
// rarely escapes into proof-visible reads, so keep them on cheap stack temps and reserve the heap path
// for USER code (and any enum). This keeps the suite fast while fixing user static-init dangles.
static bool is_model_class(const std::string &class_name)
{
  static const char *prefixes[] = {
    "java.",
    "javax.",
    "kotlin.",
    "kotlinx.",
    "jdk.",
    "sun.",
    "scala.",
    "org.cprover.",
    "org.bmc4j."};
  for (const char *p : prefixes)
    if (class_name.rfind(p, 0) == 0)
      return true;
  return false;
}

// Heap-allocate (cpp_new) when the object needs to outlive its allocating frame: any enum constant/array,
// or a user class's static-init escape. Everything else stays a cheap stack temp.
static bool needs_heap_alloc(
  const typet &element_type,
  const std::string &class_name,
  const std::string &function_name)
{
  // Enum constant/array (always), or USER code allocating an OBJECT (struct -- heap + zero-init so its
  // fields read the Java default, e.g. a `lateinit` field reads null) or a static-init array. Model
  // classes keep cheap stack temps. (Heap objects each add per-deref dynamic-memory VCs -> slower, but
  // correct: a stack-temp `new` reads its uninitialised fields as nondet.)
  return is_enum_alloc(element_type) ||
         (!is_model_class(class_name) &&
          ((element_type.id() == "struct" && function_name != "main") ||
           in_static_init(function_name)));
}

// Build the C++-style `cpp_new`/`cpp_new[]` heap allocation side effect. goto_convert's do_cpp_new reads
// the element COUNT from the `size` field (size_irep), multiplies by sizeof, and hands symex_cpp_new a
// fresh persistent `symex_dynamic::dynamic_N` object (unique counter -> no dangle, no aliasing).
static exprt
make_cpp_new(contextt &ctx, const typet &element_type, const exprt *count)
{
  side_effect_exprt new_expr(count ? "cpp_new[]" : "cpp_new");
  new_expr.type() = pointer_typet(element_type);
  if (count)
    new_expr.size(
      *count); // element count goes in `size`, NOT cmt_size (do_cpp_new overwrites cmt_size)

  namespacet ns(ctx);
  exprt size_of = c_sizeof(element_type, ns);
  size_of.set("#c_sizeof_type", element_type);
  new_expr.set("sizeof", size_of);
  return new_expr;
}

exprt jimple_new::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // `new T` (a single object) must allocate T's STRUCT, not a T* reference slot. The inherited
  // jimple_newarray path takes its element type from to_typet(), which for a class returns
  // pointer_typet(struct) -- right for ARRAY elements (Java arrays hold references) but wrong for the
  // object itself: it would size the allocation as a single pointer, so any field write past the first
  // pointer's width (e.g. Enum.__ordinal, or any class with more than one field) lands out of bounds.
  // Allocate a 1-element array of the STRUCT type and return &arr[0] (a T*) so field writes have the
  // whole object to land in.
  typet element_type;
  const symbolt *sym = ctx.find_symbol("tag-" + type->name);
  if (sym != nullptr)
    element_type = sym->get_type();
  else
    element_type =
      type->to_typet(ctx); // unresolved class: fall back to the reference slot

  if (needs_heap_alloc(element_type, class_name, function_name))
    return make_cpp_new(ctx, element_type, nullptr);

  array_typet arr_type(element_type, from_integer(1, uint_type()));
  symbolt arr_symbol = get_temp_symbol(arr_type, class_name, function_name);
  symbolt &added = *ctx.move_symbol_to_context(arr_symbol);
  index_exprt first_elem(
    symbol_expr(added), from_integer(0, int_type()), element_type);
  return address_of_exprt(first_elem);
}

void jimple_expr_invoke::from_json(const json &j)
{
  lhs = nil_exprt();
  j.at("base_class").get_to(base_class);
  j.at("method").get_to(method);
  for (auto x : j.at("parameters"))
  {
    parameters.push_back(std::move(jimple_expr::get_expression(x)));
  }
  method += "_" + get_hash_name();

  // TODO: Move intrinsics to backend
  if (base_class == "java.lang.Integer" && method == "valueOf_1")
  {
    log_debug("jimple", "Got an intrinsic call to valueOf int");
    is_intrinsic_method = true;
  }
  // Every java.lang.Integer call lowers to a VALUE (boxing typecast or, for any other method, a
  // nondet) rather than a GOTO call -- mark it so the assignment lowering assigns the value directly
  // instead of treating to_exprt() as a retargetable call block (else: "non-code operand in block").
  if (base_class == "java.lang.Integer")
    is_intrinsic_method = true;
  // Static String factories (valueOf/join/format) lower to a fresh axiomatic
  // String VALUE, never a call into the model body -- see jimple_string_lower.
  if (jimple_is_string_class(base_class))
    is_intrinsic_method = true;
}

exprt jimple_expr_invoke::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // TODO: Move intrinsics to backend
  if (base_class == "kotlin.jvm.internal.Intrinsics")
  {
    code_skipt skip;
    return skip;
  }

  // TODO: Move intrinsics to backend
  if (base_class == "java.lang.Runtime")
  {
    code_skipt skip;
    return skip;
  }

  // Engine-native String static factory (valueOf/join/format): a fresh opaque
  // String VALUE, never the model body.
  if (jimple_is_string_class(base_class))
  {
    std::vector<exprt> args;
    for (const auto &p : parameters)
      args.push_back(p->to_exprt(ctx, class_name, function_name));
    exprt v = jimple_string_lower(
      ctx, class_name, function_name, method, nil_exprt(), args);
    if (!v.is_nil())
      return v;
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  // Autoboxing: Integer.valueOf(int) returns a BOXED reference. Represent the box as a tagged pointer
  // carrying the int in its bits (typecast int -> pointer), so it sits in an Object[] type-correctly
  // and round-trips; Integer.intValue() casts it back (below). This replaces collapsing it to the raw
  // int, which type-punned a 32-bit value into a pointer slot and read back nondet.
  if (base_class == "java.lang.Integer" && method == "valueOf_1")
  {
    exprt v = parameters[0]->to_exprt(ctx, class_name, function_name);
    return typecast_exprt(v, pointer_typet(empty_typet()));
  }

  // Any OTHER java.lang.Integer static (toHexString, parseInt, …): never emitted as a symbol (Integer
  // is a base/boxing type, not a stubbed class), so model it as a nondet result rather than aborting
  // on the missing symbol. Reached e.g. via the char-array String model's hashCode/toString.
  if (base_class == "java.lang.Integer")
  {
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  if (is_nondet_call())
  {
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  code_blockt block;
  code_function_callt call;

  std::ostringstream oss;
  oss << base_class << ":" << method;

  auto symbol = ctx.find_symbol(oss.str());
  if (!symbol)
  {
    // Unresolved method: a call on a base-typed library class (java.lang.Class.forName, String/Integer
    // surface) or an unmodelled library method reached only by dead model code. JBMC lowers an
    // unmodelled call to a nondet return; do the same here (a sound over-approximation) instead of
    // aborting GOTO generation over a frequently-unreachable call. Warn so it is never silent.
    log_warning(
      "Unresolved method {} -> nondet result (over-approximation)", oss.str());
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }
  call.function() = symbol_expr(*symbol);
  if (!lhs.is_nil())
    call.lhs() = lhs;

  for (long unsigned int i = 0; i < parameters.size(); i++)
  {
    // Just adding the arguments should be enough to set the parameters
    auto parameter_expr =
      parameters[i]->to_exprt(ctx, class_name, function_name);
    call.arguments().push_back(parameter_expr);
    // Hack, manually adding parameters, this should be done at symex
    std::ostringstream oss;
    oss << "@parameter" << i;
    auto temp = get_symbol_name(base_class, method, oss.str());
    symbolt &added_symbol = require_symbol(ctx, temp);
    code_assignt assign(symbol_expr(added_symbol), parameter_expr);
    block.operands().push_back(assign);
  }
  block.operands().push_back(call);
  return block;
}

void jimple_virtual_invoke::from_json(const json &j)
{
  lhs = nil_exprt();
  j.at("base_class").get_to(base_class);
  j.at("method").get_to(method);
  j.at("name").get_to(variable);
  for (auto x : j.at("parameters"))
  {
    parameters.push_back(std::move(jimple_expr::get_expression(x)));
  }
  method += "_" + get_hash_name();
  // Integer.intValue lowers to a VALUE (unbox cast), not a function call -- see to_exprt below.
  if (base_class == "java.lang.Integer" && method == "intValue_1")
    is_intrinsic_method = true;
  // Any other java.lang.Integer instance call also lowers to a nondet VALUE (Integer is never a class).
  if (base_class == "java.lang.Integer")
    is_intrinsic_method = true;
  // Object/array clone() lowers to the receiver reference (a VALUE), not a call -- see to_exprt.
  if (method == "clone_1")
    is_intrinsic_method = true;
  // Every java.lang.String instance method lowers to an axiomatic VALUE
  // (length -> int, equals -> bool, substring -> a fresh String ref), never a
  // call into the char-array model body -- see jimple_string_lower in to_exprt.
  if (jimple_is_string_class(base_class))
    is_intrinsic_method = true;
}

exprt jimple_virtual_invoke::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // TODO: Move intrinsics to backend
  if (base_class == "kotlin.jvm.internal.Intrinsics")
  {
    code_skipt skip;
    return skip;
  }

  // TODO: Move intrinsics to backend
  if (base_class == "java.lang.Runtime")
  {
    code_skipt skip;
    return skip;
  }

  // TODO: Move intrinsics to backend
  if (base_class == "java.lang.Class")
  {
    code_skipt skip;
    return skip;
  }

  // Engine-native String: lower length/charAt/equals/substring/... axiomatically
  // (no char-array model body, no backing()/value deref). Bypasses the runtime
  // dispatch below so the shipped model method is never reached.
  if (jimple_is_string_class(base_class))
  {
    exprt recv =
      (variable != "")
        ? jimple_symbol(variable).to_exprt(ctx, class_name, function_name)
        : nil_exprt();
    std::vector<exprt> args;
    for (const auto &p : parameters)
      args.push_back(p->to_exprt(ctx, class_name, function_name));
    exprt v =
      jimple_string_lower(ctx, class_name, function_name, method, recv, args);
    if (!v.is_nil())
      return v; // a VALUE (is_intrinsic_method drives the assignment lowering)
    // Unrecognised String method -> nondet over-approximation.
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  // Auto-UNBOXING: Integer.intValue() reads the boxed int back out of the tagged pointer that
  // Integer.valueOf produced (jimple_expr_invoke::to_exprt). Cast the receiver reference -> 32-bit int.
  if (base_class == "java.lang.Integer" && method == "intValue_1")
  {
    exprt recv =
      jimple_symbol(variable).to_exprt(ctx, class_name, function_name);
    return typecast_exprt(recv, signedbv_typet(32));
  }

  // Array/object clone(): Java returns a fresh shallow copy; model it as the receiver reference.
  // This is sound for the read-only clone patterns the bundled models use (enum values() returning
  // $VALUES.clone(); the char-array String model copying its backing array) where the result is only
  // read. java.lang.Object is never emitted, so the call could not resolve to a real method anyway.
  // TODO: a true element-wise copy if a proof ever mutates a clone independently of the original.
  if (method == "clone_1")
    return jimple_symbol(variable).to_exprt(ctx, class_name, function_name);

  // Non-boxing java.lang.Integer instance method: Integer is never emitted as a class, so model it as
  // a nondet result rather than aborting on the missing symbol (see the static path above).
  if (base_class == "java.lang.Integer")
  {
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  if (is_nondet_call())
  {
    jimple_nondet nondet(method);
    return nondet.to_exprt(ctx, class_name, function_name);
  }

  // RUNTIME VIRTUAL DISPATCH. The producer emits `base_class` = the call site's
  // DECLARED type (an interface / superclass, e.g. java.util.List). Binding
  // straight to `base_class:method` reaches the library model's nondet stub even
  // when the receiver's concrete class overrides the method -- a true property
  // (`new MyList().size() == 1` held through a `List` ref) then FALSELY REFUTES.
  // Static-type resolution fixes the monomorphic case but NOT `this.m()` inside
  // an inherited base method, where the static type of `this` is the base.
  //
  // So we dispatch on the object's RUNTIME class id (`@class_identifier`, stamped
  // at `new`): read it back and switch over the candidate overrides. One
  // candidate -> a direct call (no switch, identical to a monomorphic site);
  // many -> a cid-keyed if/else chain; none -> nondet over-approximation.

  // Static type of the receiver variable, for exact monomorphic dispatch.
  std::string recv_static;
  exprt recv;
  if (variable != "")
  {
    recv = jimple_symbol(variable).to_exprt(ctx, class_name, function_name);
    typet rt = recv.type();
    if (rt.id() == "pointer")
      rt = rt.subtype();
    if (rt.id() == "struct")
      recv_static = to_struct_type(rt).tag().as_string();
    else if (rt.id() == "symbol")
    {
      std::string tag = rt.get("identifier").as_string();
      if (tag.compare(0, 4, "tag-") == 0)
        recv_static = tag.substr(4);
    }
  }

  auto candidates =
    jimple_hierarchy::candidate_overrides(ctx, base_class, recv_static, method);

  // A candidate override resolving to java.lang.String (e.g. a polymorphic
  // element.equals(x) where the runtime element is a String) must use the
  // engine-native axiomatic lowering, NOT the char-array model body -- otherwise
  // dispatch re-enters String.equals/length and unwinds the backing array we
  // bypassed at direct String call sites. Build the intrinsic as a code block
  // (assign the value to lhs, or skip when discarded) so it slots into the
  // dispatch chain where make_call's block would go.
  auto make_string_call = [&]() -> codet
  {
    std::vector<exprt> args;
    for (const auto &p : parameters)
      args.push_back(p->to_exprt(ctx, class_name, function_name));
    exprt v =
      jimple_string_lower(ctx, class_name, function_name, method, recv, args);
    if (v.is_nil())
    {
      jimple_nondet nondet(method);
      v = nondet.to_exprt(ctx, class_name, function_name);
    }
    if (lhs.is_nil())
      return code_skipt();
    if (v.type() != lhs.type())
      v = typecast_exprt(v, lhs.type());
    return code_assignt(lhs, v);
  };

  // Build the call block targeting `callee_qual` (a "Class:method" symbol),
  // wiring @this and @parameters exactly as a non-virtual call would.
  auto make_call = [&](const std::string &callee_qual) -> codet
  {
    std::string cc = callee_qual.substr(0, callee_qual.rfind(':'));
    if (jimple_is_string_class(cc))
      return make_string_call();

    code_blockt blk;
    code_function_callt call;
    symbolt &symbol = require_symbol(ctx, callee_qual);
    call.function() = symbol_expr(symbol);
    if (!lhs.is_nil())
      call.lhs() = lhs;

    std::string callee_class = callee_qual.substr(0, callee_qual.rfind(':'));
    if (variable != "")
    {
      exprt this_expression =
        jimple_symbol(variable).to_exprt(ctx, class_name, function_name);
      call.arguments().push_back(this_expression);
      auto temp = get_symbol_name(callee_class, method, "@this");
      symbolt &added = require_symbol(ctx, temp);
      blk.operands().push_back(
        code_assignt(symbol_expr(added), this_expression));
    }
    for (long unsigned int i = 0; i < parameters.size(); i++)
    {
      exprt parameter_expr =
        parameters[i]->to_exprt(ctx, class_name, function_name);
      call.arguments().push_back(parameter_expr);
      std::ostringstream pn;
      pn << "@parameter" << i;
      auto temp = get_symbol_name(callee_class, method, pn.str());
      symbolt &added = require_symbol(ctx, temp);
      blk.operands().push_back(
        code_assignt(symbol_expr(added), parameter_expr));
    }
    blk.operands().push_back(call);
    return blk;
  };

  // No body anywhere -> unmodelled / abstract-only call (e.g. a java.util.function
  // interface the producer never linked). JBMC lowers this to a nondet return; do
  // the same (sound over-approximation). Must return CODE: assign the nondet to
  // the result in assignment position, or skip it when the result is discarded
  // (statement position). Returning the bare nondet side-effect would leave a
  // non-code operand in the enclosing block -> goto_convert abort.
  if (candidates.empty())
  {
    log_warning(
      "Unresolved virtual method {}:{} -> nondet result (over-approximation)",
      base_class,
      method);
    if (lhs.is_nil())
      return code_skipt();
    jimple_nondet nondet(method);
    exprt nd = nondet.to_exprt(ctx, class_name, function_name);
    nd.type() = lhs.type();
    return code_assignt(lhs, nd);
  }

  // Monomorphic: exactly one possible body -> a plain call, no dispatch cost.
  // Also when there is no receiver to read a runtime id from (variable == "")
  // or the receiver's static type is opaque (no @class_identifier component,
  // e.g. an unresolved interface modelled as pointer-to-empty): we cannot read a
  // runtime id, so bind to the static-type-rooted body (candidates.front(), the
  // receiver's declared type). JBMC binds the same way for a fully-opaque type.
  if (candidates.size() == 1 || variable == "" || !jimple_has_class_id(recv))
    return make_call(candidates.front().second);

  // Polymorphic: switch on the receiver's runtime class id. The trailing
  // candidate is the chain's `else`, so an object whose runtime id matches no
  // explicit arm runs the root's own body (candidate_overrides puts the
  // static-type/base body last). Build innermost-first so the chain nests as
  // if (cid==id0) c0 else if (cid==id1) c1 else ... else cN.
  exprt cid_read = jimple_class_id_member(recv);
  codet chain = make_call(candidates.back().second);
  for (long unsigned int k = candidates.size() - 1; k-- > 0;)
  {
    code_ifthenelset ite;
    ite.cond() = equality_exprt(
      cid_read, from_integer(candidates[k].first, signedbv_typet(32)));
    ite.then_case() = make_call(candidates[k].second);
    ite.else_case() = chain;
    chain = ite;
  }
  return chain;
}

exprt jimple_newarray::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  typet element_type = type->to_typet(ctx);
  if (element_type.is_nil())
    element_type = char_type();

  exprt count = size->to_exprt(ctx, class_name, function_name);
  if (count.is_nil())
    count = from_integer(1, uint_type());

  if (needs_heap_alloc(element_type, class_name, function_name))
    return make_cpp_new(ctx, element_type, &count);

  array_typet arr_type(element_type, count);
  symbolt arr_symbol = get_temp_symbol(arr_type, class_name, function_name);
  symbolt &added = *ctx.move_symbol_to_context(arr_symbol);
  index_exprt first_elem(
    symbol_expr(added), from_integer(0, int_type()), element_type);
  return address_of_exprt(first_elem);
};

void jimple_deref::from_json(const json &j)
{
  base = get_expression(j.at("base"));
  index = get_expression(j.at("index"));
}

exprt jimple_deref::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto arr = base->to_exprt(ctx, class_name, function_name);
  auto i = index->to_exprt(ctx, class_name, function_name);
  auto index = index_exprt(arr, i, arr.type().subtype());
  exprt &array_expr = index.op0();

  exprt addition("+", array_expr.type());
  addition.operands().swap(index.operands());

  index.move_to_operands(addition);
  index.id("dereference");
  index.type() = array_expr.type().subtype();

  return index;
};

exprt jimple_nondet::to_exprt(
  contextt &,
  const std::string &,
  const std::string &) const
{
  auto type = int_type(); // TODO: hashmap here!
  exprt rhs = exprt("sideeffect", type);
  rhs.statement("nondet");

  return rhs;
};

void jimple_static_member::from_json(const json &j)
{
  j.at("base_class").get_to(from);
  j.at("member").get_to(field);
  jimple_type t;
  j.at("type").get_to(t);
  type = std::make_shared<jimple_type>(t);
}

exprt jimple_static_member::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto result = gen_zero(type->to_typet(ctx));
  // HACK: For now I will set some intrinsics directly (this should go to SYMEX)
  if (from == "kotlin._Assertions" && field == "ENABLED")
  {
    result.make_true();
    return result;
  }

  if (from == "Main" && field == "$assertionsDisabled")
  {
    result.make_false();
    return result;
  }

  // Coroutine / stdlib singletons we don't model (kotlin.Unit.INSTANCE,
  // Dispatchers.Default, ...). A zero/opaque value suffices — the value is never
  // used as real data once launch/runBlocking become __ESBMC_spawn_thread.
  if (from == "kotlin.Unit" || from.rfind("kotlinx.coroutines", 0) == 0)
    return result;

  // Static field: resolve the global symbol registered by jimple_file::to_exprt.
  // Returning the symbol directly works for both reads and writes (assignment LHS).
  {
    std::string gid = from + "." + field;
    symbolt *g = ctx.find_symbol(gid);
    if (g != nullptr)
      return symbol_expr(*g);
  }

  // TODO: Needs OOP members
  // Fallback: an instance member accessed through a local of the same name.
  auto symbol_name = get_symbol_name(class_name, function_name, from);
  symbolt *s = ctx.find_symbol(symbol_name);
  if (s == nullptr)
  {
    // Unresolved static field on a base-typed / unemitted library class (java.lang.Boolean.FALSE, ...),
    // typically reached only by dead model code. Register an on-demand, zero-initialised static global
    // (like jimple_file's own static-field globals) so both reads and assignment LHS resolve, instead of
    // aborting GOTO generation. Warn so it stays visible.
    log_warning(
      "Unresolved static field {}.{} -> on-demand global (over-approximation)",
      from,
      field);
    std::string ondemand_id = from + "." + field;
    typet ft = type->to_typet(ctx);
    symbolt g = create_jimple_symbolt(ft, from, field, ondemand_id);
    g.lvalue = true;
    g.static_lifetime = true;
    g.is_extern = false;
    g.set_value(gen_zero(ft));
    ctx.move_symbol_to_context(g);
    return symbol_expr(*ctx.find_symbol(ondemand_id));
  }
  member_exprt op(symbol_expr(*s), "tag-" + field, s->get_type());
  exprt &base = op.struct_op();
  if (base.type().is_pointer())
  {
    exprt deref("dereference");
    deref.type() = base.type().subtype();
    deref.move_to_operands(base);
    base.swap(deref);
  }
  return op;
};

void jimple_virtual_member::from_json(const json &j)
{
  j.at("variable").get_to(variable);
  j.at("signature").at("base_class").get_to(from);
  j.at("signature").at("member").get_to(field);
  jimple_type t;
  j.at("signature").at("type").get_to(t);
  type = std::make_shared<jimple_type>(t);
}

exprt jimple_virtual_member::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  auto result = gen_zero(type->to_typet(ctx));
  auto struct_type = require_symbol(ctx, "tag-" + from).get_type();

  // 1. Look over the local scope
  auto symbol_name = get_symbol_name(class_name, function_name, variable);
  symbolt &s = require_symbol(ctx, symbol_name);
  member_exprt op(symbol_expr(s), "tag-" + field, type->to_typet(ctx));
  exprt &base = op.struct_op();
  if (base.type().is_pointer())
  {
    exprt deref("dereference");
    deref.type() = base.type().subtype();
    deref.move_to_operands(base);
    base.swap(deref);
  }

  return op;
};
