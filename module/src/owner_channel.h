#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

/**
 * @brief The owner channel: the agent asks, the owner answers, over Logos
 *        Messaging and nothing else.
 *
 * The prize asks that "the owner can interact with the agent in real time from a
 * separate Logos app instance using Logos Messaging, with no intermediary
 * server", and that above-threshold spends are held until the owner approves.
 * This class is the request/response half of that: a *correlated* exchange —
 * every message names the payment it is about — rather than a fire-and-forget
 * notification that cannot tell a late answer from an answer to something else.
 *
 * WHAT THIS CHANNEL IS NOT
 *
 * It is not the authority. An `approve` message arriving here does not let the
 * agent spend a penny more than its anchored ceiling. The ceiling is the data
 * of the agent's one policy account, which only the policy program may write, and
 * an above-threshold payment goes through `spend_approved`, which requires an
 * approval account at a PDA seeded by `compute_approval_marker(compute_spend_ref(
 * policy_hash, recipient, amount, nonce))` — an account only the owner's own
 * signature on `approve_spend` can create, once, for that exact payment.
 *
 * So a forged approval message buys an attacker nothing on chain: it makes the
 * agent build a transaction that fails at an account that was never initialised.
 * The checks below are defence in depth, not the defence. They exist because an
 * agent that acts on a message it cannot verify wastes a proof and a fee, and
 * because a channel that reports "the owner approved" on a message that names
 * different terms is lying to its operator even when the chain saves it.
 *
 * That is also why an approval here can never raise a limit: there is no field
 * in the reply that could. A reply carrying `per_tx`, `per_period` or
 * `period_blocks` is refused outright rather than ignored, because the only
 * reason to send one is to try.
 *
 * TRANSPORT
 *
 * A reliable channel, not a bare topic. Delivery's `entryLayer` defaults to
 * `channels` for a reason: an approval that is silently dropped is
 * indistinguishable from an owner who is asleep, and this class would report the
 * second when the first happened. The module API is
 * `channelCreate(channelId, contentTopic, senderId)`, `channelSend(channelId,
 * payload)`, with inbound frames arriving as `channelMessageReceived(channelId,
 * senderId, payload, timestamp)`.
 *
 * Delivery's lifecycle is `createNode` once, then `start`, then message
 * operations, then `stop` — and `start` returns as soon as it is dispatched, with
 * completion arriving later as a `nodeStarted` event. So `ready()` is checked
 * before opening and before every send: a channel opened into a node that is not
 * up yet is a channel whose messages go nowhere.
 */
namespace logos::agent {

/// One frame off the reliable channel, after the transport has decoded it.
struct InboundMessage {
    /// The SDS sender identifier the frame arrived with. A hint about who spoke,
    /// not proof: sender ids are self-declared. The proof is on chain.
    std::string senderId;
    std::vector<std::uint8_t> payload;
};

/// The name you register the listener under.
inline constexpr const char *kInboundListenerName = "onChannelMessageReceived";

/// The name that actually comes back, inside the event payload's `eventType`.
///
/// These are two different strings and that is the trap: register
/// `onChannelMessageReceived`, then match that same string against `eventType`,
/// and nothing ever fires. The channel sends perfectly and never hears a word,
/// which looks exactly like an owner who never answered — and this class would
/// dutifully report the owner as unreachable — a wrong answer that reads
/// like a right one. Hence two constants, and @ref parseInboundEvent matches the
/// second.
inline constexpr const char *kInboundEventType = "channel_message_received";

/// Wire protocol identifier, carried in every message so the owner's app can
/// tell an approval exchange from anything else sharing the topic.
inline constexpr const char *kOwnerChannelProtocol = "/lp-0008/1/owner-channel";

/// Decode one raw liblogosdelivery event into an inbound frame.
///
/// Only for adapters wired to the C library directly; the delivery *module*
/// already hands over decoded bytes through its `channelMessageReceived` event.
/// It lives here because the event-name trap above lives here, and because the
/// channel payload crosses the FFI base64-encoded (`node_api.nim` encodes it) —
/// two details a caller should not have to rediscover.
///
/// Returns false for anything that is not a channel message for `channelId`,
/// including a payload that is not valid base64: half-decoded bytes would be
/// parsed as a truncated reply, and a truncated reply is not a "no".
bool parseInboundEvent(const std::string &eventJson,
                       const std::string &channelId,
                       InboundMessage &out);

/// The exact terms of one payment. Every field is part of what the owner is
/// asked to approve, and every field is checked again in the answer.
///
/// `amount` is a decimal string because the chain's amounts are `u128`, which no
/// JSON number can hold; passing it as text keeps the agent, the owner's app and
/// the transaction builder looking at the same digits.
///
/// Named for the exchange it belongs to, not for the payment: `wallet_skills.h`
/// already has a `SpendRequest`, which is the *other* half — what goes to the
/// policy program once there is something to submit. Two different structs of
/// the same name in one namespace is not a warning, it is a silent ODR
/// violation, and both headers now live in one library target, so the two halves
/// of the approval path could never have been included together.
struct ApprovalRequest {
    /// Correlation id: a name for *these terms*, unique for the life of the
    /// channel. Answers are matched on it.
    std::string id;
    /// Hex seed of the anchored policy account, which is what bounds this agent.
    std::string policyHash;
    /// Account id that will be paid — an account, not a label. `spend_approved`
    /// checks the paid account against the one the approval names.
    std::string recipient;
    std::string amount;
    /// What makes this payment distinct from an identical later one. Without it,
    /// "send 100 to Bob" approved once would authorise the same transfer forever.
    std::uint64_t nonce = 0;
    /// Hex seed of the approval account, derived by `agent-policy-core` from
    /// exactly the four fields above. Never computed here: one derivation, in
    /// one place, re-derived on chain by `approve_spend`.
    std::string markerSeed;
};

/// What came back, or did not.
enum class ApprovalVerdict {
    /// The owner answered these exact terms with a yes.
    Approved,
    /// The owner answered these exact terms with a no. A complete, successful
    /// exchange with a negative answer — not a failure of the channel.
    Denied,
    /// Answers claiming this request's id arrived, none of them naming its
    /// terms, and the wait then ran out. Terminal: see below.
    Refused,
    /// Nothing usable arrived before the timeout, after retrying the request.
    Unreachable,
    /// The transport would not carry the request: node not started, the channel
    /// would not open, or the first send was refused. The owner was never asked.
    ChannelUnavailable,
    /// The request itself was not fit to send, so nothing was sent. A caller
    /// error, not a channel outcome.
    NotAsked,
};

struct ApprovalDecision {
    ApprovalVerdict verdict = ApprovalVerdict::Unreachable;
    std::string requestId;
    /// Why, in words an operator can act on.
    std::string detail;
    /// How many times the request was put on the wire. The prize requires the
    /// agent to retry notification before timing out; this is the count.
    int attempts = 0;
    /// Frames seen on the channel that were not answers to this request.
    int ignored = 0;
    /// Frames that claimed this request's id and named other terms.
    ///
    /// Reported on every verdict, including @ref ApprovalVerdict::Approved: an
    /// operator whose owner app is answering with the wrong amount, or whose
    /// topic somebody else is writing to, needs to know that even when the real
    /// answer arrived afterwards.
    int altered = 0;
    /// Echoed only on @ref ApprovalVerdict::Approved: the seed the caller hands
    /// to `spend_approved`. It is the request's own marker seed — an approval
    /// that named a different one never gets this far.
    std::string markerSeed;

    bool approved() const { return verdict == ApprovalVerdict::Approved; }

    /// The state the prize calls "the owner could not be reached": terminal, and
    /// never a fallback to acting alone. "Above-threshold transactions that fail
    /// to reach the owner for approval are not executed — the agent retries
    /// notification before timing out and reports the failure."
    ///
    /// `Unreachable`, `ChannelUnavailable` and `Refused` all mean the same thing
    /// to a caller holding an above-threshold payment: no valid approval arrived,
    /// so nothing is submitted. They stay distinct in @ref verdict because an
    /// operator needs to know whether the node was down, the owner was away, or
    /// somebody was answering with altered terms.
    ///
    /// `Refused` is reached at the *end* of the wait, not the instant an altered
    /// frame is seen. Ending the exchange on the first one made a remote,
    /// repeatable denial of approval out of a single injected frame: anyone who
    /// could write to the topic could stop every spend the owner wanted to allow.
    /// And it bought nothing — an attacker able to forge a frame naming other
    /// terms is equally able to forge one naming *these* terms, so refusing to
    /// read any further message never stood between an attacker and an approval.
    /// What does stand there is the approval account on chain, which no message
    /// on this channel can create.
    ///
    /// It is deliberately false for `Approved`. An approval unlocks the
    /// `spend_approved` path and nothing more: whether the payment happened is
    /// the chain's answer, given to whoever watched the transaction land, and
    /// reporting it from here would announce a payment that has not been made.
    /// It is false for `Denied` too — the owner was reached and said no.
    bool ownerUnreachable() const;

    /// Same shape as the skills return, so a caller — or the owner's app — can
    /// branch without knowing what produced it. Deliberately no `ok` field: a
    /// denial is a successful exchange, and `ok:false` would read as a fault.
    std::string json() const;
};

/// What the channel needs from the delivery module, named so it can be exercised
/// against a fake and so the agent module does not link delivery directly.
///
/// This mirrors `DeliveryModuleImpl`'s reliable-channel API rather than the
/// topic-level `send` in `DeliveryPort`: the owner channel is the one place where
/// at-least-once delivery is worth the extra state.
struct OwnerChannelPort {
    /// Has the node reported `nodeStarted`? Not "did `start` return".
    std::function<bool()> ready;
    /// `channelCreate(channelId, contentTopic, senderId)`.
    std::function<bool(const std::string &channelId,
                       const std::string &contentTopic,
                       const std::string &senderId)> channelCreate;
    /// `channelSend(channelId, payload)`.
    std::function<bool(const std::string &channelId,
                       const std::vector<std::uint8_t> &payload)> channelSend;
    /// Everything that has arrived on `channelId` since the last call, oldest
    /// first. The adapter owning the `channelMessageReceived` listener queues
    /// frames; this drains the queue.
    std::function<std::vector<InboundMessage>(const std::string &channelId)> drain;
    /// Monotonic milliseconds. Injected so a timeout is testable without sleeping
    /// through one.
    std::function<std::int64_t()> nowMs;
    /// Called between polls so waiting does not spin a core. Optional; a fake
    /// leaves it empty and a real adapter sleeps a little.
    std::function<void()> idle;
};

struct OwnerChannelConfig {
    /// The content topic the channel runs on. The plugin passes
    /// `ownerTopic(ownerAccount)` from messaging_skills.h — it is a parameter
    /// rather than a derivation here so the topic grammar has one owner, and so a
    /// deployment can pin a different topic without editing this file.
    ///
    /// Known leak: every agent belonging to one owner shares that content topic,
    /// so an observer counting traffic learns how many agents an owner runs and
    /// how busy they are. The messages are encrypted; the pattern is not.
    std::string contentTopic;
    /// Who must approve. Half of the channel id, so two agents of the same owner
    /// do not share a channel and read each other's approvals.
    std::string ownerAccount;
    /// This agent. Its SDS sender identifier on the channel.
    std::string agentAccount;
    /// How long an above-threshold spend waits before it is abandoned. Terminal:
    /// the prize is explicit that a transaction which fails to reach the owner
    /// must not execute, so this timeout never falls back to acting alone.
    std::int64_t timeoutMs = 120000;
    /// How often the same request — same id, same terms — is put back on the
    /// wire while waiting. "Retries notification before timing out."
    std::int64_t resendIntervalMs = 15000;
    /// Frames from any other sender are ignored. **Required**: @ref
    /// OwnerChannel::open refuses without it.
    ///
    /// It used to be optional, and empty was the default — and an empty value
    /// accepted *every* sender as the owner. Measured: a third party who could
    /// see the topic replayed the request's own terms back and got
    /// `verdict:"approved"`. An unset filter that silently means "anyone" is
    /// worse than no filter at all, because it reads like one that is on.
    ///
    /// It is still a hint and still proves nothing — sender ids are
    /// self-declared, see @ref InboundMessage::senderId — and the proof is the
    /// approval account on chain. What requiring it buys is that the agent
    /// cannot be *configured* into believing everybody by leaving a field out.
    std::string ownerSenderId;
};

/**
 * @brief A dedicated, correlated, retrying approval channel with one owner.
 *
 * Lifecycle: @ref open once, then @ref requestApproval per above-threshold
 * payment. Both refuse rather than pretend.
 */
class OwnerChannel {
public:
    OwnerChannel(OwnerChannelConfig config, OwnerChannelPort port);

    /// Stable on both ends without configuration: both sides know both accounts.
    std::string channelId() const { return channelId_; }
    std::string contentTopic() const { return config_.contentTopic; }
    bool isOpen() const { return open_; }

    /// Open the reliable channel. `{"ok":true,...}` or `{"ok":false,"error":...}`.
    ///
    /// Refuses when the port cannot hear as well as when it cannot speak: a
    /// half-wired port that can send and has no inbound path would turn every
    /// approval into a plausible-looking timeout.
    std::string open();

    /// Ask the owner, wait, and answer with what actually happened.
    ApprovalDecision requestApproval(const ApprovalRequest &request);

    /// The exact bytes @ref requestApproval puts on the wire. Exposed so the
    /// owner-side app can be written against the same document, and so a test can
    /// read what was sent rather than trust that something was.
    static std::string encodeRequest(const ApprovalRequest &request,
                                     const std::string &agentAccount,
                                     std::int64_t expiresAtMs);

private:
    OwnerChannelConfig config_;
    OwnerChannelPort port_;
    std::string channelId_;
    bool open_ = false;
    /// id -> fingerprint of the terms it was minted for. An id is a name for one
    /// payment; reusing it for different terms would let the answer to the first
    /// settle the second.
    std::map<std::string, std::string> asked_;
};

} // namespace logos::agent
