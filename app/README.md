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

It is also the **owner's** app, and not by having a second build: install the
same two packages into a second Basecamp with its own user directory, press
*Join Messaging* and *Watch as owner* there, and that instance is the owner end
of a Logos Messaging channel whose agent end is in the first one. Which role a
window is playing is a matter of which buttons are pressed, not of which binary
was installed. See §"The second panel" below.

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
registers more now — `messaging.receive`, `agent.update`, `agent.poll` and the
three `owner.*` skills were all added after this run — and the window reads
whatever the installed module offers rather than a number of its own, so the
transcript is left as it was rather than edited to agree with a run that did not
happen. No current figure is written into this paragraph either: it said "25 now"
against a module registering 28, because a sentence naming a count next to a
transcript that names a different one is read as a correction and goes stale
silently. `docs/skills.md` §7 carries the number, and `scripts/check-docs.py`
holds it to the registry.

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

## The second panel, which is the one the criterion is about

Everything above reaches the module *this* app loaded. The panel below the rule
— **Owner channel over Logos Messaging — the other app** — reaches an agent in
a **different Basecamp**, over Logos Delivery, with nothing between them but
the public relays. Four buttons and two fields, and every one of them is still
one `invoke` on the loaded module:

| Button | The call | What it is for |
|---|---|---|
| Join Messaging | `meta.configure` ×3: `owner_channel_account`, `agent_account`, `delivery=on` | Pressed on **both** instances. The channel id is derived from the two accounts on both sides, so nothing is exchanged to agree on it |
| Watch as owner | `owner.watch` | Pressed on the **owner's** instance. The agent's end is opened by the module itself when a spend needs approving |
| Approve over Delivery | `owner.answer {"decision":"approve"}` | The reply goes on the reliable channel; the agent is in another process |
| Deny over Delivery | `owner.answer {"decision":"deny"}` | The control, and a completed exchange rather than a failure of one |

Once watching, the window polls `owner.pending` once a second. That poll is the
"in real time" half: a spend minted in the other app appears here without
anybody pressing anything, and it appears with the terms and with this app's own
verdict on them — `owner.pending` never lists a request whose approval marker it
could not re-derive from the request's own recipient, amount and nonce, so what
the window shows is a payment it checked and not a payment it was told about.

The transcript of one such exchange, read out of the two windows, is in
[`docs/basecamp.md`](../docs/basecamp.md) §"Two Basecamps, and the owner in the
second one" — 573 ms from the call to the request appearing in the other app,
371 ms from the Deny button to the verdict, 177 ms from Approve, and
`owner_unreachable` with the owner's app killed.

**The two owner panels answer through different doors, and that is deliberate.**
`approveSpend` (top panel) has to reach a module that is blocked inside the very
call it is answering — which is why this window opens a *second* connection for
it, see the note on `answerApi_` in the header. `owner.answer` (bottom panel)
does not: it hands a reply to this app's own Delivery node and returns, and the
module it is answering is in another process on the other side of a relay.

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
  ok    main[linux-amd64] = agent_ui.so is in the package
  ok    main[linux-arm64] = agent_ui.so is in the package
```

`app/agent-ui.lgx` carries **three** variants — `darwin-arm64`, `linux-amd64`
and `linux-arm64`, one per platform the Logos app is published for. The Linux
ones are built the same way, in a container, against the same official Qt
6.9.2 (`linux_gcc_64` and `linux_gcc_arm64` respectively) — see
[`../docs/basecamp.md`](../docs/basecamp.md), "Linux, and the two Linux
variants", which covers the whole toolchain once for both packages:

```sh
cmake -S app -B build-ui -DCMAKE_PREFIX_PATH=$QT_ROOT \
      -DLOGOS_CPP_SDK_ROOT=$LOGOS_CPP_SDK_ROOT
cmake --build build-ui -j"$(nproc)"
app/package-ui.sh build-ui linux-amd64
```

and `package-ui.sh` asks ELF the same three questions it asks Mach-O, in ELF's
own terms:

```
  ok    Qt is referenced by soname (libQt6Widgets.so.6 libQt6Gui.so.6
        libQt6Core.so.6 ) with RUNPATH $ORIGIN, built against 6.9
  ok    all 7 undefined Logos symbol(s) are exported by the host's liblogos_core
```

There is no `@rpath` on ELF, so the portability question is asked of
`DT_RUNPATH` — which CMake sets to the **build machine's Qt directory** unless
told otherwise, and which `app/CMakeLists.txt` now sets to `$ORIGIN`. And the Qt
version has to be read out of the plugin's own metadata note rather than
inferred from a hardcoded path, because a distribution Qt hardcodes nothing and
a 6.11 plugin would otherwise pass every check and then time out on every call.

The undefined-symbol check on Linux reads `liblogos_core.so` out of the unpacked
AppImage (`./scripts/fetch-logos-core.sh`), and if that is not there it says so
rather than passing: five of the seven symbols were reported missing from a
library that exports all seven the first time it ran, because `printf | grep -q`
under `set -o pipefail` returns 141 for every symbol grep finds early. The macOS
branch carries a comment about exactly that, written years of debugging ago, and
the Linux branch reproduced it anyway.

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

On Linux the directory is `~/.local/share/Logos/LogosBasecamp/plugins` and the
variant is `linux-amd64` or `linux-arm64` — either way `agent_ui.so`, and they
are not interchangeable. Flatten the variant you need, not the
first one in the archive: a plugin directory holding the other platform's binary
looks complete and can never load, and Basecamp reports a plugin that fails to
load to nobody.

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
| `package-ui.sh` | Packaging, with the seven refusals above |
| `agent-ui.lgx.sources` | What the committed package was built from — see below |
| `tests/ui_plugin_load_test.cpp` | The load harness |

## What the committed package was built from

`agent-ui.lgx` is a binary in a repository, and nothing about a binary says what
source it came from. `module/agent.lgx` has shipped stale twice for exactly that
reason — once left behind by five commits to `module/src`, once signing Agent
Cards with an algorithm this repository's own verifier rejects — and the two
layers built to stop it a third time covered `module/` only. This package had no
record at all.

`agent-ui.lgx.sources` is that record, written by `package-ui.sh` at the moment
the package is made and recomputed by `scripts/check-package-fresh.py` on every
CI run. It holds the SHA-256 of every build input, and per variant the shipped
binary's SHA-256 and length, the format and architecture read out of the
binary's own header, and the Qt it was linked against. What is checked back:

- every build input hashes as it did, and the file *set* is compared, so a
  source added to `app/src` and forgotten is caught as surely as one edited;
- every string literal of ≥ 8 bytes in `app/src` is present in each of the three
  shipped binaries — the record is a record, and a hand-edited one is a claim
  about a build that did not happen, so this is what the hashes are corroborated
  with. How many there were is printed by the run rather than written down here,
  and the checker refuses to call it corroboration below a floor: a scanner that
  extracts nothing finds nothing missing from any binary at all;
- every variant really is the architecture its name claims, from the ELF or
  Mach-O header, because the flattening instructions above hand an installer one
  variant and a directory holding another platform's binary looks complete and
  can never load;
- every variant was built against Qt 6.9 or lower, the ceiling Basecamp 0.2.2
  bundles. `package-ui.sh` asks that of the platform it runs on and cannot ask
  it of a variant built elsewhere; the checker asks it of all three, out of
  `LC_LOAD_DYLIB` on Mach-O and the `Qt_6.9` symbol version on ELF.

The three variants were recorded from the package's own bytes rather than from
three builds, because the package was committed before the record existed and
no one machine can build all three. That is written into the record's own
`recorded_from` field, along with what it therefore cannot say: `compiler` and
`built_on` are `null`, and repackaging any variant replaces its entry with a
real build's.
