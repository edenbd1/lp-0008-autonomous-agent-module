#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <logos_module_context.h>
#include <logos_result.h>

#include "agent_module_interface.h"

/**
 * @brief The LP-0008 agent, as a Logos Core module.
 *
 * Lifecycle contract, mirroring the delivery module the owner channel runs on:
 * - @ref configure exactly once, with the anchored policy address
 * - @ref start before any skill call
 * - @ref stop before shutdown
 *
 * Each of those is enforced rather than documented. "Exactly once" used to be
 * prose only: configure → start → configure returned ok and silently rebound a
 * running agent to a different owner and a different policy anchor, which is
 * not a reconfiguration but a takeover.
 *
 * The agent's authority is bounded by an account address, not by this code. The
 * policy account's PDA seed is the hash of (owner, agent, per-tx, per-period,
 * period), so a compromised agent cannot raise its own ceiling: it would have to
 * present a different account, which nobody created.
 *
 * Third-party skills are called with no lock held, so a skill may call back into
 * the module, and every call into one is wrapped: a skill that throws costs its
 * own entry, not the module.
 */
class AgentModuleImpl : public LogosModuleContext
{
public:
    AgentModuleImpl();
    ~AgentModuleImpl() override;

    /// Bind the agent to its owner and its anchored policy. Once, and only once.
    ///
    /// `policyHashHex` is the 32-byte seed of the on-chain policy account, as 64
    /// hex characters. The limits are read back from the chain rather than
    /// supplied here, so a tampered config file cannot widen them.
    ///
    /// A second call is refused and changes nothing, including after @ref stop:
    /// the binding is the agent's identity, and rebinding it in place would let
    /// anything that can reach this method redirect the agent's spending.
    StdLogosResult configure(const std::string &ownerAddress,
                          const std::string &policyHashHex);

    StdLogosResult start();
    StdLogosResult stop();

    /// Register a skill. Third-party skills arrive through this rather than by
    /// editing the module, which is what the prize's usability criterion asks.
    ///
    /// `name()` is called before the lock is taken and inside a try/catch, so a
    /// skill that calls back into the module — or throws — cannot hang or crash
    /// the registration.
    StdLogosResult registerSkill(std::shared_ptr<logos::agent::ISkill> skill);

    /// `meta.skills()` — every registered skill and its parameter schema, as a
    /// JSON array built with the JSON library rather than by string splicing.
    ///
    /// A skill whose `parameterSchema()` throws or does not parse as a JSON
    /// object gets `{"name":…,"error":…}` in place of its parameters; the other
    /// skills are unaffected. Before @ref start this returns the object
    /// `{"error":…}` rather than `[]`, because an empty array is
    /// indistinguishable from an agent that genuinely has no skills and would be
    /// published as a valid — and empty — Agent Card.
    std::string skills() const;

    /// Call a registered skill by name. Returns the skill's own JSON result, or
    /// `{"ok":false,"error":…}` if the agent is not started, the name is not
    /// registered, the skill threw, or the skill's answer was not JSON.
    std::string invoke(const std::string &name, const std::string &paramsJson);

    /// `{"configured":…,"started":…,"owner":…,"policy":…}` — what this agent is
    /// bound to and whether it is running, so a misbinding is visible from
    /// outside instead of only at the next spend.
    std::string status() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<logos::agent::ISkill>> skills_;
    std::string ownerAddress_;
    std::string policyHashHex_;
    bool configured_ = false;
    bool started_ = false;
};
