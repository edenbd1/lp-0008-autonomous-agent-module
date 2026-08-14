# The skill interface, and what is wired

The prize asks for "a documented skill interface (module/SDK) that can be used
to add new skills without modifying the core agent module". `ISkill` in
`module/src/agent_module_interface.h` is that: a name, a JSON Schema for the
parameters — which is what an A2A Agent Card publishes — and an `invoke` the
plugin wraps, because a failing skill must not take the module down.

`registerSkill` refuses a duplicate name rather than overwriting. One plugin
shadowing another's `wallet.send` is not a hypothetical.

## Status, honestly

| Category | Skill | State |
|---|---|---|
| Blockchain | `wallet.send` | **on chain** — `spend`, enforced by the anchored envelope |
| Blockchain | `program.call` | **on chain** — same path, same threshold |
| Blockchain | `wallet.balance`, `wallet.history` | reads over JSON-RPC |
| Agent | `agent.card`, `agent.discover`, `agent.task` | **demonstrated** by `scripts/a2a-task.sh`, settled on the public testnet |
| Messaging | `messaging.send`, `messaging.join`, `messaging.create_group` | **written against the Delivery API**, compiled; not yet exercised against a running node |
| Storage | `storage.upload`, `download`, `list`, `share` | **written against the Storage API**, compiled; not yet exercised against a running node |

The last two rows are the honest part. The messaging skills are written against
Delivery's real signatures — `send(contentTopic, payload)`, `subscribe`,
`channelCreate` — and they compile, but "compiles" is not "works": nothing here
claims they deliver a message until one has been sent and seen. Both ABIs have now been read off their
module headers rather than guessed.

Two things the messaging code does that a stub would not. It **refuses** when the
node is not started rather than returning success, because `start` returns as
soon as the request is dispatched and completion only arrives later as a
`nodeStarted` event — a skill that sends immediately after `start` is sending
into a node that may not be up. And `create_group` opens a **reliable channel**
rather than a bare topic, since group traffic that silently drops messages is
worse than a group that fails to open.

Storage makes one point of its own. Content addressing means `storage.share` has
nothing to copy and no permission to grant — sharing is *sending someone the
address*, which is a messaging act. So it takes both ports and reports which half
failed, rather than pretending a delivery failure is a storage failure.
`storage.download` asks `exists` first, so an unknown address is reported as
unknown instead of as a download that failed for unstated reasons.

The `DeliveryPort` and `StoragePort` indirections are there so the skills can be
exercised against a fake, and so the agent module does not link either directly.

## What binding them needs

Both are C++ wrappers around C libraries, and both are wired through the `nix`
section of `metadata.json` — `external_libraries` with a `vendor_path`, plus the
extra include dirs and link libraries — the way the migrated Waku example
vendors `waku` from `lib/`.

**Delivery** (Logos Messaging) already documents its contract in its own plugin
header, and it is a lifecycle, not a function call:

- `createNode` once per context, from a JSON config;
- `start` before any message operation;
- `stop` before shutdown;
- `start`/`stop` return once dispatched — completion arrives as `nodeStarted` /
  `nodeStopped` events, so a skill that returns immediately after `start` has not
  waited for anything;
- `entryLayer` chooses how much of the stack mounts: `kernel` (transport only),
  `messaging`, or `channels` (reliable channels, the default).

That lifecycle is what the owner channel needs, and it is also what the A2A
transport binding needs, since Logos Messaging is what replaces A2A's HTTP.

**Storage** exposes upload, download, list and share. Its wrapper is the larger
of the two (~200 MB checked out) and its ABI has not been read yet.

## Why the on-chain half came first

Every closed LP-0008 submission fell on evidence rather than on features — no
visible testnet activity, a video showing localnet, e2e that never ran. Skills
that cannot be checked from outside the repository do not answer that. A spend
that the chain refused above the threshold does.

## Running a Delivery node, without Nix

`logos-delivery-module`'s `metadata.json` declares two `external_libraries`,
`logosdelivery` and `rln`, and vendors neither. An earlier version of this
document concluded from that — plus "`nix` is not installed here" — that a
running node needed a Nix install, and listed three blocked paths. That was
wrong, and wrong in an instructive way: it enumerated ways to *obtain* the
libraries without ever asking where they are *built*.

They are built from source, with no privileged step:

- `liblogosdelivery` is an ordinary Nim project at
  [`logos-messaging/logos-delivery`](https://github.com/logos-messaging/logos-delivery)
  with a `liblogosdelivery` make target. Two submodules, shallow-clonable.
- `librln` is an ordinary cargo build of `vendor/zerokit`, driven by that same
  Makefile. The flake warns that zerokit v2.0.2 ships a stale `cargoHash`, but
  `cargoHash` is a Nix concept and does not apply to a plain `cargo build`.

```
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/logos-messaging/logos-delivery _external/logos-delivery
cd _external/logos-delivery
export PATH="$HOME/.nimble/bin:$PATH"     # `make` installs nimble here and does
make liblogosdelivery                     # not put it on PATH itself
```

The one snag is that last point: the build installs Nim and nimble into `~/.nim`
and `~/.nimble`, then fails at `nimble setup` because nimble is not on `PATH`.
Exporting it is the whole fix.

That produces `build/liblogosdelivery.dylib` (42 MB) and `librln_v2.0.2.a`.
`scripts/exercise-nodes.sh` builds `module/tests/delivery_node_drive.c` against
them and runs it.

### What the run proves

Every step in the driver is an assertion and the exit code is the result. The
shipped C example prints what each call returned and exits 0 either way, which
is fine for a tutorial and useless as evidence — a node that never started would
produce a transcript that reads like success.

A green run creates a node, registers listeners, starts it and waits for the
node to confirm rather than for `start_node` to return, asks the running node
for its own peer id, subscribes, publishes, waits for the network to propagate
the message back, then stops and destroys the context.

One trap is worth recording, because it fails silently and looks exactly like a
message that never left: **the name you register with is not the name that comes
back.** You subscribe to `onMessageSent`, and the event payload carries
`"eventType":"message_sent"`. Matching the registration name never fires.

### Why this is not in CI

Deliberately. Building the library from scratch takes tens of minutes — it
builds the Nim compiler first — and the run itself needs live peers on a public
network, so a green result depends on someone else's uptime.

A job like that goes amber on a bad afternoon, and an amber job teaches everyone
to ignore it. The e2e sequencer job in CI is different: it talks to a service
this project can reason about, and it fails for reasons that are ours.

So this one is a local command whose output is quoted here, and CI checks the
things it can check honestly. A skipped or perpetually-flaky CI step counts as
not run, which is the standard applied to everything else in this repository.

### Storage

Same shape: `libstorage` comes from
[`logos-storage/logos-storage-nim`](https://github.com/logos-storage/logos-storage-nim)
at `v0.4.4`, also Nim, also buildable from source. It is the larger of the two
(247 MB checked out).
