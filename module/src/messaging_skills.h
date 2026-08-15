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

/// Content topic for one-to-one traffic with a Logos account.
///
/// Follows the content-topic grammar Logos documents rather than inventing a
/// scheme: /<application>/<version>/<name>/<encoding>.
std::string ownerTopic(const std::string &account);

/// Content topic an agent publishes its A2A Agent Card on.
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
