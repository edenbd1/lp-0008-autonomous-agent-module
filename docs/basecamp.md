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
  <-    getMethods(): start, skills, configure, invoke, status, stop
  ok    before configure it reports itself unconfigured
  ok    configure refuses a malformed policy hash
  ok    a second configure is refused — the binding is the agent's identity
  ok    before start, skills() is an error rather than an empty card
  ok    status reflects the running agent
  <-    skills(): 23 entries: storage.share, wallet.send, program.deploy, …
  ok    every skill the module ships with is listed: missing none
  ok    the card has exactly 23 entries, and 22 distinct names
  ok    each carries a parameter schema: all present
  ok    invoke() dispatches to every one of them: undispatched none
  <-    invoke(meta.skills): {"count":22,"ok":true,"skills":[{"name":"agent.cancel", …
  ok    meta.skills lists all 23 skills over the boundary, and counts them
  ok    and every one of them carries the parameter schema skills() published for it
  ok    including itself: it is a registered skill, not a special case in invoke()
  ok    an unwired skill refuses as itself, not as a name nobody registered
  ok    and a name that is not registered is refused as that, without taking the module down
all steps confirmed (0 failure(s))
```

Those three `meta.skills` lines are the ones this harness did not have, and not
having them is how the skill came to be documented in three C++ headers and in
`docs/a2a-binding.md` while `invoke("meta.skills")` answered *no skill named
'meta.skills' is registered*. `AgentModuleImpl::skills()` had always produced
the catalogue, so every reader of the source saw a working feature; `invoke()`
is a plain map lookup, and nothing had ever put that name in the map. A harness
that asks the loaded binary is the only thing that tells those two apart.

That run is against the `agent_plugin.dylib` unpacked from the committed
`module/agent.lgx`, not against `build-basecamp/`.

**2. Logos Core itself loads it, and the module it loaded offers its skills**
(`module/tests/logos_core_load_test.cpp`). This one is not a reproduction of
the host: it `dlopen`s the real `liblogos_core.dylib` out of the installed
`LogosBasecamp.app` and drives it through the same C API, in the same order, as
Basecamp's own `app/main.cpp` — `logos_core_init`, both module directories
added, persistence path, access policy, `logos_core_start`, then
`logos_core_load_module("agent", true)`. That last call is exactly what runs
when a user enables a module; Basecamp itself auto-loads only `package_manager`
and `package_downloader`.

It does not stop at "loaded", because loaded was never the claim worth making.
A module that loads and answers `skills()` with `[]` is *worse* than one that
fails to load: an empty Agent Card is a valid Agent Card, so what a reviewer
sees is a module that installed, enabled, and does nothing, with no error
anywhere saying why. So everything after the load goes back into the module
over the runtime's own transport — `LogosAPI("core")` / `LogosAPIClient`, the
same SDK facade `app/main.cpp` constructs, and the only way to reach a core
module, which the runtime runs in its own `logos_host` process:

```
  <-    known modules: package_manager package_downloader capability_module agent
  ok    the runtime discovers the module in the user modules directory
  ok    the installed module names the variant it is
  <-    installed manifest: main[darwin-arm64] = agent_plugin.dylib
  ok    the manifest declares a `main` for darwin-arm64
  ok    `main` (agent_plugin.dylib) names a file that is really in the module directory
  ok    metadata.json's `main` (agent_plugin) agrees with the manifest's (agent_plugin.dylib)
  ok    logos_core_load_module() reports success
  <-    loaded modules: capability_module agent
  ok    the module is in the runtime's loaded set
  ok    the SDK hands out a client for the loaded module
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  ok    skills() answers with a JSON array, not an error object: [{"name":"agent.cancel", …
  <-    skills(): 23 entries
  ok    the loaded module lists all 23 documented skills
  ok    it lists exactly 22 — no more, no fewer (got 22)
  ok    every listed skill carries a parameter schema (22 checked)
  ok    and it answered as a running agent, not as a stopped one
  ok    invoke() dispatches to every one of the 23
  <-    meta.status durability: {"path":".../agent-persistence/agent/a45bddb77136/tasks.json","recovered_active":0,"recovered_tasks":0,"recovery":"absent","recovery_ran":true,"settled_payments":0,"uncertain_payments":0}
  ok    the loaded module reports a durability record, not null: it was given a persistence directory and opened a task snapshot in it
  ok    and the snapshot lives under the persistence base the host set
  ok    recovery ran before the agent started serving, and reported 'absent'
  ok    approval_timeout_ms is settable on the running module, and effective: {"effective":true,"key":"approval_timeout_ms","ok":true,"stored":"1500","value":"1500"}
  ok    approval_resend_ms is settable on the running module, and effective
  <-    wallet.send above threshold: {"amount":"100","answer_path":true,"attempts":8,"delivered":8,"error":"the owner did not answer within 1500ms: 8 notification attempt(s), 8 of which the channel accepted; the spend was not submitted","ok":false,"outcome":"owner_unreachable","submitted":false, …}
  ok    an above-threshold spend nobody approved is not submitted by the loaded module
  ok    and the outcome is the terminal owner-unreachable one, not a fallback to acting alone
  ok    the notification was retried before the timeout: 8 attempts
  ok    and the failure is reported against the correlation id the owner was asked under
  ok    approveSpend is reachable, and refuses a request nobody is waiting on: no spend is waiting on 'spend-nobody-asked': there is nothing here for this answer to release
  ok    a name nobody registered is refused as unregistered: {"error":"no skill named 'wallet.definitely_not' is registered","ok":false}
all steps confirmed (0 failure(s))
```

Two of those lines are the Reliability criteria, and both were previously
demonstrable only against classes the shipped plugin never constructed:

- `meta.status` reports a **durability** record, which means the host really did
  hand this module a per-instance directory and the module really did open a
  task snapshot under it. The negative control is worth running: put a
  half-written file at that path and the same harness reports
  `FAIL start() is accepted across the transport: pending task state could not
  be recovered from …: the snapshot … is truncated or corrupt. Refusing to
  start with an empty task list on top of a snapshot that could not be read`,
  followed by `skills(): 0 entries` — the agent serves nothing rather than
  coming up believing it owes nobody anything. Executed; exit 1, 13 failures.
- `wallet.send` above the envelope was published to the owner **eight times over
  1500 ms** and then reported as unreachable with nothing submitted. The
  runtime's own log carries the eight `emitEvent: "ownerApprovalRequested"`
  lines between the call and the answer.

The runtime's own log during that run prints `Module loaded: agent` — the same
line it prints for `capability_module` — then spawns `logos_host` for it,
publishes `local:logos_agent_<id>`, and the capability module issues a token for
`agent`. So the module was published over the transport and registered with the
capability system, not merely dlopened, and the `skills()` above came back
across that transport from the separate process the runtime started.

Two details make those last checks mean what they say rather than pass for
free:

- **The dispatch check has a control.** Called with `{}`, most skills refuse,
  because a module loaded as a *plugin* has no way to receive a `std::function`
  port across the boundary. That refusal is the skill's own —
  `{"error":"no account to read: …"}` — and is the proof the call arrived. Only
  the registry's `no skill named '…' is registered` means it did not. The
  harness asserts both directions: no listed skill may produce the registry's
  refusal, and a name nobody registered must.
- **It fails on the artefact this repository shipped in `333e7a8`.** Run against
  that `agent.lgx` — packaged before the skills were registered, and otherwise
  identical in manifest, type and load behaviour — the same harness reports
  `skills(): 0 entries`, `[]`, and three failures, while still passing every
  check up to and including "the module is in the runtime's loaded set". The
  checks are not describing the code; they discriminate between two builds of
  it, and they discriminate exactly where the difference is.

Environment for run 2: **LogosBasecamp 0.2.2**, official macOS arm64 `.dmg`,
`/Applications/LogosBasecamp.app`, portable build, bundling **Qt 6.9.2**. The
package under test was the committed `module/agent.lgx`, unpacked into the user
modules directory by the procedure below — not the build tree.

## The ports a loaded module builds for itself

For most of this repository's life the sentence above — "a port is a
`std::function` and there is no wire format for one" — stood as the reason a
loaded module could do nothing on the network. It is a true sentence about what
a HOST can PASS. It was read as a statement about what a MODULE can HAVE, and
those are different claims: a module that links `liblogosdelivery` can open a
node from its own configuration and construct its own ports on the far side of
the boundary, where nothing has to be serialised because nothing crosses.

What crosses is `invoke("meta.configure", {"key":"delivery","value":"on"})` —
two strings, which Qt Remote Objects has carried since the beginning. Nothing
starts on its own: loading the module joins no network and opens no socket, and
every skill on the wire keeps the refusal it already had (`"delivery node is not
started"`) until an operator asks. `meta.status` reports which of the two
situations a refusal means:

```
  {"linked":true, "state":"off"}       the library is in the binary, no node yet
  {"linked":true, "state":"ready"}     a node is up
  {"linked":false,"state":"absent"}    this build has no Delivery library at all
```

**3. The runtime loads it, and the module opens a Delivery node inside
`logos_host`** (`module/tests/logos_core_delivery_test.cpp`). Harness 2 above
proves the module is reachable; this one proves what it can do once reached. A
core module does not run in the caller's process — Logos Core spawns
`logos_host` for it — so the node below is opened inside a process the harness
never enters, by a plugin it only caused to be `dlopen`ed. Run against the
plugin unpacked from the committed `module/agent.lgx`:

```
  ok    logos_core_load_module() reports success
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  <-    meta.status delivery: {"linked":true,"state":"off"}
  ok    the module the RUNTIME loaded links Logos Delivery into itself
  ok    and has started no node, because nobody has asked it to
  <-    messaging.send: {"error":"delivery node is not started","ok":false}
  ok    the wire skills refuse over the transport, exactly as documented
  <-    meta.configure: {"effective":true,"key":"delivery","ok":true,"stored":"on","value":"on"}
  ok    meta.configure('delivery','on') crosses Qt Remote Objects
  ok    the module opened and started its own Delivery node inside logos_host
  <-    messaging.join: {"ok":true,"topic":"/lp-0008/1/discovery-lp0008corelive/json"}
  <-    messaging.send: {"bytes":7,"ok":true,"topic":"/lp-0008/1/owner-lp0008corelive/json"}
  ok    messaging.send put a message on the public network
  <-    agent.discover: {"agents":[],"ok":true,"rejected":[{"index":0,"reason":"the card is not valid JSON"}],"seen":1,…}
  ok    agent.discover answers with a result, not 'no discovery transport is configured'
  <-    agent.task: {"ok":true,"state":"submitted","task_id":"corelivetask","topic":"/lp-0008/1/task-lp0008corelive-corelivetask/json",…}
  ok    agent.task opened a task and put the A2A request on the wire
  ok    agent.subscribe subscribed to that task's topic
  ok    meta.configure('delivery','off')
  ok    the node is down and the module says so
the module Logos Core loaded obtained a working Delivery port (0 failure(s))
```

`agent.discover`'s `"seen":1` is not decoration: the document it rejected is the
seven bytes `messaging.send` published two lines earlier, which went out to the
public relays and came back into the module's own inbox on that exact content
topic. A card it cannot parse is reported as rejected; a topic nothing arrived
on reports `"seen":0`.

**4. Two loaded modules discover each other's signed Agent Cards**
(`module/tests/plugin_delivery_test.cpp`, `peer` mode; run it with
`./scripts/delivery-in-plugin.sh peers`). Harness 3 cannot be evidence for the
discovery criterion on its own, because a Waku node receives its own published
messages — a single process can satisfy any assertion about "a card arrived"
with every other agent on earth switched off. So this is two processes, each
loading the same plugin, each with its own node, its own working directory and
its own LEZ account, sharing nothing but a content topic derived from one run
id. Each accepts only a card whose `url` names the OTHER account, so neither can
satisfy itself:

```
agent A                                    agent B
  ok  this agent's own Delivery node came up  ok  this agent's own Delivery node came up
  ok  the card names this agent's account     ok  the card names this agent's account
  ok  and carries a signature                 ok  and carries a signature
  ok  published its own signed card           ok  published its own signed card
  ok  discovered the OTHER agent's signed Agent Card over the public network
                                              ok  (the same, in the other direction)
  ok  which is signed — `require_signed` was on
two loaded modules discovered each other (0 failure(s))
```

The cards are real: `agent.card` is assembled by the loaded module out of its
own registry — so it cannot advertise a skill the agent has not registered — and
signed BIP-340 over secp256k1 by the LEZ account key, through the `card_signer`
command `meta.configure` names. `CardPort::sign` has always declared exactly
that contract ("given `<protected>.<payload>`, return the base64url signature"),
and the curve arithmetic stays outside the module deliberately: this plugin
links no crypto library, and a hand-rolled 256-bit field inside the binary that
signs payment instructions is the last place to put one.

**5. A loaded module pays for the task it was served**
(`module/tests/plugin_delivery_test.cpp`, `peer` mode with a payment configured;
run it with `./scripts/delivery-in-plugin.sh settle`). Harness 4 ends with two
agents that have found each other and served each other, and no money moving.
That was not a limitation of the plugin boundary either — it was
`TaskPort::pay` left unwired, with a note in `agent_module_plugin.cpp` saying a
settlement needs a wallet and a sequencer "and this module has neither". It is
the same sentence as the one about ports, and it has the same answer: the module
does not need to HAVE a wallet, it needs to be able to REACH one, and
`card_signer` had already shown how.

So `pay_signer` is `card_signer`'s mechanism with a different command behind it —
literally the same function, `AgentModuleImpl::runConfiguredCommand`, of which
the card signer is now one of three callers. `policy_source` is the third, and it
is what makes the payment *unattended*: it reads the agent's anchored policy
account off the chain, so `agent.task` can see that the price is inside the
envelope its owner anchored and pay it without asking anyone. Both are named in
`meta.configure`, both take their input on stdin, and both have their answer
checked character by character before it is believed — 64 lower-case hex for a
settlement, decimal digits for a limit.

The buyer is handed no price and no payee. It reads both off the seller's signed
card, which arrived over Waku a few seconds earlier:

```
buyer                                       seller
  ok  discovered the OTHER agent's signed Agent Card over the public network
  ok  the discovered card advertises a price to pay: 1 LEZ
  ok  and a public account to pay it into: Public/BzYks91a…
  ok  this agent opened an A2A task addressed to the other one
  ok  it paid the price the peer's card advertised, 1 LEZ
  ok  and settled it on chain, from inside the loaded module, with no owner in
      the path: <64 hex>
                                              ok  the card this agent was handed
                                                  advertises no price, so there
                                                  is nothing to pay
                                              ok  and no settlement hash came
                                                  back for it
  ok  and READ the other agent's A2A request off its own task topic
```

The seller's two lines are the control, and they are the same code path: one
`agent.task` call, one card, and the answer differs only because the card does.
Without them "the module reported a transaction hash" would be indistinguishable
from "the module reports a transaction hash whenever it opens a task".

**The refusals are a separate harness, because this one costs money.**
`./scripts/delivery-in-plugin.sh signers` needs no second agent, no key and no
chain, and pins nine decisions: an envelope the module cannot read is *unknown*
and unknown is outside, a price over the anchored limits never reaches the
signer at all, and neither an empty answer nor a diagnostic from the signer
becomes a settlement. Its assertions were watched failing against three mutated
builds — with the hash check removed the module writes `error: this signer holds
no key` into the task record as a settlement, and a module that reads "I do not
know" as "no limit" pays a task nobody configured it for.

**What the module still cannot do here.** `TaskPort::refund` stays unwired: a
refund would have to be signed by the payee, whose key this agent does not hold.
And an above-envelope *task* price is refused immediately with
`owner_unreachable` rather than put to the owner — `wallet.send` has that path
and `agent.task` does not, because on this chain the owner who anchored a policy
cannot approve under it (one program transaction per public signer), so wiring
it would add a two-minute wait that can never succeed.

**The negative control, which is what makes the four transcripts above mean
anything.** Build without `-DLOGOS_DELIVERY_ROOT` — the default — and run the
same harness against the same package layout. It reports:

```
  <-    meta.status delivery: {"error":"this build of the agent module does not link Logos
        Delivery, so no node can be started in it","linked":false,"state":"absent"}
  FAIL  this build links Logos Delivery into the plugin
  FAIL  and has not started a node, because nobody has asked it to
  ok    meta.configure('delivery','on') is accepted
  FAIL  the module's own Delivery node came up and reported it
```

Note which line stays green: `meta.configure('delivery','on')` is *accepted* by
a build that cannot act on it, because the setting is stored either way. A
harness that stopped at "the call succeeded" would have called that a pass —
which is the same shape as the Qt-version failure two sections down, where
`logos_core_load_module` returns success for a plugin the host then cannot use.

### What this needs at build time

The library is not vendored. Point the build at a `logos-delivery` checkout that
has been through `make liblogosdelivery` (see `scripts/exercise-nodes.sh`, which
already builds it):

```sh
cmake -S module -B build-basecamp \
      -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
      -DCMAKE_CXX_FLAGS=-I/opt/homebrew/include \
      -DLOGOS_DELIVERY_ROOT=$PWD/_external/logos-delivery
```

Without that flag the module still builds and still ships `delivery_runtime.cpp`
— every entry point answers "no" and `meta.status` says `absent`. That is a
deliberate difference from leaving the file out: a build that cannot start a
node has to be able to say so, and `"absent"` and `"off"` produce byte-identical
refusals from every skill that touches the wire.

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
- **The GUI click is now verified; two rows below it are not.** This bullet used
  to read "No click in the Basecamp GUI" and then "No owner-facing UI plugin",
  and both are closed — `app/` is the `ui` plugin, the tile is in Basecamp's
  left rail, and the section "Inside the running app" below is the record of it
  being clicked. What is still true is the sentence those bullets opened with:
  Basecamp 0.2.2 has no "install from file" button — its Package Manager
  installs from a configured package repository only — so **both** packages are
  installed by hand, by the procedures below, and a reviewer cannot do it
  through the GUI either.
- **The storage skills have no ports wired**, and neither do the sequencer, the
  local toolchain, or the *reading* half of the wallet.
  `invoke("wallet.balance", "{}")` returns `{"ok":false,"error":"no account to
  read: the agent has none configured and none was given"}`, and `meta.status`
  reports `balance: null` with `balance_error` rather than `0`. Those need a
  Logos Storage node and a sequencer client in the module's process, which is a
  different problem from the transport one below and is not solved.

  The *spending* half is a third case and it is now closed, by delegation rather
  than by linking: `agent.task` pays through a `pay_signer` command and reads
  its anchored envelope through a `policy_source` command, the same shape
  `card_signer` has always had. `wallet.send` deliberately does not — its
  envelope is a struct of strings fixed at `start()`, so wiring a spend there
  would let one out under an envelope that was empty when the module came up.
  See §5 above and [`docs/skills.md`](skills.md).
- **The module reaches Logos Delivery and not Logos Storage.** This bullet used
  to say it linked neither, and to give the reason: "a port is a `std::function`
  and there is no wire format for one, so a host that loads this as a plugin
  cannot wire them". The premise is right and the conclusion was wrong — see
  §"The ports a loaded module builds for itself" above.
  `module/src/delivery_runtime.cpp` opens `liblogosdelivery` with `dlopen` at
  the moment a node is asked for, `module/agent.lgx` carries the library and its
  licence beside the plugin, and the messaging, discovery and task transports
  work in a module Logos Core loaded. `libstorage.h` is still included by
  nothing, so `storage.*` still refuses.
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
python3 -m venv ~/logos/aqt && ~/logos/aqt/bin/pip install aqtinstall
~/logos/aqt/bin/python -m aqt install-qt mac desktop 6.9.2 clang_64 \
    -m qtremoteobjects --archives qtbase --outputdir ~/logos/Qt
```

That lands 209 MB in `~/logos/Qt/6.9.2/macos` and takes about six seconds.
`--archives qtbase` is what keeps it that small, and it also avoids an
extraction bug in aqt 3.3.0 that trips over `QtSvg`'s symlinks. On Linux,
`install-qt linux desktop 6.9.2 linux_gcc_64 -m qtremoteobjects --archives
qtbase`; a distribution Qt at or below 6.9 works too (Debian bookworm's 6.4.2
is fine).

**Not `/tmp`.** Earlier revisions of this document said `--outputdir /tmp/Qt`,
and that is how an afternoon was lost. What was found there later was
`/tmp/Qt/6.9.2/macos/lib` holding 63 frameworks and the directory holding
nothing else: no `lib/cmake/`, no `include/`, no `bin/`, no `libexec/`, no
`mkspecs/`, and no `QtRemoteObjects.framework`. The `aqt` virtualenv beside it
had been reduced to a `bin/` with no `lib/`. That is macOS's periodic `/tmp`
cleaner, which deletes files by age and takes out precisely the ones a finished
build stops touching. The result reads as a partially extracted download and is
not one, which is why the symptom is so hard to place.

CMake's behaviour on that directory is the expensive part: it does not fail. It
cannot find `Qt6Config.cmake`, falls through to the system Qt without a word,
and produces a Homebrew-linked 6.11 plugin. Install Qt somewhere durable. The
check is two lines:

```sh
ls ~/logos/Qt/6.9.2/macos/lib/cmake/Qt6/Qt6Config.cmake   # must exist
ls -d ~/logos/Qt/6.9.2/macos/lib/QtRemoteObjects.framework # and this
```

A complete install has `bin include lib libexec mkspecs plugins` and more; one
that has only `lib` is the trap above, and re-running the `aqt install-qt`
command repairs it.

What a 6.11 plugin then does in Basecamp is worth stating exactly, because it
is not "fails to load" and it is the reason `package-basecamp.sh` refuses to
build one. Harness 2, run against a module directory holding the Homebrew
build, reports:

```
  ok    logos_core_load_module() reports success
  ok    the module is in the runtime's loaded set
  FAIL  configure() is accepted across the transport: no LogosResult came back (got nothing)
```

`logos_core_load_module` returns success and the module joins the loaded set.
The truth is 250 ms earlier, in the runtime's log, and nothing surfaces it:

```
[error] [logos] [agent] LogosModule: Failed to load plugin: ".../agent_plugin.dylib"
  Error: "The plugin '.../agent_plugin.dylib' uses incompatible Qt library. (6.11.0) [release]"
```

After that the module's `logos_host` is gone, nothing is published, and every
call spends 20 seconds on `Timeout waiting for replica: "agent"` before giving
back an empty QVariant. A harness that stopped at "loaded" would have called
that a pass.

`nlohmann/json.hpp` is also needed — `brew install nlohmann-json`, or
`apt install nlohmann-json3-dev`.

### Build

```sh
export LOGOS_MODULE_BUILDER_ROOT=$HOME/logos/src/logos-module-builder
export LOGOS_MODULE_ROOT=$HOME/logos/src/logos-module
export LOGOS_CPP_SDK_ROOT=$HOME/logos/src/logos-cpp-sdk

cmake -S module -B build-basecamp \
      -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
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
— it must read `…/logos/Qt/6.9.2/macos/lib/cmake/Qt6Core`, not
`/opt/homebrew/opt/qt/lib/cmake/Qt6Core` — and that `Qt6Config.cmake` exists at
all. An incomplete Qt has the frameworks but not the CMake config, and CMake
then silently falls through to the system Qt. `module/package-basecamp.sh`
refuses to package a plugin that got this wrong, so the mistake cannot reach the
committed artefact, but it is cheaper to catch here.

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

It then refuses to hand back a package with either of the two defects that
produce a module which installs and loads nowhere, both of which are silent:

```
  ok    main[darwin-arm64] = agent_plugin.dylib is in the package
  ok    Qt is referenced through @rpath, version(s): 6.9.2
```

The first is not what `lgx verify` checks. `verify` compares the contents
against the manifest's hashes, which a manifest naming a file the package does
not contain passes perfectly well — the host resolves `main` inside the module
directory, finds nothing, and logs nothing. This repository shipped that defect
once, with `main` naming `agent_module_plugin` while the builder emits
`agent_plugin`. The second catches a plugin built against Homebrew's Qt: it
fails on the absolute `/opt/homebrew/...` paths before the version even matters,
because those resolve on the build machine and on no other.

### The package records what it was built from

Packaging also writes `module/agent.lgx.sources`: the SHA-256 of every build
input, the SHA-256 of the plugin binary that came out, and the package's
manifest root hash. `scripts/check-package-fresh.py` reads it back, and the
`package` job in `.github/workflows/ci.yml` runs that on every push.

It exists because the two checks above cannot see the defect that has now
shipped twice from this directory — a package that was correct when it was made
and was left behind by later commits to `module/src`. `f53f822`'s `agent.lgx`
stayed committed across five commits to the sources; and `d995d85` made the
module sign Agent Cards `secp256k1-bip340` instead of the `EdDSA` that
`scripts/use-cases/verify-agent-card.py` rejects, without repackaging, so the
published `.lgx` signed cards this repository's own verifier refuses. Both
binaries load, cast, and answer `skills()` with all 23 entries, and the stale
one is the same 3699040 bytes as the fresh one, so nothing in `module/tests/`
could tell them apart.

Do not hand-edit that file. If CI says the package is stale, the fix is to
rebuild and repackage; editing the hashes is a claim about a build that did not
happen, and the checker's second layer — every string literal in `module/src` of
8 bytes or more must be present in the shipped binary — is there to catch
exactly that.

Locally, and only locally, the same script will also rebuild and compare:

```sh
./scripts/check-package-fresh.py --rebuild build-basecamp
#   ok    a rebuild of the committed source is byte-identical to the
#         darwin-arm64 binary in the package (sha256 595c225721ab998a)
```

That comparison is whole-file, and it holds: rebuilt into a different build
directory, from a source tree at a different absolute path, under a different
`TZ` and a different locale, the plugin comes out at the identical SHA-256. What
it does not survive is a different toolchain — the compiler, the macOS SDK, the
Qt patch level and `nlohmann/json` all reach the bytes and none of them is
pinned by this repository — which is why `--rebuild` is a local command and not
a CI step. It fails loudly when the toolchain is absent rather than passing.

### The 16 MB in it, and why it is committed rather than downloaded

The committed package is `module/agent.lgx` (16 MB at the moment of writing; one
`darwin-arm64` variant). It was 674 KB, and 589 KB before that, and nobody
noticed either change — which is the whole argument for the checked record
above. This one is not an accident and it is not free, so the reasoning is here
rather than in a commit message nobody re-reads.

**What is in it.** `liblogosdelivery.dylib`, 42 MB uncompressed, because the
module opens its own Delivery node (§"The ports a loaded module builds for
itself") and the library has to be somewhere the module can find. Compressed it
is 16 MB; stripped and compressed it is 14.3 MB, and it is deliberately *not*
stripped — a 1.7 MB saving is not worth shipping a modified copy of somebody
else's binary, because "this is what `make liblogosdelivery` produced" is a
property worth keeping.

**Why not fetch it at install time with a pinned checksum**, which is what the
`storage-node` CI job does for `libstorage` and is plainly the better shape:
because there is nothing to fetch. Upstream's releases since `v0.37.0-beta`
carry **zero assets**; its `release-assets.yml` does build a darwin-arm64
`liblogosdelivery` and then uploads it to the GitHub *run artefact store* —
authenticated, expiring, no stable URL — with no release-upload step anywhere in
the file, so even a green run would publish nothing installable; and no run of
it has been green since 2025-10-16. Nimble ships source, there is no Homebrew
formula, the container images are linux executables with a 30-day expiry, and
the one Logos nix cache that really does hold an `aarch64-darwin` build holds it
at version `dev` from a pull-request commit, in a cache that garbage-collects,
signed with a key `flake.nix` does not publish. The full check, with what was
run, is in
[`module/third-party/liblogosdelivery/README.md`](../module/third-party/liblogosdelivery/README.md).
`libstorage` is fetched because `logos-storage/logos-storage-nim` — a different
organisation — publishes a full asset matrix including darwin-arm64. The pattern
is right; the delivery repository does not offer it.

**Licence.** `liblogosdelivery` is **MIT OR Apache-2.0**, © 2025-2026 Logos, so
redistributing the binary inside this package is permitted. It is not free of
obligations: MIT requires the copyright and permission notice to travel with the
copy. `package-basecamp.sh` therefore stages
`module/third-party/liblogosdelivery/` into every package that carries the
library — and **fails** if a library is staged with no licence directory beside
it, because shipping someone else's binary without one is a licensing defect and
not a packaging one. The statically-linked `librln` v2.0.2 is Apache-2.0 OR MIT
(© 2022 Vac Research). The only GPL-family files anywhere upstream are four
Solidity test harnesses under `vendor/waku-rlnv2-contract/lib/`, which are never
compiled and are not linked in.

**What would delete all of this**: one release-upload step upstream, and one
green run of it. Nothing in the module would have to change, because the library
is opened by name at run time rather than linked — where it comes from is the
installer's business.

Check the package against itself rather than trusting this document:

`lgx` is not on `PATH` after building `logos-package` — it stays in that
checkout's `build/`, which is where `package-basecamp.sh` looks for it. Written
out here, because the two lines below used to say `lgx …` and a reader following
them got `command not found: lgx` and no hint that the tool was sitting in a
directory this document had already named:

```sh
LGX="${LGX_BIN:-$HOME/logos/src/logos-package/build/lgx}"
$LGX verify   module/agent.lgx    # contents match the manifest hashes
$LGX manifest module/agent.lgx    # type: core, main: agent_plugin.dylib
```

That prints root hash
`d7fb4646e7de719cc27b2b4afce66e6a45fd35a00e2ebbbd4e3c23f6b99cbfd8`. Rebuilding
the module changes it; none of the checks below depend on the value, and this
line no longer has to be remembered — the same hash is in
`module/agent.lgx.sources`, written by the packaging script and checked by CI,
so a stale copy of it here is now a CI failure rather than a paragraph nobody
re-reads. (This line was stale: it said `cf07408e…`, which is the package as it
stood at `04c9c79` — the one whose Agent Cards this repository's own verifier
rejects. The archive's own sha256 changes on every repackage even when the root
hash does not, because gzip records a timestamp. The root hash is the one that
describes the contents.)

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
ls   # agent_plugin.dylib  liblogosdelivery.dylib  manifest.json  metadata.json
     # third-party/  variant
```

`liblogosdelivery.dylib` belongs in that directory and not somewhere on a
library path: the plugin asks for it by name and looks beside itself first, so
the module directory is where it is found. `third-party/liblogosdelivery/` is
upstream's licence, which travels with the binary because this is
redistribution — see below.

**Leaving it out is survivable, and that is the point.** The plugin does not
link the library; it opens it with `dlopen` when a node is first asked for. A
module directory missing it still loads, still registers all 23 skills, and
answers `meta.status` with the file it wanted and every path it tried:

```json
{"linked":true,"state":"failed",
 "error":"liblogosdelivery.dylib could not be opened. Tried: …/modules/agent/liblogosdelivery.dylib,
          liblogosdelivery.dylib. It belongs in the module directory, beside the plugin"}
```

Linked instead, the same mistake makes the *plugin* fail to load —
`Library not loaded: @rpath/liblogosdelivery.dylib` — which Basecamp reports to
nobody: the tile is inert and the reason goes to stderr. That was measured, both
ways, before the choice was made.

## Running the checks

Harness 2 — install, then Logos Core headless — is also
`./scripts/logos-core-headless.sh <category>`, which does everything in this
section in one command: unpacks `module/agent.lgx` into the user modules
directory, flattens the variant, compiles the SDK translation units and the
harness if they are not already built, and runs it. It additionally passes the
owner and policy account from `artifacts/agents.tsv`, so the module is
configured with the envelope anchored on chain for that agent rather than with
the placeholder below; the harness defaults to the placeholder when those
arguments are absent, so the invocation recorded here still works unchanged.

Harnesses 3 and 4 — the transport ones — have a runner of their own, for the
same reason and with the same shape:

```sh
./scripts/delivery-in-plugin.sh          # 3, and 1 again through QPluginLoader
./scripts/delivery-in-plugin.sh peers    # 4: two loaded modules, two nodes
./scripts/delivery-in-plugin.sh signers  # what the two delegates say, free
./scripts/delivery-in-plugin.sh settle   # 5: discover, serve and pay, one flow
```

`signers` needs nothing but the package and a network for the node. `settle`
needs a funded agent wallet and moves real testnet LEZ, so it is the one to
think before running.

The by-hand versions are kept because they are what the wrappers do, and because
when a wrapper fails they are the way to find out where.

All four harnesses are plain compiles — no CMake target, so they cannot silently
stop being built.

```sh
SDK=$HOME/logos/src/logos-cpp-sdk
QT=$HOME/logos/Qt/6.9.2/macos
APP=/Applications/LogosBasecamp.app
# An array, not a string: zsh does not word-split an unquoted variable, so a
# string here reaches clang as one enormous argument and every Qt header goes
# missing at once. "${QTINC[@]}" is right in both shells.
QTINC=(-F$QT/lib -I$QT/lib/QtCore.framework/Headers
       -I$QT/lib/QtRemoteObjects.framework/Headers
       -I$QT/lib/QtNetwork.framework/Headers)

# 1. the Qt plugin contract
clang++ -std=c++17 -o /tmp/plugin_load_test \
    module/tests/plugin_load_test.cpp $SDK/cpp/logos_types.cpp \
    -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-rpath,$QT/lib
# the artefact, not the build tree — install it first, per the section above
/tmp/plugin_load_test \
    "$HOME/Library/Application Support/Logos/LogosBasecamp/modules/agent/agent_plugin.dylib"
```

Harness 2 needs the SDK, because calling the loaded module is the point of it
and `liblogos_core`'s C API has no "call a method" entry point — a core module
runs in its own process and is reached over the transport. So build the SDK
translation units `LogosModule.cmake` compiles into the module itself, minus
`token_manager.cpp` (which is the trap described below) and plus
`logos_types.cpp` for the `LogosResult` metatype that crosses the wire. Run
`moc` over the headers that declare `Q_OBJECT`, and link the lot:

```sh
mkdir -p /tmp/sdkobj && cd /tmp/sdkobj
for h in logos_api logos_api_client logos_api_consumer logos_api_provider \
         module_proxy qt_provider_object; do
    $QT/libexec/moc $SDK/cpp/$h.h -o moc_$h.cpp -I$SDK/cpp -I$SDK/core
done
# module_proxy.cpp and qt_provider_object.cpp #include their own moc output, so
# `-I.` is what finds it and their moc_*.cpp is not a translation unit of its
# own — compiling it as one is a duplicate-symbol link error. logos_provider_object.h
# declares Q_OBJECT but moc emits nothing for it ("No relevant classes found").
for f in $SDK/cpp/logos_api.cpp $SDK/cpp/logos_api_client.cpp \
         $SDK/cpp/logos_api_consumer.cpp $SDK/cpp/logos_api_provider.cpp \
         $SDK/cpp/module_proxy.cpp $SDK/cpp/logos_provider_object.cpp \
         $SDK/cpp/qt_provider_object.cpp $SDK/cpp/logos_types.cpp \
         moc_logos_api.cpp moc_logos_api_client.cpp \
         moc_logos_api_consumer.cpp moc_logos_api_provider.cpp; do
    clang++ -std=c++17 -c -I. -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include \
        "${QTINC[@]}" "$f" -o "$(basename ${f%.cpp}).o"
done
cd -

# 2. Logos Core, from the installed app
clang++ -std=c++17 -o /tmp/logos_core_load_test \
    module/tests/logos_core_load_test.cpp /tmp/sdkobj/*.o \
    -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-undefined,dynamic_lookup \
    -Wl,-rpath,"$APP/Contents/Frameworks"

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
the runtime resolves. Four traps, and the first three look like a hang or a
permission error rather than a mistake in the build:

- Without a `QCoreApplication` constructed **before** `logos_core_init`, the
  module transport reports `QEventLoop: Cannot be used without QCoreApplication`
  and the load never returns. Basecamp constructs its `QApplication` first; the
  harness does the same.
- Without `LOGOS_HOST_PATH`, the runtime logs `logos_host_qt (or logos_host) not
  found` and `Failed to load module: agent`. Core modules are process-isolated
  and the host binary is what runs them.
- **`token_manager.cpp` is deliberately absent from that list**, and building it
  in is the mistake that costs the most time. `TokenManager::instance()` is a
  singleton, and the capability tokens the harness needs to call the module are
  the ones `liblogos_core` minted into *its* copy. Compile a second one into
  the executable and it wins the symbol, starts empty, and every call comes back
  `ModuleProxy: rejecting unauthorized call to "requestModule" — auth token not
  recognized`, which reads like a permissions problem and is not. Leaving it out
  lets `-Wl,-undefined,dynamic_lookup` resolve the singleton to the runtime's,
  which is what Basecamp gets for free by linking `liblogos_core` rather than
  the SDK.
- That same `dynamic_lookup` is what resolves `LogosTransportFactory` and the
  rest of the transport layer, which the module does not carry either — the
  host provides them. It is the configuration the plugin itself runs in, not a
  shortcut around one.

The persistence directory can be left in place between runs. `configure()` may
be called only once, but only per module *instance*: the flag lives in memory,
and the runtime starts a fresh `logos_host` for the module on every run, so the
second `configure()` of the day is the first one that process has seen. Checked
by running the harness twice against the same `/tmp/agent-persistence` — both
green.

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

Measured again, with the module sitting at the exact path above and Basecamp
0.2.2 launched twice — once with `LOGOS_USER_DIR` pointing at a scratch base and
once against the real one:

```
[info] [logos] Module loaded: capability_module
[info] [logos] Module loaded: package_manager
[info] [logos] Module loaded: package_downloader
Total modules: 3
```

and `grep -ci agent` over the whole of that output returns **0**. That is still
true, and it is still the right thing to expect: an installed core module that
nothing has loaded is not named anywhere, because Basecamp auto-loads only its
own three. It is not evidence that the module is unreachable — it is evidence
that nothing has asked for it yet.

What used to follow this paragraph was the conclusion that the criterion could
not be met, on two grounds. One of them was wrong and the other was
incomplete:

- **"This repository ships no Basecamp `ui` app, so a `core` module has no
  window."** True when written, and it was the whole blocker. `app/` is that
  plugin now; the section below is the record of it loading.
- **"Its Package Manager installs from a configured repository only, so a local
  `.lgx` cannot be installed through the GUI at all."** Still true, and still
  worth stating — but it was never a blocker on *accessibility*, only on the
  install being a click. Both packages are installed by hand here, and a
  reviewer does the same.

## Inside the running app

The same `grep -ci agent`, on a Basecamp launched with **both** packages
installed — `module/agent.lgx` in the modules directory, `app/agent-ui.lgx` in
the plugins directory:

```
$ grep -ci agent /tmp/out.log     # at startup, before anything is clicked
0
$ grep -ci agent /tmp/out.log     # after clicking the sidebar tile once
30
```

Still 0 at startup, and that is correct rather than a shortfall: nothing has
asked for the module yet. The click is what asks. Do not read the number
itself as the result — it counts every line the console's calls produce and
was 62 after an approval round trip in the same run. **0 → nonzero** is the
result, and the lines under it are what it is made of, in order:

```
App launcher clicked: "agent-ui"
Loading UI module: "agent-ui"
Loading core dependency for "agent-ui" : "agent"
[info] [logos] Module loaded: agent
[info] [logos] [agent] [LogosProviderObject] LogosAPIProvider: successfully published "agent"
MainContainer: Added plugin dock to WorkspaceArea: "LP-0008 Agent" (module: "agent-ui" )
Successfully loaded UI module: "agent-ui"
[info] [logos] [agent] ModuleProxy: callRemoteMethod "status" args: 0
```

`Loading core dependency` is the whole mechanism, and it is not a side effect:
Basecamp's PluginLoader reads a `ui` plugin's `dependencies`, calls
`logos_core_load_module` for each, has `capability_module` mint the plugin a
token for it, and only then calls `createWidget(LogosAPI*)`. So declaring
`"dependencies": ["agent"]` in `app/metadata.json` is what turns a click on a
tile into a loaded module. The last line is the console's first call arriving.

The app presents a window in this environment — the previous revision of this
document said it did not, and that was a property of how it had been launched,
not of the app. Both the window and the pane that could not be read before are
now readable without a screenshot, through macOS's accessibility API, which
makes the tile's own label an assertion rather than a description:

```sh
osascript -e 'tell application "System Events" to tell process "LogosBasecamp.bin" \
  to tell window "Logos Basecamp" to get name of every button'
#   LP-0008 Agent, lp-0002-multisig, lp-0003-airdrop, Applications,
#   Package Manager, Settings
```

Before `app/` existed the same command returned that list without its first
entry. That is the criterion's "accessible from the Logos app", read out of the
app itself.

What the window then does — bind the agent, start it, list its 23 skills,
invoke any of them, and answer the spends it asks the owner to approve — is in
`app/README.md`, with the transcript of a completed approval round trip. The
two module-side facts that round trip depends on are in
`docs/limitations.md` §"The owner channel inside Basecamp"; both were found by
measurement and one of them changed `module/src`.

## What still has to be built for the criterion to be met

Honest list, in the order that matters:

1. The skills need their ports wired from inside the loaded module. This item
   has been rewritten twice and is now mostly done. It first read "the plugin
   has to construct and register the skill objects"; it does, and all 23
   dispatch. It then read "`registerBuiltinSkills` takes `std::function` ports
   that cannot cross a plugin boundary", which was the wrong conclusion from a
   right premise — a host cannot pass a closure, and a module can build one. The
   transport ports are now built by the module itself
   (`module/src/delivery_runtime.cpp`), so `messaging.*`, `agent.discover`,
   `agent.task` and `agent.subscribe` work in a loaded plugin. What remains
   needs something in the module's process that no port can supply: a Logos
   Storage node for `storage.*`, a sequencer endpoint for `program.query` and
   `wallet.balance`, and a local `spel` for `program.call` / `program.deploy`.
   Those are three separate pieces of work, not one boundary.

   `agent.task`'s settlement was on that list and is not any more, and the way
   it came off it is worth reading as a pattern rather than as a fix: it did not
   need a wallet **in** the module's process, it needed the module to be able to
   **reach** one. `card_signer` had shown that a loaded plugin can run a command
   and check its answer; `pay_signer` and `policy_source` are the same function
   with a wallet and a sequencer on the other end of them. Two of the three
   remaining items are the same shape and could go the same way.
2. The owner channel over **Logos Messaging** has to be driven from inside the
   loaded module, so that "the owner can interact with the agent in real time
   from a separate Logos app instance using Logos Messaging" is demonstrable.
   This item has moved but is not closed. What the loaded module has now is an
   owner channel built out of the *runtime's* surface rather than Delivery's:
   the module emits `ownerApprovalRequested(requestJson, attempt, timestamp)`
   once per notification attempt, and the owner answers with the module method
   `approveSpend(requestId, verdict)`. Harness 2 exercises both across the
   transport, and the runtime's own log carries the emissions:

   ```
   [logos] [agent] [LogosProviderObject] emitEvent: "ownerApprovalRequested"
   [logos] [agent] [LogosProviderObject] ModuleProxy: forwarding event as Qt signal
   ... eight times, 200 ms apart ...
     ok    an above-threshold spend nobody approved is not submitted by the loaded module
     ok    the notification was retried before the timeout: 8 attempts
     ok    approveSpend is reachable, and refuses a request nobody is waiting on
   ```

   That closes the Reliability half — a spend that does not reach its owner is
   retried, timed out, reported and not executed.

   The last sentence of this item used to read: "It does not close the Usability
   half, which names Logos Messaging and a second app instance specifically, and
   `OwnerChannel` (which does speak Delivery) still needs a `DeliveryPort` the
   plugin cannot be handed." **The transport half is now closed and watched.**
   The plugin builds its own `OwnerChannelPort` from its own node and constructs
   `OwnerChannel` on it (`AgentModuleImpl::publishApprovalOverDelivery`),
   preferring it over the runtime event when `owner_channel_account` and
   `agent_account` are configured and the node is up.
   `./scripts/delivery-in-plugin.sh approval` runs it twice, against
   `module/tests/owner_responder.cpp` on a second node:

   ```
   the owner will approve
     ok  and it is the seed the agent named — two independent derivations
     ok  the owner's approval went out on the channel
     <-  outcome: approved, approved: true, attempts: 1, waited: 472 ms
   the owner will deny
     <-  outcome: denied, approved: false, attempts: 1, waited: 463 ms
     ok  the owner's DENIAL came back as a denial
   ```

   The deny run is the control: a channel that reported "approved" for whatever
   came back would pass the first run and only the first.

   **Two things this cost, both worth recording.** The path could never have
   worked as first written: `OwnerChannel::requestApproval` refuses a request
   with no marker seed *before it sends anything*, and the module was sending an
   empty one, with a comment explaining that the derivation lived in a crate it
   does not link. Every piece was separately tested and the assembly was not.
   `module/src/spend_marker.cpp` derives it now, and
   `module/tests/spend_marker_test.cpp` pins that derivation to
   `crates/agent-policy-core` by *running the crate* rather than by comparing
   against a table. And the first live run still failed, looking exactly like an
   owner who never answered — 23 attempts, no verdict — until the frame counters
   in `meta.status` showed `channel_seen: 1, channel_decoded: 1`: the answer had
   arrived and been read, and it was the *responder* that was wrong, sending
   `{"approve": true}` where `checkReply` reads `decision`.

   Still open: the **second app instance**. The owner end is a program written
   for the purpose, not Basecamp with a person in front of it.

   What *has* since been settled is the other side of that sentence: the class
   the plugin cannot be handed a port for now runs over the public network,
   outside Basecamp. `./scripts/owner-channel-live.sh` puts two processes on two
   Delivery nodes, one running `OwnerChannel` unmodified and one acting as the
   owner from its own node, and completes a correlated approval round trip on
   the owner content topic in **312 ms, on the first attempt**. So the
   transport half of "using Logos Messaging, with no intermediary server" is
   demonstrated and the *host* half is what is left: the owner in that exercise
   is a Delivery node this repository starts, not a second Basecamp. Keep the
   two apart when reading this list — item 2 is a plugin-boundary problem, and
   the live exercise does not make it go away.
   One line of item 2 is now closed inside Basecamp and it is worth separating
   from the rest: the owner really does answer a real spend from a window in the
   app, and the agent acts on the answer. `app/README.md` has the transcript.
   What that does *not* close is the criterion's wording — "from a separate
   Logos app instance using Logos Messaging" — because the owner in that round
   trip is a window in the *same* instance, reached over Logos Core's transport
   and not over Delivery.
3. A `linux-amd64` variant of **both** packages, since a reviewer may be on
   Linux and a package with only `darwin-arm64` is unopenable for them.
4. ~~A Basecamp `ui` app for the owner console.~~ Built: `app/`. It is a Qt
   Widgets plugin implementing `IComponent`, packaged `type: ui`, and it drives
   the loaded module through its published method table — no second
   implementation of anything. `app/README.md` is its build, package and
   install procedure, and `app/tests/ui_plugin_load_test.cpp` is the harness
   that reproduces Basecamp's own PluginLoader and was watched failing against
   two other real Qt plugins before it was believed.
