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
// The module tests need the Logos C++ SDK headers, which are not installed
// everywhere this file is compiled, so they are guarded. Build them with:
//   clang++ -std=c++17 -I<nlohmann>/include -I<logos-cpp-sdk>/cpp \
//     module/tests/skills_test.cpp module/src/messaging_skills.cpp \
//     module/src/storage_skills.cpp module/src/agent_module_plugin.cpp -o skt
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

    std::printf("storage\n");
    {
        StoragePort down{[] { return false; }, {}, {}, {}, {}};
        UploadSkill u(down);
        check(!okOf(u.invoke(R"({"path":"/tmp/x"})")), "upload refuses while storage is down");
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
        const auto before = json::parse(m.skills(), nullptr, false);
        check(!before.is_discarded() && before.is_object() && before.contains("error"),
              "skills() before start is an error, not an empty card");
        check(!okOf(m.invoke("storage.upload", "{}")), "and invoke before start is refused");
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
        check(!card.is_discarded() && card.is_array() && card.size() == 6,
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
        reentrant->onSchema = [m] { m->status(); return std::string("{}"); };
        m->registerSkill(reentrant);
        check(completes([m] { m->skills(); }),
              "a skill that calls back into the module does not deadlock skills()");

        auto *m2 = new AgentModuleImpl();
        m2->configure("owner-1", anchored);
        m2->start();
        auto reentrantName = std::make_shared<Fake>("reentrant.name", "{}");
        reentrantName->onName = [m2] { m2->status(); return std::string("reentrant.name"); };
        check(completes([m2, reentrantName] { m2->registerSkill(reentrantName); }),
              "nor registerSkill()");
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
