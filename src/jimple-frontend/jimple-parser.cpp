#include <fstream>
#include <jimple-frontend/AST/jimple_file.h>
#include <jimple-frontend/jimple-language.h>

void jimple_languaget::show_parse(std::ostream &out)
{
  for (auto &r : roots)
    out << r.to_string();
}

bool jimple_languaget::parse(const std::string &path)
{
  log_debug("jimple", "Parsing: {}", path);
  try
  {
    // Accumulate, don't overwrite: each parse() call appends its class(es) so
    // multiple .jimple files on the command line all get converted. A single
    // file may also contain a top-level JSON array of classes (a whole program
    // emitted in one shot).
    std::ifstream i(path);
    json j;
    i >> j;

    if (j.is_array())
    {
      for (auto &cls : j)
      {
        jimple_file f;
        f.from_json(cls);
        roots.push_back(std::move(f));
      }
    }
    else
    {
      jimple_file f;
      f.from_json(j);
      roots.push_back(std::move(f));
    }
  }

  catch (std::exception &e)
  {
    log_error("{}", e.what());
    return true;
  }

  return false;
}
