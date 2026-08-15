#include "task_persistence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace logos::agent {

using nlohmann::json;

const char *const kSnapshotKind = "logos.agent.tasks";

namespace {

/// FNV-1a, 64-bit. An integrity check, not a signature: it catches a file that
/// was truncated by a dying process or corrupted on the medium, and it catches
/// nothing at all against someone who can write the file, because they can
/// recompute it. That is the whole reason no key ever goes in here.
std::uint64_t fnv1a64(const std::string &bytes)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : bytes) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(std::uint64_t v)
{
    static const char *digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[v & 0xF];
        v >>= 4;
    }
    return out;
}

/// The digest covers the two payload documents and nothing else. `count` is
/// deliberately outside it so that a corrupted count is caught by its own
/// check rather than folded into a single pass/fail.
///
/// Deterministic because `nlohmann::json` orders object keys, so the same
/// document dumps to the same bytes in the writer and in the reader.
std::string integrityDigest(const json &tasks, const json &payments)
{
    std::string material = tasks.dump();
    material.push_back('\x1f');
    material += payments.dump();
    return "fnv1a64:" + hex64(fnv1a64(material));
}

bool requiredString(const json &j, const char *key, std::string &out, std::string &err)
{
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        err = std::string("missing or empty '") + key + "'";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

std::string parentDirectory(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string systemError()
{
    return std::string(std::strerror(errno));
}

} // namespace

// ---------------------------------------------------------------------------
// Field policy
// ---------------------------------------------------------------------------

const std::vector<std::string> &persistableTaskFields()
{
    // The allowlist. A `TaskStore::Task` field that is not named here does not
    // reach the disk, which is what makes "this file never holds keys" a
    // property of the code rather than a promise about future edits upstream.
    static const std::vector<std::string> fields{
        "id",      "contextId",    "agent",      "skill",   "state", "pricePaid",
        "payAccount", "settlementTx", "subscribed", "history", "note",
    };
    return fields;
}

bool isTerminalStateName(const std::string &state)
{
    return state == "completed" || state == "canceled" || state == "failed" ||
           state == "rejected";
}

bool isKnownStateName(const std::string &state)
{
    return state == "submitted" || state == "working" || state == "input-required" ||
           state == "auth-required" || state == "unknown" || isTerminalStateName(state);
}

const char *paymentRecordName(PaymentRecord record)
{
    switch (record) {
    case PaymentRecord::None: return "none";
    case PaymentRecord::Intended: return "intended";
    case PaymentRecord::Settled: return "settled";
    case PaymentRecord::Abandoned: return "abandoned";
    }
    return "none";
}

namespace {

/// Shared by the writer and the reader so the two cannot drift: a task this
/// build would refuse to load is a task it refuses to write.
bool checkTaskShape(const json &task, std::string &id, std::string &state, std::string &err)
{
    if (!task.is_object()) {
        err = "every task must be a JSON object";
        return false;
    }
    if (!requiredString(task, "id", id, err)) return false;
    std::string agent, skill;
    if (!requiredString(task, "agent", agent, err) ||
        !requiredString(task, "skill", skill, err)) {
        err = "task " + id + ": " + err;
        return false;
    }
    if (!requiredString(task, "state", state, err)) {
        err = "task " + id + ": " + err;
        return false;
    }
    if (!isKnownStateName(state)) {
        err = "task " + id + " has state '" + state + "', which A2A does not define";
        return false;
    }
    return true;
}

} // namespace

std::string projectPersistableTasks(const std::string &snapshotJson, std::string &err)
{
    err.clear();
    auto parsed = json::parse(snapshotJson, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        err = "a task snapshot must be a JSON array of tasks";
        return {};
    }

    json out = json::array();
    std::map<std::string, bool> seen;
    for (const auto &task : parsed) {
        std::string id, state;
        if (!checkTaskShape(task, id, state, err)) return {};
        if (seen.count(id) != 0) {
            err = "task " + id + " appears twice in the snapshot";
            return {};
        }
        seen[id] = true;

        json kept = json::object();
        for (const auto &field : persistableTaskFields()) {
            if (task.contains(field)) kept[field] = task[field];
        }
        // The note is the peer's `status.message`, straight off the wire. Cut it
        // rather than commit an unbounded peer-controlled string to durable
        // storage, and say that it was cut so nobody reads the stub as the whole
        // reason a task failed.
        if (kept.contains("note") && kept["note"].is_string()) {
            const std::string note = kept["note"].get<std::string>();
            if (note.size() > kMaxNoteBytes) {
                kept["note"] = note.substr(0, kMaxNoteBytes);
                kept["noteTruncated"] = true;
            }
        }
        out.push_back(std::move(kept));
    }
    return out.dump();
}

// ---------------------------------------------------------------------------
// The real filesystem
// ---------------------------------------------------------------------------

SnapshotFilePort posixSnapshotFilePort()
{
    SnapshotFilePort port;

    port.read = [](const std::string &path, std::string &out, bool &absent,
                   std::string &err) {
        absent = false;
        out.clear();
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                absent = true;
                return false;
            }
            err = systemError();
            return false;
        }
        std::string bytes;
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno == EINTR) continue;
                err = systemError();
                ::close(fd);
                return false;
            }
            if (n == 0) break;
            bytes.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);
        out = std::move(bytes);
        return true;
    };

    port.writeSync = [](const std::string &path, const std::string &bytes, std::string &err) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            err = systemError();
            return false;
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                err = systemError();
                ::close(fd);
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        // Before the rename, always. A rename over a file whose bytes are still
        // in the page cache survives a process crash and does not survive a
        // power loss, and the second is the case a durable snapshot is for.
        if (::fsync(fd) != 0) {
            err = systemError();
            ::close(fd);
            return false;
        }
        if (::close(fd) != 0) {
            err = systemError();
            return false;
        }
        return true;
    };

    port.rename = [](const std::string &from, const std::string &to, std::string &err) {
        if (::rename(from.c_str(), to.c_str()) != 0) {
            err = systemError();
            return false;
        }
        return true;
    };

    port.syncParent = [](const std::string &path, std::string &err) {
        const std::string dir = parentDirectory(path);
        const int fd = ::open(dir.c_str(), O_RDONLY);
        if (fd < 0) {
            err = systemError();
            return false;
        }
        const bool ok = ::fsync(fd) == 0;
        if (!ok) err = systemError();
        ::close(fd);
        return ok;
    };

    port.remove = [](const std::string &path) { ::unlink(path.c_str()); };

    return port;
}

// ---------------------------------------------------------------------------
// TaskPersistence
// ---------------------------------------------------------------------------

TaskPersistence::TaskPersistence(TaskStorePort store, std::string path, SnapshotFilePort file,
                                 std::size_t maxBytes)
    : store_(std::move(store)), path_(std::move(path)), file_(std::move(file)),
      maxBytes_(maxBytes)
{
}

bool TaskPersistence::save(std::string &err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return saveLocked(err);
}

bool TaskPersistence::saveLocked(std::string &err)
{
    err.clear();
    if (!store_.snapshot) {
        err = "no task store is wired to snapshot";
        return false;
    }
    if (!file_.writeSync || !file_.rename) {
        err = "no atomic write path is wired: a snapshot needs both a durable "
              "write and a rename";
        return false;
    }

    std::string raw;
    try {
        raw = store_.snapshot();
    } catch (...) {
        err = "the task store threw while being snapshotted";
        return false;
    }

    const std::string tasksJson = projectPersistableTasks(raw, err);
    if (!err.empty()) return false;

    json tasks = json::parse(tasksJson, nullptr, false);
    if (tasks.is_discarded()) { // unreachable: we just produced it
        err = "the projected task list did not re-parse";
        return false;
    }

    json payments = json::array();
    for (const auto &entry : journal_) {
        payments.push_back(json{{"taskId", entry.first},
                                {"account", entry.second.payAccount},
                                {"amount", entry.second.amount},
                                {"state", paymentRecordName(entry.second.state)},
                                {"settlementTx", entry.second.settlementTx},
                                {"evidence", entry.second.evidence}});
    }

    const json envelope{{"schema", kSnapshotSchema},
                        {"kind", std::string(kSnapshotKind)},
                        {"count", tasks.size()},
                        {"tasks", tasks},
                        {"payments", payments},
                        {"checksum", integrityDigest(tasks, payments)}};
    const std::string bytes = envelope.dump();
    if (bytes.size() > maxBytes_) {
        err = "the snapshot is " + std::to_string(bytes.size()) + " bytes, above the " +
              std::to_string(maxBytes_) + "-byte ceiling";
        return false;
    }

    // Temp, flush, rename, flush the directory. Never the target in place: a
    // write straight to `path_` that dies halfway leaves a file that is neither
    // the old snapshot nor the new one, and the next start reads it as
    // corruption at best.
    const std::string tmp = tempPath();
    std::string ferr;
    if (!file_.writeSync(tmp, bytes, ferr)) {
        if (file_.remove) file_.remove(tmp);
        err = "the snapshot could not be written to " + tmp +
              (ferr.empty() ? "" : ": " + ferr);
        return false;
    }
    if (!file_.rename(tmp, path_, ferr)) {
        if (file_.remove) file_.remove(tmp);
        err = "the snapshot could not be renamed over " + path_ +
              (ferr.empty() ? "" : ": " + ferr);
        return false;
    }
    if (file_.syncParent && !file_.syncParent(path_, ferr)) {
        // The bytes are in place; the directory entry naming them may not be.
        // Reported rather than swallowed: the caller asked for durability.
        err = "the snapshot was renamed into place but " + parentDirectory(path_) +
              " was not flushed" + (ferr.empty() ? "" : ": " + ferr);
        return false;
    }
    return true;
}

LoadReport TaskPersistence::load()
{
    std::lock_guard<std::mutex> lock(mutex_);

    LoadReport report;
    recoveryRan_ = true;

    // Everything below sets `report.outcome` before returning, and only the
    // success path touches `recovered_`, `terminalById_`, `journal_` or the
    // store. A failed load leaves this object saying exactly what it said
    // before, which is what lets `mayPay` keep refusing.
    const auto fail = [&](const std::string &why) {
        report.outcome = LoadOutcome::Failed;
        report.error = why;
        last_ = LoadOutcome::Failed;
        return report;
    };

    if (!file_.read) return fail("no read path is wired to recover " + path_ + " from");

    std::string bytes;
    bool absent = false;
    std::string rerr;
    if (!file_.read(path_, bytes, absent, rerr)) {
        if (absent) {
            report.outcome = LoadOutcome::Absent;
            last_ = LoadOutcome::Absent;
            recovered_.clear();
            terminalById_.clear();
            journal_.clear();
            return report;
        }
        // A file that is there and unreadable is not an empty task list. This
        // is the branch that must never fall through to `Absent`.
        return fail("the snapshot at " + path_ + " exists and could not be read" +
                    (rerr.empty() ? "" : ": " + rerr));
    }

    if (bytes.size() > maxBytes_) {
        return fail("the snapshot at " + path_ + " is " + std::to_string(bytes.size()) +
                    " bytes, above the " + std::to_string(maxBytes_) + "-byte ceiling");
    }

    auto envelope = json::parse(bytes, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        // Where a truncated file lands: a JSON document cut short does not
        // parse. Refused, not read as zero tasks.
        return fail("the snapshot at " + path_ +
                    " is not a JSON object; it is truncated or corrupt");
    }

    if (!envelope.contains("kind") || !envelope["kind"].is_string() ||
        envelope["kind"].get<std::string>() != kSnapshotKind) {
        return fail("the file at " + path_ + " is not a " + kSnapshotKind + " snapshot");
    }

    if (!envelope.contains("schema") || !envelope["schema"].is_number_integer()) {
        return fail("the snapshot at " + path_ + " does not declare a schema version");
    }
    const int schema = envelope["schema"].get<int>();
    if (schema > kSnapshotSchema) {
        // Refused rather than best-effort parsed. Loading a newer file with the
        // fields this build does not know about dropped, then saving it back,
        // would silently discard them for good.
        return fail("the snapshot at " + path_ + " is schema " + std::to_string(schema) +
                    " and this build understands at most " + std::to_string(kSnapshotSchema) +
                    "; it was written by a newer agent");
    }
    if (schema < 1) {
        return fail("the snapshot at " + path_ + " declares schema " + std::to_string(schema) +
                    ", which is not a version this format ever had");
    }

    if (!envelope.contains("tasks") || !envelope["tasks"].is_array()) {
        return fail("the snapshot at " + path_ + " carries no 'tasks' array");
    }
    const json &tasks = envelope["tasks"];

    const json payments = envelope.contains("payments") && envelope["payments"].is_array()
                              ? envelope["payments"]
                              : json::array();
    if (envelope.contains("payments") && !envelope["payments"].is_array()) {
        return fail("the snapshot at " + path_ + " carries a 'payments' field that is not an array");
    }

    if (!envelope.contains("count") || !envelope["count"].is_number_unsigned()) {
        return fail("the snapshot at " + path_ + " does not declare how many tasks it holds");
    }
    if (envelope["count"].get<std::size_t>() != tasks.size()) {
        return fail("the snapshot at " + path_ + " says it holds " +
                    std::to_string(envelope["count"].get<std::size_t>()) + " tasks and carries " +
                    std::to_string(tasks.size()));
    }

    if (!envelope.contains("checksum") || !envelope["checksum"].is_string()) {
        return fail("the snapshot at " + path_ + " carries no checksum");
    }
    const std::string expected = integrityDigest(tasks, payments);
    if (envelope["checksum"].get<std::string>() != expected) {
        return fail("the snapshot at " + path_ + " does not match its checksum; it is corrupt");
    }

    // Validate before handing anything to the task engine, so a bad state name
    // is reported against this file rather than as an opaque restore failure —
    // and so the check holds whichever store is wired in.
    std::map<std::string, bool> terminal;
    std::vector<RecoveredTask> found;
    for (const auto &task : tasks) {
        std::string id, state;
        std::string terr;
        if (!checkTaskShape(task, id, state, terr)) {
            return fail("the snapshot at " + path_ + " is not loadable: " + terr);
        }
        if (terminal.count(id) != 0) {
            return fail("the snapshot at " + path_ + " holds task " + id + " twice");
        }
        RecoveredTask r;
        r.id = id;
        r.state = state;
        r.terminal = isTerminalStateName(state);
        r.contextId = task.value("contextId", id);
        r.agent = task.value("agent", std::string{});
        r.skill = task.value("skill", std::string{});
        r.pricePaid = task.value("pricePaid", std::uint64_t{0});
        r.payAccount = task.value("payAccount", std::string{});
        r.settlementTx = task.value("settlementTx", std::string{});
        terminal[id] = r.terminal;
        found.push_back(std::move(r));
    }

    std::map<std::string, JournalEntry> journal;
    for (const auto &p : payments) {
        if (!p.is_object()) {
            return fail("the snapshot at " + path_ + " holds a payment entry that is not an object");
        }
        std::string taskId, stateName, perr;
        if (!requiredString(p, "taskId", taskId, perr)) {
            return fail("the snapshot at " + path_ + " holds a payment entry with no task: " + perr);
        }
        if (!requiredString(p, "state", stateName, perr)) {
            return fail("the snapshot at " + path_ + " holds a payment entry for " + taskId +
                        " with no state");
        }
        JournalEntry e;
        e.payAccount = p.value("account", std::string{});
        e.amount = p.value("amount", std::uint64_t{0});
        e.settlementTx = p.value("settlementTx", std::string{});
        e.evidence = p.value("evidence", std::string{});
        if (stateName == "intended") {
            e.state = PaymentRecord::Intended;
        } else if (stateName == "settled") {
            e.state = PaymentRecord::Settled;
        } else if (stateName == "abandoned") {
            e.state = PaymentRecord::Abandoned;
        } else if (stateName == "none") {
            e.state = PaymentRecord::None;
        } else {
            return fail("the snapshot at " + path_ + " records payment state '" + stateName +
                        "' for task " + taskId + ", which this build does not define");
        }
        // A journal entry that claims a settlement without naming a transaction
        // is not evidence of one. Demoted to unresolved rather than trusted:
        // the failure mode of trusting it is a task that silently never gets
        // paid, and the failure mode of demoting it is a human looking at the
        // chain.
        if (e.state == PaymentRecord::Settled && e.settlementTx.empty()) {
            e.state = PaymentRecord::Intended;
        }
        journal[taskId] = std::move(e);
    }

    // Reconcile the two records of the same fact. The task's `settlementTx`
    // comes from `TaskStore::recordPayment`, which runs *after* the wallet
    // returns; the journal entry is written *before* the wallet is called. A
    // settlement in either one means the money moved.
    for (auto &r : found) {
        auto it = journal.find(r.id);
        if (!r.settlementTx.empty()) {
            if (it == journal.end()) {
                JournalEntry e;
                e.payAccount = r.payAccount;
                e.amount = r.pricePaid;
                e.state = PaymentRecord::Settled;
                e.settlementTx = r.settlementTx;
                journal[r.id] = std::move(e);
            } else if (it->second.state != PaymentRecord::Settled) {
                it->second.state = PaymentRecord::Settled;
                it->second.settlementTx = r.settlementTx;
            }
            r.payment = PaymentRecord::Settled;
        } else if (it != journal.end()) {
            r.payment = it->second.state;
            if (it->second.state == PaymentRecord::Settled) {
                r.settlementTx = it->second.settlementTx;
                r.pricePaid = r.pricePaid != 0 ? r.pricePaid : it->second.amount;
                r.payAccount = r.payAccount.empty() ? it->second.payAccount : r.payAccount;
            }
        }
    }

    // The task engine has the last word on the payload. It refuses a snapshot
    // it cannot model and leaves itself untouched when it does, so a refusal
    // here is a failed load and not a half-restored agent.
    if (!store_.restore) {
        return fail("no task store is wired to restore " + path_ + " into");
    }
    std::string serr;
    bool restored = false;
    try {
        restored = store_.restore(tasks.dump(), serr);
    } catch (...) {
        return fail("the task store threw while restoring " + path_);
    }
    if (!restored) {
        return fail("the task store refused the snapshot at " + path_ +
                    (serr.empty() ? "" : ": " + serr));
    }

    recovered_ = std::move(found);
    terminalById_ = std::move(terminal);
    journal_ = std::move(journal);

    report.outcome = LoadOutcome::Loaded;
    report.tasks = recovered_.size();
    for (const auto &r : recovered_) {
        if (!r.terminal) ++report.active;
        if (r.payment == PaymentRecord::Settled) ++report.settledPayments;
        if (r.payment == PaymentRecord::Intended) ++report.uncertainPayments;
    }
    // An intent for a task the snapshot does not carry still has to be counted:
    // it is money that may have moved for a task nobody can now see.
    for (const auto &entry : journal_) {
        if (entry.second.state != PaymentRecord::Intended) continue;
        if (terminalById_.count(entry.first) == 0) ++report.uncertainPayments;
    }
    last_ = LoadOutcome::Loaded;
    return report;
}

LoadOutcome TaskPersistence::lastOutcome() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

bool TaskPersistence::recoveryRan() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recoveryRan_;
}

std::vector<RecoveredTask> TaskPersistence::recovered() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recovered_;
}

std::vector<RecoveredTask> TaskPersistence::needingReconciliation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoveredTask> out;
    for (const auto &r : recovered_) {
        if (r.payment == PaymentRecord::Intended) out.push_back(r);
    }
    return out;
}

PaymentRecord TaskPersistence::paymentState(const std::string &taskId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = journal_.find(taskId);
    return it == journal_.end() ? PaymentRecord::None : it->second.state;
}

bool TaskPersistence::notePaymentIntent(const std::string &taskId,
                                        const std::string &payAccount, std::uint64_t amount,
                                        std::string &err)
{
    err.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskId.empty()) {
        err = "a payment intent needs a task id";
        return false;
    }
    if (amount == 0) {
        err = "a payment of zero is not a payment and has nothing to journal";
        return false;
    }
    if (payAccount.empty()) {
        err = "a payment intent must name the account the money goes to";
        return false;
    }
    const auto it = journal_.find(taskId);
    if (it != journal_.end() && it->second.state == PaymentRecord::Settled) {
        err = "task " + taskId + " already settled as " + it->second.settlementTx +
              "; a second payment would pay twice";
        return false;
    }

    const JournalEntry previous = it == journal_.end() ? JournalEntry{} : it->second;
    const bool existed = it != journal_.end();

    JournalEntry e;
    e.payAccount = payAccount;
    e.amount = amount;
    e.state = PaymentRecord::Intended;
    journal_[taskId] = e;

    // Durable before the wallet is called, or it is not an intent at all. Roll
    // the journal back if the write did not land, so memory and disk do not
    // disagree about what was promised.
    if (!saveLocked(err)) {
        if (existed) {
            journal_[taskId] = previous;
        } else {
            journal_.erase(taskId);
        }
        err = "the payment intent for task " + taskId + " could not be made durable, so it "
              "is not one: " + err;
        return false;
    }
    return true;
}

bool TaskPersistence::notePaymentSettled(const std::string &taskId,
                                         const std::string &settlementTx, std::string &err)
{
    err.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskId.empty()) {
        err = "a settlement needs a task id";
        return false;
    }
    if (settlementTx.empty()) {
        err = "a payment without a settlement transaction is not a payment";
        return false;
    }
    const auto it = journal_.find(taskId);
    const bool existed = it != journal_.end();
    const JournalEntry previous = existed ? it->second : JournalEntry{};

    JournalEntry e = previous;
    e.state = PaymentRecord::Settled;
    e.settlementTx = settlementTx;
    e.evidence.clear();
    journal_[taskId] = e;

    if (!saveLocked(err)) {
        // The money moved and the record did not. Keep the settlement in memory
        // — losing it here is the double-payment case — and say the write
        // failed so the caller can retry the save.
        err = "the settlement " + settlementTx + " for task " + taskId +
              " is recorded in memory but was not written to " + path_ + ": " + err;
        return false;
    }
    return true;
}

bool TaskPersistence::notePaymentAbandoned(const std::string &taskId,
                                           const std::string &evidence, std::string &err)
{
    err.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (taskId.empty()) {
        err = "abandoning a payment needs a task id";
        return false;
    }
    if (evidence.empty()) {
        err = "abandoning a payment intent requires evidence of what was checked on chain";
        return false;
    }
    const auto it = journal_.find(taskId);
    if (it == journal_.end()) {
        err = "no payment for task " + taskId + " to abandon";
        return false;
    }
    if (it->second.state == PaymentRecord::Settled) {
        err = "task " + taskId + " settled as " + it->second.settlementTx +
              " and a settlement cannot be abandoned";
        return false;
    }

    const JournalEntry previous = it->second;
    it->second.state = PaymentRecord::Abandoned;
    it->second.evidence = evidence;
    if (!saveLocked(err)) {
        journal_[taskId] = previous;
        err = "task " + taskId + " could not be recorded as abandoned, so it stays "
              "unresolved: " + err;
        return false;
    }
    for (auto &r : recovered_) {
        if (r.id == taskId) r.payment = PaymentRecord::Abandoned;
    }
    return true;
}

bool TaskPersistence::mayPay(const std::string &taskId, std::string &why) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mayPayLocked(taskId, why);
}

bool TaskPersistence::mayPayLocked(const std::string &taskId, std::string &why) const
{
    why.clear();
    if (!recoveryRan_) {
        why = "recovery has not run, so nothing is known about what was already paid";
        return false;
    }
    if (last_ == LoadOutcome::Failed) {
        why = "the snapshot at " + path_ +
              " could not be read, so what was already paid is unknown";
        return false;
    }

    const auto entry = journal_.find(taskId);
    if (entry != journal_.end()) {
        switch (entry->second.state) {
        case PaymentRecord::Settled:
            why = "task " + taskId + " was already paid: settlement " +
                  entry->second.settlementTx + " is on record, and paying again would send " +
                  std::to_string(entry->second.amount) + " a second time";
            return false;
        case PaymentRecord::Intended:
            why = "a payment of " + std::to_string(entry->second.amount) + " into " +
                  entry->second.payAccount + " for task " + taskId +
                  " was begun before the restart and never resolved; check the chain before "
                  "paying again";
            return false;
        case PaymentRecord::Abandoned:
        case PaymentRecord::None:
            break;
        }
    }

    const auto terminal = terminalById_.find(taskId);
    if (terminal != terminalById_.end() && terminal->second) {
        why = "task " + taskId + " recovered in a terminal state and finished work is not "
              "bought again";
        return false;
    }
    return true;
}

} // namespace logos::agent
