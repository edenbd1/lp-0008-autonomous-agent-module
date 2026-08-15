/* SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Does Logos Core itself load the agent module?
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
 * `liblogos_core` is dlopen'd from the installed app rather than linked, so
 * what runs is the shipped binary, not a rebuild of it. The one thing this
 * harness must bring itself is a QCoreApplication: the runtime assumes the
 * host application already created one before `logos_core_init` (Logos
 * Basecamp constructs its QApplication first), and without it the module
 * transport's event loop cannot start and the load hangs. So the harness
 * links the app's *own* bundled QtCore — one QtCore in the process, the same
 * one the runtime resolves through @rpath.
 *
 * Usage:
 *   logos_core_load_test <liblogos_core> <embedded-modules-dir>
 *                        <user-modules-dir> <persistence-dir> <module-name>
 *
 * Every step is an assertion and the exit code is the result.
 */

#include <QCoreApplication>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char *what)
{
    fprintf(stderr, "  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
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

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <liblogos_core> <embedded-modules-dir> "
                "<user-modules-dir> <persistence-dir> <module-name>\n",
                argv[0]);
        return 2;
    }

    const char *libPath = argv[1];
    const char *embeddedDir = argv[2];
    const char *userDir = argv[3];
    const char *persistence = argv[4];
    const char *moduleName = argv[5];

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
    check(1, "the app's liblogos_core loads");

    auto core_init = must_sym<void (*)(int, char **)>(lib, "logos_core_init");
    auto core_add_modules_dir = must_sym<void (*)(const char *)>(lib, "logos_core_add_modules_dir");
    auto core_set_persistence = must_sym<void (*)(const char *)>(lib, "logos_core_set_persistence_base_path");
    auto core_set_policy = must_sym<void (*)(const char *)>(lib, "logos_core_set_access_policy");
    auto core_start = must_sym<void (*)()>(lib, "logos_core_start");
    auto core_known = must_sym<char **(*)()>(lib, "logos_core_get_known_modules");
    auto core_loaded = must_sym<char **(*)()>(lib, "logos_core_get_loaded_modules");
    auto core_load = must_sym<int (*)(const char *, bool)>(lib, "logos_core_load_module");
    check(1, "it exports the module-loading C API");

    char *fakeArgv[] = {(char *)"logos_core_load_test", NULL};
    core_init(1, fakeArgv);
    core_add_modules_dir(embeddedDir);
    core_add_modules_dir(userDir);
    core_set_persistence(persistence);
    core_set_policy(NULL);
    core_start();
    check(1, "the core starts with both module directories added");

    char **known = core_known();
    show("known modules", known);
    check(contains(known, moduleName),
          "the runtime discovers the module in the user modules directory");

    int loaded = core_load(moduleName, true);
    check(loaded == 1, "logos_core_load_module() reports success");

    char **running = core_loaded();
    show("loaded modules", running);
    check(contains(running, moduleName),
          "the module is in the runtime's loaded set");

    fprintf(stderr, "\n%s (%d failure(s))\n",
            failures ? "SOME CHECKS FAILED" : "all steps confirmed", failures);
    /* Deliberately no logos_core_cleanup(): the runtime tears down Qt objects
     * that outlive this harness's stack, and a crash on the way out would be
     * indistinguishable from a failed load in the exit code. The assertions
     * above have already run. */
    fflush(stderr);
    _exit(failures ? 1 : 0);
}
