#ifndef ESBMC_JIMPLE_HIERARCHY_H
#define ESBMC_JIMPLE_HIERARCHY_H

#include <string>
#include <vector>
#include <util/context.h>

/**
 * @brief Run-wide class hierarchy for the Jimple frontend.
 *
 * Populated once during typecheck (after every class is declared) from each
 * parsed class's `extends` (superclass) and `implements` (interfaces) edges, so
 * a virtual call can be resolved against the receiver's real type rather than
 * the call site's declared base type. Without this, a call through an interface
 * / superclass reference (`List l = new MyList(); l.size()`) binds to the
 * library model's nondet stub instead of the subclass override, and a true
 * property comes back falsely REFUTED.
 */
class jimple_hierarchy
{
public:
  /// Drop all recorded edges (between runs / typecheck passes).
  static void clear();

  /// Record `cls`'s direct superclass `ext` and implemented interfaces `impls`.
  static void add(
    const std::string &cls,
    const std::string &ext,
    const std::vector<std::string> &impls);

  /**
   * @brief The class providing the body an object of static type `recv_class`
   * would run for `method`, i.e. JVM virtual dispatch.
   *
   * Walks the superclass (`extends`) chain from `recv_class` upward and returns
   * the first class for which a `<class>:<method>` symbol exists in `ctx`.
   * Returns "" when none is found, so the caller can fall back to the declared
   * base type (a genuinely-polymorphic / unmodelled site is unchanged).
   */
  static std::string resolve_method(
    contextt &ctx,
    const std::string &recv_class,
    const std::string &method);

  /// Direct superclass recorded for `cls` ("" if none / unknown).
  static std::string superclass(const std::string &cls);
};

#endif
