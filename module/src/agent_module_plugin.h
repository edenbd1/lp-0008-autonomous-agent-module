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
 * The agent's authority is bounded by an account address, not by this code. The
 * policy account's PDA seed is the hash of (owner, agent, per-tx, per-period,
 * period), so a compromised agent cannot raise its own ceiling: it would have to
 * present a different account, which nobody created.
 */
class AgentModuleImpl : public LogosModuleContext
{
public:
    AgentModuleImpl();
    ~AgentModuleImpl() override;

    /// Bind the agent to its owner and its anchored policy.
    ///
    /// `policyHash` is the 32-byte seed of the on-chain policy account. The
    /// limits are read back from the chain rather than supplied here, so a
    /// tampered config file cannot widen them.
    StdLogosResult configure(const std::string &ownerAddress,
                          const std::string &policyHashHex);

    StdLogosResult start();
    StdLogosResult stop();

    /// Register a skill. Third-party skills arrive through this rather than by
    /// editing the module, which is what the prize's usability criterion asks.
    StdLogosResult registerSkill(std::shared_ptr<logos::agent::ISkill> skill);

    /// `meta.skills()` — every registered skill and its parameter schema.
    std::string skills() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<logos::agent::ISkill>> skills_;
    std::string ownerAddress_;
    std::string policyHashHex_;
    bool started_ = false;
};
