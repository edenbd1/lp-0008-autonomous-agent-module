#include "agent_module_plugin.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <thread>
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

/// The file the task snapshot lives in, inside the host's per-instance data
/// directory. One name, so a restart of the same instance finds what the last
/// run wrote — `instanceId()` is documented as stable across restarts for one
/// on-disk persistence directory, and the directory is what makes this unique.
constexpr const char *kTaskSnapshotFile = "/tasks.json";

/// How long the built-in owner channel sleeps between polls while waiting for an
/// approval. Short enough that an owner answering promptly is not left waiting
/// on a poll interval, long enough that a two-minute wait is not a busy loop.
constexpr int kApprovalPollSleepMs = 25;

} // namespace

AgentModuleImpl::AgentModuleImpl() = default;
AgentModuleImpl::~AgentModuleImpl() = default;

void AgentModuleImpl::onContextReady()
{
    // The host provisions this; a module loaded outside one (a unit test, the
    // `lgpd` CLI) gets an empty string, and the getter's own documentation says
    // to check it. Empty means nothing is written down, which is reported as
    // such rather than quietly behaving like a durable agent.
    const std::string dir = instancePersistencePath();
    if (dir.empty()) {
        return;
    }

    logos::agent::TaskStorePort port;
    port.snapshot = [this] { return tasks_.snapshot(); };
    port.restore = [this](const std::string &snapshotJson, std::string &err) {
        return tasks_.restore(snapshotJson, err);
    };

    std::lock_guard<std::mutex> lock(durableMutex_);
    durable_ = std::make_unique<logos::agent::TaskPersistence>(
        std::move(port), dir + kTaskSnapshotFile, logos::agent::posixSnapshotFilePort());
}

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return StdLogosResult{false, {}, "configure() must be called before start()"};
        }
        if (started_ || starting_) {
            return StdLogosResult{false, {}, "already started"};
        }
        starting_ = true;
    }

    // ---- recovery, before anything can be invoked --------------------------
    //
    // Here rather than in `onContextReady` because this is the step that is
    // allowed to refuse. A snapshot that cannot be read is not an empty task
    // list: truncated, corrupt, oversized or from a schema this build does not
    // know all mean "we do not know what was pending", and the one thing an
    // agent holding a payment journal must not do is come up believing it owes
    // nobody anything. `LoadReport::safeToStartEmpty()` is true for exactly one
    // outcome — there is no file at all — so that the difference cannot be lost
    // in a `if (!ok)`.
    //
    // Claimed *after* this, so a start refused here can be retried once the
    // operator has dealt with the file, and still wires the built-ins.
    std::string recoveryError;
    {
        std::lock_guard<std::mutex> durableLock(durableMutex_);
        if (durable_) {
            lastLoad_ = durable_->load();
            loadRan_ = true;
            if (!lastLoad_.ok() && !lastLoad_.safeToStartEmpty()) {
                recoveryError = "pending task state could not be recovered from " +
                                durable_->path() + ": " + lastLoad_.error +
                                ". Refusing to start with an empty task list on top of a "
                                "snapshot that could not be read";
            } else {
                lastSaved_ = tasks_.snapshot();
            }
        }
    }
    if (!recoveryError.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        starting_ = false;
        return StdLogosResult{false, {}, recoveryError};
    }

    bool wireBuiltins = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
    // A last checkpoint, before the lock, on the way down. Every state change
    // has already been written by `invoke`, so this normally writes nothing —
    // it is here for the change that came from somewhere else, and because the
    // orderly shutdown is the one restart we can actually prepare for.
    checkpointTasks();

    // Deliberately tolerant of a stop that was never started: this runs on the
    // shutdown path, where refusing would turn a tidy exit into an error.
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    return StdLogosResult{true, {}, {}};
}

void AgentModuleImpl::setOwnerNotifier(
    std::function<bool(const std::string &requestJson, int attempt)> notify)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ownerNotify_ = std::move(notify);
}

void AgentModuleImpl::setOwnerIdle(std::function<void()> idle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ownerIdle_ = std::move(idle);
}

StdLogosResult AgentModuleImpl::approveSpend(const std::string &requestId,
                                             const std::string &verdict)
{
    if (requestId.empty()) {
        return StdLogosResult{false, {}, "a request id is required: an answer that names no "
                                         "request could settle any spend"};
    }
    // Two words, and no third. "yes", "ok", "" and a JSON document are all
    // refused rather than guessed at, because the guess that costs money is the
    // one that reads an unfamiliar answer as consent.
    if (verdict != "approved" && verdict != "denied") {
        return StdLogosResult{false, {}, "the verdict must be 'approved' or 'denied'"};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingApprovals_.count(requestId) == 0) {
        // Including the case where it was pending a moment ago and the wait has
        // since timed out: that spend is over, and storing an answer for it
        // would leave an approval lying about for whatever is minted next.
        return StdLogosResult{false, {},
                              "no spend is waiting on '" + requestId +
                                  "': there is nothing here for this answer to release"};
    }
    approvalAnswers_[requestId] = verdict;
    return StdLogosResult{true, {}, {}};
}

bool AgentModuleImpl::publishApprovalRequest(const std::string &requestJson)
{
    std::function<bool(const std::string &, int)> notify;
    int attempt = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ownerNotify_) {
            return false;
        }
        notify = ownerNotify_;
        // The request is registered on the first attempt and left registered
        // for the retries, so `approveSpend` accepts an answer to any of them —
        // they all carry the same correlation id, which is the whole point of
        // re-sending the byte-identical request rather than a fresh one.
        auto parsedRequest = json::parse(requestJson, nullptr, false);
        std::string id;
        if (parsedRequest.is_object() && parsedRequest.contains("id") &&
            parsedRequest["id"].is_string()) {
            id = parsedRequest["id"].get<std::string>();
        }
        if (id.empty()) {
            // Nothing can answer a request that names no payment, so publishing
            // it would only produce an approval nobody could match to a spend.
            return false;
        }
        attempt = ++pendingApprovals_[id];
    }
    // Outside the lock: the notifier reaches the host, and a host that called
    // back into the module from it would deadlock against a non-recursive
    // mutex — the same rule the skill registry follows.
    return notify(requestJson, attempt);
}

std::string AgentModuleImpl::approvalAnswerFor(const std::string &requestId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = approvalAnswers_.find(requestId);
    return it == approvalAnswers_.end() ? std::string{} : it->second;
}

std::int64_t AgentModuleImpl::settingMs(const char *key, std::int64_t fallback) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = settings_.find(key);
    if (it == settings_.end() || it->second.empty()) {
        return fallback;
    }
    // `meta.configure` has already checked it is decimal digits, but this reads
    // a map anything holding a reference to the module could have written, and
    // a `stoll` that throws here would take down a spend rather than a setting.
    try {
        const long long value = std::stoll(it->second);
        return value < 0 ? fallback : static_cast<std::int64_t>(value);
    } catch (...) {
        return fallback;
    }
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
        // twenty-two refusals reported as a failure to register.
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
    if (!ports.status.durability) {
        ports.status.durability = [this] { return durabilityJson(); };
    }
    // The card's skill list comes from the registry, so a published card cannot
    // name a skill this agent has not registered — and cannot omit one it has.
    if (!ports.card.skills) {
        ports.card.skills = [this] { return skills(); };
    }
    // And `meta.skills` reads the same registry the card does, rather than a
    // catalogue of its own. Two producers would be two answers to "what can this
    // agent do", and the one a caller got would depend on which it asked.
    if (!ports.registry.listing) {
        ports.registry.listing = [this] { return skills(); };
    }

    // ---- runtime settings, for the two keys something actually reads ------
    //
    // `ConfigPort` was left unwired on purpose for years of this file's life,
    // and the reason still stands for most keys: a setting nothing reads would
    // let `meta.configure` answer `effective:true` for a value that changes
    // nothing. It is wired now because two keys are read — the approval timeout
    // and resend interval, below — and `meta.configure` still reports the
    // anchored envelope keys as *not* effective, because they still are not.
    if (!ports.config.set) {
        ports.config.set = [this](const std::string &key, const std::string &value) {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_[key] = value;
            return true;
        };
    }
    if (!ports.config.get) {
        ports.config.get = [this](const std::string &key) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = settings_.find(key);
            return it == settings_.end() ? std::string{} : it->second;
        };
    }

    // ---- the built-in owner channel ---------------------------------------
    //
    // Only when this module is hosted. `ownerNotify_` is installed by the Logos
    // Core export, which is the only thing that can put an event on the
    // runtime's transport; a module constructed directly — a unit test, a host
    // that links it and wires its own channel — has none, and then every field
    // below stays as the caller left it. That distinction is load-bearing:
    // wiring an answer path that nothing can reach would turn today's immediate,
    // honest "no owner channel to ask for approval" into a two-minute wait
    // ending in a timeout blamed on an owner who was never asked.
    //
    // A host that wires its own `requestApproval` and nothing else still gets
    // the answer path, because `approveSpend` really is a route by which an
    // answer can arrive.
    bool hosted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hosted = static_cast<bool>(ownerNotify_);
    }
    if (hosted) {
        if (!ports.ownerApproval.requestApproval) {
            ports.ownerApproval.requestApproval = [this](const std::string &requestJson) {
                return publishApprovalRequest(requestJson);
            };
        }
        if (!ports.ownerApproval.pollDecision) {
            ports.ownerApproval.pollDecision = [this](const std::string &requestId) {
                return approvalAnswerFor(requestId);
            };
        }
        if (!ports.ownerApproval.nextNonce) {
            // Milliseconds since the epoch, not a counter starting at one.
            //
            // A process-local counter is reset by exactly the restart this
            // module now survives, so the agent would mint nonce 1 a second
            // time — and a nonce is half of what binds an approval to one
            // payment. The clock does not go back, so an approval the owner
            // signed before the restart cannot be replayed against a spend
            // minted after it.
            ports.ownerApproval.nextNonce = [] {
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            };
        }
        if (!ports.ownerApproval.nowMs) {
            // Steady, not system: a wall clock that is stepped backwards by NTP
            // in the middle of a wait would extend the deadline by however far
            // it moved.
            ports.ownerApproval.nowMs = [] {
                return static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            };
        }
        if (!ports.ownerApproval.idle) {
            // The host's idle, when it has one. Inside a loaded module it pumps
            // the `logos_host` event loop, which is the only reason the owner's
            // answer can reach a wait that is running on that same loop —
            // see setOwnerIdle(). Sleeping is the fallback, and it is right for
            // every caller that has no event loop to pump.
            std::function<void()> hostIdle;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                hostIdle = ownerIdle_;
            }
            ports.ownerApproval.idle =
                hostIdle ? std::move(hostIdle) : std::function<void()>([] {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kApprovalPollSleepMs));
                });
        }
        if (!ports.ownerApproval.timeoutMs) {
            ports.ownerApproval.timeoutMs = [this] {
                return settingMs("approval_timeout_ms", kDefaultApprovalTimeoutMs);
            };
        }
        if (!ports.ownerApproval.resendIntervalMs) {
            ports.ownerApproval.resendIntervalMs = [this] {
                return settingMs("approval_resend_ms", kDefaultApprovalResendMs);
            };
        }
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
        // The module's own three. `meta.skills` is the catalogue the prize asks
        // for by name; it is registered like any other skill rather than
        // special-cased in `invoke()`, so it appears in its own listing and in
        // the Agent Card, and a caller cannot tell it from a third party's.
        std::make_shared<StatusSkill>(ports.status, tasks),
        std::make_shared<ConfigureSkill>(ports.config),
        std::make_shared<SkillsSkill>(ports.registry),
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
    const auto answer = json::parse(result, nullptr, false);
    if (answer.is_discarded()) {
        return fail("skill '" + name + "' returned a result that is not JSON");
    }

    // The spend that was waiting on an approval has finished waiting, whatever
    // it decided, so retire its correlation id. Read off the answer rather than
    // tracked here on purpose: `invoke` is called from several threads and the
    // reply is the one thing that certainly belongs to *this* call. An id left
    // behind would let a late "approved" sit in the table until something was
    // minted with the same id — an approval waiting for a payment nobody has
    // described yet.
    if (answer.is_object() && answer.contains("request_id") && answer["request_id"].is_string()) {
        const std::string requestId = answer["request_id"].get<std::string>();
        std::lock_guard<std::mutex> lock(mutex_);
        pendingApprovals_.erase(requestId);
        approvalAnswers_.erase(requestId);
    }

    // Whatever the skill did to the task store, write it down before the answer
    // leaves the module. A crash between the reply and the next checkpoint is
    // the window this closes.
    checkpointTasks();
    return result;
}

void AgentModuleImpl::checkpointTasks()
{
    std::lock_guard<std::mutex> lock(durableMutex_);
    if (!durable_) {
        return;
    }
    // The store's own rendering, compared against what was last written. Two
    // calls that changed nothing — `meta.status`, a refused `agent.task` — cost
    // a snapshot and no fsync.
    std::string current = tasks_.snapshot();
    if (current == lastSaved_) {
        return;
    }
    std::string err;
    if (!durable_->save(err)) {
        // Not propagated to the caller: the skill's work happened, and what
        // failed is the record of it. Reported through `meta.status`, which is
        // where an operator looks after the restart that lost something.
        lastSaveError_ = err;
        return;
    }
    lastSaveError_.clear();
    lastSaved_ = std::move(current);
}

std::string AgentModuleImpl::durabilityJson() const
{
    std::lock_guard<std::mutex> lock(durableMutex_);
    if (!durable_) {
        // Empty, not `{"durable":false}`: `meta.status` renders an unreported
        // durability as null, and null is the honest answer for a module nobody
        // gave a directory to.
        return {};
    }
    json out{{"path", durable_->path()}, {"recovery_ran", loadRan_}};
    if (loadRan_) {
        switch (lastLoad_.outcome) {
        case logos::agent::LoadOutcome::Loaded:
            out["recovery"] = "loaded";
            break;
        case logos::agent::LoadOutcome::Absent:
            out["recovery"] = "absent";
            break;
        case logos::agent::LoadOutcome::Failed:
            out["recovery"] = "failed";
            break;
        }
        out["recovered_tasks"] = lastLoad_.tasks;
        out["recovered_active"] = lastLoad_.active;
        out["settled_payments"] = lastLoad_.settledPayments;
        // The number an operator has to act on: money that may or may not have
        // moved, which no restart may decide on its own.
        out["uncertain_payments"] = lastLoad_.uncertainPayments;
        if (!lastLoad_.error.empty()) {
            out["recovery_error"] = lastLoad_.error;
        }
    }
    if (!lastSaveError_.empty()) {
        out["last_save_error"] = lastSaveError_;
    }
    return dumpSafe(out);
}

std::string AgentModuleImpl::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dumpSafe(json{{"configured", configured_},
                         {"started", started_},
                         {"owner", ownerAddress_},
                         {"policy", policyHashHex_}});
}
