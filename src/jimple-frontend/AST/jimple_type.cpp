#include <jimple-frontend/AST/jimple_type.h>
#include <util/std_types.h>

void jimple_type::from_json(const json &j)
{
  j.at("identifier").get_to(name);
  j.at("dimensions").get_to(dimensions);

  bt = from_map.count(name) != 0 ? from_map[name] : BASE_TYPES::OTHER;
}

typet jimple_type::get_base_type(const contextt &ctx) const
{
  switch (bt)
  {
  // Widths are fixed by the JVM spec (JLS sec. 4.2), independent of the host
  // platform, so use explicit-width bitvector/IEEE types rather than the
  // platform-dependent C-frontend helpers (int_type(), etc.).
  case BASE_TYPES::INT:
    return signedbv_typet(32);

  case BASE_TYPES::LONG:
    return signedbv_typet(64);

  case BASE_TYPES::SHORT:
    return signedbv_typet(16);

  case BASE_TYPES::BYTE:
    return signedbv_typet(8);

  case BASE_TYPES::CHAR:
    // Java char is an unsigned 16-bit UTF-16 code unit.
    return unsignedbv_typet(16);

  case BASE_TYPES::FLOAT:
    // IEEE 754 single precision (build_float_type honours
    // use_fixed_for_float; 32 -> floatbv with 23-bit fraction otherwise).
    return build_float_type(32);

  case BASE_TYPES::DOUBLE:
    // IEEE 754 double precision.
    return build_float_type(64);

  case BASE_TYPES::BOOLEAN:
    return bool_type();

  case BASE_TYPES::_VOID:
    return empty_typet();

  default:
    auto symbol = ctx.find_symbol("tag-" + name);
    if (symbol == nullptr)
      // Unknown class/interface — typically a library type we don't convert
      // (e.g. java.lang.Runnable appearing only in a cast). Model it as an
      // opaque pointer instead of aborting; such values aren't used as data.
      return pointer_typet(empty_typet());
    return pointer_typet(symbol->get_type());
  }
}

typet jimple_type::to_typet(const contextt &ctx) const
{
  if (is_array())
    return get_arr_type(ctx);
  return get_base_type(ctx);
}

std::string jimple_type::to_string() const
{
  std::ostringstream oss;
  oss << "Type: " << name << " [" << dimensions << "]";
  return oss.str();
}
