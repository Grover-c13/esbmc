#include <jimple-frontend/AST/jimple_type.h>

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
  case BASE_TYPES::INT:
    return int_type();

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
