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
// That rule is applied twice, because the lifecycle needs it as badly as
// discovery did. Step 5 has each agent publish status updates for the task it
// was asked to do, and read its OWN task's topic until its store reaches
// `completed` — and before it reads, it publishes a forged `completed` for its
// own task onto that same topic, as itself. A node receives what it publishes,
// so without an author rule that one frame would end the step alone. The
// assertion is not that no such update was applied, which would also hold if it
// never arrived: it is that the poll counted one and refused it.
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
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    // `QJsonValue::toBool()` on an ABSENT key returns false, and `call()`
    // answers `QJsonObject{}` for a call that never arrived — so `!ok` was
    // satisfied by a dead module and a timed-out Qt Remote Objects call alike.
    // Its neighbour eight lines up pins the exact error for the same class of
    // refusal; this one now does too.
    check(r.contains(QStringLiteral("ok")) && !r.value("ok").toBool()
              && !r.value("error").toString().isEmpty(),
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
    check(r.contains(QStringLiteral("ok")) && !r.value("ok").toBool()
              && r.value("error").toString() == QLatin1String("delivery node is not started"),
          "which they do, with the same refusal as before delivery was ever on");

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

    // THE PAYMENT HALF, WHICH IS DELIBERATELY ASYMMETRIC.
    //
    // Everything else in this mode is the same on both sides, because both sides
    // do both roles and one binary then proves both directions. A payment cannot
    // be: somebody sells and somebody buys, and running it symmetrically would
    // move money twice to demonstrate it once — on a testnet whose faucet is
    // gone, where the agents hold a handful of LEZ between them, and where an
    // account at zero cannot sign at all.
    //
    // So the seller sets `LP0008_PRICE` and its published card carries a price;
    // the buyer sets `LP0008_PAY_SIGNER` and `LP0008_POLICY_SOURCE` and pays it.
    // Neither knows which the other is: each reads the other's card off the
    // public topic and acts on what it says.
    const QString payAccount = qEnvironmentVariableIsSet("LP0008_PAY_ACCOUNT")
                                   ? qEnvironmentVariable("LP0008_PAY_ACCOUNT")
                                   : me;
    const QString price = qEnvironmentVariable("LP0008_PRICE");
    const QString paySigner = qEnvironmentVariable("LP0008_PAY_SIGNER");
    const QString policySource = qEnvironmentVariable("LP0008_POLICY_SOURCE");
    const bool expectPay = !paySigner.isEmpty();

    step("1. configure this agent's identity and start its node");
    std::vector<std::pair<const char *, QString>> settings{
        {"agent_account", me},
        {"pay_account", payAccount},
        {"card_signer", signer},
        {"agent_name", QStringLiteral("lp0008-agent-") + me.left(6)},
    };
    if (!price.isEmpty()) settings.push_back({"price_per_task", price});
    if (!paySigner.isEmpty()) settings.push_back({"pay_signer", paySigner});
    if (!policySource.isEmpty()) settings.push_back({"policy_source", policySource});
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

    // THE INBOX IS OPENED BEFORE DISCOVERY, NOT AFTER IT.
    //
    // These four lines used to live at the top of step 4, and the assertion that
    // the first read is empty was a race that this run lost: the two processes
    // leave the discovery loop up to one poll apart, whichever leaves first
    // publishes its A2A request, and the other one's "first" read then finds it
    // already waiting. The claim was never about timing — it is that
    // `messaging.receive` subscribes on first use and reports only what really
    // arrived — so it is made here, where neither agent has minted a task yet
    // and the answer is empty for a reason instead of by luck.
    //
    // It is also strictly safer: a Waku node buffers what its relay shard
    // carries, and opening the inbox this early is one less way for the peer's
    // request to be published into a topic nobody was listening on.
    const QString myTask = QStringLiteral("t") + runId + me.left(6);
    const QString theirTask = QStringLiteral("t") + runId + other.left(6);
    const QString myInbox =
        QStringLiteral("/lp-0008/1/task-") + me + "-" + theirTask + "/json";
    QJsonObject inbox =
        call(p, "messaging.receive", QStringLiteral(R"({"topic":"%1"})").arg(myInbox));
    check(inbox.value("ok").toBool(), "messaging.receive answers, and subscribes on first use");
    check(inbox.value("total").toInt() == 0,
          "and its first answer is empty, because neither agent has opened a task yet");

    step("3. publish it, and watch the topic for the other agent's card");
    note("topic " + topic);
    const int rounds = qEnvironmentVariableIntValue("LP0008_ROUNDS") > 0
                           ? qEnvironmentVariableIntValue("LP0008_ROUNDS")
                           : 40;
    // HOW LONG TO WAIT FOR THE PEER'S STATUS UPDATES, WHICH IS NOT THE SAME
    // QUESTION AS HOW LONG TO WAIT FOR ITS CARD.
    //
    // A settlement blocks the paying module for as long as the proof takes —
    // 418 s in the run this repository cites, 767 s in one the day before — and
    // a module that is blocked publishes nothing. So in `settle` mode the two
    // sides are waiting for very different things: the payer unblocks to find
    // its peer's updates already on the topic, and the peer has to outwait a
    // proof it cannot see.
    //
    // With one bound for both, that mode could not pass, and the failure would
    // have read as the lifecycle being broken rather than as this harness
    // timing a proof. `rounds` is 40 polls at three seconds — two minutes,
    // against a step measured at seven.
    //
    // The seller knows it is the seller without asking anyone: it is the side
    // whose own card carries a price. Nothing here times the peer or waits on a
    // message from it; the bound is a function of this process's own
    // configuration. In `peers` mode neither side has a price, so both keep the
    // short bound and a broken run still goes red in two minutes.
    const int updateRounds = qEnvironmentVariableIntValue("LP0008_UPDATE_ROUNDS") > 0
                                 ? qEnvironmentVariableIntValue("LP0008_UPDATE_ROUNDS")
                                 : (price.isEmpty() ? rounds : rounds * 12);
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
    if (!sawOther) return 1;
    note("discovered: " +
         QString::fromUtf8(QJsonDocument(seen).toJson(QJsonDocument::Compact)));
    check(seen.value("signed").toBool(),
          "which is signed - `require_signed` was on, so an unsigned card would not be in "
          "this list at all");

    // The price this agent is about to be asked for, read off the card that
    // arrived over the network rather than out of anything either process was
    // told. `expectPay` is the buyer; there is no configuration on this side
    // naming the price, the payee, or that there is one.
    const QJsonObject theirCard = seen.value("card").toObject();
    const double advertised = seen.value("price").toDouble();
    if (expectPay) {
        check(advertised > 0,
              QStringLiteral("the discovered card advertises a price to pay: %1 LEZ")
                  .arg(advertised));
        check(seen.value("pay_account").toString().startsWith(QLatin1String("Public/")),
              "and a public account to pay it into: " + seen.value("pay_account").toString());
    }

    step("4. and then one serves the other: an A2A request, sent and READ");
    // The half the module did not have. `agent.task` addresses a peer on
    // `/lp-0008/1/task-<agent>-<task>/json`; until `messaging.receive` existed
    // the peer had no skill that could read that topic, so two agents could
    // find each other and neither could serve the other. Both sides do both
    // roles here, so one binary proves both directions. The inbox was opened
    // before discovery — see the note above step 3.
    //
    // WITH THE CARD, which is the whole difference between this and the version
    // of this harness that proved discovery and payment as two separate halves.
    // `agent.task` takes the price and the payee off the card it is handed —
    // the peer's own signed document, discovered a moment ago on the public
    // topic — checks that price against the envelope its owner anchored on
    // chain, sends the A2A request, and settles. One call, one flow.
    const QString cardArg =
        QString::fromUtf8(QJsonDocument(theirCard).toJson(QJsonDocument::Compact));
    note(expectPay ? "this call blocks while the settlement is proved and confirmed"
                   : "this task is free, so nothing is settled for it");
    const auto paidFrom = std::chrono::steady_clock::now();
    r = call(p, "agent.task",
             QStringLiteral(R"({"agent_address":"%1","skill":"storage.upload",)"
                            R"("task_id":"%2","params":{"path":"/tmp/x"},"card":%3})")
                 .arg(other, myTask, cardArg));
    const auto paidMs = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - paidFrom)
                            .count();
    note("agent.task: " + QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact))
                              .left(400));
    check(r.value("ok").toBool() && r.value("state").toString() == QLatin1String("submitted"),
          "this agent opened an A2A task addressed to the other one: " +
              r.value("error").toString());

    // THE SETTLEMENT, OR THE ABSENCE OF ONE, AND BOTH ARE ASSERTED.
    //
    // A hash is only evidence if the same code path can also produce no hash.
    // The seller runs this identical block against a card advertising nothing,
    // and has to come back with an empty `settlement_tx` — so "the module
    // reports a transaction hash" is distinguished from "the module reports a
    // transaction hash whenever it is asked to open a task".
    const QString settlement = r.value("settlement_tx").toString();
    // Captured now rather than read out of `r` later: the context id this agent
    // minted for its own task, which the peer will echo back in every status
    // update and which the decoy below has to carry to be a decoy at all.
    const QString myContext = r.value("context_id").toString();
    const double paidPrice = r.value("price").toDouble();
    note(QStringLiteral("price %1, settlement_tx '%2', %3 s")
             .arg(paidPrice).arg(settlement).arg(paidMs));
    if (expectPay) {
        check(paidPrice == advertised,
              QStringLiteral("it paid the price the peer's card advertised, %1 LEZ")
                  .arg(advertised));
        // 64 lower-case hex. The module refuses anything else out of the
        // signer, and the signer refuses to print a hash that is not in a block,
        // so a non-empty value here is a transaction the chain has taken.
        static const QRegularExpression hex(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        check(hex.match(settlement).hasMatch(),
              "and settled it on chain, from inside the loaded module, with no owner in the "
              "path: " + settlement);
    } else {
        check(paidPrice == 0,
              "the card this agent was handed advertises no price, so there is nothing to pay");
        check(settlement.isEmpty(),
              "and no settlement hash came back for it — the same code path, and it reports "
              "nothing when nothing was paid");
    }

    // THE UPDATE TOPIC IS OPENED HERE, BEFORE THE PEER CAN HAVE ANSWERED.
    //
    // One call, and no assertion about what it found. The peer's first status
    // update can only follow the request that has just gone out — but "can only
    // follow" is about causality, not about scheduling, and the peer may well
    // have read, worked and answered before this line runs. So what is asserted
    // is that the poll ANSWERS; asserting it found nothing would be an
    // assertion about which process got there first, which is the mistake
    // `docs/limitations.md` records costing a settlement.
    //
    // WHAT IT FINDS IS COUNTED, THOUGH, AND THAT COST A SECOND SETTLEMENT.
    //
    // In `peers` mode this poll runs before the peer has answered, so it applies
    // nothing and the loop below sees every update. In `settle` mode it runs on
    // the far side of a 434-second proof, by which time the peer's `working` and
    // `completed` are both waiting — so THIS call applies them both, and a
    // counter that only watched the loop reported "applying 0 status update(s)
    // the peer published" about a task whose own history read
    // submitted -> working -> completed.
    //
    // Settlement 9 is that run: the money moved, the lifecycle ran, and the
    // harness went red four lines past the payment on an assertion that was
    // measuring where the work happened rather than whether it did. So the
    // accounting starts here, at the first poll, and not at the loop.
    const QString myUpdates =
        QStringLiteral("/lp-0008/1/task-") + other + "-" + myTask + "/json";
    QString state = QStringLiteral("submitted");
    QStringList authors;
    int selfIgnored = 0, applied = 0, since = 0;
    QJsonArray history;
    // Everything one poll answered, folded into what this step is counting.
    const auto absorb = [&](const QJsonObject &poll) {
        since = poll.value("next_since").toInt();
        selfIgnored += poll.value("ignored").toObject().value("self").toInt();
        for (const QJsonValue &entry : poll.value("applied").toArray()) {
            authors.append(entry.toObject().value("from").toString());
            ++applied;
        }
        state = poll.value("state").toString();
        history = poll.value("history").toArray();
    };
    QJsonObject updates =
        call(p, "agent.poll",
             QStringLiteral(R"({"agent_address":"%1","task_id":"%2"})").arg(other, myTask));
    check(updates.value("ok").toBool(),
          "agent.poll answers on the task's own topic, and opens it for reading: " +
              updates.value("error").toString());
    check(updates.value("topic").toString() == myUpdates,
          "which is the topic the request was sent to — one topic per task, both ways");
    if (updates.value("ok").toBool()) absorb(updates);

    bool served = false;
    QString servedText;
    QString servedContext;
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
                // The context id the OTHER agent minted, which appears nowhere
                // but in this request. `agent.update` requires it, so an update
                // this agent publishes below is one it could not have published
                // without having read what arrived.
                servedContext = message.value("contextId").toString();
            }
        }
        if (!served) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    check(served,
          "and READ the other agent's A2A request off its own task topic — the half of the "
          "lifecycle that had no skill until messaging.receive");
    if (!served) {
        std::fprintf(stderr, "\nFAILED (%d failure(s))\n", failures);
        return 1;
    }
    note("served: " + servedText.left(220) + QStringLiteral("..."));
    check(!servedContext.isEmpty(),
          "carrying the context id the other agent minted: " + servedContext);

    // -----------------------------------------------------------------------
    step("5. and the task moves BECAUSE OF WHAT ARRIVED, not because we say so");
    // -----------------------------------------------------------------------
    //
    // Everything up to here happened inside one agent's own store. `agent.task`
    // opens a task in `submitted` and leaves it there on purpose — "local state
    // is never a claim about the remote agent" — so `working` and `completed`
    // were states this repository could reach only by driving `TaskStore`
    // itself, which is one process asserting about itself.
    //
    // This step is the other thing. Each agent SERVES the task it just read,
    // publishing A2A `TaskStatusUpdateEvent`s on that task's topic; and each
    // agent POLLS its own task's topic and applies what the peer published. The
    // terminal state below is asserted on THIS agent's store and was written
    // into it by a frame from the other account.
    const auto publish = [&](const QString &state, const QString &text) {
        return call(p, "agent.update",
                    QStringLiteral(R"({"agent_address":"%1","task_id":"%2","context_id":"%3",)"
                                   R"("state":"%4","message":"%5"})")
                        .arg(me, theirTask, servedContext, state, text));
    };
    QJsonObject u = publish(QStringLiteral("working"),
                            QStringLiteral("accepted, reading the request"));
    check(u.value("ok").toBool(), "this agent publishes `working` for the task it was asked to "
                                  "do: " + u.value("error").toString());
    check(u.value("from").toString() == me,
          "signed with its own account — which agent.update reads off this module's "
          "configuration, not off the call: " + u.value("from").toString());
    check(u.value("final").toBool() == false, "and not final, because `working` is not terminal");
    u = publish(QStringLiteral("completed"), QStringLiteral("done"));
    check(u.value("ok").toBool() && u.value("final").toBool(),
          "then `completed`, which agent.update marks final because the state is");

    // THE DECOY, AND IT IS NOT OPTIONAL.
    //
    // A Delivery node receives its own published messages. So this agent now
    // publishes, on the very topic it is about to read, a `completed` update
    // about its OWN task — the frame a process talking to itself would use to
    // satisfy every assertion below with the other agent switched off. It is
    // identical to the peer's in every respect but the account it names.
    u = call(p, "agent.update",
             QStringLiteral(R"({"agent_address":"%1","task_id":"%2","context_id":"%3",)"
                            R"("state":"completed","message":"I declare myself served"})")
                 .arg(other, myTask, myContext));
    check(u.value("ok").toBool(),
          "and it puts a forged `completed` for its OWN task on the topic it reads");

    // THE LOOP WAITS FOR BOTH FACTS, AND THIS IS THE SECOND TIME THAT RULE HAS
    // HAD TO BE APPLIED IN THIS FILE.
    //
    // Written the obvious way — poll until the task is `completed` — the
    // self-ignored assertion below becomes an assertion about the scheduler.
    // The decoy is published to this node a moment before the loop starts, but
    // it comes back through the relay like anything else, and if the peer's two
    // updates were already waiting the FIRST poll applies them, exits with the
    // task complete, and the decoy is counted in a poll that never happens. That
    // is exactly the shape `docs/limitations.md` records under "an assertion
    // that was right most of the time": it would pass nearly always and fail on
    // an afternoon when nothing was wrong.
    //
    // So both conditions gate the exit. Neither is about ordering between the
    // processes: one says the peer's terminal update arrived, the other says
    // this agent's own frame came back off its own node, and a bounded loop that
    // never sees either fails honestly rather than intermittently.
    note(QStringLiteral("polling for the peer's updates, up to %1 rounds of 3 s%2")
             .arg(updateRounds)
             .arg(price.isEmpty() ? QString()
                                  : QStringLiteral(" — this agent advertises a price, so its "
                                                   "peer is paying and is blocked while it proves")));
    for (int i = 0; i < updateRounds; ++i) {
        updates = call(p, "agent.poll",
                       QStringLiteral(R"({"agent_address":"%1","task_id":"%2","since":%3})")
                           .arg(other, myTask)
                           .arg(since));
        if (!updates.value("ok").toBool()) {
            note("agent.poll: " + updates.value("error").toString());
            break;
        }
        absorb(updates);
        if (state == QLatin1String("completed") && selfIgnored >= 1) break;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    note("agent.poll: " +
         QString::fromUtf8(QJsonDocument(updates).toJson(QJsonDocument::Compact)).left(400));
    check(state == QLatin1String("completed"),
          "THIS agent's own TaskStore reached `completed`, and every transition into it came "
          "off the wire");
    check(applied >= 2,
          QStringLiteral("applying %1 status update(s) the peer published").arg(applied));
    // `!authors.contains(me)` is true of an empty list, and in the run that
    // found this the list WAS empty — so this line printed ok next to a FAIL
    // saying nothing had been applied. An assertion that holds when the thing
    // it describes did not happen is not an assertion, and this repository has
    // spent a day removing that shape. The non-emptiness is part of the claim.
    check(!authors.isEmpty() && !authors.contains(me) &&
              authors.count(other) == int(authors.size()),
          QStringLiteral("all %1 of them published by ").arg(authors.size()) + other +
              ", the OTHER account — none by this one");
    check(selfIgnored >= 1,
          QStringLiteral("while the forged update this agent published about its own task was "
                         "read back off the same topic and refused (%1 of them)")
              .arg(selfIgnored));
    QStringList walk;
    for (const QJsonValue &s : history) walk.append(s.toString());
    check(walk == QStringList{QStringLiteral("submitted"), QStringLiteral("working"),
                              QStringLiteral("completed")},
          "and the walk it records is submitted -> working -> completed: " + walk.join(" -> "));

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "FAILED"
                          : "two loaded modules discovered each other and ran a task lifecycle "
                            "across the network",
                 failures);
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// signers: what the module does with what its pay_signer and policy_source say
// ---------------------------------------------------------------------------
//
// The `settle` mode below costs a real settlement on a testnet with no faucet,
// so it can be run once and it proves the happy path. THIS mode is the other
// eleven-twelfths of the behaviour, it costs nothing, and it is the one to run
// when either delegate changes.
//
// The two decisions being pinned:
//
//   1. WHAT COUNTS AS AN ENVELOPE. `agent.task` reads three decimal strings out
//      of `policy_source`. Anything it cannot read is *unknown*, and unknown is
//      OUTSIDE — which holds the payment for the owner rather than making it.
//      A source that returns nothing, a source that returns prose, and a source
//      that returns a limit smaller than the price all have to reach the same
//      refusal, and none of them may reach the signer.
//
//   2. WHAT COUNTS AS A SETTLEMENT. `pay_signer` is a command an operator names,
//      and `agent.task` writes what it prints into the task record as proof the
//      task was paid for. So a signer that prints nothing, and a signer that
//      prints a diagnostic, must both leave the task unpaid — the difference
//      between the two is exactly the difference between a settlement and a
//      sentence.
//
// The last case here hands the module 64 characters of hex that name no
// transaction, and asserts the module reports them. That is not a claim that
// anything settled: the module cannot check a hash against a chain it has no
// client for, which is precisely why `scripts/agent-spend.py` confirms inclusion
// itself and prints nothing until a block holds the transaction. It is here as
// the positive control, because without it "the payment did not settle" is
// equally consistent with a module that never runs its signer at all.

int runSigners(const QString &path, const QString &me)
{
    LogosProviderObject *p = bringUpModule(path);
    if (!p) return 1;

    const auto configure = [&](const char *key, const QString &value) {
        const QJsonObject r = call(p, "meta.configure",
                                   QStringLiteral(R"({"key":"%1","value":%2})")
                                       .arg(QString::fromUtf8(key),
                                            QString::fromUtf8(QJsonDocument(
                                                QJsonObject{{"v", value}}).toJson(
                                                QJsonDocument::Compact))
                                                .section(':', 1)
                                                .chopped(1)));
        check(r.value("ok").toBool(),
              QStringLiteral("meta.configure %1").arg(QString::fromUtf8(key)) +
                  (r.value("ok").toBool() ? QString() : ": " + r.value("error").toString()));
    };
    // A priced task addressed to a peer that does not exist. Nothing here waits
    // on an answer: `agent.task` sends the request and settles, and it is the
    // settling that is under test.
    const auto task = [&](int price, int n) {
        return call(p, "agent.task",
                    QStringLiteral(R"({"agent_address":"%1","skill":"storage.upload",)"
                                   R"("task_id":"signers%2","price":%3,)"
                                   R"("pay_account":"Public/%1","params":{}})")
                        .arg(me).arg(n).arg(price));
    };

    step("1. an envelope nobody anchored: every price is outside it");
    configure("agent_account", me);
    QJsonObject r = task(1, 1);
    note("agent.task: " + QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(!r.value("ok").toBool() &&
              r.value("error").toString().contains(QLatin1String("outside the owner's envelope")),
          "with no policy_source the envelope is unknown, and unknown is outside: " +
              r.value("error").toString());
    check(r.value("submitted").toBool() == false && r.value("reason").toString().contains(
              QLatin1String("not known to this process")),
          "and the reason names the envelope, not the transport: " +
              r.value("reason").toString());

    step("2. a policy_source that answers with prose is not an envelope");
    configure("policy_source", QStringLiteral("printf 'per_tx is twenty five'"));
    r = task(1, 2);
    check(!r.value("ok").toBool() &&
              r.value("reason").toString().contains(QLatin1String("not known to this process")),
          "an unparseable answer is unknown, which is outside — it is not zero and it is not "
          "unlimited: " + r.value("reason").toString());

    step("3. a policy_source with a hole in it is not an envelope either");
    // Two fields of three. `outsideEnvelope` refuses on the FIRST unreadable
    // one, so a source that answers two questions and not the third must not
    // produce a comparison against a total nobody supplied.
    configure("policy_source",
              QStringLiteral(R"(printf '{"per_tx":"25","per_period":"250"}')"));
    r = task(1, 3);
    check(!r.value("ok").toBool() &&
              r.value("reason").toString().contains(QLatin1String("period total is unknown")),
          "a missing running total is unknown, and assuming zero is how a period limit becomes "
          "decorative: " + r.value("reason").toString());

    step("4. a real envelope, and a price above it");
    configure("policy_source",
              QStringLiteral(R"(printf '{"per_tx":"25","per_period":"250","spent":"240"}')"));
    r = task(26, 4);
    check(!r.value("ok").toBool() &&
              r.value("reason").toString().contains(QLatin1String("per-transaction limit of 25")),
          "26 is over the per-transaction limit: " + r.value("reason").toString());
    r = task(20, 5);
    check(!r.value("ok").toBool() &&
              r.value("reason").toString().contains(QLatin1String("per-period limit of 250")),
          "and 20 is over what is left of the period, with 240 already spent: " +
              r.value("reason").toString());

    step("5. inside the envelope — and now the transport is what is missing");
    configure("policy_source",
              QStringLiteral(R"(printf '{"per_tx":"25","per_period":"250","spent":"0"}')"));
    r = task(1, 6);
    check(!r.value("ok").toBool() &&
              r.value("error").toString() == QLatin1String("delivery node is not started"),
          "1 LEZ is inside, so the owner is not asked and the next thing to fail is the node: " +
              r.value("error").toString());

    step("6. start the node, because the signer is only reached past it");
    check(call(p, "meta.configure", QStringLiteral(R"({"key":"delivery","value":"on"})"))
              .value("ok").toBool(), "meta.configure('delivery','on')");
    const bool up = waitReady(p, 240);
    check(up, "the module's own Delivery node came up");
    if (!up) return 1;

    step("7. a signer that says nothing has not settled anything");
    configure("pay_signer", QStringLiteral("printf ''"));
    r = task(1, 7);
    note("agent.task: " + QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(!r.value("ok").toBool() &&
              r.value("error").toString().contains(QLatin1String("did not settle")),
          "the request went out and the payment did not: " + r.value("error").toString());

    step("8. and a signer that prints a diagnostic has not settled anything either");
    configure("pay_signer", QStringLiteral("printf 'error: this signer holds no key'"));
    r = task(1, 8);
    check(!r.value("ok").toBool() &&
              r.value("error").toString().contains(QLatin1String("did not settle")),
          "a sentence is not a transaction hash, and the module refuses it rather than writing "
          "it into the task record: " + r.value("error").toString());

    step("9. the positive control — the module does run its signer and does report it");
    const QString fake =
        QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    configure("pay_signer", QStringLiteral("printf '%1'").arg(fake));
    r = task(1, 9);
    check(r.value("ok").toBool() && r.value("settlement_tx").toString() == fake,
          "64 hex characters come back as the settlement hash — which is what makes steps 7 "
          "and 8 mean 'refused' rather than 'never called'. NOTHING SETTLED HERE: this hash "
          "names no transaction, and it is agent-spend.py, not the module, that refuses to "
          "print one before a block holds it");

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "FAILED"
                          : "the module's envelope and its signer both refuse what they should",
                 failures);
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
                     "       %s <plugin> approval <me> <owner> <recipient> <amount>\n"
                     "       %s <plugin> signers <me>\n",
                     argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const QString path = QString::fromUtf8(argv[1]);
    const QString mode = QString::fromUtf8(argv[2]);
    if (mode == QLatin1String("probe")) return runProbe(path);
    if (mode == QLatin1String("signers")) {
        if (argc < 4) {
            std::fprintf(stderr, "signers needs <me>\n");
            return 2;
        }
        return runSigners(path, QString::fromUtf8(argv[3]));
    }
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
