// The owner's END of the approval channel, with no Logos node running.
//
// `owner_channel_test.cpp` exercises the agent's half against a fake port. This
// is the other half — the three `owner.*` skills a second Logos app calls — and
// then the two halves against EACH OTHER, which is the part neither file could
// do alone and the part a live run cannot decide:
//
//   * a live run cannot show the authorship rule working, because the transport
//     does not hand a node its own reliable-channel frames back. Measured, in
//     the two-module run: the owner's `frames` counter reads 1, which is the
//     agent's request and nothing else, and `self_refused` stays 0 for want of
//     anything to refuse. So the rule is exercised here, where a self-authored
//     frame can be put in front of it — and the falsification is the same
//     bytes with the other account as the author, which must be accepted.
//
//   * a live run cannot show that the two documents agree for the right reason.
//     Here the reply this owner writes is fed to the agent's own `checkReply`
//     through `OwnerChannel::requestApproval`, and a reply altered in flight is
//     refused. Two writers of one wire format is the defect this repository has
//     already paid for once, when the responder sent `{"approve":true}` where
//     the agent reads `decision` and the run looked exactly like an owner who
//     never answered.
//
// Build:
//   INC=/opt/homebrew/opt/nlohmann-json/include
//   clang++ -std=c++17 -Wall -Wextra -I"$INC" -o /tmp/oskt \
//       module/tests/owner_skills_test.cpp module/src/owner_skills.cpp \
//       module/src/owner_channel.cpp module/src/messaging_skills.cpp \
//       module/src/spend_marker.cpp && /tmp/oskt
#include "../src/owner_skills.h"

#include "../src/messaging_skills.h"
#include "../src/spend_marker.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

namespace {

int failures = 0;

void check(bool cond, const std::string &what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++failures;
}

json parsed(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

bool ok(const std::string &r) { return parsed(r).value("ok", false); }

std::string err(const std::string &r) { return parsed(r).value("error", std::string{}); }

const char *kOwner = "BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu";
const char *kAgent = "5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ";
const char *kOther = "A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu";
const char *kPolicy = "8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90";
const char *kPayee = "Public/Dxh7ZLHFmhKdNVE69XWayqLrquMk9iLfFVpmiJdfpEwD";

/// One request document, exactly as `OwnerChannel::encodeRequest` writes it.
/// Written through the real encoder rather than by hand: a test that composes
/// its own version of the wire format proves that the reader agrees with the
/// test, which is not the question.
std::string requestFrom(const std::string &agent, const std::string &amount,
                        std::uint64_t nonce, const std::string &seedOverride = {})
{
    ApprovalRequest r;
    r.id = "spend-" + std::to_string(nonce);
    r.policyHash = kPolicy;
    r.recipient = kPayee;
    r.amount = amount;
    r.nonce = nonce;
    r.markerSeed = seedOverride.empty() ? approvalMarkerSeed(agent, r.recipient, r.amount, r.nonce)
                                        : seedOverride;
    return OwnerChannel::encodeRequest(r, agent, 0);
}

/// A reliable channel that hands over whatever the test queues, and keeps what
/// was sent on it.
struct FakeChannel {
    bool nodeUp = true;
    bool createOk = true;
    bool sendOk = true;
    std::string createdChannel, createdTopic, createdSender;
    std::vector<std::string> sent;
    std::deque<InboundMessage> inbound;
    std::int64_t clock = 0;

    void arrive(const std::string &text, const std::string &sender)
    {
        InboundMessage m;
        m.senderId = sender;
        m.payload.assign(text.begin(), text.end());
        inbound.push_back(std::move(m));
    }

    OwnerChannelPort port()
    {
        OwnerChannelPort p;
        p.ready = [this] { return nodeUp; };
        p.channelCreate = [this](const std::string &c, const std::string &t,
                                 const std::string &s) {
            createdChannel = c;
            createdTopic = t;
            createdSender = s;
            return createOk;
        };
        p.channelSend = [this](const std::string &, const std::vector<std::uint8_t> &b) {
            if (!sendOk) return false;
            sent.emplace_back(b.begin(), b.end());
            return true;
        };
        p.drain = [this](const std::string &) {
            std::vector<InboundMessage> out(inbound.begin(), inbound.end());
            inbound.clear();
            return out;
        };
        // A clock that only ever moves forward, one step per reading. A frozen
        // one is not a neutral choice here: `OwnerChannel::requestApproval`
        // ends its wait on the clock, so a constant `nowMs` turns the case
        // where nothing valid arrives — the falsification below — into a hang
        // rather than a refusal.
        p.nowMs = [this] {
            const std::int64_t n = clock;
            clock += 100;
            return n;
        };
        return p;
    }
};

OwnerResponderPort responderPort(FakeChannel &ch, const char *owner = kOwner,
                                 const char *agent = kAgent)
{
    OwnerResponderPort p;
    p.channel = ch.port();
    p.ownerAccount = [owner] { return std::string(owner); };
    p.agentAccount = [agent] { return std::string(agent); };
    return p;
}

// ---------------------------------------------------------------------------

void refusesBeforeItCan(void)
{
    std::printf("\nwhat the owner's app refuses before it can do anything\n");

    FakeChannel down;
    down.nodeUp = false;
    OwnerResponder noNode(responderPort(down));
    check(err(noNode.watch("{}")) == "delivery node is not started",
          "owner.watch refuses without a node, in the words every wire skill uses");

    FakeChannel ch;
    OwnerResponder unwatched(responderPort(ch));
    check(err(unwatched.pending("{}")).find("owner.watch first") != std::string::npos,
          "owner.pending refuses to report on a channel nobody opened");
    check(err(unwatched.answer(R"({"id":"x","decision":"approve"})")).find("owner.watch first") !=
              std::string::npos,
          "and owner.answer refuses to send on one");

    FakeChannel noAccounts;
    OwnerResponderPort p = responderPort(noAccounts);
    p.ownerAccount = [] { return std::string{}; };
    OwnerResponder anonymous(std::move(p));
    check(err(anonymous.watch("{}")).find("no owner account") != std::string::npos,
          "an app with no owner account cannot answer as one");

    FakeChannel refuses;
    refuses.createOk = false;
    OwnerResponder unopened(responderPort(refuses));
    check(err(unopened.watch("{}")).find("would not open") != std::string::npos,
          "and a channel the node will not open is reported, not remembered");

    OwnerResponder r(responderPort(ch));
    check(ok(r.watch("{}")), "with a node and two accounts, the channel opens");
    check(ch.createdChannel ==
              std::string("/lp-0008/1/owner-channel/") + kOwner + "/" + kAgent,
          "under the id both ends derive from the two accounts, exchanging nothing");
    check(ch.createdTopic == ownerTopic(kOwner),
          "on the owner's own content topic");
    check(ch.createdSender == kOwner,
          "declaring the owner as its sender, which is what the agent filters on");
    check(parsed(r.watch("{}")).value("already", false),
          "a second watch of the same pair is idempotent rather than a second channel");
}

void theAuthorshipRule(void)
{
    std::printf("\nthe control: a node that hears itself must not answer itself\n");

    // THE PAIR THAT DISCRIMINATES. The same bytes arrive twice, differing only
    // in who published them. One is this app's own frame, which it must refuse
    // before it reads the type; the other is the agent's, which it must accept.
    // An app checking nothing passes half of this, and an app that ignores
    // every frame passes the other half.
    const std::string document = requestFrom(kAgent, "250", 7);

    FakeChannel mine;
    OwnerResponder self(responderPort(mine));
    check(ok(self.watch("{}")), "watching");
    mine.arrive(document, kOwner);
    json r = parsed(self.pending("{}"));
    check(r.value("pending", json::array()).empty(),
          "a request this app published ITSELF is not something it can answer");
    check(r.value("self_refused", 0) == 1,
          "and it was counted refused, not merely absent: 'nothing was applied' is also true "
          "of a channel nothing arrived on");
    check(r.value("frames", 0) == 1, "the frame did arrive — this is a refusal, not a silence");

    FakeChannel theirs;
    OwnerResponder listening(responderPort(theirs));
    check(ok(listening.watch("{}")), "watching");
    theirs.arrive(document, kAgent);
    r = parsed(listening.pending("{}"));
    check(r.value("pending", json::array()).size() == 1,
          "the SAME BYTES from the agent are a question this owner must answer");
    check(r.value("self_refused", 0) == 0, "and nothing was refused on authorship this time");
}

void whatItWillNotAnswer(void)
{
    std::printf("\nwhat arrives and is still not answerable\n");

    FakeChannel ch;
    OwnerResponder r(responderPort(ch));
    check(ok(r.watch("{}")), "watching");

    // An owner listens to one agent. Every agent of one owner shares the
    // content topic, so another agent's request really does arrive here.
    ch.arrive(requestFrom(kOther, "250", 11), kOther);
    json p = parsed(r.pending("{}"));
    check(p.value("pending", json::array()).empty() && p.value("ignored", 0) == 1,
          "a request from an agent this owner is not watching is ignored");

    // THE CHECK THAT MAKES THIS AN OWNER RATHER THAN A RUBBER STAMP: the seed
    // is re-derived from the request's own terms, and a request naming a seed
    // these terms do not produce is not answerable at all.
    ch.arrive(requestFrom(kAgent, "250", 12,
                          "0000000000000000000000000000000000000000000000000000000000000000"),
              kAgent);
    p = parsed(r.pending("{}"));
    check(p.value("pending", json::array()).empty(),
          "a request whose marker seed these terms do not derive is not offered for approval");
    check(p.value("unverifiable", json::array()).size() == 1,
          "it is reported as unverifiable rather than dropped");
    check(!ok(r.answer(R"({"id":"spend-12","decision":"approve"})")),
          "and it cannot be answered even by naming its id");

    ch.arrive("not json at all", kAgent);
    ch.arrive(R"({"protocol":"/lp-0008/1/owner-channel","type":"something_else"})", kAgent);
    p = parsed(r.pending("{}"));
    check(p.value("ignored", 0) == 3,
          "an unreadable frame and a frame of another type are ignored, and counted");

    // One id names one payment. A second frame under a known id with other
    // terms must not overwrite the first, or the answer to the first settles
    // the second.
    ch.arrive(requestFrom(kAgent, "250", 13), kAgent);
    (void)r.pending("{}");
    ch.arrive(requestFrom(kAgent, "999", 13, approvalMarkerSeed(kAgent, kPayee, "999", 13)), kAgent);
    p = parsed(r.pending("{}"));
    bool stillTwoFifty = false;
    for (const auto &one : p.value("pending", json::array())) {
        if (one.value("id", std::string{}) == "spend-13") {
            stillTwoFifty = one.value("amount", std::string{}) == "250";
        }
    }
    check(stillTwoFifty, "a second request under a known id does not rewrite its terms");
    check(p.value("unverifiable", json::array()).size() == 1,
          "and the attempt is reported");
}

void theTwoHalvesAgainstEachOther(void)
{
    std::printf("\nthe agent asks, this owner answers, and the agent's own checker reads it\n");

    // Two fake channels wired to each other: what the agent sends arrives at the
    // owner as the agent, and what the owner sends arrives at the agent as the
    // owner. The owner's polling runs on the agent's `idle`, so the whole
    // exchange is deterministic and no clock is involved.
    FakeChannel agentSide;
    FakeChannel ownerSide;
    OwnerResponder owner(responderPort(ownerSide));
    check(ok(owner.watch("{}")), "the owner's app is watching");

    OwnerChannelConfig cfg;
    cfg.contentTopic = ownerTopic(kOwner);
    cfg.ownerAccount = kOwner;
    cfg.agentAccount = kAgent;
    cfg.ownerSenderId = kOwner;
    cfg.timeoutMs = 5000;
    cfg.resendIntervalMs = 100000;

    std::string decision = "approve";
    bool alterTheReply = false;
    OwnerChannelPort agentPort = agentSide.port();
    // Whatever the agent puts on the wire reaches the owner's app, and whatever
    // the owner answers comes back — through the two real implementations, with
    // no document composed by this file.
    agentPort.channelSend = [&](const std::string &, const std::vector<std::uint8_t> &b) {
        ownerSide.arrive(std::string(b.begin(), b.end()), kAgent);
        return true;
    };
    agentPort.idle = [&] {
        const json p = parsed(owner.pending("{}"));
        for (const auto &one : p.value("pending", json::array())) {
            const std::string id = one.value("id", std::string{});
            const std::string sent = owner.answer(
                json{{"id", id}, {"decision", decision}}.dump());
            if (!parsed(sent).value("ok", false)) continue;
            std::string text = ownerSide.sent.back();
            if (alterTheReply) {
                // One field, in flight. The reply is otherwise perfect.
                json j = parsed(text);
                j["amount"] = "1";
                text = j.dump();
            }
            agentSide.arrive(text, kOwner);
        }
    };

    OwnerChannel channel(cfg, agentPort);
    check(ok(channel.open()), "the agent's end of the channel opened");

    ApprovalRequest req;
    req.id = "spend-100";
    req.policyHash = kPolicy;
    req.recipient = kPayee;
    req.amount = "250";
    req.nonce = 100;
    req.markerSeed = approvalMarkerSeed(kAgent, req.recipient, req.amount, req.nonce);

    ApprovalDecision d = channel.requestApproval(req);
    check(d.verdict == ApprovalVerdict::Approved,
          "the agent read this owner's reply as an approval of these exact terms");
    check(d.markerSeed == req.markerSeed,
          "and got back the marker seed it will present to `spend_approved`");
    check(d.altered == 0, "with nothing on the channel naming other terms");

    // The denial, which is a completed exchange and not a failure of it.
    decision = "deny";
    req.id = "spend-101";
    req.nonce = 101;
    req.markerSeed = approvalMarkerSeed(kAgent, req.recipient, req.amount, req.nonce);
    d = channel.requestApproval(req);
    check(d.verdict == ApprovalVerdict::Denied, "and a denial as a denial");
    check(!d.ownerUnreachable(), "the owner was reached; nothing here is a timeout");

    // THE FALSIFICATION. The owner answers correctly and one field is changed
    // in flight. The agent must not act on it — and must say which field.
    decision = "approve";
    alterTheReply = true;
    req.id = "spend-102";
    req.nonce = 102;
    req.markerSeed = approvalMarkerSeed(kAgent, req.recipient, req.amount, req.nonce);
    d = channel.requestApproval(req);
    check(d.verdict != ApprovalVerdict::Approved,
          "an approval altered in flight is not an approval");
    check(d.altered >= 1, "and it is counted as a frame naming other terms");
    check(d.detail.find("amount") != std::string::npos,
          "naming the field that changed: " + d.detail);
}

} // namespace

int main()
{
    refusesBeforeItCan();
    theAuthorshipRule();
    whatItWillNotAnswer();
    theTwoHalvesAgainstEachOther();

    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED"
                         : "the owner's end refuses what it must and answers what the agent's "
                           "own checker accepts",
                failures);
    return failures ? 1 : 0;
}
