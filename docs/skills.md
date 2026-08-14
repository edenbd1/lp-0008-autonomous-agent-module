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

## What actually blocks a running node

Checked rather than assumed, by cloning the module and reading its manifest.

`logos-delivery-module`'s `metadata.json` declares two `external_libraries`:
`logosdelivery` and `rln`. Neither is vendored in the repository — they are
supplied by the Nix flake, and `nix` is not installed here. The C++ wrapper is
small (1.8 MB checked out) and would build in minutes; the C libraries under it
are the blocker. `logos-storage-module` is the same shape and considerably
larger.

So the gap between "the skills compile and their behaviour is tested" and "a
message went out" is not more code. It is one of:

- install Nix and build both modules through their flakes, which is the path
  upstream supports;
- obtain prebuilt `liblogosdelivery` and `librln` and vendor them under `lib/`,
  the way the migrated Waku example vendors `waku`;
- run the modules from a Logos Core distribution that already ships them, and
  drive them over Qt Remote Objects rather than linking.

All three were checked on this machine, not reasoned about:

- **Nix** — not installed.
- **Vendored libraries** — `lib/` does not exist in either module's repository.
- **A Logos Core distribution** — `LogosBasecamp.app` 0.2.2 ships exactly one
  module, `capability_module`. No delivery, no storage. The two matching dylibs
  in the bundle are Qt's own (`libqmldbg_messages`, `libqmllocalstorageplugin`),
  not Logos libraries.

So the shortest path is the first: install Nix, build both modules through their
flakes, and point the skills at the running nodes. That is a setup step, not a
design question — the ports are already there and their behaviour is already
tested. `scripts/exercise-nodes.sh` records what that run has to do and exits
non-zero until it does it.
