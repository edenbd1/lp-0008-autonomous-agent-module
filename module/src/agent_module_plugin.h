#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <logos_module_context.h>
#include <logos_result.h>

#include "agent_module_interface.h"
#include "agent_skills.h"
#include "inference.h"
#include "messaging_skills.h"
#include "program_skills.h"
#include "storage_skills.h"
#include "wallet_skills.h"

namespace logos::agent {

/**
 * @brief Everything the module's own skills need from outside the module.
 *
 * The reason this struct exists rather than the module constructing its own
 * transports: the skills reach Delivery, Storage, the wallet, the sequencer, the
 * local toolchain and a model through injected `std::function`s, and that is
 * what lets the agent module link none of them. Registration therefore cannot
 * invent its ports — it has to be handed them, which is what this is.
 *
 * Every field is default-constructed to *nothing wired*, and that is a
 * supported state rather than an accident: a skill whose port is empty refuses
 * with a message naming what is missing (`"no sequencer connection is wired"`,
 * `"storage upload is not wired"`, `"no inference backend is configured"`). It
 * does not crash on an empty `std::function`, and — the failure that would be
 * worse — it does not report success for work nothing performed. So a partially
 * wired deployment loses exactly the skills it did not wire.
 *
 * The two things a caller must supply for the skills that need them, because the
 * module cannot know them: `agentAccount` (which account the agent reads its own
 * balance from, and in which scope) and `envelope` (the display mirror of the
 * anchored policy — see below).
 */
struct SkillPorts {
    /// Logos Delivery, for `messaging.*`.
    DeliveryPort delivery;
    /// Logos Storage, for `storage.*`.
    StoragePort storage;
    /// How `storage.share` gets an address to a recipient — a messaging act, so
    /// a failure to deliver is reported as that and not as a storage failure.
    SharePort share;
    /// The wallet and the sequencer, for `wallet.*`.
    WalletPort wallet;
    /// How an above-threshold spend reaches the owner. Unwired means every spend
    /// outside the envelope is refused rather than made unattended, which is the
    /// direction this has to fail in.
    OwnerApprovalPort ownerApproval;
    /// The local LEZ toolchain, for `program.call` and `program.deploy`.
    ProgramPort program;
    /// The sequencer's JSON-RPC, for `program.query` and `program.deploy`.
    SequencerPort sequencer;
    /// What `agent.card` needs to describe and sign this agent. Its `skills`
    /// field is wired to this module's own registry when left empty, so the card
    /// cannot advertise a skill the agent has not registered.
    CardPort card;
    /// Where other agents' cards are read from, for `agent.discover`.
    DiscoveryPort discovery;
    /// Transport and settlement for `agent.task`, `agent.subscribe`,
    /// `agent.cancel`.
    TaskPort task;
    /// What `meta.status` reports on. The four fields the module knows about
    /// itself — configured, started, owner, policy hash — are wired to the
    /// module when left empty; the rest (balance, storage usage, account) are
    /// facts about the outside world and stay the caller's to supply.
    StatusPort status;
    /// Where a validated runtime setting goes. Deliberately not defaulted to an
    /// internal map: storing a setting nothing reads would let `meta.configure`
    /// report `"effective":true` for a value that changes nothing.
    ConfigPort config;
    /// What `meta.skills` lists. Wired to this module's own registry when left
    /// empty, for the same reason `card.skills` is: a catalogue assembled
    /// anywhere else could name a skill `invoke()` will not dispatch.
    RegistryPort registry;
    /// The pluggable model behind `agent.evaluate_task`. Null is a supported
    /// deployment: the skill reports that no backend is configured, which is a
    /// decline, and declining is the only direction a backend may move a spend
    /// anyway.
    std::shared_ptr<IInferenceBackend> inference;

    /// This agent's own account, qualified — `Private/<base58>` or
    /// `Public/<base58>`. Which read path an account needs is part of its
    /// identity, so `wallet.balance` will not guess one.
    std::string agentAccount;

    /// The display mirror of the anchored spending envelope.
    ///
    /// One field, not two, on purpose. `wallet.send` wants decimal strings and
    /// `agent.evaluate_task` wants `AnchoredLimits`' `unsigned __int128`;
    /// carrying both would let the two mirrors of one on-chain fact disagree,
    /// and the disagreement would be invisible until a spend. Registration
    /// derives the second from this one, and a limit that does not parse as a
    /// decimal integer becomes zero — every spend then goes to the owner, and
    /// every offer is declined, which is the safe direction.
    ///
    /// Neither copy is the authority. The limits are the PDA seed of the policy
    /// account (`crates/agent-policy-core`), so editing them here changes what
    /// this process bothers asking about and nothing about what the chain
    /// accepts.
    SpendEnvelope envelope;

    /// Where the A2A task lifecycle is kept. Null means the module uses its own,
    /// which is enough to run but cannot be snapshotted from outside: a host
    /// that has to survive a restart owns the store, restores it before
    /// registration, and passes it here. It must outlive the module.
    TaskStore *tasks = nullptr;
};

} // namespace logos::agent

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

    /// Start the agent, and — if nobody has done it yet — put the module's own
    /// skills in the registry first.
    ///
    /// That fallback is the difference between a module that loads and a module
    /// that does anything. Loaded by Basecamp there is no in-process caller to
    /// call @ref registerBuiltinSkills: the host reaches this module through
    /// `configure`/`start`/`skills`/`invoke` and nothing else, so without it a
    /// freshly loaded module answers `skills()` with `[]` and `invoke()` with
    /// "no skill named …" for every name it documents — which looks exactly like
    /// a module that loaded correctly.
    ///
    /// Registered with no ports wired, those skills refuse and say what is
    /// missing. That is the honest state for a module nobody has wired, and it
    /// is distinguishable from both "the skill does not exist" and "it worked".
    ///
    /// The built-ins are registered *before* `started_` is set, so `skills()`
    /// never answers a card that is still being filled in, and outside the lock,
    /// because @ref registerSkill takes it.
    StdLogosResult start();
    StdLogosResult stop();

    /// Register a skill. Third-party skills arrive through this rather than by
    /// editing the module, which is what the prize's usability criterion asks.
    ///
    /// `name()` is called before the lock is taken and inside a try/catch, so a
    /// skill that calls back into the module — or throws — cannot hang or crash
    /// the registration.
    StdLogosResult registerSkill(std::shared_ptr<logos::agent::ISkill> skill);

    /// Register the twenty-two skills this module ships with, wired to `ports`.
    ///
    /// Before @ref start, and once. A host that has transports wires them here;
    /// a host that does not gets the same twenty-two from @ref start with
    /// nothing wired, and each one refuses with the port it is missing.
    ///
    /// `value` is the number registered. A name already taken — a third party
    /// registered its own `wallet.send` first — is *not* overwritten and not
    /// silently dropped: that built-in is skipped, the result is a failure
    /// naming it, and the registry keeps exactly one skill under the name so the
    /// card cannot advertise anything `invoke()` will not dispatch. Whichever
    /// skill holds the name answers for it.
    StdLogosResult registerBuiltinSkills(logos::agent::SkillPorts ports);

    /// Every registered skill and its parameter schema, as a JSON array built
    /// with the JSON library rather than by string splicing.
    ///
    /// This is the module method. The *skill* of that description is
    /// `meta.skills`, which is registered like any other and reads its
    /// catalogue from here — as does `agent.card`. One producer, so the
    /// catalogue, the card and the registry cannot disagree. For a while this
    /// comment said "`meta.skills()`" and there was no such skill: the method
    /// was reachable in-process and by nothing that loads this module.
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
    /// Build the built-in skills and register them. Called with no lock held —
    /// it goes through @ref registerSkill, which takes one.
    StdLogosResult installBuiltinSkills(logos::agent::SkillPorts ports);

    mutable std::mutex mutex_;
    /// Declared before `skills_` so it is destroyed after them: the three task
    /// skills and `meta.status` hold a reference to whichever store is in use.
    logos::agent::TaskStore tasks_;
    std::map<std::string, std::shared_ptr<logos::agent::ISkill>> skills_;
    std::string ownerAddress_;
    std::string policyHashHex_;
    bool configured_ = false;
    bool started_ = false;
    /// Claimed under the lock by whichever of @ref registerBuiltinSkills or
    /// @ref start gets there first, so the built-ins are wired exactly once and
    /// a start that races a wiring caller does not register them twice.
    bool builtinsClaimed_ = false;
    /// A start that has claimed the built-ins but has not finished registering
    /// them. Without it, a second `start()` on another thread would slip past
    /// the `started_` check while the first is still filling the registry.
    bool starting_ = false;
};
