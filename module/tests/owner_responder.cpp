// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The owner, answering an agent it has never met, over Logos Messaging.
//
//   owner_responder --owner <account> --agent <account> [--deny] [--seconds 240]
//
// WHAT THIS IS FOR, AND WHY `owner_channel_live.cpp` COULD NOT DO IT
//
// That harness puts two processes on two Delivery nodes and completes a real
// approval round trip, which is why the *class* is trusted. But both of its
// sides derive the payment's terms from one shared run id — the request id, the
// policy hash, the recipient, the amount, the nonce and the marker seed are all
// computed from a string passed on both command lines. That is a perfectly good
// way to test the channel and it is useless for testing an agent, because a
// real agent mints its own terms: `spend-<nonce>` from a clock, the policy hash
// it was configured with, a recipient and amount from the caller, and a marker
// seed derived from all of them.
//
// So this responder knows *nothing* in advance. It opens the channel, waits for
// whatever request arrives, reads the terms out of it, and answers those. That
// is what an owner's app does, and it is the only shape that can close the loop
// against a module nobody has told what to ask.
//
// THE ASSERTION THIS MAKES, AND THE ONE IT REFUSES TO MAKE
//
// It re-derives the marker seed from the request's own agent, recipient, amount
// and nonce, using `module/src/spend_marker.cpp` — the module's copy, which
// `spend_marker_test.cpp` pins to the crate the chain runs — and REFUSES to
// answer a request whose seed does not match. That is the whole value of the
// field: an owner who echoes back whatever seed arrived is a rubber stamp, and
// the agent's own check would then be checking its own arithmetic. Two
// independent derivations agreeing is a fact about the payment; one derivation
// copied twice is not.
//
// It does not claim anything about the chain. `verdict: approved` here unlocks
// the `spend_approved` path; it moves no money, and this process holds no key.

#include "../src/messaging_skills.h"
#include "../src/owner_channel.h"
#include "../src/spend_marker.h"

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

using nlohmann::json;

namespace {

int failures = 0;

void check(bool condition, const std::string &what)
{
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++failures;
    std::fflush(stdout);
}

void note(const std::string &what)
{
    std::printf("  ..    %s\n", what.c_str());
    std::fflush(stdout);
}

std::string arg(int argc, char **argv, const std::string &name, const std::string &fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) return argv[i + 1];
    }
    return fallback;
}

bool flag(int argc, char **argv, const std::string &name)
{
    for (int i = 1; i < argc; ++i) {
        if (name == argv[i]) return true;
    }
    return false;
}

/// One live Delivery node and the queue its event thread fills.
///
/// The event callback runs on a dedicated thread and is documented as having to
/// be fast, non-blocking and thread-safe, so it copies `len` bytes under a
/// mutex and does nothing else. Every buffer becomes a `std::string(msg, len)`
/// before anything looks at it: the library's buffers are not NUL-terminated
/// and their slots are reused, and `strstr` on one has produced a false pass in
/// this repository before.
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
        if (ret == NIMFFI_RET_STALE_WARN) return;
        if (ret != RET_OK && msg) self->lastError.assign(msg, len);
        self->started.store(ret == RET_OK ? 1 : 0);
    }
    static void onEvent(int ret, const char *ev, std::size_t len, void *ud)
    {
        if (ret != RET_OK || !ev || len == 0) return;
        auto *self = static_cast<Node *>(ud);
        std::lock_guard<std::mutex> lock(self->mu);
        self->events.emplace_back(ev, len);
    }

    std::vector<logos::agent::InboundMessage> drain(const std::string &channelId)
    {
        std::vector<std::string> raw;
        {
            std::lock_guard<std::mutex> lock(mu);
            raw.swap(events);
        }
        std::vector<logos::agent::InboundMessage> out;
        for (const std::string &e : raw) {
            logos::agent::InboundMessage m;
            if (logos::agent::parseInboundEvent(e, channelId, m)) out.push_back(std::move(m));
        }
        return out;
    }
};

struct Reply {
    std::atomic<int> state{-1};
    static void fn(int ret, const char *, const char *, void *ud)
    {
        static_cast<Reply *>(ud)->state.store(ret == RET_OK ? 1 : 0);
    }
    bool wait(int seconds)
    {
        for (int i = 0; i < seconds * 10 && state.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return state.load() == 1;
    }
};

std::string base64Encode(const std::string &bytes)
{
    static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
        const std::uint32_t b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const std::uint32_t v = (a << 16) | (b << 8) | c;
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? A[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? A[v & 63] : '=';
    }
    return out;
}

bool channelSend(Node &node, const std::string &channelId, const std::string &payload)
{
    const std::string message =
        json{{"payload", base64Encode(payload)}, {"ephemeral", false}}.dump();
    LogosdeliveryChannelSendReq req{};
    req.channelIdStr = channelId.c_str();
    req.messageJson = message.c_str();
    Reply r;
    if (logosdelivery_channel_send(node.ctx, &Reply::fn, &r, &req) != RET_OK) return false;
    return r.wait(30);
}

} // namespace

int main(int argc, char **argv)
{
    const std::string owner = arg(argc, argv, "--owner");
    const std::string agent = arg(argc, argv, "--agent");
    const bool deny = flag(argc, argv, "--deny");
    const int seconds = std::atoi(arg(argc, argv, "--seconds", "240").c_str());
    if (owner.empty() || agent.empty()) {
        std::fprintf(stderr, "usage: %s --owner <account> --agent <account> [--deny]\n", argv[0]);
        return 2;
    }

    std::printf("\nowner: bring up a Delivery node of its own\n");
    Node node;
    const char *config = "{\"mode\":\"Core\",\"preset\":\"logos.dev\","
                         "\"messagingOverrides\":{\"log-level\":\"WARN\"}}";
    LogosdeliveryCreateNodeCtorReq createReq{};
    createReq.configJson = config;
    node.ctx = logosdelivery_create_node(&createReq, &Node::onCreated, &node);
    check(node.ctx != nullptr, "create_node returned a context");
    if (!node.ctx) return 1;
    for (int i = 0; i < 600 && node.created.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(node.created.load() == 1, "the node was constructed");
    if (node.created.load() != 1) {
        note("error: " + node.lastError);
        return 1;
    }
    const std::uint64_t listener = logosdelivery_add_event_listener(
        node.ctx, logos::agent::kInboundListenerName, &Node::onEvent, &node);
    check(listener != 0, "a listener registered for the channel");
    logosdelivery_start_node(node.ctx, &Node::onScalar, &node);
    for (int i = 0; i < 1800 && node.started.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(node.started.load() == 1, "the node started and said so");
    if (node.started.load() != 1) {
        note("error: " + node.lastError);
        return 1;
    }

    // The channel id both sides derive from the two accounts, exactly as
    // `OwnerChannel` does. Nothing is passed between the processes.
    const std::string channelId =
        std::string(logos::agent::kOwnerChannelProtocol) + "/" + owner + "/" + agent;
    const std::string topic = logos::agent::ownerTopic(owner);
    note("topic   " + topic);
    note("channel " + channelId);

    LogosdeliveryChannelCreateReq openReq{};
    openReq.channelIdStr = channelId.c_str();
    openReq.contentTopicStr = topic.c_str();
    openReq.senderIdStr = owner.c_str();   // the sender id the agent filters on
    Reply opened;
    check(logosdelivery_channel_create(node.ctx, &Reply::fn, &opened, &openReq) == RET_OK &&
              opened.wait(30),
          "the owner opened the same reliable channel");

    std::printf("\nowner: wait for an agent to ask, and answer what it asked\n");
    note(deny ? "this run will DENY whatever arrives" : "this run will approve");

    bool answered = false;
    for (int elapsed = 0; elapsed < seconds * 5 && !answered; ++elapsed) {
        for (const auto &frame : node.drain(channelId)) {
            const std::string text(frame.payload.begin(), frame.payload.end());
            const json request = json::parse(text, nullptr, false);
            if (!request.is_object()) continue;
            if (request.value("type", std::string{}) != "spend_approval_request") continue;
            if (request.value("protocol", std::string{}) !=
                std::string(logos::agent::kOwnerChannelProtocol)) {
                continue;
            }
            // A node hears its own frames. The owner sends only replies and
            // accepts only requests, so it can never satisfy itself — the same
            // rule owner_channel_live.cpp is shaped around.
            if (frame.senderId == owner) continue;

            const std::string id = request.value("id", std::string{});
            const std::string askingAgent = request.value("agent", std::string{});
            const std::string recipient = request.value("recipient", std::string{});
            const std::string amount = request.value("amount", std::string{});
            const std::uint64_t nonce = request.value("nonce", std::uint64_t{0});
            const std::string seed = request.value("marker_seed", std::string{});
            const std::string policyHash = request.value("policy_hash", std::string{});

            std::printf("\n  <-    the agent asks: %s\n", text.c_str());
            check(!id.empty(), "the request carries a correlation id");
            check(askingAgent == agent,
                  "and it comes from the agent this owner is waiting for");

            // The check that makes this an owner rather than a rubber stamp.
            const std::string derived =
                logos::agent::approvalMarkerSeed(askingAgent, recipient, amount, nonce);
            check(!derived.empty(), "the owner can derive a marker seed for these terms");
            check(derived == seed,
                  "and it is the seed the agent named — two independent derivations of the "
                  "account `spend_approved` will look for, not one copied twice");
            if (derived != seed) {
                note("agent said  " + seed);
                note("owner makes " + derived);
                note("refusing to answer terms the owner cannot verify");
                continue;
            }

            // `decision`, spelled `approve` or `deny`. NOT a boolean field named
            // `approve`: `checkReply` reads `decision` and treats a reply it
            // cannot classify as *altered* — an answer claiming this request and
            // naming terms it will not act on — so a boolean here produces a
            // wait that ends in `Refused`, which `ownerUnreachable()` reports as
            // the owner being unreachable. The first version of this file sent
            // the boolean, and the resulting run looked exactly like an owner
            // who never answered: 23 attempts, no verdict, one frame received
            // and decoded. It was the frame counters in `meta.status` that told
            // the two apart.
            const json reply{
                {"protocol", logos::agent::kOwnerChannelProtocol},
                {"type", "spend_approval_reply"},
                {"id", id},
                {"decision", deny ? "deny" : "approve"},
                {"policy_hash", policyHash},
                {"recipient", recipient},
                {"amount", amount},
                {"nonce", nonce},
                {"marker_seed", seed},
            };
            check(channelSend(node, channelId, reply.dump()),
                  deny ? "the owner's denial went out on the channel"
                       : "the owner's approval went out on the channel");
            answered = true;
            break;
        }
        if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    check(answered, "an agent asked, within the time allowed");

    // Stay up briefly so the reply propagates before the node is torn down: a
    // channel send that is accepted locally and then loses its node has not
    // been delivered, and this process exiting is not the agent's answer
    // arriving.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (node.started.load() == 1) {
        node.started.store(-1);
        logosdelivery_stop_node(node.ctx, &Node::onScalar, &node);
        for (int i = 0; i < 300 && node.started.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    logosdelivery_destroy(node.ctx);

    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED" : "the owner answered the agent's own terms", failures);
    return failures ? 1 : 0;
}
