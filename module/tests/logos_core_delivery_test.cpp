// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The same question as `plugin_delivery_test.cpp`, asked of the real runtime.
//
//   logos_core_delivery_test <liblogos_core> <embedded-modules-dir> \
//                            <user-modules-dir> <persistence-dir> <module-name>
//
// WHY BOTH HARNESSES EXIST
//
// `plugin_delivery_test` opens the plugin with `QPluginLoader`, in this
// process. That answers the Qt half of the question and not the Logos half: a
// `core` module does not run in the host's process at all. Logos Core spawns
// `logos_host` for it and reaches it over Qt Remote Objects, so the Delivery
// node this module opens for itself is opened *inside a process this harness
// never touches*, by a plugin `dlopen`ed by a binary this harness only starts.
// Everything below therefore goes through `LogosAPI` / `LogosAPIClient` — the
// facade Basecamp's own `main.cpp` constructs — and the only thing that crosses
// is `invoke("meta.configure", {"key":"delivery","value":"on"})`, which is two
// strings.
//
// This is also the harness that can be wrong in the most expensive way, and the
// way is known: `logos_core_load_module` returns success for a plugin built
// against the wrong Qt, and every later call then spends twenty seconds on
// `Timeout waiting for replica` before handing back an empty QVariant. So no
// check here is satisfied by an empty answer — each asserts on a field of a
// document that came back.

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "logos_api.h"
#include "logos_api_client.h"
#include "logos_types.h"

static int failures = 0;

static void check(bool condition, const QString &what)
{
    fprintf(stderr, "  %s  %s\n", condition ? "ok  " : "FAIL", qPrintable(what));
    fflush(stderr);
    if (!condition) ++failures;
}

static void note(const QString &what)
{
    fprintf(stderr, "  <-    %s\n", qPrintable(what));
    fflush(stderr);
}

static void step(const QString &what)
{
    fprintf(stderr, "\n%s\n", qPrintable(what));
    fflush(stderr);
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

static bool resultOk(const QVariant &v)
{
    return v.canConvert<LogosResult>() && v.value<LogosResult>().success;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <liblogos_core> <embedded-modules-dir> <user-modules-dir> "
                "<persistence-dir> <module-name>\n",
                argv[0]);
        return 2;
    }
    const char *libPath = argv[1];
    const char *embeddedDir = argv[2];
    const char *userDir = argv[3];
    const char *persistence = argv[4];
    const char *moduleName = argv[5];

    static int hostArgc = 1;
    static char hostArg0[] = "logos_core_delivery_test";
    static char *hostArgv[] = {hostArg0, nullptr};
    QCoreApplication hostApp(hostArgc, hostArgv);

    void *lib = dlopen(libPath, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "  FAIL  dlopen(%s): %s\n", libPath, dlerror());
        return 1;
    }
    auto core_init = must_sym<void (*)(int, char **)>(lib, "logos_core_init");
    auto core_add_modules_dir = must_sym<void (*)(const char *)>(lib, "logos_core_add_modules_dir");
    auto core_set_persistence =
        must_sym<void (*)(const char *)>(lib, "logos_core_set_persistence_base_path");
    auto core_set_policy = must_sym<void (*)(const char *)>(lib, "logos_core_set_access_policy");
    auto core_start = must_sym<void (*)()>(lib, "logos_core_start");
    auto core_load = must_sym<int (*)(const char *, bool)>(lib, "logos_core_load_module");

    char *fakeArgv[] = {(char *)"logos_core_delivery_test", nullptr};
    core_init(1, fakeArgv);
    core_add_modules_dir(embeddedDir);
    core_add_modules_dir(userDir);
    core_set_persistence(persistence);
    core_set_policy(nullptr);
    core_start();
    check(core_load(moduleName, true) == 1, "logos_core_load_module() reports success");

    const QString target = QString::fromUtf8(moduleName);
    LogosAPI logosAPI(QStringLiteral("core"));
    LogosAPIClient *client = logosAPI.getClient(target);
    check(client != nullptr, "the SDK hands out a client for the loaded module");
    if (!client) {
        fprintf(stderr, "\nFAILED (%d failure(s))\n", failures);
        _exit(1);
    }

    check(resultOk(client->invokeRemoteMethod(
              target, QStringLiteral("configure"),
              QVariant(QStringLiteral("0x00000000000000000000000000000000000000a9")),
              QVariant(QString(64, QLatin1Char('a'))))),
          "configure() is accepted across the transport");
    check(resultOk(client->invokeRemoteMethod(target, QStringLiteral("start"), QVariantList())),
          "start() is accepted across the transport");

    const auto invoke = [&](const char *skill, const QString &params) {
        const QVariant v = client->invokeRemoteMethod(
            target, QStringLiteral("invoke"), QVariant(QString::fromUtf8(skill)),
            QVariant(params));
        return QJsonDocument::fromJson(v.toString().toUtf8()).object();
    };
    const auto deliveryStatus = [&] {
        return invoke("meta.status", QStringLiteral("{}")).value("delivery").toObject();
    };

    step("1. the transport, as the module in logos_host reports it");
    QJsonObject d = deliveryStatus();
    note("meta.status delivery: " +
         QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
    // An empty object here is the twenty-second-timeout failure, not a passing
    // "off": `state` is always present in a real answer.
    check(!d.isEmpty(), "meta.status came back with a delivery record, not an empty QVariant");
    check(d.value("linked").toBool(),
          "the module the RUNTIME loaded can open Logos Delivery");
    check(d.value("state").toString() == QLatin1String("off"),
          "and has started no node, because nobody has asked it to");

    step("2. the refusal, before");
    QJsonObject r = invoke("messaging.send",
                           QStringLiteral(R"({"recipient":"probe","message":"hi"})"));
    note("messaging.send: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("error").toString() == QLatin1String("delivery node is not started"),
          "the wire skills refuse over the transport, exactly as documented");

    step("3. two strings, and a Delivery node opened in a process this harness never entered");
    r = invoke("meta.configure", QStringLiteral(R"({"key":"delivery","value":"on"})"));
    note("meta.configure: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "meta.configure('delivery','on') crosses Qt Remote Objects");

    bool ready = false;
    for (int i = 0; i < 240 && !ready; ++i) {
        const QJsonObject s = deliveryStatus();
        if (s.value("state").toString() == QLatin1String("ready")) ready = true;
        else if (s.value("state").toString() == QLatin1String("failed")) {
            note("delivery failed: " + s.value("error").toString());
            break;
        } else {
            sleep(1);
        }
    }
    check(ready, "the module opened and started its own Delivery node inside logos_host");
    if (!ready) {
        fprintf(stderr, "\nFAILED (%d failure(s))\n", failures);
        _exit(1);
    }

    step("4. the skills that had no ports, on the module the runtime loaded");
    r = invoke("messaging.join", QStringLiteral(R"({"group_id":"lp0008corelive"})"));
    note("messaging.join: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "messaging.join subscribed on the public network");

    r = invoke("messaging.send",
               QStringLiteral(R"({"recipient":"lp0008corelive","message":"lp-0008"})"));
    note("messaging.send: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "messaging.send put a message on the public network");

    r = invoke("agent.discover", QStringLiteral(R"({"topic":"/lp-0008/1/owner-lp0008corelive/json"})"));
    note("agent.discover: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)).left(200));
    check(r.value("ok").toBool(),
          "agent.discover answers with a result, not 'no discovery transport is configured'");

    // `agent.task` at price zero: no wallet, no settlement, no funding spent —
    // and the A2A request really does leave the node. A priced task is refused
    // for a different reason, and that reason is the honest one: this module
    // wires no `pay`.
    r = invoke("agent.task",
               QStringLiteral(R"({"agent_address":"lp0008corelive","skill":"storage.upload",)"
                              R"("task_id":"corelivetask","params":{"path":"/tmp/x"}})"));
    note("agent.task: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(),
          "agent.task opened a task and put the A2A request on the wire");
    check(r.value("state").toString() == QLatin1String("submitted"),
          "and the task is in the A2A `submitted` state");

    r = invoke("agent.subscribe",
               QStringLiteral(R"({"agent_address":"lp0008corelive","task_id":"corelivetask"})"));
    note("agent.subscribe: " +
         QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    check(r.value("ok").toBool(), "agent.subscribe subscribed to that task's topic");

    step("5. and it can be put back");
    r = invoke("meta.configure", QStringLiteral(R"({"key":"delivery","value":"off"})"));
    check(r.value("ok").toBool(), "meta.configure('delivery','off')");
    check(deliveryStatus().value("state").toString() == QLatin1String("off"),
          "the node is down and the module says so");

    fprintf(stderr, "\n%s (%d failure(s))\n",
            failures ? "FAILED"
                     : "the module Logos Core loaded obtained a working Delivery port",
            failures);
    fflush(stderr);
    // `_exit`, like the harness beside it: the runtime's own threads are still
    // running and a clean return would run their destructors from here.
    _exit(failures ? 1 : 0);
}
