#include <util/std_code.h>
#include <util/std_expr.h>
#include <util/std_types.h>
#include <jimple-frontend/AST/jimple_statement.h>
#include <jimple-frontend/AST/jimple_expr.h>
#include <jimple-frontend/AST/jimple_hierarchy.h>
#include <util/arith_tools.h>
#include <util/expr_util.h>
#include <util/message.h>
#include "util/c_typecast.h"

// Resolve a symbol that MUST exist by this point in conversion. A missing
// symbol means the module referenced something it never declared (e.g. an
// invoke to a constructor/method, or a field/local, that was not emitted).
// Fail loudly naming the symbol instead of dereferencing null (a segfault that
// hides the cause).
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

void jimple_identity::from_json(const json &j)
{
  j.at("identifier").get_to(at_identifier);
  j.at("name").get_to(local_name);
  j.at("type").get_to(type);
}

exprt jimple_identity::to_exprt(
  contextt &ctx,
  const std::string &,
  const std::string &) const
{
  // TODO: Symbol-table / Typecast
  exprt val("at_identifier");
  symbolt &added_symbol = require_symbol(ctx, local_name);
  symbolt rhs;
  rhs.name = "@" + at_identifier;
  rhs.id = "@" + at_identifier;
  code_assignt assign(symbol_expr(added_symbol), symbol_expr(rhs));
  return assign;
}
std::string jimple_identity::to_string() const
{
  std::ostringstream oss;
  oss << "Identity:  " << this->local_name << " = @" << at_identifier << " | "
      << type.to_string();
  return oss.str();
}

exprt jimple_return::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // TODO: jimple return with support to other returns
  typet return_type = empty_typet();
  code_returnt ret_expr;
  if (expr)
  {
    auto return_value = expr->to_exprt(ctx, class_name, function_name);
    ret_expr.op0() = return_value;
  }
  // TODO: jimple return should support values
  return ret_expr;
}

std::string jimple_return::to_string() const
{
  return "Return: (Nothing)";
}
void jimple_return::from_json(const json &j)
{
  if (j.contains("value"))
    expr = jimple_expr::get_expression(j.at("value"));
}
std::string jimple_label::to_string() const
{
  std::ostringstream oss;
  oss << "Label: " << this->label;
  for (auto member : this->members->members)
    oss << "\n\t\t\t" << member->to_string();
  return oss.str();
}

exprt jimple_label::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // TODO: DRY (clang-c-converter)
  code_labelt c_label;
  c_label.set_label(label);

  code_blockt block;
  for (auto member : members->members)
  {
    block.operands().push_back(
      std::move(member->to_exprt(ctx, class_name, function_name)));
  }
  c_label.code() = to_code(block);

  return c_label;
}

void jimple_goto::from_json(const json &j)
{
  j.at("goto").get_to(label);
}

std::string jimple_goto::to_string() const
{
  std::ostringstream oss;
  oss << "Goto: " << this->label;
  return oss.str();
}

exprt jimple_goto::to_exprt(
  contextt &,
  const std::string &,
  const std::string &) const
{
  code_gotot code_goto;
  code_goto.set_destination(label);
  return code_goto;
}

void jimple_label::from_json(const json &j)
{
  j.at("label_id").get_to(label);
  jimple_full_method_body b;
  b.from_json(j.at("content"));
  members = std::make_shared<jimple_full_method_body>(b);
}

std::string jimple_assignment::to_string() const
{
  std::ostringstream oss;
  oss << "Assignment: " << lhs->to_string() << " = " << rhs->to_string();
  return oss.str();
}

void jimple_assignment::from_json(const json &j)
{
  lhs = jimple_expr::get_expression(j.at("lhs"));
  rhs = jimple_expr::get_expression(j.at("rhs"));
}

exprt jimple_assignment::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  //TODO: Remove this hack
  if (is_skip)
  {
    code_skipt skip;
    return skip;
  }

  auto lhs_handle = lhs->to_exprt(ctx, class_name, function_name);

  auto dyn_expr = std::dynamic_pointer_cast<jimple_expr_invoke>(rhs);
  if (dyn_expr && !dyn_expr->is_nondet_call() && !dyn_expr->is_intrinsic_method)
  {
    dyn_expr->set_lhs(lhs_handle);
    return rhs->to_exprt(ctx, class_name, function_name);
  }

  auto dyn2_expr = std::dynamic_pointer_cast<jimple_virtual_invoke>(rhs);
  if (
    dyn2_expr && !dyn2_expr->is_nondet_call() &&
    !dyn2_expr->is_intrinsic_method)
  {
    dyn2_expr->set_lhs(lhs_handle);
    return rhs->to_exprt(ctx, class_name, function_name);
  }

  auto from_expr = rhs->to_exprt(ctx, class_name, function_name);
  // A nondet RHS must take the LHS's own type so it ranges over that type's FULL domain: a nondet
  // double has to include NaN/inf and fractional values, a nondet long the full 64-bit range.
  // jimple_nondet emits an int-typed nondet by default; narrowing it to the target via the implicit
  // cast below would instead leave `d = (double)(nondet int)` -- only integer-valued, never-NaN
  // doubles. Retyping the nondet directly restores the full range.
  if (from_expr.id() == "sideeffect" && from_expr.statement() == "nondet")
    from_expr.type() = lhs_handle.type();
  else
  {
    c_typecastt c_typecast(ctx);
    c_typecast.implicit_typecast(from_expr, lhs_handle.type());
  }

  code_assignt assign(lhs_handle, from_expr);

  // Stamp the runtime class id on a freshly-allocated object: `lhs = new T`
  // then `lhs->@class_identifier = id(T)`. Read back at a virtual call to
  // dispatch over T even when the reference is later widened to a base/interface
  // type (the only sound way to bind `this.m()` in an inherited base method).
  // Gate on the LHS struct actually carrying the @class_identifier component:
  // when the LHS is declared with an opaque/unresolved static type (modelled as
  // pointer-to-empty), the component is absent and the write would name a
  // non-existent member. Such an object also cannot be dispatched on later
  // (the read site gates the same way), so skipping the stamp is consistent.
  auto new_expr = std::dynamic_pointer_cast<jimple_new>(rhs);
  if (new_expr && jimple_has_class_id(lhs_handle))
  {
    int id = jimple_hierarchy::class_id(new_expr->get_type_name());
    if (id != 0)
    {
      code_blockt block;
      block.copy_to_operands(assign);
      block.copy_to_operands(code_assignt(
        jimple_class_id_member(lhs_handle),
        from_integer(id, signedbv_typet(32))));
      return block;
    }
  }

  return assign;
}

/*
std::string jimple_assignment_deref::to_string() const
{
  std::ostringstream oss;
  oss << "Assignment: " << variable << "[" << pos->to_string()
      << "]  = " << expr->to_string();
  return oss.str();
}

void jimple_assignment_deref::from_json(const json &j)
{
  j.at("name").get_to(variable);
  expr = jimple_expr::get_expression(j.at("value"));
  pos = jimple_expr::get_expression(j.at("pos"));
}

exprt jimple_assignment_deref::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  jimple_symbol s(variable);

  jimple_deref d(pos, std::make_shared<jimple_symbol>(s));

  code_assignt assign(
    d.to_exprt(ctx, class_name, function_name),
    expr->to_exprt(ctx, class_name, function_name));
  return assign;
}

std::string jimple_assignment_field::to_string() const
{
  std::ostringstream oss;
  oss << "Assignment: " << variable << "->" << field << " = " << expr->to_string();
  return oss.str();
}

void jimple_assignment_field::from_json(const json &j)
{
  j.at("name").get_to(variable);
  j.at("field").get_to(field);
  expr = jimple_expr::get_expression(j.at("value"));
}

exprt jimple_assignment_field::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
    // 1. Look over the local scope
  auto symbol_name = get_symbol_name(class_name, function_name, variable);
  symbolt &s = require_symbol(ctx, symbol_name);
  member_exprt op(symbol_expr(s), "tag-" + field, s.get_type());
  exprt &base = op.struct_op();
  if(base.type().is_pointer())
  {
    exprt deref("dereference");
    deref.type() = base.type().subtype();
    deref.move_to_operands(base);
    base.swap(deref);
  }

  code_assignt assign(
    op,
    expr->to_exprt(ctx, class_name, function_name));
  return assign;
}
*/

std::string jimple_if::to_string() const
{
  std::ostringstream oss;
  oss << "If: " << cond->to_string() << " THEN GOTO " << label;
  return oss.str();
}

void jimple_if::from_json(const json &j)
{
  cond = jimple_expr::get_expression(j.at("expression"));
  j.at("goto").get_to(label);
}

exprt jimple_if::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  code_gotot code_goto;
  code_goto.set_destination(label);

  auto condition = cond->to_exprt(ctx, class_name, function_name);
  codet if_expr("ifthenelse");
  if_expr.copy_to_operands(condition, code_goto);

  return if_expr;
}

// Coerce a Jimple condition expression to a boolean. Comparison binops already
// produce a bool-typed expr; a bare integer condition (Soot lowers booleans to
// int 0/1) becomes `cond != 0`.
static exprt jimple_cond_to_bool(exprt cond)
{
  if (cond.type().is_bool())
    return cond;

  exprt zero = gen_zero(cond.type());
  return binary_relation_exprt(cond, "notequal", zero);
}

std::string jimple_assertion::to_string() const
{
  std::ostringstream oss;
  oss << "Assertion: " << cond->to_string();
  return oss.str();
}

void jimple_assertion::from_json(const json &j)
{
  cond = jimple_expr::get_expression(j.at("expression"));
  if (j.contains("comment"))
    j.at("comment").get_to(comment);
}

exprt jimple_assertion::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  exprt condition =
    jimple_cond_to_bool(cond->to_exprt(ctx, class_name, function_name));
  code_assertt assertion(condition);
  if (!comment.empty())
    assertion.location().comment(comment);
  return assertion;
}

std::string jimple_assume::to_string() const
{
  std::ostringstream oss;
  oss << "Assume: " << cond->to_string();
  return oss.str();
}

void jimple_assume::from_json(const json &j)
{
  cond = jimple_expr::get_expression(j.at("expression"));
}

exprt jimple_assume::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  exprt condition =
    jimple_cond_to_bool(cond->to_exprt(ctx, class_name, function_name));
  return code_assumet(condition);
}

std::string jimple_invoke::to_string() const
{
  std::ostringstream oss;
  oss << "Invoke: " << method;
  return oss.str();
}

void jimple_invoke::from_json(const json &j)
{
  j.at("base_class").get_to(base_class);
  j.at("method").get_to(method);
  if (j.contains("variable"))
    j.at("variable").get_to(variable);
  for (auto x : j.at("parameters"))
  {
    parameters.push_back(std::move(jimple_expr::get_expression(x)));
  }
  if (j.contains("spawn"))
    j.at("spawn").get_to(spawn);
  method += "_" + get_hash_name();
}

exprt jimple_invoke::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // Thread.start() lowering: spawn a thread running base_class:method through the
  // engine's __ESBMC_spawn_thread intrinsic. The intrinsic takes the address of a
  // (no-arg) function symbol and runs its body as a new thread; the engine's POR /
  // context-bounding then explores the interleavings. The address-of wrapping must
  // happen here (the Jimple JSON cannot express a function pointer).
  if (spawn)
  {
    std::ostringstream target_name;
    target_name << base_class << ":" << method;
    symbolt *target = ctx.find_symbol(target_name.str());
    if (target == nullptr)
    {
      log_error(
        "jimple spawn: target function {} not found", target_name.str());
      abort();
    }

    // Canonical intrinsic: unsigned int __ESBMC_spawn_thread(void (*)(void))
    // (see pthread_lib.c). We declare it with an ellipsis argument list rather
    // than a fixed function-pointer param: jimple static no-arg methods are
    // registered as ellipsis functions (void run(...)), so address_of(run) is
    // pointer-to-void(...), which would not match a fixed void(*)(void) param
    // and would make goto-convert wrap the argument in a typecast. symex's
    // intrinsic_spawn_thread requires operand[0] to be a *literal*
    // address_of(symbol_expr(run)) (it asserts is_address_of2t after simplify),
    // so any wrapping typecast breaks it. An ellipsis param keeps the argument
    // a bare address_of and symex ignores the pointee type.
    code_typet fn_type;
    fn_type.return_type() = unsignedbv_typet(32);
    fn_type.make_ellipsis();

    const irep_idt spawn_id = "c:@F@__ESBMC_spawn_thread";
    if (ctx.find_symbol(spawn_id) == nullptr)
    {
      symbolt spawn_symbol = create_jimple_symbolt(
        fn_type, base_class, "__ESBMC_spawn_thread", spawn_id.as_string());
      spawn_symbol.is_extern = true;
      ctx.move_symbol_to_context(spawn_symbol);
    }
    symbolt *spawn_symbol = ctx.find_symbol(spawn_id);

    // intrinsic_spawn_thread reads call.ret->type, so a return lvalue is required.
    symbolt tid =
      get_temp_symbol(unsignedbv_typet(32), base_class, function_name);
    symbolt &tid_added = *ctx.move_symbol_to_context(tid);

    code_function_callt call;
    call.lhs() = symbol_expr(tid_added);
    call.function() = symbol_expr(*spawn_symbol);
    call.type() = fn_type.return_type();
    call.arguments().push_back(address_of_exprt(symbol_expr(*target)));
    return call;
  }

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

  // Don't care for the default object constructor
  if (base_class == "java.lang.Object")
  {
    code_skipt skip;
    return skip;
  }

  // Don't care for Random
  if (base_class == "java.util.Random")
  {
    code_skipt skip;
    return skip;
  }

  // A statement-position java.lang.Integer call (result discarded): Integer is never emitted as a
  // class, so skip rather than abort on the missing symbol (the value form returns nondet).
  if (base_class == "java.lang.Integer")
  {
    code_skipt skip;
    return skip;
  }

  // java.lang.String calls are NOT skipped: they dispatch to the char-array
  // String MODEL's methods (the producer puts the model on the classpath and
  // emits it). A missing model method then fails loudly via require_symbol
  // below rather than being silently dropped.

  if (base_class == "java.lang.AssertionError")
  {
    code_skipt skip;
    return skip;
  }

  // Kotlin coroutine / stdlib runtime calls we model as no-ops in statement
  // position: Result unwrapping, the single-state IllegalStateException guard,
  // and any leftover kotlinx scheduling (the actual concurrency is introduced by
  // the producer rewriting launch/runBlocking to __ESBMC_spawn_thread).
  if (
    base_class == "kotlin.ResultKt" ||
    base_class == "java.lang.IllegalStateException" ||
    base_class.rfind("kotlinx.coroutines", 0) == 0 ||
    base_class.rfind("kotlin.coroutines", 0) == 0)
  {
    code_skipt skip;
    return skip;
  }

  // Exception / Error constructors: skip the <init>(message, cause, …) chain. We do not model the
  // exception object's state, so the constructor is a no-op -- the preceding `new` still allocates the
  // object and a following `throw` still surfaces it (an uncaught throw is a violation). This matches
  // the AssertionError / IllegalStateException handling above and avoids building a call into the bare
  // nondet stub constructor the producer emits for a library exception (which aborts goto conversion).
  {
    auto ends_with = [](const std::string &s, const std::string &suf)
    {
      return s.size() >= suf.size() &&
             s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (
      method.rfind("<init>", 0) == 0 &&
      (ends_with(base_class, "Exception") || ends_with(base_class, "Error")))
    {
      code_skipt skip;
      return skip;
    }
  }

  code_blockt block;
  code_function_callt call;

  std::ostringstream oss;
  oss << base_class << ":" << method;
  // Unresolved invoke statement: a method on a base-typed library class (java.lang.Class.forName) or an
  // unmodelled library call reached only by dead model code (e.g. kotlin Intrinsics.checkHasClass).
  // Skip it -- a no-op over-approximation, consistent with the Intrinsics/Runtime special-cases above --
  // rather than aborting GOTO generation over an often-unreachable call. Warn so it is never silent.
  symbolt *isym = ctx.find_symbol(oss.str());
  if (!isym)
  {
    log_warning(
      "Unresolved invoke {} -> skipped (over-approximation)", oss.str());
    return code_skipt();
  }
  symbolt &symbol = *isym;
  call.function() = symbol_expr(symbol);

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

  for (unsigned long int i = 0; i < parameters.size(); i++)
  {
    // Just adding the arguments should be enough to set the parameters
    auto parameter_expr =
      parameters[i]->to_exprt(ctx, class_name, function_name);
    call.arguments().push_back(parameter_expr);
    // Hack, manually adding parameters
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

std::string jimple_throw::to_string() const
{
  std::ostringstream oss;
  oss << "Throw: " << expr->to_string();
  return oss.str();
}

void jimple_throw::from_json(const json &j)
{
  expr = jimple_expr::get_expression(j.at("expr"));
}

exprt jimple_throw::to_exprt(
  contextt &ctx,
  const std::string &class_name,
  const std::string &function_name) const
{
  // Model a `throw` as a reachability failure: a REACHED throw is a verification violation (matching
  // jbmc's "an uncaught exception is a violation"); a throw on an infeasible path (e.g. a bounds-check
  // throw excluded by the proof's assumptions) is simply never reached, so the assertion never fires.
  // esbmc's `cpp-throw` machinery for the Jimple frontend is incomplete and SEGFAULTS the moment a
  // throw is reached, so we lower to `assert(false)` instead -- sound for the uncaught case the proofs
  // exercise (try/catch propagation is a separate TODO). A throw also HALTS the path; without that the
  // assertion falls through into the following code and a backward goto would spin in a spurious loop,
  // so follow it with `assume(false)` to prune the continuation exactly as the throw did.
  (void)expr;
  code_blockt block;
  block.copy_to_operands(code_assertt(gen_boolean(false)));
  block.copy_to_operands(code_assumet(gen_boolean(false)));
  return block;
}
