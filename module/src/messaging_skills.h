#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_module_interface.h"

/**
 * @brief The Messaging skills, over Logos Delivery.
 *
 * Delivery is a lifecycle, not a function: `createNode` once per context,
 * `start` before any message operation, `stop` before shutdown, and `start`
 * returns as soon as the request is dispatched — completion arrives later as a
 * `nodeStarted` event. A skill that sends immediately after `start` is sending
 * into a node that may not be up, which is why `ready()` below exists and why
 * `send` refuses rather than silently dropping.
 *
 * The transport is addressed by content topic, not by peer. A recipient's topic
 * is derived from their LEZ account, so an agent that knows an Agent Card knows
 * where to reach its owner — this is also the A2A transport binding, since
 * Logos Messaging is what replaces A2A's HTTP.
 */
namespace logos::agent {

/// What this needs from the delivery module, named so the skills can be tested
/// against a fake and so the agent module does not link delivery directly.
struct DeliveryPort {
    std::function<bool()> ready;
    std::function<bool(const std::string &topic, const std::vector<std::uint8_t> &payload)> send;
    std::function<bool(const std::string &topic)> subscribe;
    std::function<bool(const std::string &channelId)> channelCreate;
};

/// Whether `id` may be spliced into the `<name>` segment of a content topic.
///
/// The grammar this file cites is `/<application>/<version>/<name>/<encoding>`,
/// and every topic below is built by concatenation — so the `<name>` segment is
/// whatever the caller hands over, and a caller is often a stranger. Two
/// measured consequences of not checking, both from identifiers a peer chooses:
/// a recipient of `AAA/json", "x":1, "y":"` publishes on a topic of the
/// attacker's choosing, and a member id of `../../owner-VICTIM/json` redirects a
/// group invitation onto somebody else's topic. Since `agent_address` is read
/// out of a peer's Agent Card, a stranger otherwise picks the topic a victim
/// broadcasts on.
///
/// Letters, digits, `-` and `_`, up to 128 of them. That is a superset of every
/// identifier this repository puts in a topic — base58 account ids, hex task
/// ids, group names — and a subset of what the grammar can carry safely. `.` and
/// `/` are not in it: `/` ends the segment, and `.` only buys `..`, which reads
/// as a path to a human and as nothing at all to the topic router.
///
/// Defined inline, in the header, on purpose. The two translation units that
/// splice identifiers into topics — this one and `agent_skills.cpp` — are linked
/// into *separate* test binaries, so an out-of-line definition would force one
/// of them to carry its own copy of the grammar, and a grammar with two
/// implementations is a grammar with two answers.
inline bool isTopicIdentifier(const std::string &id)
{
    if (id.empty() || id.size() > 128) return false;
    for (const char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool allowed = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                             (u >= '0' && u <= '9') || u == '-' || u == '_';
        if (!allowed) return false;
    }
    return true;
}

/// Content topic for one-to-one traffic with a Logos account.
///
/// Follows the content-topic grammar Logos documents rather than inventing a
/// scheme: /<application>/<version>/<name>/<encoding>.
///
/// **Empty when `account` is not a @ref isTopicIdentifier.** A caller that
/// forgets to check then publishes nowhere, rather than somewhere chosen by
/// whoever supplied the id.
std::string ownerTopic(const std::string &account);

/// Content topic an agent publishes its A2A Agent Card on. Empty on an
/// identifier the grammar cannot carry, for the same reason.
std::string discoveryTopic(const std::string &namespace_);

/// `messaging.send(recipient, message)`
class SendSkill final : public ISkill {
public:
    explicit SendSkill(DeliveryPort port) : port_(std::move(port)) {}
    std::string name() const override { return "messaging.send"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    DeliveryPort port_;
};

/// `messaging.join(group_id)`
class JoinSkill final : public ISkill {
public:
    explicit JoinSkill(DeliveryPort port) : port_(std::move(port)) {}
    std::string name() const override { return "messaging.join"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    DeliveryPort port_;
};

/// `messaging.create_group(members)`
class CreateGroupSkill final : public ISkill {
public:
    explicit CreateGroupSkill(DeliveryPort port) : port_(std::move(port)) {}
    std::string name() const override { return "messaging.create_group"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    DeliveryPort port_;
};

} // namespace logos::agent
