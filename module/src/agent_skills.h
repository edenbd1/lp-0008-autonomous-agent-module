#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "agent_module_interface.h"

/**
 * @brief Agent coordination, A2A-compatible, over Logos Messaging.
 *
 * A2A defines three things this file has to be honest about: the Agent Card
 * (who an agent is and what it will do), the task lifecycle (what may follow
 * what), and the transport binding (how the two ends talk). Logos Messaging
 * replaces A2A's HTTP, and LEZ fills the payment gap A2A deliberately leaves
 * open — so a task here can cost money, and money that moved cannot be
 * un-moved by a state machine that allows anything.
 *
 * Three decisions are worth stating up front, because a plausible-looking
 * implementation gets each of them wrong:
 *
 * 1. **The card is signed or it is not produced.** The prize calls the card "a
 *    signed JSON document" and A2A carries the signature as a detached-payload
 *    JWS. `scripts/a2a-task.sh` publishes an unsigned card, which any reader
 *    can forge. `agent.card` refuses when it cannot sign rather than emitting
 *    something that reads like a signed card and is not one.
 *
 * 2. **Local state is never a claim about the remote agent.** `agent.task`
 *    leaves a submitted task in `submitted`. It has posted a request; it has
 *    not seen the other agent start work. `working` arrives as a status update
 *    or not at all.
 *
 * 3. **Nothing reports a payment or a refund it does not have a transaction
 *    for.** A cancel whose refund did not settle says so and stays pending;
 *    that is the same standard `a2a-task.sh` applies when it refuses to write a
 *    manifest for a hash that never landed.
 */
namespace logos::agent {

// ---------------------------------------------------------------------------
// The A2A task lifecycle
// ---------------------------------------------------------------------------

/// The task states A2A defines, spelled exactly as the protocol spells them
/// (`specification/json/a2a.json`, `TaskState`). `Unknown` is A2A's "we have
/// lost track", not a state a task is driven into deliberately.
enum class TaskState {
    Submitted,
    Working,
    InputRequired,
    AuthRequired,
    Completed,
    Canceled,
    Failed,
    Rejected,
    Unknown,
};

/// The wire name, e.g. `input-required`.
std::string taskStateName(TaskState state);

/// Wire name to state. False for anything A2A does not define — an unknown
/// state name is a protocol error, not a reason to invent a state.
bool taskStateFromName(const std::string &name, TaskState &out);

/// Completed, canceled, failed and rejected are final: no update, cancellation
/// or continuation may move a task out of them.
bool isTerminalState(TaskState state);

/// Whether A2A permits `from -> to`.
///
/// `working -> working` is the one self-transition that is allowed: a streaming
/// agent repeats `working` as progress arrives. Every other state that stays
/// put is a bug, and every transition out of a terminal state is a lie about
/// work that already finished.
bool canTransition(TaskState from, TaskState to);

/// Content topic a task's traffic runs on, in the grammar Logos documents:
/// /<application>/<version>/<name>/<encoding>.
///
/// **Empty when either identifier is not one** (@ref isTopicIdentifier). Both
/// halves are spliced into the `<name>` segment, and both can come from a
/// stranger — `agent_address` is read out of a peer's Agent Card — so an id
/// carrying a `/` chooses a different topic entirely: `AAA/json", "x":1, "y":"`
/// produced a topic a victim then broadcast on. Returning empty rather than a
/// forged topic means a caller that forgets to check publishes nowhere instead
/// of publishing somewhere an attacker picked.
std::string taskTopic(const std::string &agentAddress, const std::string &taskId);

/// Base64url, unpadded (RFC 7515 §2). Exposed so a caller — and the tests —
/// can rebuild a card's JWS signing input and check what was signed.
std::string base64Url(const std::string &bytes);

/// The deepest nesting this module accepts in any JSON document handed to it —
/// by a caller, by a peer, or by a snapshot on disk.
///
/// Not a style limit: a memory-safety one. nlohmann's parser and its destructor
/// are iterative, but its **copy constructor is not** — copying a value copies
/// its children recursively, one stack frame per level. A 1.7 KB document
/// nested some hundreds deep therefore overflows a worker thread's 512 KiB
/// stack the moment anything copies it, and the copies are not optional:
/// `summary["card"] = c` in `agent.discover` and `p["params"]` in `agent.task`
/// are both on the accepted path. A stack overflow is a signal, not an
/// exception, so no `try`/`catch` here or in the host turns it back into an
/// error — the bound has to be applied *before* the document is parsed, and it
/// is, by scanning the raw text. Everything past that point is known to be
/// copyable.
///
/// 64 is far past anything this protocol describes: the deepest structure in an
/// A2A Agent Card is a skill's parameter schema, which nests single digits.
inline constexpr int kMaxJsonDepth = 64;

/// Whether `text` nests no deeper than `maxDepth` object/array levels.
///
/// A scan of the raw characters — no recursion, no parse, string contents
/// skipped — because it has to be safe on exactly the documents that are not.
bool withinJsonDepth(const std::string &text, int maxDepth = kMaxJsonDepth);

/**
 * @brief Every task this agent is a party to, and the only place their state
 *        changes.
 *
 * Kept separate from the skills for two reasons the prize asks for directly:
 * pending task state has to survive a node restart (@ref snapshot, @ref
 * restore), and a failing skill must not be able to corrupt the lifecycle —
 * illegal transitions are refused here, once, rather than in each caller.
 *
 * Locked, because the module serves skills from more than one thread.
 */
class TaskStore {
public:
    struct Task {
        std::string id;
        std::string contextId;
        /// The other agent's LEZ account: who this task is with.
        std::string agent;
        /// The skill id requested, e.g. `storage.upload`.
        std::string skill;
        TaskState state = TaskState::Submitted;
        /// What was paid, and where it went. Zero means the task was free.
        std::uint64_t pricePaid = 0;
        std::string payAccount;
        std::string settlementTx;
        bool subscribed = false;
        /// Every state this task has been in, in order, starting at `submitted`.
        std::vector<std::string> history;
        /// The last note attached to a status update; the reason, when failed.
        std::string note;
    };

    /// Open a task in `submitted`. Refuses a duplicate id: two tasks under one
    /// id would let a status update for one settle the other.
    bool create(const std::string &id, const std::string &contextId, const std::string &agent,
                const std::string &skill, std::string &err);

    /// Move a task, if A2A allows the move. Refuses an unknown task rather than
    /// creating one, so a status update for a task we never opened is an error.
    bool advance(const std::string &id, TaskState to, const std::string &note, std::string &err);

    /// Apply an A2A status update as it arrives off the wire:
    /// `{"taskId":"…","status":{"state":"working","message":"…"}}`.
    /// A `Task` object (`{"id":…,"status":{…}}`) is accepted too. Malformed
    /// JSON, an unknown state and an illegal transition are all refused.
    bool applyUpdate(const std::string &eventJson, std::string &err);

    /// Record a settlement against a task. The amount and the transaction hash
    /// travel together: an amount with no hash is not a payment.
    bool recordPayment(const std::string &id, std::uint64_t amount, const std::string &payAccount,
                       const std::string &settlementTx, std::string &err);

    bool markSubscribed(const std::string &id, std::string &err);

    /// A copy, or false when there is no such task.
    bool find(const std::string &id, Task &out) const;

    /// Tasks that are not in a terminal state.
    std::vector<Task> active() const;
    std::size_t size() const;

    /// The whole store as JSON, for restart. Pending task state that only lives
    /// in memory is lost by the first node restart, which the prize counts as a
    /// reliability failure.
    std::string snapshot() const;

    /// Replace the contents from a @ref snapshot. Refuses malformed JSON, an
    /// unknown state name and duplicate ids, and leaves the store untouched
    /// when it refuses — a half-restored store is worse than an empty one.
    ///
    /// *Refuses*, and never throws. Every field is type-checked rather than read
    /// with `value()`, which throws when the key is present with the wrong type:
    /// a structured retype of a snapshot produced 40 exceptions escaping this
    /// function, each of them a promise in this comment being broken by the code
    /// under it. `pricePaid` gets one check more — a negative or fractional
    /// price is refused rather than converted, because `"pricePaid": -1`
    /// restored cleanly as 18446744073709551615 and `agent.cancel` then called
    /// `refund()` for 2^64-1 and reported ok.
    bool restore(const std::string &snapshotJson, std::string &err);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Task> tasks_;
};

// ---------------------------------------------------------------------------
// Agent Cards
// ---------------------------------------------------------------------------

/// Check a document against the A2A Agent Card schema (v0.3.0) plus the two
/// rules this transport binding adds.
///
/// Returns an empty string when the card is valid, otherwise why it is not.
/// This exists because "it parsed as JSON" is not discovery: a card without
/// `capabilities`, or with a `provider` that names an organization and no URL —
/// which A2A requires alongside it, and which the card in `scripts/a2a-task.sh`
/// omits — cannot be used, and accepting it only moves the failure somewhere
/// less obvious.
///
/// The added rules: a `logos-messaging` card's `url` must actually be a
/// `logos-messaging://` address (A2A binds `preferredTransport` to `url`), and
/// an `x-logos` block advertising a price must name the account to pay it into.
std::string validateAgentCard(const std::string &cardJson);

/// What `agent.card` needs to know about the agent it is describing.
struct CardPort {
    std::function<std::string()> name;
    std::function<std::string()> description;
    /// The agent's own version, not the protocol's.
    std::function<std::string()> version;
    /// The shielded LEZ account that is this agent's identity.
    std::function<std::string()> lezAccount;
    /// The public account a price is paid into. Public on purpose: `spel`
    /// resolves `Private/<id>` only for accounts the signing wallet holds keys
    /// for, so a payer cannot name our private account — and a public payee is
    /// the only reason a settlement is checkable from outside with `getAccount`.
    std::function<std::string()> payAccount;
    /// LEZ per task. Zero advertises a free agent.
    std::function<std::uint64_t()> pricePerTask;
    std::function<std::string()> providerOrganization;
    std::function<std::string()> providerUrl;
    /// The registered skills, in the shape the plugin's own `skills()` returns:
    /// `[{"name":"storage.upload","parameters":{…}}]`. Taking the registry's own
    /// output means the card cannot advertise a skill the agent does not have.
    /// The `meta.skills` skill reads the same function — see @ref RegistryPort;
    /// it wraps this array in the `{"ok":true,…}` envelope every skill answers
    /// in, which is the one difference between the two.
    std::function<std::string()> skills;
    /// Detached-payload JWS (RFC 7515 appendix F): given
    /// `<protected>.<payload>`, return the base64url signature. Empty means the
    /// key refused or is absent, and an unsigned card is not published.
    std::function<std::string(const std::string &signingInput)> sign;
    /// The `kid` naming the key `sign` used, so a verifier can find it.
    std::function<std::string()> keyId;
    /// JWS `alg`. Empty defaults to `EdDSA`, the LEZ account key type.
    std::function<std::string()> algorithm;
};

/// Where other agents' cards are read from.
struct DiscoveryPort {
    /// Cards seen on a discovery topic, each as a JSON document, newest last.
    /// The topic is passed through exactly as given: turning a namespace into a
    /// content topic is `discoveryTopic()`'s job in the messaging skills, not
    /// this one's.
    std::function<std::vector<std::string>(const std::string &topic)> fetch;
};

/// What the task skills need from the transport and the wallet.
struct TaskPort {
    /// Delivery's readiness, the same one the messaging skills refuse on:
    /// `start` returns as soon as it is dispatched, so a send straight after it
    /// can vanish.
    std::function<bool()> ready;
    std::function<bool(const std::string &topic, const std::string &json)> send;
    std::function<bool(const std::string &topic)> subscribe;
    /// Pay a price into an account. Returns the settlement transaction hash, or
    /// empty if nothing settled. Anything above the anchored envelope is
    /// refused by the chain, not here.
    std::function<std::string(const std::string &payAccount, std::uint64_t amount)> pay;
    /// Reverse a settlement of `amount` that was paid into `paidAccount`.
    /// Returns the refund transaction hash, or empty when no refund settled.
    std::function<std::string(const std::string &paidAccount, std::uint64_t amount)> refund;

    // -----------------------------------------------------------------------
    // THE ENVELOPE, ON THE PATH WHERE THE PRICE IS CHOSEN BY SOMEBODY ELSE
    //
    // `agent.task` pays a price it read out of the *peer's* Agent Card, into an
    // account named in that same card. `max_price` is optional, so leaving it
    // out used to mean "unlimited": a card advertising 2^64-1 LEZ payable to
    // `Public/ATTACKER` was paid in full, with no per-transaction and no
    // per-period check anywhere on the path. `wallet.send` has never worked that
    // way, and `docs/architecture.md` already documented the check that was
    // missing. These four fields are that check, and they are deliberately the
    // same shape as `SpendEnvelope` and `OwnerApprovalPort` in `wallet_skills.h`
    // so the two spending paths give the same answer.
    //
    // As there, and for the reason argued in `agent_module_interface.h`, this is
    // a *courtesy*: the anchored policy account is the enforcer. What it buys is
    // that a payment the chain would refuse goes to the owner before the agent
    // pays proving cost for it — and that an unwired agent pays nothing rather
    // than everything. Every unknown is outside the envelope: an agent that
    // cannot say what its limits are, or what it has already spent, cannot say a
    // payment is inside them.
    // -----------------------------------------------------------------------

    /// Largest single task payment the agent may make unattended, in LEZ as
    /// decimal digits. Absent or unparseable means *unknown*, which is outside.
    std::function<std::string()> perTxLimit;
    /// Largest total the agent may move in one period, same encoding.
    std::function<std::string()> perPeriodLimit;
    /// What it has already moved in this period, same encoding. Unknown is not
    /// zero: assuming zero is precisely how a period limit becomes decorative.
    std::function<std::string()> spentThisPeriod;
    /// Deliver an above-envelope task payment to the owner. False means the
    /// owner was not reached — and such a payment is not made, the task is not
    /// opened, and nothing is sent.
    std::function<bool(const std::string &requestJson)> requestApproval;
};

/// What `meta.status` reports on.
struct StatusPort {
    std::function<bool()> configured;
    std::function<bool()> started;
    std::function<std::string()> ownerAddress;
    std::function<std::string()> policyHash;
    std::function<std::string()> lezAccount;
    /// The balance as a decimal string. **Empty means the chain could not be
    /// reached** — reported as null, never as zero: an agent that says it holds
    /// nothing when it cannot see its own account has told the owner a
    /// falsehood at the worst possible moment.
    std::function<std::string()> balance;
    /// Storage usage as JSON, e.g. `{"files":3,"bytes":8192}`. Empty when the
    /// storage node could not be asked.
    std::function<std::string()> storage;
};

/// Where a runtime setting goes once it has been validated.
struct ConfigPort {
    /// Apply a setting. False means the module refused it.
    std::function<bool(const std::string &key, const std::string &value)> set;
    std::function<std::string(const std::string &key)> get;
};

/// Where `meta.skills` reads the catalogue from.
struct RegistryPort {
    /// Every registered skill and its parameter schema, in the shape the
    /// plugin's own `skills()` returns: `[{"name":"storage.upload",
    /// "parameters":{…}}]`. Taking the registry's own output — the same
    /// function `CardPort::skills` is wired to — is what keeps the catalogue
    /// and the Agent Card from being two answers to one question.
    std::function<std::string()> listing;
};

// ---------------------------------------------------------------------------
// The skills
// ---------------------------------------------------------------------------

/// `agent.card()` — this agent's signed A2A Agent Card.
///
/// Building only. Publishing it to a discovery topic is `messaging.send`'s job,
/// the same split `storage.share` makes: a card is a document, publishing is a
/// messaging act.
class CardSkill final : public ISkill {
public:
    explicit CardSkill(CardPort port) : port_(std::move(port)) {}
    std::string name() const override { return "agent.card"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    CardPort port_;
};

/// `agent.discover(topic)` — cards published by other agents, validated.
class DiscoverSkill final : public ISkill {
public:
    explicit DiscoverSkill(DiscoveryPort port) : port_(std::move(port)) {}
    std::string name() const override { return "agent.discover"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    DiscoveryPort port_;
};

/// `agent.task(agent_address, skill, params)` — open a task with another agent,
/// or answer one that asked for input.
///
/// Order matters and is deliberate: check, post the request, then pay. A price
/// paid for a request that never left the node buys nothing, so a transport
/// failure must not reach the wallet.
///
/// The first of those checks is the spending envelope (@ref TaskPort). A price
/// above it is held for the owner *before* anything is sent — not after, because
/// a request that is out and a payment that is held is a task the peer believes
/// it is owed for.
class TaskSkill final : public ISkill {
public:
    TaskSkill(TaskPort port, TaskStore &tasks) : port_(std::move(port)), tasks_(tasks) {}
    std::string name() const override { return "agent.task"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    TaskPort port_;
    TaskStore &tasks_;
};

/// `agent.subscribe(agent_address, task_id)` — stream a running task's status
/// updates over Logos Messaging.
class SubscribeSkill final : public ISkill {
public:
    SubscribeSkill(TaskPort port, TaskStore &tasks) : port_(std::move(port)), tasks_(tasks) {}
    std::string name() const override { return "agent.subscribe"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    TaskPort port_;
    TaskStore &tasks_;
};

/// `agent.cancel(agent_address, task_id)` — cancel a task and trigger any
/// refund.
class CancelSkill final : public ISkill {
public:
    CancelSkill(TaskPort port, TaskStore &tasks) : port_(std::move(port)), tasks_(tasks) {}
    std::string name() const override { return "agent.cancel"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    TaskPort port_;
    TaskStore &tasks_;
};

/// `meta.status()` — what the agent is bound to, what it holds, what it is
/// doing. Answers even when the module is unconfigured: a diagnostic that
/// refuses in the state you most need it is not a diagnostic.
class StatusSkill final : public ISkill {
public:
    StatusSkill(StatusPort port, const TaskStore &tasks) : port_(std::move(port)), tasks_(tasks) {}
    std::string name() const override { return "meta.status"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    StatusPort port_;
    const TaskStore &tasks_;
};

/// `meta.configure(key, value)` — update one runtime setting.
///
/// Only known keys, each validated for its own shape. The envelope keys
/// (`per_tx`, `per_period`, `period_blocks`) are accepted but reported as *not
/// effective*: those limits live in the PDA seed of the anchored policy
/// account, so writing a larger number here changes what the agent asks the
/// owner about and changes nothing about what the chain will let it spend.
class ConfigureSkill final : public ISkill {
public:
    explicit ConfigureSkill(ConfigPort port) : port_(std::move(port)) {}
    std::string name() const override { return "meta.configure"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    ConfigPort port_;
};

/// `meta.skills()` — every registered skill and its parameter schema.
///
/// This existed as prose before it existed as code. Three doc comments
/// described it — `agent_module_interface.h`, `agent_module_plugin.h`, and the
/// note on `CardPort::skills` above — while `invoke("meta.skills")` answered
/// *no skill named 'meta.skills' is registered*, because `invoke()` is a plain
/// map lookup and nothing had ever put that name in the map. A documented skill
/// that does not dispatch is worse than an absent one: a caller reads the
/// header, writes against it, and finds out at run time.
///
/// It is a skill rather than only a module method on purpose. `AgentModuleImpl`
/// has always had `skills()`, and an in-process host could call it — but a host
/// that *loads* this module reaches it through `invoke()` and nothing else, and
/// another agent over A2A reaches it through `invoke()` and nothing else. The
/// information was reachable; the skill was not.
///
/// The listing is passed through as the registry built it, including the
/// `{"name":…,"error":…}` entry a skill gets when its own `parameterSchema()`
/// throws or does not parse. That isolation is the plugin's, and re-deriving it
/// here would be a second place for it to be wrong.
class SkillsSkill final : public ISkill {
public:
    explicit SkillsSkill(RegistryPort port) : port_(std::move(port)) {}
    std::string name() const override { return "meta.skills"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    RegistryPort port_;
};

} // namespace logos::agent
