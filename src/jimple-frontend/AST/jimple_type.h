#ifndef ESBMC_JIMPLE_TYPE_H
#define ESBMC_JIMPLE_TYPE_H

#include <jimple-frontend/AST/jimple_ast.h>
#include <util/std_code.h>
#include <util/c_types.h>
#include <util/expr_util.h>

// TODO: Specialize this class
class jimple_type : public jimple_ast
{
public:
  virtual void from_json(const json &j) override;
  virtual std::string to_string() const override;
  virtual typet to_typet(const contextt &ctx) const;

  bool is_array() const
  {
    return dimensions > 0;
  }

  std::string name; // e.g. int[][][][][] => name = int
  short dimensions; // e.g. int[][][][][] => dimensions = 5

protected:
  typet get_base_type(const contextt &ctx) const;
  typet get_builtin_type() const;

  typet get_arr_type(const contextt &ctx) const
  {
    typet base = get_base_type(ctx);
    typet ptr_type = pointer_typet(base);
    for (int i = 1; i < dimensions; i++)
      ptr_type = pointer_typet(ptr_type);

    return ptr_type;
  }

private:
  enum class BASE_TYPES
  {
    INT,     // 32-bit signed   (Java int)
    LONG,    // 64-bit signed   (Java long)
    SHORT,   // 16-bit signed   (Java short)
    BYTE,    // 8-bit signed    (Java byte)
    CHAR,    // 16-bit unsigned (Java char)
    BOOLEAN, // boolean
    FLOAT,   // IEEE 754 single (Java float)
    DOUBLE,  // IEEE 754 double (Java double)
    _VOID,
    OTHER
  };
  BASE_TYPES bt;
  std::map<std::string, BASE_TYPES> from_map = {
    /* Basic JVM types - widths fixed by the JVM spec (JLS sec. 4.2), not by
       the host platform, so map each to an explicit-width ESBMC type. */
    {"int", BASE_TYPES::INT},
    {"byte", BASE_TYPES::BYTE},
    {"char", BASE_TYPES::CHAR},
    {"short", BASE_TYPES::SHORT},
    {"boolean", BASE_TYPES::BOOLEAN},
    {"long", BASE_TYPES::LONG},
    {"float", BASE_TYPES::FLOAT},
    {"double", BASE_TYPES::DOUBLE},
    {"void", BASE_TYPES::_VOID},
    /* Basic Java classes that can work as primitive types */
    // java.lang.Integer is NOT a primitive: it is a REFERENCE (boxed). Collapsing it to int type-puns
    // a 32-bit value into the pointer slot of a reference container (the collection models' Object[]),
    // so reads come back nondet and every List<Integer>/Map proof goes UNKNOWN. Leaving it OUT of the
    // map routes it to BASE_TYPES::OTHER -> get_base_type returns an (Object-like) pointer; the boxing
    // surface valueOf/intValue is handled as a tagged-pointer cast in jimple_expr.cpp so the int rides
    // in the pointer bits and round-trips type-correctly through the Object[].
    {"java.util.Random",
     BASE_TYPES::
       INT}, // We dont really care about the initialization of this mode
    // java.lang.String is NOT a primitive: it resolves to the char-array String
    // MODEL struct (a reference type) like any other class, so the model's char[]
    // value field and methods bind. Falling through to BASE_TYPES::OTHER makes
    // get_base_type return pointer_typet(tag-java.lang.String) when the model is
    // on the classpath (and an opaque pointer when it is not - never INT).
    /* TODO: these are hacks and should be moved into an intrinsics class */
    {"Main", BASE_TYPES::INT},                     // TODO: handle this properly
    {"java.lang.AssertionError", BASE_TYPES::INT}, // TODO: handle this properly
    {"java.lang.Runtime", BASE_TYPES::INT},        // TODO: handle this properly
    {"java.lang.Class", BASE_TYPES::INT},          // TODO: handle this properly
    {"__other", BASE_TYPES::OTHER}};
};

#endif //ESBMC_JIMPLE_TYPE_H
