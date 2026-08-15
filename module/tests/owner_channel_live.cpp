// The owner channel, between two SEPARATE processes, over two SEPARATE Logos
// Delivery nodes, on the public network.
//
//   ./scripts/owner-channel-live.sh
//
// WHAT THIS IS EVIDENCE FOR
//
// "The owner can interact with the agent in real time from a separate Logos app
// instance using Logos Messaging, with no intermediary server." Everything the
// module already had was tested against a *fake* port: a std::function that
// answered instantly and in-process, which cannot tell a correct channel from a
// correct-looking one. This runs the same `logos::agent::OwnerChannel` — not a
// copy of it, the class the module links — with its port wired to real
// `liblogosdelivery`, in two processes that share nothing but a content topic
// on the public Logos network. There is no broker, no relay of ours, no shared
// memory and no file between them: the only thing in the middle is Waku relay,
// which is the network, not a server either side operates.
//
// The exchange is the real one. The agent asks for approval of an
// above-threshold payment; the owner reads the terms and answers them. The
// agent's verdict is produced by `OwnerChannel::requestApproval`, which only
// returns `Approved` if a frame arrived naming *this* request's id, policy
// hash, recipient, amount, nonce and marker seed. So a pass is an assertion
// about the bytes that crossed the network, not about a send returning success.
//
// TWO TRAPS THIS FILE IS SHAPED BY
//
// 1. Payload buffers from the node library are not NUL-terminated and their
//    slots are reused between callbacks, so anything past `len` is the tail of
//    an older, longer event. `strstr` on one has produced a false pass in this
//    repository before. Every buffer here becomes a `std::string(msg, len)` in
//    the callback that receives it, before anything looks at it, and every
//    comparison afterwards is a length-bounded `std::string` compare. There is
//    no `strstr`, `strcmp` or `%s` on a library buffer anywhere below.
//
// 2. A node receives its own published messages. So a test that merely asserted
//    "a frame arrived on the channel" would pass with the other process dead.
//    Each side therefore only ever accepts a message *type it never sends*: the
//    agent accepts `spend_approval_reply` and sends only requests; the owner
//    accepts `spend_approval_request` and sends only replies. Neither can
//    satisfy itself, and `--alone` is a standing check that it stays that way.

#include "../src/owner_channel.h"
#include "../src/messaging_skills.h"

#include "liblogosdelivery.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using logos::agent::ApprovalRequest;
using logos::agent::ApprovalVerdict;
using logos::agent::InboundMessage;
using logos::agent::OwnerChannel;
using logos::agent::OwnerChannelConfig;
using logos::agent::OwnerChannelPort;
using nlohmann::json;

namespace {

int failures = 0;

void check(bool cond, const std::string &what)
{
    std::printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!cond) ++failures;
    std::fflush(stdout);
}

void note(const std::string &what)
{
    std::printf("  ..    %s\n", what.c_str());
    std::fflush(stdout);
}

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t envMs(const char *name, std::int64_t fallback)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    return std::strtoll(v, nullptr, 10);
}

/// One live Delivery node, and the inbound queue its event thread fills.
///
/// The event callback is documented as running on a dedicated thread and having
/// to be fast, non-blocking and thread-safe. It therefore does exactly one
/// thing: copy `len` bytes into a `std::string` under the mutex. All parsing —
/// which allocates and can throw — happens on the main thread in `drain`.
struct Node {
    void *ctx = nullptr;
    std::atomic<int> created{-1};
    std::atomic<int> started{-1};
    std::mutex mu;
    std::vector<std::string> events;
    std::string lastError;

    static void onCreated(int ret, const char *, const char *errMsg, void *ud)
    {
        auto *self = static_cast<Node *>(ud);
        if (ret != RET_OK && errMsg) self->lastError = errMsg;
        self->created.store(ret == RET_OK ? 1 : 0);
    }

    static void onScalar(int ret, char *msg, std::size_t len, void *ud)
    {
        auto *self = static_cast<Node *>(ud);
        if (ret == NIMFFI_RET_STALE_WARN) return;   // progress tick, not terminal
        if (ret != RET_OK && msg) self->lastError.assign(msg, len);
        self->started.store(ret == RET_OK ? 1 : 0);
    }

    static void onEvent(int ret, const char *ev, std::size_t len, void *ud)
    {
        if (ret != RET_OK || !ev || len == 0) return;
        auto *self = static_cast<Node *>(ud);
        std::lock_guard<std::mutex> lock(self->mu);
        self->events.emplace_back(ev, len);        // length-bounded, always
    }

    /// Every channel frame that has arrived since the last call, oldest first.
    std::vector<InboundMessage> drain(const std::string &channelId)
    {
        std::vector<std::string> raw;
        {
            std::lock_guard<std::mutex> lock(mu);
            raw.swap(events);
        }
        std::vector<InboundMessage> out;
        for (const std::string &e : raw) {
            InboundMessage m;
            // The module's own decoder: it is the thing that knows the event
            // name that actually comes back differs from the one you register.
            if (logos::agent::parseInboundEvent(e, channelId, m)) out.push_back(std::move(m));
        }
        return out;
    }
};

bool waitFlag(std::atomic<int> &flag, int seconds)
{
    for (int i = 0; i < seconds * 10; ++i) {
        const int v = flag.load();
        if (v == 1) return true;
        if (v == 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

/// Replies that carry a value are matched by the string handed to `userData`,
/// so a failed call is reported as the call it was rather than as "something".
struct Reply {
    std::atomic<int> state{-1};                    // -1 pending, 1 ok, 0 failed
    std::string value;
    std::string error;

    static void fn(int ret, const char *reply, const char *errMsg, void *ud)
    {
        auto *self = static_cast<Reply *>(ud);
        if (ret == RET_OK) {
            if (reply) self->value = reply;        // NUL-terminated: nimffi dups it
            self->state.store(1);
        } else {
            if (errMsg) self->error = errMsg;
            self->state.store(0);
        }
    }
};

/// Deterministic terms both processes derive from one shared run id, so nothing
/// has to be passed between them except that id — and so a frame left on the
/// public topic by an earlier run names a different payment and is ignored.
struct Terms {
    std::string ownerAccount, agentAccount, requestId, policyHash, recipient, amount, markerSeed;
    std::uint64_t nonce = 0;
};

Terms termsFor(const std::string &runId)
{
    Terms t;
    t.ownerAccount = "owner" + runId;
    t.agentAccount = "agent" + runId;
    t.requestId    = "req-" + runId;
    t.recipient    = "payee" + runId;
    t.amount       = "250";
    // 64 hex characters, which is the shape both `policy_hash` and
    // `marker_seed` have on chain. Built from the run id so two runs never
    // share them.
    std::string h;
    while (h.size() < 64) h += runId;
    h.resize(64);
    t.policyHash = h;
    std::string m = h;
    m[0] = 'a';                                    // distinct from the policy hash
    t.markerSeed = m;
    std::uint64_t n = 0;
    for (const char c : runId) n = n * 31 + static_cast<unsigned char>(c);
    t.nonce = n % 1000000;
    return t;
}

std::string channelIdFor(const Terms &t)
{
    return std::string(logos::agent::kOwnerChannelProtocol) + "/" + t.ownerAccount + "/" +
           t.agentAccount;
}

/// Bring a node up and, unless `skipStart`, wait for it to say it started.
///
/// `skipStart` exists so the failure this whole exercise depends on is
/// reachable: `OwnerChannel::open()` must refuse on a node that has not
/// reported started, and a refusal nobody has watched is not a guarantee.
bool bringUp(Node &node, bool skipStart)
{
    const char *config = "{\"mode\":\"Core\",\"preset\":\"logos.dev\","
                         "\"messagingOverrides\":{\"log-level\":\"WARN\"}}";
    LogosdeliveryCreateNodeCtorReq req{};
    req.configJson = config;
    node.ctx = logosdelivery_create_node(&req, &Node::onCreated, &node);
    check(node.ctx != nullptr, "create_node returned a context");
    if (!node.ctx) return false;
    check(waitFlag(node.created, 30), "the node was constructed");
    if (node.created.load() != 1) {
        note("error: " + (node.lastError.empty() ? std::string("unknown") : node.lastError));
        return false;
    }

    const std::uint64_t l =
        logosdelivery_add_event_listener(node.ctx, logos::agent::kInboundListenerName,
                                         &Node::onEvent, &node);
    check(l != 0, std::string("a listener registered for ") + logos::agent::kInboundListenerName);
    if (l == 0) return false;

    if (skipStart) {
        note("start_node deliberately NOT called");
        return true;
    }
    logosdelivery_start_node(node.ctx, &Node::onScalar, &node);
    check(waitFlag(node.started, 120), "the node started and said so");
    return node.started.load() == 1;
}

void tearDown(Node &node)
{
    if (!node.ctx) return;
    if (node.started.load() == 1) {
        node.started.store(-1);
        logosdelivery_stop_node(node.ctx, &Node::onScalar, &node);
        waitFlag(node.started, 30);
    }
    logosdelivery_destroy(node.ctx);
    node.ctx = nullptr;
}

/// The port the module is written against, wired to the real library.
///
/// Nothing here is a stand-in: `channelCreate` is `logosdelivery_channel_create`,
/// `channelSend` is `logosdelivery_channel_send`, and `drain` returns what the
/// node's own event thread delivered.
OwnerChannelPort livePort(Node &node)
{
    OwnerChannelPort port;
    port.ready = [&node] { return node.started.load() == 1; };
    port.nowMs = &nowMs;
    port.idle  = [] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); };
    port.drain = [&node](const std::string &channelId) { return node.drain(channelId); };
    port.channelCreate = [&node](const std::string &channelId, const std::string &contentTopic,
                                 const std::string &senderId) {
        LogosdeliveryChannelCreateReq req{};
        req.channelIdStr   = channelId.c_str();
        req.contentTopicStr = contentTopic.c_str();
        req.senderIdStr    = senderId.c_str();
        Reply r;
        if (logosdelivery_channel_create(node.ctx, &Reply::fn, &r, &req) != RET_OK) return false;
        for (int i = 0; i < 300 && r.state.load() == -1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return r.state.load() == 1;
    };
    port.channelSend = [&node](const std::string &channelId,
                               const std::vector<std::uint8_t> &payload) {
        // The library takes the channel payload base64-encoded (channel_api.nim
        // decodes it), which is the mirror of the encoding `parseInboundEvent`
        // undoes on the way in.
        static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        for (std::size_t i = 0; i < payload.size(); i += 3) {
            const std::uint32_t a = payload[i];
            const std::uint32_t b = i + 1 < payload.size() ? payload[i + 1] : 0;
            const std::uint32_t c = i + 2 < payload.size() ? payload[i + 2] : 0;
            const std::uint32_t v = (a << 16) | (b << 8) | c;
            b64 += A[(v >> 18) & 63];
            b64 += A[(v >> 12) & 63];
            b64 += i + 1 < payload.size() ? A[(v >> 6) & 63] : '=';
            b64 += i + 2 < payload.size() ? A[v & 63] : '=';
        }
        const std::string msg = json{{"payload", b64}, {"ephemeral", false}}.dump();
        LogosdeliveryChannelSendReq req{};
        req.channelIdStr = channelId.c_str();
        req.messageJson  = msg.c_str();
        Reply r;
        if (logosdelivery_channel_send(node.ctx, &Reply::fn, &r, &req) != RET_OK) return false;
        for (int i = 0; i < 300 && r.state.load() == -1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return r.state.load() == 1;
    };
    return port;
}

// ── the agent side ───────────────────────────────────────────────────────────

int runAgent(const Terms &t, bool skipStart)
{
    std::printf("\nagent: bring up a Delivery node of its own\n");
    Node node;
    if (!bringUp(node, skipStart)) {
        if (!skipStart) { tearDown(node); return 1; }
    }

    OwnerChannelConfig cfg;
    cfg.contentTopic     = logos::agent::ownerTopic(t.ownerAccount);
    cfg.ownerAccount     = t.ownerAccount;
    cfg.agentAccount     = t.agentAccount;
    cfg.ownerSenderId    = t.ownerAccount;
    cfg.timeoutMs        = envMs("LP0008_TIMEOUT_MS", 300000);
    cfg.resendIntervalMs = envMs("LP0008_RESEND_MS", 12000);
    check(!cfg.contentTopic.empty(), "the owner topic follows the content-topic grammar");
    note("topic   " + cfg.contentTopic);

    OwnerChannel channel(cfg, livePort(node));
    note("channel " + channel.channelId());

    std::printf("\nagent: open the channel on the running node\n");
    const std::string opened = channel.open();
    std::printf("  <-    open(): %s\n", opened.c_str());
    if (skipStart) {
        // The point of the whole run in this mode: a node that never started
        // must produce a refusal, not a channel that looks fine and delivers
        // nothing.
        const json o = json::parse(opened, nullptr, false);
        check(!o.is_discarded() && o.contains("ok") && o["ok"] == false,
              "open() refuses on a node that has not started");
        check(!o.is_discarded() && o.value("error", "") == "delivery node is not started",
              "and refuses with the reason, not a generic failure");
        tearDown(node);
        std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "refused as it must", failures);
        return failures ? 1 : 0;
    }
    check(json::parse(opened, nullptr, false).value("ok", false), "the channel opened");
    if (failures) { tearDown(node); return 1; }

    ApprovalRequest req;
    req.id         = t.requestId;
    req.policyHash = t.policyHash;
    req.recipient  = t.recipient;
    req.amount     = t.amount;
    req.nonce      = t.nonce;
    req.markerSeed = t.markerSeed;

    std::printf("\nagent: ask the owner to approve %s to %s, and wait for the answer\n",
                req.amount.c_str(), req.recipient.c_str());
    note("this is OwnerChannel::requestApproval, unmodified, over the public network");
    const auto started = nowMs();
    const auto d = channel.requestApproval(req);
    std::printf("  <-    decision: %s\n", d.json().c_str());
    std::printf("  ..    %lld ms, %d attempt(s) on the wire\n",
                static_cast<long long>(nowMs() - started), d.attempts);

    check(d.verdict == ApprovalVerdict::Approved,
          "the owner's answer arrived and approved these exact terms");
    check(d.requestId == req.id, "the decision is about the request that was asked");
    check(d.markerSeed == req.markerSeed,
          "and echoes the approval marker `spend_approved` will present on chain");
    check(d.attempts >= 1, "the request was actually put on the wire");
    check(!d.ownerUnreachable(), "the owner was reached");

    tearDown(node);
    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED" : "the owner answered over the public network", failures);
    return failures ? 1 : 0;
}

// ── the owner side: a separate Logos app instance ────────────────────────────

int runOwner(const Terms &t, bool alter, bool alone)
{
    std::printf("\nowner: bring up a Delivery node of its own\n");
    Node node;
    if (!bringUp(node, false)) { tearDown(node); return 1; }

    const std::string topic   = logos::agent::ownerTopic(t.ownerAccount);
    const std::string channel = channelIdFor(t);
    note("topic   " + topic);
    note("channel " + channel);

    OwnerChannelPort port = livePort(node);
    check(port.channelCreate(channel, topic, t.ownerAccount),
          "the owner joined the same channel from its own node");
    if (failures) { tearDown(node); return 1; }

    std::printf("\nowner: wait for the agent's request, and read its terms\n");
    if (alone) note("--alone: nothing will be answered; the agent must not settle itself");

    const std::int64_t deadline = nowMs() + envMs("LP0008_OWNER_WAIT_MS", 300000);
    // How many times the answer goes back on the wire. The owner never learns
    // that the agent got it — there is no acknowledgement in this protocol and
    // adding one would just move the question — so it answers a fixed number of
    // times and stops. Six over fifty seconds is far more than the one attempt
    // a healthy run needs, and the margin is deliberate: it is what keeps the
    // `--alter` case honest, since the agent has to see an altered frame in
    // order to refuse it. Stopping matters too — an owner that idles to its
    // deadline after doing its job turns a ninety-second exercise into a
    // five-minute one.
    const int kReplies = 6;
    bool sawRequest = false;
    bool done = false;
    std::string senderSeen;
    int replies = 0;
    std::int64_t nextReply = 0;

    while (nowMs() < deadline && !done) {
        for (const InboundMessage &m : port.drain(channel)) {
            // Length-bounded by construction: `payload` is a byte vector with a
            // size, and this is the only way its bytes ever become text.
            const std::string text(m.payload.begin(), m.payload.end());
            const json j = json::parse(text, nullptr, false);
            if (j.is_discarded() || !j.is_object()) continue;
            if (j.value("type", "") != "spend_approval_request") continue;  // never sent by us
            if (j.value("id", "") != t.requestId) continue;
            if (sawRequest) continue;

            sawRequest = true;
            senderSeen = m.senderId;
            std::printf("  <-    request: %s\n", text.c_str());
            check(j.value("protocol", "") == logos::agent::kOwnerChannelProtocol,
                  "the frame names the owner-channel protocol");
            check(j.value("agent", "") == t.agentAccount,
                  "it comes from the agent this owner runs");
            check(m.senderId == t.agentAccount,
                  "the channel sender id is the agent's, not this process's own");
            check(j.value("policy_hash", "") == t.policyHash,
                  "it names the anchored policy the agent is bound by");
            check(j.value("recipient", "") == t.recipient, "it names the recipient");
            check(j.value("amount", "") == t.amount, "it names the amount");
            check(j.contains("nonce") && j["nonce"].is_number_unsigned() &&
                      j["nonce"].get<std::uint64_t>() == t.nonce,
                  "it names the nonce that makes this payment distinct");
            check(j.value("marker_seed", "") == t.markerSeed,
                  "it names the approval marker seed");
            nextReply = 0;                         // answer immediately
        }

        if (sawRequest && !alone && nowMs() >= nextReply && replies < kReplies) {
            // The owner's answer. Every term is echoed, because the agent
            // refuses an approval that names anything else — which is what
            // `--alter` below demonstrates.
            json reply{
                {"protocol",    logos::agent::kOwnerChannelProtocol},
                {"type",        "spend_approval_reply"},
                {"id",          t.requestId},
                {"decision",    "approve"},
                {"policy_hash", t.policyHash},
                {"recipient",   t.recipient},
                {"amount",      alter ? "251" : t.amount},
                {"nonce",       t.nonce},
                {"marker_seed", t.markerSeed},
            };
            const std::string out = reply.dump();
            const std::vector<std::uint8_t> bytes(out.begin(), out.end());
            const bool sent = port.channelSend(channel, bytes);
            if (replies == 0) {
                std::printf("  ->    reply: %s\n", out.c_str());
                check(sent, "the owner's answer was accepted by its own node");
                if (alter) note("--alter: the amount above is NOT the amount asked for");
            }
            ++replies;
            // Resent on an interval for the same reason the agent resends: the
            // first frame on a fresh channel is routinely the one that does not
            // arrive. Measured, not assumed — see scripts/owner-channel-live.sh.
            nextReply = nowMs() + 10000;
            if (replies >= kReplies) done = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    check(sawRequest, "the agent's request arrived from the other process");
    if (!alone) check(replies > 0, "the owner answered it");
    if (sawRequest) note("sender id on the frame: " + senderSeen);

    tearDown(node);
    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED" : "the owner read the agent's terms", failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s agent|owner <run-id> [--alter] [--alone] [--no-start]\n",
                     argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const std::string role  = argv[1];
    const std::string runId = argv[2];
    bool alter = false, alone = false, noStart = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--alter")    alter   = true;
        else if (a == "--alone")    alone   = true;
        else if (a == "--no-start") noStart = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    // The run id is spliced into a content topic and into a channel id, so it
    // goes through the same grammar check every other identifier does.
    if (!logos::agent::isTopicIdentifier(runId)) {
        std::fprintf(stderr, "the run id may only carry letters, digits, '-' and '_'\n");
        return 2;
    }

    const Terms t = termsFor(runId);
    std::printf("role %s   run %s\n", role.c_str(), runId.c_str());
    if (role == "agent") return runAgent(t, noStart);
    if (role == "owner") return runOwner(t, alter, alone);
    std::fprintf(stderr, "unknown role: %s\n", role.c_str());
    return 2;
}
