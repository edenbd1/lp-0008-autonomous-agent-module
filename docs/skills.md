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
| Storage | `storage.*` | interface defined, C ABI binding not written |
| Messaging | `messaging.*` | interface defined, C ABI binding not written |

The last two rows are the honest part. The module builds and the interface is
real, but nothing here claims Storage or Delivery work until a call to them has
run and been shown. Stubs that return success are worse than an empty row.

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
