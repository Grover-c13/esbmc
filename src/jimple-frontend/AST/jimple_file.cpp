#include <map>
#include <fstream>
#include <jimple-frontend/AST/jimple_file.h>

#include <util/std_code.h>
#include <util/expr_util.h>

// (De)-Serialization helpers (from jimple_ast.h)
void to_json(json &, const jimple_ast &)
{
  // Don't care
}

void from_json(const json &j, jimple_ast &p)
{
  p.from_json(j);
}

std::string jimple_file::to_string() const
{
  std::ostringstream oss;
  oss << "Jimple File:\n"
      << "\t"
      << "Name: " << this->class_name << "\n\t"
      << "Mode: " << to_string(this->mode) << "\n\t"
      << "Extends: " << this->extends << "\n\t"
      << "Implements: " << this->implements << "\n\t"
      << this->modifiers.to_string();

  oss << "\n\n";
  for (auto &x : body)
  {
    oss << x->to_string();
    oss << "\n\n";
  }

  return oss.str();
}

void jimple_file::from_json(const json &j)
{
  // Get ClassName
  j.at("name").get_to(this->class_name);

  std::string t;
  j.at("object").get_to(t);
  this->mode = from_string(t);

  if (j.contains("implements"))
    j.at("implements").get_to(this->implements);
  else
    this->implements = "(No implements)";

  if (j.contains("extends"))
    j.at("extends").get_to(this->extends);
  else
    this->implements = "(No extends)";

  modifiers = j.at("modifiers").get<jimple_modifiers>();

  auto filebody = j.at("content");
  for (auto &x : filebody)
  {
    // TODO: Here is where to add support for signatures
    auto content_type = x.at("object").get<std::string>();
    std::shared_ptr<jimple_class_member> to_add;
    if (content_type == "Method")
    {
      jimple_method m;
      x.get_to(m);
      to_add = std::make_shared<jimple_method>(m);
    }
    else if (content_type == "Field")
    {
      jimple_class_field m;
      x.get_to(m);
      to_add = std::make_shared<jimple_class_field>(m);
    }
    else
    {
      log_error("Unsupported object: {}", content_type);
      abort();
    }
    body.push_back(to_add);
  }
}

inline jimple_file::file_type
jimple_file::from_string(const std::string &name) const
{
  return from_map.at(name);
}

inline std::string
jimple_file::to_string(const jimple_file::file_type &ft) const
{
  return to_map.at(ft);
}
void jimple_file::load_file(const std::string &path)
{
  std::ifstream i(path);
  json j;
  i >> j;

  from_json(j);
}

void jimple_file::declare(contextt &ctx) const
{
  // Phase 1: register the class type, static-field globals, and the method
  // symbols (signatures). Bodies are filled later in define(), so that
  // cross-class references resolve regardless of class order.

  std::string id, name;
  id = "tag-" + this->class_name;
  name = this->class_name;

  // Check if class already exists
  if (ctx.find_symbol(id) != nullptr)
    throw "Duplicated class name";

  struct_typet t;
  t.tag(name);

  auto symbol = create_jimple_symbolt(t, name, name, id);
  std::string symbol_name = symbol.id.as_string();

  // A class/interface is a type
  symbol.is_type = true;

  // Add symbol into the context
  ctx.move_symbol_to_context(symbol);
  symbolt *added_symbol = ctx.find_symbol(symbol_name);

  // Add class/interface members
  auto total_size = 0;
  for (auto const &field : body)
  {
    auto cf = std::dynamic_pointer_cast<jimple_class_field>(field);
    if (cf)
    {
      struct_typet::componentt comp;
      exprt &tmp = comp;
      tmp = field->to_exprt(ctx, name, name);
      comp.swap(tmp);
      t.components().push_back(comp);
      // Only bitvector fields carry a width; reference-typed fields (class types,
      // e.g. a lambda singleton's INSTANCE) have none, so skip them in the size
      // sum instead of std::stoi-ing an empty string (which aborts).
      const std::string w = comp.type().width().as_string();
      if (!w.empty())
        total_size += std::stoi(w);

      // A static field is shared global state. Register a global symbol so
      // jimple_static_member can read and write it (e.g. across threads).
      // Instance fields stay as struct components above.
      if (cf->modifiers.is_static())
      {
        typet ft = cf->type.to_typet(ctx);
        std::string gid = name + "." + cf->name;
        if (ctx.find_symbol(gid) == nullptr)
        {
          symbolt g = create_jimple_symbolt(ft, name, cf->name, gid);
          g.lvalue = true;
          g.static_lifetime = true;
          g.set_value(gen_zero(ft));
          ctx.move_symbol_to_context(g);
        }
      }
    }
  }

  // Here is where we add the inherited fields

  // Finally, the structure is ready. Lets add it
  t.set("width", total_size);
  added_symbol->set_type(t);

  // Register method symbols (signatures only); bodies are filled in define().
  for (auto const &field : body)
  {
    auto m = std::dynamic_pointer_cast<jimple_method>(field);
    if (m)
      m->declare(ctx, name);
  }
}

void jimple_file::define(contextt &ctx) const
{
  // Phase 2: fill method bodies. All classes have been declared by now, so
  // cross-class calls/field accesses resolve.
  for (auto const &field : body)
  {
    auto m = std::dynamic_pointer_cast<jimple_method>(field);
    if (m)
      m->define(ctx, this->class_name);
  }
}

exprt jimple_file::to_exprt(contextt &ctx) const
{
  // Single-class convenience: declare then define this one class.
  declare(ctx);
  define(ctx);
  return code_skipt();
}
