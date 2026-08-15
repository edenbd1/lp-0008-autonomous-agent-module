#include "agent_skills.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>

namespace logos::agent {

using nlohmann::json;

namespace {

/// Every skill answers in the same shape, so a caller — including another agent
/// over A2A — can branch without knowing which skill it called.
std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
}

/// Parse and require a field, rather than letting nlohmann throw out of invoke()
/// and take the module down with it.
json parse(const std::string &s, std::string &err)
{
    auto j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "parameters must be a JSON object";
        return json::object();
    }
    return j;
}

bool field(const json &j, const char *key, std::string &out, std::string &err)
{
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        err = std::string("missing or empty '") + key + "'";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

/// A string field that is allowed to be absent, but not to be the wrong type.
bool optionalString(const json &j, const char *key, std::string &out, std::string &err)
{
    if (!j.contains(key) || j[key].is_null()) return true;
    if (!j[key].is_string()) {
        err = std::string("'") + key + "' must be a string";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

bool optionalUnsigned(const json &j, const char *key, std::uint64_t &out, bool &present,
                      std::string &err)
{
    present = false;
    if (!j.contains(key) || j[key].is_null()) return true;
    if (!j[key].is_number_unsigned()) {
        err = std::string("'") + key + "' must be a non-negative integer";
        return false;
    }
    out = j[key].get<std::uint64_t>();
    present = true;
    return true;
}

/// 32 hex characters, the same width `scripts/a2a-task.sh` draws for a task id.
std::string randomId()
{
    static const char *digits = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> pick(0, 15);
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(digits[pick(gen)]);
    return out;
}

bool isDecimal(const std::string &s)
{
    if (s.empty() || s.size() > 20) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool isLowerHex64(const std::string &s)
{
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool startsWith(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string taskStateName(TaskState state)
{
    switch (state) {
    case TaskState::Submitted: return "submitted";
    case TaskState::Working: return "working";
    case TaskState::InputRequired: return "input-required";
    case TaskState::AuthRequired: return "auth-required";
    case TaskState::Completed: return "completed";
    case TaskState::Canceled: return "canceled";
    case TaskState::Failed: return "failed";
    case TaskState::Rejected: return "rejected";
    case TaskState::Unknown: break;
    }
    return "unknown";
}

bool taskStateFromName(const std::string &name, TaskState &out)
{
    static const std::map<std::string, TaskState> byName{
        {"submitted", TaskState::Submitted},
        {"working", TaskState::Working},
        {"input-required", TaskState::InputRequired},
        {"auth-required", TaskState::AuthRequired},
        {"completed", TaskState::Completed},
        {"canceled", TaskState::Canceled},
        {"failed", TaskState::Failed},
        {"rejected", TaskState::Rejected},
        {"unknown", TaskState::Unknown},
    };
    const auto it = byName.find(name);
    if (it == byName.end()) return false;
    out = it->second;
    return true;
}

bool isTerminalState(TaskState state)
{
    switch (state) {
    case TaskState::Completed:
    case TaskState::Canceled:
    case TaskState::Failed:
    case TaskState::Rejected:
        return true;
    default:
        return false;
    }
}

bool canTransition(TaskState from, TaskState to)
{
    // `unknown` is a confession that the state was lost, not a live state to
    // drive a task out of.
    if (from == TaskState::Unknown || to == TaskState::Unknown) return false;
    if (to == TaskState::Submitted) return false; // only `create` opens a task

    if (from == to) {
        // A streaming agent repeats `working` as progress arrives, so this one
        // self-transition is normal. Every other state that stays put is a bug
        // dressed as an update.
        return from == TaskState::Working;
    }

    // One switch, no earlier shortcut for the terminal states: two places that
    // both decide this would let a test pass while only one of them is right.
    switch (from) {
    case TaskState::Submitted:
        // Anything may follow submission, including an outright rejection.
        return true;
    case TaskState::Working:
        return to != TaskState::Rejected; // rejection is an answer to the request itself
    case TaskState::InputRequired:
    case TaskState::AuthRequired:
        return to == TaskState::Working || to == TaskState::Completed ||
               to == TaskState::Failed || to == TaskState::Canceled;
    // Nothing leaves a terminal state. A completed task that goes back to
    // working would un-finish work that was already paid for and reported.
    case TaskState::Completed:
    case TaskState::Canceled:
    case TaskState::Failed:
    case TaskState::Rejected:
    case TaskState::Unknown:
        return false;
    }
    return false;
}

std::string taskTopic(const std::string &agentAddress, const std::string &taskId)
{
    return "/lp-0008/1/task-" + agentAddress + "-" + taskId + "/json";
}

std::string base64Url(const std::string &bytes)
{
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
        i += 3;
    }
    const std::size_t left = bytes.size() - i;
    if (left == 1) {
        const std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
    } else if (left == 2) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
    }
    // No padding: RFC 7515 §2 defines base64url for JWS without it.
    return out;
}

// ---------------------------------------------------------------------------
// TaskStore
// ---------------------------------------------------------------------------

bool TaskStore::create(const std::string &id, const std::string &contextId,
                       const std::string &agent, const std::string &skill, std::string &err)
{
    if (id.empty()) {
        err = "a task needs an id";
        return false;
    }
    if (agent.empty() || skill.empty()) {
        err = "a task needs an agent address and a skill";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (tasks_.count(id) != 0) {
        err = "task " + id + " already exists";
        return false;
    }
    Task t;
    t.id = id;
    t.contextId = contextId.empty() ? id : contextId;
    t.agent = agent;
    t.skill = skill;
    t.state = TaskState::Submitted;
    t.history.push_back(taskStateName(TaskState::Submitted));
    tasks_.emplace(id, std::move(t));
    return true;
}

bool TaskStore::advance(const std::string &id, TaskState to, const std::string &note,
                        std::string &err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        err = "no task " + id;
        return false;
    }
    if (!canTransition(it->second.state, to)) {
        err = "task " + id + " is " + taskStateName(it->second.state) +
              " and cannot move to " + taskStateName(to);
        return false;
    }
    it->second.state = to;
    it->second.history.push_back(taskStateName(to));
    if (!note.empty()) it->second.note = note;
    return true;
}

bool TaskStore::applyUpdate(const std::string &eventJson, std::string &err)
{
    auto j = json::parse(eventJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "a status update must be a JSON object";
        return false;
    }
    std::string id;
    if (j.contains("taskId") && j["taskId"].is_string()) {
        id = j["taskId"].get<std::string>();
    } else if (j.contains("id") && j["id"].is_string()) {
        id = j["id"].get<std::string>();
    }
    if (id.empty()) {
        err = "a status update must name a task";
        return false;
    }
    if (!j.contains("status") || !j["status"].is_object()) {
        err = "a status update must carry a 'status' object";
        return false;
    }
    const json &status = j["status"];
    if (!status.contains("state") || !status["state"].is_string()) {
        err = "a status update must carry 'status.state'";
        return false;
    }
    TaskState to = TaskState::Unknown;
    const std::string stateName = status["state"].get<std::string>();
    if (!taskStateFromName(stateName, to)) {
        err = "'" + stateName + "' is not an A2A task state";
        return false;
    }
    std::string note;
    if (status.contains("message") && status["message"].is_string()) {
        note = status["message"].get<std::string>();
    }
    return advance(id, to, note, err);
}

bool TaskStore::recordPayment(const std::string &id, std::uint64_t amount,
                              const std::string &payAccount, const std::string &settlementTx,
                              std::string &err)
{
    // An amount with no transaction hash is not a payment, and recording one
    // would make an unsettled task look paid for the rest of its life.
    if (settlementTx.empty()) {
        err = "a payment without a settlement transaction is not a payment";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        err = "no task " + id;
        return false;
    }
    it->second.pricePaid = amount;
    it->second.payAccount = payAccount;
    it->second.settlementTx = settlementTx;
    return true;
}

bool TaskStore::markSubscribed(const std::string &id, std::string &err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        err = "no task " + id;
        return false;
    }
    it->second.subscribed = true;
    return true;
}

bool TaskStore::find(const std::string &id, Task &out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;
    out = it->second;
    return true;
}

std::vector<TaskStore::Task> TaskStore::active() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Task> out;
    for (const auto &entry : tasks_) {
        if (!isTerminalState(entry.second.state)) out.push_back(entry.second);
    }
    return out;
}

std::size_t TaskStore::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

std::string TaskStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto &entry : tasks_) {
        const Task &t = entry.second;
        out.push_back(json{{"id", t.id},
                           {"contextId", t.contextId},
                           {"agent", t.agent},
                           {"skill", t.skill},
                           {"state", taskStateName(t.state)},
                           {"pricePaid", t.pricePaid},
                           {"payAccount", t.payAccount},
                           {"settlementTx", t.settlementTx},
                           {"subscribed", t.subscribed},
                           {"history", t.history},
                           {"note", t.note}});
    }
    return out.dump();
}

bool TaskStore::restore(const std::string &snapshotJson, std::string &err)
{
    auto j = json::parse(snapshotJson, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        err = "a snapshot must be a JSON array of tasks";
        return false;
    }
    // Build into a scratch map first. A restore that fails halfway would
    // otherwise leave the agent running against a store that is neither the old
    // one nor the new one.
    std::map<std::string, Task> loaded;
    for (const auto &e : j) {
        if (!e.is_object()) {
            err = "every task in a snapshot must be an object";
            return false;
        }
        Task t;
        std::string ferr;
        if (!field(e, "id", t.id, ferr) || !field(e, "agent", t.agent, ferr) ||
            !field(e, "skill", t.skill, ferr)) {
            err = "task in snapshot: " + ferr;
            return false;
        }
        std::string stateName;
        if (!field(e, "state", stateName, ferr)) {
            err = "task " + t.id + " in snapshot: " + ferr;
            return false;
        }
        if (!taskStateFromName(stateName, t.state)) {
            err = "task " + t.id + " in snapshot has state '" + stateName +
                  "', which A2A does not define";
            return false;
        }
        if (loaded.count(t.id) != 0) {
            err = "task " + t.id + " appears twice in the snapshot";
            return false;
        }
        t.contextId = e.value("contextId", t.id);
        t.pricePaid = e.value("pricePaid", std::uint64_t{0});
        t.payAccount = e.value("payAccount", std::string{});
        t.settlementTx = e.value("settlementTx", std::string{});
        t.subscribed = e.value("subscribed", false);
        t.note = e.value("note", std::string{});
        if (e.contains("history") && e["history"].is_array()) {
            for (const auto &h : e["history"]) {
                if (h.is_string()) t.history.push_back(h.get<std::string>());
            }
        }
        if (t.history.empty()) t.history.push_back(stateName);
        loaded.emplace(t.id, std::move(t));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_ = std::move(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Agent Cards
// ---------------------------------------------------------------------------

namespace {

/// Required by the A2A v0.3.0 schema (`AgentCard.required`), all strings.
const char *const kRequiredCardStrings[] = {"protocolVersion", "name", "description", "url",
                                            "version"};

bool stringArray(const json &j, const char *key, std::string &err)
{
    if (!j.contains(key)) {
        err = std::string("the card has no '") + key + "', which A2A requires";
        return false;
    }
    if (!j[key].is_array() || j[key].empty()) {
        err = std::string("'") + key + "' must be a non-empty array";
        return false;
    }
    for (const auto &e : j[key]) {
        if (!e.is_string() || e.get<std::string>().empty()) {
            err = std::string("every entry in '") + key + "' must be a non-empty string";
            return false;
        }
    }
    return true;
}

std::string validateCard(const json &c)
{
    if (!c.is_object()) return "an Agent Card must be a JSON object";

    for (const char *key : kRequiredCardStrings) {
        if (!c.contains(key)) {
            return std::string("the card has no '") + key + "', which A2A requires";
        }
        if (!c[key].is_string() || c[key].get<std::string>().empty()) {
            return std::string("'") + key + "' must be a non-empty string";
        }
    }
    if (!c.contains("capabilities")) {
        return "the card has no 'capabilities', which A2A requires";
    }
    if (!c["capabilities"].is_object()) return "'capabilities' must be an object";

    std::string err;
    if (!stringArray(c, "defaultInputModes", err)) return err;
    if (!stringArray(c, "defaultOutputModes", err)) return err;

    if (!c.contains("skills")) return "the card has no 'skills', which A2A requires";
    if (!c["skills"].is_array()) return "'skills' must be an array";
    for (const auto &s : c["skills"]) {
        if (!s.is_object()) return "every skill must be an object";
        for (const char *key : {"id", "name", "description"}) {
            if (!s.contains(key) || !s[key].is_string() || s[key].get<std::string>().empty()) {
                return std::string("every skill needs a non-empty '") + key + "'";
            }
        }
        if (!stringArray(s, "tags", err)) {
            return "skill '" + s["id"].get<std::string>() + "': " + err;
        }
    }

    // A2A makes `provider` optional and both of its fields required. A card
    // naming an organization with no URL — which is what this repository has
    // been publishing — leaves a reader with a name and no way to check it.
    if (c.contains("provider")) {
        const json &p = c["provider"];
        if (!p.is_object()) return "'provider' must be an object";
        for (const char *key : {"organization", "url"}) {
            if (!p.contains(key) || !p[key].is_string() || p[key].get<std::string>().empty()) {
                return std::string("'provider' needs a non-empty '") + key +
                       "': A2A requires both";
            }
        }
    }

    // A2A binds `preferredTransport` to `url`: the transport named there MUST
    // be reachable at that URL. For our own binding that is checkable.
    if (c.contains("preferredTransport")) {
        if (!c["preferredTransport"].is_string() ||
            c["preferredTransport"].get<std::string>().empty()) {
            return "'preferredTransport' must be a non-empty string";
        }
        if (c["preferredTransport"].get<std::string>() == "logos-messaging" &&
            !startsWith(c.value("url", std::string{}), "logos-messaging://")) {
            return "the card prefers logos-messaging but its 'url' is not a "
                   "logos-messaging:// address";
        }
    }

    if (c.contains("signatures")) {
        if (!c["signatures"].is_array()) return "'signatures' must be an array";
        for (const auto &s : c["signatures"]) {
            if (!s.is_object()) return "every signature must be an object";
            for (const char *key : {"protected", "signature"}) {
                if (!s.contains(key) || !s[key].is_string() ||
                    s[key].get<std::string>().empty()) {
                    return std::string("every signature needs a non-empty '") + key +
                           "' (RFC 7515)";
                }
            }
        }
    }

    // The Logos extension. A price nobody can pay is worse than no price: the
    // client agent would settle into nothing and call the task accepted.
    if (c.contains("x-logos")) {
        const json &x = c["x-logos"];
        if (!x.is_object()) return "'x-logos' must be an object";
        if (x.contains("lezAccount") &&
            (!x["lezAccount"].is_string() || x["lezAccount"].get<std::string>().empty())) {
            return "'x-logos.lezAccount' must be a non-empty string";
        }
        if (x.contains("pricePerTask")) {
            if (!x["pricePerTask"].is_number_unsigned()) {
                return "'x-logos.pricePerTask' must be a non-negative integer";
            }
            if (x["pricePerTask"].get<std::uint64_t>() > 0) {
                if (!x.contains("paymentAccount") || !x["paymentAccount"].is_string() ||
                    x["paymentAccount"].get<std::string>().empty()) {
                    return "the card charges a price but names no 'x-logos.paymentAccount' "
                           "to pay it into";
                }
            }
        }
    }
    return {};
}

} // namespace

std::string validateAgentCard(const std::string &cardJson)
{
    auto j = json::parse(cardJson, nullptr, false);
    if (j.is_discarded()) return "the card is not valid JSON";
    return validateCard(j);
}

// ---------------------------------------------------------------------------
// agent.card
// ---------------------------------------------------------------------------

std::string CardSkill::parameterSchema() const
{
    return R"({"type":"object","required":[],"properties":{}})";
}

std::string CardSkill::invoke(const std::string &paramsJson)
{
    // The card takes no parameters, but it is still called with some, and a
    // malformed call must not be answered with a card.
    std::string err;
    if (!paramsJson.empty()) {
        parse(paramsJson, err);
        if (!err.empty()) return fail(err);
    }

    const auto call = [](const std::function<std::string()> &f) {
        return f ? f() : std::string{};
    };
    const std::string account = call(port_.lezAccount);
    if (account.empty()) return fail("the agent has no LEZ account to identify itself with");
    const std::string agentName = call(port_.name);
    if (agentName.empty()) return fail("the agent has no name");
    const std::string version = call(port_.version);
    if (version.empty()) return fail("the agent has no version");

    const std::uint64_t price = port_.pricePerTask ? port_.pricePerTask() : 0;
    const std::string payAccount = call(port_.payAccount);
    if (price > 0 && payAccount.empty()) {
        return fail("the agent charges " + std::to_string(price) +
                    " LEZ per task but has no account to be paid into");
    }

    json card;
    card["protocolVersion"] = "0.3.0";
    card["name"] = agentName;
    const std::string description = call(port_.description);
    card["description"] = description.empty() ? ("Logos agent " + agentName) : description;
    // Logos Messaging is the transport, so the address is the agent's account.
    // A2A binds this URL to the transport named below.
    card["url"] = "logos-messaging://" + account;
    card["preferredTransport"] = "logos-messaging";
    card["version"] = version;
    card["capabilities"] = json{{"streaming", true},        // agent.subscribe
                                {"stateTransitionHistory", true}, // the store keeps it
                                {"pushNotifications", false}};    // no webhooks here
    card["defaultInputModes"] = json::array({"application/json"});
    card["defaultOutputModes"] = json::array({"application/json"});

    const std::string org = call(port_.providerOrganization);
    const std::string orgUrl = call(port_.providerUrl);
    if (!org.empty() || !orgUrl.empty()) {
        if (org.empty() || orgUrl.empty()) {
            return fail("a provider needs both 'organization' and 'url': A2A requires both, "
                        "and a name with no URL cannot be checked");
        }
        card["provider"] = json{{"organization", org}, {"url", orgUrl}};
    }

    // The skills come from the registry's own listing, so the card cannot
    // advertise something the agent has not registered.
    json skills = json::array();
    const std::string skillsJson = call(port_.skills);
    if (!skillsJson.empty()) {
        auto parsed = json::parse(skillsJson, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array()) {
            return fail("the skill registry did not return a JSON array");
        }
        for (const auto &s : parsed) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string() ||
                s["name"].get<std::string>().empty()) {
                return fail("the skill registry returned an entry with no name");
            }
            const std::string id = s["name"].get<std::string>();
            const auto dot = id.find('.');
            json entry;
            entry["id"] = id;
            entry["name"] = id;
            entry["description"] = "Logos agent skill " + id;
            entry["tags"] = json::array({dot == std::string::npos ? std::string("logos")
                                                                  : id.substr(0, dot)});
            entry["inputModes"] = json::array({"application/json"});
            entry["outputModes"] = json::array({"application/json"});
            if (s.contains("parameters")) {
                // A2A has no field for a parameter schema, and the prize asks
                // the card to declare input/output schemas — so it travels as an
                // extension rather than being dropped.
                if (s["parameters"].is_string()) {
                    auto schema = json::parse(s["parameters"].get<std::string>(), nullptr, false);
                    if (!schema.is_discarded()) entry["x-logos-parameters"] = schema;
                } else {
                    entry["x-logos-parameters"] = s["parameters"];
                }
                if (entry.contains("x-logos-parameters") &&
                    entry["x-logos-parameters"].is_object() &&
                    entry["x-logos-parameters"].contains("description") &&
                    entry["x-logos-parameters"]["description"].is_string()) {
                    entry["description"] = entry["x-logos-parameters"]["description"];
                }
            }
            skills.push_back(std::move(entry));
        }
    }
    card["skills"] = std::move(skills);

    json x;
    x["lezAccount"] = account;
    x["pricePerTask"] = price;
    x["settlement"] = "lez-chained-authenticated-transfer";
    if (!payAccount.empty()) {
        // `Public/…` is the form `spel` resolves, and the form the settlement in
        // scripts/a2a-task.sh is addressed to.
        x["paymentAccount"] =
            startsWith(payAccount, "Public/") ? payAccount : ("Public/" + payAccount);
    }
    card["x-logos"] = std::move(x);

    // Check our own output against the same validator discovery uses. If these
    // two ever disagree, an agent publishes a card its own peers reject.
    const std::string invalid = validateCard(card);
    if (!invalid.empty()) {
        return fail("the card this agent produced is not a valid A2A card: " + invalid);
    }

    // A2A carries the signature as an RFC 7515 JWS with a detached payload: the
    // payload is the card without its own signatures, so a verifier can strip
    // them and recompute exactly this string.
    std::string alg = call(port_.algorithm);
    if (alg.empty()) alg = "EdDSA"; // LEZ account keys
    std::string kid = call(port_.keyId);
    if (kid.empty()) kid = account; // the account is the identity
    const std::string protectedB64 = base64Url(json{{"alg", alg}, {"kid", kid}}.dump());
    const std::string payloadB64 = base64Url(card.dump());
    const std::string signingInput = protectedB64 + "." + payloadB64;

    // Refuse rather than publish an unsigned card. An unsigned card is a
    // document anyone can rewrite — including the payment account in it — and
    // the prize asks for a signed one.
    if (!port_.sign) return fail("no signing key is configured, and an unsigned Agent Card "
                                 "is forgeable by anyone who can publish on the topic");
    const std::string signature = port_.sign(signingInput);
    if (signature.empty()) {
        return fail("the Agent Card could not be signed, so it is not published");
    }
    card["signatures"] = json::array({json{{"protected", protectedB64},
                                           {"signature", signature}}});

    return done(json{{"card", card}, {"signed", true}});
}

// ---------------------------------------------------------------------------
// agent.discover
// ---------------------------------------------------------------------------

std::string DiscoverSkill::parameterSchema() const
{
    return R"({"type":"object","required":["topic"],)"
           R"("properties":{"topic":{"type":"string","description":"Logos Messaging discovery topic"},)"
           R"("require_signed":{"type":"boolean","description":"reject unsigned cards"}}})";
}

std::string DiscoverSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string topic;
    if (!field(p, "topic", topic, err)) return fail(err);
    bool requireSigned = false;
    if (p.contains("require_signed")) {
        if (!p["require_signed"].is_boolean()) return fail("'require_signed' must be a boolean");
        requireSigned = p["require_signed"].get<bool>();
    }
    if (!port_.fetch) return fail("no discovery transport is configured");

    const std::vector<std::string> documents = port_.fetch(topic);
    json agents = json::array();
    json rejected = json::array();
    std::size_t index = 0;
    for (const auto &doc : documents) {
        const std::size_t at = index++;
        auto c = json::parse(doc, nullptr, false);
        if (c.is_discarded()) {
            rejected.push_back(json{{"index", at}, {"reason", "the card is not valid JSON"}});
            continue;
        }
        const std::string why = validateCard(c);
        if (!why.empty()) {
            rejected.push_back(json{{"index", at}, {"reason", why}});
            continue;
        }
        const bool signed_ = c.contains("signatures") && c["signatures"].is_array() &&
                             !c["signatures"].empty();
        if (requireSigned && !signed_) {
            rejected.push_back(json{{"index", at},
                                    {"reason", "the card is unsigned and this call requires a "
                                               "signed card"}});
            continue;
        }
        json summary;
        summary["name"] = c["name"];
        summary["url"] = c["url"];
        summary["version"] = c["version"];
        summary["signed"] = signed_;
        summary["transport"] = c.value("preferredTransport", std::string("JSONRPC"));
        json ids = json::array();
        for (const auto &s : c["skills"]) ids.push_back(s["id"]);
        summary["skills"] = std::move(ids);
        summary["price"] = 0;
        if (c.contains("x-logos")) {
            const json &x = c["x-logos"];
            summary["price"] = x.value("pricePerTask", std::uint64_t{0});
            if (x.contains("lezAccount")) summary["lez_account"] = x["lezAccount"];
            if (x.contains("paymentAccount")) summary["pay_account"] = x["paymentAccount"];
        }
        summary["card"] = c;
        agents.push_back(std::move(summary));
    }
    return done(json{{"topic", topic},
                     {"seen", documents.size()},
                     {"agents", agents},
                     {"rejected", rejected}});
}

// ---------------------------------------------------------------------------
// agent.task
// ---------------------------------------------------------------------------

std::string TaskSkill::parameterSchema() const
{
    return R"({"type":"object","required":["agent_address"],)"
           R"("properties":{"agent_address":{"type":"string"},"skill":{"type":"string"},)"
           R"("params":{"type":"object"},"card":{"type":"object","description":"the peer's A2A Agent Card"},)"
           R"("task_id":{"type":"string"},"context_id":{"type":"string"},)"
           R"("message":{"type":"string","description":"input for a task in input-required"},)"
           R"("max_price":{"type":"integer","description":"refuse a declared price above this"},)"
           R"("price":{"type":"integer"},"pay_account":{"type":"string"}}})";
}

std::string TaskSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string agentAddress;
    if (!field(p, "agent_address", agentAddress, err)) return fail(err);
    std::string taskId, contextId, skill, message;
    if (!optionalString(p, "task_id", taskId, err)) return fail(err);
    if (!optionalString(p, "context_id", contextId, err)) return fail(err);
    if (!optionalString(p, "skill", skill, err)) return fail(err);
    if (!optionalString(p, "message", message, err)) return fail(err);
    if (p.contains("params") && !p["params"].is_null() && !p["params"].is_object()) {
        return fail("'params' must be an object");
    }
    const json inner = p.contains("params") && p["params"].is_object() ? p["params"]
                                                                       : json::object();

    // Continuation: a task that asked for input is answered on the same id.
    TaskStore::Task existing;
    if (!taskId.empty() && tasks_.find(taskId, existing)) {
        if (existing.agent != agentAddress) {
            return fail("task " + taskId + " is with " + existing.agent + ", not " +
                        agentAddress);
        }
        if (message.empty() && inner.empty()) {
            return fail("a continuation must carry a 'message' or 'params'");
        }
        // Only a task that asked for something can be answered. This is also
        // what stops a retried `agent.task` from being mistaken for an answer:
        // reopening an id that is still `submitted` is a duplicate request, not
        // a continuation, and a task in a terminal state is neither.
        if (existing.state != TaskState::InputRequired &&
            existing.state != TaskState::AuthRequired) {
            return fail("task " + taskId + " is " + taskStateName(existing.state) +
                        " and has not asked for input, so it cannot be continued");
        }
        if (!canTransition(existing.state, TaskState::Working)) {
            return fail("task " + taskId + " is " + taskStateName(existing.state) +
                        " and cannot be continued");
        }
        if (!port_.ready || !port_.ready()) return fail("delivery node is not started");
        json request = json{
            {"jsonrpc", "2.0"},
            {"id", randomId()},
            {"method", "message/send"},
            {"params",
             json{{"message", json{{"kind", "message"},
                                   {"role", "user"},
                                   {"messageId", randomId()},
                                   {"taskId", taskId},
                                   {"contextId", existing.contextId},
                                   {"parts", json::array({json{{"kind", "data"},
                                                               {"data", json{{"message", message},
                                                                             {"params", inner}}}}})}}}}}};
        if (!port_.send || !port_.send(taskTopic(agentAddress, taskId), request.dump())) {
            return fail("the input for task " + taskId + " could not be delivered");
        }
        if (!tasks_.advance(taskId, TaskState::Working, message, err)) return fail(err);
        return done(json{{"task_id", taskId},
                         {"context_id", existing.contextId},
                         {"state", taskStateName(TaskState::Working)},
                         {"continued", true}});
    }

    if (skill.empty()) return fail("missing or empty 'skill'");

    // The price comes from the peer's card when there is one, because that is
    // the figure the peer published and the only one it is bound to.
    std::uint64_t price = 0;
    std::string payAccount;
    bool present = false;
    if (p.contains("card") && !p["card"].is_null()) {
        if (!p["card"].is_object()) return fail("'card' must be an object");
        const json &card = p["card"];
        const std::string why = validateCard(card);
        if (!why.empty()) return fail("the peer's Agent Card is not usable: " + why);
        bool advertises = false;
        for (const auto &s : card["skills"]) {
            if (s["id"].get<std::string>() == skill) advertises = true;
        }
        if (!advertises) {
            return fail("the card does not advertise '" + skill + "'");
        }
        if (card.contains("x-logos")) {
            const json &x = card["x-logos"];
            if (x.contains("lezAccount") &&
                x["lezAccount"].get<std::string>() != agentAddress) {
                return fail("the card belongs to " + x["lezAccount"].get<std::string>() +
                            ", not to " + agentAddress);
            }
            price = x.value("pricePerTask", std::uint64_t{0});
            payAccount = x.value("paymentAccount", std::string{});
        }
    } else {
        if (!optionalUnsigned(p, "price", price, present, err)) return fail(err);
        if (!optionalString(p, "pay_account", payAccount, err)) return fail(err);
    }
    if (price > 0 && payAccount.empty()) {
        return fail("the task costs " + std::to_string(price) +
                    " LEZ but no account was named to pay it into");
    }

    std::uint64_t maxPrice = 0;
    if (!optionalUnsigned(p, "max_price", maxPrice, present, err)) return fail(err);
    if (present && price > maxPrice) {
        // Refused before anything is sent and long before anything is signed:
        // proving time on a transfer the owner's envelope would refuse is paid
        // for whether or not the chain accepts it.
        return fail("the declared price " + std::to_string(price) + " LEZ is above the " +
                    std::to_string(maxPrice) + " LEZ this call allows");
    }

    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    if (taskId.empty()) taskId = randomId();
    if (contextId.empty()) contextId = randomId();
    if (!tasks_.create(taskId, contextId, agentAddress, skill, err)) return fail(err);

    const json request = json{
        {"jsonrpc", "2.0"},
        {"id", randomId()},
        {"method", "message/send"},
        {"params",
         json{{"message", json{{"kind", "message"},
                               {"role", "user"},
                               {"messageId", randomId()},
                               {"taskId", taskId},
                               {"contextId", contextId},
                               {"parts", json::array({json{{"kind", "data"},
                                                           {"data", json{{"skill", skill},
                                                                         {"params", inner}}}}})}}}}}};
    const std::string topic = taskTopic(agentAddress, taskId);
    if (!port_.send || !port_.send(topic, request.dump())) {
        // Nothing is paid for a request that never left the node. The task is
        // marked failed rather than left dangling in `submitted`, where it
        // would count as active forever.
        std::string ignored;
        tasks_.advance(taskId, TaskState::Failed, "the request could not be delivered", ignored);
        return fail("the task request could not be delivered to " + agentAddress);
    }

    std::string settlementTx;
    if (price > 0) {
        if (!port_.pay) {
            std::string ignored;
            tasks_.advance(taskId, TaskState::Failed, "no payment path", ignored);
            return fail("the task costs " + std::to_string(price) +
                        " LEZ and no payment path is configured");
        }
        settlementTx = port_.pay(payAccount, price);
        if (settlementTx.empty()) {
            // The request is out and the money is not. Reported as a failure
            // rather than retried silently: a second attempt would risk paying
            // twice for one task.
            std::string ignored;
            tasks_.advance(taskId, TaskState::Failed, "the payment did not settle", ignored);
            return fail("the request was delivered but the payment of " +
                        std::to_string(price) + " LEZ did not settle");
        }
        if (!tasks_.recordPayment(taskId, price, payAccount, settlementTx, err)) return fail(err);
    }

    // Deliberately still `submitted`. The peer has been asked; nothing has been
    // seen of it starting. `working` arrives as a status update or not at all.
    return done(json{{"task_id", taskId},
                     {"context_id", contextId},
                     {"state", taskStateName(TaskState::Submitted)},
                     {"skill", skill},
                     {"topic", topic},
                     {"price", price},
                     {"settlement_tx", settlementTx}});
}

// ---------------------------------------------------------------------------
// agent.subscribe
// ---------------------------------------------------------------------------

std::string SubscribeSkill::parameterSchema() const
{
    return R"({"type":"object","required":["agent_address","task_id"],)"
           R"("properties":{"agent_address":{"type":"string"},"task_id":{"type":"string"}}})";
}

std::string SubscribeSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string agentAddress, taskId;
    if (!field(p, "agent_address", agentAddress, err)) return fail(err);
    if (!field(p, "task_id", taskId, err)) return fail(err);

    TaskStore::Task task;
    if (!tasks_.find(taskId, task)) return fail("no task " + taskId);
    if (task.agent != agentAddress) {
        return fail("task " + taskId + " is with " + task.agent + ", not " + agentAddress);
    }
    if (isTerminalState(task.state)) {
        return fail("task " + taskId + " is " + taskStateName(task.state) +
                    "; there are no further updates to stream");
    }
    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    const std::string topic = taskTopic(agentAddress, taskId);
    if (!port_.subscribe || !port_.subscribe(topic)) {
        return fail("delivery refused the subscription to " + topic);
    }
    if (!tasks_.markSubscribed(taskId, err)) return fail(err);
    return done(json{{"task_id", taskId},
                     {"topic", topic},
                     {"state", taskStateName(task.state)},
                     {"already", task.subscribed}});
}

// ---------------------------------------------------------------------------
// agent.cancel
// ---------------------------------------------------------------------------

std::string CancelSkill::parameterSchema() const
{
    return R"({"type":"object","required":["agent_address","task_id"],)"
           R"("properties":{"agent_address":{"type":"string"},"task_id":{"type":"string"}}})";
}

std::string CancelSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string agentAddress, taskId;
    if (!field(p, "agent_address", agentAddress, err)) return fail(err);
    if (!field(p, "task_id", taskId, err)) return fail(err);

    TaskStore::Task task;
    if (!tasks_.find(taskId, task)) return fail("no task " + taskId);
    if (task.agent != agentAddress) {
        return fail("task " + taskId + " is with " + task.agent + ", not " + agentAddress);
    }
    if (isTerminalState(task.state)) {
        return fail("task " + taskId + " is already " + taskStateName(task.state) +
                    " and cannot be canceled");
    }
    // A cancel that never leaves the node is not a cancel: the peer keeps
    // working and keeps the money. Refuse while the transport is down rather
    // than marking the task canceled locally.
    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    const json request = json{{"jsonrpc", "2.0"},
                              {"id", randomId()},
                              {"method", "tasks/cancel"},
                              {"params", json{{"id", taskId}}}};
    if (!port_.send || !port_.send(taskTopic(agentAddress, taskId), request.dump())) {
        return fail("the cancellation could not be delivered to " + agentAddress);
    }
    if (!tasks_.advance(taskId, TaskState::Canceled, "canceled by this agent", err)) {
        return fail(err);
    }

    json result{{"task_id", taskId}, {"state", taskStateName(TaskState::Canceled)}};
    if (task.pricePaid > 0) {
        json refund{{"amount", task.pricePaid}, {"paid_into", task.payAccount}};
        // Never a refund without a transaction to point at. `a2a-task.sh`
        // refuses to record a settlement it cannot see on chain; a refund gets
        // the same treatment.
        const std::string tx = port_.refund ? port_.refund(task.payAccount, task.pricePaid)
                                            : std::string{};
        if (tx.empty()) {
            refund["pending"] = true;
            refund["reason"] = port_.refund ? "the refund did not settle"
                                            : "no refund path is configured";
        } else {
            refund["pending"] = false;
            refund["tx"] = tx;
        }
        result["refund"] = std::move(refund);
    } else {
        result["refund"] = json{{"amount", 0}, {"pending", false}};
    }
    return done(result);
}

// ---------------------------------------------------------------------------
// meta.status
// ---------------------------------------------------------------------------

std::string StatusSkill::parameterSchema() const
{
    return R"({"type":"object","required":[],"properties":{}})";
}

std::string StatusSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    if (!paramsJson.empty()) {
        parse(paramsJson, err);
        if (!err.empty()) return fail(err);
    }

    const auto call = [](const std::function<std::string()> &f) {
        return f ? f() : std::string{};
    };
    const bool configured = port_.configured && port_.configured();
    const bool started = port_.started && port_.started();

    json out;
    out["configured"] = configured;
    out["started"] = started;

    // What the agent is bound to. Null rather than "" when there is nothing:
    // an empty string reads like a value that was set to nothing.
    const auto orNull = [](const std::string &s) {
        return s.empty() ? json(nullptr) : json(s);
    };
    out["owner"] = orNull(call(port_.ownerAddress));
    out["policy_hash"] = orNull(call(port_.policyHash));
    out["lez_account"] = orNull(call(port_.lezAccount));

    // An unreachable chain is reported as unknown, never as zero. An agent that
    // says it holds nothing when it cannot see its own account has told its
    // owner a falsehood at the moment it matters most.
    const std::string balance = call(port_.balance);
    if (balance.empty()) {
        out["balance"] = nullptr;
        out["balance_error"] = "the balance could not be read from the chain";
    } else {
        out["balance"] = balance;
    }

    const std::string storage = call(port_.storage);
    if (storage.empty()) {
        out["storage"] = nullptr;
    } else {
        auto parsed = json::parse(storage, nullptr, false);
        out["storage"] = parsed.is_discarded() ? json(nullptr) : parsed;
        if (parsed.is_discarded()) {
            out["storage_error"] = "the storage node returned something that is not JSON";
        }
    }

    const auto active = tasks_.active();
    json byState = json::object();
    json ids = json::array();
    for (const auto &t : active) {
        const std::string state = taskStateName(t.state);
        byState[state] = byState.value(state, std::uint64_t{0}) + 1;
        ids.push_back(json{{"id", t.id},
                           {"agent", t.agent},
                           {"skill", t.skill},
                           {"state", state},
                           {"price_paid", t.pricePaid},
                           {"subscribed", t.subscribed}});
    }
    const std::size_t total = tasks_.size();
    out["tasks"] = json{{"total", total},
                        {"active", active.size()},
                        {"terminal", total - active.size()},
                        {"by_state", byState},
                        {"open", ids}};
    return done(out);
}

// ---------------------------------------------------------------------------
// meta.configure
// ---------------------------------------------------------------------------

std::string ConfigureSkill::parameterSchema() const
{
    return R"({"type":"object","required":["key","value"],)"
           R"("properties":{"key":{"type":"string","enum":["owner_address","policy_hash",)"
           R"("per_tx","per_period","period_blocks","price_per_task","discovery_topic",)"
           R"("approval_timeout_blocks"]},"value":{"type":"string"}}})";
}

std::string ConfigureSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string key, value;
    if (!field(p, "key", key, err)) return fail(err);
    // The value is required but may legitimately be "0", not "".
    if (!p.contains("value") || !p["value"].is_string()) {
        return fail("missing or non-string 'value'");
    }
    value = p["value"].get<std::string>();

    // The keys that are anchored on chain rather than held here. Accepting one
    // updates the local mirror — what the agent asks the owner about — and
    // changes nothing about what the chain will let it spend, because those
    // numbers are in the PDA seed of the policy account.
    const bool anchored = key == "per_tx" || key == "per_period" || key == "period_blocks";

    if (key == "owner_address" || key == "discovery_topic") {
        if (value.empty()) return fail("'" + key + "' cannot be empty");
    } else if (key == "policy_hash") {
        if (!isLowerHex64(value)) {
            return fail("'policy_hash' must be 32 bytes as 64 lower-case hex characters");
        }
    } else if (anchored || key == "price_per_task" || key == "approval_timeout_blocks") {
        if (!isDecimal(value)) {
            return fail("'" + key + "' must be a non-negative decimal integer");
        }
    } else {
        // Refuse rather than store: a mistyped key that is accepted silently
        // leaves the owner believing a limit was set.
        return fail("'" + key +
                    "' is not a configurable key. Known keys: owner_address, policy_hash, "
                    "per_tx, per_period, period_blocks, price_per_task, discovery_topic, "
                    "approval_timeout_blocks");
    }

    if (!port_.set || !port_.set(key, value)) {
        return fail("the module refused to set '" + key + "'");
    }

    json out{{"key", key}, {"value", value}, {"effective", !anchored}};
    if (anchored) {
        out["note"] = "the spending envelope is anchored on chain: this updates the local "
                      "mirror only, and takes effect when the policy account is re-anchored";
    }
    if (port_.get) {
        const std::string readBack = port_.get(key);
        if (!readBack.empty()) out["stored"] = readBack;
    }
    return done(out);
}

} // namespace logos::agent
