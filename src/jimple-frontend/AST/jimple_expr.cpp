#include <jimple-frontend/AST/jimple_expr.h>
#include <util/arith_tools.h>
#include <util/c_typecast.h>
#include <util/c_types.h>
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
      pointer_typet(empty_typet()), class_name, var_name, symbol_name, function_name);
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
    log_error("Could not find symbol {}", oss.str());
    abort();
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

  code_blockt block;
  code_function_callt call;

  std::ostringstream oss;
  oss << base_class << ":" << method;

  symbolt &symbol = require_symbol(ctx, oss.str());
  call.function() = symbol_expr(symbol);
  if (!lhs.is_nil())
  {
    call.lhs() = lhs;
  }

  if (variable != "")
  {
    // Let's add @THIS
    auto this_expression =
      jimple_symbol(variable).to_exprt(ctx, class_name, function_name);
    call.arguments().push_back(this_expression);
    auto temp = get_symbol_name(base_class, method, "@this");
    symbolt &added_symbol = require_symbol(ctx, temp);
    code_assignt assign(symbol_expr(added_symbol), this_expression);
    block.operands().push_back(assign);
  }

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

  // Allocate `new T[N]` as a fresh, typed STACK array object `T[N]`, not dynamic memory. goto-symex's
  // dynamic-allocation machinery (cpp_new[]/malloc side effects) is not wired up for the Jimple
  // frontend and crashes; a plain array symbol is a first-class object whose `array_size` is exactly N.
  // `array.length` (lengthof -> __ESBMC_get_object_size) reads that size back, and element indexing
  // dereferences it directly. Return &arr[0] so the value has the array's element-pointer (T*) type,
  // matching how Jimple holds an array reference. The previous code allocated via a body-less `malloc`
  // symbol with a byte size and a void* result, so nothing was tracked and `a.length` was nondet.
  array_typet arr_type(element_type, count);
  symbolt arr_symbol = get_temp_symbol(arr_type, class_name, function_name);
  symbolt &added = *ctx.move_symbol_to_context(arr_symbol);
  exprt arr_ref = symbol_expr(added);
  index_exprt first_elem(arr_ref, from_integer(0, int_type()), element_type);
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
    log_error("jimple static_member: unresolved field {}.{}", from, field);
    abort();
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
