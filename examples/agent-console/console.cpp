// SPDX-License-Identifier: MIT OR Apache-2.0
//
// `agent-console` — a command-line front end for the LP-0008 agent module.
//
// WHY THIS EXISTS
//
// Two of the prize's criteria need a program rather than a paragraph, and this
// is the smallest program that answers both:
//
//   - "Provide a documented skill interface (module/SDK) that can be used to add
//     new skills without modifying the core agent module." The console `dlopen`s
//     skill libraries named on its command line and registers them through the
//     module's public `registerSkill`. `selftest` turns that into an assertion.
//   - "…step-by-step instructions for deploying and interacting with the agent
//     via CLI…". Before this, every way to reach the module's dispatcher was C++
//     in a test. `agent-console skills` and `agent-console invoke <name> <json>`
//     are the same dispatcher from a shell.
//
// WHAT IT REALLY DOES, AND WHAT IT REALLY CANNOT
//
// This is stated up front because the module's failure mode worth hiding is a
// console that prints plausible JSON for work nothing performed.
//
// It *links* the agent module — the same sources `module/CMakeLists.txt`
// packages into `agent.lgx` — and wires the ports it can honestly wire from a
// command line, which is exactly the read side of the chain:
//
//   wired    SequencerPort::rpc, WalletPort::getAccount, WalletPort::getTransaction
//            → `program.query` and `wallet.balance` really answer, from the
//              sequencer, over the network, and `--sequencer` picks which one.
//   unwired  everything else: Delivery, Storage, the wallet's signing side, the
//            owner channel, an inference backend. Each of those skills refuses
//            and names the port it is missing.
//
// `WalletPort::spend` is deliberately left null. **This console cannot move
// money and is not a wallet.** Spending goes through the anchored policy program
// (`crates/agent-verifier-spel`), and the CLI that does it is
// `scripts/deploy-agents.sh` / `scripts/a2a-task.sh`, which hold keys. A console
// that could sign would be a second, unaudited spending path around the very
// mechanism this submission is about.
//
// It is also not Logos Core. Logos Core loads the module as a Qt plugin and
// reaches it over a transport; `registerSkill` takes a `std::shared_ptr<ISkill>`,
// which has no wire format, so a skill cannot be added to an *already installed*
// module. That boundary is `module/agent_module_plugin_export.h:30-48`, it is
// real, and `docs/skills.md` says what would have to change.

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent_module_interface.h"
#include "agent_module_plugin.h"

namespace {

int failures = 0;

void ok(const std::string &what) { std::cout << "  ok    " << what << "\n"; }
void bad(const std::string &what) { std::cout << "  FAIL  " << what << "\n"; ++failures; }
void check(bool c, const std::string &what) { c ? ok(what) : bad(what); }

void usage(const char *argv0)
{
    std::cerr <<
"usage: " << argv0 << " [options] <command> [arguments]\n"
"\n"
"commands\n"
"  skills                    the registered skills and their parameter schemas, as JSON\n"
"  status                    what the module is bound to and whether it is running\n"
"  invoke <name> <json>      dispatch one skill by name with a JSON parameter object\n"
"  selftest <content>        assert that a --skill library was loaded, registered and ran\n"
"\n"
"options\n"
"  --skill <path>            load a skill library and register it. Repeatable.\n"
"                            The library exports logos_agent_skill_abi_version and\n"
"                            logos_agent_skill_create; examples/skills/notary-digest is one.\n"
"  --sequencer <url>         LEZ JSON-RPC endpoint. Default https://testnet.lez.logos.co\n"
"  --offline                 wire no sequencer; every chain read then refuses\n"
"  --account <id>            the agent's own account for wallet.balance, qualified:\n"
"                            Public/<base58> or Private/<base58>\n"
"  --owner <id>              owner address passed to configure(). Default 'console-owner'\n"
"  --policy <64 hex>         policy anchor passed to configure(). Default is 64 zeros,\n"
"                            which binds nothing: this console performs no spend, and the\n"
"                            module validates the shape of the anchor rather than resolving\n"
"                            it. README §2 derives the real value from artifacts/agents.tsv.\n";
}

// ---------------------------------------------------------------------------
// The one outbound dependency: `curl`, driven through a file so no JSON ever
// passes through a shell quote.
//
// A subprocess rather than a linked HTTP client on purpose. The agent module
// links no HTTP client — that is what `SequencerPort` is for — and an example
// that added one would be demonstrating its own dependencies rather than the
// module's interface.
// ---------------------------------------------------------------------------

std::string tempPath(const char *tag)
{
    const char *dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && *dir != '\0') ? dir : "/tmp";
    if (!base.empty() && base.back() != '/') base.push_back('/');
    return base + "lp0008-console-" + tag + "-" + std::to_string(::getpid()) + ".json";
}

/// One JSON-RPC round trip. Returns the raw body, or "" for *no answer* — which
/// the skills treat as unreachable rather than as a null result, so the two must
/// not be conflated here either.
std::string rpcCall(const std::string &url, const std::string &method, const std::string &paramsJson)
{
    if (url.empty()) {
        return std::string();
    }
    const std::string reqPath = tempPath("req");
    {
        std::ofstream req(reqPath, std::ios::binary);
        if (!req) return std::string();
        req << R"({"jsonrpc":"2.0","id":1,"method":")" << method << R"(","params":)"
            << (paramsJson.empty() ? "[]" : paramsJson) << "}";
    }
    // `--fail-with-body` is deliberately absent: an RPC error body is an answer,
    // and the skills parse it. A transport failure is what must read as empty,
    // and `-s -S` plus a non-zero exit gives that.
    std::string cmd = "curl -s -S -m 30 -X POST '" + url
                    + "' -H 'Content-Type: application/json' --data-binary @'" + reqPath + "' 2>/dev/null";
    std::string out;
    if (FILE *pipe = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof buf, pipe) != nullptr) {
            out += buf;
        }
        const int rc = pclose(pipe);
        std::remove(reqPath.c_str());
        if (rc != 0) {
            return std::string();
        }
        return out;
    }
    std::remove(reqPath.c_str());
    return std::string();
}

/// A dlopen'd skill library, kept alive for the life of the process.
///
/// Never `dlclose`d, and that is correct rather than lazy: the module holds
/// `shared_ptr`s to objects whose vtables live in these libraries, and unloading
/// one before the module is destroyed is a crash in the next destructor.
struct LoadedLibrary {
    void *handle = nullptr;
    std::string path;
};

std::vector<LoadedLibrary> g_libraries;

/// Load one library and hand back the skill it makes. `error` is set and null is
/// returned on any failure — a third-party library is third-party code, so every
/// step of this is checked rather than assumed.
std::shared_ptr<logos::agent::ISkill> loadSkill(const std::string &path, std::string &error)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *e = dlerror();
        error = std::string("dlopen failed: ") + (e != nullptr ? e : "unknown");
        return nullptr;
    }
    g_libraries.push_back(LoadedLibrary{handle, path});

    using AbiFn = int (*)(void);
    using CreateFn = logos::agent::ISkill *(*)(void);
    auto abi = reinterpret_cast<AbiFn>(dlsym(handle, "logos_agent_skill_abi_version"));
    auto create = reinterpret_cast<CreateFn>(dlsym(handle, "logos_agent_skill_create"));
    if (abi == nullptr || create == nullptr) {
        error = "the library exports no logos_agent_skill_abi_version/logos_agent_skill_create";
        return nullptr;
    }
    // Checked *before* `create`, so a library built against a different
    // generation of the interface is a named refusal and not a jump into a
    // vtable whose layout moved.
    const int version = abi();
    if (version != 1) {
        error = "skill ABI version " + std::to_string(version) + ", this console speaks 1";
        return nullptr;
    }
    logos::agent::ISkill *raw = create();
    if (raw == nullptr) {
        error = "the library's factory returned null";
        return nullptr;
    }
    // The deleter runs the virtual destructor, which lands back in the library
    // that allocated the object.
    return std::shared_ptr<logos::agent::ISkill>(raw);
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> skillPaths;
    std::string sequencer = "https://testnet.lez.logos.co";
    std::string account;
    std::string owner = "console-owner";
    std::string policy(64, '0');
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << arg << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--skill")          skillPaths.push_back(next("a path"));
        else if (arg == "--sequencer") sequencer = next("a URL");
        else if (arg == "--offline")   sequencer.clear();
        else if (arg == "--account")   account = next("an account id");
        else if (arg == "--owner")     owner = next("an owner address");
        else if (arg == "--policy")    policy = next("64 hex characters");
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option " << arg << "\n";
            usage(argv[0]);
            return 2;
        } else positional.push_back(arg);
    }
    if (positional.empty()) { usage(argv[0]); return 2; }
    const std::string command = positional[0];

    // ---- the module ------------------------------------------------------
    AgentModuleImpl agent;
    const StdLogosResult configured = agent.configure(owner, policy);
    if (!configured.success) {
        std::cerr << "configure() refused: " << configured.error << "\n";
        return 1;
    }

    // ---- the ports this console can honestly supply -----------------------
    logos::agent::SkillPorts ports;
    ports.agentAccount = account;
    if (!sequencer.empty()) {
        ports.sequencer.rpc = [sequencer](const std::string &method, const std::string &params) {
            return rpcCall(sequencer, method, params);
        };
        ports.wallet.getAccount = [sequencer](const std::string &id) {
            return rpcCall(sequencer, "getAccount", "[\"" + id + "\"]");
        };
        ports.wallet.getTransaction = [sequencer](const std::string &tx) {
            return rpcCall(sequencer, "getTransaction", "[\"" + tx + "\"]");
        };
    }
    // ports.wallet.spend stays null. See the header comment: this console cannot
    // sign, so `wallet.send` refuses rather than submitting anything.

    // ---- third-party skills, before start() -------------------------------
    //
    // Before, because `skills()` opens for business at start() and a skill that
    // appeared afterwards would be missing from a card already published.
    std::vector<std::string> registeredNames;
    for (const std::string &path : skillPaths) {
        std::string error;
        std::shared_ptr<logos::agent::ISkill> skill = loadSkill(path, error);
        if (!skill) {
            std::cerr << "could not load " << path << ": " << error << "\n";
            return 1;
        }
        const StdLogosResult r = agent.registerSkill(skill);
        if (!r.success) {
            std::cerr << "registerSkill refused " << path << ": " << r.error << "\n";
            return 1;
        }
        registeredNames.push_back(skill->name());
    }

    // Once. Called twice, the second call is refused as already-claimed, and a
    // console that reported that refusal would be reporting its own bug.
    const StdLogosResult builtins = agent.registerBuiltinSkills(ports);
    if (!builtins.success) {
        // Not fatal by itself: a third-party skill holding a built-in's name
        // costs that built-in and nothing else, which is the documented
        // behaviour, so report it and carry on.
        std::cerr << "note: " << builtins.error << "\n";
    }
    const StdLogosResult started = agent.start();
    if (!started.success) {
        std::cerr << "start() refused: " << started.error << "\n";
        return 1;
    }

    // ---- commands ---------------------------------------------------------
    if (command == "skills") {
        std::cout << agent.skills() << "\n";
        return 0;
    }
    if (command == "status") {
        std::cout << agent.invoke("meta.status", "{}") << "\n";
        return 0;
    }
    if (command == "invoke") {
        if (positional.size() < 2) {
            std::cerr << "invoke needs a skill name\n";
            return 2;
        }
        const std::string params = positional.size() >= 3 ? positional[2] : "{}";
        std::cout << agent.invoke(positional[1], params) << "\n";
        return 0;
    }
    if (command == "selftest") {
        // The assertion behind the usability criterion. Each step is checked and
        // the exit code is the result; `run.sh` then checks the digest against
        // `shasum -a 256`, which is the part this program cannot fake.
        const std::string content = positional.size() >= 2
                                  ? positional[1]
                                  : std::string("LP-0008 third-party skill");
        if (registeredNames.empty()) {
            std::cerr << "selftest needs at least one --skill\n";
            return 2;
        }
        std::cout << "\n[1/4] a library the agent module was never compiled against\n";
        for (std::size_t i = 0; i < skillPaths.size(); ++i) {
            ok("dlopen'd " + skillPaths[i] + " -> '" + registeredNames[i] + "'");
        }

        std::cout << "\n[2/4] registered through the module's public registerSkill()\n";
        const std::string card = agent.skills();
        for (const std::string &name : registeredNames) {
            check(card.find("\"" + name + "\"") != std::string::npos,
                  "skills() advertises " + name + " alongside the built-ins");
        }
        // Registering the same name twice must be refused, not silently
        // overwritten: one library shadowing another's `wallet.send` is the
        // failure this guards.
        std::string reloadError;
        std::shared_ptr<logos::agent::ISkill> again = loadSkill(skillPaths.front(), reloadError);
        if (again) {
            const StdLogosResult dup = agent.registerSkill(again);
            check(!dup.success, "a second skill of the same name is refused: " + dup.error);
        } else {
            bad("could not reload the library for the duplicate-name check: " + reloadError);
        }

        std::cout << "\n[3/4] invoked by name through the module's own dispatcher\n";
        std::string params = "{\"content\":\"";
        for (const char c : content) {
            if (c == '"' || c == '\\') params.push_back('\\');
            params.push_back(c);
        }
        params += "\"}";
        std::cout << "  ->    " << params << "\n";
        const std::string answer = agent.invoke("notary.digest", params);
        std::cout << "  <-    " << answer << "\n";
        check(answer.find("\"ok\":true") != std::string::npos,
              "the skill answered through the module");
        const std::size_t at = answer.find("\"digest\":\"");
        if (at != std::string::npos && at + 74 <= answer.size()) {
            std::cout << "DIGEST " << answer.substr(at + 10, 64) << "\n";
        } else {
            bad("the answer carried no 64-character digest");
        }

        std::cout << "\n[4/4] a failing call costs the call, not the module\n";
        const std::string refused = agent.invoke("notary.digest", "not json at all");
        std::cout << "  <-    " << refused << "\n";
        check(refused.find("\"ok\":false") != std::string::npos, "malformed parameters are refused");
        const std::string missing = agent.invoke("notary.digest", "{}");
        std::cout << "  <-    " << missing << "\n";
        check(missing.find("\"ok\":false") != std::string::npos, "a missing 'content' is refused");
        check(agent.invoke("notary.digest", params) == answer,
              "and the same call afterwards returns the same answer");
        const std::string unwired = agent.invoke("storage.upload", "{\"path\":\"/tmp/x\"}");
        std::cout << "  <-    " << unwired << "\n";
        check(unwired.find("\"ok\":false") != std::string::npos,
              "an unwired built-in refuses and names what it is missing");

        std::cout << "\n"
                  << (failures == 0 ? "the module accepted a skill it was never built with"
                                    : "FAILED")
                  << " (" << failures << " failure(s))\n";
        return failures == 0 ? 0 : 1;
    }

    std::cerr << "unknown command '" << command << "'\n";
    usage(argv[0]);
    return 2;
}
