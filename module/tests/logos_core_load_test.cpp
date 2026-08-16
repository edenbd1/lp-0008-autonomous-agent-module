/* SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Does Logos Core itself load the agent module — and does the module it loaded
 * offer its skills?
 *
 * `plugin_load_test.cpp` proves the plugin satisfies the Qt side of the
 * contract. This proves the other side: it drives the real `liblogos_core`
 * shipped inside the Logos app, through the same C API and in the same order
 * as the app's own `main.cpp` —
 *
 *     logos_core_init            → logos_core_add_modules_dir  (embedded)
 *     logos_core_add_modules_dir (user)
 *     logos_core_set_persistence_base_path
 *     logos_core_set_access_policy(NULL)
 *     logos_core_start
 *     logos_core_load_module("<name>", with_dependencies = true)
 *
 * — and asserts that the module turns up first in the runtime's *known*
 * modules and then in its *loaded* modules. Basecamp itself only auto-loads
 * `package_manager` and `package_downloader`; everything else is loaded by
 * this exact call when the user enables it, so this is that step, not an
 * imitation of it.
 *
 * Then it keeps going, because "loaded" was never the claim worth making. A
 * module that loads and answers `skills()` with `[]` is worse than one that
 * fails to load: an empty Agent Card is a valid Agent Card, so a reviewer sees
 * a module that installed, enabled, and does nothing, with no error anywhere
 * to say why. That is the state this repository shipped in until the skills
 * were registered, and this harness asserted it — honestly, and uselessly.
 * What it asserts now is the thing a reviewer actually wants to know:
 *
 *   - the installed manifest's `main` names a file that is really there
 *     (a `main` naming a file the package does not contain is an invisible
 *     load failure — the host resolves it, finds nothing, and says nothing);
 *   - `configure()` and `start()` are accepted across the runtime's own
 *     transport, not merely in-process;
 *   - `skills()` lists every one of the module's documented skills, each with
 *     a parameter schema;
 *   - `invoke()` reaches every one of them. An unwired port makes a skill
 *     refuse *as itself* ("no sequencer connection is wired"); only an
 *     unregistered name is refused by the registry ("no skill named …"). The
 *     harness asserts both directions, so "dispatched" cannot be confused with
 *     "swallowed".
 *
 * Everything after the load goes over the wire, through `LogosAPI` /
 * `LogosAPIClient` — the same SDK facade Basecamp's `main.cpp` constructs
 * (`LogosAPI logosAPI("core", nullptr)`) and the only way to reach a core
 * module, which the runtime runs in its own `logos_host` process. So these are
 * calls into the loaded module, not into a copy of it linked here.
 *
 * `liblogos_core` is dlopen'd from the installed app rather than linked for
 * the C API, so what runs is the shipped binary, not a rebuild of it. The one
 * thing this harness must bring itself is a QCoreApplication: the runtime
 * assumes the host application already created one before `logos_core_init`
 * (Logos Basecamp constructs its QApplication first), and without it the
 * module transport's event loop cannot start and the load hangs. So the
 * harness links the app's *own* bundled QtCore — one QtCore in the process,
 * the same one the runtime resolves through @rpath.
 *
 * Usage:
 *   logos_core_load_test <liblogos_core> <embedded-modules-dir>
 *                        <user-modules-dir> <persistence-dir> <module-name>
 *
 * Every step is an assertion and the exit code is the result.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_types.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <utility>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    fprintf(stderr, "  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) {
        ++failures;
    }
}

static void note(const QString &what)
{
    fprintf(stderr, "  <-    %s\n", qPrintable(what));
}

/* The list getters hand back a malloc'd, NULL-terminated char* array. */
static int contains(char **names, const char *needle)
{
    if (!names) {
        return 0;
    }
    for (char **p = names; *p; ++p) {
        if (strcmp(*p, needle) == 0) {
            return 1;
        }
    }
    return 0;
}

static void show(const char *label, char **names)
{
    fprintf(stderr, "  <-    %s:", label);
    if (names) {
        for (char **p = names; *p; ++p) {
            fprintf(stderr, " %s", *p);
        }
    }
    fprintf(stderr, "\n");
}

template <typename Fn>
static Fn must_sym(void *lib, const char *name)
{
    void *sym = dlsym(lib, name);
    if (!sym) {
        fprintf(stderr, "  FAIL  %s is not exported by the library\n", name);
        exit(2);
    }
    return reinterpret_cast<Fn>(sym);
}

namespace {

/// Every skill this module ships with, spelled out rather than counted, so a
/// skill that stops being registered fails this harness *by name*. Same table
/// as `plugin_load_test.cpp`, asserted here against the module the *runtime*
/// loaded rather than the one `QPluginLoader` opened.
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

/// The registry's refusal, spelled exactly as `AgentModuleImpl::invoke` emits
/// it — `no skill named '<name>' is registered`. Its absence is what "the call
/// reached the skill" means: a skill that refuses because nobody wired its port
/// answers as itself and never produces this. Matching a phrase that the module
/// does not actually emit would make the dispatch check below pass for free,
/// which is why the control at the end asserts this string *does* appear for a
/// name nobody registered.
const char *const kUnregistered = "no skill named";

/// The refusal `invoke()` gives *before* it ever consults the registry, when the
/// module has not been started.
///
/// It has to be counted as a failure to dispatch, and it was not. `agent is not
/// started` does not contain @ref kUnregistered, so a module that never started
/// answered every one of the 22 names with it and the dispatch check printed  (count-as-it-was)
/// `ok invoke() dispatches to every one of the 22` — directly under  (count-as-it-was)
/// `skills(): 0 entries`. A check that passes hardest when nothing is running is
/// worse than no check: it is the exact configuration this harness exists to
/// catch, reported as a pass. Both strings are now failures, and they are kept
/// apart because they mean different things to whoever reads the output: one
/// says the module is not up, the other says the name is not in its registry.
const char *const kNotStarted = "agent is not started";

/// THE SHAPE BOTH OF THOSE STRINGS EXIST TO GUARD AGAINST, stated once so the
/// checks below can be read against it.
///
/// An assertion whose pass condition is *"the answer did not contain X"* is
/// satisfied by every answer that is not X — including the ones that mean
/// nothing ran. The same defect wearing its other hat is *"the field is not
/// true"*: `QJsonValue::toBool()` on a key that is absent returns `false`, so
/// `answer.value("submitted").toBool() == false` passes for
/// `{"ok":false,"error":"agent is not started"}`, which carries no `submitted`
/// key at all. And its third hat is *"the call did not succeed"*: `resultOk()`
/// is false both for a `LogosResult` that failed and for a call that never
/// arrived, so `!resultOk(...)` reads a dead transport as a refusal.
///
/// Three checks in this file had one of those three shapes, and all three
/// printed `ok` in a run where the module never started. Every check that is
/// about the *behaviour* of a running module therefore now asserts something
/// positive: a field is **present** and has a value, an answer echoes the terms
/// it was asked about, a `LogosResult` **came back** and says why it refused.
/// The rule of thumb, since it will come up again: if breaking the module makes
/// a line print `ok`, the line is checking the wrong thing.

/// The installed layout is the package flattened: `manifest.json` beside the
/// binary it names, plus a `variant` file saying which variant was unpacked.
/// `main` is resolved by the host against that directory, and a `main` that
/// names a file which is not there fails the load before Qt is ever reached —
/// silently, because nothing logs a filename it could not find.
void checkInstalledManifest(const QString &moduleDir)
{
    QFile manifestFile(moduleDir + "/manifest.json");
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        check(false, "the installed module directory has a manifest.json: "
                         + moduleDir);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        check(false, "the installed manifest.json parses: " + parseError.errorString());
        return;
    }
    const QJsonObject manifest = doc.object();

    // Which variant was installed. The `variant` file is what the host reads;
    // falling back to the sole key keeps the check meaningful on a package
    // unpacked by hand without writing it.
    QString variant;
    QFile variantFile(moduleDir + "/variant");
    if (variantFile.open(QIODevice::ReadOnly)) {
        variant = QString::fromUtf8(variantFile.readAll()).trimmed();
    }
    const QJsonObject mains = manifest.value("main").toObject();
    if (variant.isEmpty() && mains.size() == 1) {
        variant = mains.keys().first();
    }

    check(!variant.isEmpty(), "the installed module names the variant it is");
    if (variant.isEmpty()) {
        return;
    }

    const QString main = mains.value(variant).toString();
    note(QStringLiteral("installed manifest: main[%1] = %2")
             .arg(variant, main.isEmpty() ? QStringLiteral("(unset)") : main));
    check(!main.isEmpty(),
          QStringLiteral("the manifest declares a `main` for %1").arg(variant));
    if (main.isEmpty()) {
        return;
    }

    // The whole point: the name in the manifest, against the directory as it
    // actually is. `agent_module_plugin` vs `agent_plugin` is one character
    // class apart and costs the entire module.
    const QFileInfo mainFile(moduleDir + "/" + main);
    check(mainFile.isFile(),
          QStringLiteral("`main` (%1) names a file that is really in the module "
                         "directory")
              .arg(main));

    // And that the metadata the host reads before opening the binary agrees
    // with it — modulo the extension, which the manifest carries and
    // metadata.json does not.
    QFile metaFile(moduleDir + "/metadata.json");
    if (metaFile.open(QIODevice::ReadOnly)) {
        const QJsonObject meta =
            QJsonDocument::fromJson(metaFile.readAll()).object();
        const QString declared = meta.value("main").toString();
        check(!declared.isEmpty()
                  && (declared == main
                      || main.startsWith(declared + QLatin1Char('.'))),
              QStringLiteral("metadata.json's `main` (%1) agrees with the "
                             "manifest's (%2)")
                  .arg(declared.isEmpty() ? QStringLiteral("(unset)") : declared,
                       main));
    }
}

/// A `LogosResult` that came back across the transport, reduced to a bool and
/// a reason. Returned as a QVariant carrying the struct; a QVariant that is
/// not one means the call never got there.
bool resultOk(const QVariant &value, QString *why)
{
    if (!value.canConvert<LogosResult>()) {
        *why = QStringLiteral("no LogosResult came back (got %1)")
                   .arg(QString::fromUtf8(value.typeName() ? value.typeName()
                                                           : "nothing"));
        return false;
    }
    const LogosResult result = value.value<LogosResult>();
    if (!result.success) {
        *why = result.error.toString();
    }
    return result.success;
}

/// The method names in whatever `getPluginMethods` came back as.
///
/// `getPluginMethods` is framework-level: the SDK answers it for every Logos
/// module before the call ever reaches the module's own dispatch
/// (`qt_provider_object.cpp` and `module_proxy.cpp` both special-case it), so
/// it can be asked of a module this harness knows nothing about. What comes
/// back across the transport is a `QJsonArray` of objects with a `name`, but
/// which QVariant shape it arrives in depends on the transport, so all three
/// are read rather than assumed — and an empty list is returned for a QVariant
/// that carries nothing at all, which is exactly what a module whose host
/// process is gone answers with.
QStringList pluginMethodNames(const QVariant &value)
{
    QJsonArray table;
    if (value.canConvert<QJsonArray>()) {
        table = value.value<QJsonArray>();
    } else if (value.typeId() == QMetaType::QVariantList) {
        table = QJsonArray::fromVariantList(value.toList());
    } else if (value.typeId() == QMetaType::QString
               || value.typeId() == QMetaType::QByteArray) {
        table = QJsonDocument::fromJson(value.toString().toUtf8()).array();
    }

    QStringList names;
    for (const QJsonValue &entry : table) {
        const QString name = entry.isObject()
                                 ? entry.toObject().value(QStringLiteral("name")).toString()
                                 : entry.toString();
        if (!name.isEmpty()) {
            names << name;
        }
    }
    return names;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <liblogos_core> <embedded-modules-dir> "
                "<user-modules-dir> <persistence-dir> <module-name> "
                "[owner-address] [policy-account-hex]\n",
                argv[0]);
        return 2;
    }

    const char *libPath = argv[1];
    const char *embeddedDir = argv[2];
    const char *userDir = argv[3];
    const char *persistence = argv[4];
    const char *moduleName = argv[5];

    // What this agent is configured to BE, optionally. Defaulted so the
    // invocation recorded in docs/basecamp.md keeps working unchanged; supplied
    // by `scripts/logos-core-headless.sh`, which reads the owner and the policy
    // account out of `artifacts/agents.tsv` and so configures the loaded module
    // with the envelope that is actually anchored on chain for that agent
    // rather than with a placeholder. The read-back below is what makes the
    // difference visible: `configure() was accepted` is a much weaker statement
    // than `the running module reports this owner and this policy account`.
    const char *owner =
        argc > 6 ? argv[6] : "0x00000000000000000000000000000000000000a9";
    static char defaultPolicy[65];
    memset(defaultPolicy, 'a', 64);
    defaultPolicy[64] = '\0';
    const char *policyHex = argc > 7 ? argv[7] : defaultPolicy;
    if (strlen(policyHex) != 64) {
        fprintf(stderr, "  FAIL  policy account must be 64 hex characters, got %zu\n",
                strlen(policyHex));
        return 2;
    }

    // The OTHER modules this runtime is to load, named in `$LOGOS_ALONGSIDE` as
    // a comma-separated list and installed into the same user modules directory
    // as the agent. Empty by default, so the invocation recorded in
    // docs/basecamp.md keeps meaning exactly what it meant.
    //
    // This exists for one sentence of the prize: "the agent module loads and
    // runs inside Logos Core ALONGSIDE the wallet, storage, and messaging
    // modules without requiring modifications to those modules". Passing them
    // by name rather than by path is what makes the "without modifications"
    // half checkable somewhere else — `scripts/logos-core-headless.sh` installs
    // them from packages built out of untouched upstream checkouts and proves
    // that with `git status --porcelain`, the same way
    // `examples/agent-console/run.sh` does for `module/`.
    QStringList companions;
    const bool companionsRequested = qEnvironmentVariableIsSet("LOGOS_ALONGSIDE");
    for (const QString &piece :
         qEnvironmentVariable("LOGOS_ALONGSIDE")
             .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = piece.trimmed();
        if (!name.isEmpty()) {
            companions << name;
        }
    }
    if (companionsRequested) {
        // An empty list would make every companion check below a loop over
        // nothing, and a loop over nothing passes. Asking for companions and
        // naming none is the one way this whole section could report `ok` while
        // proving that a single module loaded on its own.
        check(!companions.isEmpty(),
              QStringLiteral("LOGOS_ALONGSIDE names at least one module: %1")
                  .arg(companions.isEmpty() ? QStringLiteral("(none)")
                                            : companions.join(QStringLiteral(", "))));
    }

    // Before anything else, and before logos_core_init: the runtime's module
    // transport needs an existing QCoreApplication to attach its event loop to.
    static int hostArgc = 1;
    static char hostArg0[] = "logos_core_load_test";
    static char *hostArgv[] = {hostArg0, nullptr};
    QCoreApplication hostApp(hostArgc, hostArgv);

    void *lib = dlopen(libPath, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "  FAIL  dlopen(%s): %s\n", libPath, dlerror());
        return 1;
    }
    check(true, "the app's liblogos_core loads");

    auto core_init = must_sym<void (*)(int, char **)>(lib, "logos_core_init");
    auto core_add_modules_dir = must_sym<void (*)(const char *)>(lib, "logos_core_add_modules_dir");
    auto core_set_persistence = must_sym<void (*)(const char *)>(lib, "logos_core_set_persistence_base_path");
    auto core_set_policy = must_sym<void (*)(const char *)>(lib, "logos_core_set_access_policy");
    auto core_start = must_sym<void (*)()>(lib, "logos_core_start");
    auto core_known = must_sym<char **(*)()>(lib, "logos_core_get_known_modules");
    auto core_loaded = must_sym<char **(*)()>(lib, "logos_core_get_loaded_modules");
    auto core_load = must_sym<int (*)(const char *, bool)>(lib, "logos_core_load_module");
    check(true, "it exports the module-loading C API");

    char *fakeArgv[] = {(char *)"logos_core_load_test", NULL};
    core_init(1, fakeArgv);
    core_add_modules_dir(embeddedDir);
    core_add_modules_dir(userDir);
    core_set_persistence(persistence);
    core_set_policy(NULL);
    core_start();
    check(true, "the core starts with both module directories added");

    char **known = core_known();
    show("known modules", known);
    check(contains(known, moduleName),
          "the runtime discovers the module in the user modules directory");

    // ---- the companions, loaded FIRST -------------------------------------
    //
    // Before the agent, deliberately. The claim is that the agent works with
    // the wallet, storage and messaging modules present — not that four modules
    // each load into an empty runtime one at a time. Loading them first means
    // every one of the agent's own assertions further down runs in a process
    // that is already hosting them.
    for (const QString &name : companions) {
        const QByteArray n = name.toUtf8();
        check(contains(known, n.constData()),
              QStringLiteral("the runtime discovers the companion module '%1' in "
                             "the same user modules directory")
                  .arg(name));
        checkInstalledManifest(QString::fromUtf8(userDir) + "/" + name);
        check(core_load(n.constData(), true) == 1,
              QStringLiteral("logos_core_load_module('%1') reports success").arg(name));
    }

    // What the host is about to resolve, checked against what is on disk. This
    // runs before the load so a `main` that names nothing is reported as that,
    // rather than as a load which failed for no stated reason.
    checkInstalledManifest(QString::fromUtf8(userDir) + "/"
                           + QString::fromUtf8(moduleName));

    int loaded = core_load(moduleName, true);
    check(loaded == 1, "logos_core_load_module() reports success");

    char **running = core_loaded();
    show("loaded modules", running);
    check(contains(running, moduleName),
          "the module is in the runtime's loaded set");
    for (const QString &name : companions) {
        const QByteArray n = name.toUtf8();
        check(contains(running, n.constData()),
              QStringLiteral("and so is '%1' — one runtime, both modules").arg(name));
    }

    // ---- the loaded module, over the runtime's own transport ---------------
    //
    // A core module runs in its own `logos_host` process, so there is no
    // in-process pointer to call and nothing here can accidentally be talking
    // to a locally linked copy. `LogosAPI("core")` is what Basecamp's main.cpp
    // constructs; `getClient` fetches a capability token for the target from
    // `capability_module` on the first call.
    const QString target = QString::fromUtf8(moduleName);
    LogosAPI logosAPI(QStringLiteral("core"));
    LogosAPIClient *client = logosAPI.getClient(target);
    check(client != nullptr, "the SDK hands out a client for the loaded module");
    if (!client) {
        fprintf(stderr, "\nSOME CHECKS FAILED (%d failure(s))\n", failures);
        fflush(stderr);
        _exit(1);
    }

    // `configure` binds the agent to an owner and to the 32-byte policy anchor,
    // exactly once. Without it `start()` refuses, and without `start()` the
    // module answers `skills()` with `{"error":"agent is not started"}` rather
    // than a card — deliberately, so an unstarted agent is never mistaken for
    // an empty one.
    QString why;
    note(QStringLiteral("configure(owner=%1, policy=%2)")
             .arg(QString::fromUtf8(owner), QString::fromUtf8(policyHex)));
    const QVariant configured = client->invokeRemoteMethod(
        target, QStringLiteral("configure"),
        QVariant(QString::fromUtf8(owner)),
        QVariant(QString::fromUtf8(policyHex)));
    check(resultOk(configured, &why),
          QStringLiteral("configure() is accepted across the transport%1")
              .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));

    why.clear();
    const QVariant started = client->invokeRemoteMethod(
        target, QStringLiteral("start"), QVariantList());
    check(resultOk(started, &why),
          QStringLiteral("start() is accepted across the transport%1")
              .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));

    // ---- the card ---------------------------------------------------------
    const QVariant card = client->invokeRemoteMethod(
        target, QStringLiteral("skills"), QVariantList());
    const QJsonDocument cardDoc = QJsonDocument::fromJson(card.toString().toUtf8());
    check(cardDoc.isArray(),
          QStringLiteral("skills() answers with a JSON array, not an error "
                         "object: %1")
              .arg(card.toString().left(120)));

    const QJsonArray entries = cardDoc.array();
    note(QStringLiteral("skills(): %1 entries").arg(entries.size()));

    QStringList listed;
    QStringList unschemad;
    for (const QJsonValue &entry : entries) {
        const QJsonObject skill = entry.toObject();
        const QString name = skill.value("name").toString();
        listed << name;
        // A skill that answers with a name and nothing callable is a card entry
        // a caller cannot act on; `skills()` records that as an `error` key
        // rather than dropping the skill, so check for the schema itself.
        if (!skill.value("parameters").isObject()) {
            unschemad << name;
        }
    }

    QStringList missing;
    for (int i = 0; i < kSkillCount; ++i) {
        if (!listed.contains(QString::fromUtf8(kSkills[i]))) {
            missing << QString::fromUtf8(kSkills[i]);
        }
    }
    check(missing.isEmpty(),
          missing.isEmpty()
              ? QStringLiteral("the loaded module lists all %1 documented skills")
                    .arg(kSkillCount)
              : QStringLiteral("the loaded module is missing %1 skill(s): %2")
                    .arg(missing.size())
                    .arg(missing.join(QStringLiteral(", "))));
    check(entries.size() == kSkillCount,
          QStringLiteral("it lists exactly %1 — no more, no fewer (got %2)")
              .arg(kSkillCount)
              .arg(entries.size()));
    // `!entries.isEmpty()` is the load-bearing half. "Every listed skill
    // carries a schema" is also true of a card with nothing on it, and this
    // line used to say so out loud in its message — `(0 checked)` — while still
    // printing `ok`. It did exactly that in a run where `start()` had failed and
    // `skills()` answered `{"error":"agent is not started"}`. Naming the failure
    // mode in the message is not the same as failing on it.
    check(!entries.isEmpty() && unschemad.isEmpty(),
          entries.isEmpty()
              ? QStringLiteral("the card is empty, so there is no schema to check: this line "
                               "must not pass for that")
              : unschemad.isEmpty()
                    ? QStringLiteral("every listed skill carries a parameter schema "
                                     "(%1 checked)")
                          .arg(entries.size())
                    : QStringLiteral("%1 skill(s) list no parameter schema: %2")
                          .arg(unschemad.size())
                          .arg(unschemad.join(QStringLiteral(", "))));

    // ---- and that the card is not a list of names nothing answers to -------
    //
    // Called with `{}`: most of these refuse, because a module loaded as a
    // plugin has no way to wire a `std::function` port across the boundary.
    // That refusal is the skill's own and is the proof the call arrived. Only
    // the registry's "no skill named … is registered" means it did not.
    QStringList undispatched;
    QStringList unstarted;
    for (int i = 0; i < kSkillCount; ++i) {
        const QString name = QString::fromUtf8(kSkills[i]);
        const QString answer =
            client
                ->invokeRemoteMethod(target, QStringLiteral("invoke"),
                                     QVariant(name), QVariant(QStringLiteral("{}")))
                .toString();
        // Three ways this is not a dispatch, and only one of them used to
        // count. `agent is not started` is the one that mattered: it comes
        // from before the registry lookup, so it never contains
        // kUnregistered, and a module that failed to start passed this check
        // for all 22 names.
        if (answer.contains(QString::fromUtf8(kNotStarted))) unstarted << name;
        else if (answer.isEmpty() || answer.contains(QString::fromUtf8(kUnregistered)))
            undispatched << name;
    }
    check(unstarted.isEmpty(),
          unstarted.isEmpty()
              ? QStringLiteral("and it answered as a running agent, not as a stopped one")
              : QStringLiteral("%1 of %2 name(s) answered '%3': the module is not running, "
                               "and this check must not pass for that")
                    .arg(unstarted.size())
                    .arg(kSkillCount)
                    .arg(QString::fromUtf8(kNotStarted)));
    check(undispatched.isEmpty() && unstarted.isEmpty(),
          undispatched.isEmpty() && unstarted.isEmpty()
              ? QStringLiteral("invoke() dispatches to every one of the %1")
                    .arg(kSkillCount)
              : QStringLiteral("invoke() reached no skill for %1 name(s): %2")
                    .arg(undispatched.size() + unstarted.size())
                    .arg((undispatched + unstarted).join(QStringLiteral(", "))));

    // ---- Reliability, in the module the runtime loaded --------------------
    //
    // Two of the prize's three Reliability criteria are about what this module
    // does when something goes wrong, and both were previously demonstrable
    // only against classes the shipped plugin never constructed. These check
    // them here, across the transport, in the module Logos Core is running.

    // R1. "The agent module recovers from transient failures … without losing
    // pending task state." The whole mechanism hangs off the host handing over
    // a per-instance data directory, so the first thing worth asserting is that
    // it really does, and that the module opened its snapshot under it.
    //
    // What cannot be shown from here is a task surviving a restart: opening a
    // task needs a Delivery transport, and a plugin cannot be handed one (see
    // the note above about `std::function` ports). That half is proven in
    // module/tests/module_recovery_test.cpp against the same code. What is
    // proven here is the half that was actually missing — that the wiring is
    // live in the shipped artefact rather than in a class nothing constructs.
    const QString statusJson = client
                                   ->invokeRemoteMethod(target, QStringLiteral("invoke"),
                                                        QVariant(QStringLiteral("meta.status")),
                                                        QVariant(QStringLiteral("{}")))
                                   .toString();
    const QJsonObject status = QJsonDocument::fromJson(statusJson.toUtf8()).object();

    // Read back what this agent was configured to be, from the module the
    // runtime is running, over the runtime's own transport. `configure()`
    // returning ok says the call was accepted; this says the binding took, and
    // it is the difference between "Logos Core loaded a module" and "Logos Core
    // is running THIS agent, under the policy account anchored for it on chain".
    check(status.value(QStringLiteral("configured")).toBool(),
          "the running module reports itself configured");
    check(status.value(QStringLiteral("started")).toBool(),
          "the running module reports itself started");
    check(status.value(QStringLiteral("owner")).toString() == QString::fromUtf8(owner),
          QStringLiteral("and bound to the owner it was configured with (%1)")
              .arg(status.value(QStringLiteral("owner")).toString()));
    check(status.value(QStringLiteral("policy_hash")).toString()
              == QString::fromUtf8(policyHex),
          QStringLiteral("and to the policy account it was configured with (%1)")
              .arg(status.value(QStringLiteral("policy_hash")).toString()));

    const QJsonValue durability = status.value(QStringLiteral("durability"));
    note(QStringLiteral("meta.status durability: %1")
             .arg(durability.isObject()
                      ? QString::fromUtf8(QJsonDocument(durability.toObject()).toJson(
                            QJsonDocument::Compact))
                      : QStringLiteral("null")));
    check(durability.isObject(),
          "the loaded module reports a durability record, not null: it was given a "
          "persistence directory and opened a task snapshot in it");
    const QJsonObject durable = durability.toObject();
    const QString snapshotPath = durable.value(QStringLiteral("path")).toString();
    check(snapshotPath.startsWith(QString::fromUtf8(persistence)),
          QStringLiteral("and the snapshot lives under the persistence base the host set (%1)")
              .arg(snapshotPath.isEmpty() ? QStringLiteral("(none)") : snapshotPath));
    const QString recovery = durable.value(QStringLiteral("recovery")).toString();
    check(durable.value(QStringLiteral("recovery_ran")).toBool()
              && (recovery == QLatin1String("absent") || recovery == QLatin1String("loaded")),
          QStringLiteral("recovery ran before the agent started serving, and reported '%1'")
              .arg(recovery.isEmpty() ? QStringLiteral("(nothing)") : recovery));

    // R2. "Above-threshold transactions that fail to reach the owner for
    // approval are not executed — the agent retries notification before timing
    // out and reports the failure."
    //
    // No port is wired here, so the module cannot know its anchored envelope,
    // and an unknown envelope holds *every* spend for the owner — which is what
    // makes this reachable across the transport at all. The owner channel a
    // loaded plugin has is the module's own: `ownerApprovalRequested` out,
    // `approveSpend` back. Nothing is listening in this harness, so the request
    // is published, retried, and never answered — the exact failure the
    // criterion is about.
    //
    // The wait is shortened first, through the module's own `meta.configure`,
    // because the default is two minutes and the transport gives up at twenty
    // seconds.
    for (const auto &setting : {std::make_pair("approval_timeout_ms", "1500"),
                                std::make_pair("approval_resend_ms", "200")}) {
        const QString reply =
            client
                ->invokeRemoteMethod(
                    target, QStringLiteral("invoke"), QVariant(QStringLiteral("meta.configure")),
                    QVariant(QStringLiteral("{\"key\":\"%1\",\"value\":\"%2\"}")
                                 .arg(QString::fromUtf8(setting.first),
                                      QString::fromUtf8(setting.second))))
                .toString();
        const QJsonObject set = QJsonDocument::fromJson(reply.toUtf8()).object();
        check(set.value(QStringLiteral("ok")).toBool()
                  && set.value(QStringLiteral("effective")).toBool(),
              QStringLiteral("%1 is settable on the running module, and effective: %2")
                  .arg(QString::fromUtf8(setting.first), reply.left(120)));
    }

    const QString sendJson =
        client
            ->invokeRemoteMethod(
                target, QStringLiteral("invoke"), QVariant(QStringLiteral("wallet.send")),
                QVariant(QStringLiteral(
                    "{\"recipient\":\"9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb72Ntu8bT\","
                    "\"amount\":\"100\"}")))
            .toString();
    const QJsonObject send = QJsonDocument::fromJson(sendJson.toUtf8()).object();
    note(QStringLiteral("wallet.send above threshold: %1").arg(sendJson.left(300)));

    // Did a *running* agent answer this, about *this* spend?
    //
    // Asked first, and separately, because the check under it used to be
    // `send.value("submitted").toBool() == false` on its own — and that passes
    // for `{"ok":false,"error":"agent is not started"}`, which has no
    // `submitted` key: an absent key reads back as `false`, and `false == false`
    // is a pass. So "an above-threshold spend is not submitted" printed `ok`
    // against a module that was not serving at all, which is the worst place in
    // this harness for that shape to be — it is half of the criterion this
    // repository claims to meet.
    //
    // Presence of the key is the minimum, and the echoed terms are what make it
    // an answer about the payment that was asked about rather than about some
    // other refusal that happens to carry the field.
    const bool spendWasAnswered =
        send.contains(QStringLiteral("submitted"))
        && send.value(QStringLiteral("recipient")).toString().endsWith(
               QLatin1String("9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb72Ntu8bT"))
        && send.value(QStringLiteral("amount")).toString() == QLatin1String("100");
    check(spendWasAnswered,
          spendWasAnswered
              ? QStringLiteral("a running agent answered the spend, naming the recipient and "
                               "amount it was asked about")
              : QStringLiteral("wallet.send was not answered by a running agent (%1): every "
                               "check below is about what a running one does")
                    .arg(sendJson.left(120)));
    check(spendWasAnswered && send.value(QStringLiteral("submitted")).toBool() == false,
          "an above-threshold spend nobody approved is not submitted by the loaded module");
    check(send.value(QStringLiteral("outcome")).toString() == QLatin1String("owner_unreachable"),
          "and the outcome is the terminal owner-unreachable one, not a fallback to acting alone");
    const int attempts = send.value(QStringLiteral("attempts")).toInt();
    check(attempts > 1,
          QStringLiteral("the notification was retried before the timeout: %1 attempts")
              .arg(attempts));
    check(!send.value(QStringLiteral("request_id")).toString().isEmpty(),
          "and the failure is reported against the correlation id the owner was asked under");

    // The answer path exists and is reachable over the same transport. Asked
    // about a request nobody made, so this cannot accidentally authorise
    // anything: a *refusal* here is the pass, and it proves `approveSpend` is
    // dispatched rather than swallowed.
    //
    // "A refusal is the pass" is the third hat of the shape above, and it needs
    // two conditions rather than one. `!resultOk(...)` alone is true when the
    // call never arrived — `resultOk` reports "no LogosResult came back" through
    // the same `false` it reports a refusal with — so a dead transport, an
    // unloaded module or a method that does not exist would all have read as
    // "reachable, and refuses". So: a `LogosResult` must actually have come
    // back, it must be a failure, and it must be *this* failure.
    //
    // These two lines stay green in the corrupted-snapshot run below, and that
    // is correct rather than another instance of the shape: `approveSpend` is
    // not gated on `start()`, so a module that is loaded but not started really
    // does answer, and really does refuse. Do not "fix" it into a failure — the
    // check is about the answer path being reachable, and it was.
    QString approveWhy;
    const QVariant approved = client->invokeRemoteMethod(
        target, QStringLiteral("approveSpend"), QVariant(QStringLiteral("spend-nobody-asked")),
        QVariant(QStringLiteral("approved")));
    const bool approveAnswered = approved.canConvert<LogosResult>();
    const bool approveRefused = approveAnswered && !resultOk(approved, &approveWhy);
    check(approveAnswered,
          approveAnswered
              ? QStringLiteral("approveSpend answered across the transport")
              : QStringLiteral("approveSpend never answered (%1): a call that did not arrive "
                               "is not a refusal")
                    .arg(approveWhy));
    check(approveRefused && approveWhy.contains(QLatin1String("no spend is waiting")),
          QStringLiteral("approveSpend is reachable, and refuses a request nobody is waiting "
                         "on: %1")
              .arg(approveWhy));

    // The control. Without it, "dispatched" would only mean "answered
    // something", and a module that answered everything the same way would
    // pass the check above.
    const QString stranger =
        client
            ->invokeRemoteMethod(target, QStringLiteral("invoke"),
                                 QVariant(QStringLiteral("wallet.definitely_not")),
                                 QVariant(QStringLiteral("{}")))
            .toString();
    check(stranger.contains(QString::fromUtf8(kUnregistered)),
          QStringLiteral("a name nobody registered is refused as unregistered: %1")
              .arg(stranger.left(120)));

    // ---- and the companions are RUNNING, not merely loaded ----------------
    //
    // This is the check the whole companion section exists for, and the failure
    // it exists to catch does not look like a failure. A plugin built against a
    // Qt whose minor version exceeds the host's makes `logos_core_load_module`
    // return success and join the loaded set; its `logos_host` is then gone, and
    // every call to it spends twenty seconds on `Timeout waiting for replica`
    // before handing back an empty QVariant. Every companion check above passes
    // for that — docs/basecamp.md records the run where it did, for this very
    // module. "Loaded" is not the claim; "runs alongside" is.
    //
    // `getPluginMethods` is asked because it needs no knowledge of what the
    // module does, and because only a live module process can answer it. The
    // answer is required to be a NON-EMPTY method table: `pluginMethodNames`
    // returns an empty list for a QVariant carrying nothing, so "the call did
    // not fail" cannot stand in for "the module answered".
    for (const QString &name : companions) {
        LogosAPIClient *companion = logosAPI.getClient(name);
        check(companion != nullptr,
              QStringLiteral("the SDK hands out a client for '%1'").arg(name));
        if (!companion) {
            continue;
        }
        const QStringList methods = pluginMethodNames(companion->invokeRemoteMethod(
            name, QStringLiteral("getPluginMethods"), QVariantList()));
        note(QStringLiteral("%1 getPluginMethods(): %2 method(s)%3")
                 .arg(name)
                 .arg(methods.size())
                 .arg(methods.isEmpty()
                          ? QString()
                          : QStringLiteral(" — ")
                                + methods.mid(0, 6).join(QStringLiteral(", "))
                                + (methods.size() > 6 ? QStringLiteral(", …")
                                                      : QString())));
        check(!methods.isEmpty(),
              QStringLiteral("'%1' answers across the runtime's transport with its own "
                             "method table: it is running, not just loaded")
                  .arg(name));
    }

    // Everything above happened while the agent was being driven. This is the
    // state of the runtime AFTER all of it: the whole set, still up, in one
    // process.
    char **together = core_loaded();
    show("loaded modules, after the agent's skills were exercised", together);
    check(contains(together, moduleName),
          "the agent module is still loaded after every skill was invoked");
    for (const QString &name : companions) {
        const QByteArray n = name.toUtf8();
        check(contains(together, n.constData()),
              QStringLiteral("and '%1' is still loaded beside it").arg(name));
    }

    // And the agent still answers with all of them up — asked last, so that a
    // companion that took the runtime down with it fails here rather than
    // leaving the earlier `meta.status` as the last word.
    const QJsonObject statusWithCompanions =
        QJsonDocument::fromJson(client
                                    ->invokeRemoteMethod(target, QStringLiteral("invoke"),
                                                         QVariant(QStringLiteral("meta.status")),
                                                         QVariant(QStringLiteral("{}")))
                                    .toString()
                                    .toUtf8())
            .object();
    check(statusWithCompanions.value(QStringLiteral("started")).toBool()
              && statusWithCompanions.value(QStringLiteral("owner")).toString()
                     == QString::fromUtf8(owner),
          QStringLiteral("and the agent still reports itself started and bound to %1 with "
                         "%2 other module(s) loaded in the same runtime")
              .arg(QString::fromUtf8(owner))
              .arg(companions.size()));

    // The control for the whole section. Without it, every `contains(...)` line
    // above would also be green in a runtime that reported every name as known
    // and every load as successful.
    static const char *const kGhost = "no_such_module_is_installed";
    check(!contains(known, kGhost),
          "a module nobody installed is not in the runtime's known set");
    check(core_load(kGhost, true) != 1,
          "and logos_core_load_module() refuses to load it");

    fprintf(stderr, "\n%s (%d failure(s))\n",
            failures ? "SOME CHECKS FAILED" : "all steps confirmed", failures);
    /* Deliberately no logos_core_cleanup(): the runtime tears down Qt objects
     * that outlive this harness's stack, and a crash on the way out would be
     * indistinguishable from a failed load in the exit code. The assertions
     * above have already run. */
    fflush(stderr);
    _exit(failures ? 1 : 0);
}
