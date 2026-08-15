#include "agent_module_plugin.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

/// Serialise without ever throwing on the way out.
///
/// `dump()` throws `type_error.316` on a string that is not valid UTF-8, and the
/// strings in these documents include a third-party skill's name and its own
/// error text. Replacing the offending bytes keeps one skill's malformed name
/// from turning the whole card into an exception.
std::string dumpSafe(const json &j)
{
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

/// The answer shape the skills already use, so a caller — including another
/// agent over A2A — can branch on a refusal without knowing which layer refused.
std::string fail(const std::string &why)
{
    return dumpSafe(json{{"ok", false}, {"error", why}});
}

/// 64 hex characters, checked as hex.
///
/// Checking only the length accepted `std::string(64, 'z')`, which is exactly
/// the misconfiguration the check exists to catch: it is not an address, so it
/// resolves to an account nobody created, and the failure surfaces as a rejected
/// spend on chain long after startup.
bool isPolicyHashHex(const std::string &s)
{
    if (s.size() != 64) {
        return false;
    }
    for (const char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

} // namespace

AgentModuleImpl::AgentModuleImpl() = default;
AgentModuleImpl::~AgentModuleImpl() = default;

StdLogosResult AgentModuleImpl::configure(const std::string &ownerAddress,
                                       const std::string &policyHashHex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Exactly once, as the header promises. A second call used to return ok and
    // rebind a running agent's owner and policy anchor in place.
    if (configured_) {
        return StdLogosResult{false, {}, "configure() may only be called once"};
    }
    if (ownerAddress.empty()) {
        return StdLogosResult{false, {}, "owner address is required"};
    }
    // 64 hex characters: the policy account's 32-byte PDA seed. Rejecting a
    // malformed one here turns a silent misconfiguration into an error at
    // startup rather than a spend that fails on chain much later.
    if (!isPolicyHashHex(policyHashHex)) {
        return StdLogosResult{false, {}, "policy hash must be 32 bytes as 64 hex characters"};
    }
    ownerAddress_ = ownerAddress;
    policyHashHex_ = policyHashHex;
    configured_ = true;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return StdLogosResult{false, {}, "configure() must be called before start()"};
    }
    if (started_) {
        return StdLogosResult{false, {}, "already started"};
    }
    started_ = true;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::stop()
{
    // Deliberately tolerant of a stop that was never started: this runs on the
    // shutdown path, where refusing would turn a tidy exit into an error.
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::registerSkill(std::shared_ptr<logos::agent::ISkill> skill)
{
    if (!skill) {
        return StdLogosResult{false, {}, "null skill"};
    }

    // `name()` is third-party code. Call it before taking the lock, so a skill
    // that calls back into the module does not deadlock against a non-recursive
    // mutex, and catch it here rather than letting it escape into the host.
    std::string name;
    try {
        name = skill->name();
    } catch (const std::exception &e) {
        return StdLogosResult{false, {}, std::string("skill name() threw: ") + e.what()};
    } catch (...) {
        return StdLogosResult{false, {}, "skill name() threw"};
    }
    if (name.empty()) {
        return StdLogosResult{false, {}, "skill name is required"};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Refuse rather than overwrite: silently replacing a registered skill would
    // let one plugin shadow another's `wallet.send`.
    if (skills_.count(name) != 0) {
        return StdLogosResult{false, {}, "a skill named '" + name + "' is already registered"};
    }
    skills_.emplace(std::move(name), std::move(skill));
    return StdLogosResult{true, {}, {}};
}

std::string AgentModuleImpl::skills() const
{
    std::vector<std::pair<std::string, std::shared_ptr<logos::agent::ISkill>>> registered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return dumpSafe(json{{"error", "agent is not started"}});
        }
        registered.assign(skills_.begin(), skills_.end());
    }
    std::string out = "[";
    bool first = true;
    for (const auto &entry : registered) {
        if (!first) out += ",";
        first = false;
        std::string schema;
        try { schema = entry.second->parameterSchema(); } catch (...) { schema = "{}"; }
        out += "{\"name\":\"" + entry.first + "\",\"parameters\":" + schema + "}";
    }
    out += "]";
    return out;
}

std::string AgentModuleImpl::invoke(const std::string &name, const std::string &paramsJson)
{
    std::shared_ptr<logos::agent::ISkill> skill;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return fail("agent is not started");
        }
        const auto it = skills_.find(name);
        if (it == skills_.end()) {
            return fail("no skill named '" + name + "' is registered");
        }
        // Take a reference and drop the lock: the call happens unlocked, and the
        // shared_ptr keeps the skill alive even if it deregisters itself.
        skill = it->second;
    }

    std::string result;
    try {
        result = skill->invoke(paramsJson);
    } catch (const std::exception &e) {
        return fail("skill '" + name + "' threw: " + e.what());
    } catch (...) {
        return fail("skill '" + name + "' threw");
    }

    // Pass the skill's own answer through, but only once it is known to be JSON.
    // Prose returned from a skill would otherwise corrupt whatever document the
    // caller splices it into — the same defect as the schema, one layer up.
    if (json::parse(result, nullptr, false).is_discarded()) {
        return fail("skill '" + name + "' returned a result that is not JSON");
    }
    return result;
}

std::string AgentModuleImpl::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dumpSafe(json{{"configured", configured_},
                         {"started", started_},
                         {"owner", ownerAddress_},
                         {"policy", policyHashHex_}});
}
