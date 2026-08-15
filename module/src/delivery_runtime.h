#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "messaging_skills.h"
#include "owner_channel.h"

/**
 * @brief A Logos Delivery node the module brings up and owns ITSELF.
 *
 * WHY THIS EXISTS, AND WHAT IT DISPROVES
 *
 * Everything else in this module reaches Delivery through `DeliveryPort` — a
 * struct of `std::function`s a *host* fills in. That is fine for a host which
 * links the module, and it is nothing at all for a host which loads it: Logos
 * Core runs a core module in its own `logos_host` process and reaches it over
 * Qt Remote Objects, and there is no wire format for a `std::function`. So a
 * loaded plugin was handed `SkillPorts{}` and refused `agent.card`,
 * `agent.discover` and `agent.task` — documented in `docs/basecamp.md` as "the
 * registered skills have no ports wired".
 *
 * The step that was never taken is the one this class is: *a host being unable
 * to PASS a port is not the module being unable to CONSTRUCT one*. The port is
 * a set of function objects; the module can build them out of a node it opened
 * from its own configuration, and then nothing has to cross the boundary
 * except the `meta.configure` call that says "go" — two strings, which Qt
 * Remote Objects has carried since the beginning.
 *
 * LIFECYCLE
 *
 * `logosdelivery_create_node` then `logosdelivery_start_node`, both
 * asynchronous: the call returns as soon as it is dispatched and the answer
 * arrives on a callback. Joining the public network takes tens of seconds, so
 * @ref bringUp does it on a thread of its own and returns immediately — a
 * `meta.configure` that blocked for a minute would time out on the runtime's
 * transport long before the node was up, and the caller would be told the
 * module was broken.
 *
 * That is also why @ref ready is a separate question with a separate answer:
 * every skill that touches the transport already refuses with `"delivery node
 * is not started"` when its port's `ready()` is false, and this class simply
 * makes that refusal true for a while and then false. Nothing had to change in
 * the skills.
 *
 * TWO TRAPS THIS FILE IS SHAPED BY, BOTH PAID FOR ALREADY
 *
 * 1. Payload buffers from the library are NOT NUL-terminated and their slots
 *    are reused between callbacks. Everything that arrives is copied into a
 *    `std::string(msg, len)` inside the callback, before anything looks at it.
 *    There is no `strstr`, `strcmp` or `%s` on a library buffer below.
 * 2. The name you register a listener under is not the name that comes back:
 *    `onMessageReceived` arrives with `"eventType":"message_received"`. Both
 *    constants are named, and the decoder matches the second.
 */
namespace logos::agent {

/// One relay message the node's event thread delivered, already decoded.
struct DeliveredMessage {
    std::string contentTopic;
    /// The Waku payload, base64-decoded. Cards and A2A requests are JSON text,
    /// so this is handed to the skills as a string.
    std::string payload;
};

/// Whether a `msg`/`len` event is a relay `message_received`, and what it says.
///
/// Free and testable on purpose: the event-name trap above is the kind of thing
/// that is only ever caught by feeding a decoder the bytes the library actually
/// emits, and a decoder buried inside a node object cannot be fed anything.
bool parseMessageEvent(const std::string &eventJson, DeliveredMessage &out);

/// Standard base64, which is what the library's message JSON carries.
std::string base64Encode(const std::string &bytes);

/**
 * @brief The node, its event thread's inbox, and the ports built out of both.
 *
 * Thread-safe. The library's event callback runs on a dedicated thread and is
 * documented as having to be fast, non-blocking and thread-safe, so it does one
 * thing — copy bytes under a mutex — and every decode happens on the caller's
 * thread inside @ref received / @ref drainChannel.
 */
class DeliveryRuntime {
public:
    /// Whether this build knows how to reach `liblogosdelivery` at all.
    ///
    /// The alternative to this function is a module that reports a delivery node
    /// "off" when the truth is that this binary was compiled without any
    /// Delivery support, which is the difference between "not started yet" and
    /// "never".
    ///
    /// It is deliberately NOT the question "is the library present": the
    /// library is opened with `dlopen` when a node is first asked for, not
    /// linked, so its absence is a `state` of `failed` carrying the path that
    /// was tried — see @ref bringUp. Linking it instead would make a module
    /// directory missing one file fail to load the *plugin*, which Basecamp
    /// reports to nobody.
    static bool linkedIn();

    DeliveryRuntime();
    ~DeliveryRuntime();

    DeliveryRuntime(const DeliveryRuntime &) = delete;
    DeliveryRuntime &operator=(const DeliveryRuntime &) = delete;

    /// What to do between polls while a call into the library is outstanding.
    ///
    /// **Not decoration, and the reason is measured elsewhere in this module.**
    /// `subscribe`, `publish`, `channelCreate` and `channelSend` are dispatched
    /// asynchronously by the library and answered on a callback, so each one
    /// waits — and every one of them is reached from `invoke()`, which in a
    /// loaded module runs on the `logos_host` event loop that also dispatches
    /// everything this module emits. A wait that *sleeps* on that thread queues
    /// behind itself every event the module is publishing and every call anyone
    /// makes to it. `AgentModuleImpl::setOwnerIdle` documents the same hazard
    /// measured on the owner-approval path, where six `ownerApprovalRequested`
    /// events and an unrelated `status()` all arrived in the same four
    /// milliseconds, thirty seconds late, at the instant the wait gave up.
    ///
    /// These waits are milliseconds rather than seconds — the calls are local to
    /// the node — so the symptom would be a stutter and not a stall. That is
    /// exactly why it is worth wiring rather than arguing about: a hazard whose
    /// symptom is small is one nobody finds.
    ///
    /// Left unset, the wait sleeps, which is right for every caller that has no
    /// event loop to pump.
    void setIdle(std::function<void()> idle);

    /// Start bringing the node up, in the background. Idempotent: a second call
    /// while one is in flight is accepted and does nothing.
    ///
    /// Returns false only when this build cannot do it at all.
    bool bringUp(std::string &error);

    /// Stop and destroy the node. Safe to call when nothing was ever started.
    void shutDown();

    /// `"absent"` (not linked), `"off"`, `"starting"`, `"ready"` or `"failed"`.
    std::string state() const;
    std::string lastError() const;
    bool ready() const;

    /// Subscribe to a content topic. Blocks until the library answers, which is
    /// a local operation; false means the node refused or is not up.
    bool subscribe(const std::string &topic);

    /// Publish `payload` on `topic`. Blocks until the send is accepted — not
    /// until it is propagated, which arrives later as an event.
    bool publish(const std::string &topic, const std::string &payload);

    /// The reliable-channel half, which the owner channel runs on.
    bool channelCreate(const std::string &channelId, const std::string &contentTopic,
                       const std::string &senderId);
    bool channelSend(const std::string &channelId, const std::vector<std::uint8_t> &payload);

    /// Everything received on `topic` since the node came up, oldest first, and
    /// nothing else. Subscribes on first use, so a caller cannot ask for a topic
    /// it forgot to join and be told, wrongly, that nobody is there.
    ///
    /// Deliberately NOT draining: `agent.discover` is a read, and two reads of
    /// one discovery topic returning different cards because the first consumed
    /// them is the sort of thing that only shows up in a demo.
    std::vector<std::string> received(const std::string &topic);

    /// Channel frames since the last call. Draining, unlike @ref received: the
    /// owner channel's wait loop is a consumer and re-reading an answer it has
    /// already acted on would let one approval release two spends.
    std::vector<InboundMessage> drainChannel(const std::string &channelId);

    /// The ports, built out of this node. What a host could not hand over.
    DeliveryPort deliveryPort();
    OwnerChannelPort ownerChannelPort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    /// @see setIdle. Set once, before the node is asked for.
    std::function<void()> idle_;
};

} // namespace logos::agent
