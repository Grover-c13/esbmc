#include <jimple-frontend/AST/jimple_hierarchy.h>
#include <map>
#include <set>
#include <sstream>

namespace
{
// Run-wide; the frontend converts one program per process invocation.
std::map<std::string, std::string> g_super;
std::map<std::string, std::vector<std::string>> g_interfaces;
std::map<std::string, int> g_class_id;
int g_next_id = 1; // 0 is reserved for "unknown class"
} // namespace

const char *jimple_hierarchy::class_id_field()
{
  return "@class_identifier";
}

void jimple_hierarchy::clear()
{
  g_super.clear();
  g_interfaces.clear();
  g_class_id.clear();
  g_next_id = 1;
}

void jimple_hierarchy::add(
  const std::string &cls,
  const std::string &ext,
  const std::vector<std::string> &impls)
{
  if (cls.empty())
    return;
  if (!ext.empty() && ext != cls)
    g_super[cls] = ext;
  if (!impls.empty())
    g_interfaces[cls] = impls;
  class_id(cls); // assign an id eagerly so declared classes are dispatchable
}

std::string jimple_hierarchy::superclass(const std::string &cls)
{
  auto it = g_super.find(cls);
  return it == g_super.end() ? std::string() : it->second;
}

int jimple_hierarchy::class_id(const std::string &cls)
{
  if (cls.empty())
    return 0;
  auto it = g_class_id.find(cls);
  if (it != g_class_id.end())
    return it->second;
  int id = g_next_id++;
  g_class_id[cls] = id;
  return id;
}

bool jimple_hierarchy::is_subtype(
  const std::string &sub,
  const std::string &base)
{
  if (sub.empty() || base.empty())
    return false;
  std::set<std::string> visited;
  std::vector<std::string> work{sub};
  while (!work.empty())
  {
    std::string cur = work.back();
    work.pop_back();
    if (cur == base)
      return true;
    if (!visited.insert(cur).second)
      continue;
    std::string sup = superclass(cur);
    if (!sup.empty())
      work.push_back(sup);
    auto it = g_interfaces.find(cur);
    if (it != g_interfaces.end())
      for (const auto &iface : it->second)
        work.push_back(iface);
  }
  return false;
}

std::string jimple_hierarchy::resolve_method(
  contextt &ctx,
  const std::string &recv_class,
  const std::string &method)
{
  std::set<std::string> visited;
  std::string cur = recv_class;
  while (!cur.empty() && visited.insert(cur).second)
  {
    std::ostringstream oss;
    oss << cur << ":" << method;
    if (ctx.find_symbol(oss.str()) != nullptr)
      return cur;
    cur = superclass(cur);
  }
  return std::string();
}

std::vector<std::pair<int, std::string>> jimple_hierarchy::candidate_overrides(
  contextt &ctx,
  const std::string &base_class,
  const std::string &recv_static,
  const std::string &method)
{
  // The runtime object is always a SUBTYPE of the static receiver type, so the
  // candidate set is that root type plus its transitive descendants. Prefer the
  // declared static type `recv_static` (tightest); fall back to `base_class`.
  // This stays sound while avoiding the blow-up of enumerating every class that
  // merely defines a same-named method.
  std::string root = !recv_static.empty() ? recv_static : base_class;

  // Resolve the body class `cls` actually runs (own override or nearest
  // inherited one) -- but only if it is a real INSTANCE method (its @this param
  // exists). Statics that share the name, and unmodelled methods, return "".
  auto owner_body = [&](const std::string &cls) -> std::string
  {
    std::string owner = resolve_method(ctx, cls, method);
    if (owner.empty())
      return std::string();
    if (ctx.find_symbol(owner + ":" + method + "@@this") == nullptr)
      return std::string();
    return owner;
  };

  // Collect root + every transitive subtype of it that the program declares.
  std::set<std::string> subtypes;
  for (const auto &kv : g_class_id)
    if (kv.first == root || is_subtype(kv.first, root))
      subtypes.insert(kv.first);

  // The fallback body is the one the root type itself runs; it covers every
  // runtime class whose body does NOT differ from it (e.g. the dozens of JDK
  // model classes that all inherit the same nondet stub), in ONE arm instead of
  // dozens. Only classes whose body genuinely differs get their own cid arm.
  std::string fallback_owner = owner_body(root);

  std::vector<std::pair<int, std::string>> out;

  // One arm per subtype whose runtime body DIFFERS from the fallback, keyed on
  // that subtype's own class id (a subtype sharing the fallback body needs no
  // arm -- it lands on the else branch correctly).
  for (const auto &cls : subtypes)
  {
    std::string owner = owner_body(cls);
    if (!owner.empty() && owner != fallback_owner)
      out.emplace_back(class_id(cls), owner + ":" + method);
  }

  // Append the fallback last (the switch's else branch). If the root has no body
  // at all but some subtype did, fall back to the first override instead.
  if (!fallback_owner.empty())
    out.emplace_back(class_id(root), fallback_owner + ":" + method);
  else if (out.empty())
    return out; // genuinely unmodelled -> caller emits nondet

  return out;
}
