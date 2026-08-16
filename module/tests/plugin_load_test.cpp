// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Does the built module actually load as a Logos Core plugin?
//
// Shipping a plugin is not the same as it loading. Basecamp gives no visible
// error when a module fails to load — the sidebar tile is simply inert — so
// "it builds" is not evidence and neither is "it is in the package". This
// drives the same three steps the host performs, in order, and asserts on each:
//
//   1. QPluginLoader accepts the binary and reports the interface IID the host
//      compares against — `org.logos.LogosProviderPlugin`.
//   2. The embedded metadata is the module's own metadata.json, and its `main`
//      names the file that was actually built. A `main` that names a file which
//      does not exist is the failure mode that is invisible until load time.
//   3. `qobject_cast` to both interfaces succeeds across the plugin boundary,
//      the provider object constructs, and its published method table is
//      callable — with the module's real behaviour on the other side, not a
//      stub: a malformed policy hash is refused, a well-formed one is accepted,
//      a second bind is refused, and an unregistered skill fails without
//      taking the module down.
//   4. The loaded module offers the skills it documents. This step used to
//      assert the opposite — that a freshly loaded module honestly reports no
//      skills — which was true and was the whole problem: thirteen skills were
//      implemented, tested and merged while nothing registered them with the
//      module, so the binary that loaded in Basecamp answered `skills()` with
//      an empty array and `invoke()` with "no skill named …" for every one of
//      them. An empty card is a valid Agent Card, so that state is
//      indistinguishable from a working agent that happens to do nothing.
//      What is asserted now is that every documented skill is listed with a
//      parameter schema and that `invoke()` dispatches to it — including the
//      skills whose ports nobody wired, which must answer as themselves
//      ("no sequencer connection is wired") rather than as the registry
//      ("no skill named …"). Those two are the difference between a skill that
//      is missing and one that is unconfigured.
//
// Build and run: see docs/basecamp.md. Every step is an assertion and the exit
// code is the result.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPluginLoader>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdio>

#include "interface.h"
#include "logos_provider_object.h"

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    std::fprintf(stderr, "  %s  %s\n", condition ? "ok  " : "FAIL", qPrintable(what));
    if (!condition) {
        ++failures;
    }
}

void note(const QString &what)
{
    std::fprintf(stderr, "  <-    %s\n", qPrintable(what));
}

/// The module's own JSON string results, checked for a substring rather than
/// parsed: the assertion is about behaviour reaching this side of the boundary,
/// and a parser here would only add a second thing that can be wrong.
bool contains(const QVariant &value, const char *needle)
{
    return value.toString().contains(QString::fromUtf8(needle));
}

/// Every skill this module ships with, spelled out rather than counted, so a
/// skill that stops being registered fails this harness *by name*.
const char *const kSkills[] = {
    "messaging.send",      "messaging.receive", "messaging.join",
    "messaging.create_group",
    "storage.upload",      "storage.download", "storage.list",
    "storage.share",       "wallet.balance",   "wallet.send",
    "wallet.history",      "program.query",    "program.call",
    "program.deploy",      "agent.card",       "agent.discover",
    "agent.task",          "agent.subscribe",  "agent.update",
    "agent.poll",          "agent.cancel",
    "meta.status",         "meta.configure",   "meta.skills",
    "agent.evaluate_task",
    // The owner's end of the approval channel, which a SECOND Logos app calls
    // to hear a spend request and answer it over Logos Messaging. Registered in
    // every build, node or no node, so that the registry is not a fact about
    // how the binary was compiled.
    "owner.watch",         "owner.pending",    "owner.answer",
};
constexpr int kSkillCount = int(sizeof(kSkills) / sizeof(kSkills[0]));

/// The registry's refusal, and the refusal `invoke()` gives BEFORE it ever
/// consults the registry.
///
/// `module/tests/logos_core_load_test.cpp` carries both of these and explains at
/// length why: `{"ok":false,"error":"agent is not started"}` starts with `{`,
/// contains `"ok":false`, and does NOT contain `no skill named`, so a module
/// that never started satisfies every dispatch check phrased as the absence of
/// the registry's message. That harness was hardened; this one was not, and the
/// hole was still open. Measured, against the shipped darwin-arm64 plugin loaded
/// through QPluginLoader, with the `start()` call removed:
///
///     ok    invoke() dispatches to every one of them: undispatched none
///     <-    invoke(wallet.balance): {"error":"agent is not started","ok":false}
///     ok    an unwired skill refuses as itself, not as a name nobody registered
///
/// Both strings are failures below, and they are kept apart because they mean
/// different things to whoever reads the output: one says the module is not up,
/// the other says the name is not in its registry.
const char *const kUnregistered = "no skill named";
const char *const kNotStarted = "agent is not started";

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path to agent_plugin.dylib|.so>\n", argv[0]);
        return 2;
    }

    // ---- 1. the loader ----------------------------------------------------
    QPluginLoader loader(QString::fromUtf8(argv[1]));
    const QJsonObject meta = loader.metaData();

    check(!meta.isEmpty(), "the binary carries Qt plugin metadata");
    const QString iid = meta.value("IID").toString();
    note("IID: " + iid);
    check(iid == QStringLiteral("org.logos.LogosProviderPlugin"),
          "the interface IID is the one Logos Core casts against");

    // ---- 2. the embedded manifest ----------------------------------------
    const QJsonObject embedded = meta.value("MetaData").toObject();
    note("name: " + embedded.value("name").toString()
         + ", main: " + embedded.value("main").toString()
         + ", type: " + embedded.value("type").toString());
    check(embedded.value("name").toString() == QStringLiteral("agent"),
          "metadata.json is embedded and names this module");
    check(embedded.value("type").toString() == QStringLiteral("core"),
          "it declares itself a core module");

    // The host resolves `main` to a filename inside the module directory, so a
    // manifest naming something the package does not contain fails to load
    // before Qt is ever reached — with no error that points at the manifest.
    const QString main = embedded.value("main").toString();
    const QString built = QString::fromUtf8(argv[1]).section('/', -1).section('.', 0, 0);
    check(main == built,
          QStringLiteral("`main` (%1) names the file that was built (%2)").arg(main, built));

    // ---- 3. the instance and the two interfaces ---------------------------
    QObject *instance = loader.instance();
    if (!instance) {
        check(false, "the plugin instantiates: " + loader.errorString());
        return failures ? 1 : 0;
    }
    check(true, "the plugin instantiates");

    auto *asPlugin = qobject_cast<PluginInterface *>(instance);
    check(asPlugin != nullptr, "it casts to PluginInterface across the boundary");

    auto *asProvider = qobject_cast<LogosProviderPlugin *>(instance);
    check(asProvider != nullptr, "it casts to LogosProviderPlugin across the boundary");

    if (!asPlugin || !asProvider) {
        return 1;
    }

    note("name(): " + asPlugin->name() + ", version(): " + asPlugin->version());
    check(asPlugin->name() == QStringLiteral("agent"), "it reports its module name");

    LogosProviderObject *provider = asProvider->createProviderObject();
    check(provider != nullptr, "createProviderObject() returns a provider");
    if (!provider) {
        return 1;
    }

    check(provider->providerName() == QStringLiteral("agent"),
          "the provider names the module");

    // ---- the published method table ---------------------------------------
    QSet<QString> published;
    for (const QJsonValue &entry : provider->getMethods()) {
        published.insert(entry.toObject().value("name").toString());
    }
    note("getMethods(): " + QStringList(published.values()).join(", "));
    for (const char *expected : {"configure", "start", "stop", "skills", "status", "invoke"}) {
        check(published.contains(QString::fromUtf8(expected)),
              QStringLiteral("the method table publishes %1()").arg(expected));
    }

    // ---- the module's real behaviour, through callMethod ------------------
    const QString owner = QStringLiteral("lez1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq");
    // The blockchain agent's account id, which is the 32-byte seed the current
    // program derives its policy account from — `PDA(program, ["agent-policy/v1",
    // agent_id])`. Read it out of artifacts/agents.tsv (`agent_id`, base58
    // A7UBoMbSo…) rather than out of this comment.
    //
    // Any 64-hex string exercises the same path. Using a live one means a reader
    // who greps for it lands on something the chain really holds — but it also
    // means this constant goes stale on a redeploy, and it did: it used to be a
    // policy hash from `artifacts/anchored.tsv` "under the current program
    // 8c87cc9b…", two programs after that stopped being the current program, and
    // under a scheme where the limits were part of the seed at all. Nothing here
    // could fail on that, because `configure()` only checks the shape.
    // `scripts/verify-deployment.sh` is what checks the identifiers against the
    // chain; this is a well-formed input, and that is all it has to be.
    const QString policy =
        QStringLiteral("8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90");

    QVariant status = provider->callMethod(QStringLiteral("status"), {});
    note("status(): " + status.toString());
    check(contains(status, "\"configured\":false"),
          "before configure it reports itself unconfigured");

    QVariant bad = provider->callMethod(QStringLiteral("configure"),
                                        {owner, QStringLiteral("not-a-policy-hash")});
    check(!bad.value<LogosResult>().success,
          "configure refuses a malformed policy hash");

    QVariant good = provider->callMethod(QStringLiteral("configure"), {owner, policy});
    check(good.value<LogosResult>().success,
          "configure accepts the anchored policy hash");

    QVariant rebind = provider->callMethod(QStringLiteral("configure"), {owner, policy});
    check(!rebind.value<LogosResult>().success,
          "a second configure is refused — the binding is the agent's identity");

    // Before start, deliberately an error object and not `[]`: an empty array is
    // a valid Agent Card and a caller cannot tell it from an agent that has no
    // skills.
    QVariant early = provider->callMethod(QStringLiteral("skills"), {});
    check(contains(early, "\"error\"") && !contains(early, "["),
          "before start, skills() is an error rather than an empty card");

    QVariant started = provider->callMethod(QStringLiteral("start"), {});
    check(started.value<LogosResult>().success, "start succeeds once configured");

    status = provider->callMethod(QStringLiteral("status"), {});
    note("status(): " + status.toString());
    check(contains(status, "\"started\":true"), "status reflects the running agent");

    // ---- 4. the skills the loaded module actually offers -------------------
    //
    // Parsed here rather than substring-matched, because the assertion is about
    // the document's shape: one entry per skill, each with a schema another
    // agent can call it from.
    QVariant skills = provider->callMethod(QStringLiteral("skills"), {});
    QJsonParseError parseError{};
    const QJsonDocument card = QJsonDocument::fromJson(skills.toString().toUtf8(), &parseError);
    check(parseError.error == QJsonParseError::NoError && card.isArray(),
          "after start, skills() is a JSON array: " + parseError.errorString());

    const QJsonArray entries = card.array();
    QSet<QString> listed;
    QStringList schemaless;
    for (const QJsonValue &entry : entries) {
        const QJsonObject object = entry.toObject();
        const QString name = object.value("name").toString();
        listed.insert(name);
        if (!object.value("parameters").isObject()) {
            schemaless << (name + ": " + object.value("error").toString());
        }
    }
    note(QStringLiteral("skills(): %1 entries: ").arg(entries.size())
         + QStringList(listed.values()).join(", "));

    QStringList missing;
    for (const char *skill : kSkills) {
        if (!listed.contains(QString::fromUtf8(skill))) {
            missing << QString::fromUtf8(skill);
        }
    }
    check(missing.isEmpty(), "every skill the module ships with is listed: missing "
                                 + (missing.isEmpty() ? QStringLiteral("none")
                                                      : missing.join(", ")));
    check(entries.size() == kSkillCount,
          QStringLiteral("the card has exactly %1 entries, and %2 distinct names")
              .arg(kSkillCount)
              .arg(listed.size()));
    check(schemaless.isEmpty(),
          "each carries a parameter schema: " + (schemaless.isEmpty()
                                                     ? QStringLiteral("all present")
                                                     : schemaless.join("; ")));

    // Dispatch, across the plugin boundary. A skill nobody wired must refuse as
    // itself — the registry's "no skill named" would mean it is not there at
    // all, which is the state this step exists to tell apart.
    QStringList undispatched, unstarted;
    for (const char *skill : kSkills) {
        const QVariant answer = provider->callMethod(
            QStringLiteral("invoke"), {QString::fromUtf8(skill), QStringLiteral("{}")});
        if (contains(answer, kNotStarted)) unstarted << QString::fromUtf8(skill);
        if (!answer.toString().startsWith(QLatin1Char('{'))
            || contains(answer, kUnregistered)) {
            undispatched << QString::fromUtf8(skill);
        }
    }
    check(unstarted.isEmpty(),
          QStringLiteral("the module answered as a running agent, not \"%1\": %2")
              .arg(QString::fromUtf8(kNotStarted),
                   unstarted.isEmpty() ? QStringLiteral("none said it")
                                       : unstarted.join(", ")));
    check(undispatched.isEmpty() && kSkillCount > 0,
          "invoke() dispatches to every one of them: undispatched "
              + (undispatched.isEmpty() ? QStringLiteral("none") : undispatched.join(", ")));

    // The one skill that answers without any port wired: what the module knows
    // about itself. It is also how an operator sees the binding from outside.
    QVariant metaStatus = provider->callMethod(QStringLiteral("invoke"),
                                               {QStringLiteral("meta.status"),
                                                QStringLiteral("{}")});
    note("invoke(meta.status): " + metaStatus.toString());
    check(contains(metaStatus, "\"ok\":true") && contains(metaStatus, "\"started\":true")
              && metaStatus.toString().contains(policy),
          "meta.status answers over the boundary with what the agent is bound to");

    // The catalogue the prize asks for by name, over the same boundary. This is
    // the check that would have caught its absence: it was documented in three
    // C++ headers and in the A2A binding while `invoke("meta.skills")` answered
    // "no skill named 'meta.skills' is registered", because nothing in this
    // harness had ever asked the loaded binary for it. Asserted on its content,
    // not just its shape — a catalogue that omits the skill listing it, or
    // disagrees with `skills()`, is the same defect one layer along.
    QVariant catalogue = provider->callMethod(QStringLiteral("invoke"),
                                              {QStringLiteral("meta.skills"),
                                               QStringLiteral("{}")});
    note("invoke(meta.skills): " + catalogue.toString());
    const QJsonObject listing =
        QJsonDocument::fromJson(catalogue.toString().toUtf8()).object();
    const QJsonArray listed2 = listing.value("skills").toArray();
    QSet<QString> catalogued;
    for (const QJsonValue &entry : listed2) {
        const QJsonObject object = entry.toObject();
        if (object.value("parameters").isObject()) {
            catalogued.insert(object.value("name").toString());
        }
    }
    check(listing.value("ok").toBool() && listed2.size() == kSkillCount
              && listing.value("count").toInt() == kSkillCount,
          QStringLiteral("meta.skills lists all %1 skills over the boundary, and counts them")
              .arg(kSkillCount));
    // `catalogued == listed` ON ITS OWN IS SATISFIED BY TWO EMPTY SETS.
    //
    // `catalogued` is only appended to inside `if (object.value("parameters")
    // .isObject())`, and `listed` only inside the loop over `entries`, so a
    // `meta.skills` that answered nothing and a `skills()` that answered nothing
    // compare equal and this line printed `ok` having compared no skill against
    // no skill — the "ok (0 checked)" shape `module/tests/skills_test.cpp` and
    // `module/tests/logos_core_load_test.cpp` have each already had removed,
    // both times with the note that a line must be anchored on its own rather
    // than on its neighbours. The equality is the claim; the count is what makes
    // the claim about something.
    check(!catalogued.isEmpty() && catalogued.size() == kSkillCount && catalogued == listed,
          QStringLiteral("and all %1 of them carry the parameter schema skills() published "
                         "for it (%2 compared)")
              .arg(kSkillCount)
              .arg(catalogued.size()));
    // The control, so the comparison above is known to be able to say no. One
    // name removed from the catalogue's side and the same `==` has to come out
    // false — which it cannot do if either side is empty, so this line fails on
    // exactly the state the line above used to pass on.
    QSet<QString> oneShort = catalogued;
    if (!oneShort.isEmpty()) {
        oneShort.remove(*oneShort.constBegin());
    }
    check(!oneShort.isEmpty() && oneShort != listed,
          "and the same comparison says no when one entry is taken away from it");
    check(catalogued.contains(QStringLiteral("meta.skills")),
          "including itself: it is a registered skill, not a special case in invoke()");

    // A skill whose transport nobody wired: refused, and refused with the port
    // it is missing rather than with a claim that it worked.
    QVariant unwired = provider->callMethod(QStringLiteral("invoke"),
                                            {QStringLiteral("wallet.balance"),
                                             QStringLiteral("{}")});
    note("invoke(wallet.balance): " + unwired.toString());
    check(contains(unwired, "\"ok\":false") && !contains(unwired, kUnregistered)
              && !contains(unwired, kNotStarted),
          "an unwired skill refuses as itself, not as a name nobody registered "
          "and not as a module that never started");

    QVariant missingName = provider->callMethod(QStringLiteral("invoke"),
                                                {QStringLiteral("no.such.skill"),
                                                 QStringLiteral("{}")});
    note("invoke(no.such.skill): " + missingName.toString());
    check(contains(missingName, "\"ok\":false") && contains(missingName, "no skill named"),
          "and a name that is not registered is refused as that, without taking the module down");

    QVariant stopped = provider->callMethod(QStringLiteral("stop"), {});
    check(stopped.value<LogosResult>().success, "stop succeeds");

    delete provider;
    loader.unload();

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "SOME CHECKS FAILED" : "all steps confirmed", failures);
    return failures ? 1 : 0;
}
