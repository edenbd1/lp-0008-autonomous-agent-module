# The owner console — LP-0008's Basecamp `ui` plugin

The prize asks for an owner-facing interface "accessible from the Logos app
(Basecamp) via the owner channel". `module/` is the `core` half: it loads, it
answers, and until this directory existed it had no window, because a `core`
module is not a surface. Basecamp gives windows to `ui` plugins only, and there
was none.

This is that window. It is a Qt Widgets plugin implementing Basecamp's
`IComponent`, packaged `type: ui`, and it holds no agent logic of its own:
every button is one call on the loaded `agent` module over Logos Core's own
transport — `configure`, `start`, `stop`, `skills`, `status`, `invoke`,
`approveSpend` — the same method table `module/tests/logos_core_load_test.cpp`
drives. Nothing here is computed twice.

## What it does, and what that proves

Launched from a terminal so its stderr can be read, Basecamp 0.2.2 with this
package installed prints, on the click of the sidebar tile:

```
App launcher clicked: "agent-ui"
Loading UI module: "agent-ui"
Loading core dependency for "agent-ui" : "agent"
[info] [logos] Module loaded: agent
[info] [logos] [agent] RemoteTransportHost: Published object: "agent"
MainContainer: Added plugin dock to WorkspaceArea: "LP-0008 Agent" (module: "agent-ui" )
Successfully loaded UI module: "agent-ui"
```

`Loading core dependency` is the mechanism, and it is why `metadata.json`
declares `"dependencies": ["agent"]`: Basecamp's PluginLoader loads each core
dependency of a `ui` plugin, has `capability_module` mint the plugin a token
for it, and only then calls `createWidget(LogosAPI*)`. So the window opens onto
a module that is already loaded and already reachable. The console also calls
`logos_core_load_module` itself if the module is somehow not loaded, which is
the case a reviewer hits if they install this package and not the other one —
the transcript then says which one is missing.

The window's own transcript, from a run inside the app:

```
<- already loaded: package_manager package_downloader capability_module agent
<- got a LogosAPIClient for 'agent'
<- opened a second connection for the owner's verdict …
<- subscribed to ownerApprovalRequested — this window is now the owner end of the channel
-> agent.status()
<- status: {"configured":false,"owner":"","policy":"","started":false}
-> agent.configure(0x…a9, aaaa…)
<- configure accepted: the agent is bound to this owner and this policy anchor
-> agent.start()
<- start accepted
<- skills(): 23 entries parsed, 22 names in 4436 bytes of reply (* = required)   (count-as-it-was)
```

That count is the one this window read on the day it was captured. The module
registers 25 now — `agent.update` and `agent.poll` were added after this run —
and the window reads whatever the installed module offers rather than a number
of its own, so the transcript is left as it was rather than edited to agree with
a run that did not happen.

and then the owner channel, which is the part the criterion is actually about:

```
-> agent.invoke(wallet.send, {"recipient":"9xQe…","amount":"100"})
<= event ownerApprovalRequested (attempt 1): {"amount":"100","id":"spend-1786826270705", …
<= event ownerApprovalRequested (attempt 2): …
<= event ownerApprovalRequested (attempt 3): …
-> agent.approveSpend(spend-1786826270705, approved)
<- invoke(wallet.send): {"amount":"100","answer_path":true,"approved":true,"attempts":3,
   "delivered":3,"error":"the owner approved this spend; submitting it goes through the
   policy program's spend_approved path…"}
<- approveSpend(spend-1786826270705, approved) accepted
```

A spend the agent could not authorise itself was published to its owner, the
owner answered it from a window inside the Logos app, and the agent acted on
the answer — 7.2 s from the call to the verdict, well inside a 60 s wait.

Two things had to be fixed in the module to get that round trip, and both were
found by measuring rather than reading. See `docs/limitations.md` §"The owner
channel inside Basecamp" for the measurements and what is still bounded.

## Building it

Qt is a **ceiling, not a floor**. Basecamp 0.2.2 bundles Qt 6.9.2 and refuses a
plugin built against a higher minor with `uses incompatible Qt library`, and a
Homebrew build additionally hardcodes `/opt/homebrew/...` library paths that
resolve on the build machine and nowhere else. Get 6.9.2 the way
`docs/basecamp.md` does — that section is shared with the module and its
warnings apply here unchanged. `QtRemoteObjects` is not needed for this target,
but `--archives qtbase` already carries QtWidgets, which is:

```sh
python3 -m venv ~/logos/aqt && ~/logos/aqt/bin/pip install aqtinstall
~/logos/aqt/bin/python -m aqt install-qt mac desktop 6.9.2 clang_64 \
    -m qtremoteobjects --archives qtbase --outputdir ~/logos/Qt
```

The only other input is a `logos-cpp-sdk` checkout, pinned at `c87f343` like
the module's, and it is used for **headers only**:

```sh
cmake -S app -B build-ui \
      -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
      -DLOGOS_CPP_SDK_ROOT=$HOME/logos/src/logos-cpp-sdk
cmake --build build-ui -j8
```

That produces `build-ui/agent_ui.dylib`. Two checks before going further, both
of which cost nothing:

```sh
otool -L build-ui/agent_ui.dylib | grep QtCore   # @rpath, current version 6.9.2
nm -u build-ui/agent_ui.dylib | grep -i logos    # 7 undefined symbols, and that is correct
```

Undefined `LogosAPI` / `LogosAPIClient` symbols are the design, not a build
error. They resolve at load time against the `liblogos_core` Basecamp already
has in its process — which is exactly what `nm -u` on Basecamp's own
`main_ui.dylib` shows, and it is the only way to reach the module: compiling
the SDK's translation units in instead gives this plugin its own
`TokenManager` singleton, which starts empty and turns every call into
`ModuleProxy: rejecting unauthorized call … auth token not recognized`.

## Packaging and installing

```sh
app/package-ui.sh build-ui
```

That drives the real `lgx` from `logos-co/logos-package` — found via `$LGX_BIN`,
then `~/logos/src/logos-package/build/lgx`, then `PATH` — and refuses to hand
back a package with any of the defects that produce a plugin which installs and
loads nowhere:

```
  ok    the plugin is newer than every source it is built from
  ok    Qt is referenced through @rpath, version(s): 6.9.2
  ok    all 7 undefined Logos symbol(s) are exported by the host's liblogos_core
  ok    type: ui — Basecamp installs this into its plugins directory
  ok    main[darwin-arm64] = agent_ui.dylib is in the package
```

`lgx` is not on `PATH` after building `logos-package`; it stays in that
checkout's `build/`, which is where the script looks.

Basecamp 0.2.2's Package Manager installs from a configured package repository
only — there is no "install from file" — so install by hand, into the user
**plugins** directory (not the modules one, which is where `agent.lgx` goes):

| Platform | User plugins directory |
|---|---|
| macOS | `~/Library/Application Support/Logos/LogosBasecamp/plugins` |
| Linux | `~/.local/share/Logos/LogosBasecamp/plugins` |

An installed plugin is the package's variant **flattened**, plus a `variant`
file naming it. Dropping the archive in does nothing.

```sh
DEST=~/Library/Application\ Support/Logos/LogosBasecamp/plugins/agent-ui
mkdir -p "$DEST" && cd "$DEST"
tar xzf /path/to/lp-0008/app/agent-ui.lgx
mv variants/darwin-arm64/* . && rm -rf variants
printf 'darwin-arm64' > variant
ls   # agent_ui.dylib  manifest.json  metadata.json  variant
```

Install `module/agent.lgx` too, per `docs/basecamp.md` — this window is a
window onto that module, and without it the console reports
`the runtime does not know a module called 'agent'` rather than pretending.

Restart Basecamp. The tile is in the left rail, labelled from the manifest's
`display_name`: **LP-0008 Agent**.

## The load harness

`app/tests/ui_plugin_load_test.cpp` reproduces what Basecamp's PluginLoader
does, and asserts on each step: the plugin binds against `liblogos_core` and
**fails to bind without it**, the IID is `com.logos.component.IComponent`, the
embedded manifest says `type: ui` and declares `agent`, `qobject_cast` succeeds
across the boundary, and `createWidget(nullptr)` returns this plugin's console
rather than taking the host down.

```sh
QT=$HOME/logos/Qt/6.9.2/macos
QTINC=(-F$QT/lib -I$QT/lib/QtCore.framework/Headers
       -I$QT/lib/QtGui.framework/Headers -I$QT/lib/QtWidgets.framework/Headers)
clang++ -std=c++17 -Wall -Wextra -o /tmp/ui_plugin_load_test \
    app/tests/ui_plugin_load_test.cpp "${QTINC[@]}" \
    -framework QtCore -framework QtGui -framework QtWidgets -Wl,-rpath,$QT/lib

QT_QPA_PLATFORM=offscreen /tmp/ui_plugin_load_test \
    "$HOME/Library/Application Support/Logos/LogosBasecamp/plugins/agent-ui/agent_ui.dylib" \
    /Applications/LogosBasecamp.app/Contents/Frameworks/liblogos_core.dylib
```

`QTINC` is an array, not a string: zsh does not word-split an unquoted variable,
so a string here reaches clang as one enormous argument and every Qt header goes
missing at once.

It was watched failing before it was believed, against two real Qt plugins that
are not this one:

| Run against | Result |
|---|---|
| `plugins/agent-ui/agent_ui.dylib` | `all steps confirmed (0 failure(s))`, exit 0 |
| Basecamp's `package_manager_ui_plugin.dylib` | exit 1 — `type` is `ui_qml`, no `agent` dependency, and `qobject_cast to IComponent returned null` |
| this repo's own `agent_plugin.dylib` (the `core` module) | exit 1 — `type` is `core`, and the same null cast |

The third is the one worth keeping: a `core` plugin loads perfectly, casts to
its own interfaces perfectly, and is not a window. That is the state this
repository was in before this directory existed.

## Files

| File | What |
|---|---|
| `metadata.json` | The manifest. `type: ui` puts it in the plugins directory; `dependencies: ["agent"]` is what makes Basecamp load the core module |
| `src/plugin.{h,cpp}` | The Qt plugin object and the ABI-critical `IComponent` declaration |
| `src/agent_console.{h,cpp}` | The console: every button is one call on the loaded module |
| `package-ui.sh` | Packaging, with the five refusals above |
| `tests/ui_plugin_load_test.cpp` | The load harness |
