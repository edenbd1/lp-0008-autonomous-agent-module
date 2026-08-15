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
of the two, and its ABI has now been read off `library/libstorage.h` and driven
against a running node — see "Storage" below.

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
- `librln` comes from `scripts/build_rln.sh`, driven by that same Makefile. It
  **downloads a prebuilt release asset** for the host triple
  (`aarch64-apple-darwin-stateless-rln.tar.gz`) and only falls back to building
  `vendor/zerokit` with cargo. That is what happened here: `librln_v2.0.2.a`
  carries the release artifact's own mtime and `vendor/zerokit/target` was
  never created, so no cargo build ran. An earlier version of this document
  claimed the cargo path as fact; it was not checked.

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
shipped C example fails if `create_node` fails and then prints what every later
call returned without checking it, which is fine for a tutorial and useless as
evidence — a node that started and then did nothing would produce a transcript
that reads like success.

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

So this one is a local command, and CI checks the things it can check honestly.
A skipped or perpetually-flaky CI step counts as not run, which is the standard
applied to everything else in this repository.

A green run looks like this:

```
[3/4] run it against the live network
  ok    the node started
  <-    MyPeerId: 16Uiu2HAmFn4KGMY9anuPCzHophueE7UKp3D6v2GhMzYZypYjSb4v
  ok    it reported a peer id
  event {"eventType":"message_propagated","messageHash":"0xf5c1ef97…"}
  ok    the message reached the network, and stayed sent
[4/4] the same, for Storage
  <-    peer id: 16Uiu2HAmT5DmYZ6DPvvZu7cBKHezwRsU4YrkeY9mcworqLwwRbBo
  <-    cid: zDvZRwzm6L91tfhtuGMSSt8ESiFX9eoHvWKfk2WczzaJCRUcUX4V
  ok    the content address is in the store
  <-    manifest: {"manifestVersion":0,…,"filename":"lp0008-upload.txt"}
  ok    its manifest names the file we uploaded
all steps confirmed (0 failure(s))
```

The peer ids differ on every run — a node generates a fresh identity per start,
so a transcript that reproduced them exactly would be the suspicious one.

### Storage

Same shape, same answer: `libstorage` comes from
[`logos-storage/logos-storage-nim`](https://github.com/logos-storage/logos-storage-nim)
at `v0.4.4`, also Nim, also buildable from source with no privileged step.

```
git clone --depth 1 --recurse-submodules --shallow-submodules -b v0.4.4 \
    https://github.com/logos-storage/logos-storage-nim _external/logos-storage-nim
cd _external/logos-storage-nim
export PATH="$HOME/.nimble/bin:$PATH"
make libstorage
```

It is much the larger of the two — **3.1 GB** checked out with its submodules
and build artefacts, and it builds the Nim compiler and LevelDB on the way —
and produces `build/libstorage.dylib` (21 MB).

Upload is a session rather than a single call: `storage_upload_init` on a path
returns a session id, and `storage_upload_file` on that session returns the
content address. The assertion that carries weight is not the return code but
the manifest — `storage_download_manifest` on the returned CID must name the
file that went in. A stub can return `RET_OK`; it cannot return a content
address that resolves back to the right filename and byte count.
