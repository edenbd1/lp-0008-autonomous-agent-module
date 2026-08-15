// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Can a module that Logos Core LOADS obtain a working Delivery port at all?
//
//   plugin_delivery_test <plugin.dylib> probe
//   plugin_delivery_test <plugin.dylib> peer <run-id> <me> <mykey-cmd> <other>
//
// THE QUESTION
//
// `docs/basecamp.md` records, correctly, that a loaded plugin is handed
// `SkillPorts{}` and refuses every skill that touches the wire, "because a port
// is a `std::function` and there is no wire format for one". That sentence is
// about what a HOST can PASS. It was read, for months, as a statement about
// what a MODULE can HAVE — and those are different claims. This harness is the
// difference: the plugin under test links `liblogosdelivery`, opens a node from
// its own configuration when `meta.configure("delivery","on")` arrives, and
// builds its own ports out of it. Nothing crosses the boundary but two strings.
//
// Everything below runs against the plugin through `QPluginLoader` and
// `LogosProviderObject::callMethod` — the same path `module/tests/plugin_load_test.cpp`
// uses, which is the path Logos Core itself takes into a core module.
//
// TWO MODES, AND WHY THE SECOND ONE EXISTS
//
// `probe` is one process: it watches the transport refuse before it is started,
// start, and stop refusing. That is enough to answer the question and not
// enough to be evidence for the prize criterion, because a Waku node receives
// its own published messages — a one-process test that asserted "a card arrived
// on the discovery topic" would pass with every other agent on earth switched
// off.
//
// `peer` is therefore two processes, each with its own node, its own working
// directory and its own LEZ account, sharing nothing but a content topic on the
// public network. Each publishes its own signed Agent Card and accepts only a
// card carrying the OTHER account: neither can satisfy itself. Run two of them
// with the same run id and swapped accounts.
//
// A NOTE ON WORKING DIRECTORIES, WHICH COST AN AFTERNOON ELSEWHERE
//
// A Delivery node keeps reliable-channel state in the CURRENT WORKING
// DIRECTORY. Two nodes started from one directory silently share it, and the
// first frame is not delivered — which reads exactly like an unreliable
// network. Each `peer` process must be started in a directory of its own; the
// runner script does that, and this file says so because the failure it
// produces accuses the wrong component.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPluginLoader>
#include <QString>
#include <QVariant>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "interface.h"
#include "logos_provider_object.h"

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    std::fprintf(stderr, "  %s  %s\n", condition ? "ok  " : "FAIL", qPrintable(what));
    std::fflush(stderr);
    if (!condition) ++failures;
}

void note(const QString &what)
{
    std::fprintf(stderr, "  <-    %s\n", qPrintable(what));
    std::fflush(stderr);
}

void step(const QString &what)
{
    std::fprintf(stderr, "\n%s\n", qPrintable(what));
    std::fflush(stderr);
}

QJsonObject asObject(const QVariant &v)
{
    QJsonParseError e{};
    const QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8(), &e);
    return d.isObject() ? d.object() : QJsonObject{};
}

/// `invoke(name, params)` on the loaded module, as JSON.
QJsonObject call(LogosProviderObject *p, const char *skill, const QString &params)
{
    return asObject(p->callMethod(QStringLiteral("invoke"),
                                  {QString::fromUtf8(skill), params}));
}

/// The transport's own account of itself, out of `meta.status`.
QJsonObject deliveryStatus(LogosProviderObject *p)
{
    return call(p, "meta.status", QStringLiteral("{}")).value("delivery").toObject();
}

/// Wait for the node to say it is up. Bounded, and the bound is the answer:
/// a node that never comes up must fail this harness rather than hang it.
bool waitReady(LogosProviderObject *p, int seconds)
{
    for (int i = 0; i < seconds; ++i) {
        const QJsonObject d = deliveryStatus(p);
        const QString state = d.value("state").toString();
        if (state == QLatin1String("ready")) return true;
        if (state == QLatin1String("failed")) {
            note("delivery failed: " + d.value("error").toString());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

/// Bring the loaded module up to "configured and started", the state every
/// mode needs before it can ask the module anything.
LogosProviderObject *bringUpModule(const QString &path)
{
    QPluginLoader loader(path);
    QObject *instance = loader.instance();
    if (!instance) {
        check(false, "the plugin instantiates: " + loader.errorString());
        return nullptr;
    }
    auto *asProvider = qobject_cast<LogosProviderPlugin *>(instance);
    if (!asProvider) {
        check(false, "it casts to LogosProviderPlugin across the boundary");
        return nullptr;
    }
    LogosProviderObject *provider = asProvider->createProviderObject();
    if (!provider) {
        check(false, "createProviderObject() returns a provider");
        return nullptr;
    }
    check(true, "the plugin loaded and produced a provider");

    const QString owner = QStringLiteral("lez1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq");
    const QString policy =
        QStringLiteral("8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90");
    check(provider->callMethod(QStringLiteral("configure"), {owner, policy})
              .value<LogosResult>()
              .success,
          "configure() is accepted across the plugin boundary");
    check(provider->callMethod(QStringLiteral("start"), {}).value<LogosResult>().success,
          "start() is accepted");
    return provider;
}

// ---------------------------------------------------------------------------
// probe: the transport, before and after
// ---------------------------------------------------------------------------

int runProbe(const QString &path)
{
    LogosProviderObject *p = bringUpModule(path);
    if (!p) return 1;

    step("1. what the loaded module says about its transport before anybody asks");
    QJsonObject d = deliveryStatus(p);
    note("meta.status delivery: " +
         QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
    // These two are the whole distinction. `linked` is a fact about the binary;
    // `state` is a fact about the run. A build with no library reports
    // `absent`, and every skill below would refuse for a reason no amount of
    // configuring could change.
    check(d.value("linked").toBool(),
          "this build of the plugin knows how to open Logos Delivery");
    check(d.value("state").toString() == QLatin1String("off"),
          "and has not started a node, because nobody has asked it to");

    step("2. the refusal, which must still be the honest one");
    QJsonObject r = call(p, "messaging.send",
                         QStringLiteral(R"({"recipient":"probe","message":"hi"})"));
    note("messaging.send: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(!r.value("ok").toBool() &&
              r.value("error").toString() == QLatin1String("delivery node is not started"),
          "with the node off, the wire skills refuse exactly as they did before");

    step("3. two strings across the boundary");
    r = call(p, "meta.configure", QStringLiteral(R"({"key":"delivery","value":"maybe"})"));
    check(!r.value("ok").toBool(),
          "a value the module cannot read is refused rather than guessed at: " +
              r.value("error").toString());
    r = call(p, "meta.configure", QStringLiteral(R"({"key":"delivery","value":"on"})"));
    note("meta.configure delivery=on: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "meta.configure('delivery','on') is accepted");

    step("4. the node the module opened for itself");
    const bool ready = waitReady(p, 240);
    check(ready, "the module's own Delivery node came up and reported it");
    if (!ready) {
        note("delivery: " + QString::fromUtf8(
                                QJsonDocument(deliveryStatus(p)).toJson(QJsonDocument::Compact)));
        return 1;
    }

    step("5. the same skills, on the same loaded module, now that it has a port");
    r = call(p, "messaging.join", QStringLiteral(R"({"group_id":"lp0008probe"})"));
    note("messaging.join: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "messaging.join subscribed on the real network");

    r = call(p, "messaging.send",
             QStringLiteral(R"({"recipient":"lp0008probe","message":"lp-0008"})"));
    note("messaging.send: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "messaging.send put a message on the network");

    r = call(p, "agent.discover", QStringLiteral(R"({"topic":"/lp-0008/1/discovery-x/json"})"));
    check(r.value("ok").toBool(),
          "agent.discover answers with a result rather than 'no discovery transport is "
          "configured'");

    step("6. and it can be put back");
    r = call(p, "meta.configure", QStringLiteral(R"({"key":"delivery","value":"off"})"));
    check(r.value("ok").toBool(), "meta.configure('delivery','off') is accepted");
    check(deliveryStatus(p).value("state").toString() == QLatin1String("off"),
          "the node is down again, and the wire skills refuse again");
    r = call(p, "messaging.send",
             QStringLiteral(R"({"recipient":"probe","message":"hi"})"));
    check(!r.value("ok").toBool(), "which they do");

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "FAILED" : "a loaded plugin obtained a working Delivery port",
                 failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// peer: two loaded modules, two nodes, two accounts, one topic
// ---------------------------------------------------------------------------

int runPeer(const QString &path, const QString &runId, const QString &me,
            const QString &signer, const QString &other)
{
    LogosProviderObject *p = bringUpModule(path);
    if (!p) return 1;

    // The card topic. `messaging.send` publishes on `ownerTopic(recipient)` and
    // `agent.discover` reads whatever topic it is handed, so one well-known name
    // both sides derive from the run id is enough: nothing is passed between the
    // two processes except that id.
    const QString board = QStringLiteral("lp0008cards") + runId;
    const QString topic = QStringLiteral("/lp-0008/1/owner-") + board + "/json";

    step("1. configure this agent's identity and start its node");
    const std::pair<const char *, QString> settings[] = {
        {"agent_account", me},
        {"pay_account", me},
        {"card_signer", signer},
        {"agent_name", QStringLiteral("lp0008-agent-") + me.left(6)},
    };
    for (const auto &kv : settings) {
        const QJsonObject s = call(
            p, "meta.configure",
            QStringLiteral(R"({"key":"%1","value":"%2"})")
                .arg(QString::fromUtf8(kv.first), kv.second));
        check(s.value("ok").toBool(),
              QStringLiteral("meta.configure %1").arg(QString::fromUtf8(kv.first)) +
                  (s.value("ok").toBool() ? QString() : ": " + s.value("error").toString()));
    }
    QJsonObject r = call(p, "meta.configure",
                         QStringLiteral(R"({"key":"delivery","value":"on"})"));
    check(r.value("ok").toBool(), "meta.configure('delivery','on')");
    const bool up = waitReady(p, 240);
    check(up, "this agent's own Delivery node came up");
    if (!up) return 1;

    step("2. the Agent Card this loaded module assembles and signs for itself");
    r = call(p, "agent.card", QStringLiteral("{}"));
    if (!r.value("ok").toBool()) {
        note("agent.card: " + r.value("error").toString());
        check(false, "agent.card produced a signed card");
        return 1;
    }
    const QJsonObject card = r.value("card").toObject();
    const QString cardText =
        QString::fromUtf8(QJsonDocument(card).toJson(QJsonDocument::Compact));
    check(card.value("url").toString().endsWith(me), "the card names this agent's account");
    check(!card.value("signatures").toArray().isEmpty(), "and carries a signature");
    note("card: " + cardText.left(200) + QStringLiteral("..."));

    step("3. publish it, and watch the topic for the other agent's card");
    note("topic " + topic);
    const int rounds = qEnvironmentVariableIntValue("LP0008_ROUNDS") > 0
                           ? qEnvironmentVariableIntValue("LP0008_ROUNDS")
                           : 40;
    bool sawOther = false;
    bool published = false;
    QJsonObject seen;
    for (int i = 0; i < rounds && !sawOther; ++i) {
        // Resent every round rather than once: the two processes do not start
        // together, and a card published before the other node had subscribed
        // is a card nobody will ever see again — Waku relay carries no history.
        r = call(p, "messaging.send",
                 QStringLiteral(R"({"recipient":"%1","message":%2})")
                     .arg(board, QString::fromUtf8(QJsonDocument(QJsonObject{{"m", cardText}})
                                                       .toJson(QJsonDocument::Compact))
                                     .section(':', 1)
                                     .chopped(1)));
        if (r.value("ok").toBool()) published = true;

        const QJsonObject found =
            call(p, "agent.discover",
                 QStringLiteral(R"({"topic":"%1","require_signed":true})").arg(topic));
        for (const QJsonValue &a : found.value("agents").toArray()) {
            // Only a card naming the OTHER account counts. A Waku node receives
            // its own published messages, so an assertion that merely counted
            // cards on the topic would pass with the other process dead — the
            // same trap `owner_channel_live.cpp` is shaped around.
            const QJsonObject o = a.toObject();
            if (o.value("url").toString().endsWith(other)) {
                sawOther = true;
                seen = o;
            }
        }
        if (!sawOther) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    check(published, "this agent published its own signed card on the topic");
    check(sawOther,
          "and discovered the OTHER agent's signed Agent Card over the public network");
    if (sawOther) {
        note("discovered: " +
             QString::fromUtf8(QJsonDocument(seen).toJson(QJsonDocument::Compact)));
        check(seen.value("signed").toBool(),
              "which is signed - `require_signed` was on, so an unsigned card would not be in "
              "this list at all");
    }

    step("4. and then one serves the other: an A2A request, sent and READ");
    // The half the module did not have. `agent.task` addresses a peer on
    // `/lp-0008/1/task-<agent>-<task>/json`; until `messaging.receive` existed
    // the peer had no skill that could read that topic, so two agents could
    // find each other and neither could serve the other. Both sides do both
    // roles here, so one binary proves both directions.
    const QString myTask = QStringLiteral("t") + runId + me.left(6);
    const QString theirTask = QStringLiteral("t") + runId + other.left(6);
    const QString myInbox =
        QStringLiteral("/lp-0008/1/task-") + me + "-" + theirTask + "/json";

    // Subscribe before anyone sends: Waku relay carries no history, so a topic
    // joined after the fact is a topic nothing will ever arrive on. The first
    // read is what subscribes, and it is expected to be empty.
    QJsonObject inbox =
        call(p, "messaging.receive", QStringLiteral(R"({"topic":"%1"})").arg(myInbox));
    check(inbox.value("ok").toBool(), "messaging.receive answers, and subscribes on first use");
    check(inbox.value("total").toInt() == 0,
          "and its first answer is empty, because nothing has been sent yet");

    r = call(p, "agent.task",
             QStringLiteral(R"({"agent_address":"%1","skill":"storage.upload",)"
                            R"("task_id":"%2","params":{"path":"/tmp/x"}})")
                 .arg(other, myTask));
    check(r.value("ok").toBool() && r.value("state").toString() == QLatin1String("submitted"),
          "this agent opened an A2A task addressed to the other one");

    bool served = false;
    QString servedText;
    for (int i = 0; i < rounds && !served; ++i) {
        inbox = call(p, "messaging.receive", QStringLiteral(R"({"topic":"%1"})").arg(myInbox));
        for (const QJsonValue &entry : inbox.value("messages").toArray()) {
            const QString text = entry.toObject().value("message").toString();
            const QJsonObject rpc = asObject(QVariant(text));
            // Not "a message arrived": the document has to be the A2A JSON-RPC
            // request, addressed to the task the OTHER agent minted. A frame
            // this node published itself would name `myTask`.
            if (rpc.value("method").toString() != QLatin1String("message/send")) continue;
            const QJsonObject message =
                rpc.value("params").toObject().value("message").toObject();
            if (message.value("taskId").toString() == theirTask) {
                served = true;
                servedText = text;
            }
        }
        if (!served) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    check(served,
          "and READ the other agent's A2A request off its own task topic — the half of the "
          "lifecycle that had no skill until messaging.receive");
    if (served) note("served: " + servedText.left(220) + QStringLiteral("..."));

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "FAILED" : "two loaded modules discovered each other", failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// approval: an above-threshold spend, held for an owner on another node
// ---------------------------------------------------------------------------

int runApproval(const QString &path, const QString &me, const QString &owner,
                const QString &recipient, const QString &amount)
{
    LogosProviderObject *p = bringUpModule(path);
    if (!p) return 1;

    step("1. configure the agent's identity, its owner's channel, and its node");
    const std::pair<const char *, QString> settings[] = {
        {"agent_account", me},
        {"owner_channel_account", owner},
        // Long enough that a node still finding peers is not read as an owner
        // who said nothing, short enough that a broken run ends.
        {"approval_timeout_ms", QStringLiteral("180000")},
        {"approval_resend_ms", QStringLiteral("8000")},
    };
    for (const auto &kv : settings) {
        const QJsonObject r = call(
            p, "meta.configure",
            QStringLiteral(R"({"key":"%1","value":"%2"})")
                .arg(QString::fromUtf8(kv.first), kv.second));
        check(r.value("ok").toBool(), QStringLiteral("meta.configure %1")
                                          .arg(QString::fromUtf8(kv.first)));
    }
    check(call(p, "meta.configure", QStringLiteral(R"({"key":"delivery","value":"on"})"))
              .value("ok").toBool(),
          "meta.configure('delivery','on')");
    const bool up = waitReady(p, 240);
    check(up, "the agent's own Delivery node came up");
    if (!up) return 1;

    step("2. ask to spend, above an envelope that is zero because nothing anchored one");
    // Every port is unwired, so the envelope is zero and *every* spend is
    // outside it. That is the state a loaded module is in, and it is the one
    // that sends this to the owner rather than making it unattended.
    note("this call blocks until the owner answers or the wait runs out");
    const auto started = std::chrono::steady_clock::now();
    const QJsonObject sent =
        call(p, "wallet.send", QStringLiteral(R"({"recipient":"%1","amount":"%2"})")
                                   .arg(recipient, amount));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    note(QStringLiteral("%1 ms").arg(elapsed));
    note("wallet.send: " +
         QString::fromUtf8(QJsonDocument(sent).toJson(QJsonDocument::Compact)));

    step("3. what the node actually handed over");
    // Three numbers that separate the two failures that look alike: nothing
    // arrived, versus something arrived and could not be read.
    note("meta.status delivery: " +
         QString::fromUtf8(QJsonDocument(deliveryStatus(p)).toJson(QJsonDocument::Compact)));

    step("4. what came back");
    // The outcome, not merely "ok". `awaiting_owner_approval` with
    // `owner_verdict: approved` is the owner having answered these exact terms;
    // `owner_unreachable` is the failure this whole exercise exists to tell
    // apart from it, and it is what every run of this produced before the
    // marker seed was derived rather than left empty.
    // `outcome` and `approved`, which are the fields `wallet.send` actually
    // emits. The first version of this read `owner_verdict`, which nothing
    // produces, so it was an assertion on an absent field — `QJsonValue()` to
    // `QString()` is `""`, and `"" == "approved"` is false, so it failed on a
    // run that had succeeded. That is the third time today an assertion has
    // been about a field that was not there.
    const QString outcome = sent.value("outcome").toString();
    const bool approved = sent.value("approved").toBool();
    note("outcome: " + outcome + ", approved: " + (approved ? "true" : "false") +
         ", attempts: " + QString::number(sent.value("attempts").toInt()) +
         ", waited: " + QString::number(sent.value("waited_ms").toInt()) + " ms");
    check(outcome != QLatin1String("owner_unreachable"),
          "the owner was reached — the assertion that was impossible while the module sent "
          "an empty marker seed");
    const bool expectDeny = qEnvironmentVariableIsSet("LP0008_EXPECT_DENY");
    if (expectDeny) {
        // The control. An owner who says no must produce a denial, not an
        // approval and not a timeout: a channel that reported "approved"
        // whatever came back would pass every other check on this page.
        check(outcome == QLatin1String("denied") && !approved,
              "the owner's DENIAL came back as a denial: " + outcome);
    } else {
        check(outcome == QLatin1String("approved") && approved,
              "and approved these exact terms: " + outcome);
    }
    check(sent.value("attempts").toInt() >= 1, "the request was really put on the wire");
    check(!sent.value("submitted").toBool(),
          "nothing was submitted: an approval unlocks the spend_approved path, it does not "
          "move money, and this module wires no wallet");

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "FAILED"
                          : "an owner on another node answered a loaded module's own terms",
                 failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <plugin> probe\n"
                     "       %s <plugin> peer <run-id> <me> <signer-cmd> <other>\n"
                     "       %s <plugin> approval <me> <owner> <recipient> <amount>\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }
    const QString path = QString::fromUtf8(argv[1]);
    const QString mode = QString::fromUtf8(argv[2]);
    if (mode == QLatin1String("probe")) return runProbe(path);
    if (mode == QLatin1String("approval")) {
        if (argc < 7) {
            std::fprintf(stderr, "approval needs <me> <owner> <recipient> <amount>\n");
            return 2;
        }
        return runApproval(path, QString::fromUtf8(argv[3]), QString::fromUtf8(argv[4]),
                           QString::fromUtf8(argv[5]), QString::fromUtf8(argv[6]));
    }
    if (mode == QLatin1String("peer")) {
        if (argc < 7) {
            std::fprintf(stderr, "peer needs <run-id> <me> <signer-cmd> <other>\n");
            return 2;
        }
        return runPeer(path, QString::fromUtf8(argv[3]), QString::fromUtf8(argv[4]),
                       QString::fromUtf8(argv[5]), QString::fromUtf8(argv[6]));
    }
    std::fprintf(stderr, "unknown mode %s\n", argv[2]);
    return 2;
}
