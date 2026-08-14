#include "agent_module_plugin.h"

#include <sstream>

AgentModuleImpl::AgentModuleImpl() = default;
AgentModuleImpl::~AgentModuleImpl() = default;

StdLogosResult AgentModuleImpl::configure(const std::string &ownerAddress,
                                       const std::string &policyHashHex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ownerAddress.empty()) {
        return StdLogosResult{false, {}, "owner address is required"};
    }
    // 64 hex characters: the policy account's 32-byte PDA seed. Rejecting a
    // malformed one here turns a silent misconfiguration into an error at
    // startup rather than a spend that fails on chain much later.
    if (policyHashHex.size() != 64) {
        return StdLogosResult{false, {}, "policy hash must be 32 bytes as 64 hex characters"};
    }
    ownerAddress_ = ownerAddress;
    policyHashHex_ = policyHashHex;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (policyHashHex_.empty()) {
        return StdLogosResult{false, {}, "configure() must be called before start()"};
    }
    started_ = true;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    return StdLogosResult{true, {}, {}};
}

StdLogosResult AgentModuleImpl::registerSkill(std::shared_ptr<logos::agent::ISkill> skill)
{
    if (!skill) {
        return StdLogosResult{false, {}, "null skill"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = skill->name();
    if (name.empty()) {
        return StdLogosResult{false, {}, "skill name is required"};
    }
    // Refuse rather than overwrite: silently replacing a registered skill would
    // let one plugin shadow another's `wallet.send`.
    if (skills_.count(name) != 0) {
        return StdLogosResult{false, {}, "a skill named '" + name + "' is already registered"};
    }
    skills_.emplace(name, std::move(skill));
    return StdLogosResult{true, {}, {}};
}

std::string AgentModuleImpl::skills() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto &entry : skills_) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << R"({"name":")" << entry.first
            << R"(","parameters":)" << entry.second->parameterSchema() << "}";
    }
    out << "]";
    return out.str();
}
