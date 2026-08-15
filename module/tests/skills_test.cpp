// Exercise the skills through their ports, with no Logos node running, and the
// module that hosts them against skills written to misbehave.
//
// This is what the DeliveryPort / StoragePort indirection is for. A node is
// needed to prove a message arrives; it is not needed to prove that a skill
// refuses when the node is down, reports which half of a share failed, or does
// not throw on a malformed payload. Those are the behaviours a reviewer cannot
// see from a screenshot, and they are exactly what a stub would get wrong.
//
// The module half is the same argument one level up. AgentModuleImpl's job is
// to survive skills it did not write: a name that tries to forge JSON, a schema
// that is not JSON, a method that throws, a skill that calls back into the
// module mid-call. None of that needs a node either, and none of it was covered
// — which is how a policy hash of sixty-four 'z's and a second configure() that
// silently rebound a running agent both passed for correct.
//
// The third thing this covers is registration. Thirteen skills can be
// implemented, tested and merged and still be invisible: the module that hosts
// them has to be told about them, and until it was, a module that loaded
// perfectly in Basecamp answered `skills()` with `[]` and `invoke()` with "no
// skill named …" for every name it documents — which is worse than not loading,
// because it looks like it works. So the assertions below are not only that
// each skill is registered, but that a skill whose port nobody wired *refuses
// and says so*, rather than crashing on an empty std::function or reporting
// success for work nothing performed.
//
// The module tests need the Logos C++ SDK headers, which are not installed
// everywhere this file is compiled, so they are guarded. Build them with:
//   clang++ -std=c++17 -I<nlohmann>/include -I<logos-cpp-sdk>/cpp \
//     module/tests/skills_test.cpp module/src/*.cpp -o skt
// (all of module/src: registration is what links the skills into the module, so
// the module half of this suite now needs every skill's translation unit.)
#include "../src/messaging_skills.h"
#include "../src/storage_skills.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>

#if defined(__has_include) && __has_include(<logos_module_context.h>)
#define LP0008_MODULE_TESTS 1
#include "../src/agent_module_plugin.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#endif

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static bool okOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return !j.is_discarded() && j.value("ok", false);
}

static std::string errOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? "" : j.value("error", std::string{});
}

#ifdef LP0008_MODULE_TESTS
namespace {

/// A skill whose three methods the test supplies, because the interesting
/// skills are the ones nobody would write on purpose: a name that tries to
/// forge JSON, a schema that is not JSON, a method that throws, a method that
/// calls back into the module while the module is answering.
class Fake final : public ISkill
{
public:
    Fake(std::string name, std::string schema)
        : name_(std::move(name)), schema_(std::move(schema)) {}

    std::function<std::string()> onName;
    std::function<std::string()> onSchema;
    std::function<std::string(const std::string &)> onInvoke;

    std::string name() const override { return onName ? onName() : name_; }
    std::string parameterSchema() const override { return onSchema ? onSchema() : schema_; }
    std::string invoke(const std::string &p) override
    {
        return onInvoke ? onInvoke(p) : std::string(R"({"ok":true})");
    }

private:
    std::string name_;
    std::string schema_;
};

/// Set when a watchdog fired, so main can leave without touching a mutex some
/// thread is still blocked on.
bool hung = false;

/// Run `f` with a watchdog.
///
/// A self-deadlock is a hang, not a failed assertion: the suite has to time it
/// out or it never reports at all. The thread is detached and the module it
/// works on is leaked on purpose — nothing may destroy a mutex another thread
/// is still waiting for.
bool completes(std::function<void()> f, int seconds = 5)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    std::thread([f = std::move(f), done]() mutable { f(); done->set_value(); }).detach();
    const bool ok = fut.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready;
    if (!ok) hung = true;
    return ok;
}

/// The card entry for a skill, by the name it was registered under.
json entryFor(const json &card, const std::string &name)
{
    if (!card.is_array()) return json::object();
    for (const auto &e : card) {
        if (e.is_object() && e.value("name", std::string{}) == name) return e;
    }
    return json::object();
}

/// How many entries the card has under `name`. More than one would mean the
/// registry accepted a duplicate, and `invoke()` dispatches on a map, so one of
/// them would be advertised and unreachable.
std::size_t entriesNamed(const json &card, const std::string &name)
{
    std::size_t n = 0;
    if (!card.is_array()) return 0;
    for (const auto &e : card) {
        if (e.is_object() && e.value("name", std::string{}) == name) ++n;
    }
    return n;
}

/// Every skill the module ships with, spelled out rather than counted.
///
/// Named so that removing one from registration turns this suite red *with the
/// name*, which is the mutation this file exists to catch: a count alone goes
/// red without saying what went missing, and no assertion at all is how thirteen
/// implemented skills came to be unreachable.
const char *kBuiltinSkills[] = {
    "messaging.send",      "messaging.join",   "messaging.create_group",
    "storage.upload",      "storage.download", "storage.list",
    "storage.share",       "wallet.balance",   "wallet.send",
    "wallet.history",      "program.query",    "program.call",
    "program.deploy",      "agent.card",       "agent.discover",
    "agent.task",          "agent.subscribe",  "agent.cancel",
    "meta.status",         "meta.configure",   "meta.skills",
    "agent.evaluate_task",
};
constexpr std::size_t kBuiltinCount = sizeof(kBuiltinSkills) / sizeof(kBuiltinSkills[0]);

/// A real-shaped LEZ account: 44 characters of base58, the alphabet the scripts
/// match with. `wallet.balance` checks this before it reads anything.
const char *kAgentAccount = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";

} // namespace
#endif

int main()
{
    std::printf("messaging\n");
    {
        // A node that has not started must refuse, not report success. `start`
        // returns as soon as it is dispatched, so this is the realistic state.
        DeliveryPort down{[] { return false; }, {}, {}, {}};
        SendSkill s(down);
        const auto r = s.invoke(R"({"recipient":"abc","message":"hi"})");
        check(!okOf(r), "send refuses while the node is not started");
        check(errOf(r).find("not started") != std::string::npos, "and says why");
    }
    {
        std::string sentTopic;
        DeliveryPort up{[] { return true; },
                        [&](const std::string &t, const std::vector<std::uint8_t> &) {
                            sentTopic = t; return true; },
                        [](const std::string &) { return true; },
                        [](const std::string &) { return true; }};
        SendSkill s(up);
        check(okOf(s.invoke(R"({"recipient":"abc","message":"hi"})")), "send succeeds when up");
        check(sentTopic == ownerTopic("abc"), "and addresses the recipient's owner topic");

        // Malformed input must not throw out of invoke: skill failures are
        // required to stay isolated from the module.
        check(!okOf(s.invoke("not json")), "malformed json is refused, not thrown");
        check(!okOf(s.invoke(R"({"recipient":"abc"})")), "a missing field is refused");
        check(!okOf(s.invoke(R"({"recipient":"","message":"hi"})")), "an empty field is refused");
    }
    {
        bool created = false;
        int invites = 0;
        DeliveryPort up{[] { return true; },
                        [&](const std::string &, const std::vector<std::uint8_t> &) { ++invites; return true; },
                        [](const std::string &) { return true; },
                        [&](const std::string &) { created = true; return true; }};
        CreateGroupSkill g(up);
        const auto r = g.invoke(R"({"group_id":"g1","members":["a","b"]})");
        check(okOf(r) && created, "create_group opens a reliable channel");
        check(invites == 2, "and invites every member on their own topic");
        check(!okOf(g.invoke(R"({"group_id":"g1","members":[]})")), "an empty member list is refused");
    }
    {
        // TOPICS ARE BUILT BY CONCATENATION, SO THE PARTS ARE CHECKED.
        //
        // `/<application>/<version>/<name>/<encoding>` is the grammar this
        // repository cites, and every id below lands in `<name>`. Measured
        // before the fix: a recipient of `AAA/json", "x":1, "y":"` published on
        // `/lp-0008/1/owner-AAA/json/json` — a topic the sender chose, not the
        // one the caller named — and a member id of `../../owner-VICTIM/json`
        // redirected a group invitation onto somebody else's topic.
        check(ownerTopic("abc") == "/lp-0008/1/owner-abc/json",
              "an ordinary account still builds the documented topic");
        check(ownerTopic(R"(AAA/json", "x":1, "y":")").empty(),
              "an account carrying the topic separator names no topic at all");
        check(ownerTopic("../../owner-VICTIM/json").empty(), "and neither does a traversal");
        check(discoveryTopic("a/json").empty(), "the discovery topic is built the same way");
        check(discoveryTopic("g1") == "/lp-0008/1/discovery-g1/json", "and still works");

        std::string sentTopic;
        int sends = 0, created2 = 0;
        DeliveryPort up{[] { return true; },
                        [&](const std::string &t, const std::vector<std::uint8_t> &) {
                            sentTopic = t; ++sends; return true; },
                        [](const std::string &) { return true; },
                        [&](const std::string &) { ++created2; return true; }};

        SendSkill s(up);
        check(!okOf(s.invoke(R"({"recipient":"AAA/json\", \"x\":1, \"y\":\"","message":"hi"})")),
              "messaging.send refuses a recipient that would name another topic");
        check(sends == 0, "and publishes nothing at all");

        JoinSkill j(up);
        check(!okOf(j.invoke(R"({"group_id":"a/json"})")),
              "messaging.join refuses a group id that would subscribe elsewhere");

        CreateGroupSkill g(up);
        check(!okOf(g.invoke(R"({"group_id":"g1","members":["../../owner-VICTIM/json"]})")),
              "and create_group refuses to invite onto a forged topic");
        check(created2 == 0 && sends == 0,
              "before the channel is opened, so no half-built group is left behind");
    }

    std::printf("storage\n");
    {
        // Two things were wrong with this port and only one of them was under
        // test. `ready` returns false AND `upload` is an empty std::function, so
        // `!okOf(...)` was satisfied by either guard — and deleting the readiness
        // guard from UploadSkill left the whole suite printing "all skill
        // behaviours hold" with a storage.upload that ignores a stopped node.
        // The port is therefore crippled ONE WAY AT A TIME, and the error is
        // pinned, which is the shape `send refuses while the node is not
        // started` two hundred lines up already uses and `share` was fixed to.
        StoragePort stopped{[] { return false; },
                            [](const std::string &, std::int64_t) { return std::string("cid-1"); },
                            {}, {}, {}};
        UploadSkill uStopped(stopped);
        const auto rs = uStopped.invoke(R"({"path":"/tmp/x"})");
        check(!okOf(rs), "upload refuses while storage is down");
        check(errOf(rs).find("not started") != std::string::npos,
              "and blames the stopped node, not the wiring");

        StoragePort unwired{[] { return true; }, {}, {}, {}, {}};
        UploadSkill uUnwired(unwired);
        const auto ru = uUnwired.invoke(R"({"path":"/tmp/x"})");
        check(!okOf(ru), "and refuses when the node is up but nobody wired upload");
        check(errOf(ru).find("not wired") != std::string::npos,
              "blaming the wiring that time");
    }
    {
        StoragePort up{[] { return true; },
                       [](const std::string &, std::int64_t) { return std::string("cid-1"); },
                       [](const std::string &, const std::string &) { return true; },
                       [] { return std::string("[]"); },
                       [](const std::string &c) { return c == "cid-1"; }};
        UploadSkill u(up);
        const auto r = u.invoke(R"({"path":"/tmp/x","label":"notes"})");
        auto j = json::parse(r, nullptr, false);
        check(okOf(r) && j.value("address", std::string{}) == "cid-1", "upload returns the content address");

        DownloadSkill d(up);
        check(!okOf(d.invoke(R"({"address":"nope","path":"/tmp/y"})")),
              "download reports an unknown address as unknown");
        check(okOf(d.invoke(R"({"address":"cid-1","path":"/tmp/y"})")), "and succeeds on a known one");
    }
    {
        // share must refuse when it CANNOT check the address, not only when the
        // check comes back negative. Without these two, deleting the existence
        // check outright leaves the suite green while share reports ok:true for
        // an address nobody ever verified.
        SharePort willSend{[](const std::string &, const std::string &) { return true; }};

        StoragePort down{[] { return false; }, {}, {}, {},
                         [](const std::string &) { return true; }};
        ShareSkill sDown(down, willSend);
        check(!okOf(sDown.invoke(R"({"address":"cid-1","recipient":"bob"})")),
              "share refuses while the storage node is stopped");

        StoragePort noExists{[] { return true; }, {}, {}, {}, {}};
        ShareSkill sNo(noExists, willSend);
        check(!okOf(sNo.invoke(R"({"address":"cid-1","recipient":"bob"})")),
              "and refuses when it has no way to verify the address");
    }
    {
        // The point of share taking two ports: a delivery failure must not be
        // reported as a storage failure.
        StoragePort up{[] { return true; }, {}, {}, {},
                       [](const std::string &) { return true; }};
        SharePort broken{[](const std::string &, const std::string &) { return false; }};
        ShareSkill s(up, broken);
        const auto r = s.invoke(R"({"address":"cid-1","recipient":"bob"})");
        check(!okOf(r), "share fails when delivery fails");
        check(errOf(r).find("could not be delivered") != std::string::npos,
              "and blames delivery, not storage");
    }

#ifdef LP0008_MODULE_TESTS
    const std::string anchored(64, 'a');

    std::printf("module lifecycle\n");
    {
        AgentModuleImpl m;
        check(!m.configure("", anchored).success, "configure refuses an empty owner");
        // Sixty-four characters that are not an address. Checking only the
        // length accepted this, and the misconfiguration surfaced much later as
        // a spend the chain refused.
        check(!m.configure("owner-1", std::string(64, 'z')).success,
              "and refuses a 64-character policy hash that is not hex");
        check(!m.configure("owner-1", std::string(63, 'a')).success,
              "and one of the wrong length");
        check(m.configure("owner-1", anchored).success, "and accepts an anchored policy hash");

        // configure -> start -> configure used to return ok and rebind a running
        // agent to another owner and another policy anchor.
        check(m.start().success, "start succeeds once configured");
        check(!m.configure("attacker", std::string(64, 'b')).success,
              "a second configure is refused");
        const auto st = json::parse(m.status(), nullptr, false);
        check(st.value("owner", std::string{}) == "owner-1",
              "and leaves the owner it was bound to");
        check(st.value("policy", std::string{}) == anchored, "and the policy anchor");
        check(st.value("started", false), "status reports the running agent as started");
        check(!m.start().success, "a second start is refused");
    }
    {
        AgentModuleImpl m;
        check(!m.start().success, "start before configure is refused");
        // Registered but not startable: the skill exists, so a refusal here is
        // the lifecycle refusing rather than a name that was never registered.
        bool called = false;
        auto early = std::make_shared<Fake>("early", "{}");
        early->onInvoke = [&called](const std::string &) {
            called = true;
            return std::string(R"({"ok":true})");
        };
        m.registerSkill(early);
        const auto before = json::parse(m.skills(), nullptr, false);
        check(!before.is_discarded() && before.is_object() && before.contains("error"),
              "skills() before start is an error, not an empty card");
        check(!okOf(m.invoke("early", "{}")) && !called,
              "and invoke before start refuses without reaching the skill");
    }

    std::printf("skills document\n");
    {
        AgentModuleImpl m;
        check(m.configure("owner-1", anchored).success && m.start().success,
              "a configured agent starts");

        // Both halves of every entry are third-party text. Spliced in raw, this
        // name produced {"name":"evil","x":"","parameters":{}} — a forged key in
        // everyone's Agent Card — and an empty schema produced "parameters":
        // with no value at all, which is not a document any consumer can read.
        const std::string evil = R"(evil","x":")";
        check(m.registerSkill(std::make_shared<Fake>(evil, "{}")).success, "a hostile name registers");
        check(m.registerSkill(std::make_shared<Fake>("good", R"({"type":"object"})")).success,
              "as does an ordinary skill");
        m.registerSkill(std::make_shared<Fake>("empty.schema", ""));
        m.registerSkill(std::make_shared<Fake>("not.json", "parameters:"));
        m.registerSkill(std::make_shared<Fake>("not.object", "5"));
        check(!m.registerSkill(std::make_shared<Fake>("good", "{}")).success,
              "and a second skill may not take a name already registered");

        auto thrower = std::make_shared<Fake>("throws", "{}");
        thrower->onSchema = []() -> std::string { throw std::runtime_error("boom"); };
        m.registerSkill(thrower);

        std::string doc;
        bool escaped = false;
        try {
            doc = m.skills();
        } catch (...) {
            escaped = true;
        }
        check(!escaped, "a throwing parameterSchema() does not escape skills()");

        const auto card = json::parse(doc, nullptr, false);
        // Six fakes, on top of the skills the module registers for itself when
        // nobody wired them.
        check(!card.is_discarded() && card.is_array() && card.size() == kBuiltinCount + 6,
              "the card parses, with one entry per registered skill");

        const auto hostile = entryFor(card, evil);
        check(hostile.value("name", std::string{}) == evil,
              "a name carrying quotes is escaped, not spliced");
        check(hostile.size() == 2 && hostile.contains("parameters"),
              "so it cannot add a key of its own to the entry");
        check(doc.find(R"("x":)") == std::string::npos, "nor to the document");

        check(entryFor(card, "good").value("parameters", json::object()).value("type", std::string{})
                  == "object",
              "a valid schema is published as JSON, not as text");
        check(!entryFor(card, "empty.schema").contains("parameters")
                  && !entryFor(card, "empty.schema").value("error", std::string{}).empty(),
              "an empty schema is that skill's error");
        check(!entryFor(card, "not.json").value("error", std::string{}).empty(),
              "so is a schema that does not parse");
        check(!entryFor(card, "not.object").value("error", std::string{}).empty(),
              "so is one that parses but is not an object");
        check(entryFor(card, "throws").value("error", std::string{}).find("boom")
                  != std::string::npos,
              "and a thrown schema is reported as that skill's error");
        // The point of all of the above: discovery survives them.
        check(entryFor(card, "good").contains("parameters"),
              "one bad skill does not cost the others their entry");

        auto badName = std::make_shared<Fake>("unused", "{}");
        badName->onName = []() -> std::string { throw std::runtime_error("nope"); };
        StdLogosResult reg;
        bool escapedName = false;
        try {
            reg = m.registerSkill(badName);
        } catch (...) {
            escapedName = true;
        }
        check(!escapedName && !reg.success, "a throwing name() is reported, not propagated");
    }
    {
        // dump() throws on a string that is not valid UTF-8, and the name in
        // every entry is a third party's.
        AgentModuleImpl m;
        m.configure("owner-1", anchored);
        m.start();
        m.registerSkill(std::make_shared<Fake>(std::string("bad") + char(0xff) + "name", "{}"));
        std::string doc;
        bool escaped = false;
        try {
            doc = m.skills();
        } catch (...) {
            escaped = true;
        }
        check(!escaped && !json::parse(doc, nullptr, false).is_discarded(),
              "a skill name that is not valid UTF-8 still yields a parseable card");
    }
    {
        // Third-party code called under the module's own non-recursive lock. A
        // skill that calls back into the module deadlocked it against itself,
        // which is a hang rather than a failure — hence the watchdog.
        auto *m = new AgentModuleImpl();  // leaked on purpose: see completes()
        m->configure("owner-1", anchored);
        m->start();
        auto reentrant = std::make_shared<Fake>("reentrant.schema", "{}");
        reentrant->onSchema = [m] {
            m->registerSkill(std::make_shared<Fake>("added.while.answering", "{}"));
            return std::string("{}");
        };
        m->registerSkill(reentrant);
        check(completes([m] { m->skills(); }),
              "a skill that calls back into the module does not deadlock skills()");

        auto *m2 = new AgentModuleImpl();
        m2->configure("owner-1", anchored);
        m2->start();
        auto reentrantName = std::make_shared<Fake>("reentrant.name", "{}");
        reentrantName->onName = [m2] { m2->skills(); return std::string("reentrant.name"); };
        check(completes([m2, reentrantName] { m2->registerSkill(reentrantName); }),
              "nor registerSkill()");

        auto *m3 = new AgentModuleImpl();
        m3->configure("owner-1", anchored);
        m3->start();
        auto reentrantInvoke = std::make_shared<Fake>("reentrant.invoke", "{}");
        reentrantInvoke->onInvoke = [m3](const std::string &) {
            m3->skills();
            return std::string(R"({"ok":true})");
        };
        m3->registerSkill(reentrantInvoke);
        check(completes([m3] { m3->invoke("reentrant.invoke", "{}"); }), "nor invoke()");
    }

    std::printf("module invoke\n");
    {
        AgentModuleImpl m;
        m.configure("owner-1", anchored);
        m.start();

        std::string seen;
        auto echo = std::make_shared<Fake>("echo", "{}");
        echo->onInvoke = [&seen](const std::string &p) {
            seen = p;
            return std::string(R"({"ok":true})");
        };
        m.registerSkill(echo);
        check(okOf(m.invoke("echo", R"({"a":1})")) && seen == R"({"a":1})",
              "invoke dispatches to the named skill, with its parameters");
        const auto unknown = m.invoke("nope", "{}");
        check(!okOf(unknown) && errOf(unknown).find("nope") != std::string::npos,
              "an unregistered name is refused, and named");

        auto boom = std::make_shared<Fake>("boom", "{}");
        boom->onInvoke = [](const std::string &) -> std::string { throw std::runtime_error("kaboom"); };
        m.registerSkill(boom);
        std::string thrown;
        bool escaped = false;
        try {
            thrown = m.invoke("boom", "{}");
        } catch (...) {
            escaped = true;
        }
        check(!escaped, "a throwing skill does not escape invoke()");
        check(!okOf(thrown) && errOf(thrown).find("kaboom") != std::string::npos,
              "it comes back as that skill's failure");
        check(okOf(m.invoke("echo", "{}")), "and the module still serves the others");

        auto prose = std::make_shared<Fake>("prose", "{}");
        prose->onInvoke = [](const std::string &) { return std::string("done!"); };
        m.registerSkill(prose);
        check(!okOf(m.invoke("prose", "{}")), "a result that is not JSON is refused, not passed on");

        check(m.stop().success, "stop succeeds");
        check(!okOf(m.invoke("echo", "{}")), "and a stopped agent refuses to invoke");
    }

    std::printf("built-in skills, with nothing wired\n");
    {
        // This is the state a module loaded by Basecamp is actually in. The host
        // reaches it through configure/start/skills/invoke and nothing else —
        // `registerSkill` takes a std::shared_ptr and is not remoteable — so no
        // caller wires the ports, and what this module offers on the other side
        // of the plugin boundary is exactly what is asserted here.
        AgentModuleImpl m;
        m.configure("owner-1", anchored);
        check(m.start().success, "a module nobody wired starts");

        const auto card = json::parse(m.skills(), nullptr, false);
        check(!card.is_discarded() && card.is_array(), "and answers skills() with a card");

        std::string missing;
        std::string schemaless;
        for (const char *name : kBuiltinSkills) {
            const auto entry = entryFor(card, name);
            if (entry.empty()) {
                missing += std::string(missing.empty() ? "" : " ") + name;
            } else if (!entry.contains("parameters") || !entry["parameters"].is_object()) {
                schemaless += std::string(schemaless.empty() ? "" : " ") + name;
            }
        }
        if (!missing.empty()) std::printf("       missing: %s\n", missing.c_str());
        check(missing.empty(), "every skill the module ships with is registered and listed");
        // `schemaless` is only ever appended to in the `else` branch of `if
        // (entry.empty())`, so an EMPTY card leaves it empty and this prints
        // `ok` having checked nothing — the "ok (0 checked)" shape that has
        // shipped here before. `missing` above catches the empty card from the
        // other side, and the count below catches it from a third; this line is
        // now anchored on its own rather than relying on its neighbours.
        check(!card.empty(),
              "the card is not empty, so the schema loop above checked something");
        check(schemaless.empty(),
              "each with a parameter schema another agent can call it from");
        check(card.size() == kBuiltinCount, "and the card lists nothing it cannot dispatch");

        // The module's own status port is the one it can answer honestly without
        // anybody wiring anything, so it is the one skill that works out of the
        // box — and it is how an operator sees the binding from outside.
        const auto status = m.invoke("meta.status", "{}");
        const auto sj = json::parse(status, nullptr, false);
        check(okOf(status), "meta.status answers: the module wired itself into its own status port");
        check(sj.value("configured", false) && sj.value("started", false)
                  && sj.value("owner", std::string{}) == "owner-1"
                  && sj.value("policy_hash", std::string{}) == anchored,
              "and reports what the agent is bound to");
        check(sj.contains("balance") && sj["balance"].is_null(),
              "with an unreadable balance reported as unknown, never as zero");

        // The catalogue the prize asks for by name. Like `meta.status` it needs
        // no port from the host — the module wires it to its own registry — so
        // it answers out of the box, and what it answers has to be the same
        // card `skills()` just produced. It listing itself is the point: it is a
        // registered skill and not a special case inside `invoke()`.
        const auto listed = m.invoke("meta.skills", "{}");
        const auto lj = json::parse(listed, nullptr, false);
        check(okOf(listed) && lj.contains("skills") && lj["skills"].is_array(),
              "meta.skills answers: the module wired itself into its own registry port");
        check(lj["skills"] == card && lj.value("count", std::size_t{0}) == kBuiltinCount,
              "and lists exactly the registry, count included");
        check(!entryFor(lj["skills"], "meta.skills").empty(),
              "including itself, because it is registered rather than special-cased");

        // The other twenty have no port. Each must come back as its own refusal
        // — not as "no skill named", which is the registry saying the skill does
        // not exist, and not as ok:true, and not as a crash on an empty
        // std::function.
        //
        // `meta.status` and `meta.skills` are the two exceptions, and they are
        // exceptions for one reason: both answer questions about *this module*,
        // which it can answer honestly with nothing wired. Everything else is a
        // question about the world outside it.
        const auto selfAnswering = [](const std::string &name) {
            return name == "meta.status" || name == "meta.skills";
        };
        int dispatched = 0;
        int refusedCleanly = 0;
        int notStarted = 0;
        std::string wrong;
        for (const char *name : kBuiltinSkills) {
            std::string answer;
            bool escaped = false;
            try {
                answer = m.invoke(name, "{}");
            } catch (...) {
                escaped = true;
            }
            const auto j = json::parse(answer, nullptr, false);
            if (escaped || j.is_discarded() || !j.is_object()) {
                wrong += std::string(wrong.empty() ? "" : " ") + name;
                continue;
            }
            // Two refusals have to be counted, not one. `agent is not started`
            // does not contain `no skill named`, so a module that never started
            // — or a started one whose registry is empty and whose registry
            // error was reworded — answered all 22 with something this counted
            // as a successful dispatch. Both suites stayed green under exactly
            // that mutation. The second string is now its own tally.
            if (errOf(answer).find("agent is not started") != std::string::npos) ++notStarted;
            if (errOf(answer).find("no skill named") == std::string::npos) ++dispatched;
            if (!selfAnswering(name) && !okOf(answer) && !errOf(answer).empty()) {
                ++refusedCleanly;
            }
        }
        if (!wrong.empty()) std::printf("       not JSON: %s\n", wrong.c_str());
        check(wrong.empty(), "invoking every one of them yields a JSON answer, not an exception");
        check(notStarted == 0,
              "the module answered as a running agent, not 'agent is not started'");
        check(dispatched == static_cast<int>(kBuiltinCount) && kBuiltinCount > 0,
              "each dispatches to a skill rather than answering 'no skill named'");
        check(refusedCleanly == static_cast<int>(kBuiltinCount) - 2,
              "and each unwired one refuses, naming what it is missing");

        // The refusal that matters most: an unwired module must not be able to
        // move money, and must not claim it did.
        const auto send = m.invoke("wallet.send",
                                   std::string(R"({"recipient":"Public/)") + kAgentAccount
                                       + R"(","amount":"1"})");
        const auto sendj = json::parse(send, nullptr, false);
        check(!okOf(send) && sendj.value("submitted", true) == false,
              "wallet.send with no wallet and no owner channel refuses, and says nothing was sent");
    }

    std::printf("built-in skills, wired to ports\n");
    {
        AgentModuleImpl m;
        m.configure("owner-1", anchored);

        SkillPorts ports;
        std::string sentTopic;
        ports.delivery = DeliveryPort{
            [] { return true; },
            [&](const std::string &t, const std::vector<std::uint8_t> &) { sentTopic = t; return true; },
            [](const std::string &) { return true; },
            [](const std::string &) { return true; }};
        ports.storage = StoragePort{[] { return true; },
                                    [](const std::string &, std::int64_t) { return std::string("cid-1"); },
                                    [](const std::string &, const std::string &) { return true; },
                                    [] { return std::string("[]"); },
                                    [](const std::string &c) { return c == "cid-1"; }};
        // The shielded read: the only path to a private balance, and the reason
        // WalletPort has two account functions rather than one.
        ports.agentAccount = std::string("Private/") + kAgentAccount;
        ports.wallet.walletAccount = [](const std::string &) { return std::string(R"({"balance":42})"); };
        ports.sequencer.rpc = [](const std::string &, const std::string &) {
            return std::string(R"({"result":{"id":"7"}})");
        };
        std::map<std::string, std::string> settings;
        ports.config.set = [&](const std::string &k, const std::string &v) {
            settings[k] = v;
            return true;
        };
        ports.config.get = [&](const std::string &k) { return settings.count(k) ? settings[k] : std::string{}; };
        ports.envelope = SpendEnvelope{"1000", "5000", 100};
        ports.inference = std::make_shared<StubLocalBackend>(
            StubLocalBackend::Rules{{"storage.upload"}, 100});

        const auto wired = m.registerBuiltinSkills(ports);
        check(wired.success && wired.value == static_cast<int>(kBuiltinCount),
              "a host that has ports wires every skill through them");
        check(!m.registerBuiltinSkills(ports).success,
              "and may not wire a second copy over the first");
        check(m.start().success, "the wired module starts");
        check(json::parse(m.skills(), nullptr, false).size() == kBuiltinCount,
              "with the same card as the unwired one: wiring changes answers, not the offer");

        check(okOf(m.invoke("messaging.send", R"({"recipient":"abc","message":"hi"})"))
                  && sentTopic == ownerTopic("abc"),
              "messaging.send reaches the delivery port it was registered with");
        check(json::parse(m.invoke("storage.upload", R"({"path":"/tmp/x"})"), nullptr, false)
                      .value("address", std::string{})
                  == "cid-1",
              "storage.upload reaches the storage port");
        check(json::parse(m.invoke("wallet.balance", "{}"), nullptr, false).value("balance", 0) == 42,
              "wallet.balance reads the agent's own shielded account through the wallet");
        check(okOf(m.invoke("program.query", R"({"program_id":"p","method":"getLastBlockId"})")),
              "program.query reaches the sequencer port");
        check(okOf(m.invoke("meta.configure", R"({"key":"price_per_task","value":"5"})"))
                  && settings["price_per_task"] == "5",
              "meta.configure reaches the config port");

        // The one skill whose port is a model. Registration hands it the same
        // envelope wallet.send got, converted once, so the two mirrors of the
        // anchored limits cannot disagree about what is affordable.
        const auto verdict = m.invoke(
            "agent.evaluate_task",
            R"({"task_id":"t1","skill":"storage.upload","price":"50"})");
        const auto vj = json::parse(verdict, nullptr, false);
        check(okOf(verdict) && vj.value("accept", false) && vj.value("from_backend", false),
              "agent.evaluate_task asks the backend it was wired with");
        const auto tooBig = m.invoke(
            "agent.evaluate_task",
            R"({"task_id":"t2","skill":"storage.upload","price":"9000"})");
        check(okOf(tooBig) && !json::parse(tooBig, nullptr, false).value("accept", true),
              "and declines a price outside the envelope registration derived from the mirror");
    }
    {
        AgentModuleImpl m;
        m.configure("owner-1", anchored);
        m.start();
        check(!m.registerBuiltinSkills(SkillPorts{}).success,
              "wiring after start is refused: the card has already been answered");
    }
    {
        // A third party got there first. The built-in must not overwrite it and
        // must not be dropped in silence — and whatever holds the name has to be
        // the thing invoke() reaches, or the card advertises a skill nobody can
        // call.
        AgentModuleImpl m;
        m.configure("owner-1", anchored);
        bool mine = false;
        auto theirs = std::make_shared<Fake>("wallet.send", R"({"type":"object"})");
        theirs->onInvoke = [&mine](const std::string &) {
            mine = true;
            return std::string(R"({"ok":true})");
        };
        check(m.registerSkill(theirs).success, "a third party registers wallet.send first");

        const auto wired = m.registerBuiltinSkills(SkillPorts{});
        check(!wired.success && wired.error.find("wallet.send") != std::string::npos,
              "the built-in of that name is reported as not registered, not dropped quietly");
        check(wired.value == static_cast<int>(kBuiltinCount) - 1, "and the rest are registered");

        m.start();
        const auto card = json::parse(m.skills(), nullptr, false);
        check(card.size() == kBuiltinCount && entriesNamed(card, "wallet.send") == 1,
              "the card carries exactly one wallet.send");
        check(okOf(m.invoke("wallet.send", "{}")) && mine,
              "and it is the one invoke() dispatches to");
    }
    {
        // Registration goes through registerSkill, which takes the module's own
        // non-recursive lock. Doing it from inside start()'s critical section
        // would deadlock on the first skill — a hang, not a failed assertion.
        auto *m = new AgentModuleImpl();  // leaked on purpose: see completes()
        m->configure("owner-1", anchored);
        check(completes([m] { m->start(); }),
              "start registers the built-ins without holding the module's lock");
        check(json::parse(m->skills(), nullptr, false).size() == kBuiltinCount,
              "and the module it started offers them");
    }
#else
    std::printf("module lifecycle / skills document / module invoke\n"
                "  SKIPPED - logos_module_context.h is not on the include path.\n"
                "  Add -I<logos-cpp-sdk>/cpp and module/src/agent_module_plugin.cpp to run them.\n");
#endif

    std::printf("\n%s\n", failures ? "FAILURES" : "all skill behaviours hold");
#ifdef LP0008_MODULE_TESTS
    if (hung) {
        // A detached thread is still blocked on a mutex that will never be
        // released. Leave without running static destructors around it.
        std::fflush(stdout);
        std::_Exit(1);
    }
#endif
    return failures ? 1 : 0;
}
