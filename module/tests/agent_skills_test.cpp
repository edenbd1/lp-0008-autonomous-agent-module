// Exercise the agent-coordination skills against fakes, with no node, no peer
// and no chain.
//
// The three things a stub gets wrong here are the three things this file is
// about. A card that is emitted unsigned, or with a provider A2A rejects, still
// looks like a card. A discovery that accepts anything which parses still
// returns a list. A lifecycle with no illegal transitions still moves tasks
// around. None of that is visible in a screenshot, and all of it is visible
// here: every check below was confirmed by breaking the code it covers and
// watching this suite go red.
#include "../src/agent_skills.h"

#include <nlohmann/json.hpp>
#include <pthread.h>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static json jsonOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

static bool okOf(const std::string &r)
{
    return jsonOf(r).value("ok", false);
}

static std::string errOf(const std::string &r)
{
    return jsonOf(r).value("error", std::string{});
}

static bool mentions(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// One agent's worth of card material, so a test can spoil exactly one field.
struct FakeAgent {
    std::string name = "logos-storage-agent";
    std::string description = "Encrypts and stores a file on Logos Storage";
    std::string version = "0.1.0";
    std::string account = "7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6";
    std::string pay = "5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ";
    std::uint64_t price = 25;
    std::string org = "LP-0008 reference agent";
    std::string orgUrl = "https://github.com/logos-co/lambda-prize";
    std::string skillsJson =
        R"([{"name":"storage.upload","parameters":{"type":"object","description":"Encrypt and upload a file"}}])";
    bool willSign = true;
    std::string keyId;
    std::string algorithm;

    // What the signing key was asked to sign, and how often.
    mutable std::string lastSigningInput;
    mutable int signCalls = 0;

    CardPort port() const
    {
        CardPort p;
        p.name = [this] { return name; };
        p.description = [this] { return description; };
        p.version = [this] { return version; };
        p.lezAccount = [this] { return account; };
        p.payAccount = [this] { return pay; };
        p.pricePerTask = [this] { return price; };
        p.providerOrganization = [this] { return org; };
        p.providerUrl = [this] { return orgUrl; };
        p.skills = [this] { return skillsJson; };
        p.keyId = [this] { return keyId; };
        p.algorithm = [this] { return algorithm; };
        p.sign = [this](const std::string &in) {
            ++signCalls;
            lastSigningInput = in;
            return willSign ? std::string("c2lnbmF0dXJl") : std::string();
        };
        return p;
    }
};

/// A card that is valid by construction: whatever `agent.card` emits.
static json goodCard()
{
    FakeAgent a;
    CardSkill c(a.port());
    return jsonOf(c.invoke("{}"))["card"];
}

/// Wire the owner's envelope onto a task port, generously enough that a test
/// which is about something else is not accidentally about the envelope.
///
/// Every one of these is *required* for a priced task to go through: an agent
/// that cannot say what its limits are, or what it has already spent, is outside
/// them. So a fake that omits them is testing the refusal, which several tests
/// below do on purpose.
static void insideTheEnvelope(TaskPort &p, const char *perTx = "1000",
                              const char *perPeriod = "1000", const char *spent = "0")
{
    p.perTxLimit = [perTx] { return std::string(perTx); };
    p.perPeriodLimit = [perPeriod] { return std::string(perPeriod); };
    p.spentThisPeriod = [spent] { return std::string(spent); };
}

/// A JSON document nested `depth` levels: `[[[…1…]]]`.
static std::string nested(int depth)
{
    std::string s;
    s.reserve(static_cast<std::size_t>(depth) * 2 + 1);
    for (int i = 0; i < depth; ++i) s += '[';
    s += '1';
    for (int i = 0; i < depth; ++i) s += ']';
    return s;
}

/// A card that passes every A2A rule and carries one junk key nested `depth`
/// deep. 1.7 KB of it was enough to kill the host process.
static std::string deepCard(int depth)
{
    return std::string(
               R"({"protocolVersion":"0.3.0","name":"peer","description":"d",)"
               R"("url":"logos-messaging://peer","preferredTransport":"logos-messaging",)"
               R"("version":"1","capabilities":{},"defaultInputModes":["application/json"],)"
               R"("defaultOutputModes":["application/json"],)"
               R"("skills":[{"id":"storage.upload","name":"n","description":"d","tags":["t"]}],)"
               R"("x-logos":{"lezAccount":"peer","pricePerTask":0},"junk":)") +
           nested(depth) + "}";
}

/// Run `fn` on a thread with a 512 KiB stack — the size of the worker threads
/// this module's skills are actually invoked on.
///
/// This is what makes the depth tests real. A stack overflow is a signal, not an
/// exception: `try`/`catch` cannot see it, the module's own catch does not save
/// it, and the process dies. On the main thread (8 MiB) a deep copy might well
/// survive and the test would pass while the product crashed. Run here, removing
/// the depth bound does not turn these checks red — it kills the suite outright,
/// which is a louder failure and the honest one.
static int runOnSmallStack(void *(*fn)(void *))
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, fn, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) return -1;
    pthread_join(th, nullptr);
    return 0;
}

/// The documents that used to kill the process, exercised where they would.
///
/// Everything in here runs on a 512 KiB stack (see @ref runOnSmallStack). Every
/// one of these calls copies the document it accepts — `summary["card"] = c` in
/// `agent.discover`, `p["params"]` in `agent.task` — and nlohmann's copy
/// constructor recurses once per level, so without the depth bound this function
/// does not fail: it takes the whole suite down with SIGSEGV, exactly as it took
/// the host process down. The *invalid*-card path was always safe, which is why
/// the accepted-card path is what is checked here.
static void *deepDocumentsOnASmallStack(void *)
{
    // The bound itself, first, since everything below trusts it.
    check(withinJsonDepth(nested(kMaxJsonDepth), kMaxJsonDepth), "a document at the limit passes");
    check(!withinJsonDepth(nested(kMaxJsonDepth + 1), kMaxJsonDepth),
          "one level deeper does not");
    check(withinJsonDepth(R"({"note":"[[[[[[{{{{{{"})", kMaxJsonDepth),
          "brackets inside a string are characters, not levels");
    check(withinJsonDepth(R"({"note":"\""})", kMaxJsonDepth),
          "and an escaped quote does not desynchronise the scan");
    check(!withinJsonDepth(nested(600), kMaxJsonDepth), "600 levels is refused");

    // agent.discover: the accepted path, which is the one that copies.
    {
        const std::string atLimit = deepCard(kMaxJsonDepth - 1);
        const std::string overLimit = deepCard(kMaxJsonDepth);
        const std::string wayOver = deepCard(600);
        check(withinJsonDepth(atLimit, kMaxJsonDepth) && !withinJsonDepth(overLimit, kMaxJsonDepth),
              "the two cards sit either side of the limit");
        check(wayOver.size() < 2048, "and the lethal one is under 2 KB, as reported");

        DiscoveryPort p;
        p.fetch = [&](const std::string &) {
            return std::vector<std::string>{atLimit, overLimit, wayOver};
        };
        DiscoverSkill d(p);
        const json r = jsonOf(d.invoke(R"({"topic":"/lp-0008/1/discovery-x/json"})"));
        check(r.value("ok", false), "discovery answers rather than dying");
        check(r["agents"].size() == 1, "the card at the limit is accepted and copied");
        check(r["agents"][0]["card"].contains("junk"),
              "with its whole document, which is what the copy was");
        check(r["rejected"].size() == 2, "and both of the deeper ones are rejected");
        check(mentions(r["rejected"][0].value("reason", std::string{}), "nests deeper"),
              "with a reason that states the limit");
    }

    // agent.task: the second site, `const json inner = p["params"]`.
    {
        TaskStore store;
        std::string sentTo;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [&](const std::string &t, const std::string &) {
            sentTo = t;
            return true;
        };
        TaskSkill task(port, store);

        const std::string deep = R"({"agent_address":"peer","skill":"s","params":{"a":)" +
                                 nested(600) + "}}";
        check(deep.size() < 2048, "1.3 KB of parameters");
        const auto r = task.invoke(deep);
        check(!okOf(r), "agent.task refuses parameters it would have to copy");
        check(mentions(errOf(r), "nests deeper"), "and states the limit");
        check(store.size() == 0 && sentTo.empty(), "with nothing opened and nothing sent");

        // And the same call one level inside the limit still works, so the bound
        // is a bound and not a wall.
        const std::string ok_ = R"({"agent_address":"peer","skill":"s","params":{"a":)" +
                                nested(kMaxJsonDepth - 2) + "}}";
        check(withinJsonDepth(ok_, kMaxJsonDepth), "the accepted call is at the limit");
        check(okOf(task.invoke(ok_)), "a task with parameters at the limit is opened");
    }

    // The public validator, and the two documents that arrive off disk or wire.
    {
        check(!validateAgentCard(deepCard(600)).empty(), "validateAgentCard refuses a deep card");
        TaskStore store;
        std::string err;
        const std::string snap = R"([{"id":"t1","agent":"a","skill":"s","state":"submitted",)"
                                 R"("history":)" + nested(600) + "}]";
        check(!store.restore(snap, err), "a snapshot nested past the limit is refused");
        check(mentions(err, "nests deeper"), "and says so");
        check(!store.applyUpdate(R"({"taskId":"t1","status":)" + nested(600) + "}", err),
              "and so is a status update off the wire");
    }
    return nullptr;
}

int main()
{
    std::printf("documents that must not be copied (on a 512 KiB stack)\n");
    {
        check(runOnSmallStack(deepDocumentsOnASmallStack) == 0,
              "the small-stack thread ran");
    }

    std::printf("base64url (RFC 4648 vectors, unpadded as RFC 7515 requires)\n");
    {
        check(base64Url("") == "", "the empty string");
        check(base64Url("f") == "Zg", "one byte, no padding");
        check(base64Url("fo") == "Zm8", "two bytes, no padding");
        check(base64Url("foo") == "Zm9v", "three bytes");
        check(base64Url("foobar") == "Zm9vYmFy", "six bytes");
        check(base64Url(std::string("\xfb\xff", 2)) == "-_8", "- and _ replace + and /");
    }

    std::printf("agent.card\n");
    {
        FakeAgent a;
        CardSkill c(a.port());
        const auto r = c.invoke("{}");
        check(okOf(r), "a configured agent produces a card");
        const json card = jsonOf(r)["card"];

        // The shape scripts/a2a-task.sh publishes, field for field.
        check(card.value("protocolVersion", std::string{}) == "0.3.0", "A2A protocolVersion 0.3.0");
        check(card.value("url", std::string{}) == "logos-messaging://" + a.account,
              "the url is the agent's Logos Messaging address");
        check(card.value("preferredTransport", std::string{}) == "logos-messaging",
              "and the transport bound to it is logos-messaging");
        check(card["x-logos"].value("lezAccount", std::string{}) == a.account,
              "x-logos carries the LEZ account");
        check(card["x-logos"].value("pricePerTask", std::uint64_t{0}) == 25,
              "and the price per task");
        check(card["x-logos"].value("paymentAccount", std::string{}) == "Public/" + a.pay,
              "and the public account it is paid into");
        check(!card["x-logos"].value("settlement", std::string{}).empty(),
              "and how the payment settles");

        // The skills come from the registry, not from a literal in the card.
        check(card["skills"].size() == 1 &&
                  card["skills"][0].value("id", std::string{}) == "storage.upload",
              "the skills are the registered ones");
        check(card["skills"][0]["tags"].size() == 1 &&
                  card["skills"][0]["tags"][0].get<std::string>() == "storage",
              "with a tag derived from the skill's category");
        check(card["skills"][0].contains("x-logos-parameters"),
              "and the parameter schema a caller needs to invoke it");

        // The audit's second finding: A2A requires provider.url alongside
        // provider.organization, and the published card omitted it.
        check(card["provider"].value("organization", std::string{}) == a.org &&
                  card["provider"].value("url", std::string{}) == a.orgUrl,
              "provider carries both organization and url");

        // The first finding: the card is signed, and signed over itself.
        check(card.contains("signatures") && card["signatures"].size() == 1,
              "the card is signed");
        json unsignedCard = card;
        unsignedCard.erase("signatures");
        const std::string wantProtected =
            base64Url(json{{"alg", "EdDSA"}, {"kid", a.account}}.dump());
        const std::string wantInput = wantProtected + "." + base64Url(unsignedCard.dump());
        check(card["signatures"][0].value("protected", std::string{}) == wantProtected,
              "the JWS protected header names the algorithm and the key");
        check(a.lastSigningInput == wantInput,
              "and the signature is over the card itself, detached-payload JWS");
        check(card["signatures"][0].value("signature", std::string{}) == "c2lnbmF0dXJl",
              "the signature the key produced is the one published");
        check(validateAgentCard(card.dump()).empty(), "the card it emits validates as A2A");
    }
    {
        // An unsigned card is forgeable by anyone who can publish on the topic,
        // so there is no unsigned fallback: refuse instead.
        FakeAgent a;
        a.willSign = false;
        CardSkill c(a.port());
        const auto r = c.invoke("{}");
        check(!okOf(r), "a key that refuses to sign means no card");
        check(mentions(errOf(r), "signed"), "and says the card could not be signed");

        FakeAgent b;
        CardPort noKey = b.port();
        noKey.sign = {};
        CardSkill c2(noKey);
        const auto r2 = c2.invoke("{}");
        check(!okOf(r2), "and an agent with no signing key at all refuses too");
        check(mentions(errOf(r2), "unsigned"), "naming the reason");
    }
    {
        FakeAgent a;
        a.orgUrl.clear();
        CardSkill c(a.port());
        const auto r = c.invoke("{}");
        check(!okOf(r), "an organization with no url is refused, not emitted");
        check(mentions(errOf(r), "A2A requires both"), "citing the rule");
        check(a.signCalls == 0, "and nothing was signed on the way out");
    }
    {
        FakeAgent a;
        a.pay.clear();
        CardSkill c(a.port());
        const auto r = c.invoke("{}");
        check(!okOf(r), "a price with no account to pay it into is refused");
        check(mentions(errOf(r), "has no account to be paid into"),
              "by the agent, before the card is ever assembled");

        FakeAgent free_;
        free_.price = 0;
        free_.pay.clear();
        CardSkill c2(free_.port());
        check(okOf(c2.invoke("{}")), "but a free agent needs no payment account");
    }
    {
        FakeAgent a;
        a.account.clear();
        CardSkill c1(a.port());
        check(!okOf(c1.invoke("{}")), "an agent with no LEZ account has no identity to publish");

        FakeAgent b;
        b.name.clear();
        CardSkill c2(b.port());
        check(!okOf(c2.invoke("{}")), "and one with no name is refused");

        FakeAgent d;
        d.version.clear();
        CardSkill c3(d.port());
        check(!okOf(c3.invoke("{}")), "and one with no version is refused");
    }
    {
        FakeAgent a;
        a.skillsJson = "not json";
        CardSkill c(a.port());
        check(!okOf(c.invoke("{}")), "a registry that answers with garbage is refused, not thrown");

        FakeAgent b;
        b.skillsJson = R"([{"parameters":{}}])";
        CardSkill c2(b.port());
        check(!okOf(c2.invoke("{}")), "and a nameless skill cannot be advertised");

        // The card checks itself against the same validator its readers use.
        // A schema whose description is empty produces a skill A2A rejects, and
        // nothing before the self-check would notice.
        FakeAgent blankDescription;
        blankDescription.skillsJson =
            R"([{"name":"storage.upload","parameters":{"type":"object","description":""}}])";
        CardSkill c4(blankDescription.port());
        const auto r = c4.invoke("{}");
        check(!okOf(r), "a card the agent cannot validate is not published");
        check(mentions(errOf(r), "not a valid A2A card"), "and it says the card is the problem");

        FakeAgent d;
        CardSkill c3(d.port());
        check(!okOf(c3.invoke("not json")), "malformed parameters are refused, not thrown");
    }

    std::printf("Agent Card validation\n");
    {
        const json valid = goodCard();
        check(validateAgentCard(valid.dump()).empty(), "the reference card is valid");

        // Every field A2A v0.3.0 lists in AgentCard.required, one at a time.
        for (const char *key : {"protocolVersion", "name", "description", "url", "version",
                                "capabilities", "defaultInputModes", "defaultOutputModes",
                                "skills"}) {
            json missing = valid;
            missing.erase(key);
            const std::string why = validateAgentCard(missing.dump());
            check(!why.empty() && mentions(why, key),
                  (std::string("a card with no '") + key + "' is rejected, by name").c_str());
        }
    }
    {
        json c = goodCard();
        c["provider"].erase("url");
        check(!validateAgentCard(c.dump()).empty(), "a provider without a url is rejected");

        json d = goodCard();
        d["skills"][0].erase("tags");
        check(!validateAgentCard(d.dump()).empty(), "a skill without tags is rejected");

        json e = goodCard();
        e["skills"][0]["description"] = "";
        check(!validateAgentCard(e.dump()).empty(), "an empty skill description is rejected");

        json f = goodCard();
        f["defaultInputModes"] = json::array();
        check(!validateAgentCard(f.dump()).empty(),
              "an agent that accepts no input mode is rejected");

        json g = goodCard();
        g["capabilities"] = "yes";
        check(!validateAgentCard(g.dump()).empty(), "capabilities of the wrong type is rejected");

        // A2A binds preferredTransport to url, and this is our transport.
        json h = goodCard();
        h["url"] = "https://example.com/a2a";
        check(!validateAgentCard(h.dump()).empty(),
              "a logos-messaging card whose url is not a logos-messaging address is rejected");

        json i = goodCard();
        i["x-logos"].erase("paymentAccount");
        check(!validateAgentCard(i.dump()).empty(),
              "a price with no payment account is rejected");

        json k = goodCard();
        k["signatures"][0].erase("signature");
        check(!validateAgentCard(k.dump()).empty(),
              "a signature block with no signature is rejected");

        check(!validateAgentCard("not json").empty(), "and something that is not JSON at all");
        check(!validateAgentCard("[]").empty(), "and a JSON array that is not a card");
    }

    std::printf("agent.discover\n");
    {
        const std::string valid = goodCard().dump();
        json broken = goodCard();
        broken.erase("capabilities");

        std::string asked;
        DiscoveryPort port{[&](const std::string &t) {
            asked = t;
            return std::vector<std::string>{valid, broken.dump(), "{oops"};
        }};
        DiscoverSkill d(port);
        const auto r = d.invoke(R"({"topic":"/lp-0008/1/discovery-market/json"})");
        const json j = jsonOf(r);
        check(okOf(r), "discovery answers");
        check(asked == "/lp-0008/1/discovery-market/json", "on the topic it was given");
        check(j["agents"].size() == 1, "one usable agent out of three documents");
        check(j["rejected"].size() == 2, "and the other two are rejected, not silently dropped");
        check(mentions(j["rejected"][0].value("reason", std::string{}), "capabilities"),
              "each rejection says which field was missing");
        check(mentions(j["rejected"][1].value("reason", std::string{}), "JSON"),
              "including the one that is not JSON at all");
        check(j["agents"][0]["skills"].size() == 1 &&
                  j["agents"][0]["skills"][0].get<std::string>() == "storage.upload",
              "a discovered agent lists the skills it declared");
        check(j["agents"][0].value("price", std::uint64_t{0}) == 25, "and its price");
        check(j["agents"][0].value("signed", false), "and whether the card was signed");
    }
    {
        json unsignedCard = goodCard();
        unsignedCard.erase("signatures");
        DiscoveryPort port{[&](const std::string &) {
            return std::vector<std::string>{unsignedCard.dump()};
        }};
        DiscoverSkill d(port);
        const auto lax = jsonOf(d.invoke(R"({"topic":"/t"})"));
        check(lax["agents"].size() == 1 && !lax["agents"][0].value("signed", true),
              "an unsigned card is reported as unsigned");
        const auto strict = jsonOf(d.invoke(R"({"topic":"/t","require_signed":true})"));
        check(strict["agents"].empty() && strict["rejected"].size() == 1,
              "and refused outright when the caller requires a signature");
    }
    {
        DiscoverSkill none(DiscoveryPort{});
        check(!okOf(none.invoke(R"({"topic":"/t"})")), "discovery with no transport refuses");

        DiscoveryPort empty{[](const std::string &) { return std::vector<std::string>{}; }};
        DiscoverSkill d(empty);
        check(!okOf(d.invoke(R"({"topic":""})")), "an empty topic is refused");
        check(!okOf(d.invoke("not json")), "malformed parameters are refused, not thrown");
        check(!okOf(d.invoke(R"({"topic":"/t","require_signed":"yes"})")),
              "and a non-boolean require_signed is refused");
        const auto r = d.invoke(R"({"topic":"/t"})");
        check(okOf(r) && jsonOf(r)["agents"].empty(),
              "a topic nobody publishes on is empty, not an error");
    }

    std::printf("task lifecycle\n");
    {
        check(canTransition(TaskState::Submitted, TaskState::Working), "submitted -> working");
        check(canTransition(TaskState::Working, TaskState::InputRequired),
              "working -> input-required");
        check(canTransition(TaskState::InputRequired, TaskState::Working),
              "input-required -> working");
        check(canTransition(TaskState::Working, TaskState::Completed), "working -> completed");
        check(canTransition(TaskState::Working, TaskState::Canceled), "working -> canceled");
        check(canTransition(TaskState::Working, TaskState::Working),
              "working -> working, which is what a progress stream sends");

        check(!canTransition(TaskState::Completed, TaskState::Working),
              "completed -> working is refused");
        check(!canTransition(TaskState::Canceled, TaskState::Working),
              "canceled -> working is refused");
        check(!canTransition(TaskState::Failed, TaskState::Completed),
              "failed -> completed is refused");
        check(!canTransition(TaskState::Completed, TaskState::Completed),
              "and completed -> completed is refused: a terminal state is final");
        check(!canTransition(TaskState::Submitted, TaskState::Submitted),
              "submitted -> submitted is refused: only create opens a task");
        check(!canTransition(TaskState::Working, TaskState::Submitted),
              "nothing goes back to submitted");
        check(!canTransition(TaskState::InputRequired, TaskState::InputRequired),
              "and asking for input twice over is not a transition");

        TaskState parsed = TaskState::Unknown;
        check(taskStateFromName("input-required", parsed) && parsed == TaskState::InputRequired,
              "the wire names are A2A's own");
        check(!taskStateFromName("in_progress", parsed), "and nothing else is a state");
        check(taskStateName(TaskState::Canceled) == "canceled", "canceled, spelled A2A's way");
    }
    {
        TaskStore store;
        std::string err;
        check(store.create("t1", "c1", "peer", "storage.upload", err), "a task opens");
        check(!store.create("t1", "c1", "peer", "storage.upload", err),
              "a duplicate id is refused");
        check(!store.advance("nope", TaskState::Working, {}, err), "an unknown task cannot move");
        check(mentions(err, "no task"), "and says so");

        check(store.advance("t1", TaskState::Working, {}, err), "submitted -> working");
        check(store.advance("t1", TaskState::Completed, {}, err), "working -> completed");
        check(!store.advance("t1", TaskState::Working, {}, err),
              "completed -> working is refused by the store too");
        check(mentions(err, "completed") && mentions(err, "working"),
              "naming both states in the refusal");

        TaskStore::Task t;
        check(store.find("t1", t) && t.history.size() == 3 && t.history[0] == "submitted" &&
                  t.history[1] == "working" && t.history[2] == "completed",
              "and the history records every state, in order");
        check(!store.find("nope", t), "an unknown task is not found");
        check(store.active().empty() && store.size() == 1,
              "a completed task is no longer active, but is still known");
    }
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        check(store.applyUpdate(R"({"taskId":"t1","status":{"state":"working"}})", err),
              "a status update off the wire moves the task");
        check(!store.applyUpdate("{not json", err), "malformed JSON is refused, not thrown");
        check(!store.applyUpdate(R"({"status":{"state":"working"}})", err),
              "an update naming no task is refused");
        check(!store.applyUpdate(R"({"taskId":"t1"})", err), "an update with no status is refused");
        check(!store.applyUpdate(R"({"taskId":"t1","status":{"state":"in_progress"}})", err),
              "a state A2A does not define is refused");
        check(mentions(err, "not an A2A task state"), "and says why");
        check(!store.applyUpdate(R"({"taskId":"t9","status":{"state":"working"}})", err),
              "an update for a task we never opened is refused, not silently created");
        check(store.size() == 1, "and no task was conjured by it");

        check(store.applyUpdate(R"({"taskId":"t1","status":{"state":"working"}})", err),
              "a repeated working update is accepted, as a stream sends it");
        check(store.applyUpdate(
                  R"({"taskId":"t1","status":{"state":"failed","message":"disk full"}})", err),
              "and a failure carries its reason");
        TaskStore::Task t;
        store.find("t1", t);
        check(t.note == "disk full", "which is kept");
        check(!store.applyUpdate(R"({"taskId":"t1","status":{"state":"completed"}})", err),
              "after which nothing can complete it");
    }
    {
        // Pending task state has to survive a restart.
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.advance("t1", TaskState::Working, {}, err);
        store.recordPayment("t1", 25, "Public/pay", "deadbeef", err);
        const std::string snap = store.snapshot();

        TaskStore restarted;
        check(restarted.restore(snap, err), "a snapshot restores");
        TaskStore::Task t;
        check(restarted.find("t1", t) && t.state == TaskState::Working && t.pricePaid == 25 &&
                  t.settlementTx == "deadbeef" && t.history.size() == 2,
              "with state, payment and history intact");
        check(restarted.advance("t1", TaskState::Completed, {}, err),
              "and the restored task still obeys the lifecycle");

        check(!restarted.restore("{not json", err), "a malformed snapshot is refused");
        check(!restarted.restore(R"([{"id":"x","agent":"a","skill":"s","state":"nope"}])", err),
              "a snapshot with a state A2A does not define is refused");
        check(!restarted.restore(R"([{"id":"x","agent":"a","skill":"s","state":"working"},)"
                                 R"({"id":"x","agent":"a","skill":"s","state":"failed"}])",
                                 err),
              "and one that names the same task twice");
        check(restarted.find("t1", t) && t.state == TaskState::Completed,
              "a refused restore leaves the store as it was");
        check(restarted.size() == 1,
              "with nothing half-loaded from the snapshot it refused");
    }
    {
        // A SNAPSHOT IS A FILE, AND A FILE HAS AN AUTHOR.
        //
        // Six fields were read with `value()`, which throws `type_error.302` the
        // moment a key is present with the wrong type. A structured retype of a
        // snapshot produced 40 exceptions escaping a function whose own header
        // says it "refuses malformed JSON". Refusing and throwing are not the
        // same thing, and the caller can only handle one of them.
        const std::pair<const char *, const char *> retyped[] = {
            {"contextId", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","contextId":5}])"},
            {"pricePaid as text", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","pricePaid":"x"}])"},
            {"payAccount", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","payAccount":[]}])"},
            {"settlementTx", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","settlementTx":{}}])"},
            {"subscribed", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","subscribed":"yes"}])"},
            {"note", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","note":7}])"},
            {"a fractional price", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","pricePaid":1.5}])"},
            {"history that is not an array", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","history":"working"}])"},
            {"a history entry that is not a state", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","history":["working",7]}])"},
            {"a history entry A2A does not define", R"([{"id":"t1","agent":"a","skill":"s","state":"submitted","history":["in_progress"]}])"},
        };
        int refused = 0, threw = 0;
        for (const auto &entry : retyped) {
            TaskStore store;
            std::string err;
            try {
                if (!store.restore(entry.second, err)) ++refused;
            } catch (...) {
                ++threw;
            }
            check(store.size() == 0, entry.first);
        }
        check(threw == 0, "no retyped field throws out of restore");
        check(refused == static_cast<int>(sizeof(retyped) / sizeof(retyped[0])),
              "every one of them is refused instead");
    }
    {
        // `"pricePaid": -1` restored cleanly as 18446744073709551615, after which
        // `agent.cancel` called `refund(attacker, 2^64-1)` and reported ok:true.
        // The refusal is checked here and the consequence just below it, because
        // the second is what made the first worth fixing.
        TaskStore store;
        std::string err;
        const char *negative =
            R"([{"id":"t1","agent":"peer","skill":"s","state":"submitted","pricePaid":-1,)"
            R"("payAccount":"Public/ATTACKER","settlementTx":"aa"}])";
        check(!store.restore(negative, err), "a negative price is refused, not converted");
        check(mentions(err, "non-negative"), "and says what it wanted");
        TaskStore::Task t;
        check(!store.find("t1", t), "so the task never enters the store");

        std::string refundedTo;
        std::uint64_t refunded = 0;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [](const std::string &, const std::string &) { return true; };
        port.refund = [&](const std::string &acct, std::uint64_t amount) {
            refundedTo = acct;
            refunded = amount;
            return std::string("refund-tx");
        };
        CancelSkill cancel(port, store);
        check(!okOf(cancel.invoke(R"({"agent_address":"peer","task_id":"t1"})")),
              "and agent.cancel has nothing to cancel");
        check(refunded == 0 && refundedTo.empty(), "so no 2^64 refund is ever requested");
    }
    {
        // The fields still restore when they are the right type, which is the
        // other half of the fix: refusing everything would pass the tests above
        // and lose every pending task across a restart.
        TaskStore store;
        std::string err;
        check(store.restore(R"([{"id":"t1","agent":"peer","skill":"s","state":"working",)"
                            R"("contextId":"c1","pricePaid":25,"payAccount":"Public/p",)"
                            R"("settlementTx":"deadbeef","subscribed":true,"note":"n",)"
                            R"("history":["submitted","working"]}])",
                            err),
              "a well-typed snapshot still restores");
        TaskStore::Task t;
        check(store.find("t1", t) && t.contextId == "c1" && t.pricePaid == 25 &&
                  t.payAccount == "Public/p" && t.settlementTx == "deadbeef" && t.subscribed &&
                  t.note == "n" && t.history.size() == 2,
              "with every field intact");
    }
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        check(!store.recordPayment("t1", 25, "Public/pay", "", err),
              "an amount with no settlement hash is not recorded as a payment");
        check(!store.recordPayment("t9", 25, "Public/pay", "deadbeef", err),
              "and a payment for a task that does not exist is refused");
    }

    std::printf("agent.task\n");
    {
        const json card = goodCard();
        TaskStore store;
        std::vector<std::string> order;
        int payCalls = 0;
        std::string paidTo;
        std::uint64_t paidAmount = 0;
        std::string sentTopic, sentBody;

        TaskPort port;
        port.ready = [] { return true; };
        port.send = [&](const std::string &t, const std::string &b) {
            order.push_back("send");
            sentTopic = t;
            sentBody = b;
            return true;
        };
        port.subscribe = [](const std::string &) { return true; };
        port.pay = [&](const std::string &acct, std::uint64_t amount) {
            order.push_back("pay");
            ++payCalls;
            paidTo = acct;
            paidAmount = amount;
            return std::string("settle-tx");
        };
        insideTheEnvelope(port);

        TaskSkill task(port, store);
        json params{{"agent_address", card["x-logos"]["lezAccount"]},
                    {"skill", "storage.upload"},
                    {"card", card},
                    {"task_id", "t1"},
                    {"params", json{{"path", "/tmp/x"}}}};
        const auto r = task.invoke(params.dump());
        const json j = jsonOf(r);
        check(okOf(r), "a task opens against a discovered card");
        check(j.value("state", std::string{}) == "submitted",
              "and stays submitted: the peer has been asked, not seen to start");
        check(j.value("settlement_tx", std::string{}) == "settle-tx", "the price settled");
        check(paidAmount == 25 && paidTo == "Public/" + FakeAgent{}.pay,
              "into the account the card declared, at the price it declared");
        check(order.size() == 2 && order[0] == "send" && order[1] == "pay",
              "the request goes out before the money does");
        check(sentTopic == taskTopic(card["x-logos"]["lezAccount"], "t1"),
              "on the task's own content topic");
        const json body = jsonOf(sentBody);
        check(body.value("method", std::string{}) == "message/send" &&
                  body["params"]["message"].value("taskId", std::string{}) == "t1",
              "and the body is an A2A message/send naming the task");

        check(!okOf(task.invoke(params.dump())), "the same task id cannot be opened twice");

        // The peer asks for input; the client answers, and that is the one
        // transition a client makes on its own.
        std::string err;
        store.advance("t1", TaskState::InputRequired, {}, err);
        const auto cont = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                           {"task_id", "t1"},
                                           {"message", "yes"}}
                                          .dump());
        check(okOf(cont) && jsonOf(cont).value("state", std::string{}) == "working",
              "answering an input-required task moves it back to working");

        store.advance("t1", TaskState::Completed, {}, err);
        const auto after = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                            {"task_id", "t1"},
                                            {"message", "again"}}
                                           .dump());
        check(!okOf(after), "a completed task cannot be continued");
        check(mentions(errOf(after), "completed"), "and the refusal names the state");
    }
    {
        // A transport that is down must not reach the wallet: a price paid for
        // a request that never left the node buys nothing.
        const json card = goodCard();
        TaskStore store;
        int payCalls = 0;
        TaskPort down;
        down.ready = [] { return false; };
        down.send = [](const std::string &, const std::string &) { return true; };
        down.pay = [&](const std::string &, std::uint64_t) {
            ++payCalls;
            return std::string("tx");
        };
        insideTheEnvelope(down);
        TaskSkill task(down, store);
        const auto r = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                        {"skill", "storage.upload"},
                                        {"card", card}}
                                       .dump());
        check(!okOf(r), "a task refuses while the delivery node is not started");
        check(payCalls == 0, "and nothing was paid");
        check(store.size() == 0, "and no task was opened");
    }
    {
        const json card = goodCard();
        TaskStore store;
        int payCalls = 0;
        TaskPort broken;
        broken.ready = [] { return true; };
        broken.send = [](const std::string &, const std::string &) { return false; };
        broken.pay = [&](const std::string &, std::uint64_t) {
            ++payCalls;
            return std::string("tx");
        };
        insideTheEnvelope(broken);
        TaskSkill task(broken, store);
        const auto r = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                        {"skill", "storage.upload"},
                                        {"card", card},
                                        {"task_id", "t1"}}
                                       .dump());
        check(!okOf(r), "a request that cannot be delivered is a failure");
        check(payCalls == 0, "and still nothing was paid");
        TaskStore::Task t;
        check(store.find("t1", t) && t.state == TaskState::Failed,
              "the task is marked failed rather than left open forever");
    }
    {
        const json card = goodCard();
        TaskStore store;
        TaskPort unsettled;
        unsettled.ready = [] { return true; };
        unsettled.send = [](const std::string &, const std::string &) { return true; };
        unsettled.pay = [](const std::string &, std::uint64_t) { return std::string(); };
        insideTheEnvelope(unsettled);
        TaskSkill task(unsettled, store);
        const auto r = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                        {"skill", "storage.upload"},
                                        {"card", card},
                                        {"task_id", "t1"}}
                                       .dump());
        check(!okOf(r), "a payment that does not settle fails the task");
        check(mentions(errOf(r), "did not settle"), "and says so rather than assuming");
        TaskStore::Task t;
        check(store.find("t1", t) && t.state == TaskState::Failed && t.settlementTx.empty(),
              "with no settlement recorded against it");
    }
    {
        const json card = goodCard();
        TaskStore store;
        int payCalls = 0;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [](const std::string &, const std::string &) { return true; };
        port.pay = [&](const std::string &, std::uint64_t) {
            ++payCalls;
            return std::string("tx");
        };
        insideTheEnvelope(port);
        TaskSkill task(port, store);

        const auto tooDear = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                              {"skill", "storage.upload"},
                                              {"card", card},
                                              {"max_price", 10}}
                                             .dump());
        check(!okOf(tooDear), "a price above the caller's ceiling is refused");
        check(payCalls == 0 && store.size() == 0, "before anything is sent or signed");

        const auto wrongSkill = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                                 {"skill", "wallet.send"},
                                                 {"card", card}}
                                                .dump());
        check(!okOf(wrongSkill), "a skill the card does not advertise is refused");

        const auto wrongAgent = task.invoke(json{{"agent_address", "someone-else"},
                                                 {"skill", "storage.upload"},
                                                 {"card", card}}
                                                .dump());
        check(!okOf(wrongAgent), "a card that belongs to another agent is refused");

        json bad = card;
        bad.erase("capabilities");
        const auto badCard = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                              {"skill", "storage.upload"},
                                              {"card", bad}}
                                             .dump());
        check(!okOf(badCard), "and an invalid card is refused before a task is opened");

        check(!okOf(task.invoke("not json")), "malformed parameters are refused, not thrown");
        check(!okOf(task.invoke(R"({"skill":"storage.upload"})")),
              "a task with no agent address is refused");
        check(!okOf(task.invoke(R"({"agent_address":"peer"})")),
              "and one with no skill is refused");
        check(!okOf(task.invoke(R"({"agent_address":"peer","skill":"s","price":25})")),
              "a price with no account to pay it into is refused");
        check(payCalls == 0 && store.size() == 0, "none of which paid or opened anything");
    }

    std::printf("agent.task and the owner's spending envelope\n");
    {
        // THE ATTACK THIS SECTION EXISTS FOR.
        //
        // Both numbers below come out of a document a stranger wrote. `max_price`
        // is optional, so a call that omits it used to mean "pay whatever the
        // card says, to whoever the card names". Measured before the fix: this
        // exact card produced `pay("Public/ATTACKER", 18446744073709551615)` and
        // `ok:true`.
        json card = goodCard();
        card["x-logos"]["pricePerTask"] = 18446744073709551615ULL;
        card["x-logos"]["paymentAccount"] = "Public/ATTACKER";
        const json ask{{"agent_address", card["x-logos"]["lezAccount"]},
                       {"skill", "storage.upload"},
                       {"card", card}};

        // A port with a wallet and no envelope: everything is unknown, and
        // unknown is outside.
        {
            TaskStore store;
            int payCalls = 0, sends = 0;
            TaskPort port;
            port.ready = [] { return true; };
            port.send = [&](const std::string &, const std::string &) {
                ++sends;
                return true;
            };
            port.pay = [&](const std::string &, std::uint64_t) {
                ++payCalls;
                return std::string("tx");
            };
            TaskSkill task(port, store);
            const auto r = task.invoke(ask.dump());
            check(!okOf(r), "a card-chosen price with no envelope known is refused");
            check(jsonOf(r).value("outcome", std::string{}) == "owner_unreachable",
                  "as an owner who cannot be asked, which is terminal");
            check(payCalls == 0, "nothing was paid");
            check(sends == 0 && store.size() == 0,
                  "and nothing was sent: the peer is never told about a task we will not pay for");
        }
        // An envelope, and an owner who can be reached.
        {
            TaskStore store;
            int payCalls = 0, sends = 0;
            std::string asked;
            TaskPort port;
            port.ready = [] { return true; };
            port.send = [&](const std::string &, const std::string &) {
                ++sends;
                return true;
            };
            port.pay = [&](const std::string &, std::uint64_t) {
                ++payCalls;
                return std::string("tx");
            };
            port.requestApproval = [&](const std::string &req) {
                asked = req;
                return true;
            };
            insideTheEnvelope(port);
            TaskSkill task(port, store);
            const auto r = task.invoke(ask.dump());
            const json j = jsonOf(r);
            check(okOf(r) && j.value("outcome", std::string{}) == "awaiting_owner_approval",
                  "an above-envelope price is held for the owner");
            check(!j.value("submitted", true), "and says plainly that nothing was submitted");
            check(payCalls == 0 && sends == 0 && store.size() == 0,
                  "with nothing paid, nothing sent and no task opened");
            const json q = jsonOf(asked);
            check(q.value("amount", std::string{}) == "18446744073709551615" &&
                      q.value("recipient", std::string{}) == "Public/ATTACKER",
                  "and the owner is asked about this exact payment, in these exact terms");
        }
        // An owner who cannot be reached is terminal, not a fallback.
        {
            TaskStore store;
            int payCalls = 0;
            TaskPort port;
            port.ready = [] { return true; };
            port.send = [](const std::string &, const std::string &) { return true; };
            port.pay = [&](const std::string &, std::uint64_t) {
                ++payCalls;
                return std::string("tx");
            };
            port.requestApproval = [](const std::string &) { return false; };
            insideTheEnvelope(port);
            TaskSkill task(port, store);
            const auto r = task.invoke(ask.dump());
            check(!okOf(r), "an owner who cannot be reached does not become permission");
            check(jsonOf(r).value("outcome", std::string{}) == "owner_unreachable", "and says so");
            check(payCalls == 0 && store.size() == 0, "and still nothing was paid or opened");
        }
    }
    {
        // The two limits separately, at prices a real agent would see. The card
        // asks 25.
        const json card = goodCard();
        const json ask{{"agent_address", card["x-logos"]["lezAccount"]},
                       {"skill", "storage.upload"},
                       {"card", card}};
        const auto attempt = [&](const char *perTx, const char *perPeriod, const char *spent,
                                 std::uint64_t *paid) {
            TaskStore store;
            TaskPort port;
            port.ready = [] { return true; };
            port.send = [](const std::string &, const std::string &) { return true; };
            port.pay = [paid](const std::string &, std::uint64_t amount) {
                *paid = amount;
                return std::string("tx");
            };
            port.requestApproval = [](const std::string &) { return true; };
            insideTheEnvelope(port, perTx, perPeriod, spent);
            TaskSkill task(port, store);
            return jsonOf(task.invoke(ask.dump()));
        };

        std::uint64_t paid = 0;
        json r = attempt("10", "1000", "0", &paid);
        check(r.value("outcome", std::string{}) == "awaiting_owner_approval" && paid == 0,
              "a price over the per-transaction limit goes to the owner");
        check(mentions(r.value("reason", std::string{}), "per-transaction"), "and says which limit");

        paid = 0;
        r = attempt("1000", "1000", "990", &paid);
        check(r.value("outcome", std::string{}) == "awaiting_owner_approval" && paid == 0,
              "a price that fits alone but not in the period total goes to the owner too");
        check(mentions(r.value("reason", std::string{}), "per-period"), "and says which limit");

        paid = 0;
        r = attempt("1000", "1000", "", &paid);
        check(r.value("outcome", std::string{}) == "awaiting_owner_approval" && paid == 0,
              "a period total that is unknown is outside the envelope, not zero");

        paid = 0;
        r = attempt("1000", "1000", "18446744073709551615", &paid);
        check(r.value("outcome", std::string{}) == "awaiting_owner_approval" && paid == 0,
              "and a period total near 2^64 does not add its way back down to affordable");

        paid = 0;
        r = attempt("1000", "1000", "0", &paid);
        check(r.value("ok", false) && paid == 25 && !r.contains("outcome"),
              "a price inside the envelope is paid, with no owner round trip");
    }
    {
        // A free task asks nobody: there is no spend to hold.
        TaskStore store;
        json card = goodCard();
        card["x-logos"]["pricePerTask"] = 0;
        card["x-logos"].erase("paymentAccount");
        int asks = 0;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [](const std::string &, const std::string &) { return true; };
        port.requestApproval = [&](const std::string &) {
            ++asks;
            return true;
        };
        TaskSkill task(port, store);
        const auto r = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                        {"skill", "storage.upload"},
                                        {"card", card}}
                                       .dump());
        check(okOf(r), "a free task opens with no envelope wired at all");
        check(asks == 0, "and the owner is not asked about a payment that is not happening");
    }

    std::printf("the two card validators agree\n");
    {
        // `agent.discover` used to accept and republish a card that `agent.task`
        // threw `type_error.302` on: `paymentAccount` was only type-checked when
        // a non-zero `pricePerTask` was present. Two validators disagreeing about
        // one document is the defect; the type belongs to the field.
        for (const char *shape : {"42", "null", "[]", "{}", "true", R"("")"}) {
            json card = goodCard();
            card["x-logos"]["paymentAccount"] = json::parse(shape);
            const std::string text = card.dump();

            check(!validateAgentCard(text).empty(),
                  "the published validator refuses a card whose paymentAccount is mistyped");

            DiscoveryPort dp;
            dp.fetch = [&](const std::string &) { return std::vector<std::string>{text}; };
            DiscoverSkill d(dp);
            const json dr = jsonOf(d.invoke(R"({"topic":"t"})"));
            check(dr["agents"].empty() && dr["rejected"].size() == 1,
                  "agent.discover rejects it rather than republishing it");

            TaskStore store;
            TaskPort port;
            port.ready = [] { return true; };
            port.send = [](const std::string &, const std::string &) { return true; };
            insideTheEnvelope(port);
            TaskSkill task(port, store);
            bool threw = false;
            std::string r;
            try {
                r = task.invoke(json{{"agent_address", card["x-logos"]["lezAccount"]},
                                     {"skill", "storage.upload"},
                                     {"card", card}}
                                    .dump());
            } catch (...) {
                threw = true;
            }
            check(!threw, "and agent.task returns an error rather than throwing out of invoke()");
            check(!okOf(r), "which is a refusal");
        }
        // The original rule is still there: a price with no payee is refused.
        json noPayee = goodCard();
        noPayee["x-logos"].erase("paymentAccount");
        check(!validateAgentCard(noPayee.dump()).empty(),
              "a card that charges a price and names no account is still refused");
    }

    std::printf("identifiers that would name another content topic\n");
    {
        check(taskTopic("peer", "t1") == "/lp-0008/1/task-peer-t1/json",
              "an ordinary pair still builds the documented topic");
        check(taskTopic(R"(AAA/json", "x":1, "y":")", "t1").empty(),
              "an agent address carrying the topic separator names no topic at all");
        check(taskTopic("peer", "../../t").empty(), "and neither does a task id");
        check(taskTopic("", "t1").empty() && taskTopic("peer", "").empty(),
              "nor does an empty one");

        TaskStore store;
        std::string sentTo;
        int sends = 0;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [&](const std::string &t, const std::string &) {
            sentTo = t;
            ++sends;
            return true;
        };
        port.subscribe = [&](const std::string &t) {
            sentTo = t;
            return true;
        };
        TaskSkill task(port, store);
        const auto r = task.invoke(
            json{{"agent_address", R"(AAA/json", "x":1, "y":")"}, {"skill", "s"}}.dump());
        check(!okOf(r), "agent.task refuses an agent address that is not a topic identifier");
        check(mentions(errOf(r), "content topic"), "and says why");
        check(sends == 0 && store.size() == 0, "with nothing published and no task opened");
        check(!okOf(task.invoke(R"({"agent_address":"peer","skill":"s","task_id":"a/b"})")),
              "and a task id that would do the same");

        // The path a hostile *snapshot* would take: a task restored with an
        // agent id that splices. Nothing may be published on the forged topic.
        std::string err;
        check(store.restore(R"([{"id":"t1","agent":"AAA/json\", \"x\":1, \"y\":\"",)"
                            R"("skill":"s","state":"submitted"}])",
                            err),
              "a snapshot may carry any agent string");
        SubscribeSkill sub(port, store);
        CancelSkill cancel(port, store);
        sentTo.clear();
        check(!okOf(sub.invoke(json{{"agent_address", R"(AAA/json", "x":1, "y":")"},
                                    {"task_id", "t1"}}
                                   .dump())),
              "agent.subscribe refuses to listen on a topic an id forged");
        check(!okOf(cancel.invoke(json{{"agent_address", R"(AAA/json", "x":1, "y":")"},
                                       {"task_id", "t1"}}
                                      .dump())),
              "and agent.cancel refuses to publish on one");
        check(sentTo.empty() && sends == 0, "neither of which touched the wire");
    }

    std::printf("agent.subscribe\n");
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.create("t2", "c2", "peer", "storage.upload", err);
        store.advance("t2", TaskState::Working, {}, err);
        store.advance("t2", TaskState::Completed, {}, err);

        std::string subscribed;
        TaskPort port;
        port.ready = [] { return true; };
        port.subscribe = [&](const std::string &t) {
            subscribed = t;
            return true;
        };
        SubscribeSkill sub(port, store);

        const auto r = sub.invoke(R"({"agent_address":"peer","task_id":"t1"})");
        check(okOf(r), "subscribing to a running task works");
        check(subscribed == taskTopic("peer", "t1"), "on the task's own topic");
        TaskStore::Task t;
        check(store.find("t1", t) && t.subscribed, "and the task records it");

        check(!okOf(sub.invoke(R"({"agent_address":"peer","task_id":"t9"})")),
              "a task that does not exist cannot be subscribed to");
        const auto done_ = sub.invoke(R"({"agent_address":"peer","task_id":"t2"})");
        check(!okOf(done_), "nor a completed one");
        check(mentions(errOf(done_), "completed"), "and the refusal names the state");
        check(!okOf(sub.invoke(R"({"agent_address":"stranger","task_id":"t1"})")),
              "and not somebody else's task");
        check(!okOf(sub.invoke("not json")), "malformed parameters are refused, not thrown");
    }
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        TaskPort down;
        down.ready = [] { return false; };
        down.subscribe = [](const std::string &) { return true; };
        SubscribeSkill sub(down, store);
        check(!okOf(sub.invoke(R"({"agent_address":"peer","task_id":"t1"})")),
              "subscribing refuses while the node is not started");

        TaskPort refuses;
        refuses.ready = [] { return true; };
        refuses.subscribe = [](const std::string &) { return false; };
        SubscribeSkill sub2(refuses, store);
        check(!okOf(sub2.invoke(R"({"agent_address":"peer","task_id":"t1"})")),
              "and when delivery refuses the subscription");
        TaskStore::Task t;
        check(store.find("t1", t) && !t.subscribed, "with nothing recorded either way");
    }

    std::printf("agent.cancel\n");
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.advance("t1", TaskState::Working, {}, err);
        store.recordPayment("t1", 25, "Public/pay", "settle-tx", err);

        std::string cancelBody;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [&](const std::string &, const std::string &b) {
            cancelBody = b;
            return true;
        };
        port.refund = [](const std::string &, std::uint64_t) { return std::string("refund-tx"); };
        CancelSkill cancel(port, store);

        const auto r = cancel.invoke(R"({"agent_address":"peer","task_id":"t1"})");
        const json j = jsonOf(r);
        check(okOf(r) && j.value("state", std::string{}) == "canceled", "a running task cancels");
        check(jsonOf(cancelBody).value("method", std::string{}) == "tasks/cancel",
              "with an A2A tasks/cancel on the wire");
        check(j["refund"].value("amount", std::uint64_t{0}) == 25 &&
                  j["refund"].value("tx", std::string{}) == "refund-tx" &&
                  !j["refund"].value("pending", true),
              "and the refund is reported with the transaction that carried it");

        const auto again = cancel.invoke(R"({"agent_address":"peer","task_id":"t1"})");
        check(!okOf(again), "cancelling it a second time is refused");
        check(mentions(errOf(again), "canceled"), "because it is already canceled");
    }
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.advance("t1", TaskState::Working, {}, err);
        store.advance("t1", TaskState::Completed, {}, err);

        int sendCalls = 0;
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [&](const std::string &, const std::string &) {
            ++sendCalls;
            return true;
        };
        CancelSkill cancel(port, store);

        const auto r = cancel.invoke(R"({"agent_address":"peer","task_id":"t1"})");
        check(!okOf(r), "a completed task cannot be canceled");
        check(sendCalls == 0,
              "and no cancellation is put on the wire for work the peer already finished");
        check(mentions(errOf(r), "completed"), "and the refusal names the state it is in");
        TaskStore::Task t;
        check(store.find("t1", t) && t.state == TaskState::Completed, "and it stays completed");

        const auto missing = cancel.invoke(R"({"agent_address":"peer","task_id":"nope"})");
        check(!okOf(missing), "a task that does not exist cannot be canceled");
        check(mentions(errOf(missing), "no task"), "and the refusal says so");
        check(sendCalls == 0, "with nothing said to a peer about a task nobody opened");
        check(!okOf(cancel.invoke(R"({"agent_address":"stranger","task_id":"t1"})")),
              "and neither can somebody else's task");
        check(!okOf(cancel.invoke("not json")), "malformed parameters are refused, not thrown");
        check(!okOf(cancel.invoke(R"({"task_id":"t1"})")), "and a call with no agent address");
    }
    {
        // A cancel that never leaves the node is not a cancel: the peer keeps
        // working and keeps the money.
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.advance("t1", TaskState::Working, {}, err);

        TaskPort down;
        down.ready = [] { return false; };
        down.send = [](const std::string &, const std::string &) { return true; };
        CancelSkill c1(down, store);
        check(!okOf(c1.invoke(R"({"agent_address":"peer","task_id":"t1"})")),
              "cancelling refuses while the node is not started");
        TaskStore::Task t;
        check(store.find("t1", t) && t.state == TaskState::Working, "and the task is untouched");

        TaskPort lost;
        lost.ready = [] { return true; };
        lost.send = [](const std::string &, const std::string &) { return false; };
        CancelSkill c2(lost, store);
        check(!okOf(c2.invoke(R"({"agent_address":"peer","task_id":"t1"})")),
              "and when the cancellation cannot be delivered");
        check(store.find("t1", t) && t.state == TaskState::Working,
              "the task is still running, because as far as the peer knows it is");
    }
    {
        // Never claim a refund there is no transaction for.
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.advance("t1", TaskState::Working, {}, err);
        store.recordPayment("t1", 25, "Public/pay", "settle-tx", err);

        TaskPort noRefund;
        noRefund.ready = [] { return true; };
        noRefund.send = [](const std::string &, const std::string &) { return true; };
        CancelSkill c1(noRefund, store);
        const json j = jsonOf(c1.invoke(R"({"agent_address":"peer","task_id":"t1"})"))["refund"];
        check(j.value("pending", false) && !j.contains("tx"),
              "with no refund path, the refund is pending and no transaction is claimed");
        check(j.value("amount", std::uint64_t{0}) == 25, "and the amount owed is still stated");

        TaskStore store2;
        store2.create("t2", "c2", "peer", "storage.upload", err);
        store2.advance("t2", TaskState::Working, {}, err);
        store2.recordPayment("t2", 25, "Public/pay", "settle-tx", err);
        TaskPort failing;
        failing.ready = [] { return true; };
        failing.send = [](const std::string &, const std::string &) { return true; };
        failing.refund = [](const std::string &, std::uint64_t) { return std::string(); };
        CancelSkill c2(failing, store2);
        const json j2 = jsonOf(c2.invoke(R"({"agent_address":"peer","task_id":"t2"})"))["refund"];
        check(j2.value("pending", false) && !j2.contains("tx"),
              "and a refund that does not settle is pending too, not reported as paid");

        TaskStore store3;
        store3.create("t3", "c3", "peer", "storage.upload", err);
        TaskPort port;
        port.ready = [] { return true; };
        port.send = [](const std::string &, const std::string &) { return true; };
        CancelSkill c3(port, store3);
        const json j3 = jsonOf(c3.invoke(R"({"agent_address":"peer","task_id":"t3"})"))["refund"];
        check(j3.value("amount", std::uint64_t{1}) == 0 && !j3.value("pending", true),
              "a free task owes no refund");
    }

    std::printf("meta.status\n");
    {
        TaskStore store;
        StatusPort blank;
        StatusSkill status(blank, store);
        const auto r = status.invoke("{}");
        const json j = jsonOf(r);
        check(okOf(r), "status answers even with nothing wired up");
        check(!j.value("configured", true) && !j.value("started", true),
              "reporting the module as neither configured nor started");
        check(j["owner"].is_null() && j["policy_hash"].is_null(),
              "and what it is bound to as null, not as an empty string");
        check(j["balance"].is_null() && j.contains("balance_error"),
              "an unreadable balance is null with a reason, never zero");
    }
    {
        TaskStore store;
        std::string err;
        store.create("t1", "c1", "peer", "storage.upload", err);
        store.create("t2", "c2", "peer", "wallet.send", err);
        store.advance("t2", TaskState::Working, {}, err);
        store.create("t3", "c3", "peer", "storage.list", err);
        store.advance("t3", TaskState::Working, {}, err);
        store.advance("t3", TaskState::Completed, {}, err);

        StatusPort port;
        port.configured = [] { return true; };
        port.started = [] { return true; };
        port.ownerAddress = [] { return std::string("DumJ4LCBnHE9jUu2yxPfqdL14g3v756Gzby6LuT9hE51"); };
        port.policyHash = [] { return std::string("b040065da682204ed56ab2067f5ead745295fe30cb328265ed51d0c4ec749a87"); };
        port.lezAccount = [] { return std::string("7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6"); };
        port.balance = [] { return std::string("175"); };
        port.storage = [] { return std::string(R"({"files":2,"bytes":8192})"); };
        StatusSkill status(port, store);
        const json j = jsonOf(status.invoke("{}"));

        check(j.value("configured", false) && j.value("started", false),
              "a running module reports itself running");
        check(j.value("balance", std::string{}) == "175" && !j.contains("balance_error"),
              "with the balance it could read");
        check(j["storage"].value("files", 0) == 2, "and its storage usage");
        check(mentions(j.value("policy_hash", std::string{}), "b040065d"),
              "and the anchored policy it is bound to");
        check(j["tasks"].value("total", 0) == 3 && j["tasks"].value("active", 0) == 2 &&
                  j["tasks"].value("terminal", 0) == 1,
              "and counts its tasks, active apart from finished");
        check(j["tasks"]["by_state"].value("submitted", 0) == 1 &&
                  j["tasks"]["by_state"].value("working", 0) == 1,
              "broken down by state");
        check(j["tasks"]["open"].size() == 2, "listing only the open ones");
    }
    {
        TaskStore store;
        StatusPort port;
        port.balance = [] { return std::string(); };
        port.storage = [] { return std::string("not json"); };
        StatusSkill status(port, store);
        const json j = jsonOf(status.invoke("{}"));
        check(j["balance"].is_null(), "an empty balance is unknown, not zero");
        check(j["storage"].is_null() && j.contains("storage_error"),
              "and storage that answers with garbage is unknown, not empty");
        check(!okOf(status.invoke("not json")), "malformed parameters are refused, not thrown");
    }

    std::printf("meta.configure\n");
    {
        std::string setKey, setValue;
        ConfigPort port;
        port.set = [&](const std::string &k, const std::string &v) {
            setKey = k;
            setValue = v;
            return true;
        };
        ConfigureSkill cfg(port);

        const auto owner = cfg.invoke(R"({"key":"owner_address","value":"DumJ4LCB"})");
        check(okOf(owner) && setKey == "owner_address" && setValue == "DumJ4LCB",
              "a known key is validated and applied");
        check(jsonOf(owner).value("effective", false), "and takes effect immediately");

        const auto perTx = cfg.invoke(R"({"key":"per_tx","value":"200"})");
        check(okOf(perTx), "the spending envelope can be mirrored locally");
        check(!jsonOf(perTx).value("effective", true) && jsonOf(perTx).contains("note"),
              "but is reported as not effective: that limit lives in the anchored account");

        setKey.clear();
        check(!okOf(cfg.invoke(R"({"key":"spending_limit","value":"200"})")),
              "an unknown key is refused rather than stored");
        check(setKey.empty(), "and never reaches the module");
        check(!okOf(cfg.invoke(R"({"key":"per_tx","value":"lots"})")),
              "a non-numeric limit is refused");
        check(!okOf(cfg.invoke(R"({"key":"per_tx","value":"-1"})")), "and a negative one");
        check(!okOf(cfg.invoke(R"({"key":"policy_hash","value":"abc"})")),
              "a policy hash of the wrong length is refused");
        check(!okOf(cfg.invoke(
                  R"({"key":"policy_hash","value":"zz40065da682204ed56ab2067f5ead745295fe30cb328265ed51d0c4ec749a87"})")),
              "and one that is not hex");
        check(okOf(cfg.invoke(
                  R"({"key":"policy_hash","value":"b040065da682204ed56ab2067f5ead745295fe30cb328265ed51d0c4ec749a87"})")),
              "while a 32-byte hash is accepted");
        check(!okOf(cfg.invoke(R"({"key":"owner_address","value":""})")),
              "an empty owner address is refused");
        check(!okOf(cfg.invoke(R"({"key":"owner_address","value":7})")),
              "a value that is not a string is refused");
        check(!okOf(cfg.invoke(R"({"value":"x"})")), "a call with no key is refused");
        check(!okOf(cfg.invoke("not json")), "malformed parameters are refused, not thrown");
    }
    {
        ConfigPort refuses;
        refuses.set = [](const std::string &, const std::string &) { return false; };
        ConfigureSkill cfg(refuses);
        check(!okOf(cfg.invoke(R"({"key":"owner_address","value":"x"})")),
              "a module that refuses the setting is reported, not papered over");

        ConfigureSkill none(ConfigPort{});
        check(!okOf(none.invoke(R"({"key":"owner_address","value":"x"})")),
              "and a configure with nowhere to store it refuses");
    }

    // The skill the prize names and this repository documented for months
    // without registering. These cases are about the skill itself; that the
    // module registers it and wires it to its own registry is asserted in
    // skills_test.cpp, which is where the registry lives.
    std::printf("meta.skills\n");
    {
        RegistryPort port;
        port.listing = [] {
            return std::string(R"([{"name":"storage.upload","parameters":{"type":"object"}},)"
                               R"({"name":"meta.skills","parameters":{"type":"object"}}])");
        };
        SkillsSkill skills(port);

        const auto all = skills.invoke("{}");
        check(okOf(all), "the catalogue answers");
        const auto j = jsonOf(all);
        check(j.contains("skills") && j["skills"].is_array() && j["skills"].size() == 2,
              "with one entry per registered skill");
        check(j.value("count", std::size_t{0}) == 2, "and a count of them");
        check(j["skills"][0].value("name", std::string{}) == "storage.upload"
                  && j["skills"][0]["parameters"].value("type", std::string{}) == "object",
              "each carrying the parameter schema another agent would call it from");
        check(okOf(skills.invoke("")), "empty parameters are accepted, like meta.status");
        check(!okOf(skills.invoke("not json")),
              "malformed parameters are refused, not thrown");
    }
    {
        // The listing is passed through as the registry built it, including the
        // entry a skill gets when its own parameterSchema() misbehaved. Deriving
        // that isolation a second time here would be a second place for it to be
        // wrong.
        RegistryPort port;
        port.listing = [] {
            return std::string(
                R"([{"name":"bad.schema","error":"parameter schema is not a JSON object"}])");
        };
        SkillsSkill skills(port);
        const auto j = jsonOf(skills.invoke("{}"));
        check(j["skills"][0].value("error", std::string{}).find("not a JSON object")
                  != std::string::npos,
              "a skill whose schema did not parse is listed with its error, not dropped");
    }
    {
        SkillsSkill none(RegistryPort{});
        check(!okOf(none.invoke("{}")),
              "a catalogue with no registry behind it refuses, rather than reporting no skills");

        RegistryPort notJson;
        notJson.listing = [] { return std::string("{}"); };
        check(!okOf(SkillsSkill(notJson).invoke("{}")),
              "and a registry that did not answer with an array is refused");

        // The module answers `{"error":…}` before start. `invoke()` refuses
        // first, so this is only reachable by a host that links the module —
        // and passing the reason on beats reporting an agent with no skills.
        RegistryPort early;
        early.listing = [] { return std::string(R"({"error":"agent is not started"})"); };
        const auto refused = SkillsSkill(early).invoke("{}");
        check(!okOf(refused)
                  && jsonOf(refused).value("error", std::string{}).find("not started")
                         != std::string::npos,
              "a registry that refused says why, in its own words");

        // Bounded before it is parsed, for the reason every other copied
        // document here is: `done()` takes its argument by value and a copy
        // recurses once per level.
        RegistryPort deep;
        deep.listing = [] {
            return std::string("[") + std::string(200, '[') + std::string(200, ']') + "]";
        };
        check(!okOf(SkillsSkill(deep).invoke("{}")),
              "a listing nested past the depth bound is refused, not copied");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all agent-coordination behaviours hold");
    return failures ? 1 : 0;
}
