// Skill failures are isolated *from concurrently running skills*, on threads.
//
// The prize's Reliability criterion says: "skill failures are isolated and do
// not affect concurrently running skills". What this repository had for it was
// a try/catch exercised from one thread — `skills_test.cpp` invokes a throwing
// skill and then, afterwards, invokes another one and checks it still answers.
// That is sequential recovery. It is worth having and it is not what the
// criterion says: nothing in `module/tests/` started a second thread against
// the module at all, so "concurrently" was the one word in the sentence no
// assertion touched.
//
// The difference is not pedantry, and it is measured rather than asserted. The
// mutation `std::string result;` -> `static std::string result;` inside
// `AgentModuleImpl::invoke` — one keyword, the shape of every result-buffer bug
// ever written — leaves the whole sequential suite green while every caller
// starts getting somebody else's answer. This file goes red on it, and the
// mutation list at the bottom records what each one costs.
//
// So there are two phases, because they falsify different things:
//
//   1. A RENDEZVOUS. Eight skills are invoked on eight threads and every one of
//      them blocks inside its own `invoke()` until all eight have arrived. Only
//      then do the two throwers throw — while the other six are demonstrably
//      still inside the module. The count of assembled rounds is asserted, so
//      "they overlapped" is a measurement and not a hope: an implementation
//      that serialised the calls would never assemble the party and would say
//      so here rather than passing quietly.
//
//   2. A STORM. No synchronisation at all, thousands of invocations across the
//      same eight threads, throwers mixed in with skills that mutate the shared
//      task store, plus a thread registering new skills the whole time. Enough
//      iterations that a race has room to happen.
//
// Every call carries its own `{"thread","seq"}` and every answer has to carry
// the same pair back. That is the assertion a shared buffer fails: "it returned
// ok" is satisfied by returning another thread's success just as well as by
// returning your own.
//
// The last thing checked is durability across a throw, because it is the same
// property one layer down: a skill that moved the task store and *then* threw
// used to skip the checkpoint entirely — `invoke` returned from inside the
// catch, straight past it — so the store had moved and nothing on disk said so.
//
// Build (the module's own suite line, with this file in place of skills_test):
//   clang++ -std=c++17 -Wall -Wextra -I<logos-cpp-sdk>/cpp -I<nlohmann>/include \
//     module/tests/concurrent_skills_test.cpp module/src/*.cpp -o concurrent_skills_test

#include "../src/agent_module_plugin.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
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

/// The count half of an assertion, with both numbers printed when they differ —
/// for a count the disagreement is the diagnosis, and `check` prints only the
/// property.
static void checkCount(long long got, long long want, const char *what)
{
    const bool ok = got == want;
    if (!ok) std::printf("       expected %lld, got %lld\n", want, got);
    check(ok, what);
}

static json parsed(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return (j.is_discarded() || !j.is_object()) ? json::object() : j;
}

static bool mentions(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

static const std::string kPolicy(64, 'a');

namespace {

// ---------------------------------------------------------------------------
// A reusable rendezvous
// ---------------------------------------------------------------------------

/// Every participant blocks until `need` of them have arrived, then all are
/// released together; reusable, so one object serves every round.
///
/// `std::barrier` is C++20 and this module builds at C++17. The deadline is the
/// part that matters for a test: a party that never assembles has to be
/// *reported*, not waited on forever, because a suite that hangs says nothing
/// at all. @ref assembled counts the rounds that really did gather, and that
/// count is asserted — it is the evidence that the calls overlapped.
class Rendezvous
{
public:
    explicit Rendezvous(int need) : need_(need) {}

    bool arriveAndWait(int seconds = 5)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Once a party has failed to assemble, stop waiting for the next one.
        // A build that serialises the calls would otherwise pay the deadline
        // forty times over and be killed by the watchdog with a message about
        // deadlock, when the true finding — "they never overlapped" — is the
        // one @ref assembled is about to report.
        if (broken_) return false;
        const unsigned long long generation = generation_;
        if (++arrived_ >= need_) {
            arrived_ = 0;
            ++generation_;
            ++assembled_;
            cv_.notify_all();
            return true;
        }
        const bool gathered =
            cv_.wait_for(lock, std::chrono::seconds(seconds),
                         [this, generation] { return generation_ != generation || broken_; }) &&
            !broken_;
        if (!gathered && !broken_) {
            broken_ = true;
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
        }
        return gathered;
    }

    int assembled() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return assembled_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int need_;
    int arrived_ = 0;
    int assembled_ = 0;
    bool broken_ = false;
    unsigned long long generation_ = 0;
};

// ---------------------------------------------------------------------------
// The skills under test
// ---------------------------------------------------------------------------

/// Answers with the `{"thread","seq"}` it was called with.
///
/// The echo is the whole point. An assertion that the answer is `ok` is
/// satisfied by another thread's `ok`; an assertion that the answer names *this*
/// call is not.
class Worker final : public ISkill
{
public:
    Worker(std::string name, Rendezvous *gate) : name_(std::move(name)), gate_(gate) {}

    std::string name() const override { return name_; }
    std::string parameterSchema() const override { return R"({"type":"object"})"; }

    std::string invoke(const std::string &paramsJson) override
    {
        const json in = parsed(paramsJson);
        // Inside the skill, holding nothing of the module's: this is where the
        // eight calls are proven to be in flight at once.
        if (gate_) gate_->arriveAndWait();
        // A little real work, so the threads are not merely queued behind each
        // other on the way in and out.
        std::string churn;
        for (int i = 0; i < 128; ++i) churn += std::to_string(i);
        ++completed_;
        return json{{"ok", true},
                    {"skill", name_},
                    {"thread", in.value("thread", -1)},
                    {"seq", in.value("seq", -1)},
                    {"work", churn.size()}}
            .dump();
    }

    int completed() const { return completed_.load(); }

private:
    std::string name_;
    Rendezvous *gate_;
    std::atomic<int> completed_{0};
};

/// Throws, and throws while the others are still inside.
///
/// Two flavours, because the module has two catch clauses and only one of them
/// is reached by a `std::exception`. A build that kept `catch (const
/// std::exception &)` and lost `catch (...)` would take the whole process down
/// on the second flavour, and nothing here exercised it from a thread before.
class Thrower final : public ISkill
{
public:
    Thrower(std::string name, Rendezvous *gate, bool standard)
        : name_(std::move(name)), gate_(gate), standard_(standard) {}

    std::string name() const override { return name_; }
    std::string parameterSchema() const override { return R"({"type":"object"})"; }

    std::string invoke(const std::string &paramsJson) override
    {
        const json in = parsed(paramsJson);
        if (gate_) gate_->arriveAndWait();
        ++thrown_;
        const std::string tag = name_ + "/" + std::to_string(in.value("thread", -1)) + "/" +
                                std::to_string(in.value("seq", -1));
        if (standard_) {
            throw std::runtime_error("kaboom " + tag);
        }
        // Not derived from std::exception on purpose: only `catch (...)` sees
        // this one.
        throw tag;
    }

    int thrown() const { return thrown_.load(); }

private:
    std::string name_;
    Rendezvous *gate_;
    bool standard_;
    std::atomic<int> thrown_{0};
};

/// Moves the shared task store, which is the state a failing neighbour must not
/// be able to corrupt.
class Mutator final : public ISkill
{
public:
    Mutator(std::string name, TaskStore &store, const std::string &peer)
        : name_(std::move(name)), store_(store), peer_(peer) {}

    std::string name() const override { return name_; }
    std::string parameterSchema() const override { return R"({"type":"object"})"; }

    std::string invoke(const std::string &paramsJson) override
    {
        const json in = parsed(paramsJson);
        const std::string id = "t-" + std::to_string(in.value("thread", -1)) + "-" +
                               std::to_string(in.value("seq", -1));
        std::string err;
        const bool created = store_.create(id, "ctx-" + id, peer_, "storage.upload", err);
        if (created) ++created_;
        return json{{"ok", created},
                    {"skill", name_},
                    {"thread", in.value("thread", -1)},
                    {"seq", in.value("seq", -1)},
                    {"task_id", id},
                    {"error", err}}
            .dump();
    }

    int created() const { return created_.load(); }

private:
    std::string name_;
    TaskStore &store_;
    std::string peer_;
    std::atomic<int> created_{0};
};

/// Moves the store and then throws, without returning. The durability of what
/// it did is not the caller's problem to remember.
class MutateThenThrow final : public ISkill
{
public:
    MutateThenThrow(std::string name, TaskStore &store, const std::string &peer)
        : name_(std::move(name)), store_(store), peer_(peer) {}

    std::string name() const override { return name_; }
    std::string parameterSchema() const override { return R"({"type":"object"})"; }

    std::string invoke(const std::string &paramsJson) override
    {
        const json in = parsed(paramsJson);
        const std::string id = in.value("task_id", std::string("t-throw"));
        std::string err;
        store_.create(id, "ctx-" + id, peer_, "storage.upload", err);
        throw std::runtime_error("moved the store, then failed");
    }

private:
    std::string name_;
    TaskStore &store_;
    std::string peer_;
};

/// One thread's tally, so nothing is written from two threads at once and the
/// assertions can be made on the main thread after every join.
struct Tally {
    int escaped = 0;      ///< an exception left `invoke()` — isolation broken
    int okAnswers = 0;    ///< came back `ok:true`
    int reported = 0;     ///< came back as a failure that names the skill
    int mismatched = 0;   ///< the answer did not belong to this call
    int notJson = 0;      ///< the module returned something that is not JSON
    int wrongSkill = 0;   ///< the answer came from a skill we did not call
};

/// Set when the watchdog fired: a deadlock is a hang, and a suite that hangs
/// reports nothing at all.
std::atomic<bool> watchdogFired{false};

void startWatchdog(int seconds)
{
    std::thread([seconds] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (watchdogFired.load()) return; // set to `true` by main on the way out
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("  FAIL the suite deadlocked: no answer within %d seconds\n", seconds);
        std::fflush(stdout);
        // Not `exit`: static destructors would run against mutexes other
        // threads are still blocked on.
        std::_Exit(1);
    }).detach();
}

std::string scratchDir(const char *name)
{
    const char *tmp = std::getenv("TMPDIR");
    const std::string root = std::string(tmp && *tmp ? tmp : "/tmp") + "/lp0008-concurrent-" +
                             std::to_string(static_cast<long long>(getpid()));
    ::mkdir(root.c_str(), 0700);
    const std::string dir = root + "/" + name;
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

std::string readFile(const std::string &path)
{
    std::string out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
    std::fclose(f);
    return out;
}

/// A real base58 account id, and one `taskTopic` accepts.
const std::string kPeer = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";

} // namespace

int main()
{
    // Line-buffered even when this is piped into `tee`, which CI does. A suite
    // that only flushes on the way out tells a reader nothing about WHERE it
    // stopped, and this one has phases that can block.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    startWatchdog(120);

    // -----------------------------------------------------------------------
    // 1. The rendezvous: everybody inside, then two of them throw
    // -----------------------------------------------------------------------
    std::printf("eight skills inside the module at once, two of them throwing\n");

    constexpr int kThreads = 8;
    constexpr int kRounds = 40;
    {
        AgentModuleImpl m;
        m.configure("owner-1", kPolicy);
        check(m.start().success, "the module starts");

        Rendezvous gate(kThreads);
        std::vector<std::shared_ptr<Worker>> workers;
        int wired = 0;
        for (int i = 0; i < kThreads - 2; ++i) {
            workers.push_back(std::make_shared<Worker>("work." + std::to_string(i), &gate));
            if (m.registerSkill(workers.back()).success) ++wired;
        }
        checkCount(wired, kThreads - 2, "and takes a worker skill for each thread");
        // Ungated, for the "is the module still usable" call at the end: a
        // gated skill invoked on its own would sit out the rendezvous deadline
        // waiting for seven threads that have already been joined.
        auto solo = std::make_shared<Worker>("work.solo", nullptr);
        check(m.registerSkill(solo).success, "and one that answers on its own");
        auto boomStd = std::make_shared<Thrower>("boom.std", &gate, true);
        auto boomRaw = std::make_shared<Thrower>("boom.raw", &gate, false);
        check(m.registerSkill(boomStd).success, "a skill that throws a std::exception");
        check(m.registerSkill(boomRaw).success, "and one that throws something else entirely");

        std::vector<Tally> tallies(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t, &m, &tallies] {
                // The last two threads drive the throwers; the rest do work.
                const bool throws = t >= kThreads - 2;
                const std::string skill =
                    throws ? (t == kThreads - 2 ? "boom.std" : "boom.raw")
                           : ("work." + std::to_string(t));
                Tally &tally = tallies[static_cast<std::size_t>(t)];
                for (int seq = 0; seq < kRounds; ++seq) {
                    const std::string params = json{{"thread", t}, {"seq", seq}}.dump();
                    std::string answer;
                    try {
                        answer = m.invoke(skill, params);
                    } catch (...) {
                        // The module's whole isolation promise, measured from
                        // outside it. Counted rather than allowed to escape,
                        // so a build without the catch fails an assertion
                        // instead of taking the process down with it.
                        ++tally.escaped;
                        continue;
                    }
                    const auto j = json::parse(answer, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) {
                        ++tally.notJson;
                        continue;
                    }
                    if (j.value("ok", false)) {
                        ++tally.okAnswers;
                        // The answer has to be THIS call's. A shared result
                        // buffer returns a neighbour's, which is `ok` too.
                        if (j.value("thread", -1) != t || j.value("seq", -1) != seq) {
                            ++tally.mismatched;
                        }
                        if (j.value("skill", std::string{}) != skill) ++tally.wrongSkill;
                    } else {
                        ++tally.reported;
                        if (!mentions(j.value("error", std::string{}), skill)) ++tally.wrongSkill;
                    }
                }
            });
        }
        for (auto &thread : threads) thread.join();

        Tally total;
        for (const auto &tally : tallies) {
            total.escaped += tally.escaped;
            total.okAnswers += tally.okAnswers;
            total.reported += tally.reported;
            total.mismatched += tally.mismatched;
            total.notJson += tally.notJson;
            total.wrongSkill += tally.wrongSkill;
        }

        // The evidence that this was concurrent at all. Every round released
        // only once all eight calls were inside their own `invoke()`, so the
        // throws below happened with six other skills mid-flight — not before
        // them and not after them.
        checkCount(gate.assembled(), kRounds,
                   "every round assembled all eight calls inside the module before any returned");
        checkCount(total.escaped, 0, "no exception escaped invoke(), on any thread");
        checkCount(total.okAnswers, (kThreads - 2) * kRounds,
                   "every skill that was not throwing completed, while the throwers threw");
        checkCount(total.mismatched, 0,
                   "and each answer carries its OWN call's thread and sequence, not a neighbour's");
        checkCount(total.wrongSkill, 0, "and names the skill it was actually dispatched to");
        checkCount(total.notJson, 0, "no call came back as something other than JSON");
        checkCount(total.reported, 2 * kRounds,
                   "both throwers' failures came back as failures, every round");
        checkCount(boomStd->thrown() + boomRaw->thrown(), 2 * kRounds,
                   "and they really did throw that many times, rather than quietly not running");

        int workerCompleted = 0;
        for (const auto &worker : workers) workerCompleted += worker->completed();
        checkCount(workerCompleted, (kThreads - 2) * kRounds,
                   "the workers ran to the end of their own invoke(), which is what 'unaffected' means");

        // The module is still a module.
        check(json::parse(m.skills(), nullptr, false).is_array(),
              "the module still answers skills() after all of that");
        check(parsed(m.invoke("work.solo", R"({"thread":0,"seq":0})")).value("ok", false),
              "and still dispatches");
    }

    // -----------------------------------------------------------------------
    // 2. The storm: no synchronisation, thousands of calls, shared state
    // -----------------------------------------------------------------------
    std::printf("\nthe same eight threads, unsynchronised, with the store underneath them\n");

    constexpr int kIterations = 1000;
    {
        TaskStore store;
        AgentModuleImpl m;
        SkillPorts ports;
        ports.tasks = &store;
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        check(m.start().success, "a module over a task store the caller owns");

        auto worker = std::make_shared<Worker>("work.free", nullptr);
        auto boomStd = std::make_shared<Thrower>("boom.std", nullptr, true);
        auto boomRaw = std::make_shared<Thrower>("boom.raw", nullptr, false);
        auto mutator = std::make_shared<Mutator>("store.touch", store, kPeer);
        m.registerSkill(worker);
        m.registerSkill(boomStd);
        m.registerSkill(boomRaw);
        m.registerSkill(mutator);

        // Four names, invoked round-robin by (thread + seq), so each thread
        // meets each skill and no two threads are ever in step.
        const std::vector<std::string> names{"work.free", "boom.std", "store.touch", "boom.raw"};

        std::vector<Tally> tallies(kThreads);
        std::atomic<int> registered{0};
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t, &m, &tallies, &names] {
                Tally &tally = tallies[static_cast<std::size_t>(t)];
                for (int seq = 0; seq < kIterations; ++seq) {
                    const std::string skill =
                        names[static_cast<std::size_t>((t + seq) % static_cast<int>(names.size()))];
                    const std::string params = json{{"thread", t}, {"seq", seq}}.dump();
                    std::string answer;
                    try {
                        answer = m.invoke(skill, params);
                    } catch (...) {
                        ++tally.escaped;
                        continue;
                    }
                    const auto j = json::parse(answer, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) {
                        ++tally.notJson;
                        continue;
                    }
                    if (j.value("ok", false)) {
                        ++tally.okAnswers;
                        if (j.value("thread", -1) != t || j.value("seq", -1) != seq) {
                            ++tally.mismatched;
                        }
                        if (j.value("skill", std::string{}) != skill) ++tally.wrongSkill;
                    } else {
                        ++tally.reported;
                        if (!mentions(j.value("error", std::string{}), skill)) ++tally.wrongSkill;
                    }
                }
            });
        }

        // A ninth thread registering skills the whole time. Registration takes
        // the module's lock and `invoke` drops it before calling a skill, so
        // this is the pair that would deadlock if either changed its mind.
        std::thread registrar([&m, &registered, &stop] {
            for (int i = 0; i < 200 && !stop.load(); ++i) {
                auto extra = std::make_shared<Worker>("late." + std::to_string(i), nullptr);
                if (m.registerSkill(extra).success) ++registered;
                std::this_thread::yield();
            }
        });

        for (auto &thread : threads) thread.join();
        stop.store(true);
        registrar.join();

        Tally total;
        for (const auto &tally : tallies) {
            total.escaped += tally.escaped;
            total.okAnswers += tally.okAnswers;
            total.reported += tally.reported;
            total.mismatched += tally.mismatched;
            total.notJson += tally.notJson;
            total.wrongSkill += tally.wrongSkill;
        }

        const int calls = kThreads * kIterations;
        // (t + seq) % 4 over a whole number of cycles: each of the four names
        // gets exactly a quarter of the calls, for every thread.
        const int perSkill = calls / 4;
        checkCount(total.escaped, 0, "no exception escaped invoke() under free-running contention");
        checkCount(total.okAnswers + total.reported + total.notJson, calls,
                   "every call was answered exactly once");
        checkCount(total.notJson, 0, "and every answer was JSON");
        checkCount(total.mismatched, 0,
                   "no answer belonged to another thread's call, across the whole storm");
        checkCount(total.wrongSkill, 0, "and none of them named another skill");
        checkCount(total.reported, 2 * perSkill,
                   "the two throwing skills failed on exactly their own share of the calls");
        checkCount(total.okAnswers, 2 * perSkill,
                   "and the two that do not throw succeeded on exactly theirs");

        // The shared state the failures ran alongside. `store.touch` creates a
        // task per call with an id derived from its own thread and sequence, so
        // a lost update, a duplicate or a torn map all show up as a count that
        // is not this one.
        checkCount(static_cast<long long>(store.size()), perSkill,
                   "the task store holds exactly the tasks the mutating skill created");
        checkCount(mutator->created(), perSkill, "which is every call it was given");
        checkCount(static_cast<long long>(store.active().size()), perSkill,
                   "all of them still pending, none damaged into a terminal state");

        // Every id present exactly once, which is the assertion a count alone
        // cannot make: two threads writing the same slot would keep the total
        // right and lose a task.
        std::set<std::string> want;
        for (int t = 0; t < kThreads; ++t) {
            for (int seq = 0; seq < kIterations; ++seq) {
                if (names[static_cast<std::size_t>((t + seq) % 4)] == "store.touch") {
                    want.insert("t-" + std::to_string(t) + "-" + std::to_string(seq));
                }
            }
        }
        int found = 0;
        for (const auto &id : want) {
            TaskStore::Task task;
            if (store.find(id, task) && task.agent == kPeer && task.skill == "storage.upload") {
                ++found;
            }
        }
        checkCount(found, static_cast<long long>(want.size()),
                   "and each one is the task that call opened, with its own terms");

        check(registered.load() > 0, "skills were registered while all of that was in flight");
        const auto card = json::parse(m.skills(), nullptr, false);
        check(card.is_array() && card.size() >= static_cast<std::size_t>(registered.load()),
              "and the registry came out of it whole");
        check(parsed(m.invoke("late.0", R"({"thread":0,"seq":0})")).value("ok", false),
              "with the late arrivals dispatchable");
        check(parsed(m.invoke("meta.status", "{}")).value("ok", false),
              "and the module's own status skill still answering");
    }

    // -----------------------------------------------------------------------
    // 3. What a throwing skill did to the store is still written down
    // -----------------------------------------------------------------------
    std::printf("\na skill that moves the store and then throws does not take the record with it\n");
    {
        // Attribution is why this one is a single call rather than a storm: any
        // later `invoke` would checkpoint the store too, so a suite that mixed
        // them could not say which write it was watching. Exactly one call is
        // made, and the file is read before anything else happens.
        const std::string dir = scratchDir("throw-checkpoint");
        TaskStore store;
        AgentModuleImpl m;
        m._logosCoreSetContext_("/modules/agent", "agent-1", dir);
        SkillPorts ports;
        ports.tasks = &store;
        m.registerBuiltinSkills(ports);
        m.configure("owner-1", kPolicy);
        check(m.start().success, "the module starts with somewhere to write");

        auto halfDone = std::make_shared<MutateThenThrow>("half.done", store, kPeer);
        check(m.registerSkill(halfDone).success, "a skill that opens a task and then throws");

        const json answer = parsed(m.invoke("half.done", R"({"task_id":"t-half-done"})"));
        check(answer.value("ok", true) == false, "the call is reported as the failure it was");
        check(mentions(answer.value("error", std::string{}), "moved the store, then failed"),
              "carrying what the skill said on its way out");
        checkCount(static_cast<long long>(store.size()), 1,
                   "and the store really did move before it threw");

        const std::string snapshot = readFile(dir + "/tasks.json");
        check(!snapshot.empty(),
              "the snapshot exists: a throw is not a reason to skip the checkpoint");
        check(mentions(snapshot, "t-half-done"),
              "and holds the task the skill opened, so a restart from here still has it");
    }

    if (failures == 0) {
        // Tell the watchdog to stand down before the process winds down.
        watchdogFired.store(true);
    }
    std::printf("\n%s\n", failures
                              ? "FAILURES"
                              : "skill failures stay inside the skill that failed, concurrently");
    std::fflush(stdout);
    // `_Exit` rather than `return`: the watchdog is detached and the registrar's
    // late skills are held by a module that is about to be destroyed. Nothing
    // here needs a static destructor to run, and running them around a detached
    // thread is how a green suite ends in a crash.
    std::_Exit(failures ? 1 : 0);
}

// MUTATIONS RUN AGAINST THIS SUITE
//
// Each was applied on its own to module/src/agent_module_plugin.cpp, built with
// this file, and run. Recorded because a test nobody has tried to break is a
// test nobody has checked. The last two columns are the point of the file: they
// are the same mutations run against the two SEQUENTIAL suites, which between
// them carry 201 assertions about this module.
//
//                                                    here   skills_  module_
//                                                            test   recovery
//   `result` becomes a shared buffer, correctly
//   locked (a neighbour's answer is returned) ....  4 FAIL   green    green
//   `std::string result` -> `static std::string
//   result` (the same bug, unlocked) .............  crash    green    green
//   remove invoke()'s try/catch entirely .........  5 FAIL   2 FAIL   green
//   return from inside the catch, so a skill that
//   moved the store and then threw is never
//   checkpointed .................................  2 FAIL   green    green
//
// The two shared-buffer rows are the reason this file exists. Both leave every
// sequential assertion in the repository green — 119 in skills_test, 82 in
// module_recovery_test, none of which ever has two calls in flight — while
// every caller here starts receiving another thread's answer. The unlocked form
// is a data race, so what it produces is a crash rather than a count; the
// locked form is perfectly defined C++ and simply wrong, and that one is
// counted. Neither is exotic: a result buffer that outlives the call is the
// oldest bug in the genre.
//
// The no-catch build is the one row where the sequential suite already did its
// job: `skills_test` wraps its own `invoke` of a throwing skill and notices the
// escape. Both builds then abort, because an exception that leaves a worker
// thread ends the process — here after printing its five FAILs, since the
// threads count escapes rather than letting them out.
