#pragma once

#include <cstdint>
#include <string>

/**
 * @brief What the agent module offers the rest of Logos Core.
 *
 * Kept separate from the implementation so a skill can be added without
 * touching the plugin, which the prize asks for explicitly: "a documented skill
 * interface (module/SDK) that can be used to add new skills without modifying
 * the core agent module".
 *
 * The spending calls below deliberately do *not* decide whether a transfer is
 * allowed. That decision belongs to the on-chain policy program
 * (`crates/agent-verifier-spel`), whose account address encodes the owner's
 * limits. A check here would be advisory: the agent holds its own keys on a
 * remote node, so anything it can decide, an attacker holding the process can
 * decide differently. What these calls do is *build* the transaction the chain
 * will accept or reject.
 */
namespace logos::agent {

/// The owner's limits, mirrored from the anchored policy account.
///
/// Held for display and for deciding whether to ask the owner *before* paying
/// for a transaction that the chain would reject anyway. It is a cache of an
/// on-chain fact, never the source of it.
struct SpendPolicy {
    /// Largest single transfer the agent may make unattended.
    unsigned __int128 perTx = 0;
    /// Largest total the agent may move within one period, unattended.
    unsigned __int128 perPeriod = 0;
    /// Length of that period, in blocks.
    std::uint64_t periodBlocks = 0;
};

/// Why a spend was held rather than submitted.
enum class SpendOutcome {
    /// Inside the envelope: submitted without asking anyone.
    Autonomous,
    /// Outside it: an approval request went to the owner and we are waiting.
    AwaitingOwnerApproval,
    /// The owner approved and the transfer was submitted.
    ApprovedAndSubmitted,
    /// The owner did not answer within the timeout. Nothing was submitted.
    ///
    /// The prize is explicit that an above-threshold transaction which fails to
    /// reach the owner must not execute — so this is a terminal state, not a
    /// fallback to autonomous.
    OwnerUnreachable,
};

/// A skill the agent can perform. Third parties implement this; the plugin
/// discovers registered skills and exposes them through `meta.skills()`.
class ISkill {
public:
    virtual ~ISkill() = default;

    /// Stable identifier, e.g. `storage.upload`.
    virtual std::string name() const = 0;

    /// JSON Schema of the parameters, published in the agent's A2A Agent Card
    /// so another agent can call this skill without out-of-band knowledge.
    virtual std::string parameterSchema() const = 0;

    /// Perform the skill. Returns a JSON result.
    ///
    /// A throwing or failing skill must not take the module down with it: the
    /// prize requires skill failures to be isolated, so the plugin catches here
    /// rather than trusting implementers.
    virtual std::string invoke(const std::string &paramsJson) = 0;
};

} // namespace logos::agent
