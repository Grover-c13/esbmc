#include <jimple-frontend/AST/jimple_hierarchy.h>
#include <map>
#include <set>
#include <sstream>

namespace
{
// Run-wide; the frontend converts one program per process invocation.
std::map<std::string, std::string> g_super;
std::map<std::string, std::vector<std::string>> g_interfaces;
} // namespace

void jimple_hierarchy::clear()
{
  g_super.clear();
  g_interfaces.clear();
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
}

std::string jimple_hierarchy::superclass(const std::string &cls)
{
  auto it = g_super.find(cls);
  return it == g_super.end() ? std::string() : it->second;
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
