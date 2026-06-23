#include <jimple-frontend/jimple-language.h>
#include <jimple-frontend/jimple-converter.h>
#include <jimple-frontend/AST/jimple_hierarchy.h>

bool jimple_languaget::typecheck(contextt &context, const std::string &)
{
  // Record the class hierarchy (extends/implements) before any body is
  // converted, so a virtual call resolves against the receiver's real type.
  jimple_hierarchy::clear();
  for (auto &r : roots)
  {
    std::vector<std::string> impls;
    if (!r.implements.empty() && r.implements.rfind("(No", 0) != 0)
      impls.push_back(r.implements);
    std::string ext =
      (!r.extends.empty() && r.extends.rfind("(No", 0) != 0) ? r.extends
                                                             : std::string();
    jimple_hierarchy::add(r.class_name, ext, impls);
  }

  // Two-pass over all parsed classes so cross-class references link regardless
  // of order: first declare every class (type, static globals, method
  // signatures), then fill every method body.
  for (auto &r : roots)
  {
    log_status("Converting Jimple module {} to GOTO", r.class_name);
    try
    {
      r.declare(context);
    }
    catch (const char *e)
    {
      log_error("Failed to declare module {}: {}", r.class_name, e);
      return true;
    }
  }

  for (auto &r : roots)
    r.define(context);

  // Conversion has populated the context; drop the ASTs so a second typecheck
  // pass (should one occur) does not re-add the classes and trip the
  // duplicate-class check.
  roots.clear();

  return false;
}
