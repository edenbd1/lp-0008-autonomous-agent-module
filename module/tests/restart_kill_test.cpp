// A node restart that nobody was polite about: SIGKILL, and a different process
// afterwards.
//
// `module_recovery_test.cpp` covers the same criterion — "recovers from
// transient failures (network interruptions, node restarts) without losing
// pending task state" — by destroying an `AgentModuleImpl` and building another
// one over the same directory. That is a real check and it has caught real
// defects. What it cannot check is the difference between a *teardown* and a
// *kill*, and that difference is the whole of the criterion:
//
//   - a destroyed object runs `~AgentModuleImpl`, and every member's destructor
//     under it;
//   - a destroyed object leaves the process alive, with its static state, its
//     heap, its file descriptors and its page cache exactly where they were;
//   - a build that wrote the snapshot from the destructor, or from `stop()`, or
//     from an `atexit`, would pass a teardown test and lose everything on a
//     node that was killed.
//
// So this suite kills. Not `exit`, not `abort`, not a raised signal a handler
// could catch — SIGKILL, which no code in the target process can observe, delay
// or clean up after. The kill is *asserted*: `WIFSIGNALED` and `WTERMSIG ==
// SIGKILL`, so a child that quietly exited 0 cannot pass as one that was killed.
// Then a FRESH process — `execv` of this same binary, a new address space with
// nothing carried over — is pointed at the same directory and asked what it
// found.
//
// THE BOUNDARY, WHICH IS THE OTHER HALF OF THE ANSWER
//
// The module checkpoints in `invoke()`, after the skill returns. So what
// survives a kill is exactly the work of the last COMPLETED invoke, and the
// second scenario below measures that edge rather than asserting around it: a
// task that `agent.task` has already opened in the store, whose delivery is
// still hanging when the kill lands, is NOT in the snapshot afterwards. That is
// asserted in both directions — the completed tasks are there, the hung one is
// not — because a boundary nobody has measured from both sides is a guess.
//
// That loss is also the safe direction, and the suite says so rather than
// leaving a reader to wonder: the hung task never reached the wire and nothing
// was paid for it, so forgetting it costs a request that has to be made again,
// while remembering it would leave the agent holding a task no peer has heard
// of. What must not be lost is work that was reported as done, and that is
// scenario one.
//
// Build (the module's own suite line, with this file in place of skills_test):
//   clang++ -std=c++17 -Wall -Wextra -I<logos-cpp-sdk>/cpp -I<nlohmann>/include \
//     module/tests/restart_kill_test.cpp module/src/*.cpp -o restart_kill_test
//
// It re-executes itself with a role argument, so it must be run as a path the
// process can execv — which `./restart_kill_test` and any absolute path both
// are. Run it with no arguments; the roles are internal.

#include "../src/agent_module_plugin.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

namespace {

int failures = 0;

void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

json parsed(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return (j.is_discarded() || !j.is_object()) ? json::object() : j;
}

/// A real base58 account id, and one `taskTopic` accepts.
const std::string kPeer = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";
const std::string kPolicy(64, 'a');

/// Printed by a child once it has reached the state the parent is about to kill
/// it in. The parent waits for this rather than sleeping: a sleep long enough to
/// be safe is a sleep long enough to hide the thing being measured.
const char *const kReady = "READY";

// ---------------------------------------------------------------------------
// The child roles
// ---------------------------------------------------------------------------

TaskPort transportThatWorks()
{
    TaskPort port;
    port.ready = [] { return true; };
    port.send = [](const std::string &, const std::string &) { return true; };
    port.subscribe = [](const std::string &) { return true; };
    return port;
}

std::string openTask(AgentModuleImpl &m, const std::string &id)
{
    return m.invoke("agent.task", json{{"agent_address", kPeer},
                                       {"skill", "storage.upload"},
                                       {"task_id", id}}
                                      .dump());
}

void announce(const char *what)
{
    std::printf("%s\n", what);
    std::fflush(stdout);
}

/// Never returns. Deliberately not a sleep with a bound: the parent's kill is
/// what ends this process, and anything that could end it first would be a
/// second explanation for the exit status the parent asserts on.
[[noreturn]] void waitToBeKilled()
{
    for (;;) {
        ::pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

/// Open `count` tasks through completed `invoke()` calls, say so, and wait to be
/// killed. Every task here went in and came back: this is the state the
/// criterion says must survive.
[[noreturn]] int roleOpen(const std::string &dir, int count)
{
    auto *m = new AgentModuleImpl(); // never destroyed: this process is killed
    m->_logosCoreSetContext_("/modules/agent", "agent-1", dir);
    SkillPorts ports;
    ports.task = transportThatWorks();
    m->registerBuiltinSkills(ports);
    m->configure("owner-1", kPolicy);
    if (!m->start().success) {
        announce("START-FAILED");
        waitToBeKilled();
    }
    for (int i = 1; i <= count; ++i) {
        const std::string answer = openTask(*m, "task-" + std::to_string(i));
        if (!parsed(answer).value("ok", false)) {
            announce("OPEN-FAILED");
            waitToBeKilled();
        }
    }
    announce(kReady);
    waitToBeKilled();
}

/// Two completed tasks, then a third whose `invoke()` is still running when the
/// kill lands: `agent.task` opens the task in the store and *then* addresses the
/// transport, so the store has moved and this call has not returned. The
/// boundary, arranged on the real built-in path rather than on a stub.
[[noreturn]] int roleHang(const std::string &dir)
{
    auto *m = new AgentModuleImpl();
    m->_logosCoreSetContext_("/modules/agent", "agent-1", dir);
    SkillPorts ports;
    ports.task = transportThatWorks();
    // The third send never comes back — a peer that accepted the connection and
    // then stopped reading, which is what a network interruption looks like from
    // inside a call.
    auto *sends = new int(0);
    ports.task.send = [sends](const std::string &, const std::string &) {
        if (++*sends < 3) return true;
        // The store already holds task-3 at this point. Announce from here, so
        // the parent kills during the call and not a moment before it.
        announce(kReady);
        waitToBeKilled();
    };
    m->registerBuiltinSkills(ports);
    m->configure("owner-1", kPolicy);
    if (!m->start().success) {
        announce("START-FAILED");
        waitToBeKilled();
    }
    for (int i = 1; i <= 2; ++i) {
        if (!parsed(openTask(*m, "task-" + std::to_string(i))).value("ok", false)) {
            announce("OPEN-FAILED");
            waitToBeKilled();
        }
    }
    openTask(*m, "task-3"); // does not return
    announce("UNREACHABLE");
    waitToBeKilled();
}

/// The restart. A process that has never seen the others: print what recovery
/// found and leave.
int roleRead(const std::string &dir)
{
    AgentModuleImpl m;
    m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
    SkillPorts ports;
    ports.task = transportThatWorks();
    m.registerBuiltinSkills(ports);
    m.configure("owner-1", kPolicy);
    const auto started = m.start();
    if (!started.success) {
        std::printf("STATUS %s\n",
                    json{{"started", false}, {"error", started.error}}.dump().c_str());
        std::fflush(stdout);
        return 0;
    }
    json out = parsed(m.invoke("meta.status", "{}"));
    out["started"] = true;
    std::printf("STATUS %s\n", out.dump().c_str());
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// The parent
// ---------------------------------------------------------------------------

struct Child {
    pid_t pid = -1;
    int out = -1;
};

/// Fork and `execv` this same binary with a role. `execv` and not just `fork`,
/// on purpose: a forked child inherits the parent's whole address space, and
/// "the state survived" would then be a claim about memory this process was
/// already holding. The recovering process has to be able to start from nothing
/// but the directory.
Child spawn(const std::string &self, const std::vector<std::string> &args)
{
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {};

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(self.c_str()));
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        ::execv(self.c_str(), argv.data());
        // Only reached when execv failed; say so down the pipe so the parent's
        // wait ends in a diagnosis rather than a timeout.
        std::printf("EXEC-FAILED %s\n", std::strerror(errno));
        std::fflush(stdout);
        std::_Exit(127);
    }
    ::close(pipefd[1]);
    return Child{pid, pipefd[0]};
}

/// Read from `fd` until `marker` shows up, or the deadline passes.
std::string readUntil(int fd, const std::string &marker, int seconds)
{
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval timeout{0, 200000};
        const int ready = ::select(fd + 1, &set, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        char chunk[4096];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) break; // EOF: the child is gone
        buffer.append(chunk, static_cast<std::size_t>(n));
        if (buffer.find(marker) != std::string::npos) break;
    }
    return buffer;
}

/// SIGKILL, reaped, with the exit status returned for the caller to assert on.
int killHard(pid_t pid)
{
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

/// Run the reader role and hand back what recovery found.
json recoveredState(const std::string &self, const std::string &dir)
{
    Child child = spawn(self, {"read", dir});
    if (child.pid < 0) return json::object();
    const std::string output = readUntil(child.out, "STATUS ", 30);
    int status = 0;
    while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
    }
    ::close(child.out);
    const auto at = output.find("STATUS ");
    if (at == std::string::npos) return json::object();
    return parsed(output.substr(at + 7));
}

std::string scratchRoot()
{
    const char *tmp = std::getenv("TMPDIR");
    static const std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/lp0008-restart-kill-" +
                                   std::to_string(static_cast<long long>(::getpid()));
    return dir;
}

std::string scenarioDir(const char *name)
{
    ::mkdir(scratchRoot().c_str(), 0700);
    const std::string dir = scratchRoot() + "/" + name;
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

bool fileExists(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

/// Whether `status` says the process was killed by SIGKILL, which is the whole
/// premise of this suite. A child that exited normally ran destructors, and a
/// snapshot written by one of those proves nothing about a node that was killed.
bool wasKilled(int status)
{
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

/// The ids `meta.status` reports as open, as a set-ish string for `find`.
bool holds(const json &status, const char *id)
{
    if (!status.contains("tasks") || !status["tasks"].is_object()) return false;
    const json &tasks = status["tasks"];
    if (!tasks.contains("open") || !tasks["open"].is_array()) return false;
    for (const auto &task : tasks["open"]) {
        if (task.value("id", std::string{}) == id) return true;
    }
    return false;
}

int taskCount(const json &status, const char *field)
{
    if (!status.contains("tasks") || !status["tasks"].is_object()) return -1;
    return status["tasks"].value(field, -1);
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // ---- the child roles, dispatched before anything else ------------------
    if (argc >= 3 && std::string(argv[1]) == "open") {
        return roleOpen(argv[2], argc >= 4 ? std::atoi(argv[3]) : 1);
    }
    if (argc >= 3 && std::string(argv[1]) == "hang") {
        return roleHang(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "read") {
        return roleRead(argv[2]);
    }

    // `argv[0]` as given may be relative, and the children are spawned from the
    // same working directory, so it would work — but a resolved path is what
    // makes the failure mode "no such file" instead of a mystery.
    //
    // `realpath(path, nullptr)` rather than a `PATH_MAX` buffer: POSIX.1-2008
    // allocates for you, and `PATH_MAX` is the one constant whose header
    // differs between the two platforms this has to build on.
    char *const resolved = ::realpath(argv[0], nullptr);
    const std::string self = resolved ? std::string(resolved) : std::string(argv[0]);
    std::free(resolved);

    std::printf("a process that is KILLED keeps the tasks its invokes had finished\n");
    {
        const std::string dir = scenarioDir("killed");
        Child child = spawn(self, {"open", dir, "3"});
        check(child.pid > 0, "a second process is started against the snapshot directory");
        const std::string ready = readUntil(child.out, kReady, 30);
        check(ready.find(kReady) != std::string::npos,
              "it opens three tasks through three completed invokes, and says so");
        check(child.pid != ::getpid(), "and it really is a different process");
        check(fileExists(dir + "/tasks.json"),
              "the snapshot is on disk while that process is still alive");

        // The kill. No handler, no unwinding, no destructor, no `stop()`, no
        // atexit: SIGKILL cannot be observed by the process it ends.
        const int status = killHard(child.pid);
        ::close(child.out);
        check(wasKilled(status),
              "the process is killed with SIGKILL — not asked to exit, which is the case that "
              "already worked");
        check(!WIFEXITED(status),
              "so nothing in it ran on the way down: no destructor, no stop(), no atexit");

        // A third process, fresh image, same directory.
        const json after = recoveredState(self, dir);
        check(after.value("started", false), "a fresh process starts against what the killed one left");
        check(taskCount(after, "total") == 3 && taskCount(after, "active") == 3,
              "and finds all three pending tasks, which is the criterion");
        check(holds(after, "task-1") && holds(after, "task-2") && holds(after, "task-3"),
              "each by the id it was opened under");

        const json durability = after.contains("durability") && after["durability"].is_object()
                                    ? after["durability"]
                                    : json::object();
        check(durability.value("recovery", std::string{}) == "loaded",
              "reported as a load, not as an agent that happens to have three tasks");
        check(durability.value("recovered_tasks", -1) == 3,
              "with the count it recovered, so an empty recovery cannot pass unnoticed");

        // The terms, not just the ids: a restart that kept the ids and lost the
        // peer and the skill would satisfy every count above.
        bool sameTerms = false;
        for (const auto &task : after["tasks"]["open"]) {
            if (task.value("id", std::string{}) == "task-2") {
                sameTerms = task.value("agent", std::string{}) == kPeer &&
                            task.value("skill", std::string{}) == "storage.upload" &&
                            task.value("state", std::string{}) == "submitted";
            }
        }
        check(sameTerms, "and with the peer, the skill and the state they were opened with");
    }

    std::printf("\nthe boundary: what a kill takes is the invoke that had not returned\n");
    {
        const std::string dir = scenarioDir("boundary");
        Child child = spawn(self, {"hang", dir});
        check(child.pid > 0, "a process opens two tasks and then hangs inside a third");
        const std::string ready = readUntil(child.out, kReady, 30);
        check(ready.find(kReady) != std::string::npos,
              "and reports from inside that third call — the store has moved, the call has not "
              "returned");

        const int status = killHard(child.pid);
        ::close(child.out);
        check(wasKilled(status), "it is killed mid-call");

        const json after = recoveredState(self, dir);
        check(after.value("started", false), "a fresh process starts against what it left");
        check(holds(after, "task-1") && holds(after, "task-2"),
              "the two tasks whose invokes COMPLETED are both there");
        // The finding, asserted rather than assumed. The checkpoint runs when
        // `invoke` returns, so a call still in flight has written nothing — and
        // this suite says which side of the line that puts it on instead of
        // choosing an assertion that would be true either way.
        check(!holds(after, "task-3"),
              "and the one whose invoke never returned is NOT: the boundary is the completed call");
        check(taskCount(after, "total") == 2,
              "exactly two, so the boundary is measured from both sides");
        // Why losing it is the safe direction, checked rather than argued: the
        // hung task never reached the wire.
        check(taskCount(after, "active") == 2,
              "and the agent comes up owing exactly the work a peer has actually heard about");
    }

    std::printf("\n%s\n", failures ? "FAILURES"
                                   : "a SIGKILLed node keeps every task its invokes had finished");
    return failures ? 1 : 0;
}

// MUTATIONS RUN AGAINST THIS SUITE
//
// Each applied on its own to module/src/agent_module_plugin.cpp, built with
// this file, and run. The third column is `module_recovery_test`, the
// in-process teardown suite this one sits beside, and it is why the file is
// worth its length.
//
//                                                       here   module_recovery
//   never construct TaskPersistence (the state
//   before any of this existed) ...................... 9 FAIL     29 FAIL
//   checkpoint only from ~AgentModuleImpl() and
//   stop(), never from invoke() ...................... 9 FAIL     14 FAIL
//   a snapshot path that is not stable across
//   restarts (`tasks.json.<pid>`) .................... 9 FAIL     20 FAIL †
//
// † The interesting one. Under that third mutation `module_recovery_test`'s own
//   criterion assertion — "and both pending tasks are still there, which is the
//   criterion" — stays GREEN, and so do "the recovery is reported as a load"
//   and its recovered-task count. It goes red on twenty *other* checks, all of
//   them about the file's NAME and LOCATION: it notices the path string it was
//   told, and the file it expected to find at `<dir>/tasks.json`. It cannot
//   notice the loss itself, because both of its lives run in one process and
//   therefore share a pid and therefore share a file. A build that loses every
//   pending task on every real restart satisfies the assertion that names the
//   criterion, in the suite written for the criterion. Here the criterion
//   assertion is the one that fails.
//
// TWO THINGS THIS SUITE CAN CHECK AND A TEARDOWN SUITE CANNOT
//
//   1. That nothing on the way down is load-bearing. A destroyed object runs
//      its destructor; a killed process runs nothing. Above, that is asserted
//      rather than assumed — `WIFSIGNALED` and `WTERMSIG == SIGKILL`.
//   2. The boundary. There is no way to destroy an object in the middle of one
//      of its own calls, so "what happens to a change made by an invoke that
//      never returned" is a question only a second process can be asked.
//
// AND ONE THIS SUITE CANNOT CHECK, said plainly rather than left implied:
// SIGKILL does not falsify the fsync. Bytes handed to `write(2)` belong to the
// kernel's page cache and outlive the process that wrote them whatever kills
// it, so a build with both `fsync` calls in `posixSnapshotFilePort()` removed
// passes every assertion above — measured, 20 ok and 0 FAIL, not deduced. The flush
// is for power loss and a reset virtual machine, which is not something a unit
// test on one host can stage; `task_persistence_test.cpp` covers the ordering
// it depends on (write, flush, rename, flush the parent) against an injected
// file port instead.
