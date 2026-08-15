// Exercise the owner channel through its port, with no Logos node running.
//
// A node is needed to prove that a message crosses the network. It is not
// needed — and is actively unhelpful — to prove the things that decide whether
// money moves: that a channel which will not open is reported rather than
// pretended, that an answer to another request cannot settle this one, that an
// approval naming a different amount is refused, and that an owner who never
// replies is a terminal outcome rather than a quiet fallback to acting alone.
// Those are exactly what a stub gets wrong, and none of them are visible in a
// screencast.
//
// Build:
//   INC=/opt/homebrew/opt/nlohmann-json/include
//   clang++ -std=c++17 -Wall -Wextra -I"$INC" \
//       module/tests/owner_channel_test.cpp module/src/owner_channel.cpp -o /tmp/ockt && /tmp/ockt
#include "../src/owner_channel.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static bool okOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return !j.is_discarded() && j.value("ok", false);
}

static std::string errOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? "" : j.value("error", std::string{});
}

static std::string b64(const std::string &in)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(in[i]);
        const unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0u;
        const unsigned t = (a << 16) | (b << 8) | c;
        out += tbl[(t >> 18) & 0x3F];
        out += tbl[(t >> 12) & 0x3F];
        out += i + 1 < in.size() ? tbl[(t >> 6) & 0x3F] : '=';
        out += i + 2 < in.size() ? tbl[t & 0x3F] : '=';
    }
    return out;
}

// A delivery module that does whatever the test needs, and records what it was
// asked to do.
struct Fake {
    bool nodeUp = true;
    bool createOk = true;
    bool sendOk = true;

    std::string createdChannel, createdTopic, createdSender;
    std::string sentOn, drainedFrom;
    std::vector<std::string> sent; // request documents, as text
    int drains = 0;

    // Frames handed over on the n-th drain.
    std::vector<std::pair<int, InboundMessage>> queued;

    // A clock that only ever moves forward, one step per reading.
    std::int64_t clock = 0;
    std::int64_t stepMs = 100;

    void reply(int onDrain, const std::string &text, const std::string &sender = "owner-sds")
    {
        InboundMessage m;
        m.senderId = sender;
        m.payload.assign(text.begin(), text.end());
        queued.emplace_back(onDrain, m);
    }

    OwnerChannelPort port()
    {
        OwnerChannelPort p;
        p.ready = [this] { return nodeUp; };
        p.channelCreate = [this](const std::string &c, const std::string &t, const std::string &s) {
            createdChannel = c;
            createdTopic = t;
            createdSender = s;
            return createOk;
        };
        p.channelSend = [this](const std::string &c, const std::vector<std::uint8_t> &b) {
            if (!sendOk) return false;
            sentOn = c;
            sent.emplace_back(b.begin(), b.end());
            return true;
        };
        p.drain = [this](const std::string &c) {
            drainedFrom = c;
            ++drains;
            std::vector<InboundMessage> out;
            for (const auto &q : queued) {
                if (q.first == drains) out.push_back(q.second);
            }
            return out;
        };
        p.nowMs = [this] {
            const std::int64_t n = clock;
            clock += stepMs;
            return n;
        };
        return p;
    }
};

static const char *kTopic = "/lp-0008/1/owner-owner-acct/json";

static ApprovalRequest request()
{
    ApprovalRequest r;
    r.id = "req-1";
    r.policyHash = "0d1c…policy";
    r.recipient = "bob-account";
    r.amount = "100";
    r.nonce = 7;
    r.markerSeed = "9f3a…marker";
    return r;
}

static json replyFor(const ApprovalRequest &r, const char *decision)
{
    return json{
        {"protocol", kOwnerChannelProtocol},
        {"type", "spend_approval_reply"},
        {"id", r.id},
        {"policy_hash", r.policyHash},
        {"recipient", r.recipient},
        {"amount", r.amount},
        {"nonce", r.nonce},
        {"marker_seed", r.markerSeed},
        {"decision", decision},
    };
}

// A fake and a channel wired to it, held together so neither outlives the other.
struct Rig {
    Fake fake;
    std::unique_ptr<OwnerChannel> channel;

    explicit Rig(const std::string &ownerSenderId = "")
    {
        OwnerChannelConfig cfg;
        cfg.contentTopic = kTopic;
        cfg.ownerAccount = "owner-acct";
        cfg.agentAccount = "agent-acct";
        cfg.timeoutMs = 1000;
        cfg.resendIntervalMs = 300;
        cfg.ownerSenderId = ownerSenderId;
        channel = std::make_unique<OwnerChannel>(cfg, fake.port());
    }
};

// One exchange: open, queue `reply` for the first poll, ask.
static ApprovalDecision answeredWith(const json &reply)
{
    Rig r;
    r.channel->open();
    r.fake.reply(1, reply.dump());
    return r.channel->requestApproval(request());
}

int main()
{
    std::printf("opening the channel\n");
    {
        Rig r;
        r.fake.nodeUp = false;
        const auto o = r.channel->open();
        check(!okOf(o), "a node that has not reported nodeStarted refuses to open a channel");
        check(errOf(o).find("not started") != std::string::npos, "and says why");
        check(!r.channel->isOpen(), "and the channel does not consider itself open");
    }
    {
        Rig r;
        r.fake.createOk = false;
        const auto o = r.channel->open();
        check(!okOf(o), "a channel that will not open is reported, not pretended");
        check(errOf(o).find("refused to open") != std::string::npos, "and blames the open, not the node");
        check(!r.channel->isOpen(), "and it stays closed");
    }
    {
        // A port that can send and cannot hear is the dangerous half-wiring: it
        // turns every approval into a plausible-looking timeout.
        Rig r;
        OwnerChannelPort deaf = r.fake.port();
        deaf.drain = nullptr;
        OwnerChannelConfig cfg;
        cfg.contentTopic = kTopic;
        cfg.ownerAccount = "owner-acct";
        cfg.agentAccount = "agent-acct";
        OwnerChannel ch(cfg, deaf);
        const auto o = ch.open();
        check(!okOf(o), "a port with no inbound path refuses to open");
        check(errOf(o).find("inbound") != std::string::npos, "and names the missing half");
    }
    {
        Rig r;
        const auto o = r.channel->open();
        check(okOf(o), "a healthy port opens the channel");
        check(r.fake.createdTopic == kTopic, "on the owner's content topic");
        check(r.fake.createdChannel == r.channel->channelId(), "with the derived channel id");
        check(r.fake.createdSender == "agent-acct", "and the agent as the SDS sender");
        check(r.channel->channelId().find("owner-acct") != std::string::npos &&
                  r.channel->channelId().find("agent-acct") != std::string::npos,
              "the channel id names both ends, so two agents of one owner do not share it");
    }
    {
        Rig r; // never opened
        const auto d = r.channel->requestApproval(request());
        check(d.verdict == ApprovalVerdict::ChannelUnavailable,
              "asking on an unopened channel is unavailable, not a timeout");
        check(d.ownerUnreachable(), "and reports the owner as unreachable");
        check(r.fake.sent.empty(), "and nothing was sent");
    }

    std::printf("\nthe request that goes on the wire\n");
    {
        Rig r;
        r.channel->open();
        const auto d = r.channel->requestApproval(request());
        check(d.verdict == ApprovalVerdict::Unreachable, "an owner who never answers times out");
        check(d.ownerUnreachable(), "which is the terminal owner-unreachable state: nothing submitted");
        check(!d.approved(), "and it is not an approval");
        check(d.attempts >= 2, "the request is retried before timing out, not sent once");
        check(r.fake.sentOn == r.channel->channelId(), "sent on the channel that was opened");
        check(r.fake.drainedFrom == r.channel->channelId(), "and listened to on the same one");

        bool sameId = !r.fake.sent.empty(), fullTerms = true;
        for (const auto &s : r.fake.sent) {
            const auto j = json::parse(s, nullptr, false);
            if (j.is_discarded() || j.value("id", std::string{}) != "req-1") sameId = false;
            if (j.value("protocol", std::string{}) != kOwnerChannelProtocol ||
                j.value("type", std::string{}) != "spend_approval_request" ||
                j.value("amount", std::string{}) != "100" ||
                j.value("recipient", std::string{}) != "bob-account" ||
                j.value("marker_seed", std::string{}) != "9f3a…marker" ||
                j.value("nonce", 0u) != 7u || !j.contains("expires_at_ms")) {
                fullTerms = false;
            }
        }
        check(sameId, "every retry carries the same correlation id: one question asked again");
        check(fullTerms, "and the full terms the owner is being asked to approve");
    }
    {
        Rig r;
        r.channel->open();
        r.fake.sendOk = false;
        const auto d = r.channel->requestApproval(request());
        check(d.verdict == ApprovalVerdict::ChannelUnavailable,
              "a request that never left is unavailable, not an owner who ignored it");
        check(d.attempts == 0, "and no attempt is claimed");
        check(d.ownerUnreachable(), "still terminal: nothing is submitted");
    }

    std::printf("\nthe owner answers\n");
    {
        const auto d = answeredWith(replyFor(request(), "approve"));
        check(d.verdict == ApprovalVerdict::Approved && d.approved(), "an approval is recognised");
        check(d.markerSeed == "9f3a…marker",
              "and carries the marker seed the spend_approved path needs");
        check(!d.ownerUnreachable(),
              "an approval is not a submitted payment, but the owner was certainly reached");
    }
    {
        auto reply = replyFor(request(), "deny");
        reply["reason"] = "too much for a Tuesday";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Denied && !d.approved(), "a denial is recognised");
        check(d.detail.find("too much for a Tuesday") != std::string::npos,
              "and carries the owner's reason back to the operator");
        check(!d.ownerUnreachable(), "a denial is a complete exchange, not an unreachable owner");
    }
    {
        // The same number, spelled with a leading zero. Refusing this would be a
        // false alarm — the check must catch a different amount, not a different
        // spelling of the same one.
        auto reply = replyFor(request(), "approve");
        reply["amount"] = "0100";
        const auto d = answeredWith(reply);
        check(d.approved(), "an amount written with a leading zero is the same amount");
    }

    std::printf("\nanswers that must not settle this payment\n");
    {
        Rig r;
        r.channel->open();
        auto stray = replyFor(request(), "approve");
        stray["id"] = "req-2";
        r.fake.reply(1, stray.dump());
        const auto d = r.channel->requestApproval(request());
        check(!d.approved(), "an approval carrying another request's id does not approve this one");
        check(d.verdict == ApprovalVerdict::Unreachable, "it is ignored, and the wait continues");
        check(d.ignored >= 1, "and it is counted, not silently dropped");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply["amount"] = "1000";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused, "an approval that alters the amount is refused");
        check(d.detail.find("amount") != std::string::npos, "and says which term was altered");
        check(d.ownerUnreachable(), "nothing is submitted on a refusal");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply["recipient"] = "mallory-account";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused, "an approval that alters the recipient is refused");
        check(d.detail.find("recipient") != std::string::npos, "and says so");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply["marker_seed"] = "0000…somebody-elses-approval";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused,
              "an approval pointing at another approval account is refused");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply["policy_hash"] = "beef…other-policy";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused,
              "an approval naming a different anchored policy is refused");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply["nonce"] = 8;
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused,
              "an approval for a different nonce is refused: approvals are single-use");
    }
    {
        // The point of the whole design: an approval names a payment. It cannot
        // hand the agent a bigger envelope, and a reply that tries is treated as
        // hostile rather than tidied up.
        auto reply = replyFor(request(), "approve");
        reply["per_tx"] = "1000000";
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused, "an approval carrying a new per-tx limit is refused");
        check(d.detail.find("limit") != std::string::npos, "and says a limit is not a message's to set");
    }
    {
        auto reply = replyFor(request(), "approve");
        reply.erase("amount");
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused,
              "an approval that does not restate the terms is refused, not assumed");
    }
    {
        auto reply = replyFor(request(), "sure why not");
        const auto d = answeredWith(reply);
        check(d.verdict == ApprovalVerdict::Refused,
              "an answer that is neither approve nor deny is not a yes");
    }
    {
        Rig r;
        r.channel->open();
        r.fake.reply(1, "{not json at all");
        r.fake.reply(2, json{{"hello", "world"}}.dump());
        const auto d = r.channel->requestApproval(request());
        check(d.verdict == ApprovalVerdict::Unreachable, "garbage on the channel is ignored, not thrown");
        check(d.ignored >= 2, "and counted");
    }
    {
        Rig r("owner-sds");
        r.channel->open();
        r.fake.reply(1, replyFor(request(), "approve").dump(), "somebody-else");
        const auto d = r.channel->requestApproval(request());
        check(!d.approved(), "a reply from another sender does not approve");
        check(d.ignored >= 1, "when the owner's sender id is pinned, other senders are ignored");
    }

    std::printf("\nrequests this channel refuses to make\n");
    {
        Rig r;
        r.channel->open();
        ApprovalRequest bad = request();
        bad.amount = "one hundred";
        const auto d = r.channel->requestApproval(bad);
        check(d.verdict == ApprovalVerdict::NotAsked, "an amount that is not digits is never asked about");
        bad = request();
        bad.markerSeed.clear();
        check(r.channel->requestApproval(bad).verdict == ApprovalVerdict::NotAsked,
              "nor is a request with no approval marker");
        bad = request();
        bad.recipient.clear();
        check(r.channel->requestApproval(bad).verdict == ApprovalVerdict::NotAsked,
              "nor one with no recipient account");
        bad = request();
        bad.id.clear();
        check(r.channel->requestApproval(bad).verdict == ApprovalVerdict::NotAsked,
              "nor one with no correlation id");
        check(r.fake.sent.empty(), "and none of them reached the wire");
    }
    {
        // An id is a name for one set of terms. Reusing it would let the answer
        // to the first payment settle the second.
        Rig r;
        r.channel->open();
        r.fake.reply(1, replyFor(request(), "deny").dump());
        check(r.channel->requestApproval(request()).verdict == ApprovalVerdict::Denied, "ask once");
        ApprovalRequest again = request();
        again.amount = "999";
        const auto d = r.channel->requestApproval(again);
        check(d.verdict == ApprovalVerdict::NotAsked,
              "the same id cannot be reused for different terms");
        check(d.detail.find("already used") != std::string::npos, "and says so");
    }

    std::printf("\ndecoding what the transport actually emits\n");
    {
        const std::string body = replyFor(request(), "approve").dump();
        const std::string chan = "/lp-0008/1/owner-channel/owner-acct/agent-acct";
        const std::string event = json{{"eventType", kInboundEventType},
                                       {"channelId", chan},
                                       {"senderId", "owner-sds"},
                                       {"payload", b64(body)}}
                                      .dump();
        InboundMessage m;
        check(parseInboundEvent(event, chan, m), "a real channel_message_received event decodes");
        check(std::string(m.payload.begin(), m.payload.end()) == body,
              "and its base64 payload comes back byte for byte");
        check(m.senderId == "owner-sds", "with the sender it arrived from");

        // The trap: the name you register with is not the name that comes back.
        // A decoder matching the registration name never fires, and a channel
        // that never hears looks exactly like an owner who never answered.
        const std::string wrongName = json{{"eventType", kInboundListenerName},
                                           {"channelId", chan},
                                           {"payload", b64(body)}}
                                          .dump();
        check(!parseInboundEvent(wrongName, chan, m),
              "the listener name is not an event type, and is not accepted as one");
        check(std::string(kInboundEventType) != std::string(kInboundListenerName),
              "the two names really are different strings");

        const std::string otherChannel = json{{"eventType", kInboundEventType},
                                              {"channelId", "/somebody/else"},
                                              {"payload", b64(body)}}
                                             .dump();
        check(!parseInboundEvent(otherChannel, chan, m), "another channel's traffic is not ours");
        // Three separate ways to be undecodable, because they fail at three
        // different points and a payload that decodes "as far as it can" hands
        // the reply checker a truncated document — which is not a "no".
        const auto payloadEvent = [&](const char *p) {
            return json{{"eventType", kInboundEventType}, {"channelId", chan}, {"payload", p}}.dump();
        };
        check(!parseInboundEvent(payloadEvent("abc"), chan, m),
              "a payload whose length no base64 document can have is refused");
        check(!parseInboundEvent(payloadEvent("ab!d"), chan, m),
              "so is one of the right length carrying a character base64 has no value for");
        check(!parseInboundEvent(payloadEvent("ab=d"), chan, m),
              "and padding in the middle is not padding");
        check(!parseInboundEvent("{", chan, m), "and a malformed event is refused, not thrown");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all owner-channel behaviours hold");
    return failures ? 1 : 0;
}
