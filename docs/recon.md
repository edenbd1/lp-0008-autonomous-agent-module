# The stack this has to sit in

Written before any code, from the upstream repositories rather than from the
prize text. What is hard here is not agent logic; it is integration, and proving
the integration to somebody who was not watching.

## What the four moving parts actually are

| Prize wording | Real component | Language | Last touched |
|---|---|---|---|
| Logos Core module | `logos-co/logos-module-loader-qt` | C++ | 2026-08-11 |
| Logos Storage | `logos-co/logos-storage-module` | C++ | 2026-08-13 |
| Logos Messaging | `logos-co/logos-delivery-module` (wraps `liblogosdelivery`) | C++ | 2026-08-08 |
| Headless deploy | `logos-co/logos-logoscore-cli` | C++ | 2026-08-12 |

All four are alive — every one updated within a week of 2026-08-14. That matters:
the API can move under us, so anything we pin needs a recorded revision, the way
`vendor/spel` is pinned here.

## The module contract

A Logos Core module is a plugin pair plus a handler:

```
src/<name>_module_plugin.h      class <Name>ModuleImpl : public LogosModuleContext
src/<name>_module_plugin.cpp
src/api_call_handler.h
```

- Includes `logos_module_context.h` and `logos_result.h`.
- Asynchronous events are declared in `logos_events:` sections; codegen emits the
  bodies, which route through `LogosModuleContext::emitEventImpl_`.
- Messaging has a lifecycle to respect: `createNode` once per context, `start`
  before any message operation, `stop` before shutdown. `start`/`stop` are
  dispatch-and-return; completion arrives as `nodeStarted` / `nodeStopped`
  events.
- `entryLayer` selects how much of the delivery stack is mounted: `kernel`
  (transport only), `messaging` (kernel + client), `channels` (adds reliable
  channels, the default).
- Content topics follow <https://lip.logos.co/messaging/informational/23/topics.html>.

## What has to be true at the end, and therefore first

A working agent that nobody can check is indistinguishable from one that does
not work. Four things decide whether this is checkable at all, and none of them
is agent logic:

- **visible activity on the public testnet** — transactions a reader can look up
  themselves, not a local chain that disappears with the process;
- **CI that genuinely runs** — a job that skips, or that compiles without
  asserting anything, is a green tick over an untested claim;
- **a demo that runs from a clean clone** — with no funded account, no keys and
  no local sequencer, because that is what a reader has;
- **a video against the public testnet** — the terminal on screen, not a
  reconstruction.

Every one of those is expensive and none of them can be added at the end. So
they are built first, and the agent is built into them.

## The two hardest criteria

Everything else is work; these two are the ones that decide it.

1. **Three agents deployed on LEZ testnet**, one per skill category (Storage,
   Messaging, Blockchain), each with reproducible deployment and evidence.
2. **Two agents discover each other via Agent Cards, run an A2A task lifecycle,
   and transfer LEZ payment autonomously** — no owner in the loop.

Both are on-chain, publicly checkable, and neither can be faked with a localnet
recording. They set the shape of the whole build.

## Building a module without Nix

The blessed path is `logos-module-builder`, a Nix flake library. Nix is not
installed here, and it does not have to be: the generated `CMakeLists.txt` looks
for the helper in three places, and the environment variable comes first.

```cmake
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    ...
```

So pointing `LOGOS_MODULE_BUILDER_ROOT` at a checkout of the builder makes plain
CMake work. Confirmed available locally: cmake 4.1.2, qmake 3.1, Qt via
Homebrew. No ninja, no nix.

### The four files a module is

```
metadata.json     name, version, type "core", category, main plugin, deps
CMakeLists.txt    include LogosModule.cmake, then logos_module(NAME .. SOURCES ..)
src/<n>_interface.h
src/<n>_plugin.h      class <N>ModuleImpl : public LogosModuleContext
src/<n>_plugin.cpp
```

`metadata.json` also carries a `nix` section — build/runtime packages, external
libraries with a `vendor_path`, and extra CMake include dirs and link libraries.
That is where Logos Storage and Logos Delivery get wired in, the way the
migrated Waku example vendors `waku` from `lib/`.

### Where this leaves the risk

The unknown is no longer "can we build a module" — it is which of Storage's and
Delivery's C ABIs we have to drive, and their lifecycles. Delivery's is already
documented in its plugin header: `createNode` once, `start` before any message
operation, completion by event, `entryLayer` choosing how much of the stack
mounts.

## CI status

**This section is the oldest thing in this repository and it has been rewritten
rather than left, because what it said about CI stopped being true.** It was
written before any code; the paragraphs below are what CI is now, and the
retraction under them is what it used to say.

`.github/workflows/ci.yml` runs **seven** jobs, all carrying evidence rather
than just compiling: the policy primitive and its adversarial tests (`rust`);
the committed program hashing to the deployed transaction **and** that
transaction being live on the public testnet, with a cannot-exist hash as the
control (`binaries`); the C++ suites against fake ports (`skills`); the shipped
`.lgx` against the source committed beside it (`package`); a real Logos Storage
node (`storage-node`); Logos Core loading, configuring and starting the
committed module headless on Linux (`linux-headless`); and the illustrative use
cases against the public testnet (`use-cases`).
`.github/workflows/e2e-local-sequencer.yml` is the second workflow.

**Retracted: "the Linux plugin build is not in CI, and the job is removed until
it passes."** That was written about a *compile* that failed on
`logos-cpp-sdk`'s own `ScopedQArg`, an overload GCC 13 rejects and Clang
accepts — upstream code, not ours, which is why the module built fine on the dev
machine under Apple Clang, and switching CI to Clang did not clear it either.
That compile is still not in CI and the reason is still in `ci.yml`'s own
comments. What was wrong is the conclusion drawn from it, which was that CI
covers nothing on Linux. It covers the thing that matters more: `linux-headless`
fetches the published Logos Basecamp AppImage on `ubuntu-latest`, unpacks it
with no installer and no display, and runs
`./scripts/logos-core-headless.sh storage` against the **committed**
`module/agent.lgx` — the file a reviewer downloads — with three negative
controls under it. A Linux binary that loads out of the shipped package is a
stronger claim than a Linux binary that compiles, and it was reachable without
the compile ever going green.

Marking anything `continue-on-error` was never on the table: a job that completes
through a skip path reports green without having run, which is worse than red
because nobody looks at it again. The standalone-sequencer e2e this prize also
requires is present too, as the second workflow, and it has no skip path for the
same reason.

Four runs were spent learning one thing, worth writing down: **pin to what you
have proven, not to what is newest.** Following default branches pulled
`logos-qt-sdk`, then `logos-protocol`, then an SDK whose layout had moved so the
builder could no longer find `cpp/logos_api.h`. It is the same failure as
regenerating a `Cargo.lock`, in a different build system.
