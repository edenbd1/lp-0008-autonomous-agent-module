// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The five skills the audit found unexercised, called AS SKILLS, against real
// nodes.
//
//   skills-live-drive <mode> <run-id> <data-dir> <upload-path> <download-path> <control-path>
//
//   mode = module-port   the storage skills, and messaging.create_group through
//                        the DeliveryPort the MODULE builds for itself
//        = host-port     messaging.create_group through a DeliveryPort this
//                        process builds, on its own node. No storage node.
//
// Built and run by scripts/skills-live.sh, which owns the paths and does the
// `shasum -a 256` comparison. Every step here is an assertion and the exit code
// is the result, the same contract as module/tests/storage_node_drive.c and
// scripts/use-cases/vault_drive.c.
//
// WHAT WAS MISSING, EXACTLY
//
// `docs/skills.md` recorded that `storage.upload`, `storage.download`,
// `storage.list`, `storage.share` and `messaging.create_group` were written
// against the real APIs, compiled, and tested against fake ports. The two things
// that existed sat either side of them. `scripts/exercise-nodes.sh` drives the
// Storage and Delivery LIBRARIES through C drivers — the node proven, not the
// skill. `scripts/use-cases/01-file-vault.sh` drives the storage library through
// `vault_drive.c`, so the "personal file vault" use case never called a storage
// skill at all. Between the two, nothing had ever asked
// `AgentModuleImpl::invoke("storage.upload", …)` and watched a content address
// come back.
//
// Every call below is `agent.invoke("<skill>", "<json>")` on the module's own
// dispatcher. Nothing calls a skill object directly, and nothing reaches past
// the module into a library except through a port the module was handed.
//
// WHAT THE FIRST RUN FOUND, AND WHAT WAS DONE ABOUT IT
//
// Two defects, both invisible to every fake, both in the ports the module builds
// for ITSELF — which are the only ports a loaded plugin has.
//
// 1. There was no storage port at all. `installBuiltinSkills` consumed
//    `ports.storage` verbatim, so a plugin loaded by Basecamp — handed
//    `SkillPorts{}` — answered "storage node is not started" to all four
//    `storage.*` skills in every shipped configuration. The refusal was true and
//    misleading: the node was not down, there was nothing that could open one.
//    `module/src/storage_skills.cpp` now carries a `StorageRuntime` on the same
//    argument `DeliveryRuntime` already made — a host cannot PASS a
//    `std::function` over Qt Remote Objects, and that is not the module being
//    unable to CONSTRUCT one.
//
// 2. `messaging.create_group` was refused by a real node through the module's
//    own port. `DeliveryRuntime::deliveryPort()` wired
//
//        port.channelCreate = [this](const std::string &channelId) {
//            return channelCreate(channelId, channelId, channelId);
//        };
//
//    — the group id passed as the CONTENT TOPIC. A real node subscribes to that
//    topic before opening the channel and a bare identifier is not one, so the
//    skill answered "delivery refused to create the channel" through the port a
//    loaded plugin has, and succeeded through a port a host supplied.
//    `owner_channel.cpp` passes `ownerTopic(account)` in that position and has
//    always worked live, which is why nothing caught it. It now passes
//    `groupTopic(channelId)`.
//
// Both are fixed in `module/src`, which makes the shipped `agent.lgx` stale and
// forces a repackage. `module-port` below asserts the fixed behaviour and
// `host-port` keeps the measurement that established defect 2, on one node, two
// calls apart — so neither can regress quietly in either direction.
//
// WHERE EACH PORT COMES FROM
//
//   delivery   `module-port`: the MODULE'S OWN — `meta.configure("delivery","on")`
//              makes `DeliveryRuntime` open a node and build `DeliveryPort` out
//              of it, the same path a plugin loaded by Basecamp takes.
//              `host-port`: this process opens the node and builds the port.
//   storage    the MODULE'S OWN, and this is the change.
//              `meta.configure("storage_data_dir", …)` then
//              `meta.configure("storage","on")` makes `StorageRuntime` open a
//              Logos Storage node and build `StoragePort` out of it. THIS FILE
//              LINKS NO STORAGE LIBRARY AND INCLUDES NO STORAGE HEADER — it
//              cannot open a node even if it wanted to, and every content
//              address below therefore came out of one the module opened for
//              itself, through the same `dlopen` a loaded plugin uses.
//   share      the MODULE'S OWN. `SharePort::send` derives the recipient's owner
//              topic and publishes on the module's Delivery node. Sharing a
//              content address IS a messaging act, so this is two of the
//              module's own halves wired together rather than a transport
//              invented for the occasion.
//
// `SkillPorts` in `module-port` is left COMPLETELY EMPTY. That is the assertion
// this file exists to make: no host cooperation of any kind, which is the
// situation of every plugin Basecamp loads.
//
// THE CONTROLS, AND WHY EACH ONE COULD FAIL
//
//   1. Before either node exists, `storage.upload` and `messaging.create_group`
//      must REFUSE, naming the node that is not started. Without this, every
//      `ok:true` below is consistent with skills that answer ok to anything.
//   2. The control content address is not merely "the real one with a character
//      changed". `scripts/use-cases/vault_drive.c` measured `storage_exists`
//      answering *true* for such a mutant — it is a datastore key lookup and a
//      near miss can land on a key that is present — so a mutant is SEARCHED for
//      until the node itself says it does not hold it. The search asks through
//      `storage.download`, because this process has no way to ask the node
//      directly any more: an address the node holds is written out, one it does
//      not is "no such content address". Then `storage.download` and
//      `storage.share` must both refuse it and `storage.list` must not name it.
//   3. The share and the invitations are read back off the topics they were
//      published on, through `messaging.receive`. A node receives its own
//      publications, so this proves the frame went through the node's relay path
//      and came back — NOT that a second peer received it. Two processes, two
//      nodes, is `scripts/delivery-in-plugin.sh peers`, and this file does not
//      claim it.
//   4. The refusal of the control share must leave the topic with the same
//      number of frames on it. A `storage.share` that refused an address and
//      published anyway would pass every other check here.
//   5. In `host-port`, the bare-id `channelCreate` is called directly on the
//      SAME node that the skill then succeeds against. One call fails and the
//      other does not, and the only difference is the content topic — which is
//      what makes defect 2 a measurement rather than an inference, and keeps it
//      measured after the fix.
//
// The digest comparison is deliberately NOT here. `scripts/skills-live.sh` runs
// `shasum -a 256` over the file that went in and the file that came back, and
// runs it again over an altered copy that must not match — a driver comparing
// its own bytes to its own bytes would be checking itself.

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent_module_interface.h"
#include "agent_module_plugin.h"
#include "delivery_runtime.h"
#include "messaging_skills.h"
#include "storage_skills.h"

using nlohmann::json;

namespace {

int failures = 0;

void step(const std::string &what) { std::printf("\n%s\n", what.c_str()); }
void note(const std::string &what) { std::printf("  <-    %s\n", what.c_str()); }
void ok(const std::string &what) { std::printf("  ok    %s\n", what.c_str()); }
void bad(const std::string &what) { std::printf("  FAIL  %s\n", what.c_str()); ++failures; }
void check(bool c, const std::string &what) { c ? ok(what) : bad(what); }

/// One line of the transcript the shell reads, so it never has to scrape a
/// human-facing line for a value.
void emit(const char *key, const std::string &value)
{
    std::printf("%s %s\n", key, value.c_str());
}

// ---------------------------------------------------------------------------
// THE STORAGE NODE THIS FILE DOES NOT OWN
//
// There used to be one here: a `StorageNode` struct and a `livePort` that built
// a `StoragePort` out of it, because nothing in `module/src` could open a Logos
// Storage node and a port therefore had to come from a host that links the
// module. Both are gone, and their absence is the assertion.
//
// The module opens its own node now (`StorageRuntime`, in
// `module/src/storage_skills.cpp`), reached through `meta.configure` — two
// strings, which is all a loaded plugin can be sent. So this file includes no
// storage header, links no storage library, and has no way to open a node or to
// ask one a question directly. Every content address below came back through
// `AgentModuleImpl::invoke()` from a node the MODULE opened, by the same
// `dlopen` path a plugin loaded by Basecamp takes.
//
// Keeping a second copy of the node-opening code here would have been the worse
// outcome even though it compiles: two implementations of the same lifecycle,
// one of them the one that ships and the other the one that is tested.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Talking to the module
// ---------------------------------------------------------------------------

json call(AgentModuleImpl &agent, const char *skill, const json &params)
{
    const std::string answer = agent.invoke(skill, params.dump());
    auto parsed = json::parse(answer, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return json{{"ok", false},
                    {"error", "the module returned something that is not a JSON object: "
                                  + answer}};
    }
    return parsed;
}

bool answered(const json &r) { return r.contains("ok") && r["ok"].is_boolean() && r["ok"]; }

/// A refusal, and the message it refused with — `false` for a call that never
/// arrived, which is not the same thing and must not read as one.
bool refused(const json &r)
{
    return r.contains("ok") && r["ok"].is_boolean() && !r["ok"] && r.contains("error")
           && r["error"].is_string() && !r["error"].get<std::string>().empty();
}

std::string errorOf(const json &r)
{
    return r.contains("error") && r["error"].is_string() ? r["error"].get<std::string>()
                                                         : std::string();
}

std::string compact(const json &r)
{
    const std::string s = r.dump();
    return s.size() > 400 ? s.substr(0, 400) + "…" : s;
}

/// The module's own Delivery node, out of `meta.status`.
json deliveryStatus(AgentModuleImpl &agent)
{
    const json s = call(agent, "meta.status", json::object());
    return s.contains("delivery") && s["delivery"].is_object() ? s["delivery"] : json::object();
}

/// The module's own Storage node, out of `meta.status`.
json storageStatus(AgentModuleImpl &agent)
{
    const json s = call(agent, "meta.status", json::object());
    return s.contains("storage") && s["storage"].is_object() ? s["storage"] : json::object();
}

/// Wait for the module's own Storage node, reporting a failure with the words it
/// failed with — which for a missing library names every path that was tried.
bool waitModuleStorage(AgentModuleImpl &agent, int seconds)
{
    for (int i = 0; i < seconds; ++i) {
        const json d = storageStatus(agent);
        const std::string state = d.value("state", "");
        if (state == "ready") return true;
        if (state == "failed") {
            note("storage failed: " + d.value("error", std::string()));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool waitModuleDelivery(AgentModuleImpl &agent, int seconds)
{
    for (int i = 0; i < seconds; ++i) {
        const json d = deliveryStatus(agent);
        const std::string state = d.value("state", "");
        if (state == "ready") return true;
        if (state == "failed") {
            note("delivery failed: " + d.value("error", std::string()));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool waitRuntime(logos::agent::DeliveryRuntime &rt, int seconds)
{
    for (int i = 0; i < seconds; ++i) {
        if (rt.ready()) return true;
        if (rt.state() == "failed") {
            note("delivery failed: " + rt.lastError());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

/// Every frame currently on `topic`, through `messaging.receive` — which also
/// SUBSCRIBES on first use, so the first call has to happen before anything is
/// published or the node will not be listening when it is.
std::vector<std::string> framesOn(AgentModuleImpl &agent, const std::string &topic, bool &okOut)
{
    const json r = call(agent, "messaging.receive", json{{"topic", topic}});
    okOut = answered(r);
    std::vector<std::string> out;
    if (!okOut || !r.contains("messages") || !r["messages"].is_array()) return out;
    for (const auto &m : r["messages"]) {
        if (m.contains("message") && m["message"].is_string()) {
            out.push_back(m["message"].get<std::string>());
        }
    }
    return out;
}

/// Poll `topic` until a frame equal to `want` shows up, or the time runs out.
/// Reports how many frames are on the topic either way, so a caller can tell
/// "nothing arrived" from "something arrived and it was not this".
bool waitForFrame(AgentModuleImpl &agent, const std::string &topic, const std::string &want,
                  int seconds, std::size_t &total)
{
    for (int i = 0; i < seconds; ++i) {
        bool read = false;
        const std::vector<std::string> frames = framesOn(agent, topic, read);
        total = frames.size();
        for (const std::string &f : frames) {
            if (f == want) return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

// ---------------------------------------------------------------------------
// The control address
// ---------------------------------------------------------------------------

const char kBase58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/// Whether the node holds `cid`, asked THROUGH THE MODULE.
///
/// This process has no storage port and no library, so there is no
/// `storage_exists` to call: the question goes through `storage.download` into a
/// scratch path, and the module's own `DownloadSkill` answers it. That skill
/// asks `exists` before it fetches anything, so "no such content address" is the
/// node saying it does not hold the address — and any other outcome, including
/// success, means it does. Being routed through the shipped skill rather than
/// around it is the point: the probe and the thing under test are the same code
/// path, which is what makes the control a control.
bool nodeHolds(AgentModuleImpl &agent, const std::string &cid, const std::string &scratchPath)
{
    const json r = call(agent, "storage.download", json{{"address", cid}, {"path", scratchPath}});
    if (answered(r)) {
        std::remove(scratchPath.c_str());
        return true;
    }
    return errorOf(r) != "no such content address";
}

/// An address the node itself says it does not hold, derived from one it does.
///
/// Six characters of the digest are replaced, not one, and then the node is
/// ASKED. `vault_drive.c` recorded `storage_exists` answering true for a
/// one-character mutant — it is a key lookup, and a near miss can land on a key
/// that is present — so a control that was merely "well formed and different"
/// would sometimes be an address the node holds, and the refusals it exists to
/// provoke would be the wrong refusals. The prefix is left alone: what is under
/// test is the address of the content, not the shape of the string.
std::string absentAddress(AgentModuleImpl &agent, const std::string &cid,
                          const std::string &scratchPath, int &tries)
{
    tries = 0;
    if (cid.size() < 20) return {};
    for (std::size_t attempt = 0; attempt < 24; ++attempt) {
        std::string candidate = cid;
        for (std::size_t k = 0; k < 6; ++k) {
            const std::size_t at = candidate.size() - 2 - k;
            const char had = candidate[at];
            char put = kBase58[(attempt * 7 + k * 11 + 3) % (sizeof kBase58 - 1)];
            if (put == had) put = kBase58[(attempt * 7 + k * 11 + 4) % (sizeof kBase58 - 1)];
            candidate[at] = put;
        }
        ++tries;
        if (candidate == cid) continue;
        if (!nodeHolds(agent, candidate, scratchPath)) return candidate;
    }
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s <module-port|host-port> <run-id> <data-dir> <upload-path> "
                     "<download-path> <control-download-path>\n",
                     argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const std::string runId = argv[2];
    const std::string dataDir = argv[3];
    const std::string uploadPath = argv[4];
    const std::string downloadPath = argv[5];
    const std::string controlPath = argv[6];
    if (mode != "module-port" && mode != "host-port") {
        std::fprintf(stderr, "unknown mode '%s'\n", mode.c_str());
        return 2;
    }
    const bool moduleMode = (mode == "module-port");

    // Identifiers that can go into a content topic — letters, digits, '-' and
    // '_' — because `messaging.send` and `messaging.create_group` refuse
    // anything else, and being refused for the wrong reason is not a control.
    const std::string owner = "lp0008owner" + runId;
    const std::string group = "lp0008group" + runId;
    const std::string member = "lp0008member" + runId;

    // The host's own Delivery node, used only in `host-port`. Declared here so
    // it outlives the module: the port's lambdas capture it.
    logos::agent::DeliveryRuntime hostNode;

    AgentModuleImpl agent;
    const StdLogosResult configured =
        agent.configure("lez1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
                        "8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90");
    if (!configured.success) {
        std::fprintf(stderr, "configure() refused: %s\n", configured.error.c_str());
        return 1;
    }

    // EMPTY, in `module-port`, and that is the whole assertion. No storage port,
    // no share port, no delivery port — the state of every plugin Basecamp
    // loads. `installBuiltinSkills` fills all three from nodes the module opens
    // for itself, and nothing below is reached through anything this process
    // wired.
    logos::agent::SkillPorts ports;
    if (!moduleMode) {
        // The port a HOST supplies, over this process's own node. Every field is
        // the module's own `deliveryPort()` — except `channelCreate`, whose
        // middle argument is the one that established defect 2 and which this
        // mode keeps measuring after the fix.
        ports.delivery = hostNode.deliveryPort();
        ports.delivery.channelCreate = [&hostNode](const std::string &channelId) {
            return hostNode.channelCreate(channelId, logos::agent::groupTopic(channelId),
                                          channelId);
        };
    }

    const StdLogosResult registered = agent.registerBuiltinSkills(ports);
    if (!registered.success) {
        std::fprintf(stderr, "registerBuiltinSkills refused: %s\n", registered.error.c_str());
        return 1;
    }
    const StdLogosResult started = agent.start();
    if (!started.success) {
        std::fprintf(stderr, "start() refused: %s\n", started.error.c_str());
        return 1;
    }
    std::printf("mode: %s\n", mode.c_str());

    // -----------------------------------------------------------------------
    step("1. the module, before either node exists");
    // -----------------------------------------------------------------------
    const std::string catalogue = agent.skills();
    for (const char *name : {"storage.upload", "storage.download", "storage.list",
                             "storage.share", "messaging.create_group"}) {
        check(catalogue.find(std::string("\"") + name + "\"") != std::string::npos,
              std::string("the module's own registry advertises ") + name);
    }

    json r = call(agent, "storage.upload", json{{"path", uploadPath}});
    note("storage.upload: " + compact(r));
    check(refused(r) && errorOf(r) == "storage node is not started",
          "with no storage node, storage.upload refuses and names what is missing");

    r = call(agent, "messaging.create_group", json{{"group_id", group}, {"members", {member}}});
    note("messaging.create_group: " + compact(r));
    check(refused(r) && errorOf(r) == "delivery node is not started",
          "with no delivery node, messaging.create_group refuses and names what is missing");

    const json d0 = deliveryStatus(agent);
    note("meta.status delivery: " + compact(d0));
    check(d0.value("linked", false),
          "this build knows how to open Logos Delivery at all (linked:true)");
    check(d0.value("state", std::string()) == "off",
          "and the module has not opened one, because nobody has asked it to");

    // The same question of the storage half, and it is a new answer: before the
    // module could open a node there was nothing for `meta.status` to report and
    // the field was null, which reads identically to a node that is off.
    const json s0 = storageStatus(agent);
    note("meta.status storage: " + compact(s0));
    check(s0.value("linked", false),
          "this build knows how to open Logos Storage at all (linked:true)");
    check(s0.value("state", std::string()) == "off",
          "and the module has not opened one, because nobody has asked it to");
    check(!s0.contains("peerId"),
          "and reports no peer id, because there is no node to have one");

    std::string address;
    std::string ghost;

    bool storageUp = false;
    if (moduleMode) {
        // -------------------------------------------------------------------
        step("2. a real Logos Storage node, opened BY THE MODULE");
        // -------------------------------------------------------------------
        // Two strings, twice. That is the whole interface a plugin loaded over
        // Qt Remote Objects can be reached through, and it is now enough to give
        // it a working storage transport.
        r = call(agent, "meta.configure", json{{"key", "storage_data_dir"}, {"value", dataDir}});
        note("meta.configure storage_data_dir: " + compact(r));
        check(answered(r), "meta.configure('storage_data_dir', …) is accepted");

        r = call(agent, "meta.configure", json{{"key", "storage"}, {"value", "on"}});
        note("meta.configure storage=on: " + compact(r));
        check(answered(r), "meta.configure('storage','on') is accepted");

        storageUp = waitModuleStorage(agent, 240);
        check(storageUp, "the module's own Storage node came up and reported it ready");
        const json s1 = storageStatus(agent);
        note("meta.status storage: " + compact(s1));
        check(s1.value("dataDir", std::string()) == dataDir,
              "and it names the repository it was told to open");
        // The one field this module cannot invent: a state of `ready` is the
        // module's own bookkeeping, a libp2p peer identity is the node's.
        check(!s1.value("peerId", std::string()).empty(),
              "it reported a peer id, so it is a node and not a mock: "
                  + s1.value("peerId", std::string()));
    }

    if (moduleMode && storageUp) {
        // -------------------------------------------------------------------
        step("3. storage.upload, through the module's own invoke()");
        // -------------------------------------------------------------------
        r = call(agent, "storage.upload", json{{"path", uploadPath}, {"label", "vault-" + runId}});
        note("storage.upload: " + compact(r));
        check(answered(r), "the skill answered ok");
        if (r.contains("address") && r["address"].is_string()) {
            address = r["address"].get<std::string>();
        }
        // Not "some non-empty string": a Logos content address is base58 and
        // starts with 'z'. Asserting non-emptiness would accept the session id,
        // which is what the port sees one call earlier.
        check(!address.empty() && address[0] == 'z' && address.size() > 40,
              "and gave back a content address: " + address);
        check(r.value("label", std::string()) == "vault-" + runId,
              "the label the caller supplied comes back beside it, unchanged");
        if (!address.empty()) emit("ADDRESS", address);

        // -------------------------------------------------------------------
        step("4. an address the node itself says it does not hold");
        // -------------------------------------------------------------------
        if (!address.empty()) {
            int tries = 0;
            ghost = absentAddress(agent, address, controlPath + ".probe", tries);
            check(!ghost.empty() && ghost != address,
                  "a well-formed control address, found in " + std::to_string(tries)
                      + " attempt(s): " + ghost);
            if (!ghost.empty()) {
                emit("GHOST", ghost);
                check(!nodeHolds(agent, ghost, controlPath + ".probe"),
                      "the node answers that it does not hold the control address");
                check(nodeHolds(agent, address, controlPath + ".probe"),
                      "and that it does hold the real one — the same question, both ways");
            }
        }

        // -------------------------------------------------------------------
        step("5. storage.download, and the address that cannot resolve");
        // -------------------------------------------------------------------
        if (!address.empty()) {
            r = call(agent, "storage.download", json{{"address", address}, {"path", downloadPath}});
            note("storage.download: " + compact(r));
            check(answered(r) && r.value("path", std::string()) == downloadPath,
                  "the skill wrote the content to the path it was given");
            emit("RETRIEVED", downloadPath);
        }
        if (!ghost.empty()) {
            r = call(agent, "storage.download", json{{"address", ghost}, {"path", controlPath}});
            note("storage.download(control): " + compact(r));
            // Either refusal is the right one and the transcript says which:
            // `DownloadSkill` asks `exists` first, so an address the node denies
            // is "no such content address"; one it admits to and cannot produce
            // is "storage refused the download". What must not happen is ok.
            check(refused(r), "an address that cannot resolve is refused: " + errorOf(r));
            emit("CONTROL", controlPath);
        }

        // -------------------------------------------------------------------
        step("6. storage.list");
        // -------------------------------------------------------------------
        r = call(agent, "storage.list", json::object());
        const std::string listed = r.contains("manifests") ? r["manifests"].dump() : std::string();
        note("storage.list: " + compact(r));
        check(answered(r) && r.contains("manifests"), "the skill answered with a manifest list");
        check(!address.empty() && listed.find(address) != std::string::npos,
              "the list names the address that was just uploaded");
        // The containment test has to be shown capable of saying no.
        check(!ghost.empty() && listed.find(ghost) == std::string::npos,
              "and does not name the control address");
    }

    // -----------------------------------------------------------------------
    step("7. a real Logos Delivery node");
    // -----------------------------------------------------------------------
    bool deliveryUp = false;
    if (moduleMode) {
        r = call(agent, "meta.configure", json{{"key", "delivery"}, {"value", "on"}});
        note("meta.configure delivery=on: " + compact(r));
        check(answered(r), "meta.configure('delivery','on') is accepted");
        deliveryUp = waitModuleDelivery(agent, 240);
        check(deliveryUp, "the module's own Delivery node came up and reported it ready");
        note("meta.status delivery: " + compact(deliveryStatus(agent)));
    } else {
        std::string err;
        check(hostNode.bringUp(err), "this process asked for a Delivery node: " + err);
        deliveryUp = waitRuntime(hostNode, 240);
        check(deliveryUp, "the host's Delivery node came up and reported it ready");
        note("host node state: " + hostNode.state() + ", counters " + hostNode.countersJson());
    }

    const std::string ownerTopic = logos::agent::ownerTopic(owner);
    const std::string memberTopic = logos::agent::ownerTopic(member);

    if (moduleMode && deliveryUp) {
        // -------------------------------------------------------------------
        step("8. storage.share — a content address, over the wire, to an owner");
        // -------------------------------------------------------------------
        // Read the topic FIRST. `messaging.receive` subscribes on first use, and
        // a node that was not listening when the frame went past never saw it —
        // so this is not only a baseline, it is what makes the read possible.
        bool read = false;
        const std::vector<std::string> before = framesOn(agent, ownerTopic, read);
        check(read, "messaging.receive is listening on " + ownerTopic);
        note("frames before the share: " + std::to_string(before.size()));

        if (!address.empty()) {
            r = call(agent, "storage.share", json{{"address", address}, {"recipient", owner}});
            note("storage.share: " + compact(r));
            check(answered(r), "the skill answered ok, having verified the address first");

            const std::string want = json{{"sharedAddress", address}}.dump();
            std::size_t total = 0;
            const bool arrived = waitForFrame(agent, ownerTopic, want, 60, total);
            check(arrived,
                  "and the address came back off the topic it was published on: " + want);
            note("frames on " + ownerTopic + ": " + std::to_string(total));
            emit("SHARED", want);
        }

        if (!ghost.empty()) {
            bool countRead = false;
            const std::size_t countBefore = framesOn(agent, ownerTopic, countRead).size();
            r = call(agent, "storage.share", json{{"address", ghost}, {"recipient", owner}});
            note("storage.share(control): " + compact(r));
            check(refused(r) && errorOf(r) == "no such content address to share",
                  "sharing an address the node does not hold is refused, before anything is sent");
            // A refusal that published anyway would pass the line above.
            std::this_thread::sleep_for(std::chrono::seconds(5));
            bool afterRead = false;
            const std::size_t countAfter = framesOn(agent, ownerTopic, afterRead).size();
            check(afterRead && countAfter == countBefore,
                  "and nothing new appeared on the topic: " + std::to_string(countBefore) + " -> "
                      + std::to_string(countAfter));
        }
    }

    if (deliveryUp) {
        // -------------------------------------------------------------------
        step("9. messaging.create_group");
        // -------------------------------------------------------------------
        bool memberRead = false;
        const std::size_t memberBefore = framesOn(agent, memberTopic, memberRead).size();
        check(memberRead, "messaging.receive is listening on " + memberTopic);
        note("frames before the group: " + std::to_string(memberBefore));

        r = call(agent, "messaging.create_group",
                 json{{"group_id", group}, {"members", {member, owner}}});
        note("messaging.create_group: " + compact(r));

        if (moduleMode) {
            // THE FIX, asserted so it cannot regress. This call used to answer
            // "delivery refused to create the channel": the module's own port
            // passed the group id where the content topic goes, and a real node
            // subscribes to that argument before opening the channel.
            // `deliveryPort()` passes `groupTopic(channelId)` now, and the
            // bare-id control in `host-port` below keeps the difference measured
            // on a live node rather than argued from the source.
            check(answered(r), "through the module's OWN port, the channel opened on the real "
                               "node");
            check(r.value("channel", std::string()) == group && r.value("invited", 0) == 2,
                  "and both members were invited — asked and invited are counted separately");
            emit("MODULE-PORT-CREATE-GROUP", "opened");

            const std::string invite = json{{"invite", group}}.dump();
            std::size_t total = 0;
            const bool invited = waitForFrame(agent, memberTopic, invite, 60, total);
            check(invited, "the invitation came back off the member's own topic: " + invite);
            note("frames on " + memberTopic + ": " + std::to_string(total));
        } else {
            check(answered(r), "the channel opened on the real node");
            check(r.value("channel", std::string()) == group && r.value("invited", 0) == 2,
                  "and both members were invited — asked and invited are counted separately");
            emit("GROUP", group);

            const std::string invite = json{{"invite", group}}.dump();
            std::size_t total = 0;
            const bool invited = waitForFrame(agent, memberTopic, invite, 60, total);
            check(invited, "the invitation came back off the member's own topic: " + invite);
            note("frames on " + memberTopic + ": " + std::to_string(total));

            // The same node, two calls apart, one difference: the content topic.
            // This is what turns "the module's port is wrong" from an inference
            // into a measurement, and it is run on the node the skill just
            // succeeded against.
            const std::string bare = group + "bare";
            const bool bareRefused = !hostNode.channelCreate(bare, bare, bare);
            check(bareRefused,
                  "control: the same node refuses a channel whose content topic is a bare id — "
                  "which is what the module's own port used to ask for");
            const std::string proper = group + "proper";
            check(hostNode.channelCreate(proper, logos::agent::groupTopic(proper), proper),
                  "and accepts one whose content topic is in the grammar Logos documents — "
                  "which is what it asks for now");
        }

        // A member id that cannot go into a content topic must cost the whole
        // call, not one invitation — and the refusal must name the member rather
        // than the group.
        r = call(agent, "messaging.create_group",
                 json{{"group_id", group + "b"}, {"members", {"../../owner-victim"}}});
        note("messaging.create_group(control): " + compact(r));
        check(refused(r) && errorOf(r).find("member") != std::string::npos,
              "a member id that would name a different topic is refused: " + errorOf(r));

        // -------------------------------------------------------------------
        step("10. and with the node put back down, the same call refuses again");
        // -------------------------------------------------------------------
        if (moduleMode) {
            r = call(agent, "meta.configure", json{{"key", "delivery"}, {"value", "off"}});
            check(answered(r), "meta.configure('delivery','off') is accepted");
            check(deliveryStatus(agent).value("state", std::string()) == "off",
                  "the node is down again");
        } else {
            hostNode.shutDown();
            check(!hostNode.ready(), "the host's node is down again");
        }
        r = call(agent, "messaging.create_group", json{{"group_id", group}, {"members", {member}}});
        note("messaging.create_group: " + compact(r));
        check(refused(r) && errorOf(r) == "delivery node is not started",
              "and messaging.create_group refuses with the message it gave before any node "
              "existed");
    }

    // -----------------------------------------------------------------------
    step("11. shut down");
    // -----------------------------------------------------------------------
    if (node.ctx != nullptr) {
        node.up.store(false);
        node.arm();
        storage_stop(node.ctx, &StorageNode::callback, &node);
        check(node.settled(60), "the storage node stopped");
        node.arm();
        storage_close(node.ctx, &StorageNode::callback, &node);
        node.settled(30);
        check(storage_destroy(node.ctx) == RET_OK, "the storage context was destroyed");
    }
    agent.stop();

    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED" : "every assertion held",
                failures);
    return failures ? 1 : 0;
}
