# examples

Two things live here, and neither is part of the agent module.

```
agent-console/           a command-line front end for the module: `skills`,
                         `status`, `invoke <name> <json>`, and a self-test
skills/notary-digest/    a third-party skill, `notary.digest`, compiled on its
                         own against one header and loaded at runtime
```

## Run it

```sh
examples/agent-console/run.sh
```

A C++17 compiler, a checkout of
[`logos-cpp-sdk`](https://github.com/logos-co/logos-cpp-sdk) (two Qt-free
headers are reached; README §9 pins the revision CI uses), and
`nlohmann/json.hpp` — which the *module* needs, not the skill. No node, no keys,
no network: the self-test runs `--offline`.

The script builds the skill, builds the console, loads one into the other, calls
the skill through the module's own dispatcher, and then checks four things that
a transcript cannot fake:

- the returned digest equals `shasum -a 256` of the same input;
- and does **not** equal the digest of altered input, so the comparison is not
  vacuous;
- `git status --porcelain module/` is unchanged by the run — no file the agent
  module ships was edited, which is the usability criterion's own wording;
- the reference table in `docs/skills.md` §7 still lists exactly the skills the
  module registers.

Each is an exit condition. A red run names which one failed.

## What each is for

**`skills/notary-digest`** is the evidence for "a documented skill interface
(module/SDK) that can be used to add new skills without modifying the core agent
module". It includes exactly one header —
`module/src/agent_module_interface.h` — and deliberately not the plugin header,
not the Logos SDK, not Qt and not a JSON library, so that "the interface is one
header and `std::string`" is a compile-time fact rather than a claim. It is
compiled with `-Werror` into its own shared library, with no module object on
the link line.

**`agent-console`** is the evidence for "interacting with the agent via CLI". It
links the module and wires the ports a command-line tool can honestly supply —
the sequencer's JSON-RPC — so `program.query` and `wallet.balance` really
answer, from the chain. Its `WalletPort::spend` is null on purpose: **the
console cannot move money.** Spending goes through the anchored policy program
and the CLIs that hold keys, `scripts/deploy-agents.sh` and `scripts/a2a-task.sh`.

## What this is not

It is not Logos Core, and a skill dropped next to an installed `agent.lgx` does
nothing. `registerSkill` takes a `std::shared_ptr<ISkill>`, which has no wire
format, so skills are added by a host that *links* the module. The boundary, why
it is there, and what would move it are in
[`docs/skills.md` §6](../docs/skills.md).

The full interface specification — the contract, the guarantees, the loader
convention, and a reference for every registered skill — is
[`docs/skills.md`](../docs/skills.md). This file does not restate it; two
accounts of one interface is how they come to disagree.
