#include "agent_skills.h"

// For `isTopicIdentifier`, which is where this repository's content-topic
// grammar lives. Header-only for exactly this reason: the two files that splice
// identifiers into topics are built into separate test binaries, and a grammar
// with two implementations is a grammar with two answers.
#include "messaging_skills.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
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

/// A refusal that still carries what the caller needs to act on it — which
/// account was going to be paid, how much, and what is now waiting on the owner.
std::string fail(const std::string &why, json extra)
{
    extra["ok"] = false;
    extra["error"] = why;
    return extra.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
}

/// The message every depth refusal gives, so the limit is stated once.
std::string tooDeep(const std::string &what)
{
    return "the " + what + " nests deeper than " +
           std::to_string(kMaxJsonDepth) +
           " levels, which this module refuses to hold: copying such a document "
           "recurses once per level and overflows the stack";
}

/// Likewise for the content-topic grammar, so the rule is stated once and in
/// the words of the thing it protects.
std::string badTopicIdentifier(const char *what)
{
    return std::string("'") + what +
           "' is spliced into a Logos content topic, so it may only carry "
           "letters, digits, '-' and '_': anything else names a different topic "
           "than the one this agent means to speak on";
}

/// Parse and require a field, rather than letting nlohmann throw out of invoke()
/// and take the module down with it.
///
/// The depth bound comes first, on the raw text, and it is the one check that
/// cannot be moved later: past this point the document gets copied — into a
/// summary, into a request, into the store — and a copy of a deep enough
/// document is a stack overflow, which is a signal no `catch` can answer.
json parse(const std::string &s, std::string &err)
{
    if (!withinJsonDepth(s, kMaxJsonDepth)) {
        err = tooDeep("parameters");
        return json::object();
    }
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

/// An unsigned field that is allowed to be absent, but not to be negative, not
/// to be fractional and not to be some other type.
///
/// `is_number_unsigned()` is doing all three jobs at once, and each of them has
/// been a defect here: `value(key, 0ull)` throws on a string, converts `-1` into
/// 18446744073709551615, and truncates `1.5`.
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

bool optionalBool(const json &j, const char *key, bool &out, std::string &err)
{
    if (!j.contains(key) || j[key].is_null()) return true;
    if (!j[key].is_boolean()) {
        err = std::string("'") + key + "' must be true or false";
        return false;
    }
    out = j[key].get<bool>();
    return true;
}

/// A decimal string as a u64, saturating at the top.
///
/// The chain's amounts are u128 and a task price is a u64, so a limit larger
/// than a u64 cannot bind any price this skill can be asked to pay: saturating
/// is exact for every comparison made against it, and it removes the one way a
/// limit could wrap around into a small number. False means the text was not a
/// decimal number at all, which is *unknown* and never zero.
bool decimalToU64(const std::string &s, std::uint64_t &out)
{
    if (s.empty() || s.size() > 39) return false;
    std::uint64_t v = 0;
    bool saturated = false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (saturated) continue;
        if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) {
            saturated = true;
            continue;
        }
        v = v * 10 + d;
    }
    out = saturated ? std::numeric_limits<std::uint64_t>::max() : v;
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

bool withinJsonDepth(const std::string &text, int maxDepth)
{
    if (maxDepth < 0) return false;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            // A brace inside a string is a character, not a level. Missing this
            // would make the bound reject `{"note":"[[["}` and, far worse, would
            // let `{"a":"\""}` desynchronise the scan.
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            if (++depth > maxDepth) return false;
        } else if (c == '}' || c == ']') {
            if (depth > 0) --depth;
        }
    }
    return true;
}

std::string taskTopic(const std::string &agentAddress, const std::string &taskId)
{
    // Both halves land in the `<name>` segment of the content topic, and both
    // can be chosen by a stranger: `agent_address` is read out of a peer's card.
    // An id carrying a `/` is not a name, it is a different topic.
    if (!isTopicIdentifier(agentAddress) || !isTopicIdentifier(taskId)) return {};
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
    if (!withinJsonDepth(eventJson, kMaxJsonDepth)) {
        err = tooDeep("status update");
        return false;
    }
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
    if (!withinJsonDepth(snapshotJson, kMaxJsonDepth)) {
        err = tooDeep("snapshot");
        return false;
    }
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
        // Every remaining field is type-checked rather than read with `value()`.
        // `value()` throws — `type_error.302` — the moment a key is present with
        // the wrong type, and a snapshot is a file on disk that an attacker who
        // can write it chooses the types in. This function's contract is that it
        // *refuses* malformed input; throwing out of it is not refusing.
        bool present = false;
        if (!optionalString(e, "contextId", t.contextId, ferr) ||
            !optionalString(e, "payAccount", t.payAccount, ferr) ||
            !optionalString(e, "settlementTx", t.settlementTx, ferr) ||
            !optionalString(e, "note", t.note, ferr) ||
            !optionalBool(e, "subscribed", t.subscribed, ferr) ||
            !optionalUnsigned(e, "pricePaid", t.pricePaid, present, ferr)) {
            err = "task " + t.id + " in snapshot: " + ferr;
            return false;
        }
        // `pricePaid` earns the strictest check in this function because it is
        // the only field that becomes an amount of money without passing through
        // the chain again: `"pricePaid": -1` used to restore as
        // 18446744073709551615, after which `agent.cancel` called
        // `refund(attacker, 2^64-1)` and reported the refund as ok.
        // `optionalUnsigned` refuses the negative, the fractional and the string
        // alike; there is no conversion left to be surprised by.
        if (t.contextId.empty()) t.contextId = t.id;

        if (e.contains("history") && !e["history"].is_null()) {
            if (!e["history"].is_array()) {
                err = "task " + t.id + " in snapshot: 'history' must be an array of state names";
                return false;
            }
            for (const auto &h : e["history"]) {
                // Skipping a non-string entry silently would restore a task
                // whose history is shorter than the one that was saved, which is
                // a quiet rewrite of what happened to it.
                if (!h.is_string() || h.get<std::string>().empty()) {
                    err = "task " + t.id +
                          " in snapshot: every entry in 'history' must be a non-empty state name";
                    return false;
                }
                TaskState ignored = TaskState::Unknown;
                if (!taskStateFromName(h.get<std::string>(), ignored)) {
                    err = "task " + t.id + " in snapshot: '" + h.get<std::string>() +
                          "' in 'history' is not an A2A task state";
                    return false;
                }
                t.history.push_back(h.get<std::string>());
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
        // Typed whenever it is present, not only when a price is present too.
        //
        // This check used to live inside the `pricePerTask > 0` branch below, so
        // a card carrying `"paymentAccount": 42` — or null, or `[]`, or `{}`, or
        // `true` — and no price was *valid*. `agent.discover` accepted it and
        // republished it; `agent.task` then read the same field with `value()`
        // and threw `type_error.302` straight out of `invoke()`, breaking the
        // contract in `agent_module_interface.h`. One validator saying yes while
        // the other says "exception" is the defect: the type check belongs to
        // the field, not to the branch that happens to read it.
        if (x.contains("paymentAccount") &&
            (!x["paymentAccount"].is_string() ||
             x["paymentAccount"].get<std::string>().empty())) {
            return "'x-logos.paymentAccount' must be a non-empty string";
        }
        if (x.contains("pricePerTask")) {
            if (!x["pricePerTask"].is_number_unsigned()) {
                return "'x-logos.pricePerTask' must be a non-negative integer";
            }
            if (x["pricePerTask"].get<std::uint64_t>() > 0 && !x.contains("paymentAccount")) {
                return "the card charges a price but names no 'x-logos.paymentAccount' "
                       "to pay it into";
            }
        }
    }
    return {};
}

/// The two fields `agent.task` reads out of a validated card, read the way a
/// validated card guarantees them. Belt and braces: `validateCard` now types
/// both, and this cannot throw even if that check is one day loosened again.
std::uint64_t cardPrice(const json &x)
{
    if (!x.is_object() || !x.contains("pricePerTask") || !x["pricePerTask"].is_number_unsigned()) {
        return 0;
    }
    return x["pricePerTask"].get<std::uint64_t>();
}

std::string cardString(const json &x, const char *key)
{
    if (!x.is_object() || !x.contains(key) || !x[key].is_string()) return {};
    return x[key].get<std::string>();
}

/// Why this task payment is outside the owner's envelope, or empty when it is
/// inside it.
///
/// The mirror of `wallet_skills.cpp`'s check, deliberately: two spending paths
/// out of one agent that answer differently would make the smaller ceiling
/// decorative. Every unknown is outside — an agent that cannot say what its
/// limits are, or what it has already moved this period, has not shown that a
/// payment falls inside them, and "not shown" is the same as "no" when the next
/// step is to sign a transfer.
std::string outsideEnvelope(const TaskPort &port, std::uint64_t price)
{
    const auto call = [](const std::function<std::string()> &f) {
        return f ? f() : std::string{};
    };
    std::uint64_t perTx = 0, perPeriod = 0, spent = 0;
    if (!decimalToU64(call(port.perTxLimit), perTx) ||
        !decimalToU64(call(port.perPeriodLimit), perPeriod)) {
        return "the anchored envelope is not known to this process, so nothing can be shown to "
               "fall inside it";
    }
    if (!decimalToU64(call(port.spentThisPeriod), spent)) {
        return "the period total is unknown, so this payment cannot be shown to be inside the "
               "envelope";
    }
    if (price > perTx) {
        return "over the per-transaction limit of " + std::to_string(perTx) + " LEZ";
    }
    // Written as a subtraction against the limit rather than as `spent + price`,
    // which is the addition that wraps: a period total near 2^64 would otherwise
    // add its way back down to something that looks affordable.
    if (spent > perPeriod || price > perPeriod - spent) {
        return "over the per-period limit of " + std::to_string(perPeriod) + " LEZ, of which " +
               std::to_string(spent) + " is already spent";
    }
    return {};
}

} // namespace

std::string validateAgentCard(const std::string &cardJson)
{
    // Before the parse, for the reason `kMaxJsonDepth` gives: the caller of this
    // function is about to keep whatever it just validated.
    if (!withinJsonDepth(cardJson, kMaxJsonDepth)) return tooDeep("card");
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
        // A registered skill's parameter schema is third-party text and it is
        // copied into the card below, so it is bounded like everything else that
        // gets copied.
        if (!withinJsonDepth(skillsJson, kMaxJsonDepth)) {
            return fail(tooDeep("skill registry listing"));
        }
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
                    const std::string schemaText = s["parameters"].get<std::string>();
                    if (!withinJsonDepth(schemaText, kMaxJsonDepth)) {
                        return fail(tooDeep("parameter schema of skill '" + id + "'"));
                    }
                    auto schema = json::parse(schemaText, nullptr, false);
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
        // First, and on the text: a card that is accepted is copied into the
        // summary below, and the copy is what overflows the stack. Note that the
        // *rejected* path was always safe — it keeps a reason string and drops
        // the document — so this bound is what makes the accepted path safe too.
        if (!withinJsonDepth(doc, kMaxJsonDepth)) {
            rejected.push_back(json{{"index", at}, {"reason", tooDeep("card")}});
            continue;
        }
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
            summary["price"] = cardPrice(x);
            const std::string lez = cardString(x, "lezAccount");
            const std::string pay = cardString(x, "paymentAccount");
            if (!lez.empty()) summary["lez_account"] = lez;
            if (!pay.empty()) summary["pay_account"] = pay;
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
    // Checked here, not only where the topic is built. `agent_address` is
    // routinely copied out of a peer's Agent Card, so a stranger who can get a
    // card in front of this agent otherwise chooses the content topic it
    // broadcasts on.
    if (!isTopicIdentifier(agentAddress)) return fail(badTopicIdentifier("agent_address"));
    std::string taskId, contextId, skill, message;
    if (!optionalString(p, "task_id", taskId, err)) return fail(err);
    if (!optionalString(p, "context_id", contextId, err)) return fail(err);
    if (!optionalString(p, "skill", skill, err)) return fail(err);
    if (!optionalString(p, "message", message, err)) return fail(err);
    if (!taskId.empty() && !isTopicIdentifier(taskId)) return fail(badTopicIdentifier("task_id"));
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
        const std::string continuationTopic = taskTopic(agentAddress, taskId);
        if (continuationTopic.empty()) return fail(badTopicIdentifier("agent_address"));
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
        if (!port_.send || !port_.send(continuationTopic, request.dump())) {
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
            const std::string lez = cardString(x, "lezAccount");
            if (!lez.empty() && lez != agentAddress) {
                return fail("the card belongs to " + lez + ", not to " + agentAddress);
            }
            // Both read through the typed accessors rather than `value()`. A
            // card whose `paymentAccount` is a number reached `value()` here and
            // threw `type_error.302` out of `invoke()`; `validateCard` now
            // refuses that card, and these two make the refusal impossible to
            // reintroduce by loosening the validator.
            price = cardPrice(x);
            payAccount = cardString(x, "paymentAccount");
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

    // THE OWNER'S ENVELOPE, WHICH `max_price` IS NOT.
    //
    // `max_price` is a ceiling this *call* chose, and it is optional — so a call
    // that omits it used to mean "pay whatever the peer's card says", into
    // whatever account the peer's card names. That is how a card advertising
    // 18446744073709551615 LEZ payable to `Public/ATTACKER` was paid in full.
    // The envelope below is the owner's, it is not optional, and it is checked
    // whether or not the caller thought to name a ceiling.
    //
    // Before the task is created and before anything is sent: a request that is
    // on the wire with its payment held for the owner is a task the peer
    // believes it is owed for.
    if (price > 0) {
        const std::string held = outsideEnvelope(port_, price);
        if (!held.empty()) {
            const json base{{"submitted", false},
                            {"price", price},
                            {"pay_account", payAccount},
                            {"skill", skill},
                            {"agent_address", agentAddress},
                            {"reason", held}};
            if (!port_.requestApproval) {
                json e = base;
                e["outcome"] = "owner_unreachable";
                return fail("this task's price is outside the owner's envelope and there is no "
                            "owner channel to ask for approval, so nothing was sent or paid",
                            e);
            }
            const json ask{{"kind", "task-payment-approval-request"},
                           {"agent", agentAddress},
                           {"skill", skill},
                           {"recipient", payAccount},
                           {"amount", std::to_string(price)},
                           {"reason", held}};
            if (!port_.requestApproval(ask.dump())) {
                // Terminal, not a fallback. The prize is explicit that an
                // above-threshold payment which fails to reach the owner is not
                // executed, and `wallet.send` answers the same way.
                json e = base;
                e["outcome"] = "owner_unreachable";
                return fail("the owner could not be reached, so the task was not opened and "
                            "nothing was paid",
                            e);
            }
            json out = base;
            out["outcome"] = "awaiting_owner_approval";
            return done(out);
        }
    }

    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    if (taskId.empty()) taskId = randomId();
    if (contextId.empty()) contextId = randomId();
    // The topic is built before the task is opened, so an id that cannot be
    // named on the wire does not leave a task behind that nothing can address.
    const std::string topic = taskTopic(agentAddress, taskId);
    if (topic.empty()) return fail(badTopicIdentifier("agent_address"));
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
    if (topic.empty()) return fail(badTopicIdentifier("agent_address"));
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

    const std::string topic = taskTopic(agentAddress, taskId);
    if (topic.empty()) return fail(badTopicIdentifier("agent_address"));

    const json request = json{{"jsonrpc", "2.0"},
                              {"id", randomId()},
                              {"method", "tasks/cancel"},
                              {"params", json{{"id", taskId}}}};
    if (!port_.send || !port_.send(topic, request.dump())) {
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
    } else if (!withinJsonDepth(storage, kMaxJsonDepth)) {
        // Reported, not thrown away silently, and never copied: `meta.status` is
        // the diagnostic an operator reaches for when something is already
        // wrong, so it is the last skill that may take the process down.
        out["storage"] = nullptr;
        out["storage_error"] = tooDeep("storage node's answer");
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
