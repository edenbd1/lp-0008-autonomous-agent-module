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
#include "task_persistence.h"
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
    /// Neither copy is the authority. The limits are the *data* of the agent's
    /// one policy account (`crates/agent-policy-core`), which only the policy
    /// program may write, so editing them here changes what this process
    /// bothers asking about and nothing about what the chain accepts.
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
 *
 * WHAT SURVIVES A RESTART, AND WHAT THE MODULE DOES ABOUT IT
 *
 * The host hands every module a private data directory
 * (`instancePersistencePath()`), and a module that does not write to it loses
 * every pending task the first time the node restarts. That was measured on this
 * module rather than assumed: a task opened through `agent.task` showed up in
 * `meta.status` as `{"total":1,"active":1}`, the persistence directory was
 * *empty*, and the same module rebuilt against the same directory came back
 * reporting `{"total":0}` — with no error anywhere, because losing everything
 * and having nothing look exactly alike.
 *
 * So @ref onContextReady constructs a `TaskPersistence` over the module's own
 * `TaskStore`, @ref start recovers from it, and @ref invoke checkpoints after any
 * call that moved the store. The recovery decision is the part worth stating: a
 * snapshot that *cannot be read* does not start the agent. Truncated, corrupt,
 * oversized, from a future schema — those are "we do not know what was pending",
 * and coming up with an empty task list on top of them is precisely how a paid
 * task gets paid twice.
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

    /// Install the one notification path a *loaded* module has to its owner.
    ///
    /// A plugin cannot be handed a `std::function` port across the boundary, so
    /// without this the built-in `wallet.send` has no owner channel at all and
    /// every above-threshold spend is refused with "no owner channel to ask for
    /// approval" — safe, and useless. What crosses the boundary in the other
    /// direction is a module *event*, so the module's Logos Core export hands
    /// the impl a notifier here that emits `ownerApprovalRequested`, and the
    /// owner answers with @ref approveSpend, which is a module *method*. Both
    /// halves are on the runtime's own transport; neither needs an intermediary
    /// server.
    ///
    /// `notify` returns whether the request went anywhere. False is a real
    /// answer and not an error: an export whose host has not wired the event
    /// sink yet would otherwise report every unheard notification as delivered.
    ///
    /// Set before @ref start, and it is what decides whether the built-in owner
    /// channel is wired at all: a module constructed directly in a unit test has
    /// no notifier, so it keeps the old, honest refusal instead of waiting out a
    /// timeout against an owner that has no way to answer it.
    void setOwnerNotifier(std::function<bool(const std::string &requestJson, int attempt)> notify);

    /// The owner's answer to a request the agent published.
    ///
    /// `verdict` is `"approved"` or `"denied"`, and nothing else — an answer the
    /// agent cannot read is refused here rather than counted as either.
    ///
    /// Refuses a `requestId` no spend is waiting on. That is not pedantry: an
    /// approval stored ahead of the request it names would be waiting for the
    /// *next* spend that happened to be minted with the same id, and an approval
    /// that authorises a payment nobody has described yet is the one shape this
    /// path must never have.
    ///
    /// An answer here does not move money. It releases the `wallet.send` that is
    /// waiting, which then reports the approval and submits nothing — the
    /// policy program's `spend_approved` path is what submits an approved spend,
    /// and this module does not wire it. See docs/limitations.md.
    StdLogosResult approveSpend(const std::string &requestId, const std::string &verdict);

protected:
    /// The host has handed over `instancePersistencePath()`; open the task
    /// snapshot under it. Fires once, before any method dispatch.
    ///
    /// Nothing is *read* here — @ref start does the recovery, because a failed
    /// recovery has to be able to refuse the start, and this hook cannot.
    void onContextReady() override;

private:
    /// Build the built-in skills and register them. Called with no lock held —
    /// it goes through @ref registerSkill, which takes one.
    StdLogosResult installBuiltinSkills(logos::agent::SkillPorts ports);

    /// Write the task store down if it has moved since the last write.
    ///
    /// Called after every @ref invoke rather than from inside the three task
    /// skills, deliberately: the store is reachable from any skill a host
    /// registers, and a checkpoint that only the built-ins trigger would miss
    /// exactly the third-party skill this module's usability criterion invites
    /// people to write. The comparison against the last written snapshot is what
    /// keeps that from being an fsync per call.
    void checkpointTasks();

    /// `{"path":…,"recovery":…,…}` for `meta.status`, or empty when this module
    /// was never given a directory to write to.
    std::string durabilityJson() const;

    /// One notification attempt on the built-in owner channel.
    bool publishApprovalRequest(const std::string &requestJson);
    /// `"approved"`, `"denied"`, or empty for "nothing has come back yet".
    std::string approvalAnswerFor(const std::string &requestId) const;
    /// A stored setting as milliseconds, or `fallback` when it is unset or is
    /// not a number this build can use.
    std::int64_t settingMs(const char *key, std::int64_t fallback) const;

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

    /// Runtime settings from `meta.configure`, and the only ones this module
    /// actually reads back. The `ConfigPort` used to be left unwired on purpose
    /// — "storing a setting nothing reads would let `meta.configure` report
    /// `effective:true` for a value that changes nothing" — and that argument
    /// still holds for every key here except the two approval timings, which the
    /// owner wait reads on each above-threshold spend. So the map exists, and
    /// `meta.configure` keeps reporting the anchored keys as *not* effective.
    std::map<std::string, std::string> settings_;

    // ---- the built-in owner channel ---------------------------------------
    //
    // Guarded by `mutex_`. `wallet.send` polls `approvalAnswerFor` from inside
    // its wait while `approveSpend` writes from the transport thread, so these
    // two really are touched concurrently — unlike most of the state above.

    /// Set by the module's Logos Core export. Empty means this module is not
    /// hosted, and therefore has no owner channel of its own.
    std::function<bool(const std::string &requestJson, int attempt)> ownerNotify_;
    /// Requests a `wallet.send` is currently waiting on, by correlation id,
    /// against the number of times each has been put on the wire. An answer
    /// that names an id not in here is refused.
    std::map<std::string, int> pendingApprovals_;
    /// Answers that have arrived, by correlation id.
    std::map<std::string, std::string> approvalAnswers_;

    // ---- durability -------------------------------------------------------
    //
    // Its own lock, not `mutex_`: a save ends in an fsync, and holding the
    // module's lock across one would stall every other skill call — including
    // the `approveSpend` that a waiting `wallet.send` is depending on.
    mutable std::mutex durableMutex_;
    /// Null until the host hands over a persistence directory. Null is a
    /// supported state — the module runs, and `meta.status` says plainly that
    /// nothing is being written down.
    std::unique_ptr<logos::agent::TaskPersistence> durable_;
    /// What the last recovery found, for `meta.status`.
    logos::agent::LoadReport lastLoad_;
    bool loadRan_ = false;
    /// The snapshot text last written, so a checkpoint that would rewrite the
    /// same bytes does not.
    std::string lastSaved_;
    /// The last failed save, if any. Reported rather than thrown: the work the
    /// skill did really did happen, and the thing that failed is the record of
    /// it — which is exactly what an operator needs told.
    std::string lastSaveError_;
};
