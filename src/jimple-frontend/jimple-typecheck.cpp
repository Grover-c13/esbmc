#include <jimple-frontend/jimple-language.h>
#include <jimple-frontend/jimple-converter.h>

bool jimple_languaget::typecheck(contextt &context, const std::string &)
{
  // Convert every parsed class into the shared context so they link together.
  for (auto &r : roots)
  {
    log_status("Converting Jimple module {} to GOTO", r.class_name);
    jimple_converter converter(context, r);
    if (converter.convert())
    {
      log_error("Failed to convert module {}", r.class_name);
      return true;
    }
  }

  // Conversion has populated the context; drop the ASTs so a second typecheck
  // pass (should one occur) does not re-add the classes and trip the
  // duplicate-class check.
  roots.clear();

  return false;
}
