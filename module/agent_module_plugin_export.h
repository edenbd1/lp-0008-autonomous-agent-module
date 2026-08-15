// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include <logos_module_context.h>
#include <logos_result.h>

#include "agent_module_plugin.h"

/**
 * @brief The agent module's Logos Core surface — the half that crosses the
 *        plugin boundary.
 *
 * A Logos Core module is not the impl class. It is a Qt plugin exporting
 * `PluginInterface` + `LogosProviderPlugin`, wrapping a provider object that
 * dispatches `callMethod(name, args)` onto the impl and publishes `getMethods()`
 * so a caller — Basecamp's module manager, another module, or the owner console
 * over the owner channel — can discover what it may call. That wrapper is
 * generated, not written: `logos-cpp-generator --from-header` reads this header
 * and emits `generated_code/agent_qt_glue.h`, `agent_dispatch.cpp` and
 * `agent_events.cpp`. `docs/basecamp.md` has the exact command, and the output
 * is committed so a build needs no code generator.
 *
 * Why this class exists at all, rather than generating straight off
 * `AgentModuleImpl`:
 *
 * - `registerSkill` takes a `std::shared_ptr<logos::agent::ISkill>`. There is no
 *   honest `QVariant` for that, and the generator's fallback — pass the QVariant
 *   through unchanged — does not compile. Skill registration is an in-process C++
 *   API for skills linked into the module; it is deliberately not remoteable, and
 *   nothing is lost by that: `invoke` is what a remote caller needs, and
 *   `skills()` is how it finds out what to invoke.
 *
 *   The same is true of `AgentModuleImpl::registerBuiltinSkills`, and it has a
 *   consequence worth stating here rather than leaving to be discovered: a host
 *   that loads this module as a *plugin* — Basecamp — cannot wire the skills'
 *   ports, because a port is a `std::function` and there is no wire format for
 *   one. So `start()` registers the module's own skills itself when nobody has,
 *   with nothing wired, and each of them refuses naming the port it is missing.
 *   That is why a loaded module answers `skills()` with a full card and
 *   `invoke()` with a skill's own refusal, instead of `[]` and "no skill named"
 *   — the state this module shipped in until the registration existed, and one
 *   that is indistinguishable from an agent that works and does nothing.
 *   Wiring real transports means linking the module and calling
 *   `registerBuiltinSkills` before `start`.
 * - The generator's parser reads one-line prototypes. `AgentModuleImpl::configure`
 *   is wrapped across two lines and is silently skipped — the very method that
 *   binds the agent to its owner. Restating the surface here keeps that failure
 *   from being invisible.
 *
 * So this is the RPC contract, and `AgentModuleImpl` stays the implementation.
 * Every method below delegates; none adds behaviour.
 */
class AgentModuleExport : public LogosModuleContext
{
public:
    /// Bind the agent to its owner and its anchored policy. Once, and only once.
    StdLogosResult configure(const std::string &ownerAddress, const std::string &policyHashHex);

    /// Start the agent. Emits `moduleStarted` with the outcome.
    StdLogosResult start();

    /// Stop the agent. Emits `moduleStopped` with the outcome.
    StdLogosResult stop();

    /// Every registered skill and its parameter schema, as JSON.
    std::string skills();

    /// What this agent is bound to, and whether it is running, as JSON.
    std::string status();

    /// Call a registered skill by name with a JSON parameter object.
    std::string invoke(const std::string &name, const std::string &paramsJson);

    /// Answer an approval request the agent published — `"approved"` or
    /// `"denied"`, naming the request by the `id` the event carried.
    ///
    /// This is the other half of @ref ownerApprovalRequested, and the reason
    /// both are here: a module loaded as a plugin cannot be handed a
    /// `std::function`, so the only owner channel it can have is one built out
    /// of the runtime's own surface — an event out, a method back. Refused for
    /// a request nothing is waiting on.
    StdLogosResult approveSpend(const std::string &requestId, const std::string &verdict);

logos_events:
    /// Emitted after every @ref start, successful or not. The host learns the
    /// outcome from the event rather than only from the return value, which is
    /// what the delivery module does with `nodeStarted` and what a caller
    /// waiting on a lifecycle needs.
    void moduleStarted(bool success, const std::string &message, int64_t timestamp);

    /// Emitted after every @ref stop.
    void moduleStopped(bool success, const std::string &message, int64_t timestamp);

    /// Emitted every time the agent asks its owner to approve an above-threshold
    /// spend — including every retry, which is why `attempt` is on it.
    ///
    /// `requestJson` is the whole request: `id`, `recipient`, `amount`, `nonce`,
    /// the anchored limits and the reason the spend fell outside them. It is
    /// re-emitted byte-identically while the agent waits, so an owner app that
    /// missed the first one has more than one chance to see it, and an owner app
    /// that saw all of them still has exactly one payment to answer for.
    ///
    /// Emitting is not delivering. This says the request left the module on the
    /// runtime's event channel; whether anyone was listening is not knowable
    /// from here, which is why the wait ends in "the owner did not answer"
    /// rather than in any claim about what the owner saw.
    ///
    /// On one line, like every declaration in this class: the generator's parser
    /// reads one-line prototypes and skips a wrapped one **without a warning**.
    /// This event was wrapped in its first draft, and the generated `agent.lidl`
    /// came back with the method and no event at all — the module would have
    /// shipped an owner channel that could be answered and never asked.
    void ownerApprovalRequested(const std::string &requestJson, int64_t attempt, int64_t timestamp);

protected:
    /// The host has handed over the module context. Two things happen here and
    /// both need a host to exist:
    ///
    /// 1. The context is forwarded to the impl. The framework sets it on *this*
    ///    class, not on `AgentModuleImpl`, so without this line the impl's
    ///    `instancePersistencePath()` is empty inside a running Basecamp and the
    ///    agent writes its pending tasks nowhere — while every unit test that
    ///    sets the context on the impl directly passes.
    /// 2. The impl is given its owner-notification path, which is this class's
    ///    event emitter. It reports false until the context is ready, because
    ///    `emitEventImpl_` is a silent no-op before the provider installs the
    ///    sink, and a notification that went nowhere must not be counted as
    ///    delivered.
    void onContextReady() override;

private:
    AgentModuleImpl impl_;
};

// ---------------------------------------------------------------------------
// Definitions.
//
// Header-only on purpose. The module's CMakeLists.txt names its sources
// explicitly, and the generated glue is picked up by LogosModule.cmake's
// `generated_code/` glob — so an export that needs no translation unit of its
// own is one that drops in without editing the build.
//
// The generator's parser reads only the class body (it stops at the closing
// brace), so nothing below is mistaken for part of the RPC surface.
// ---------------------------------------------------------------------------

namespace logos::agent::detail {

/// Nanoseconds since the epoch, in the shape the delivery module's events use.
inline std::int64_t nowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL
         + static_cast<std::int64_t>(ts.tv_nsec);
}

} // namespace logos::agent::detail

inline StdLogosResult AgentModuleExport::configure(const std::string &ownerAddress,
                                                  const std::string &policyHashHex)
{
    return impl_.configure(ownerAddress, policyHashHex);
}

inline StdLogosResult AgentModuleExport::start()
{
    StdLogosResult result = impl_.start();
    moduleStarted(result.success, result.error, logos::agent::detail::nowNs());
    return result;
}

inline StdLogosResult AgentModuleExport::stop()
{
    StdLogosResult result = impl_.stop();
    moduleStopped(result.success, result.error, logos::agent::detail::nowNs());
    return result;
}

inline std::string AgentModuleExport::skills()
{
    return impl_.skills();
}

inline std::string AgentModuleExport::status()
{
    return impl_.status();
}

inline std::string AgentModuleExport::invoke(const std::string &name,
                                             const std::string &paramsJson)
{
    return impl_.invoke(name, paramsJson);
}

inline StdLogosResult AgentModuleExport::approveSpend(const std::string &requestId,
                                                      const std::string &verdict)
{
    return impl_.approveSpend(requestId, verdict);
}

inline void AgentModuleExport::onContextReady()
{
    impl_._logosCoreSetContext_(modulePath(), instanceId(), instancePersistencePath());
    impl_.setOwnerNotifier([this](const std::string &requestJson, int attempt) {
        if (!isContextReady()) {
            return false;
        }
        ownerApprovalRequested(requestJson, static_cast<std::int64_t>(attempt),
                               logos::agent::detail::nowNs());
        return true;
    });
}
