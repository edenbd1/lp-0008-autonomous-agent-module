# The stack this has to sit in

Written before any code, from the upstream repositories rather than from the
prize text, because the five closed LP-0008 submissions failed on evidence and
integration, not on agent logic.

## What the four moving parts actually are

| Prize wording | Real component | Language | Last touched |
|---|---|---|---|
| Logos Core module | `logos-co/logos-module-loader-qt` | C++ | 2026-08-11 |
| Logos Storage | `logos-co/logos-storage-module` | C++ | 2026-08-13 |
| Logos Messaging | `logos-co/logos-delivery-module` (wraps `liblogosdelivery`) | C++ | 2026-08-08 |
| Headless deploy | `logos-co/logos-logoscore-cli` | C++ | 2026-08-12 |

All four are alive — every one updated within a week of 2026-08-14. That matters:
the API can move under us, so anything we pin needs a recorded revision, the way
`vendor/spel` is pinned in LP-0002 and LP-0003.

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

## Why the previous five were closed

Not for the agent being weak. Quoted from the threads:

- **#99 duongja** — "there is no visible proof of agents activity on the
  testnet"; "the video in application showcases **localnet only**"; "e2e
  integration tests doesn't seem to run at all"; "`demo.sh` doesn't run".
- **#88 retraca** — "Please include a video walkthrough in line with the
  requirements."
- **#81, #85 retraca** — closed the same day on failing submission validation.
- **#34 Beach-Bum** — closed without comment.

Every one of those is an evidence failure. The reviewers check the public
testnet, they check that CI genuinely runs, and they watch the video for the
terminal. That is the part to build first, not last.

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

Green on the default branch, which the prize requires explicitly. Three jobs, all
carrying evidence rather than just compiling:

- the policy primitive and its adversarial tests;
- the committed program hashing to the deployed transaction **and** that
  transaction being live on the public testnet, with a cannot-exist hash as the
  control.

The Linux plugin build is **not** in CI yet, and that is a deliberate choice
between two bad options. It fails on `logos-cpp-sdk`'s own `ScopedQArg`, an
overload GCC 13 rejects and Clang accepts — upstream code, not ours, which is
why the module builds fine on the dev machine under Apple Clang. Switching CI to
Clang did not clear it either.

Marking it `continue-on-error` was never on the table: a job that completes
through a skip path is exactly what closed a competing LP-0003 submission
("the standalone-sequencer E2E did not run in CI; the job completed through its
explicit skip path"). Leaving it red on every commit is the other bad option,
and the prize asks for a green default branch.

So the job is removed until it passes, rather than present and lying. The module
still builds locally, and the command is in `module/CMakeLists.txt`. It comes
back the moment it is green, alongside the standalone-sequencer e2e that this
prize also requires.

Four runs were spent learning one thing, worth writing down: **pin to what you
have proven, not to what is newest.** Following default branches pulled
`logos-qt-sdk`, then `logos-protocol`, then an SDK whose layout had moved so the
builder could no longer find `cpp/logos_api.h`. It is the same failure as
regenerating a `Cargo.lock`, in a different build system.
