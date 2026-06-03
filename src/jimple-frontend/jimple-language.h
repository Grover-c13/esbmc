#pragma once

#include <langapi/mode.h>
#include <util/language.h>
#include <jimple-frontend/AST/jimple_file.h>

#include <vector>

class jimple_languaget : public languaget
{
public:
  bool parse(const std::string &path) override;

  bool final(contextt &context) override;

  // AST -> GOTO
  bool typecheck(contextt &context, const std::string &module) override;

  std::string id() const override
  {
    return "jimple_lang";
  }

  void add_intrinsics(contextt &context);

  void setup_main(contextt &context);

  void show_parse(std::ostream &out) override;

  // conversion from expression into string
  bool from_expr(
    const exprt &expr,
    std::string &code,
    const namespacet &ns,
    unsigned flags) override;

  // conversion from type into string
  bool from_type(
    const typet &type,
    std::string &code,
    const namespacet &ns,
    unsigned flags) override;

  unsigned default_flags(presentationt target) const override;

  virtual languaget *new_language() const override
  {
    return new jimple_languaget;
  }

  // One entry per class. A single .jimple file holds one class; multiple files
  // (or a top-level JSON array in one file) accumulate here and are all
  // converted into the shared context, so multi-class programs link.
  std::vector<jimple_file> roots;
};
