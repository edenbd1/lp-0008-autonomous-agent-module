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
//
// Build and run: see docs/basecamp.md. Every step is an assertion and the exit
// code is the result.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QPluginLoader>
#include <QSet>
#include <QString>
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

    // The host resolves `main` to a filename inside the module directory. This
    // is the check LP-0005's package failed: its manifest named something the
    // package did not contain, so loading failed before Qt was reached.
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
    const QString policy =
        QStringLiteral("b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549");

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

    QVariant started = provider->callMethod(QStringLiteral("start"), {});
    check(started.value<LogosResult>().success, "start succeeds once configured");

    status = provider->callMethod(QStringLiteral("status"), {});
    note("status(): " + status.toString());
    check(contains(status, "\"started\":true"), "status reflects the running agent");

    // No skills are linked into the module yet, so the honest answer to both of
    // these is an error — not an empty success. An empty skill list would be
    // published as a valid, empty Agent Card.
    QVariant skills = provider->callMethod(QStringLiteral("skills"), {});
    note("skills(): " + skills.toString());

    QVariant missing = provider->callMethod(QStringLiteral("invoke"),
                                            {QStringLiteral("wallet.balance"),
                                             QStringLiteral("{}")});
    note("invoke(wallet.balance): " + missing.toString());
    check(contains(missing, "\"ok\":false"),
          "invoking an unregistered skill fails rather than crashing the module");

    QVariant stopped = provider->callMethod(QStringLiteral("stop"), {});
    check(stopped.value<LogosResult>().success, "stop succeeds");

    delete provider;
    loader.unload();

    std::fprintf(stderr, "\n%s (%d failure(s))\n",
                 failures ? "SOME CHECKS FAILED" : "all steps confirmed", failures);
    return failures ? 1 : 0;
}
