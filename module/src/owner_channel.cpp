#include "owner_channel.h"

// For `isTopicIdentifier`: the channel id is built by concatenation too, and
// the grammar has one owner in this repository.
#include "messaging_skills.h"

#include <nlohmann/json.hpp>

namespace logos::agent {

using nlohmann::json;

namespace {

/// Same envelope the skills answer in, so a caller can branch on `ok` without
/// knowing which part of the module produced the string.
std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
}

/// Read a string field without letting nlohmann throw out of a message handler.
/// `value(key, default)` is not enough: it throws when the key exists with the
/// wrong type, and a hostile frame is exactly where that happens.
std::string str(const json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// Strict: any invalid character, misplaced padding or bad length fails the whole
/// decode. Returning the bytes decoded so far would hand the caller a truncated
/// JSON document, and a truncated approval is not a "no" — it is a message that
/// cannot be read, which must be ignored rather than acted on.
bool base64Decode(const std::string &in, std::vector<std::uint8_t> &out)
{
    out.clear();
    if (in.size() % 4 != 0) return false;

    std::size_t body = in.size();
    std::size_t pad = 0;
    while (body > 0 && in[body - 1] == '=' && pad < 2) {
        --body;
        ++pad;
    }
    // Padding in the middle needs no check of its own: '=' has no value in the
    // alphabet, so it fails below on the same path as any other character that is
    // not base64. An explicit check here would be a branch no test can tell apart
    // from its own absence.

    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < body; ++i) {
        const int v = b64val(in[i]);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

/// A u128 amount as the chain sees it: digits, nothing else, up to the 39 a
/// u128 can hold. Returns empty for anything that is not that.
///
/// Leading zeros are stripped so that "0100" and "100" are the same amount
/// rather than a spurious "the owner altered the terms" — the comparison must
/// catch a different *number*, not a different spelling of the same one.
std::string canonicalAmount(const std::string &s)
{
    if (s.empty() || s.size() > 39) return {};
    for (const char c : s) {
        if (c < '0' || c > '9') return {};
    }
    std::size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') ++i;
    return s.substr(i);
}

/// Everything an id is a name for.
std::string fingerprint(const ApprovalRequest &r)
{
    return r.policyHash + "|" + r.recipient + "|" + canonicalAmount(r.amount) + "|" +
           std::to_string(r.nonce) + "|" + r.markerSeed;
}

enum class ReplyKind {
    /// Not an answer to this request. Ignore it and keep waiting: the channel is
    /// shared, and another payment's answer must not settle this one.
    NotForThisRequest,
    /// Claims this request's id, but does not name this request's terms — or
    /// cannot be read as a yes or a no.
    Altered,
    /// A yes or a no about exactly these terms.
    Answer,
};

struct ReplyCheck {
    ReplyKind kind = ReplyKind::NotForThisRequest;
    bool approve = false;
    std::string why;
};

/// Does this frame answer *this* payment?
///
/// The order matters. Anything that is not addressed to this request is dropped
/// quietly. Once a frame claims this request's id, every term must match before
/// the decision is even read — an approval is authority over one payment, and a
/// message that names other terms is either tampering or a broken owner app.
/// Neither is a yes.
ReplyCheck checkReply(const std::string &text, const ApprovalRequest &req)
{
    ReplyCheck c;

    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        c.why = "unreadable frame";
        return c;
    }
    if (str(j, "protocol") != kOwnerChannelProtocol) {
        c.why = "not an owner-channel message";
        return c;
    }
    if (str(j, "type") != "spend_approval_reply") {
        c.why = "not an approval reply";
        return c;
    }
    if (str(j, "id") != req.id || req.id.empty()) {
        c.why = "answers a different request";
        return c;
    }

    // From here the frame claims to be about this payment, so it is answerable
    // for its contents. Everything below refuses rather than ignores.
    c.kind = ReplyKind::Altered;

    // An approval names a payment; it cannot set a limit. The ceiling is the
    // name of the anchored policy account, so a reply carrying limits could not
    // raise it even if this code believed it — but the only reason to send one
    // is to try, and a request that is being probed is not one to act on.
    for (const char *forbidden : {"per_tx", "per_period", "period_blocks", "policy"}) {
        if (j.contains(forbidden)) {
            c.why = std::string("the reply carries '") + forbidden +
                    "': an approval names a payment, it cannot change a limit";
            return c;
        }
    }

    if (str(j, "policy_hash") != req.policyHash) {
        c.why = "the reply names a different anchored policy";
        return c;
    }
    if (str(j, "recipient") != req.recipient) {
        c.why = "the reply names a different recipient";
        return c;
    }
    const std::string amount = canonicalAmount(str(j, "amount"));
    if (amount.empty() || amount != canonicalAmount(req.amount)) {
        c.why = "the reply names a different amount";
        return c;
    }
    if (!j.contains("nonce") || !j["nonce"].is_number_unsigned() ||
        j["nonce"].get<std::uint64_t>() != req.nonce) {
        c.why = "the reply names a different nonce";
        return c;
    }
    // The seed is what `spend_approved` will present on chain. A reply that
    // agrees with every term and names another seed is pointing the agent at
    // somebody else's approval account.
    if (str(j, "marker_seed") != req.markerSeed) {
        c.why = "the reply names a different approval marker";
        return c;
    }

    const std::string decision = str(j, "decision");
    if (decision == "approve") {
        c.kind = ReplyKind::Answer;
        c.approve = true;
        return c;
    }
    if (decision == "deny") {
        c.kind = ReplyKind::Answer;
        c.approve = false;
        c.why = str(j, "reason");
        return c;
    }
    c.why = "the reply is neither 'approve' nor 'deny'";
    return c;
}

const char *verdictName(ApprovalVerdict v)
{
    switch (v) {
    case ApprovalVerdict::Approved:           return "approved";
    case ApprovalVerdict::Denied:             return "denied";
    case ApprovalVerdict::Refused:            return "refused";
    case ApprovalVerdict::Unreachable:        return "unreachable";
    case ApprovalVerdict::ChannelUnavailable: return "channel_unavailable";
    case ApprovalVerdict::NotAsked:           return "not_asked";
    }
    return "unknown";
}

/// A monotonic clock does not stall, but a wrongly wired one would, and a module
/// that hangs forever is worse than one that reports a bad clock. Bounded so the
/// wait always ends.
constexpr int kMaxPolls = 1 << 20;

} // namespace

bool parseInboundEvent(const std::string &eventJson,
                       const std::string &channelId,
                       InboundMessage &out)
{
    if (channelId.empty()) return false;

    const json j = json::parse(eventJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    // kInboundEventType, never kInboundListenerName. See the header: they are
    // different strings, and matching the one you registered with means never
    // hearing anything at all.
    if (str(j, "eventType") != kInboundEventType) return false;
    if (str(j, "channelId") != channelId) return false;

    std::vector<std::uint8_t> bytes;
    if (!base64Decode(str(j, "payload"), bytes)) return false;

    out.senderId = str(j, "senderId");
    out.payload = std::move(bytes);
    return true;
}

bool ApprovalDecision::ownerUnreachable() const
{
    switch (verdict) {
    case ApprovalVerdict::Unreachable:
    case ApprovalVerdict::ChannelUnavailable:
    case ApprovalVerdict::Refused:
        // Terminal, all three: no valid approval arrived, so an above-threshold
        // payment is not submitted and does not fall back to acting alone.
        return true;
    case ApprovalVerdict::Approved:
    case ApprovalVerdict::Denied:
    case ApprovalVerdict::NotAsked:
        break;
    }
    return false;
}

std::string ApprovalDecision::json() const
{
    nlohmann::json j{
        {"verdict", verdictName(verdict)},
        {"approved", approved()},
        {"id", requestId},
        {"detail", detail},
        {"attempts", attempts},
        {"ignored", ignored},
        {"altered", altered},
        {"owner_unreachable", ownerUnreachable()},
    };
    if (approved()) j["marker_seed"] = markerSeed;
    return j.dump();
}

OwnerChannel::OwnerChannel(OwnerChannelConfig config, OwnerChannelPort port)
    : config_(std::move(config)), port_(std::move(port))
{
    // Both ends derive this from what they both already know, so nothing has to
    // be configured twice and mistyped once. The agent account is in it because
    // one owner may run several agents on the same content topic, and an agent
    // reading another agent's approvals is a spending bug.
    channelId_ = std::string(kOwnerChannelProtocol) + "/" + config_.ownerAccount + "/" +
                 config_.agentAccount;
}

std::string OwnerChannel::open()
{
    if (open_) {
        return done(json{{"channel", channelId_}, {"topic", config_.contentTopic}});
    }
    if (config_.contentTopic.empty() || config_.ownerAccount.empty() ||
        config_.agentAccount.empty()) {
        return fail("the owner channel needs a content topic, an owner and an agent account");
    }
    // Both accounts are spliced into the channel id below. An account carrying a
    // '/' would name a channel of somebody else's choosing, which is the same
    // defect the content topics had.
    if (!isTopicIdentifier(config_.ownerAccount) || !isTopicIdentifier(config_.agentAccount)) {
        return fail("the owner and agent accounts are spliced into the channel id, so they may "
                    "only carry letters, digits, '-' and '_'");
    }
    // Refuse rather than believe everybody. An empty `ownerSenderId` accepted
    // every sender on the channel as the owner: a third party who could see the
    // topic replayed the request's own terms and was told the owner approved.
    // A filter that is off by default is not a default, it is a hole.
    if (config_.ownerSenderId.empty()) {
        return fail("the owner channel has no owner sender id, and an unset one accepts every "
                    "sender on the topic as the owner");
    }
    if (config_.timeoutMs <= 0 || config_.resendIntervalMs <= 0) {
        return fail("the approval timeout and resend interval must both be positive");
    }
    if (!port_.ready || !port_.channelCreate || !port_.channelSend || !port_.nowMs) {
        return fail("the delivery port is not wired for sending");
    }
    // Refuse when it cannot hear, not only when it cannot speak. A channel with
    // no inbound path sends every request perfectly and reports every one of
    // them as an owner who never answered.
    if (!port_.drain) {
        return fail("the delivery port has no inbound path");
    }
    // `start` returns on dispatch; the node is up only once it has said so.
    if (!port_.ready()) {
        return fail("delivery node is not started");
    }
    if (!port_.channelCreate(channelId_, config_.contentTopic, config_.agentAccount)) {
        return fail("delivery refused to open the owner channel");
    }
    open_ = true;
    return done(json{{"channel", channelId_},
                     {"topic", config_.contentTopic},
                     {"owner", config_.ownerAccount}});
}

std::string OwnerChannel::encodeRequest(const ApprovalRequest &request,
                                        const std::string &agentAccount,
                                        std::int64_t expiresAtMs)
{
    // Everything the owner's app needs to render the question and to sign
    // `approve_spend` for it, and nothing that could widen anything.
    return json{
        {"protocol", kOwnerChannelProtocol},
        {"type", "spend_approval_request"},
        {"id", request.id},
        {"agent", agentAccount},
        {"policy_hash", request.policyHash},
        {"recipient", request.recipient},
        {"amount", request.amount},
        {"nonce", request.nonce},
        {"marker_seed", request.markerSeed},
        {"expires_at_ms", expiresAtMs},
    }.dump();
}

ApprovalDecision OwnerChannel::requestApproval(const ApprovalRequest &request)
{
    ApprovalDecision d;
    d.requestId = request.id;

    std::string why;
    if (request.id.empty()) {
        why = "the request has no correlation id";
    } else if (request.policyHash.empty()) {
        why = "the request names no anchored policy";
    } else if (request.recipient.empty()) {
        why = "the request names no recipient account";
    } else if (canonicalAmount(request.amount).empty()) {
        why = "'amount' must be a u128 written as decimal digits";
    } else if (request.markerSeed.empty()) {
        why = "the request carries no approval marker seed";
    }
    if (!why.empty()) {
        d.verdict = ApprovalVerdict::NotAsked;
        d.detail = why;
        return d;
    }

    // An id names one payment. Reusing it for different terms would let the
    // answer to the first settle the second, which is the whole failure mode
    // correlation exists to prevent.
    const std::string print = fingerprint(request);
    const auto seen = asked_.find(request.id);
    if (seen != asked_.end() && seen->second != print) {
        d.verdict = ApprovalVerdict::NotAsked;
        d.detail = "request id '" + request.id + "' was already used for different terms";
        return d;
    }

    if (!open_) {
        d.verdict = ApprovalVerdict::ChannelUnavailable;
        d.detail = "the owner channel is not open";
        return d;
    }
    asked_[request.id] = print;

    const std::int64_t start = port_.nowMs();
    const std::string body = encodeRequest(request, config_.agentAccount, start + config_.timeoutMs);
    const std::vector<std::uint8_t> payload(body.begin(), body.end());

    // Force the first send on the first pass.
    std::int64_t lastSend = start - config_.resendIntervalMs;
    int sendFailures = 0;
    /// Why the first altered answer was altered. The first, not the last: it is
    /// the one closest to whatever went wrong.
    std::string alteredWhy;

    for (int polls = 0; polls < kMaxPolls; ++polls) {
        const std::int64_t now = port_.nowMs();

        if (now - lastSend >= config_.resendIntervalMs) {
            lastSend = now;
            // The same id and the same terms every time: this is one question
            // asked again, not a second question. An owner who answers the
            // third copy is answering the first.
            const bool sent = port_.ready() && port_.channelSend(channelId_, payload);
            if (sent) {
                ++d.attempts;
            } else {
                ++sendFailures;
                if (d.attempts == 0) {
                    // Nothing ever left. Reporting a timeout here would blame an
                    // owner who was never asked.
                    d.verdict = ApprovalVerdict::ChannelUnavailable;
                    d.detail = "the approval request could not be sent: the node is "
                               "not started, or delivery refused the channel send";
                    return d;
                }
            }
        }

        for (const auto &frame : port_.drain(channelId_)) {
            // `open()` refuses without an owner sender id, so this is never the
            // "accept everybody" branch it used to be.
            if (frame.senderId != config_.ownerSenderId) {
                ++d.ignored;
                continue;
            }
            const std::string text(frame.payload.begin(), frame.payload.end());
            const ReplyCheck c = checkReply(text, request);
            if (c.kind == ReplyKind::NotForThisRequest) {
                ++d.ignored;
                continue;
            }
            if (c.kind == ReplyKind::Altered) {
                // Counted, and the wait continues.
                //
                // This used to return immediately, which made one injected frame
                // a permanent, repeatable denial of every approval the owner
                // wanted to give — the agent would sit at `Refused` while the
                // real answer was still on its way. Ending the exchange never
                // stood between an attacker and an approval either: anyone able
                // to forge a frame naming *other* terms can forge one naming
                // these, so the early return only ever cost the owner their say.
                // What stands between an attacker and a payment is the approval
                // account on chain, which no message here can create.
                //
                // Failing closed is preserved at the other end: if nothing valid
                // arrives, the timeout below reports `Refused` rather than
                // `Unreachable`, so a caller still gets a terminal verdict and
                // an operator still learns that somebody was answering.
                ++d.altered;
                if (alteredWhy.empty()) alteredWhy = c.why;
                continue;
            }
            if (c.approve) {
                d.verdict = ApprovalVerdict::Approved;
                d.detail = "the owner approved these exact terms";
                // Only ever the request's own seed. The reply had to name the
                // same one to get here, and echoing the *request's* copy means a
                // reply can never redirect the caller to another approval
                // account even if a check above is one day loosened.
                d.markerSeed = request.markerSeed;
            } else {
                d.verdict = ApprovalVerdict::Denied;
                d.detail = c.why.empty() ? "the owner denied this spend"
                                         : "the owner denied this spend: " + c.why;
            }
            if (d.altered > 0) {
                // Said out loud even on an approval. The exchange completed, and
                // it completed on a channel somebody else was also writing to.
                d.detail += " (after " + std::to_string(d.altered) +
                            " answer(s) claiming this request and naming other terms: " +
                            alteredWhy + ")";
            }
            return d;
        }

        if (now - start >= config_.timeoutMs) {
            d.verdict = d.altered > 0 ? ApprovalVerdict::Refused : ApprovalVerdict::Unreachable;
            d.detail = "no valid answer within " + std::to_string(config_.timeoutMs) +
                       "ms, after " + std::to_string(d.attempts) + " attempt(s)";
            if (d.altered > 0) {
                d.detail += "; " + std::to_string(d.altered) +
                            " answer(s) claimed this request and named other terms: " + alteredWhy;
            }
            if (sendFailures > 0) {
                d.detail += " (" + std::to_string(sendFailures) + " send(s) refused)";
            }
            if (d.ignored > 0) {
                d.detail += " and " + std::to_string(d.ignored) + " frame(s) that were not answers";
            }
            return d;
        }
        if (port_.idle) port_.idle();
    }

    d.verdict = ApprovalVerdict::Unreachable;
    d.detail = "gave up waiting: the clock did not advance";
    return d;
}

} // namespace logos::agent
