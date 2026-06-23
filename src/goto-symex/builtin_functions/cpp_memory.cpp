#include <cassert>
#include <goto-symex/goto_symex.h>
#include <string>
#include <util/arith_tools.h>
#include <util/c_types.h>
#include <util/expr_util.h>
#include <util/i2string.h>
#include <irep2/irep2.h>
#include <util/migrate.h>
#include <util/std_types.h>
#include <util/type_byte_size.h>

void goto_symext::symex_cpp_new(
  const expr2tc &lhs,
  const sideeffect2t &code,
  const guard2tc &guard)
{
  expr2tc size = code.size;

  bool do_array = (code.kind == sideeffect2t::allockind::cpp_new_arr);

  unsigned int &dynamic_counter = get_dynamic_counter();
  dynamic_counter++;

  const std::string count_string(i2string(dynamic_counter));

  // value
  symbolt symbol;
  symbol.name = do_array ? "dynamic_" + count_string + "_array"
                         : "dynamic_" + count_string + "_value";
  symbol.id = "symex_dynamic::" + id2string(symbol.name);
  symbol.lvalue = true;
  symbol.mode = "C++";

  const pointer_type2t &ptr_ref = to_pointer_type(code.type);
  type2tc renamedtype2 =
    migrate_type(ns.follow(migrate_type_back(ptr_ref.subtype)));

  // goto_convert's do_cpp_new hands us `code.size` as the BYTE size (element count * sizeof(element)).
  // The array object's element COUNT -- what callers read back via __ESBMC_get_object_size / Java's
  // `array.length` -- is that byte size divided by the element size. Sizing the array type by the raw
  // byte size over-allocates (count*sizeof slots) and makes the length report the byte size, so an
  // in-bounds Java index (0..count-1) is reported in-bounds but past count runs into uninitialised
  // slots. (Byte-offset C/C++ access still lands correctly either way; the byte size is kept below for
  // allocation tracking.)
  expr2tc arr_count = code.size;
  if (do_array)
  {
    BigInt elem_bytes = type_byte_size(renamedtype2);
    if (elem_bytes > 1)
    {
      arr_count = div2tc(
        code.size->type, code.size, constant_int2tc(code.size->type, elem_bytes));
      do_simplify(arr_count);
    }
  }

  type2tc newtype = do_array
                      ? type2tc(array_type2tc(renamedtype2, arr_count, false))
                      : renamedtype2;

  {
    typet t = migrate_type_back(newtype);
    t.dynamic(true);
    symbol.set_type(std::move(t));
  }

  new_context.add(symbol);

  // Java ZERO-INITIALISES a new object's fields (references null, numerics 0) before the constructor
  // runs -- so a field the constructor never sets (e.g. a Kotlin `lateinit` backing field) reads as
  // null, which an `isInitialized`/null guard depends on. Without this the fresh dynamic object's fields
  // are nondet. Only the single-object case (a struct); arrays get their elements written explicitly.
  // (The Jimple fundamentals suite has no C++ `new`, where leaving it uninitialised is the language
  // semantics; if this ever runs for C++ it should be gated on the source language.)
  if (!do_array && is_struct_type(newtype))
  {
    expr2tc zero;
    migrate_expr(gen_zero(migrate_type_back(newtype)), zero);
    symex_assign(
      code_assign2tc(symbol2tc(newtype, symbol.id), zero), true, guard);
  }

  // make symbol expression
  expr2tc rhs_ptr_obj;
  if (do_array)
  {
    expr2tc sym = symbol2tc(newtype, symbol.id);
    expr2tc idx = index2tc(renamedtype2, sym, gen_ulong(0));
    rhs_ptr_obj = idx;
  }
  else
    rhs_ptr_obj = symbol2tc(newtype, symbol.id);

  expr2tc rhs = address_of2tc(renamedtype2, rhs_ptr_obj);

  cur_state->rename(rhs);
  expr2tc rhs_copy(rhs);
  expr2tc ptr_rhs(rhs);

  symex_assign(code_assign2tc(lhs, rhs), true);

  expr2tc ptr_obj = pointer_object2tc(pointer_type2(), ptr_rhs);
  track_new_pointer(ptr_obj, newtype, guard, size);

  guard2tc g(cur_state->guard);
  g.append(guard);
  dynamic_memory.emplace_back(rhs_copy, g, false, symbol.name.as_string());
}

void goto_symext::symex_cpp_delete(const expr2tc &expr)
{
  // expr is code_cpp_delete or code_cpp_del_array; both have exactly
  // one sub-expression — the pointer being deleted.
  assert(is_code_cpp_delete2t(expr) || is_code_cpp_del_array2t(expr));
  expr2tc tmp = *expr->get_sub_expr(0);

  internal_deref_items.clear();
  expr2tc deref = dereference2tc(get_empty_type(), tmp);
  dereference(deref, dereferencet::INTERNAL);

  // we need to check the memory deallocation operator:
  // new and delete, new[] and delete[]
  if (internal_deref_items.size())
  {
    bool is_arr = is_array_type(internal_deref_items.front().object->type);
    bool is_del_arr = is_code_cpp_del_array2t(expr);

    if (is_arr != is_del_arr)
    {
      const std::string &msg =
        "Mismatched memory deallocation operators: " + get_expr_id(expr);
      claim(gen_false_expr(), msg);
    }
  }
  // implement delete as a call to free
  symex_free(expr);
}
