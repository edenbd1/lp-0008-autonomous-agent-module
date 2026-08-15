// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Does the owner console actually load the way Basecamp loads a `ui` plugin?
//
// Basecamp's PluginLoader does three things with a `ui` package and logs a
// single line when any of them fails — on stderr, from a terminal launch, which
// a reviewer clicking a tile never sees. The tile is simply inert. So this
// drives the same three, in order, and asserts on each:
//
//   1. The binary loads with every symbol bound. A `ui` plugin deliberately
//      leaves `LogosAPI` and `LogosAPIClient` undefined so they resolve to the
//      `liblogos_core` already in the host's process — the same thing
//      Basecamp's own `main_ui.dylib` does, and the reason this plugin can
//      reach a core module at all. That is right, and it is also one typo away
//      from a plugin that loads nowhere: bind it with RTLD_NOW *without* the
//      runtime and it must fail; bind it *with* the runtime and it must
//      succeed. Both directions are asserted, because only the pair
//      distinguishes "the host provides these" from "there was nothing to
//      resolve".
//   2. QPluginLoader accepts it and reports the IID the host compares against:
//      `com.logos.component.IComponent`. A private IID makes
//      `qobject_cast<IComponent*>` return null and Basecamp logs "Plugin does
//      not implement IComponent".
//   3. The embedded metadata is this plugin's own metadata.json, its `type` is
//      `ui` (a `core` type is installed into the modules directory, where
//      nothing gives it a window), its `main` names the file that was built,
//      and it declares `agent` as a dependency — which is what makes Basecamp
//      load the core module before handing this plugin a LogosAPI that can
//      reach it.
//   4. `qobject_cast<IComponent*>` succeeds across the plugin boundary and the
//      vtable is the one the host expects, checked by calling through it:
//      `createWidget(nullptr)` must return a widget and `destroyWidget` must
//      take it back. A null LogosAPI is the point — the console has to come up
//      and *say* it has no runtime rather than crash the host, because that is
//      what it will be handed if anything above it went wrong.
//
// Not asserted here: that Basecamp draws it. Nothing headless can assert that.
// What names the plugin inside the running app is in docs/basecamp.md.
//
// Build and run: see app/README.md. Every step is an assertion and the exit
// code is the result.

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QPluginLoader>
#include <QString>
#include <QWidget>

#include <cstdio>
#include <dlfcn.h>

// Basecamp's IComponent, declared exactly as app/src/plugin.h declares it. Not
// included from there on purpose: this harness is the thing that would catch
// that declaration drifting, and a harness that shares the declaration under
// test cannot. At namespace scope, because Q_DECLARE_INTERFACE expands to an
// explicit specialisation of qobject_cast and those are only legal there.
class IComponent
{
public:
    virtual ~IComponent() = default;
    virtual QWidget *createWidget(void *api) = 0;
    virtual void destroyWidget(QWidget *widget) = 0;
};
#define IComponent_IID "com.logos.component.IComponent"
Q_DECLARE_INTERFACE(IComponent, IComponent_IID)

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

} // namespace

int main(int argc, char **argv)
{
    // QApplication, not QCoreApplication: step 4 constructs a QWidget, and a
    // QWidget without a QApplication aborts. Run it with
    // QT_QPA_PLATFORM=offscreen where there is no display — see app/README.md.
    QApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <agent_ui.dylib> [liblogos_core]\n"
                     "  the second argument is Basecamp's own liblogos_core; without it\n"
                     "  the harness asserts only that the plugin *needs* one\n",
                     argv[0]);
        return 2;
    }
    const QString pluginPath = QString::fromUtf8(argv[1]);
    const QString corePath = argc > 2 ? QString::fromUtf8(argv[2]) : QString();

    // ---- 1. the symbols the host is expected to provide --------------------
    //
    // Negative control first, and it has to run before the runtime is loaded
    // into this process: once liblogos_core is in, it stays in, and the same
    // dlopen would then succeed for the wrong reason.
    {
        void *bare = dlopen(pluginPath.toUtf8().constData(), RTLD_NOW | RTLD_LOCAL);
        check(bare == nullptr,
              bare == nullptr
                  ? QStringLiteral("without the runtime, binding every symbol fails — the "
                                   "plugin really does depend on the host: %1")
                        .arg(QString::fromUtf8(dlerror()).left(90))
                  : QStringLiteral("the plugin bound with no runtime present, so the check "
                                   "below proves nothing"));
        if (bare) {
            dlclose(bare);
        }
    }

    if (!corePath.isEmpty()) {
        void *core = dlopen(corePath.toUtf8().constData(), RTLD_NOW | RTLD_GLOBAL);
        check(core != nullptr,
              core ? QStringLiteral("Basecamp's liblogos_core loads")
                   : QStringLiteral("could not load %1: %2")
                         .arg(corePath, QString::fromUtf8(dlerror())));
        if (!core) {
            std::fprintf(stderr, "\nSOME CHECKS FAILED (%d failure(s))\n", failures);
            return 1;
        }
        void *bound = dlopen(pluginPath.toUtf8().constData(), RTLD_NOW | RTLD_LOCAL);
        check(bound != nullptr,
              bound ? QStringLiteral("with it, every symbol binds — the undefined LogosAPI "
                                     "symbols resolve to the host's copies")
                    : QStringLiteral("the plugin will not bind against liblogos_core: %1")
                          .arg(QString::fromUtf8(dlerror())));
    }

    // ---- 2. the Qt plugin contract ----------------------------------------
    QPluginLoader loader(pluginPath);
    const QJsonObject metaData = loader.metaData();
    const QString iid = metaData.value(QStringLiteral("IID")).toString();
    note(QStringLiteral("declared IID: %1").arg(iid.isEmpty() ? QStringLiteral("(none)") : iid));
    check(iid == QLatin1String(IComponent_IID),
          QStringLiteral("the interface IID is the one Basecamp casts against"));

    // ---- 3. the manifest the host reads before it opens the binary ---------
    const QJsonObject manifest = metaData.value(QStringLiteral("MetaData")).toObject();
    const QString name = manifest.value(QStringLiteral("name")).toString();
    const QString type = manifest.value(QStringLiteral("type")).toString();
    const QString main = manifest.value(QStringLiteral("main")).toString();
    note(QStringLiteral("name: %1, main: %2, type: %3").arg(name, main, type));
    check(type == QLatin1String("ui"),
          QStringLiteral("type is `ui`, so Basecamp installs it into its plugins directory "
                         "rather than its modules directory (got '%1')")
              .arg(type));

    // `main` carries no extension; the built file does. Compared the way the
    // host resolves it, because a `main` naming a file the package does not
    // hold fails the load before Qt is reached and logs nothing.
    const QString built = pluginPath.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    check(!main.isEmpty() && main == built,
          QStringLiteral("`main` (%1) names the file that was built (%2)").arg(main, built));

    QStringList dependencies;
    for (const QJsonValue &dependency : manifest.value(QStringLiteral("dependencies")).toArray()) {
        dependencies << dependency.toString();
    }
    note(QStringLiteral("dependencies: %1")
             .arg(dependencies.isEmpty() ? QStringLiteral("(none)")
                                         : dependencies.join(QStringLiteral(", "))));
    // This is not decoration. Basecamp's PluginLoader logs "Loading core
    // dependency for … : agent" and calls logos_core_load_module for each name
    // here, then has capability_module mint this plugin a token for it. Drop
    // the dependency and the window opens onto a module nobody loaded.
    check(dependencies.contains(QStringLiteral("agent")),
          QStringLiteral("it declares the `agent` core module as a dependency, which is what "
                         "makes Basecamp load it and mint a token for it"));

    // ---- 4. the cast, and the vtable behind it -----------------------------
    QObject *instance = loader.instance();
    check(instance != nullptr,
          instance ? QStringLiteral("QPluginLoader instantiates it")
                   : QStringLiteral("QPluginLoader refused it: %1").arg(loader.errorString()));
    if (!instance) {
        std::fprintf(stderr, "\nSOME CHECKS FAILED (%d failure(s))\n", failures);
        return 1;
    }

    IComponent *component = qobject_cast<IComponent *>(instance);
    check(component != nullptr,
          component ? QStringLiteral("it casts to IComponent across the plugin boundary")
                    : QStringLiteral("qobject_cast to IComponent returned null — this is what "
                                     "Basecamp reports as 'Plugin does not implement "
                                     "IComponent'"));
    if (!component) {
        std::fprintf(stderr, "\nSOME CHECKS FAILED (%d failure(s))\n", failures);
        return 1;
    }

    // Through the vtable, with no LogosAPI. A widget must still come back: the
    // host hands this plugin whatever it has, and a console that dereferences a
    // null runtime takes the whole application down instead of saying what is
    // missing.
    QWidget *widget = component->createWidget(nullptr);
    check(widget != nullptr,
          widget ? QStringLiteral("createWidget(nullptr) returns a widget rather than "
                                  "crashing the host")
                 : QStringLiteral("createWidget(nullptr) returned nothing"));
    if (widget) {
        note(QStringLiteral("widget class: %1, objectName: %2")
                 .arg(QString::fromUtf8(widget->metaObject()->className()),
                      widget->objectName()));
        check(widget->objectName() == QLatin1String("lp0008AgentConsole"),
              QStringLiteral("and it is this plugin's console, not some other widget"));
        component->destroyWidget(widget);
        check(true, "destroyWidget accepts it back");
    }

    if (failures == 0) {
        std::fprintf(stderr, "all steps confirmed (0 failure(s))\n");
        return 0;
    }
    std::fprintf(stderr, "\nSOME CHECKS FAILED (%d failure(s))\n", failures);
    return 1;
}
