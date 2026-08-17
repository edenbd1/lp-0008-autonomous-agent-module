// Does the *module* survive a restart, and does it survive one honestly?
//
// `task_persistence_test.cpp` already exercises `TaskPersistence` — the file
// format, the atomic write, the payment journal, every way a snapshot can be
// unreadable. All 121 of those assertions passed for months against a class the
// module never constructed. Measured, on this module, before the code this file
// covers existed:
//
//     agent.task           -> ok, task-probe-1 opened
//     meta.status  tasks   -> {"total":1,"active":1}
//     persistence dir      -> <empty>
//     -- module destroyed and rebuilt against the same directory --
//     meta.status  tasks   -> {"total":0,"active":0}
//
// No error anywhere, because an agent that lost everything and an agent that
// never had anything look exactly alike from outside. That is the gap this
// suite is written against, and every check here is at the level the prize
// criterion is written at: the module, driven through `configure`/`start`/
// `invoke`, with a real directory on a real filesystem.
//
// The second half covers the other Reliability criterion, which lives on the
// same path for the same reason: "above-threshold transactions that fail to
// reach the owner for approval are not executed — the agent retries
// notification before timing out and reports the failure". `wallet_skills_test`
// proves the retry discipline against fake ports; what is proven here is that
// the *module* wires it, that the owner channel a loaded plugin actually has —
// an event out, `approveSpend` back — carries an answer, and that a spend
// nobody answers reaches no wallet.
//
// WHAT A PASS MEANS, since the assertions cut both ways: a pass means state was
// KEPT across the restart, that an unreadable snapshot REFUSED the start rather
// than replacing it with an empty one, and that no unapproved spend reached the
// wallet. An implementation that quietly started empty would satisfy "it
// started"; the checks below are written so that it does not satisfy them.
//
// The mutations run against this suite are listed at the bottom.

#include "../src/agent_module_plugin.h"
#include "../src/spend_marker.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static json parsed(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

static bool mentions(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// A real base58 account id, and one that survives `taskTopic`'s identifier
/// check — the peer id is spliced into a content topic, so a `/` in it names a
/// different topic entirely.
static const std::string kPeer = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";
static const std::string kPayee = "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb72Ntu8bT";
static const std::string kPolicy(64, 'a');

/// A directory of this run's own, removed and recreated so a stale snapshot
/// from a previous run cannot make a check pass. The tests below open modules
/// against subdirectories of it, one per scenario, because `TaskPersistence` is
/// single-writer per path by design.
static std::string root()
{
    const char *tmp = std::getenv("TMPDIR");
    static const std::string dir =
        std::string(tmp && *tmp ? tmp : "/tmp") + "/lp0008-module-recovery-" +
        std::to_string(static_cast<long long>(getpid()));
    return dir;
}

static std::string scenarioDir(const char *name)
{
    ::mkdir(root().c_str(), 0700);
    const std::string dir = root() + "/" + name;
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

static std::string snapshotPath(const std::string &dir) { return dir + "/tasks.json"; }

static bool fileExists(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

/// The inode the snapshot currently lives in, or 0.
///
/// Used instead of comparing the bytes, and the difference caught a check that
/// could not fail: a save that rewrites *identical* content leaves the file
/// byte-for-byte the same, so "did it write again?" is invisible to a content
/// comparison. `TaskPersistence` writes a temp file and renames it over the
/// target, so every real save installs a new inode — which is the write itself,
/// not a proxy for it, and has none of the timestamp-resolution problems of
/// mtime.
static unsigned long long snapshotInode(const std::string &path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<unsigned long long>(st.st_ino) : 0ULL;
}

static std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// A task port that behaves: the transport is up and carries everything. What
/// varies between scenarios is the transport, never the store.
static TaskPort workingTransport()
{
    TaskPort port;
    port.ready = [] { return true; };
    port.send = [](const std::string &, const std::string &) { return true; };
    port.subscribe = [](const std::string &) { return true; };
    return port;
}

/// Open a task through the module, the way a caller does.
static std::string openTask(AgentModuleImpl &m, const std::string &id)
{
    return m.invoke("agent.task", json{{"agent_address", kPeer},
                                       {"skill", "storage.upload"},
                                       {"task_id", id}}
                                      .dump());
}

/// `meta.status`, reduced to the two numbers this suite argues about.
struct TaskCounts {
    int total = -1;
    int active = -1;
};

static TaskCounts counts(AgentModuleImpl &m)
{
    const json status = parsed(m.invoke("meta.status", "{}"));
    TaskCounts out;
    if (status.contains("tasks") && status["tasks"].is_object()) {
        out.total = status["tasks"].value("total", -1);
        out.active = status["tasks"].value("active", -1);
    }
    return out;
}

static json durability(AgentModuleImpl &m)
{
    const json status = parsed(m.invoke("meta.status", "{}"));
    return status.contains("durability") && status["durability"].is_object()
               ? status["durability"]
               : json::object();
}

int main()
{
    std::printf("a restart keeps pending task state\n");
    {
        const std::string dir = scenarioDir("keeps");
        // ---- first life ----------------------------------------------------
        {
            AgentModuleImpl m;
            // Exactly what the host does, and what the module's Logos Core
            // export forwards: three strings, then `onContextReady`.
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            check(m.registerBuiltinSkills(ports).success, "the module wires its built-ins");
            check(m.configure("owner-1", kPolicy).success, "and binds to an owner and a policy");
            check(m.start().success, "a first start, against a directory with no snapshot in it");

            const json first = durability(m);
            check(first.value("recovery", std::string{}) == "absent",
                  "which reports its recovery as `absent`, not as a load of nothing");
            check(first.value("path", std::string{}) == snapshotPath(dir),
                  "and names the file it will write, so an operator can look at it");

            check(parsed(openTask(m, "task-1")).value("ok", false), "a task is opened");
            check(parsed(openTask(m, "task-2")).value("ok", false), "and a second one");
            check(counts(m).active == 2, "both are pending");

            check(fileExists(snapshotPath(dir)),
                  "and the snapshot is on disk BEFORE anything restarts");
        }

        // ---- the restart ---------------------------------------------------
        //
        // A destroyed module and a fresh one over the same directory. There is
        // no in-process state left to cheat with: this is the node restart the
        // criterion names.
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            check(m.start().success, "the restarted module starts");

            const TaskCounts after = counts(m);
            check(after.total == 2 && after.active == 2,
                  "and both pending tasks are still there, which is the criterion");

            const json report = durability(m);
            check(report.value("recovery", std::string{}) == "loaded",
                  "the recovery is reported as a load");
            check(report.value("recovered_tasks", -1) == 2,
                  "and says how many tasks it found, so an empty recovery cannot pass unnoticed");

            // The recovered task is the same task, not a placeholder with the
            // right id: a restart that kept the ids and lost the terms would
            // pass a count-only check.
            const json status = parsed(m.invoke("meta.status", "{}"));
            const json open = status["tasks"]["open"];
            bool sameTerms = false;
            for (const auto &t : open) {
                if (t.value("id", std::string{}) == "task-1") {
                    sameTerms = t.value("agent", std::string{}) == kPeer &&
                                t.value("skill", std::string{}) == "storage.upload" &&
                                t.value("state", std::string{}) == "submitted";
                }
            }
            check(sameTerms, "with the peer, the skill and the state they were opened with");
        }
    }

    std::printf("\na snapshot that cannot be read does not start an empty agent\n");
    {
        const std::string dir = scenarioDir("corrupt");
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            m.start();
            openTask(m, "task-1");
        }
        const std::string good = readFile(snapshotPath(dir));
        check(!good.empty(), "there is a snapshot to damage");

        // Cut it in half. This is a process that died mid-write, and the one
        // reading of it that must never happen is "no tasks".
        writeFile(snapshotPath(dir), good.substr(0, good.size() / 2));
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            const auto started = m.start();
            check(!started.success, "a truncated snapshot REFUSES the start");
            check(mentions(started.error, "could not be recovered") &&
                      mentions(started.error, "Refusing to start with an empty task list"),
                  "and says why, in words that name the danger rather than the errno");
            check(mentions(parsed(m.invoke("agent.task", "{}")).value("error", std::string{}),
                           "not started"),
                  "and the agent is not serving: every skill call is refused as unstarted");
        }

        // The control, and it is the assertion that makes the one above mean
        // something: the same module, the same directory, the file put back.
        writeFile(snapshotPath(dir), good);
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            check(m.start().success, "the undamaged snapshot starts perfectly well");
            check(counts(m).total == 1, "with the task that was in it");
        }

        // Not ours. A file that happens to be JSON is not a task list, and
        // parsing one into an empty store is the same failure wearing a
        // different hat.
        writeFile(snapshotPath(dir), R"({"kind":"somebody-elses-file","tasks":[]})");
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            m.configure("owner-1", kPolicy);
            check(!m.start().success, "a well-formed file that is not ours refuses the start too");
        }
    }

    std::printf("\na module nobody gave a directory to says so\n");
    {
        // The state every unit test in this repository runs in, and the state a
        // module loaded outside a host runs in. It must work — and it must not
        // read as a durable agent.
        AgentModuleImpl m;
        SkillPorts ports;
        ports.task = workingTransport();
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        check(m.start().success, "it starts without a persistence directory");
        check(parsed(openTask(m, "task-1")).value("ok", false), "and serves tasks");

        const json status = parsed(m.invoke("meta.status", "{}"));
        check(status.contains("durability") && status["durability"].is_null(),
              "and reports durability as null rather than claiming any");
    }

    std::printf("\na transport failure does not take the task with it\n");
    {
        // "Recovers from transient failures (network interruptions, node
        // restarts) without losing pending task state." This is the first half:
        // the network drops between opening a task and subscribing to it.
        const std::string dir = scenarioDir("network");
        bool up = true;
        {
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task.ready = [&up] { return up; };
            ports.task.send = [&up](const std::string &, const std::string &) { return up; };
            ports.task.subscribe = [&up](const std::string &) { return up; };
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            m.start();
            check(parsed(openTask(m, "task-1")).value("ok", false), "a task is opened while up");

            // The positive half FIRST. `!ok` is satisfied by a subscribe that
            // fails ALWAYS just as well as by one that fails because the node is
            // down, and nothing in this file or in skills_test.cpp ever asserted
            // that agent.subscribe succeeds — so making SubscribeSkill::invoke
            // return an unconditional failure left both suites entirely green.
            const json subscribed = parsed(m.invoke(
                "agent.subscribe", json{{"agent_address", kPeer}, {"task_id", "task-1"}}.dump()));
            check(subscribed.value("ok", false),
                  "agent.subscribe succeeds while the node is up");
            up = false;
            const json failed2 = parsed(m.invoke(
                "agent.subscribe", json{{"agent_address", kPeer}, {"task_id", "task-1"}}.dump()));
            check(failed2.value("ok", true) == false, "the subscribe fails while the node is down");
            check(counts(m).active == 1, "and the task is still pending, not dropped");

            // A second task cannot be opened while the transport is down — and
            // that failure must not cost the first one either.
            openTask(m, "task-2");
            check(counts(m).active == 1, "a task that could not be sent is not left half-open");
        }
        {
            up = true;
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            m.start();
            check(counts(m).active == 1,
                  "and after the node comes back, the task the interruption interrupted is there");
        }
    }

    std::printf("\nthe checkpoint is written when the store moves, and not otherwise\n");
    {
        const std::string dir = scenarioDir("checkpoint");
        AgentModuleImpl m;
        m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
        SkillPorts ports;
        ports.task = workingTransport();
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        m.start();

        openTask(m, "task-1");
        const std::string afterFirst = readFile(snapshotPath(dir));
        const unsigned long long inodeAfterFirst = snapshotInode(snapshotPath(dir));
        check(!afterFirst.empty() && inodeAfterFirst != 0, "opening a task writes the snapshot");

        // A read-only call must not rewrite the file. An fsync per `meta.status`
        // is a real cost on the diagnostic that gets polled — and it is invisible
        // to a comparison of the contents, because a redundant save writes the
        // same bytes back. Hence the inode: a save renames a new file over the
        // old one, so the inode changing IS the write.
        m.invoke("meta.status", "{}");
        m.invoke("wallet.balance", "{}");
        check(snapshotInode(snapshotPath(dir)) == inodeAfterFirst,
              "and a call that changes nothing rewrites nothing");

        openTask(m, "task-2");
        check(snapshotInode(snapshotPath(dir)) != inodeAfterFirst
                  && readFile(snapshotPath(dir)) != afterFirst,
              "while a second task does");
    }

    std::printf("\nthe store the HOST wired is the store that gets written down\n");
    {
        // `SkillPorts::tasks` exists for exactly one deployment, and its own
        // documentation names it: "a host that has to survive a restart owns the
        // store, restores it before registration, and passes it here". So that
        // host is the one caller for whom the durability half has to be right —
        // and it was the one caller for whom it was wrong. Registration handed
        // the task skills `*ports.tasks`; the snapshot port, the recovery
        // restore and the checkpoint all named the module's own `tasks_`. The
        // module therefore compared an empty store against an empty snapshot
        // after every call, found nothing had moved, and wrote NOTHING, ever,
        // while `meta.status` — reading the same store the skills use — kept
        // reporting the pending tasks. Losing everything and having nothing look
        // alike from outside; this is that failure with the host's store in it.
        const std::string dir = scenarioDir("hoststore");
        {
            TaskStore hostStore;
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            ports.tasks = &hostStore;
            check(m.registerBuiltinSkills(ports).success,
                  "the module wires its built-ins to the store the host owns");
            check(m.configure("owner-1", kPolicy).success, "and binds to an owner");
            check(m.start().success, "and starts");

            check(parsed(openTask(m, "task-1")).value("ok", false), "a task is opened");
            check(parsed(openTask(m, "task-2")).value("ok", false), "and a second one");
            // The premise of everything below: the skills really are using the
            // host's store, so a snapshot of the module's own store would be a
            // snapshot of the wrong thing rather than of nothing in particular.
            check(hostStore.size() == 2,
                  "both land in the HOST's store, which is where the skills were pointed");

            check(fileExists(snapshotPath(dir)),
                  "and the snapshot exists at all: the store that moved is the store checkpointed");
            const std::string written = readFile(snapshotPath(dir));
            check(mentions(written, "task-1") && mentions(written, "task-2"),
                  "with both tasks in it, not an empty rendering of a store nothing uses");
        }
        {
            // The restart, with a *fresh* host store — nothing in this process
            // remembers anything. Recovery has to restore into the store the
            // skills will be reading, or the agent comes up empty while its
            // snapshot file sits there full of tasks.
            TaskStore hostStore;
            AgentModuleImpl m;
            m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
            SkillPorts ports;
            ports.task = workingTransport();
            ports.tasks = &hostStore;
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            check(m.start().success, "the restarted module starts");
            check(hostStore.size() == 2,
                  "and recovery restored into the HOST's store, not into the module's own");
            const TaskCounts after = counts(m);
            check(after.total == 2 && after.active == 2,
                  "so the agent comes back serving both pending tasks");
        }
    }

    std::printf("\na save that fails is reported to the caller who lost something\n");
    {
        // `invoke` used to swallow this: the save error went into
        // `lastSaveError_`, `meta.status` grew a `last_save_error` field, and
        // the call itself answered `ok:true` for a task whose record was never
        // written. The caller — the only party in a position to retry, to
        // refuse, or to tell a human — was the one party not told.
        const std::string dir = scenarioDir("savefail");
        AgentModuleImpl m;
        m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
        SkillPorts ports;
        ports.task = workingTransport();
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        check(m.start().success, "the module starts against an empty directory");
        check(parsed(openTask(m, "task-1")).value("ok", false),
              "a first task is opened while the disk works");
        const unsigned long long good = snapshotInode(snapshotPath(dir));
        check(good != 0, "and is on disk");

        // Break the write, and break it in a way that does not depend on who is
        // running the suite. `TaskPersistence` writes `<path>.tmp` and renames
        // it over the target; `open(2)` for writing on a DIRECTORY is EISDIR for
        // root as well, where a chmod would simply be ignored.
        check(::mkdir((snapshotPath(dir) + ".tmp").c_str(), 0700) == 0,
              "the temp file every save must go through is made impossible to write");

        const json broken = parsed(openTask(m, "task-2"));
        check(broken.value("ok", true) == false,
              "the call whose record could not be written does NOT come back ok");
        check(mentions(broken.value("error", std::string{}), "could not be written") &&
                  mentions(broken.value("error", std::string{}), "restart from here loses it"),
              "and says what was lost — the record, not the work — in those terms");
        check(broken.value("durable", true) == false,
              "with a machine-readable `durable:false` beside the prose");
        check(!broken.value("durability_error", std::string{}).empty(),
              "and the underlying reason, which is the part an operator has to act on");
        // The work DID happen, and an answer that hid that would send the caller
        // to retry a task the peer has already been sent — which for a priced
        // task is the expensive direction to be wrong in.
        check(broken.value("task_id", std::string{}) == "task-2" &&
                  broken.value("state", std::string{}) == "submitted",
              "while keeping the skill's own answer underneath, because the work happened");
        check(counts(m).active == 2, "the task really is in the store, unwritten");
        check(snapshotInode(snapshotPath(dir)) == good,
              "and the last good snapshot is untouched: a failed save destroys nothing");
        check(!durability(m).value("last_save_error", std::string{}).empty(),
              "meta.status reports it too, for the operator who arrives later");

        // A read-only call is not failed by somebody else's unwritten change.
        // It retries the write — that retry is how the state lands when the disk
        // comes back — but it has lost nothing itself, and failing the very
        // diagnostic an operator opens *because* the disk is broken helps nobody.
        check(parsed(m.invoke("meta.status", "{}")).value("ok", false),
              "a call that changes nothing still answers, on the same broken disk");
        check(parsed(m.invoke("wallet.balance", "{}")).value("ok", true) == false,
              "and an unwired skill still refuses for its OWN reason, not for the disk's");

        // Not just the first one. A second change while the disk is still broken
        // is a second thing lost, and is reported as one.
        const json brokenAgain = parsed(openTask(m, "task-3"));
        check(brokenAgain.value("ok", true) == false && brokenAgain.value("durable", true) == false,
              "a further change while the disk is still broken is reported too");

        // The control that makes all of the above mean something: put the write
        // path back, and the module goes green again by itself.
        check(::rmdir((snapshotPath(dir) + ".tmp").c_str()) == 0, "the write path is restored");
        const json fixed = parsed(openTask(m, "task-4"));
        check(fixed.value("ok", false), "and the next task is recorded and reported ok");
        check(snapshotInode(snapshotPath(dir)) != good, "the snapshot is written again");
        check(durability(m).value("last_save_error", std::string{}).empty(),
              "and the reported error is cleared rather than left standing");

        const std::string recovered = readFile(snapshotPath(dir));
        check(mentions(recovered, "task-2") && mentions(recovered, "task-3"),
              "with the changes the broken interval could not write, since the retry caught up");
    }

    std::printf("\nabove-threshold spends, and the owner who is not there\n");
    {
        // The module's own owner channel: an event out, `approveSpend` back.
        // It exists only when the module is hosted, because a `std::function`
        // cannot cross the plugin boundary and a module event can.
        AgentModuleImpl m;
        int notifications = 0;
        std::string lastRequest;
        int lastAttempt = 0;
        m.setOwnerNotifier([&](const std::string &requestJson, int attempt) {
            ++notifications;
            lastRequest = requestJson;
            lastAttempt = attempt;
            return true; // the event left the module
        });

        SkillPorts ports;
        ports.envelope.perTx = "50";
        ports.envelope.perPeriod = "500";
        ports.envelope.periodBlocks = 1000;
        int spends = 0;
        ports.wallet.spentThisPeriod = [] { return std::string("0"); };
        ports.wallet.spend = [&spends](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = std::string(64, 'b');
            return r;
        };
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        m.start();

        // A short deadline, set the way an operator sets it. This is also the
        // check that `meta.configure` is wired to something that reads it back:
        // before this change the module stored no settings at all, so the key
        // was refused.
        const json timeoutSet = parsed(m.invoke(
            "meta.configure", R"({"key":"approval_timeout_ms","value":"400"})"));
        check(timeoutSet.value("ok", false) && timeoutSet.value("effective", false),
              "the approval timeout is configurable, and reported as effective");
        check(parsed(m.invoke("meta.configure", R"({"key":"approval_resend_ms","value":"50"})"))
                  .value("ok", false),
              "so is the resend interval");

        const json held = parsed(m.invoke(
            "wallet.send", json{{"recipient", "Public/" + kPayee}, {"amount", 100}}.dump()));

        check(spends == 0, "an above-threshold spend nobody approved does not reach the wallet");
        check(held.value("submitted", true) == false && held.value("ok", true) == false,
              "and the call fails, saying nothing was submitted");
        check(held.value("outcome", std::string{}) == "owner_unreachable",
              "and names the owner as the reason");
        check(notifications > 1 && held.value("attempts", 0) == notifications,
              "the request was published more than once — a retry, not one shot");
        check(lastAttempt == notifications,
              "and each notification carries its attempt number, so a listener can tell a "
              "retry from a new request");
        // Absent on BOTH sides gives "" == "" — the assertion that two ids agree
        // was satisfied by neither existing. Removing `id` from the published
        // request and `request_id` from the reply left this green. Each side has
        // to carry one before comparing them means anything.
        check(!parsed(lastRequest).value("id", std::string{}).empty(),
              "the published request carries a correlation id");
        check(!held.value("request_id", std::string{}).empty(),
              "and the reply names one back");
        check(parsed(lastRequest).value("id", std::string{}) ==
                  held.value("request_id", std::string{}),
              "under the correlation id the reply names");
        check(parsed(lastRequest).value("amount", std::string{}) == "100" &&
                  parsed(lastRequest).value("recipient", std::string{}) == "Public/" + kPayee,
              "and the owner is told which payment they are being asked about");
    }

    std::printf("\nan owner who answers\n");
    {
        AgentModuleImpl m;
        std::string requestId;
        // The owner answers on the second notification — from inside the
        // notifier, which is exactly where a real one would: the event arrives,
        // the owner's app calls back.
        m.setOwnerNotifier([&](const std::string &requestJson, int attempt) {
            requestId = parsed(requestJson).value("id", std::string{});
            if (attempt == 2) {
                m.approveSpend(requestId, "denied");
            }
            return true;
        });

        SkillPorts ports;
        ports.envelope.perTx = "50";
        ports.envelope.perPeriod = "500";
        int spends = 0;
        ports.wallet.spentThisPeriod = [] { return std::string("0"); };
        ports.wallet.spend = [&spends](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = std::string(64, 'b');
            return r;
        };
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        m.start();
        m.invoke("meta.configure", R"({"key":"approval_timeout_ms","value":"3000"})");
        m.invoke("meta.configure", R"({"key":"approval_resend_ms","value":"30"})");

        const json answered = parsed(m.invoke(
            "wallet.send", json{{"recipient", "Public/" + kPayee}, {"amount", 100}}.dump()));
        check(answered.value("outcome", std::string{}) == "denied",
              "an answer over the module's own channel reaches the spend that is waiting");
        check(spends == 0, "and a denial does not spend");

        // The answer is bound to the request. Anything else — including an
        // approval arriving for an id no spend is waiting on — is refused, so it
        // cannot sit in the table waiting for the next spend to be minted with
        // that id.
        const auto stale = m.approveSpend(requestId, "approved");
        check(!stale.success && mentions(stale.error, "no spend is waiting"),
              "and once the spend is over, an answer for it is refused rather than stored");
        const auto unknown = m.approveSpend("spend-does-not-exist", "approved");
        check(!unknown.success, "an answer naming a request nobody made is refused");
        const auto garbled = m.approveSpend("spend-1", "yes please");
        check(!garbled.success && mentions(garbled.error, "'approved' or 'denied'"),
              "and a verdict this agent cannot read is refused, not guessed at");
    }

    std::printf("\nand an owner who approves: the spend is SUBMITTED\n");
    {
        // THE STEP AFTER THE APPROVAL, WHICH USED TO BE THE STEP THAT WAS
        // MISSING.
        //
        // `wallet.send` waited for the owner, heard `approved`, and then
        // answered that submitting it "goes through the policy program's
        // spend_approved path, which this module does not wire". Both halves of
        // the exchange were real and the payment stopped between them: the chain
        // half is on the public testnet — `approve_spend` at block 10776 and
        // `spend_approved` at 10786 — and it was produced by hand, not by this
        // module.
        //
        // The module reaches it the way it reaches every other thing a 4 MB Qt
        // plugin cannot link: a command an operator names, JSON on its stdin,
        // one line back, checked before anything believes it. A shell script
        // stands in for the operator's signer here, and it records what it was
        // handed — which is the part worth asserting, because the recipient in
        // that document came out of a call parameter.
        const std::string dir = scenarioDir("approved");
        const std::string stdinPath = dir + "/signer-stdin.json";
        const std::string argvPath = dir + "/signer-argv.txt";
        const std::string signerPath = dir + "/approved-signer.sh";
        const std::string approvedTx(64, 'd');
        writeFile(signerPath, "#!/bin/sh\nprintf '%s' \"$*\" > " + argvPath + "\ncat > " +
                                  stdinPath + "\nprintf '%s\\n' " + approvedTx + "\n");
        check(::chmod(signerPath.c_str(), 0700) == 0, "an approved-spend signer is put in place");

        AgentModuleImpl m;
        std::string requestId;
        m.setOwnerNotifier([&](const std::string &requestJson, int attempt) {
            requestId = parsed(requestJson).value("id", std::string{});
            if (attempt == 2) m.approveSpend(requestId, "approved");
            return true;
        });

        SkillPorts ports;
        ports.envelope.perTx = "50";
        ports.envelope.perPeriod = "500";
        int autonomousSpends = 0;
        ports.wallet.spentThisPeriod = [] { return std::string("0"); };
        ports.wallet.spend = [&autonomousSpends](const SpendRequest &) {
            ++autonomousSpends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = std::string(64, 'b');
            return r;
        };
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        m.start();
        m.invoke("meta.configure", R"({"key":"approval_timeout_ms","value":"3000"})");
        m.invoke("meta.configure", R"({"key":"approval_resend_ms","value":"30"})");
        // The agent's own account. The approval account's address is derived
        // from it, so a module that does not know who it is cannot present an
        // approval — which is asserted below, after this one is asserted first.
        check(parsed(m.invoke("meta.configure",
                              json{{"key", "agent_account"}, {"value", kPeer}}.dump()))
                  .value("ok", false),
              "the agent is told which account it is");
        const json signerSet = parsed(m.invoke(
            "meta.configure", json{{"key", "approved_pay_signer"}, {"value", signerPath}}.dump()));
        check(signerSet.value("ok", false),
              "and `approved_pay_signer` is a configurable key, distinct from `pay_signer`");

        const json sent = parsed(m.invoke(
            "wallet.send", json{{"recipient", "Public/" + kPayee}, {"amount", 100}}.dump()));

        check(sent.value("ok", false) && sent.value("submitted", false),
              "an above-threshold spend the owner approved is SUBMITTED by the module itself");
        check(sent.value("tx", std::string{}) == approvedTx,
              "and the reply carries the hash the approved-spend path returned");
        check(sent.value("outcome", std::string{}) == "approved",
              "reported as the approved spend it is");
        check(autonomousSpends == 0,
              "while the autonomous instruction is never touched: the chain refuses that one "
              "above the per-transaction ceiling");

        // What the signer was actually handed. `spend_approved` needs the
        // recipient, the amount, the nonce and the approval marker — and the
        // marker is derived HERE, exactly as the chain derives it, rather than
        // asked for, so the signer presents the account the owner really made.
        const json handed = parsed(readFile(stdinPath));
        check(handed.value("kind", std::string{}) == "spend_approved",
              "the signer is told which instruction this is, not left to guess");
        check(handed.value("recipient", std::string{}) == "Public/" + kPayee &&
                  handed.value("amount", std::string{}) == "100",
              "with the recipient and the amount the owner was asked about");
        check(handed.value("agent", std::string{}) == kPeer,
              "the agent whose policy account bounds it");
        const std::string handedNonce = handed.value("nonce", std::string{});
        check(!handedNonce.empty() &&
                  handedNonce.find_first_not_of("0123456789") == std::string::npos,
              "the nonce, as decimal text rather than a JSON number a reader would round");
        check(handed.value("marker_seed", std::string{}).size() == 64,
              "and the approval marker seed, derived here as 32 bytes of hex");
        check(handed.value("marker_seed", std::string{}) ==
                  approvalMarkerSeed(kPeer, "Public/" + kPayee, "100", std::stoull(handedNonce)),
              "which is the seed these exact terms derive, and not a value anybody supplied");
        check(handed.value("request_id", std::string{}) == requestId,
              "under the correlation id the owner answered");

        // Nothing on the command line. The recipient is a call parameter and a
        // quote in it would end an argument; `runConfiguredCommand` puts the
        // document on stdin for exactly that reason, and this is the assertion
        // that the approved path did not invent a second way.
        check(readFile(argvPath).empty(),
              "and the signer was invoked with NO arguments: nothing a caller supplied reaches "
              "a command line");
    }

    std::printf("\nan approved spend the module cannot submit is not reported as paid\n");
    {
        // Every refusal here is a `submitted:false` with a sentence naming what
        // was missing. None of them is a payment, and none of them falls back to
        // the autonomous instruction the chain would refuse.
        const std::string dir = scenarioDir("approved-refused");
        const std::string junkSigner = dir + "/junk-signer.sh";
        writeFile(junkSigner, "#!/bin/sh\ncat > /dev/null\necho 'error: approval already spent'\n");
        ::chmod(junkSigner.c_str(), 0700);

        const auto attempt = [&](const char *agentAccount, const char *signer) {
            AgentModuleImpl m;
            m.setOwnerNotifier([&m](const std::string &requestJson, int attempt) {
                if (attempt == 2) {
                    m.approveSpend(parsed(requestJson).value("id", std::string{}), "approved");
                }
                return true;
            });
            SkillPorts ports;
            ports.envelope.perTx = "50";
            ports.envelope.perPeriod = "500";
            ports.wallet.spentThisPeriod = [] { return std::string("0"); };
            m.registerBuiltinSkills(ports);
            m.configure("owner-1", kPolicy);
            m.start();
            m.invoke("meta.configure", R"({"key":"approval_timeout_ms","value":"3000"})");
            m.invoke("meta.configure", R"({"key":"approval_resend_ms","value":"30"})");
            if (*agentAccount) {
                m.invoke("meta.configure",
                         json{{"key", "agent_account"}, {"value", agentAccount}}.dump());
            }
            if (*signer) {
                m.invoke("meta.configure",
                         json{{"key", "approved_pay_signer"}, {"value", signer}}.dump());
            }
            return parsed(m.invoke(
                "wallet.send", json{{"recipient", "Public/" + kPayee}, {"amount", 100}}.dump()));
        };

        const json noSigner = attempt(kPeer.c_str(), "");
        check(noSigner.value("submitted", true) == false &&
                  noSigner.value("outcome", std::string{}) == "approved_not_submitted",
              "with no approved-spend signer configured, an approval submits nothing");
        check(mentions(noSigner.value("error", std::string{}), "approved_pay_signer") &&
                  mentions(noSigner.value("error", std::string{}), "pay_signer"),
              "and names the setting to make, and why it is not the autonomous one");

        const json noAccount = attempt("", "/bin/true");
        check(noAccount.value("submitted", true) == false &&
                  mentions(noAccount.value("error", std::string{}), "agent_account"),
              "an agent that does not know its own account cannot present an approval");

        const json junk = attempt(kPeer.c_str(), junkSigner.c_str());
        check(junk.value("submitted", true) == false &&
                  mentions(junk.value("error", std::string{}), "transaction hash"),
              "and a signer that printed a diagnostic instead of a hash is not a settlement");
    }

    std::printf("\nan unhosted module has no owner channel, and does not pretend to wait\n");
    {
        // No notifier: a module constructed directly, which is every unit test
        // and the `lgpd` CLI. It must refuse immediately rather than sit out a
        // two-minute timeout against an owner it never gave a way to answer.
        AgentModuleImpl m;
        SkillPorts ports;
        ports.envelope.perTx = "50";
        ports.envelope.perPeriod = "500";
        int spends = 0;
        ports.wallet.spentThisPeriod = [] { return std::string("0"); };
        ports.wallet.spend = [&spends](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = std::string(64, 'b');
            return r;
        };
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        m.start();

        const json r = parsed(m.invoke(
            "wallet.send", json{{"recipient", "Public/" + kPayee}, {"amount", 100}}.dump()));
        check(spends == 0 && r.value("submitted", true) == false,
              "with no owner channel at all, the spend is refused and not made");
        check(r.value("attempts", -1) == 0,
              "and the reply says nobody was notified, rather than implying somebody was");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "a restart, and an unanswered owner, lose nothing");
    return failures ? 1 : 0;
}

// MUTATIONS RUN AGAINST THIS SUITE
//
// Each was applied on its own to module/src/, built with the same command as
// this file, and run. Every one turns the suite red; the number is how many
// checks noticed. Recorded because a test nobody has tried to break is a test
// nobody has checked.
//
//   never construct TaskPersistence (the state before this change) ....... 29
//   checkpoint nothing after invoke ...................................... 11
//   start anyway on a snapshot that could not be read .....................  4
//   treat a failed load as a fresh start ..................................  4
//   checkpoint on every call, changed or not ..............................  1
//   wire the owner channel even when the module is not hosted .............  1
//   accept an approval for a request nobody is waiting on .................  2
//   leave the correlation id in the table after the spend .................  1
//   the durability half names `tasks_` instead of the store the skills
//     were wired to (`taskStore()` -> `tasks_`) ...........................  4
//   swallow the save error again, so `invoke` answers ok:true for work
//     whose record was never written .....................................   5
//   `wallet.send` reports the approval and submits nothing, which is what
//     it did before `WalletPort::spendApproved` existed ...................  7
//   the module leaves `ports.wallet.spendApproved` unwired, so the skill
//     can submit and has no submitter .....................................  7
//
// The first row and the last four were measured against the suite as it stands.
// The six in between were measured before the last two scenarios were added and
// are therefore floors rather than current counts: a suite that grew can only
// notice a mutation more.
//
// The last two are the approval half, and they separate cleanly: the first is a
// mutation to `wallet_skills.cpp` and takes `wallet_skills_test` down with it (9
// FAIL there); the second is a mutation to `agent_module_plugin.cpp` and leaves
// `wallet_skills_test` entirely GREEN, because that suite hands the skill its
// own submitter and cannot see a module that wires none. The composition is what
// was missing — the chain half of this path has been on the public testnet since
// block 10786 and was produced by hand — so the assertion that it is wired has
// to live here, at the module.
//
// One caveat, recorded because this list exists to admit them. "A call that
// changes nothing rewrites nothing" was written as a comparison of the file's
// *contents*, and in that form it could not fail: a redundant save writes the
// same bytes back, so the file is identical either way. It went green against
// the mutation that removes the guard, and only the mutation run found it. It
// compares the inode now — `TaskPersistence` renames a new file over the old
// one, so a changed inode is the write itself — and it catches that mutation.
//
// The full revert, rather than a surgical mutation, does not compile: this
// suite calls `setOwnerNotifier`, which did not exist. That is a red run too,
// but a weaker kind of evidence, which is why the list above exists.
