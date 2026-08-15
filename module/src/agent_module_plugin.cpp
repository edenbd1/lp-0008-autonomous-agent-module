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
    bool wireBuiltins = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return StdLogosResult{false, {}, "configure() must be called before start()"};
        }
        if (started_ || starting_) {
            return StdLogosResult{false, {}, "already started"};
        }
        starting_ = true;
        // Claim the built-ins here rather than after registering them: a caller
        // that wired ports has already claimed, and this is what keeps a start
        // from registering a second, unwired copy of every skill.
        wireBuiltins = !builtinsClaimed_;
        builtinsClaimed_ = true;
    }

    // Outside the lock, and before `started_`. Outside because registerSkill
    // takes the same non-recursive mutex; before because `skills()` refuses
    // until started, and a card assembled halfway through registration is one
    // nobody asked for. The result is deliberately not propagated: a name a
    // third party already holds costs that built-in, not the whole start.
    if (wireBuiltins) {
        installBuiltinSkills(logos::agent::SkillPorts{});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    starting_ = false;
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

StdLogosResult AgentModuleImpl::registerBuiltinSkills(logos::agent::SkillPorts ports)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Before start, because `skills()` and `invoke()` open for business
        // there: a skill that appears afterwards was missing from a card that
        // has already been published.
        if (started_ || starting_) {
            return StdLogosResult{false, {}, "the built-in skills must be wired before start()"};
        }
        // Once, for the same reason `registerSkill` refuses a duplicate name: a
        // second wiring cannot replace the first, so it would only ever be
        // twenty-one refusals reported as a failure to register.
        if (builtinsClaimed_) {
            return StdLogosResult{false, {}, "the built-in skills are already registered"};
        }
        builtinsClaimed_ = true;
    }
    return installBuiltinSkills(std::move(ports));
}

StdLogosResult AgentModuleImpl::installBuiltinSkills(logos::agent::SkillPorts ports)
{
    using namespace logos::agent;

    // ---- the ports the module can honestly answer itself ------------------
    //
    // Only facts about this module. Everything else — the chain, the storage
    // node, a signing key, a model — is about the world outside it, and
    // defaulting any of those to something plausible is how a skill comes to
    // report success for work nobody did.
    //
    // These lambdas hold `this` and are held by skills the module owns, so they
    // die with it. They take the module's own lock, which is safe because
    // `invoke()` releases it before calling a skill — the same guarantee that
    // lets a third-party skill call back in.
    if (!ports.status.configured) {
        ports.status.configured = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return configured_;
        };
    }
    if (!ports.status.started) {
        ports.status.started = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return started_;
        };
    }
    if (!ports.status.ownerAddress) {
        ports.status.ownerAddress = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return ownerAddress_;
        };
    }
    if (!ports.status.policyHash) {
        ports.status.policyHash = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return policyHashHex_;
        };
    }
    // The card's skill list comes from the registry, so a published card cannot
    // name a skill this agent has not registered — and cannot omit one it has.
    if (!ports.card.skills) {
        ports.card.skills = [this] { return skills(); };
    }

    // ---- the envelope, in the two shapes the skills read it in ------------
    //
    // Derived from one field so the mirrors cannot disagree. A limit that is
    // empty or not a decimal integer becomes zero, which holds every spend for
    // the owner and declines every offer: the safe direction, and the one the
    // chain would take anyway.
    AnchoredLimits limits;
    limits.periodBlocks = ports.envelope.periodBlocks;
    if (!parseDecimal(ports.envelope.perTx, limits.perTx)) {
        limits.perTx = 0;
    }
    if (!parseDecimal(ports.envelope.perPeriod, limits.perPeriod)) {
        limits.perPeriod = 0;
    }

    // The caller's store when it owns one — it is the half that has to survive a
    // restart — and the module's own otherwise.
    TaskStore &tasks = ports.tasks ? *ports.tasks : tasks_;

    const std::vector<std::shared_ptr<logos::agent::ISkill>> builtins{
        // Messaging, over Logos Delivery.
        std::make_shared<SendSkill>(ports.delivery),
        std::make_shared<JoinSkill>(ports.delivery),
        std::make_shared<CreateGroupSkill>(ports.delivery),
        // Storage, over Logos Storage. `share` takes both ports so it can say
        // which half failed.
        std::make_shared<UploadSkill>(ports.storage),
        std::make_shared<DownloadSkill>(ports.storage),
        std::make_shared<ListSkill>(ports.storage),
        std::make_shared<ShareSkill>(ports.storage, ports.share),
        // Blockchain: the wallet, and the anchored policy path.
        std::make_shared<WalletBalanceSkill>(ports.wallet, ports.agentAccount),
        std::make_shared<WalletSendSkill>(ports.wallet, ports.ownerApproval, ports.envelope),
        std::make_shared<WalletHistorySkill>(ports.wallet),
        // LEZ programs.
        std::make_shared<ProgramQuerySkill>(ports.sequencer),
        std::make_shared<ProgramCallSkill>(ports.program),
        std::make_shared<ProgramDeploySkill>(ports.program, ports.sequencer),
        // A2A coordination.
        std::make_shared<CardSkill>(ports.card),
        std::make_shared<DiscoverSkill>(ports.discovery),
        std::make_shared<TaskSkill>(ports.task, tasks),
        std::make_shared<SubscribeSkill>(ports.task, tasks),
        std::make_shared<CancelSkill>(ports.task, tasks),
        // The module's own two.
        std::make_shared<StatusSkill>(ports.status, tasks),
        std::make_shared<ConfigureSkill>(ports.config),
        // Pluggable inference. Registered with a null backend too: the skill
        // reports that none is configured, which is a decline, and a decline is
        // the only direction a backend is allowed to move a spend in anyway.
        std::make_shared<EvaluateTaskSkill>(ports.inference, limits),
    };

    int registered = 0;
    std::string refused;
    for (const auto &skill : builtins) {
        const StdLogosResult result = registerSkill(skill);
        if (result.success) {
            ++registered;
            continue;
        }
        if (!refused.empty()) {
            refused += ", ";
        }
        // The name is asked for here only to report it. `registerSkill` has
        // already published whatever name it read, and reading it twice is how a
        // skill comes to be advertised under a name nobody can dispatch.
        std::string name;
        try {
            name = skill->name();
        } catch (...) {
            name = "<unnamed>";
        }
        refused += name + " (" + result.error + ")";
    }

    if (!refused.empty()) {
        return StdLogosResult{false, registered,
                              "these built-in skills were not registered: " + refused};
    }
    return StdLogosResult{true, registered, {}};
}

std::string AgentModuleImpl::skills() const
{
    // Copy what we need out from under the lock. Everything below this block
    // calls into third-party code, and the mutex is not recursive: asking a
    // skill for its schema while holding it deadlocked the module against any
    // skill that called back in.
    std::vector<std::pair<std::string, std::shared_ptr<logos::agent::ISkill>>> registered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            // Not `[]`: an empty array is a valid, empty Agent Card, and a
            // caller cannot tell it from an agent that has no skills.
            return dumpSafe(json{{"error", "agent is not started"}});
        }
        registered.assign(skills_.begin(), skills_.end());
    }

    json card = json::array();
    for (const auto &entry : registered) {
        // The registered name, not a second call into `name()`: it is the key
        // `invoke()` dispatches on, so publishing anything else would advertise
        // a skill nobody can call. Built through the JSON library, so a name
        // carrying quotes or braces is escaped instead of forging the document
        // it is spliced into.
        json item{{"name", entry.first}};

        std::string schema;
        try {
            schema = entry.second->parameterSchema();
        } catch (const std::exception &e) {
            item["error"] = std::string("parameterSchema() threw: ") + e.what();
            card.push_back(std::move(item));
            continue;
        } catch (...) {
            item["error"] = "parameterSchema() threw";
            card.push_back(std::move(item));
            continue;
        }

        // One skill's bad schema costs that skill its parameters. Spliced in as
        // raw text it cost every skill the whole document — `""` alone produced
        // `"parameters":` with no value.
        auto parsed = json::parse(schema, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            item["error"] = "parameter schema is not a JSON object";
        } else {
            item["parameters"] = std::move(parsed);
        }
        card.push_back(std::move(item));
    }
    return dumpSafe(card);
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
