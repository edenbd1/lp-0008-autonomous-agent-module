# Reaching the agent from the Logos app

The prize asks for an owner-facing interface "accessible from the Logos app
(Basecamp) via the owner channel — local build instructions and loadable assets
are provided". This is the build instructions and the loadable asset, and the
record of what was actually checked rather than assumed.

## What was verified, and how

Shipping a package is not the same as it loading. Basecamp gives no visible
error when a module fails: nothing appears, and the reason only reaches stderr.
So there are two harnesses in `module/tests/`, both of which are assertions with
the exit code as the result, and both of which are run against the artefact this
repository ships — not against a rebuild made for the occasion.

**1. The plugin satisfies the Qt side of the contract**
(`module/tests/plugin_load_test.cpp`). `QPluginLoader` accepts the binary, the
interface IID is `org.logos.LogosProviderPlugin`, the embedded manifest is this
module's own `metadata.json`, `main` names the file that was actually built, and
`qobject_cast` to both `PluginInterface` and `LogosProviderPlugin` succeeds
across the plugin boundary. Then it calls the module through the published
method table and asserts on the module's real behaviour:

```
  ok    the interface IID is the one Logos Core casts against
  <-    name: agent, main: agent_plugin, type: core
  ok    `main` (agent_plugin) names the file that was built (agent_plugin)
  ok    it casts to PluginInterface across the boundary
  ok    it casts to LogosProviderPlugin across the boundary
  <-    getMethods(): configure, skills, start, stop, status, invoke
  ok    configure refuses a malformed policy hash
  ok    a second configure is refused — the binding is the agent's identity
  ok    status reflects the running agent
  ok    invoking an unregistered skill fails rather than crashing the module
all steps confirmed (0 failure(s))
```

**2. Logos Core itself loads it** (`module/tests/logos_core_load_test.cpp`).
This one is not a reproduction of the host: it `dlopen`s the real
`liblogos_core.dylib` out of the installed `LogosBasecamp.app` and drives it
through the same C API, in the same order, as Basecamp's own `app/main.cpp` —
`logos_core_init`, both module directories added, persistence path, access
policy, `logos_core_start`, then `logos_core_load_module("agent", true)`. That
last call is exactly what runs when a user enables a module; Basecamp itself
auto-loads only `package_manager` and `package_downloader`.

```
  <-    known modules: package_manager package_downloader capability_module agent
  ok    the runtime discovers the module in the user modules directory
  ok    logos_core_load_module() reports success
  <-    loaded modules: capability_module agent
  ok    the module is in the runtime's loaded set
all steps confirmed (0 failure(s))
```

The runtime's own log during that run prints `Module loaded: agent` — the same
line it prints for `capability_module` — and the capability module issues a
token for `agent`, so the module was published over the transport and registered
with the capability system, not merely dlopened.

Environment for run 2: **LogosBasecamp 0.2.2**, official macOS arm64 `.dmg`,
`/Applications/LogosBasecamp.app`, portable build, bundling **Qt 6.9.2**.

## What was NOT verified

Stated plainly, because a reviewer will check.

- **No click in the Basecamp GUI.** Verification went through Logos Core's C API
  with Basecamp's own shipped library, not through the App Manager. Basecamp
  0.2.2 has no "install from file" button — its Package Manager installs from a
  configured package repository only — so a GUI install of a local `.lgx` is not
  something a reviewer can do either, which is why the module directory is
  populated by hand below.
- **macOS arm64 only.** No `linux-amd64` variant is built or packaged, and
  nothing here was run on Linux. The install paths given for Linux below are
  read off Basecamp's `LogosBasecampPaths.h`, not tested.
- **No owner-facing UI plugin.** What loads is the `core` module. The owner
  reaches it over the owner channel and through the module's method table; there
  is no Basecamp `ui` app in this repository yet, the way LP-0002 ships one.
- **No skills are registered in the plugin path.** `skills()` returns `[]` from a
  freshly loaded module and `invoke("wallet.balance", …)` returns
  `{"ok":false,"error":"no skill named … is registered"}`. The skill classes
  exist and are unit-tested; nothing in the plugin constructs and registers them
  yet. The harness asserts the honest failure rather than papering over it.
- **The module links neither Logos Delivery nor Logos Storage.** The skills
  reach them through `DeliveryPort` / `StoragePort` function objects, and no
  translation unit in `module/src/` includes `liblogosdelivery.h` or
  `libstorage.h`. `metadata.json` therefore declares `"external_libraries": []`,
  which is the truth. Note that `docs/skills.md` says of Delivery and Storage
  that "both are wired through the `nix` section of `metadata.json` —
  `external_libraries` with a `vendor_path`" — that describes what wiring them
  *would* take, not what is wired today. Nothing is.
- **`logos_protocol_version`.** The runtime logs
  `Module agent carries no usable logos_protocol_version (pre-protocol build) —
  loading permissively` and loads it. That is a property of the pinned
  `logos-cpp-sdk` (below), which predates the stamp. It loads today; a runtime
  that stops being permissive would reject it.

## Building it, without Nix

The blessed path is `logos-module-builder`, a Nix flake library. Nix is not
needed: the generated `CMakeLists.txt` looks for the helper in
`$LOGOS_MODULE_BUILDER_ROOT` first, so a plain checkout is enough.

### The four checkouts, pinned

Pinned to what has been proven, not to what is newest — following default
branches is what broke earlier attempts, when `logos-qt-sdk` and
`logos-protocol` moved and the builder could no longer find `cpp/logos_api.h`.

| Repository | Revision | Used for |
|---|---|---|
| `logos-co/logos-module-builder` | `5396513` | `cmake/LogosModule.cmake` |
| `logos-co/logos-cpp-sdk` | `c87f343` | SDK headers/sources and `logos-cpp-generator` |
| `logos-co/logos-module` | `1947784` | `src/interface.h` |
| `logos-co/logos-package` | `18b0075` | the `lgx` packager (only for packaging) |

```sh
mkdir -p ~/logos/src && cd ~/logos/src
git clone https://github.com/logos-co/logos-module-builder && \
  git -C logos-module-builder checkout 5396513
git clone https://github.com/logos-co/logos-cpp-sdk && \
  git -C logos-cpp-sdk checkout c87f343
git clone https://github.com/logos-co/logos-module && \
  git -C logos-module checkout 1947784
```

Newer `logos-module-builder` revisions additionally require `logos-qt-sdk` and
`logos-protocol` checkouts and will `FATAL_ERROR` without them. The pinned
revision above needs only the three.

### Qt — the version is a ceiling, not a floor

Qt refuses any plugin whose minor version exceeds the host's. Basecamp 0.2.2
bundles **Qt 6.9.2**, so Homebrew's current Qt (6.11.x) is rejected outright —
and a Homebrew build also hardcodes `/opt/homebrew/opt/qtbase/lib/...` as its
library paths, which resolve on the build machine and nowhere else. An official
Qt references its frameworks as `@rpath/...`, which is what resolves against the
host's bundled copy. `QtRemoteObjects` is required (the module transport) and is
not in the default archive set, so ask for it:

```sh
python3 -m venv /tmp/aqt && /tmp/aqt/bin/pip install aqtinstall
/tmp/aqt/bin/python -m aqt install-qt mac desktop 6.9.2 clang_64 \
    -m qtremoteobjects --archives qtbase --outputdir /tmp/Qt
```

`--archives qtbase` keeps it to ~1 GB and avoids an extraction bug in aqt 3.3.0
that trips over `QtSvg`'s symlinks. On Linux, `install-qt linux desktop 6.9.2
linux_gcc_64 -m qtremoteobjects --archives qtbase`; a distribution Qt at or below
6.9 works too (Debian bookworm's 6.4.2 is fine).

`nlohmann/json.hpp` is also needed — `brew install nlohmann-json`, or
`apt install nlohmann-json3-dev`.

### Build

```sh
export LOGOS_MODULE_BUILDER_ROOT=$HOME/logos/src/logos-module-builder
export LOGOS_MODULE_ROOT=$HOME/logos/src/logos-module
export LOGOS_CPP_SDK_ROOT=$HOME/logos/src/logos-cpp-sdk

cmake -S module -B build-basecamp \
      -DCMAKE_PREFIX_PATH=/tmp/Qt/6.9.2/macos \
      -DCMAKE_CXX_FLAGS=-I/opt/homebrew/include
cmake --build build-basecamp -j8
```

That produces `build-basecamp/modules/agent_plugin.dylib`. Confirm which Qt it
picked up before going further — this is the check that costs nothing and saves
an afternoon:

```sh
otool -L build-basecamp/modules/agent_plugin.dylib | head -4
#   @rpath/QtRemoteObjects.framework/... (current version 6.9.2)
#   @rpath/QtNetwork.framework/...       (current version 6.9.2)
#   @rpath/QtCore.framework/...          (current version 6.9.2)
```

`@rpath` and `6.9.2`. An absolute `/opt/homebrew/...` path or a `6.11.x` means
CMake found the wrong Qt: check `Qt6Core_DIR` in `build-basecamp/CMakeCache.txt`
and that `/tmp/Qt/6.9.2/macos/lib/cmake/Qt6/Qt6Config.cmake` exists — a partially
extracted aqt install has the frameworks but not the CMake config, and CMake
then silently falls through to the system Qt.

### The generated glue is committed

`module/generated_code/` holds `logos-cpp-generator`'s output for this module —
the `AgentProviderObject` that dispatches `callMethod`/`getMethods` onto the
impl, the `AgentPlugin` that carries `Q_PLUGIN_METADATA`, and the bodies for the
`logos_events:` declarations. It is committed so the build needs no code
generator, and `LogosModule.cmake` picks the directory up on its own.

Regenerate it after changing `module/agent_module_plugin_export.h`:

```sh
bash $LOGOS_CPP_SDK_ROOT/cpp-generator/compile.sh
~/logos/src/build/cpp-generator/bin/logos-cpp-generator \
    --from-header  $PWD/module/agent_module_plugin_export.h \
    --impl-class   AgentModuleExport \
    --impl-header  agent_module_plugin_export.h \
    --metadata     $PWD/module/metadata.json \
    --backend qt --output-dir $PWD/module/generated_code
```

Two things the generator's parser will do silently, both of which cost a method:
it reads **one-line prototypes only** (a signature wrapped across two lines is
skipped without a warning), and it maps a parameter type it does not know to
`QVariant` and passes it through unchanged, which then does not compile.
`module/agent_module_plugin_export.h` exists because of both — see the note at
the top of that file.

## Packaging

```sh
module/package-basecamp.sh build-basecamp
```

Packaging uses `lgx` from `logos-co/logos-package` — the tool Basecamp's own
packages are built with — found via `$LGX_BIN`, `~/logos/src/logos-package/build/lgx`,
then `PATH`. The script does not reimplement the package format; it only patches
the manifest's `author`/`description`/`type`/`category` afterwards, because
`lgx add` never reads `metadata.json` and leaves them empty. `type` is the one
that matters: Basecamp installs a `core` module into its modules directory and a
`ui` one into its plugins directory, and an unset type lands in neither.

The committed package is `module/agent.lgx` (543 KB, one `darwin-arm64`
variant). Check it against itself rather than trusting this document:

```sh
lgx verify   module/agent.lgx     # contents match the manifest hashes
lgx manifest module/agent.lgx     # type: core, main: agent_plugin.dylib
```

At the time of writing that prints root hash
`e08ea5c6d3ca86f581e1c6b90773690a93152cb5f741b894f2b9271bfdbc0578`. Rebuilding
the module changes it; none of the checks below depend on the value.

## Installing it into Basecamp

There is no "install from file" button in Basecamp 0.2.2's Package Manager — it
installs from a configured package repository only. Install by hand into the
user modules directory, which Basecamp adds to Logos Core at startup
(`logos_core_add_modules_dir(LogosBasecampPaths::modulesDirectory())`):

| Platform | User modules directory |
|---|---|
| macOS | `~/Library/Application Support/Logos/LogosBasecamp/modules` |
| Linux | `~/.local/share/Logos/LogosBasecamp/modules` |

`LOGOS_USER_DIR` overrides the base directory outright, which is the clean way
to try this without touching an existing install.

The package is a gzipped tar of `manifest.json` + `variants/<variant>/…`; an
installed module is that variant **flattened**, plus a `variant` file naming it.
Dropping the archive in does nothing.

```sh
DEST=~/Library/Application\ Support/Logos/LogosBasecamp/modules/agent
mkdir -p "$DEST" && cd "$DEST"
tar xzf /path/to/lp-0008/module/agent.lgx
mv variants/darwin-arm64/* . && rm -rf variants
printf 'darwin-arm64' > variant
ls   # agent_plugin.dylib  manifest.json  metadata.json  variant
```

## Running the two checks

Both harnesses are plain compiles — no CMake target, so they cannot silently
stop being built.

```sh
SDK=$HOME/logos/src/logos-cpp-sdk
QT=/tmp/Qt/6.9.2/macos
APP=/Applications/LogosBasecamp.app

# 1. the Qt plugin contract
clang++ -std=c++17 -o /tmp/plugin_load_test \
    module/tests/plugin_load_test.cpp $SDK/cpp/logos_types.cpp \
    -I$SDK/cpp -I$SDK/cpp/generated -I$SDK/core -I/opt/homebrew/include \
    -F$QT/lib -I$QT/lib/QtCore.framework/Headers \
    -I$QT/lib/QtRemoteObjects.framework/Headers \
    -I$QT/lib/QtNetwork.framework/Headers \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-rpath,$QT/lib
/tmp/plugin_load_test build-basecamp/modules/agent_plugin.dylib

# 2. Logos Core, from the installed app
clang++ -std=c++17 -o /tmp/logos_core_load_test \
    module/tests/logos_core_load_test.cpp \
    -I$QT/lib/QtCore.framework/Headers -I$QT/include \
    -F$QT/lib -framework QtCore -Wl,-rpath,"$APP/Contents/Frameworks"

LOGOS_HOST_PATH="$APP/Contents/MacOS/logos_host" \
QT_PLUGIN_PATH="$APP/Contents/Resources/qt/plugins" \
/tmp/logos_core_load_test \
    "$APP/Contents/Frameworks/liblogos_core.dylib" \
    "$APP/Contents/modules" \
    "$HOME/Library/Application Support/Logos/LogosBasecamp/modules" \
    /tmp/agent-persistence agent
```

Harness 2 links Qt for headers and stubs but adds an rpath pointing at the
**app's** frameworks, so exactly one QtCore is in the process and it is the one
the runtime resolves. Two traps, both of which look like a hang rather than an
error:

- Without a `QCoreApplication` constructed **before** `logos_core_init`, the
  module transport reports `QEventLoop: Cannot be used without QCoreApplication`
  and the load never returns. Basecamp constructs its `QApplication` first; the
  harness does the same.
- Without `LOGOS_HOST_PATH`, the runtime logs `logos_host_qt (or logos_host) not
  found` and `Failed to load module: agent`. Core modules are process-isolated
  and the host binary is what runs them.

## Watching Basecamp itself

Basecamp's file log
(`~/Library/Application Support/Logos/LogosBasecamp/logs/`) truncates early and
never records the loader messages. Launch it from a terminal and read stderr:

```sh
/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp > /tmp/out.log 2>&1 &
grep -E 'Module loaded|Total modules' /tmp/out.log
```

An installed-but-not-enabled core module does **not** appear there — Basecamp
prints `Total modules: 3` and auto-loads only its own three. That is the
expected state, not a failure: `logos_core_load_module` is what promotes a known
module to a loaded one, and harness 2 is that call.

## What still has to be built for the criterion to be met

Honest list, in the order that matters:

1. The plugin has to construct and register the skill objects, so that
   `skills()` answers with something and `invoke()` reaches real code.
2. The owner channel has to be driven from inside the loaded module rather than
   from tests, so that "the owner can interact with the agent in real time from
   a separate Logos app instance" is demonstrable.
3. A `linux-amd64` variant, since a reviewer may be on Linux and a package with
   only `darwin-arm64` is unopenable for them.
4. A Basecamp `ui` app for the owner console, if the owner-facing surface is to
   be a window rather than a method table reached over the owner channel.
