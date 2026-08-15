#pragma once

#include <string>

/**
 * @brief What the agent module offers the rest of Logos Core.
 *
 * Kept separate from the implementation so a skill can be added without
 * touching the plugin, which the prize asks for explicitly: "a documented skill
 * interface (module/SDK) that can be used to add new skills without modifying
 * the core agent module".
 *
 * WHERE THE SPENDING ENVELOPE IS, AND WHY IT IS NOT DECLARED HERE
 *
 * This header used to declare a `SpendPolicy` (per-transaction and per-period
 * limits) and a `SpendOutcome` whose `OwnerUnreachable` carried a comment
 * saying the prize requires it to be terminal. Nothing constructed either type,
 * nothing returned it, and there is no spend call in this module for them to
 * describe — the entire contract was the comment. A declared type reads as an
 * implemented one, so it claimed more than the absent code did.
 *
 * The envelope is real; it is simply not C++. It is defined in
 * `crates/agent-policy-core` (`SpendPolicy::is_autonomous`,
 * `compute_policy_hash`) and *enforced* by the anchored policy account in
 * `crates/agent-verifier-spel`, whose address is derived from the limits
 * themselves. A check in this process would be advisory at best: the agent
 * holds its own keys, so whoever controls the process controls the spending,
 * and a limit it can edit is a comment. The agent cannot raise its own ceiling
 * because the ceiling is the *name* of the account it must present, not data it
 * can write — and an above-threshold spend that never reaches the owner has
 * nothing to present, so it does not execute.
 *
 * If a C++ mirror is ever wanted for display, note that a u128 has no portable
 * spelling in C++17. The deleted struct used `unsigned __int128`, which is a
 * GCC/Clang extension, is unavailable on MSVC and on 32-bit targets, and this
 * is a header third parties are told to include. Carry the amount as a pair of
 * `uint64_t` or as a decimal string instead.
 */
namespace logos::agent {

/// A skill the agent can perform. Third parties implement this; the plugin
/// discovers registered skills and exposes them through `meta.skills()`.
///
/// Every method below is third-party code from the plugin's point of view, so
/// the plugin calls all three inside a try/catch and never while holding its
/// own lock. Two guarantees follow, and both are tested:
/// - a skill that throws from any method degrades to a reported error on that
///   skill; the exception does not cross back into the host;
/// - a skill may call back into the module — `skills()`, `invoke()`,
///   `registerSkill()` — from any method without deadlocking.
class ISkill {
public:
    virtual ~ISkill() = default;

    /// Stable identifier, e.g. `storage.upload`.
    ///
    /// The plugin calls this once, at registration, and publishes the
    /// registered name from then on: a name that changed between calls would
    /// advertise a skill that `invoke()` cannot dispatch.
    virtual std::string name() const = 0;

    /// JSON Schema of the parameters, published in the agent's A2A Agent Card
    /// so another agent can call this skill without out-of-band knowledge.
    ///
    /// Must parse as a JSON *object*. Anything else is reported as an error on
    /// that skill's entry: the schema used to be spliced into the card as raw
    /// text, where one malformed schema — or an empty string — produced a
    /// document that broke discovery for every other skill too.
    virtual std::string parameterSchema() const = 0;

    /// Perform the skill. Returns a JSON result.
    ///
    /// A throwing or failing skill must not take the module down with it: the
    /// prize requires skill failures to be isolated, so the plugin catches here
    /// rather than trusting implementers. A result that is not JSON is refused
    /// for the same reason it is escaped rather than spliced — it would corrupt
    /// whatever document the caller puts it in.
    virtual std::string invoke(const std::string &paramsJson) = 0;
};

} // namespace logos::agent
