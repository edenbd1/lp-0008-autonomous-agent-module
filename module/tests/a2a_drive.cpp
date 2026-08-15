// Drive the module's own A2A code from a shell, so a demo can show the real
// thing instead of describing it.
//
//   a2a_drive card --account ID --pay ACCT --price N --signer 'CMD'
//   a2a_drive lifecycle [--card FILE]
//
// WHY THIS EXISTS
//
// `scripts/a2a-task.sh` and `scripts/use-cases/02-services-marketplace.sh` used
// to print
//
//     for state in submitted working completed; do printf '  state -> %s\n' ...
//
// which is three strings and no state machine. `docs/a2a-binding.md` §7.1 says
// of that loop, in its own words, "this document will not describe a printed
// line as a transition" — so the scripts should not either. Everything below
// goes through `TaskStore` and `CardSkill`, the same code the module loads, and
// every transition printed is one the store accepted.
//
// The two modes need nothing from the network. `TaskPort` is a set of
// `std::function`s, so a demo can supply a transport seam that records what
// would have been sent, and a payment seam that pays nothing. That is why this
// runs unattended while the *loaded* module cannot: a loaded plugin is handed
// `SkillPorts{}` and refuses every one of these skills. See §7.1.
//
// `card` mode delegates the key operation to `--signer`, which is
// `scripts/sign-agent-card.py --sign-input`. The module still builds the
// protected header, the payload and the signing input; only the curve
// arithmetic is somebody else's, which is exactly the contract `CardPort.sign`
// declares.
#include "../src/agent_skills.h"

#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

namespace {

std::string arg(int argc, char **argv, const std::string &name, const std::string &fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i)
        if (name == argv[i]) return argv[i + 1];
    return fallback;
}

/// Run `cmd`, feed it `input`, return its stdout. Empty on any failure, which
/// `CardPort.sign` already defines as "the key refused" — and a card that could
/// not be signed is not published.
std::string runSigner(const std::string &cmd, const std::string &input)
{
    const std::string tmp = "/tmp/.a2a_drive_signing_input";
    {
        std::ofstream f(tmp);
        if (!f) return {};
        f << input;
    }
    const std::string full = cmd + " < " + tmp;
    std::unique_ptr<FILE, int (*)(FILE *)> pipe(popen(full.c_str(), "r"), pclose);
    if (!pipe) return {};
    std::string out;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) out += buf.data();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

json jsonOf(const std::string &s)
{
    auto j = json::parse(s, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

std::string slurp(const std::string &p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int driveCard(int argc, char **argv)
{
    const std::string account = arg(argc, argv, "--account");
    const std::string pay = arg(argc, argv, "--pay");
    const std::string signer = arg(argc, argv, "--signer");
    const std::string name = arg(argc, argv, "--name", "logos-storage-agent");
    const std::string desc =
        arg(argc, argv, "--description", "Encrypts and stores a file on Logos Storage");
    const std::uint64_t price = std::strtoull(arg(argc, argv, "--price", "5").c_str(), nullptr, 10);
    if (account.empty() || pay.empty() || signer.empty()) {
        std::fprintf(stderr, "usage: a2a_drive card --account ID --pay ACCT --signer CMD\n");
        return 2;
    }

    CardPort p;
    p.name = [&] { return name; };
    p.description = [&] { return desc; };
    p.version = [&] { return std::string("0.1.0"); };
    p.lezAccount = [&] { return account; };
    p.payAccount = [&] { return pay; };
    p.pricePerTask = [&] { return price; };
    p.providerOrganization = [&] { return std::string("LP-0008 reference agent"); };
    p.providerUrl = [&] { return std::string("https://github.com/logos-co/lambda-prize"); };
    p.skills = [&] {
        return std::string(R"([{"name":"storage.upload","parameters":{"type":"object",)"
                           R"("description":"Encrypt and upload a file, returning a content address"}}])");
    };
    // Deliberately NOT set: `algorithm` and `keyId`. Leaving them empty is what
    // a shipped host does, so this exercises the module's defaults — which is
    // the pair that used to be `EdDSA` over the shielded account and made the
    // module's own card unverifiable by this repository's own verifier.
    p.sign = [&](const std::string &signingInput) { return runSigner(signer, signingInput); };

    CardSkill card(p);
    const json r = jsonOf(card.invoke("{}"));
    if (!r.value("ok", false)) {
        std::fprintf(stderr, "the module refused to produce a card: %s\n",
                     r.value("error", std::string("(no reason given)")).c_str());
        return 1;
    }
    std::printf("%s\n", r["card"].dump(2).c_str());
    return 0;
}

/// One transition, applied to the real store, reported as the store answered.
///
/// `expectRefusalSaying` is not decoration on the refusing calls. `ok ==
/// expectAccepted` asks only that the store was UNSUCCESSFUL, and every refusal
/// this store can produce satisfies that — a malformed event, an unknown task
/// id, a rejected message field. The one call in the lifecycle that expects a
/// refusal is the one the file's own header calls the part that matters most:
/// a completed task cannot be reopened. Adding an unrelated rule to
/// TaskStore::advance (reject an update whose note is empty) made that step
/// refuse for the new reason, print the new reason in its own output, and still
/// exit 0. So a refusal must now also SAY what it is a refusal about.
bool step(TaskStore &tasks, const std::string &taskId, const char *state, const char *note,
          const char *why, bool expectAccepted = true,
          const char *expectRefusalSaying = nullptr)
{
    char ev[512];
    std::snprintf(ev, sizeof ev, R"({"taskId":"%s","status":{"state":"%s","message":"%s"}})",
                  taskId.c_str(), state, note);
    std::string err;
    const bool ok = tasks.applyUpdate(ev, err);
    TaskStore::Task cur;
    tasks.find(taskId, cur);
    std::printf("  %-14s -> %-14s %s   %s\n", ok ? "accepted" : "REFUSED", taskStateName(cur.state).c_str(),
                why, ok ? "" : ("(" + err + ")").c_str());
    if (ok != expectAccepted) return false;
    if (!ok && expectRefusalSaying != nullptr
        && err.find(expectRefusalSaying) == std::string::npos) {
        std::printf("                 but refused for the wrong reason: wanted \"%s\"\n",
                    expectRefusalSaying);
        return false;
    }
    return true;
}

int driveLifecycle(int argc, char **argv)
{
    const std::string cardPath = arg(argc, argv, "--card");
    json peer;
    if (!cardPath.empty()) {
        peer = jsonOf(slurp(cardPath));
        if (peer.is_null() || peer.empty()) {
            std::fprintf(stderr, "could not read an Agent Card from %s\n", cardPath.c_str());
            return 2;
        }
    }
    const std::string agentAddress =
        peer.contains("x-logos") ? peer["x-logos"].value("lezAccount", std::string("peer"))
                                 : std::string("peer");

    std::vector<std::string> sent;
    TaskStore tasks;
    TaskPort t;
    t.ready = [] { return true; };
    t.send = [&sent](const std::string &, const std::string &body) {
        sent.push_back(body);
        return true;
    };
    t.subscribe = [](const std::string &) { return true; };
    // Pays nothing and says so. A demo that settles on chain is
    // `scripts/a2a-task.sh`; this one is about the lifecycle, and a settlement
    // costs real balance.
    t.pay = [](const std::string &, std::uint64_t) { return std::string(); };
    t.refund = [](const std::string &, std::uint64_t) { return std::string(); };
    t.perTxLimit = [] { return std::string("1000"); };
    t.perPeriodLimit = [] { return std::string("1000"); };
    t.spentThisPeriod = [] { return std::string("0"); };

    // A free task, so nothing has to be paid for the lifecycle to be exercised.
    json params{{"agent_address", agentAddress}, {"skill", "storage.upload"}, {"price", 0}};
    TaskSkill ts(t, tasks);
    const json opened = jsonOf(ts.invoke(params.dump()));
    if (!opened.value("ok", false)) {
        std::fprintf(stderr, "agent.task refused: %s\n",
                     opened.value("error", std::string("(no reason)")).c_str());
        return 1;
    }
    const std::string taskId = opened.value("task_id", std::string());
    std::printf("  task %s opened against %s\n", taskId.c_str(), agentAddress.c_str());
    std::printf("  %-14s -> %-14s only TaskStore::create opens a task\n", "created",
                opened.value("state", std::string("?")).c_str());
    if (!sent.empty()) {
        const json req = jsonOf(sent.front());
        std::printf("  and an A2A %s went out for it\n",
                    req.value("method", std::string("?")).c_str());
    }

    int bad = 0;
    bad += !step(tasks, taskId, "working", "reading the file", "the peer picked it up");
    bad += !step(tasks, taskId, "working", "encrypting", "progress repeats working");
    bad += !step(tasks, taskId, "input-required", "passphrase?", "the peer needs input");

    // The answer to an input-required task is a continuation, not a new task.
    json cont{{"agent_address", agentAddress}, {"task_id", taskId}, {"message", "hunter2"}};
    const json c = jsonOf(ts.invoke(cont.dump()));
    std::printf("  %-14s -> %-14s the client answered, as an A2A message/send\n",
                c.value("ok", false) ? "accepted" : "REFUSED",
                c.value("state", std::string("?")).c_str());
    bad += !c.value("ok", false);

    bad += !step(tasks, taskId, "completed", "bzz-1a2b3c", "the peer finished");

    // And the part that matters most: terminal is terminal.
    bad += !step(tasks, taskId, "working", "done already",
                 "a completed task cannot be reopened", false,
                 "completed and cannot move to");

    TaskStore::Task fin;
    tasks.find(taskId, fin);
    std::printf("\n  final    %s (terminal=%s)\n", taskStateName(fin.state).c_str(),
                isTerminalState(fin.state) ? "yes" : "no");
    std::printf("  history  ");
    for (const auto &h : fin.history) std::printf("%s ", h.c_str());
    std::printf("\n  active   %zu task(s)\n", tasks.active().size());
    if (bad != 0) {
        std::fprintf(stderr, "  %d lifecycle step(s) did not behave as the binding requires\n", bad);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "card") return driveCard(argc, argv);
    if (mode == "lifecycle") return driveLifecycle(argc, argv);
    std::fprintf(stderr, "usage: a2a_drive {card|lifecycle} [options]\n");
    return 2;
}
