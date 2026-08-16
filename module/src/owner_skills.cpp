// SPDX-License-Identifier: MIT OR Apache-2.0

#include "owner_skills.h"

#include "messaging_skills.h"
#include "spend_marker.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <vector>

using nlohmann::json;

namespace logos::agent {
namespace {

/// The same envelope every other skill answers in, so a caller can branch on
/// `ok` without knowing which half of the module produced the string.
std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
}

/// A string field, without letting nlohmann throw out of a frame handler.
/// `value(key, default)` is not enough: it throws when the key is present with
/// the wrong type, and a hostile frame is exactly where that happens.
std::string str(const json &j, const char *key)
{
    if (!j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

/// Parameters, which are allowed to be absent entirely: `owner.pending` takes
/// none, and a `ui` plugin calling it on a timer should not have to send `{}`.
bool params(const std::string &text, json &out, std::string &error)
{
    if (text.empty()) {
        out = json::object();
        return true;
    }
    out = json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        error = "the parameters are not valid JSON";
        return false;
    }
    if (out.is_null()) {
        out = json::object();
        return true;
    }
    if (!out.is_object()) {
        error = "the parameters must be a JSON object";
        return false;
    }
    return true;
}

std::string call(const std::function<std::string()> &fn)
{
    return fn ? fn() : std::string{};
}

} // namespace

OwnerResponder::OwnerResponder(OwnerResponderPort port) : port_(std::move(port)) {}

std::string OwnerResponder::watch(const std::string &paramsJson)
{
    json p;
    std::string err;
    if (!params(paramsJson, p, err)) return fail(err);

    const std::string owner = !str(p, "owner").empty() ? str(p, "owner") : call(port_.ownerAccount);
    const std::string agent = !str(p, "agent").empty() ? str(p, "agent") : call(port_.agentAccount);
    if (owner.empty()) {
        return fail("no owner account: set it with meta.configure('owner_channel_account', …) "
                    "or pass 'owner' — this app answers AS that account, and the channel is "
                    "named after it");
    }
    if (agent.empty()) {
        return fail("no agent account: set it with meta.configure('agent_account', …) or pass "
                    "'agent' — an owner listens to one agent's channel, not to a topic");
    }

    // The same refusal every skill on the wire gives, in the same words, and
    // for the same reason: `state()` tells "off" from "absent", and a channel
    // opened into a node that is not up is a channel whose frames go nowhere.
    if (!port_.channel.ready || !port_.channel.ready()) {
        return fail("delivery node is not started");
    }
    if (!port_.channel.channelCreate || !port_.channel.drain || !port_.channel.channelSend) {
        return fail("this build has no reliable-channel transport to open");
    }

    // Derived on both ends from the two accounts, exactly as `OwnerChannel`
    // derives it. Nothing is passed between the two apps: they agree because
    // they compute the same string, which is what makes "no intermediary
    // server" true of the rendezvous as well as of the messages.
    const std::string channelId = std::string(kOwnerChannelProtocol) + "/" + owner + "/" + agent;
    const std::string topic = ownerTopic(owner);
    if (topic.empty()) {
        return fail("the owner account does not produce a content topic");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const bool rewatch = open_ && channelId != channelId_;
    if (open_ && !rewatch) {
        return done(json{{"channel", channelId_},
                         {"topic", topic_},
                         {"owner", owner_},
                         {"agent", agent_},
                         {"already", true}});
    }
    // The sender id is the OWNER's account, because that is what the agent's
    // `OwnerChannelConfig::ownerSenderId` filters on: a reply from any other
    // sender is ignored by the far end. It is a hint and not a proof — sender
    // ids are self-declared — and the agent's own header says so.
    if (!port_.channel.channelCreate(channelId, topic, owner)) {
        return fail("the reliable channel would not open on this node");
    }
    if (rewatch) {
        // A request is only answerable on the channel it arrived on, so nothing
        // held for the old pair survives. Leaving it would let a `owner.answer`
        // send a reply about agent A down agent B's channel.
        held_.clear();
        seen_ = 0;
        ignored_ = 0;
        selfRefused_ = 0;
    }
    open_ = true;
    owner_ = owner;
    agent_ = agent;
    channelId_ = channelId;
    topic_ = topic;
    return done(json{{"channel", channelId_},
                     {"topic", topic_},
                     {"owner", owner_},
                     {"agent", agent_},
                     {"rewatched", rewatch}});
}

std::string OwnerResponder::pending(const std::string &paramsJson)
{
    json p;
    std::string err;
    if (!params(paramsJson, p, err)) return fail(err);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        return fail("this app is not watching an owner channel: call owner.watch first");
    }

    json unverifiable = json::array();
    int arrived = 0;
    for (const InboundMessage &frame : port_.channel.drain(channelId_)) {
        ++seen_;
        // AUTHORSHIP FIRST, and before the type is even looked at. A Waku node
        // receives what it publishes, so every reply this app sends comes back
        // to it; and an app that read its own frames as requests could answer
        // one it had written itself. Counted rather than dropped silently: a
        // harness has to be able to assert that the control ran.
        if (frame.senderId == owner_) {
            ++selfRefused_;
            continue;
        }
        const std::string text(frame.payload.begin(), frame.payload.end());
        const json request = json::parse(text, nullptr, false);
        if (request.is_discarded() || !request.is_object() ||
            str(request, "protocol") != kOwnerChannelProtocol ||
            str(request, "type") != "spend_approval_request") {
            ++ignored_;
            continue;
        }
        // An owner listens to one agent. A request naming another one is not
        // this owner's business even when it arrives on this topic — every
        // agent belonging to one owner shares the content topic, which
        // `OwnerChannelConfig::contentTopic` already documents as a known leak.
        const std::string asking = str(request, "agent");
        if (asking != agent_) {
            ++ignored_;
            continue;
        }

        ApprovalRequest r;
        r.id = str(request, "id");
        r.policyHash = str(request, "policy_hash");
        r.recipient = str(request, "recipient");
        r.amount = str(request, "amount");
        r.markerSeed = str(request, "marker_seed");
        if (!request.contains("nonce") || !request["nonce"].is_number_unsigned()) {
            ++ignored_;
            continue;
        }
        r.nonce = request["nonce"].get<std::uint64_t>();
        if (r.id.empty() || r.policyHash.empty() || r.recipient.empty() || r.amount.empty()) {
            ++ignored_;
            continue;
        }
        ++arrived;

        // THE CHECK THAT MAKES THIS AN OWNER RATHER THAN A RUBBER STAMP.
        //
        // The seed names the on-chain account `approve_spend` will create, and
        // it is derived here from the request's own agent, recipient, amount
        // and nonce — by the module's own copy of the derivation, which
        // `module/tests/spend_marker_test.cpp` pins to `crates/agent-policy-core`
        // by RUNNING the crate. An owner that echoed back whatever seed arrived
        // would leave the agent checking its own arithmetic; two independent
        // derivations agreeing is a fact about the payment.
        const std::string derived =
            approvalMarkerSeed(asking, r.recipient, r.amount, r.nonce);
        if (derived.empty() || derived != r.markerSeed) {
            unverifiable.push_back(json{{"id", r.id},
                                        {"agent", asking},
                                        {"said", r.markerSeed},
                                        {"derived", derived},
                                        {"reason", "the marker seed the agent named is not the "
                                                   "one these terms derive: refusing to answer "
                                                   "terms this app cannot verify"}});
            continue;
        }

        const auto existing = held_.find(r.id);
        if (existing != held_.end()) {
            // The agent re-sends the byte-identical request while it waits, so
            // a repeat is normal and is not a second payment. A repeat naming
            // OTHER terms is: an id is a name for one set of terms, and letting
            // the second overwrite the first would let an answer to the first
            // settle the second.
            const ApprovalRequest &was = existing->second.request;
            if (was.policyHash != r.policyHash || was.recipient != r.recipient ||
                was.amount != r.amount || was.nonce != r.nonce ||
                was.markerSeed != r.markerSeed) {
                unverifiable.push_back(
                    json{{"id", r.id},
                         {"agent", asking},
                         {"reason", "a request with this id already arrived naming different "
                                    "terms; an id names one payment"}});
            }
            continue;
        }
        HeldRequest h;
        h.request = r;
        h.derivedSeed = derived;
        h.agent = asking;
        held_.emplace(r.id, std::move(h));
    }

    json waiting = json::array();
    json answered = json::array();
    for (const auto &entry : held_) {
        const HeldRequest &h = entry.second;
        json one{{"id", h.request.id},
                 {"agent", h.agent},
                 {"policy_hash", h.request.policyHash},
                 {"recipient", h.request.recipient},
                 {"amount", h.request.amount},
                 {"nonce", h.request.nonce},
                 {"marker_seed", h.request.markerSeed},
                 // Always true for anything in this list — a request whose seed
                 // did not derive never reaches it — and stated anyway, because
                 // a display that shows the terms without saying they were
                 // checked is a display that invites approving on trust.
                 {"seed_verified", true}};
        if (h.answered) {
            one["decision"] = h.decision;
            answered.push_back(std::move(one));
        } else {
            waiting.push_back(std::move(one));
        }
    }

    return done(json{{"channel", channelId_},
                     {"topic", topic_},
                     {"owner", owner_},
                     {"agent", agent_},
                     {"pending", waiting},
                     {"answered", answered},
                     {"arrived", arrived},
                     {"frames", seen_},
                     {"self_refused", selfRefused_},
                     {"ignored", ignored_},
                     {"unverifiable", unverifiable}});
}

std::string OwnerResponder::answer(const std::string &paramsJson)
{
    json p;
    std::string err;
    if (!params(paramsJson, p, err)) return fail(err);

    const std::string id = str(p, "id");
    const std::string decision = str(p, "decision");
    if (id.empty()) {
        return fail("a request id is required: an answer that names no request could settle "
                    "any spend");
    }
    // Two words, and no third — the same rule `approveSpend` follows. "yes",
    // "ok" and an empty string are refused rather than guessed at, because the
    // guess that costs money is the one that reads an unfamiliar answer as
    // consent.
    if (decision != "approve" && decision != "deny") {
        return fail("the decision must be 'approve' or 'deny'");
    }
    const std::string reason = str(p, "reason");

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        return fail("this app is not watching an owner channel: call owner.watch first");
    }
    const auto it = held_.find(id);
    if (it == held_.end()) {
        // Including the request that has not been READ yet: `owner.pending` is
        // what moves a frame off the channel into this map, so answering an id
        // nobody has seen would be answering from memory of a screen rather
        // than from a message.
        return fail("no request with id '" + id +
                    "' has arrived on this channel: there is nothing here to answer");
    }
    if (it->second.answered) {
        return fail("'" + id + "' was already answered with '" + it->second.decision +
                    "': one payment, one answer");
    }

    const ApprovalRequest &r = it->second.request;
    // The nine fields `checkReply` reads, in the shape it reads them: `decision`
    // spelled out, the nonce as an unsigned number, and every term echoed so
    // the agent can refuse an answer that names anything else. This document is
    // byte-compatible with `module/tests/owner_responder.cpp`'s, deliberately —
    // one wire format with two writers is a format; two are a bug waiting for a
    // live run to find it, and this repository has already had that run.
    json reply{
        {"protocol", kOwnerChannelProtocol},
        {"type", "spend_approval_reply"},
        {"id", r.id},
        {"decision", decision},
        {"policy_hash", r.policyHash},
        {"recipient", r.recipient},
        {"amount", r.amount},
        {"nonce", r.nonce},
        {"marker_seed", r.markerSeed},
    };
    if (decision == "deny" && !reason.empty()) {
        reply["reason"] = reason;
    }
    const std::string text = reply.dump();
    const std::vector<std::uint8_t> payload(text.begin(), text.end());
    if (!port_.channel.channelSend || !port_.channel.channelSend(channelId_, payload)) {
        // Not marked answered: a send the node refused is not an answer, and
        // leaving it unanswered lets the owner press the button again when the
        // node comes back rather than being told the spend was already settled.
        return fail("the reply was not accepted by this node's channel");
    }
    it->second.answered = true;
    it->second.decision = decision;
    return done(json{{"id", r.id},
                     {"decision", decision},
                     {"recipient", r.recipient},
                     {"amount", r.amount},
                     {"nonce", r.nonce},
                     {"marker_seed", r.markerSeed},
                     {"channel", channelId_},
                     {"topic", topic_}});
}

// ---------------------------------------------------------------------------
// owner.watch / owner.pending / owner.answer
// ---------------------------------------------------------------------------

std::string OwnerWatchSkill::parameterSchema() const
{
    // No parentheses anywhere in the descriptions, and that is not a style
    // choice: these are raw string literals, so a `)` followed by a `"` ends
    // one early and the rest of the schema becomes code that does not compile —
    // or, worse, still does.
    return R"({"type":"object","properties":{)"
           R"("owner":{"type":"string","description":"the account this app answers as; )"
           R"(defaults to the configured owner_channel_account"},)"
           R"("agent":{"type":"string","description":"the agent whose approval requests are )"
           R"(read; defaults to the configured agent_account"}}})";
}

std::string OwnerWatchSkill::invoke(const std::string &paramsJson)
{
    return responder_ ? responder_->watch(paramsJson)
                      : fail("this module has no owner channel to watch");
}

std::string OwnerPendingSkill::parameterSchema() const
{
    return R"({"type":"object","properties":{}})";
}

std::string OwnerPendingSkill::invoke(const std::string &paramsJson)
{
    return responder_ ? responder_->pending(paramsJson)
                      : fail("this module has no owner channel to read");
}

std::string OwnerAnswerSkill::parameterSchema() const
{
    return R"({"type":"object","required":["id","decision"],"properties":{)"
           R"("id":{"type":"string"},)"
           R"("decision":{"type":"string","enum":["approve","deny"]},)"
           R"("reason":{"type":"string","description":"carried to the agent on a denial"}}})";
}

std::string OwnerAnswerSkill::invoke(const std::string &paramsJson)
{
    return responder_ ? responder_->answer(paramsJson)
                      : fail("this module has no owner channel to answer on");
}

} // namespace logos::agent
