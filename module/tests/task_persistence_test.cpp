// What a restart must not lose, and must not corrupt.
//
// The gap this covers: `AgentModuleImpl` holds a `TaskStore` in memory and
// nothing else. `TaskStore` can render itself and be replaced from that
// rendering, but nobody was writing the rendering anywhere, so every pending
// task died with the process — which the prize counts as a Reliability failure
// and which, for a task whose payment already settled, is worse than a lost
// task: recovery that starts from an empty store will happily pay again, and a
// LEZ settlement does not come back.
//
// Everything below runs against fakes: an in-memory disk that can be told to
// fail at each step of the atomic write, and a task store that can be told to
// refuse a restore. No node, no filesystem, no chain — none of which are needed
// to prove that a truncated file is refused rather than read as zero tasks, or
// that a task with a settlement on record is never paid a second time. Two
// tests do touch the real filesystem, because "the directory is unwritable" and
// "the temp file was renamed, not written in place" are claims about POSIX and
// not about a fake.
//
// Build:
//   INC=/opt/homebrew/opt/nlohmann-json/include
//   clang++ -std=c++17 -Wall -Wextra -I"$INC" \
//     module/tests/task_persistence_test.cpp module/src/task_persistence.cpp -o tpt && ./tpt
//
// It links only `task_persistence.cpp` on purpose. The persistence layer
// reaches `TaskStore` through two `std::function`s rather than by linking the
// task engine, so this suite stays green — or red — on its own merits while
// seven agents edit `agent_skills.cpp`.
#include "../src/task_persistence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static bool mentions(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// Read one field out of a JSON array of tasks without ever indexing past the
/// end. A failed assertion has to stay a failed assertion: an earlier version of
/// this file indexed straight into the parsed array, and a mutant that emptied
/// the store took the whole binary down with a segfault instead of printing
/// which check it broke.
static std::string fieldAt(const std::string &arrayJson, std::size_t i, const char *key)
{
    const auto j = json::parse(arrayJson, nullptr, false);
    if (j.is_discarded() || !j.is_array() || i >= j.size()) return "";
    const json &e = j[i];
    if (!e.is_object() || !e.contains(key)) return "";
    return e[key].is_string() ? e[key].get<std::string>() : e[key].dump();
}

static std::size_t countOf(const std::string &arrayJson)
{
    const auto j = json::parse(arrayJson, nullptr, false);
    return j.is_discarded() || !j.is_array() ? 0 : j.size();
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

namespace {

/// An in-memory disk that can fail at each step of the atomic write
/// independently, and that counts the steps, so a test can assert not only
/// "the save failed" but "the save failed and did not touch the target".
struct FakeDisk {
    std::map<std::string, std::string> files;

    bool failWrite = false;
    bool failRename = false;
    bool failSyncParent = false;
    /// The file is there and cannot be read — a permission error, not an
    /// absence. The distinction is the whole point.
    bool failRead = false;

    int writes = 0;
    int renames = 0;
    int syncs = 0;
    int removes = 0;

    std::string lastWrite;
    std::string lastRenameFrom;
    std::string lastRenameTo;

    SnapshotFilePort port()
    {
        SnapshotFilePort p;
        p.read = [this](const std::string &path, std::string &out, bool &absent,
                        std::string &err) {
            absent = false;
            const auto it = files.find(path);
            if (it == files.end()) {
                absent = true;
                return false;
            }
            if (failRead) {
                err = "permission denied";
                return false;
            }
            out = it->second;
            return true;
        };
        p.writeSync = [this](const std::string &path, const std::string &bytes,
                             std::string &err) {
            ++writes;
            lastWrite = path;
            if (failWrite) {
                err = "no space left on device";
                return false;
            }
            files[path] = bytes;
            return true;
        };
        p.rename = [this](const std::string &from, const std::string &to, std::string &err) {
            ++renames;
            lastRenameFrom = from;
            lastRenameTo = to;
            if (failRename) {
                err = "cross-device link";
                return false;
            }
            const auto it = files.find(from);
            if (it == files.end()) {
                err = "no such file";
                return false;
            }
            if (from == to) return true; // POSIX: renaming a file onto itself does nothing
            files[to] = it->second;
            files.erase(it);
            return true;
        };
        p.syncParent = [this](const std::string &, std::string &err) {
            ++syncs;
            if (failSyncParent) {
                err = "input/output error";
                return false;
            }
            return true;
        };
        p.remove = [this](const std::string &path) {
            ++removes;
            files.erase(path);
        };
        return p;
    }
};

/// A `TaskStore` stand-in. Holds the snapshot text and nothing else: this suite
/// is about the file and the recovery decision, and the lifecycle rules are
/// `TaskStore`'s own tests' business.
struct FakeStore {
    std::string data = "[]";
    bool refuseRestore = false;
    bool throwOnSnapshot = false;
    int restores = 0;

    TaskStorePort port()
    {
        TaskStorePort p;
        p.snapshot = [this] {
            if (throwOnSnapshot) throw std::runtime_error("store is wedged");
            return data;
        };
        p.restore = [this](const std::string &j, std::string &err) {
            ++restores;
            if (refuseRestore) {
                // Like the real one: refuses and leaves itself untouched.
                err = "task 1 appears twice in the snapshot";
                return false;
            }
            data = j;
            return true;
        };
        return p;
    }
};

json aTask(const std::string &id, const std::string &state)
{
    return json{{"id", id},
                {"contextId", "ctx-" + id},
                {"agent", "z6Mkpeer"},
                {"skill", "storage.upload"},
                {"state", state},
                {"pricePaid", 0},
                {"payAccount", ""},
                {"settlementTx", ""},
                {"subscribed", false},
                {"history", json::array({"submitted"})},
                {"note", ""}};
}

json aPaidTask(const std::string &id, const std::string &state, std::uint64_t price,
               const std::string &tx)
{
    json t = aTask(id, state);
    t["pricePaid"] = price;
    t["payAccount"] = "Public/z6MkpayeE";
    t["settlementTx"] = tx;
    return t;
}

/// The same digest the writer computes, so a test can hand-build a file that is
/// wrong in exactly one way and know the checksum is not the thing that caught
/// it. Duplicated deliberately: if the format changes, this has to change too,
/// and that is the point of writing it out.
std::uint64_t fnv1a64(const std::string &bytes)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : bytes) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string digestOf(const json &tasks, const json &payments)
{
    std::string material = tasks.dump();
    material.push_back('\x1f');
    material += payments.dump();
    static const char *d = "0123456789abcdef";
    std::uint64_t v = fnv1a64(material);
    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i) {
        hex[static_cast<std::size_t>(i)] = d[v & 0xF];
        v >>= 4;
    }
    return "fnv1a64:" + hex;
}

std::string envelopeOf(const json &tasks, const json &payments = json::array(),
                       int schema = kSnapshotSchema)
{
    return json{{"schema", schema},
                {"kind", std::string(kSnapshotKind)},
                {"count", tasks.size()},
                {"tasks", tasks},
                {"payments", payments},
                {"checksum", digestOf(tasks, payments)}}
        .dump();
}

const char *kPath = "/var/lib/lp0008/tasks.json";

} // namespace

// ---------------------------------------------------------------------------

int main()
{
    std::printf("the round trip\n");
    {
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aTask("t1", "working"), aTask("t2", "input-required")}).dump();

        TaskPersistence writer(live.port(), kPath, disk.port());
        std::string err;
        check(writer.save(err) && err.empty(), "a store with two pending tasks saves");
        check(disk.files.count(kPath) == 1, "and the snapshot is at the path it was given");

        // A fresh process: a new store, a new persistence object, the same disk.
        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.ok() && r.outcome == LoadOutcome::Loaded, "and a restart loads it");
        check(r.tasks == 2 && r.active == 2, "with both pending tasks accounted for");
        check(countOf(restarted.data) == 2, "the restarted store holds them");
        check(fieldAt(restarted.data, 0, "state") == "working" &&
                  fieldAt(restarted.data, 1, "state") == "input-required",
              "each in the state it was in");
        check(fieldAt(restarted.data, 0, "contextId") == "ctx-t1" &&
                  fieldAt(restarted.data, 0, "skill") == "storage.upload",
              "with the context and skill a continuation needs");
        check(reader.recovered().size() == 2, "and recovery can report on them");
    }

    std::printf("no file is not a corrupt file, and a corrupt file is not no file\n");
    {
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "working")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Absent, "a path with no file loads as Absent");
        check(!r.ok() && r.safeToStartEmpty(),
              "which is the one case where starting with no tasks is correct");
        check(r.error.empty() && r.tasks == 0, "and it is not an error");
        check(store.restores == 0 && countOf(store.data) == 1,
              "the store is not restored over: there was nothing to restore");
    }
    {
        FakeDisk disk;
        disk.files[kPath] = "{\"schema\":1}";
        disk.failRead = true;
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed,
              "a file that exists and cannot be read is Failed");
        check(!r.safeToStartEmpty(),
              "and never Absent: a permission error is not an empty task list");
        check(mentions(r.error, "could not be read"), "the error says which it was");
    }

    std::printf("a half-written file never loads as a half-state\n");
    {
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aTask("t1", "working"), aTask("t2", "submitted")}).dump();
        TaskPersistence writer(live.port(), kPath, disk.port());
        std::string err;
        writer.save(err);

        // The process died mid-write. Only the atomic rename stops this from
        // being what is at `path`; the file is built here by hand because the
        // implementation refuses to produce it.
        const std::string whole = disk.files[kPath];
        disk.files[kPath] = whole.substr(0, whole.size() / 2);

        FakeStore restarted;
        restarted.data = json::array({aTask("survivor", "working")}).dump();
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.outcome == LoadOutcome::Failed, "a truncated snapshot is refused");
        check(!r.safeToStartEmpty() && r.tasks == 0,
              "and does not present itself as a fresh start");
        check(mentions(r.error, "truncated") || mentions(r.error, "corrupt"),
              "the error names it as truncation or corruption");
        check(restarted.restores == 0 && countOf(restarted.data) == 1 &&
                  fieldAt(restarted.data, 0, "id") == "survivor",
              "and the store is left exactly as it was");
    }
    {
        FakeDisk disk;
        disk.files[kPath] = "this is not JSON at all";
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        check(p.load().outcome == LoadOutcome::Failed, "a malformed file is refused");
    }
    {
        FakeDisk disk;
        disk.files[kPath] = json{{"schema", 1}, {"kind", "something.else"}}.dump();
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed,
              "valid JSON that is not one of our snapshots is refused");
        check(mentions(r.error, kSnapshotKind), "and the error says what it was looking for");
    }
    {
        // Corrupt but still valid JSON: one byte of a task changed. Only the
        // checksum can catch this.
        FakeDisk disk;
        json tasks = json::array({aTask("t1", "working")});
        std::string bytes = envelopeOf(tasks);
        json broken = json::parse(bytes);
        broken["tasks"][0]["agent"] = "z6Mkattacker";
        disk.files[kPath] = broken.dump();

        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed,
              "a file whose contents changed under a valid JSON shape is refused");
        check(mentions(r.error, "checksum"), "on the checksum");
        check(store.restores == 0, "and nothing reaches the store");
    }
    {
        FakeDisk disk;
        json tasks = json::array({aTask("t1", "working"), aTask("t2", "working")});
        json broken = json::parse(envelopeOf(tasks));
        broken["count"] = 5; // outside the digest, so this is its own guard
        disk.files[kPath] = broken.dump();

        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed,
              "a snapshot that miscounts its own tasks is refused");
        check(mentions(r.error, "5") && mentions(r.error, "2"), "saying both numbers");
    }
    {
        FakeDisk disk;
        json tasks = json::array({aTask("t1", "working")});
        json broken = json::parse(envelopeOf(tasks));
        broken["tasks"][0]["state"] = "in-progress"; // not an A2A state
        broken["checksum"] = digestOf(broken["tasks"], broken["payments"]);
        disk.files[kPath] = broken.dump();

        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed,
              "a task in a state A2A does not define is refused");
        check(mentions(r.error, "in-progress"), "naming the state");
        check(store.restores == 0, "before the store is asked to model it");
    }
    {
        FakeDisk disk;
        disk.files[kPath] = envelopeOf(json::array({aTask("t1", "working")}));
        FakeStore store;
        store.refuseRestore = true;
        store.data = json::array({aTask("survivor", "working")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed, "a payload the task engine refuses is Failed");
        check(mentions(r.error, "appears twice"), "and its reason is passed through");
        check(fieldAt(store.data, 0, "id") == "survivor", "with the store untouched");
    }
    {
        FakeDisk disk;
        // 16 MiB + 1 of valid-looking file.
        disk.files[kPath] = std::string(kMaxSnapshotBytes + 1, 'x');
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed, "a snapshot above the size ceiling is refused");
        check(mentions(r.error, "ceiling"), "rather than parsed");
    }

    std::printf("a snapshot from a newer build is not something to guess at\n");
    {
        FakeDisk disk;
        json tasks = json::array({aTask("t1", "working")});
        // Schema is outside the digest, so the checksum still matches and the
        // version is provably the thing that refused it.
        disk.files[kPath] = envelopeOf(tasks, json::array(), kSnapshotSchema + 1);

        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.outcome == LoadOutcome::Failed, "a future schema version is refused");
        check(mentions(r.error, "newer agent"), "as written by a newer agent");
        check(store.restores == 0,
              "not partially loaded and certainly not written back without its new fields");
    }
    {
        FakeDisk disk;
        json broken = json::parse(envelopeOf(json::array({aTask("t1", "working")})));
        broken.erase("schema");
        disk.files[kPath] = broken.dump();
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        check(p.load().outcome == LoadOutcome::Failed, "a snapshot with no schema is refused");
    }

    std::printf("a failed write does not take the last good snapshot with it\n");
    {
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "working")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        check(p.save(err), "the first save lands");
        const std::string good = disk.files[kPath];

        store.data = json::array({aTask("t1", "completed"), aTask("t2", "working")}).dump();
        disk.failWrite = true;
        const int renamesBefore = disk.renames;
        check(!p.save(err), "a save into an unwritable directory fails");
        check(mentions(err, "no space left") || mentions(err, "could not be written"),
              "and says why");
        check(disk.renames == renamesBefore, "nothing is renamed over the target");
        check(disk.files[kPath] == good, "so the previous snapshot is still there, intact");
        check(disk.files.count(p.tempPath()) == 0, "and no temp file is left behind");

        disk.failWrite = false;
        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        check(reader.load().ok() && countOf(restarted.data) == 1,
              "a restart after the failed save recovers the last good snapshot");
    }
    {
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "working")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        p.save(err);
        const std::string good = disk.files[kPath];

        disk.failRename = true;
        store.data = json::array({aTask("t2", "working")}).dump();
        check(!p.save(err), "a save whose rename fails fails");
        check(disk.files[kPath] == good, "leaving the target as it was");
        check(disk.files.count(p.tempPath()) == 0, "and cleaning up the temp file");
    }
    {
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "working")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        p.save(err);
        check(disk.writes == 1 && disk.renames == 1 && disk.syncs == 1,
              "a save is one durable write, one rename and one directory flush");
        check(disk.lastWrite == p.tempPath(),
              "the write goes to the temp path, never to the target in place");
        check(disk.lastRenameFrom == p.tempPath() && disk.lastRenameTo == kPath,
              "and the target only ever appears by rename");
        check(disk.files.count(p.tempPath()) == 0,
              "with nothing left at the temp path afterwards");

        disk.failSyncParent = true;
        check(!p.save(err) && mentions(err, "was not flushed"),
              "and a directory that will not flush is reported, not swallowed");
    }
    {
        FakeDisk disk;
        FakeStore store;
        store.throwOnSnapshot = true;
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        check(!p.save(err) && disk.writes == 0,
              "a store that throws while being snapshotted does not produce a file");
    }

    std::printf("the file holds account ids and task metadata, never keys\n");
    {
        std::string err;
        json tasks = json::array({aTask("t1", "working")});
        // A field a later TaskStore might grow. It must not reach the disk
        // merely because nobody thought to exclude it.
        tasks[0]["iskSecret"] = "MC4CAQAwBQYDK2VwBCIEIB0000000000000000000000";
        tasks[0]["ownerMnemonic"] = "abandon abandon abandon";
        const std::string projected = projectPersistableTasks(tasks.dump(), err);
        check(err.empty() && !projected.empty(), "a snapshot with unknown fields still projects");
        check(!mentions(projected, "iskSecret") && !mentions(projected, "ownerMnemonic"),
              "with the fields this build does not name dropped");
        check(!mentions(projected, "MC4CAQAwBQYDK2Vw") && !mentions(projected, "abandon"),
              "and their values gone with them");
        check(mentions(projected, "z6Mkpeer") && mentions(projected, "storage.upload"),
              "while the account id and skill recovery needs survive");

        FakeDisk disk;
        FakeStore store;
        store.data = tasks.dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        check(p.save(err), "and it saves");
        check(!mentions(disk.files[kPath], "iskSecret") &&
                  !mentions(disk.files[kPath], "abandon"),
              "with none of it on the disk");

        const auto &allow = persistableTaskFields();
        check(std::find(allow.begin(), allow.end(), "settlementTx") != allow.end() &&
                  std::find(allow.begin(), allow.end(), "state") != allow.end(),
              "the allowlist carries what recovery decides on");
    }
    {
        // `note` is the peer's `status.message`, straight off the wire.
        std::string err;
        json tasks = json::array({aTask("t1", "failed")});
        tasks[0]["note"] = std::string(kMaxNoteBytes * 4, 'A');
        const std::string projected = projectPersistableTasks(tasks.dump(), err);
        check(fieldAt(projected, 0, "note").size() == kMaxNoteBytes,
              "a peer-supplied note is cut at the ceiling");
        check(fieldAt(projected, 0, "noteTruncated") == "true",
              "and the task says it was cut rather than reading as the whole reason");
    }
    {
        std::string err;
        check(projectPersistableTasks("[{\"id\":\"t1\",\"agent\":\"a\"}]", err).empty() &&
                  mentions(err, "skill"),
              "a task missing what recovery needs is refused at write time, not at read time");
        err.clear();
        check(projectPersistableTasks(
                  "[{\"id\":\"t1\",\"agent\":\"a\",\"skill\":\"s\",\"state\":\"nope\"}]", err)
                      .empty() &&
                  mentions(err, "nope"),
              "and so is a state A2A does not define");
        err.clear();
        check(projectPersistableTasks("{}", err).empty() && !err.empty(),
              "a snapshot that is not an array of tasks is refused");
    }

    std::printf("a task in a terminal state stays terminal\n");
    {
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aTask("done", "completed"), aTask("gone", "canceled"),
                                 aTask("bad", "failed"), aTask("no", "rejected"),
                                 aTask("live", "working")})
                        .dump();
        TaskPersistence writer(live.port(), kPath, disk.port());
        std::string err;
        writer.save(err);

        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.ok() && r.tasks == 5, "all five come back");
        check(r.active == 1, "and exactly one of them is active");
        int terminal = 0;
        for (const auto &t : reader.recovered()) {
            if (t.terminal) ++terminal;
        }
        check(terminal == 4, "the other four are recovered as terminal");
        std::string why;
        check(!reader.mayPay("done", why) && mentions(why, "terminal"),
              "and recovery will not buy finished work again");
        check(reader.mayPay("live", why), "while the active one is still payable");
    }

    std::printf("a payment that already landed is not sent a second time\n");
    {
        // The plain case: the settlement was recorded before the crash.
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aPaidTask("t1", "working", 250, "8d7aba60cafe")}).dump();
        TaskPersistence writer(live.port(), kPath, disk.port());
        std::string err;
        writer.save(err);

        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.ok() && r.settledPayments == 1, "recovery counts the settled payment");
        check(reader.paymentState("t1") == PaymentRecord::Settled,
              "and the durable record says settled");
        std::string why;
        check(!reader.mayPay("t1", why), "so the payment is not sent again");
        check(mentions(why, "8d7aba60cafe") && mentions(why, "250"),
              "and the refusal names the settlement and the amount it would have re-sent");
        check(!reader.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err) &&
                  mentions(err, "pay twice"),
              "and a fresh intent for it is refused outright");
    }
    {
        // The window that a snapshot alone cannot cover: the wallet returned a
        // hash and the process died before `recordPayment` stored it. The
        // snapshot on disk says the task is unpaid. Only the write-ahead intent
        // knows better.
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aTask("t1", "submitted")}).dump();
        TaskPersistence before(live.port(), kPath, disk.port());
        std::string err;
        check(before.save(err), "the task is snapshotted before the wallet is called");
        check(before.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err),
              "the intent is journalled before the money moves");
        // ... pay() returns a hash ... and the process dies here.

        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.ok(), "the snapshot still loads");
        check(fieldAt(restarted.data, 0, "settlementTx").empty(),
              "and it shows the task as carrying no settlement");
        check(r.uncertainPayments == 1 && reader.needingReconciliation().size() == 1,
              "but recovery flags the payment as unresolved");
        std::string why;
        check(!reader.mayPay("t1", why), "and refuses to pay it again");
        check(mentions(why, "check the chain"),
              "pointing at the only place that knows whether it settled");

        check(!reader.notePaymentAbandoned("t1", "", err) && mentions(err, "evidence"),
              "closing it out requires evidence, not a shrug");
        check(reader.notePaymentAbandoned("t1", "getAccount Public/z6MkpayeE, blocks 8600-8700",
                                          err),
              "and with evidence, a human can close it");
        check(reader.mayPay("t1", why), "after which the task may be paid");
    }
    {
        // The same window on the other side: the journal was written, the
        // settlement came back, and the process died before `recordPayment`.
        FakeDisk disk;
        FakeStore live;
        live.data = json::array({aTask("t1", "submitted")}).dump();
        TaskPersistence before(live.port(), kPath, disk.port());
        std::string err;
        before.save(err);
        before.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err);
        check(before.notePaymentSettled("t1", "c45d3f24beef", err),
              "the settlement is journalled the moment the wallet returns");

        FakeStore restarted;
        TaskPersistence reader(restarted.port(), kPath, disk.port());
        const LoadReport r = reader.load();
        check(r.settledPayments == 1 && r.uncertainPayments == 0,
              "recovery reads it as settled even though the task carries no hash");
        std::string why;
        check(!reader.mayPay("t1", why) && mentions(why, "c45d3f24beef"),
              "and will not pay it again");
        for (const auto &t : reader.recovered()) {
            if (t.id == "t1") {
                check(t.payment == PaymentRecord::Settled && t.settlementTx == "c45d3f24beef",
                      "the recovered task carries the settlement the journal held");
            }
        }
    }
    {
        // A journal that claims a settlement and names no transaction is not
        // evidence of one.
        FakeDisk disk;
        json payments = json::array({json{{"taskId", "t1"},
                                          {"account", "Public/z6MkpayeE"},
                                          {"amount", 250},
                                          {"state", "settled"},
                                          {"settlementTx", ""},
                                          {"evidence", ""}}});
        disk.files[kPath] = envelopeOf(json::array({aTask("t1", "submitted")}), payments);
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        const LoadReport r = p.load();
        check(r.ok() && r.settledPayments == 0 && r.uncertainPayments == 1,
              "a settlement with no transaction is demoted to unresolved");
        std::string why;
        check(!p.mayPay("t1", why), "and still blocks a second payment");
    }
    {
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "submitted")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        p.save(err);
        p.load();

        disk.failWrite = true;
        check(!p.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err),
              "an intent that cannot be made durable is refused");
        check(mentions(err, "could not be made durable"), "and says so, so the caller does not pay");
        check(p.paymentState("t1") == PaymentRecord::None,
              "with the in-memory journal rolled back to match the disk");

        disk.failWrite = false;
        check(p.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err), "and it can be retried");

        disk.failWrite = true;
        check(!p.notePaymentSettled("t1", "c45d3f24beef", err),
              "a settlement that cannot be written is reported as unwritten");
        check(p.paymentState("t1") == PaymentRecord::Settled,
              "but is kept in memory: losing it is the double payment");
    }
    {
        // A retried intent whose save fails must not erase the intent that is
        // already on disk. That intent is the only thing standing between a
        // restart and a second payment.
        FakeDisk disk;
        FakeStore store;
        store.data = json::array({aTask("t1", "submitted")}).dump();
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string err;
        p.save(err);
        p.load();
        check(p.notePaymentIntent("t1", "Public/z6MkpayeE", 250, err),
              "a first intent is journalled and written");

        disk.failWrite = true;
        check(!p.notePaymentIntent("t1", "Public/z6Mkother", 900, err),
              "a second intent that cannot be written is refused");
        check(p.paymentState("t1") == PaymentRecord::Intended,
              "and the intent already on disk survives it");
        std::string why;
        check(!p.mayPay("t1", why) && mentions(why, "250") && mentions(why, "z6MkpayeE"),
              "with the guard still quoting what was actually written, not what was not");
    }
    {
        std::string err;
        FakeDisk disk;
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        p.load();
        check(!p.notePaymentIntent("t1", "Public/z6MkpayeE", 0, err) &&
                  mentions(err, "not a payment"),
              "a free task has no payment to journal");
        check(!p.notePaymentIntent("t1", "", 250, err) && mentions(err, "must name the account"),
              "and an intent must name where the money goes");
        check(!p.notePaymentSettled("t1", "", err) && mentions(err, "not a payment"),
              "a settlement with no transaction hash is refused");
    }

    std::printf("mayPay refuses whenever the record is unknown\n");
    {
        FakeDisk disk;
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        std::string why;
        check(!p.recoveryRan(), "a fresh persistence has not recovered");
        check(!p.mayPay("t1", why) && mentions(why, "recovery has not run"),
              "and refuses to pay anything before it has");
    }
    {
        FakeDisk disk;
        disk.files[kPath] = "{ truncated";
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        check(p.load().outcome == LoadOutcome::Failed, "after a failed load");
        std::string why;
        check(!p.mayPay("t1", why) && mentions(why, "unknown"),
              "nothing may be paid: an unreadable record is not an absent one");
    }
    {
        FakeDisk disk;
        FakeStore store;
        TaskPersistence p(store.port(), kPath, disk.port());
        check(p.load().outcome == LoadOutcome::Absent, "after a clean first run");
        std::string why;
        check(p.mayPay("brand-new", why) && why.empty(),
              "a task the record has never heard of may be paid");
    }

    std::printf("against the real filesystem\n");
    {
        char dir[] = "/tmp/lp0008-tpXXXXXX";
        const char *made = ::mkdtemp(dir);
        if (made == nullptr) {
            std::printf("  SKIPPED - no writable temp directory\n");
        } else {
            const std::string path = std::string(made) + "/tasks.json";
            FakeStore live;
            live.data = json::array({aPaidTask("t1", "working", 250, "8d7aba60cafe")}).dump();
            TaskPersistence writer(live.port(), path, posixSnapshotFilePort());
            std::string err;
            check(writer.save(err), "a real save lands");
            check(::access(path.c_str(), F_OK) == 0, "the snapshot is at the path");
            check(::access(writer.tempPath().c_str(), F_OK) != 0,
                  "and the temp file it was renamed from is gone");

            FakeStore restarted;
            TaskPersistence reader(restarted.port(), path, posixSnapshotFilePort());
            const LoadReport r = reader.load();
            check(r.ok() && r.settledPayments == 1, "and a real restart recovers it");
            std::string why;
            check(!reader.mayPay("t1", why), "with the settled payment still blocked");

            ::unlink(path.c_str());
            TaskPersistence fresh(restarted.port(), path, posixSnapshotFilePort());
            check(fresh.load().outcome == LoadOutcome::Absent,
                  "a genuinely missing file is Absent, from ENOENT and nothing else");

            if (::geteuid() == 0) {
                std::printf("  SKIPPED - running as root, which ignores directory permissions\n");
            } else {
                ::chmod(made, 0500); // read and traverse, no write
                std::string werr;
                TaskPersistence blocked(live.port(), std::string(made) + "/nested.json",
                                        posixSnapshotFilePort());
                check(!blocked.save(werr), "a save into an unwritable directory fails");
                check(mentions(werr, "could not be written"), "reporting the write, not a crash");
                check(::access((std::string(made) + "/nested.json").c_str(), F_OK) != 0,
                      "and leaves nothing behind");
                ::chmod(made, 0700);
            }
            ::unlink(path.c_str());
            ::unlink((path + ".tmp").c_str());
            ::rmdir(made);
        }
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "a restart loses nothing it was told to keep");
    return failures ? 1 : 0;
}
