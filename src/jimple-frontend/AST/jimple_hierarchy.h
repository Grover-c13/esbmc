#ifndef ESBMC_JIMPLE_HIERARCHY_H
#define ESBMC_JIMPLE_HIERARCHY_H

#include <string>
#include <vector>
#include <util/context.h>

/**
 * @brief Run-wide class hierarchy + runtime-dispatch registry for the Jimple
 * frontend.
 *
 * Populated once during typecheck (after every class is declared) from each
 * parsed class's `extends` (superclass) and `implements` (interface) edges.
 *
 * It serves two roles:
 *
 *  1. Static subtype queries (`superclass`, `is_subtype`) used to over-
 *     approximate the set of runtime types a base/interface-typed reference may
 *     hold.
 *
 *  2. Runtime virtual dispatch. Every class is given a stable integer id
 *     (`class_id`). At allocation the object's `@class_identifier` field is set
 *     to that id; at a virtual call we read it back and switch over the
 *     candidate overrides (`candidate_overrides`). This is the only sound way
 *     to dispatch `this.m()` inside an inherited base-class method, where the
 *     receiver's runtime type is not visible from its static type.
 */
class jimple_hierarchy
{
public:
  /// The struct component / field name carrying an object's runtime class id.
  static const char *class_id_field();

  /// Drop all recorded state (between runs / typecheck passes).
  static void clear();

  /// Record `cls`'s direct superclass `ext` and implemented interfaces `impls`.
  static void add(
    const std::string &cls,
    const std::string &ext,
    const std::vector<std::string> &impls);

  /// Direct superclass recorded for `cls` ("" if none / unknown).
  static std::string superclass(const std::string &cls);

  /// Stable non-zero integer id for `cls` (0 = unknown class). Assigned lazily.
  static int class_id(const std::string &cls);

  /// True if `sub` is `base`, or transitively extends/implements it.
  static bool is_subtype(const std::string &sub, const std::string &base);

  /**
   * @brief The class providing the body an object of static type `recv_class`
   * would run for `method`, i.e. JVM virtual dispatch over a known static type.
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

  /**
   * @brief Candidate runtime override bodies of a virtual call.
   *
   * The runtime object is always a SUBTYPE of the static receiver type, so the
   * candidate set is the root type (the declared static type `recv_static`, or
   * `base_class` when unknown) plus its transitive subtypes by `is_subtype`
   * (extends + recorded interface edges). For each, the body it would actually
   * run is resolved via `resolve_method` (own override or nearest inherited).
   *
   * The returned vector's LAST entry is the fallback (the root's own body): the
   * dispatch site emits it as the `else` arm, so an object whose runtime id
   * matches no earlier arm runs the root body. Earlier entries are the subtypes
   * whose body DIFFERS from the fallback, each { class_id(C), "owner:method" }.
   *
   * Soundness note: this assumes the recorded type graph is complete. Because
   * the producer records only one interface edge per class today, a multi-
   * interface implementor reached through a second interface may be missed; such
   * a call binds to the root body / nondet stub (sound over-approximation: an
   * imprecise REFUTED, never a masked bug) until the producer emits full edges.
   */
  static std::vector<std::pair<int, std::string>> candidate_overrides(
    contextt &ctx,
    const std::string &base_class,
    const std::string &recv_static,
    const std::string &method);
};

#endif
