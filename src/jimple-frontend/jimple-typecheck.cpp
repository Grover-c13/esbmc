#include <jimple-frontend/jimple-language.h>
#include <jimple-frontend/jimple-converter.h>

bool jimple_languaget::typecheck(contextt &context, const std::string &)
{
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
