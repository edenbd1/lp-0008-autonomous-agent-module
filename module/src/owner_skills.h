#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "agent_module_interface.h"
#include "owner_channel.h"

/**
 * @brief The OWNER's half of the approval exchange, run from inside a loaded
 *        module — so the owner's end can be a second Logos app instance.
 *
 * WHY THIS FILE EXISTS
 *
 * `owner_channel.h` is the agent's half: it asks, waits, and checks the answer
 * against the exact terms it asked about. Something has to be on the other end
 * of that channel, and until this file the only thing that ever was is
 * `module/tests/owner_responder.cpp` — a program written for the purpose, which
 * links `liblogosdelivery` directly and is not a Logos app. That is what kept
 * the criterion open: "the owner can interact with the agent in real time from
 * a separate Logos app instance using Logos Messaging, with no intermediary
 * server", where the prize's own Architecture section glosses the phrase as
 * "any Logos app instance that holds the owner's keys".
 *
 * The transport half was never the blocker after `delivery_runtime.cpp`: a
 * module Logos Core loads can open a Delivery node and build its own ports.
 * What was missing is that nothing on the OWNER's side could be reached through
 * a module method table, so a second Basecamp had a window and nothing to say
 * with it. These three skills are that: `owner.watch` opens the same reliable
 * channel from the owner's end, `owner.pending` reports what has arrived on it,
 * and `owner.answer` puts the reply on the wire. They cross the plugin boundary
 * as `invoke()` calls like every other skill, so a `ui` plugin in a second
 * Basecamp — which can call `invoke` and nothing else — is enough.
 *
 * THE ONE THING THIS DELIBERATELY DOES NOT DO
 *
 * It does not re-implement the reply document. `owner_responder.cpp` and this
 * file put the same nine fields on the wire, and both are checked by the one
 * `checkReply` in `owner_channel.cpp` — an owner app whose reply is *nearly*
 * right is the failure this repository has already paid for once, when the
 * responder sent `{"approve":true}` where the agent reads `decision` and the
 * run looked exactly like an owner who never answered.
 *
 * WHAT IT IS NOT AUTHORITY FOR
 *
 * The same sentence `owner_channel.h` opens with, pointed the other way: an
 * `approve` from here does not let the agent spend a penny more than its
 * anchored ceiling, and it is not a signature. Sender ids on a channel are
 * self-declared, so this reply says "the owner's app answered", not "the owner
 * signed". The signature that matters is the one that creates the approval
 * account on chain under `approve_spend`, which is the owner's key and not a
 * message.
 */
namespace logos::agent {

/// What the owner half needs, and nothing more: the reliable channel the
/// agent's half already runs on, plus the two account ids that name it.
///
/// The channel functions are the same `OwnerChannelPort` the agent side is
/// handed, built by `DeliveryRuntime::ownerChannelPort()`. Reused rather than
/// redeclared so that a change to the transport cannot reach one end of the
/// channel and miss the other.
struct OwnerResponderPort {
    OwnerChannelPort channel;
    /// `meta.configure("owner_channel_account")` — who this app answers as.
    std::function<std::string()> ownerAccount;
    /// `meta.configure("agent_account")` — the agent whose requests are read.
    /// On the owner's instance this names the OTHER side, which is what makes
    /// the channel id the two ends derive identical.
    std::function<std::string()> agentAccount;
};

/// One request this owner has been asked about and has not yet answered.
struct HeldRequest {
    ApprovalRequest request;
    /// The seed this app derived for itself from the request's own terms. Equal
    /// to `request.markerSeed` or the request is not answerable — see
    /// @ref OwnerResponder::pending.
    std::string derivedSeed;
    /// The agent id the frame named, kept for the reply and for the display.
    std::string agent;
    bool answered = false;
    std::string decision;
};

/**
 * @brief The owner's end of one channel: open it, read what arrives, answer it.
 *
 * Thread-safe, because the three skills below are dispatched from the module's
 * `invoke()` and a `ui` plugin polling `owner.pending` on a timer is exactly
 * the caller that will overlap them.
 */
class OwnerResponder {
public:
    explicit OwnerResponder(OwnerResponderPort port);

    /// Open the reliable channel as the owner. Optional `{"owner":…,"agent":…}`
    /// override the configured accounts; both must be known one way or another.
    ///
    /// Re-watching a DIFFERENT pair is allowed and replaces the channel — the
    /// alternative is an app that has to be restarted to answer a second agent
    /// — and it drops everything held for the old pair, because a request is
    /// only answerable on the channel it arrived on.
    std::string watch(const std::string &paramsJson);

    /// Drain the channel and report what is waiting for an answer.
    ///
    /// Draining, not reading: `DeliveryRuntime::drainChannel` hands each frame
    /// over once, so this holds what it decodes rather than expecting to see it
    /// again on the next call.
    std::string pending(const std::string &paramsJson);

    /// Answer one held request. `{"id":…,"decision":"approve"|"deny"}`, and an
    /// optional `"reason"` which the agent carries into its own report on a
    /// denial.
    std::string answer(const std::string &paramsJson);

private:
    OwnerResponderPort port_;
    mutable std::mutex mutex_;
    bool open_ = false;
    std::string owner_;
    std::string agent_;
    std::string channelId_;
    std::string topic_;
    /// Correlation id -> what arrived under it.
    std::map<std::string, HeldRequest> held_;
    /// Frames the node handed back that this app published itself.
    ///
    /// **A node receives its own published messages**, so an owner app that did
    /// not check authorship could answer a request it had forged, and every
    /// assertion about "the agent asked" would pass with the agent switched
    /// off. The check is first, before the type is even read, and the count is
    /// reported so a harness can assert the control ran rather than assert that
    /// nothing happened — "no self-authored frame was applied" is also true of
    /// a channel nothing ever arrived on.
    int selfRefused_ = 0;
    int ignored_ = 0;
    int seen_ = 0;
};

/// `owner.watch()` — become the owner end of the approval channel.
class OwnerWatchSkill final : public ISkill {
public:
    explicit OwnerWatchSkill(std::shared_ptr<OwnerResponder> responder)
        : responder_(std::move(responder))
    {
    }
    std::string name() const override { return "owner.watch"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    std::shared_ptr<OwnerResponder> responder_;
};

/// `owner.pending()` — what the agent has asked this owner to approve.
class OwnerPendingSkill final : public ISkill {
public:
    explicit OwnerPendingSkill(std::shared_ptr<OwnerResponder> responder)
        : responder_(std::move(responder))
    {
    }
    std::string name() const override { return "owner.pending"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    std::shared_ptr<OwnerResponder> responder_;
};

/// `owner.answer()` — approve or deny one of them, over Logos Messaging.
class OwnerAnswerSkill final : public ISkill {
public:
    explicit OwnerAnswerSkill(std::shared_ptr<OwnerResponder> responder)
        : responder_(std::move(responder))
    {
    }
    std::string name() const override { return "owner.answer"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    std::shared_ptr<OwnerResponder> responder_;
};

} // namespace logos::agent
