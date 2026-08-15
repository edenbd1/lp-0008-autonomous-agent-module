# Solution: LP-0008 — Autonomous AI Module with Wallet, Storage, and Messaging

> ## ⚠️ DRAFT — NOT READY FOR SUBMISSION
>
> This document is written against the repository as of commit `3aa05c7`
> (2026-08-15). It is **not** submittable yet. Two things must land first:
>
> 1. **A task settlement that actually transfers tokens, twice.** Today the
>    on-chain settlement is a real privacy-preserving proof that an anchored
>    policy *authorises* a payment. It does not move tokens. A fix — the policy
>    program chaining a call into LEZ's `authenticated_transfer`, which owns the
>    agents' accounts and may move their balances — is implemented and deployed
>    (`b028eabf…`, block 8590), but **no balance-moving settlement has landed on
>    the public testnet**. The bar before this document goes out is two
>    consecutive confirmed settlements whose recipient balance change is read
>    back from the chain, so that the result is reproducible rather than a
>    one-off.
> 2. **A recorded video demo.** Not yet recorded. Placeholder below.
>
> A third item is mechanical but blocking: the work described here is on a local
> branch **38 commits ahead of `origin/main`**. Until it is pushed, a reviewer
> cloning the repository will not see it. Every claim below assumes the push has
> happened; none of it is true of the public repository today.
>
> The Success Criteria Checklist marks unmet criteria as **UNMET**, including
> criteria for which code exists. Code existing is not the criterion.

**Submitted by:** edenbd1

## Summary

An agent that participates in the Logos stack directly rather than through an
API key: it holds its own shielded LEZ account, and the limit on what it may
spend is not a check inside the agent process but **an account address on
chain**.

The core idea is one design decision. An agent runs unattended on a remote node
and holds its own signing key, so any spending rule the agent evaluates can be
evaluated differently by whoever holds the process. So the rule is not evaluated
by the agent at all. The policy account's address is the PDA seed
`SHA256(owner ‖ agent ‖ per_tx ‖ per_period ‖ period_blocks)`. Raising a limit
does not edit an account — it *names a different account*, one that
`create_policy` never initialised, so the spend is rejected by the state machine
before the program body runs. The ceiling is enforced by arithmetic on an
address, not by trust in a process.

Three agents — one per default skill category — are anchored on the public LEZ
testnet, each with its own shielded account and its own envelope. Agent-to-agent
coordination is A2A-shaped: cards carry the A2A schema plus an `x-logos`
extension for the price and payment address that vanilla A2A has no field for.

**What is not delivered** is stated in the checklist and in
[`docs/limitations.md`](docs/limitations.md), and it is substantial: the
settlement does not yet move tokens, there is no owner channel, the Logos Core
module has never been loaded by Logos Core, and 13 of the 21 default skills have
no implementation.

## Repository

- **Repo:** <https://github.com/edenbd1/lp-0008-autonomous-agent-module>
- **License:** dual MIT / Apache-2.0
- **Default branch:** `main`

Everything asserted below is verifiable from a clean clone plus the public
sequencer. No claim in this document depends on trusting the author.

### The one command that checks the rest

```bash
./scripts/demo.sh
```

It recomputes the deployed program's hash from the committed binary, asks the
public sequencer whether that transaction exists, and asks the same sequencer
about a hash that cannot exist. Without that control the first question proves
nothing — an RPC that answered non-null to everything would pass it just as
happily.

## Approach

### The threshold is an address, not an `if`

The alternative considered first, and rejected, was the obvious one: hold the
limit in the module's configuration and check it before signing. It is rejected
by the deployment model the prize itself describes. The agent runs on a remote
node with its own keys; an attacker who takes the process takes the spending
decision with it. A limit that lives in the process is advisory.

So `crates/agent-verifier-spel` derives the policy account's address from the
envelope. `create_policy` is declared `#[account(init, pda = arg("policy_hash"))]`
and `init` will not overwrite, which makes anchoring single-use: an envelope,
once anchored, cannot be widened. A wider envelope is a different address that
nobody has initialised.

The cost of this choice is real and is recorded rather than hidden: because the
policy account carries **no data**, nothing on chain accumulates
`spent_this_period`. It is supplied by the caller. An agent that always passes
zero has a per-transaction limit and no period limit. Fixing that means the
policy account becoming mutable state, which forfeits the property that its
address alone encodes the envelope — a genuine trade, so it is named in
`docs/limitations.md` rather than half-made.

A second known gap in the same file: `spend` re-derives the policy hash from
`(owner_id, agent_id, limits)` but does not check that the account *signing* as
`agent` is the account `agent_id` names. A funded account can therefore present
any anchored policy, including a more generous one anchored for someone else. It
can still only spend its own money, but per-agent limits are not per-agent under
an adversary who reads the chain. The fix is one comparison and a re-anchor of
every policy.

### What did not work, and what the failures taught

These are the ones worth recording, because each cost a working session and each
changed the design.

**A program may not debit an account it does not own** (LEZ rule 5,
`UnauthorizedBalanceDecrease`). The agents' accounts are owned by LEZ's
`authenticated_transfer` program — measured with `wallet account get`, not
assumed. So the first `spend`, which returned the agent's account with a reduced
balance, could never have been accepted however the accounts were declared. The
program now gates the payment and **chains a call** into the transfer program,
which does own the accounts. That is what `b028eabf…` is, and it is why
`scripts/a2a-task.sh` passes `--bin-auth-transfer`: the privacy circuit composes
the inner call inside the proof and looks the callee up by ImageID, so it must be
handed that program's ELF or the build stops at `UndeclaredProgramDependency`.

**A confirmed transaction is not a payment.** An earlier `spend` returned its
accounts untouched with an empty chained-call vector. It produced real,
confirmed, on-chain privacy-preserving proofs — `aea80817…` (block 7506) and
`7f5a506b…` (block 8016) are both still there — that a policy *permitted* 25
LEZ, and moved nothing at all. It was written up here as "the settlement" before
being caught. Both `scripts/a2a-task.sh` and `scripts/e2e-local-sequencer.sh`
now read the recipient's balance off the chain before and after and refuse to
record a payment unless the delta equals the price. That is why this document
marks the settlement criteria unmet despite two settlements being on chain.

**`create_policy` needs a signer some program already owns.** A fresh signer
still carrying the default program owner anchors exactly once; on its second
attempt the sequencer rejects with
`InvalidProgramBehavior(DeclaredAccountMissingFromOutput)`. The path is indirect:
LEZ rule 7 rejects a post-state that keeps the default owner once the pre-state
is no longer default (a signer's nonce goes 0→1 on its first transaction), but
the vendored SPEL macro filters the signer out of the output first to dodge rule
7 — and `create_policy` still *declares* it, so the state machine rejects it for
being declared and missing instead. Removing the `owner` declaration clears the
rejection and was rejected as a fix: `spel` signs only for declared signers, so
an instruction with no signer produces an empty witness set and `create_policy`
becomes permissionless — at which point any agent could anchor itself an
unlimited envelope and the ceiling means nothing. The requirement stands and is
documented: anchor with a signer that has already received a transfer.
`DumJ4LCBnHE9jUu2yxPfqdL14g3v756Gzby6LuT9hE51` anchored two policies at nonces
29 and 30, in consecutive blocks 8050 and 8051, which is the evidence for it.

**A shielded agent can pay but cannot be paid at its shielded account.** `spel`
resolves `Private/<id>` only for accounts the *signing* wallet holds keys for —
a private account's state cannot be constructed without its viewing key — so one
agent naming another's private account as recipient fails with `KeyNotFoundError`
before anything is built. Two ways out were considered. Publishing the payee's
`npk`/`vpk` in the Agent Card's `x-logos` block is the right one and keeps both
sides shielded, but it needs `spel` to build a `PrivateForeign` recipient, a kind
the wallet already has (`lez/wallet/src/account_manager.rs:30-34`) and the CLI
does not expose. Giving each agent a **public receiving account** works today.
The second was taken, and the honest description of the trade is that the payer
stays shielded while the amount and recipient of each task payment are public —
which does not fully satisfy "indistinguishable on-chain from any other account
holder". Choosing it silently would have been the kind of trade that reads as met
and is not.

### Why the Logos stack

The property that matters here is not privacy in the abstract; it is that **the
spending ceiling is enforced by a party the agent cannot bribe, restart, or
impersonate.** On a centralised platform the agent's limit is a row in the
provider's database, and the provider is also the party running the agent, so
the limit and the thing it constrains share a trust domain. On LEZ the limit is
an account address in a state machine that neither the agent nor its owner
operates. That is the whole argument for building this here.

The privacy half is what makes the arrangement usable rather than merely safe:
an agent that must publish every payment it makes leaks its owner's strategy to
anyone watching, and a shielded account with an anchored public ceiling gives the
inverse — the envelope is public and auditable, the payments inside it are not.
Logos Messaging is the transport A2A deliberately leaves open, and it matters for
the same reason: agent-to-agent coordination over a broker is coordination the
broker can censor, and A2A's own specification has no answer for that.

What is lost on a centralised alternative is therefore specific: the ceiling
stops being verifiable by a third party, and the discovery topic acquires an
owner.

## Success Criteria Checklist

Marked against the prize's criteria verbatim. **UNMET** means the criterion is
not satisfied today, whether or not code toward it exists.

### Functionality

- [ ] **UNMET — Module loads and runs inside Logos Core alongside wallet,
  storage and messaging modules.** The module builds as a Qt plugin
  (`module/CMakeLists.txt`, producing `agent_plugin.dylib`) and declares
  `module/metadata.json`, but it has **never been loaded by Logos Core**. No
  load has been observed or recorded, so this is unmet. The storage and
  messaging skills are written against the real Delivery and Storage ABIs, read
  off `liblogosdelivery.h` and `library/libstorage.h`, and both libraries have
  been driven for real by `scripts/exercise-nodes.sh` — but that is a standalone
  driver, not Logos Core loading the module.

- [ ] **UNMET — The agent has its own shielded LEZ account and can send and
  receive tokens independently of the owner's wallet.** The first half holds:
  each agent has its own shielded account and its own persistent wallet home
  outside the repository, and each signs for itself. The second half does not.
  Sending has not been shown to move tokens on the public testnet (see the
  settlement criteria below), and the agent cannot *receive* at its shielded
  account at all — `spel` cannot resolve a private recipient it holds no keys
  for, so each agent is paid at a **public** receiving account instead.

- [ ] **UNMET — Single CLI command deploys and configures the agent on any
  machine using Logos Core headless.** `SIGNER=<id> ./scripts/deploy-agents.sh`
  is a single command and does deploy and anchor three agents, but it drives the
  LEZ `wallet` and `spel` binaries. It does not use Logos Core headless, which is
  what the criterion asks for.

- [ ] **UNMET — Owner interacts in real time from a separate Logos app instance
  over Logos Messaging with no intermediary server.** There is no owner channel.
  `messaging_skills.h` derives an owner content topic from a LEZ account and the
  `send`/`join`/`create_group` skills are implemented against Delivery's real
  signatures, but nothing wires them into an owner conversation, and no owner has
  ever talked to an agent. Not claimed.

- [ ] **UNMET — Spending threshold holds above-threshold transactions for owner
  approval and executes below-threshold ones autonomously.** The on-chain half is
  real and testable: the program has `spend` (autonomous, no approval account),
  `approve_spend` and `spend_approved`, and `scripts/e2e-local-sequencer.sh`
  asserts both directions against a real standalone sequencer with
  `RISC0_DEV_MODE=0` — a payment inside the envelope lands, a payment outside it
  is refused with no approval present and no balance moved. Unmet as stated
  because (a) the "holds for owner approval" half has no owner channel to hold it
  *on*, and (b) `spent_this_period` is supplied by the caller, so the per-period
  ceiling is not enforced by the chain.

- [ ] **UNMET — All default skills are implemented and documented.** **8 of 21**
  are implemented behind the `ISkill` interface: `storage.upload`,
  `storage.download`, `storage.list`, `storage.share`, `messaging.send`,
  `messaging.join`, `messaging.create_group`, `meta.skills`. The remaining
  **13 have no implementation**: `wallet.balance`, `wallet.send`,
  `wallet.history`, `program.query`, `program.call`, `program.deploy`,
  `agent.card`, `agent.discover`, `agent.task`, `agent.subscribe`,
  `agent.cancel`, `meta.status`, `meta.configure`. Two qualifications, both
  against the claim rather than for it: `program.query/call/deploy` exist as
  declarations in `module/src/program_skills.h` with **no `.cpp` and no entry in
  `module/CMakeLists.txt`** — they do not compile into the module;
  `wallet.send` and `agent.card/discover/task` exist as *behaviour in shell
  scripts* (`spend` in `scripts/a2a-task.sh`, cards in `artifacts/agent-cards/`),
  not as registered skills. A reviewer can reproduce this count with
  `grep -rn '<skill-name>' module/src crates scripts`.

- [ ] **UNMET — A2A compatibility: cards follow the A2A schema, tasks follow the
  A2A lifecycle, documented as an A2A transport binding over Logos Messaging.**
  A card is generated and committed (`artifacts/agent-cards/storage.json`,
  `protocolVersion 0.3.0`) with an `x-logos` extension carrying the LEZ price and
  payment account. The lifecycle is not: `scripts/a2a-task.sh` prints
  `submitted → working → completed` as a narration of the flow, it does not
  implement the A2A task state machine, and there is no transport binding — no
  A2A message ever crosses Logos Messaging. Claiming the lifecycle on the
  strength of three `printf`s would be exactly the overclaim this document is
  trying to avoid.

- [ ] **UNMET — Two agents discover each other, execute a task, and transfer LEZ
  payment autonomously.** This is the blocker named at the top. Two settlements
  are confirmed on the public testnet — `aea80817…d98449e7` (block 7506, 270,566
  bytes) and `7f5a506b…6ab87319` (block 8016, 270,278 bytes) — and both are
  genuine privacy-preserving proofs signed by the client agent's own key with no
  owner in the loop. **Neither transferred tokens.** They prove the anchored
  policy authorised 25 LEZ. The chaining fix is deployed (`b028eabf…`, block
  8590) and `scripts/a2a-task.sh` now refuses to write its manifest unless the
  recipient's on-chain balance moved by exactly the price, but no balance-moving
  settlement has landed on the public testnet yet.

- [ ] **UNMET — At least 3 illustrative use cases demonstrated end-to-end on LEZ
  testnet.** None are demonstrated end-to-end. The paid-skill-marketplace flow is
  the closest and is short by the transfer.

- [ ] **UNMET — Three separate agents deployed on LEZ testnet, one per default
  skill category, each with a reproducible deployment and evidence.** Three
  anchors are live and independently verifiable (table below), but the criterion
  says *reproducible*, and today it is not: those three were anchored under
  program `6e4a2000…f365321a` (block 8034), whose binary is **no longer the one
  in the repository** — `artifacts/programs/agent_verifier.bin` now hashes to
  `b028eabf…`. Under the program the repository currently ships, only the
  **storage** agent is anchored (`ab017c9c…`, block 8591). A policy account is a
  PDA of the program, so redeploying the program moves every policy to an address
  nobody has initialised; re-anchoring messaging and blockchain under `b028eabf…`
  is part of the same pending work as the transfer.

- [ ] **UNMET — Full documentation and a clean public repository.**
  `docs/DEPLOYMENT.md`, `docs/limitations.md`, `docs/skills.md` and
  `docs/recon.md` exist and are current with the local tree. The repository is
  public and dual-licensed. Unmet because the work is **not pushed** — `main` is
  38 commits ahead of `origin/main` — and because `docs/skills.md` has
  historically overstated skill coverage; its status table needs to be reconciled
  against the 8-of-21 count above before submission.

### Usability

- [x] **MET — A documented skill interface that adds skills without modifying
  the core module.** `logos::agent::ISkill` in
  `module/src/agent_module_interface.h` is three virtuals — `name()`,
  `parameterSchema()` (JSON Schema, which is what an A2A card publishes), and
  `invoke()`. `AgentModuleImpl::registerSkill` takes any implementation and
  **refuses a duplicate name rather than overwriting**, so one plugin cannot
  shadow another's `wallet.send`. `meta.skills()` enumerates what is registered.
  Documented in `docs/skills.md` and in the header. Exercised in
  `module/tests/skills_test.cpp` against fake `DeliveryPort` / `StoragePort`
  seams, compiled and run in CI on every push.

- [ ] **UNMET — Owner-facing interface accessible from the Logos app (Basecamp)
  via the owner channel, with local build instructions and loadable assets.** No
  Basecamp integration exists. Not claimed.

### Reliability

- [ ] **UNMET — Recovers from transient failures without losing pending task
  state.** There is no pending-task state to lose: nothing is persisted across a
  restart. The scripts are careful about transient chain failures — twelve-block
  confirmation windows, resync-before-prove, refusal to record an unconfirmed
  hash — but that is not the same claim.

- [ ] **UNMET — Above-threshold transactions that fail to reach the owner are not
  executed; the agent retries notification before timing out and reports the
  failure.** The *chain* enforces the first half unconditionally: without an
  approval account seeded by the exact payment, an above-threshold `spend` is
  refused, which `scripts/e2e-local-sequencer.sh` asserts. The notification,
  retry and timeout do not exist. `SpendOutcome::OwnerUnreachable` is an enum
  value with no code behind it.

- [ ] **UNMET — Skill failures are isolated.** The interface documents the
  contract and `skills_test.cpp` checks that malformed JSON and missing fields
  are *refused rather than thrown*, per skill. But `AgentModuleImpl` has no
  invoke dispatcher at all — it registers and lists — so there is no place where
  isolation is enforced. Unmet.

### Performance

- [ ] **UNMET — Document the compute unit (CU) cost of each on-chain operation.**
  Nothing has been measured. There is no CU figure anywhere in the repository
  (`grep -ri 'compute unit' .` returns nothing outside vendored code). The only
  performance figure recorded is wall-clock: a real proof against a local
  standalone sequencer takes roughly 150 seconds, and the scheduled CI e2e job
  completes in about 64 minutes including building LEZ.

### Supportability

- [ ] **UNMET — The agent module is deployed and tested on LEZ devnet/testnet.**
  The *policy program* is deployed and exercised on the public testnet. The
  *Logos Core module* is not — see the first Functionality criterion.

- [x] **MET — End-to-end integration tests run against a LEZ sequencer
  (standalone mode) and are included in CI.**
  `.github/workflows/e2e-local-sequencer.yml` clones
  `logos-execution-zone` at pinned revision `47eba25`, builds
  `sequencer_service --features standalone`, installs `r0vm 3.0.5`, and runs
  `scripts/e2e-local-sequencer.sh` with `RISC0_DEV_MODE: 0`. **It has no skip
  path** — a previous LP-0008 submission was closed with "the standalone-sequencer
  E2E did not run in CI; the job completed through its explicit skip path", so if
  this cannot run it fails. Last run: green, 2026-08-15T05:45Z, 1h03m58s.
  Qualification: that green run executed the version of the script published on
  `origin/main`. The current version adds the recipient-balance assertion and has
  not yet had a green scheduled run — which is the second of the two runs the
  DRAFT banner is waiting on.

- [x] **MET — CI is green on the default branch.** Every run on `main` is
  successful (`gh run list`). Qualification, in the same spirit: `origin/main` is
  38 commits behind the tree this document describes, so "green" currently
  certifies less than it will after the push.

- [ ] **UNMET — README documents end-to-end usage including interacting with the
  agent via CLI and the Logos app owner channel.** The README covers the demo,
  the layout, and running a real node. It does not document agent configuration
  end to end, and it cannot document the owner channel, which does not exist.

- [x] **MET — A reproducible end-to-end demo script that works against a real
  local sequencer with `RISC0_DEV_MODE=0`.** `scripts/e2e-local-sequencer.sh`
  starts a real `sequencer_service` in standalone mode on a free port, funds a
  throwaway wallet from the genesis vault, deploys the policy program, creates
  and funds a shielded agent, anchors its envelope, spends inside it, and is
  refused outside it — nothing mocked, `RISC0_DEV_MODE=0` throughout. It runs
  unattended in CI. Separately, `scripts/demo.sh` runs from a clean clone with no
  toolchain beyond Rust and verifies the deployment against the public chain.

- [ ] **UNMET — A recorded video demo showing terminal output including proof
  generation, confirming `RISC0_DEV_MODE=0`.** Not recorded.
  **Video: `<!-- PENDING — URL TO BE ADDED BEFORE SUBMISSION -->`**

## FURPS Self-Assessment

### Functionality

What works: an on-chain spending policy whose *address* is its enforcement, with
`create_policy` / `approve_spend` / `spend` / `spend_approved`; three agents with
their own shielded accounts anchored on the public testnet; storage and messaging
skills written against the real Delivery and Storage ABIs and driven against real
running nodes; an A2A-shaped Agent Card with a LEZ price.

What does not: the settlement authorises but does not transfer; there is no owner
channel; 13 of 21 default skills are unimplemented; the module has never been
loaded by Logos Core; there is no A2A transport binding and no A2A task state
machine. Two structural limits in the policy itself — caller-supplied
`spent_this_period`, and `spend` not binding the policy to the signing agent —
are documented in `docs/limitations.md` and are design changes rather than
patches.

### Usability

The interface a third-party developer meets is small on purpose: implement three
virtuals, call `registerSkill`, and the plugin publishes the schema. That part is
genuinely usable and tested.

The interface an *owner* meets does not exist. The prize's deployment story —
one CLI command, then chat to your agent from any Logos app instance — is
answered here only in its first half, and by a shell script rather than by Logos
Core headless. An owner today configures agents by editing a bash script's
arguments and reads results out of a TSV.

### Reliability

The chain-side guarantees are the strong ones because they do not depend on this
code behaving: an anchored envelope cannot be widened (`init` refuses), an
above-threshold spend cannot execute without an approval account seeded by the
exact payment, and total balance is preserved across every program in a
transaction.

The process-side guarantees are weak. No task state is persisted; a restart loses
everything in flight. There is no owner notification path, therefore no retry and
no timeout. Skill failure isolation is specified but not enforced, because
nothing dispatches skills yet.

Where reliability work has actually gone is into the evidence path, after being
burned by it: `a2a-task.sh` refuses to write a manifest for an unconfirmed hash,
and refuses again if the recipient's balance did not move by the price;
`deploy-agents.sh` stopped discarding `spel`'s own "Transaction NOT confirmed"
line, which had been turning reported failures into silent ones; confirmation
windows were widened from 150 seconds to twelve blocks after transactions were
declared dead that later landed.

### Performance

Not characterised, and this is a real gap against the prize's only Performance
criterion. **No compute-unit cost has been measured for any operation.**

The wall-clock figures that do exist: one real Risc0 proof against a local
standalone sequencer takes roughly 150 seconds; the full CI e2e job — cloning and
building the LEZ workspace, installing `r0vm`, then the lifecycle — completes in
about 64 minutes; a settlement transaction is ~270 KB on the wire and a program
deployment ~410 KB. Public-testnet blocks are 60 seconds apart, which is why
every confirmation loop here waits twelve of them.

### Supportability

Tests: 9 unit tests on the policy primitive, adversarial by intent — a per-tx cap
drained by repetition, a hostile period total that must not overflow into "plenty
left", an approval for one payment replayed onto another; a C++ suite over the
storage and messaging skills against fake ports, checking that they refuse when
the node is down, that `share` blames the correct half, and that malformed input
is refused rather than thrown; and the full lifecycle against a real standalone
sequencer.

CI: two workflows. `ci.yml` on every push — build and test with `--locked`
throughout, recompute the committed program's deploy hash, ask the public
sequencer whether it is live, ask it about a hash that cannot exist, and compile
and run the skill suite. `e2e-local-sequencer.yml` scheduled daily and on demand,
with no skip path.

Structure: `crates/agent-policy-core` (the primitive, shared by the guest and by
the deployment scripts so the hash cannot be computed two ways and drift),
`crates/agent-verifier-spel` (the guest), `module/` (the Logos Core plugin),
`scripts/` (deployment, A2A task, demos), `docs/` (deployment, limitations,
skills, recon).

Debugging: the failure modes on this chain are unusually silent — a dropped, a
pending, a rejected and a never-submitted transaction all return the same `null`
from `getTransaction`, and there is no mempool or status endpoint — so most
diagnostic effort went into scripts that distinguish "not landed yet" from "will
never land", and into `docs/limitations.md`, which records each blocker with the
sequencer's own log line and a file:line citation into the LEZ source.

## Supporting Materials

### Verify the deployment yourself

Every hash below is on the public testnet. `getTransaction` returns
`[<base64 transaction>, <block number>]` for a live transaction and `null` for an
absent one, so both the existence and the block are checkable in one call:

```bash
q() { curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
      -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}"; }

q 6e4a2000537c9596df87ce0404f540eabfd18233da4b9ce18f5958fdf365321a   # -> [..., 8034]
q dededededededededededededededededededededededededededededededede   # -> "result":null
```

**Run the control.** Without it the first call proves nothing.

| What | Transaction | Block |
|---|---|---|
| Policy program (three-agent set) | `6e4a2000537c9596df87ce0404f540eabfd18233da4b9ce18f5958fdf365321a` | 8034 |
| `create_policy` — storage agent | `3dcb237853ce7399d434bf903963b03789087c1e18ba66b0cfada268db05a6df` | 8150 |
| `create_policy` — messaging agent | `28930c0a1499749a58a416bfa0f21771b70464338c933fd10bba6417849a068e` | 8050 |
| `create_policy` — blockchain agent | `1075e47db9fe29c1f1a3190516033e26b1f09b7097bd78df6bcf63baf8282292` | 8051 |
| Policy program (current, chains the transfer) | `b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549` | 8590 |
| `create_policy` — storage agent, current program | `ab017c9c9d55ac6ea198e692c5ed2b1dea4a2a70a1863495e48e7a91d67735f2` | 8591 |
| Settlement — authorised 25 LEZ, moved nothing | `aea80817f6c4283c79b21095596ce774e3638cef888a3cc7b705b61ed98449e7` | 7506 |
| Settlement — authorised 25 LEZ, moved nothing | `7f5a506b2183d1ffd02634f921f431592fa4a0d14d82c112b6b7fa996ab87319` | 8016 |
| Control — cannot exist | `dededededededededededededededededededededededededededededededede` | `null` |

The last two rows of settlement are listed as evidence of *what was done*, not as
evidence of a payment. They are proofs of authorisation. See the DRAFT banner.

### The three agents, and recomputing their policy hashes

| Category | Agent account | Envelope (per-tx / per-period / window) | Policy hash | Anchor |
|---|---|---|---|---|
| storage | `7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6` | 50 / 500 / 1000 blocks | `870178a2…c7ad0f08` | `3dcb2378…` (8150) |
| messaging | `25LLt4ZxmRLjv5g7satyEvq91e4XqRi6mYT7ngMdsafw` | 25 / 250 / 1000 blocks | `79e84924…c2fa40e5` | `28930c0a…` (8050) |
| blockchain | `9KdQSJ2tB9CGDWKZYFLEuZ28enPhzb2erPwTYVVXicNe` | 200 / 1000 / 1000 blocks | `6cc36c91…53e050a9` | `1075e47d…` (8051) |

Manifest: `artifacts/agents.tsv`; anchor ledger keyed by `(program, policy_hash)`:
`artifacts/anchored.tsv`.

Each policy hash is a pure function of `(owner, agent, limits)` and recomputes
from the repository with the same code the on-chain guest runs:

```bash
hexof() { python3 -c "import hashlib,sys;print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$1"; }

# messaging agent, owner DumJ4LCBnHE9jUu2yxPfqdL14g3v756Gzby6LuT9hE51
cargo run --release -p agent-policy-core --example policy-hash -- \
  "$(hexof DumJ4LCBnHE9jUu2yxPfqdL14g3v756Gzby6LuT9hE51)" \
  "$(hexof 25LLt4ZxmRLjv5g7satyEvq91e4XqRi6mYT7ngMdsafw)" 25 250 1000
# 79e849243b369dcbed97ed49c6f8f4ee081652e0ef941c4f2f84f61ac2fa40e5

# blockchain agent, same owner
cargo run --release -p agent-policy-core --example policy-hash -- \
  "$(hexof DumJ4LCBnHE9jUu2yxPfqdL14g3v756Gzby6LuT9hE51)" \
  "$(hexof 9KdQSJ2tB9CGDWKZYFLEuZ28enPhzb2erPwTYVVXicNe)" 200 1000 1000
# 6cc36c91820a79660360223bf2b1fa3b29fa90cc268f288512e9cf6853e050a9
```

Change any argument — raise `per_tx` by one — and the hash changes, which is the
whole mechanism: the new envelope names an account `create_policy` never
initialised. **Note for reproduction:** the `owner` column is not yet in the
committed `artifacts/agents.tsv` for these three rows, so the owner ids above are
supplied here explicitly. Adding that column to the manifest is part of the
pending work.

### The deployed program is the program in this repository

A LEZ program-deployment transaction hash is `SHA256(u32_le(len) ‖ bytecode)` —
content addressed — so the committed binary hashes to exactly its deploy
transaction:

```bash
python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())"
# b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549
```

The transfer program the settlement chains into is **not** deployed by this
repository — it is one the chain already runs — but a byte-identical copy is
committed as `artifacts/programs/authenticated_transfer.bin`, because the privacy
circuit composes the inner call inside the proof and resolves the callee by
ImageID. Its identity is checked against the chain rather than asserted, by
comparing `spel program-id` against `getProgramIds`. `scripts/demo.sh` performs
both checks.

### Real nodes, driven for real

```bash
./scripts/exercise-nodes.sh
```

Builds a C driver against `liblogosdelivery` and another against `libstorage`,
and runs both. Every step is an assertion and the exit code is the result — the
shipped examples check only the first call and then print what the rest returned,
which reads like success for a node that started and did nothing. A green run
starts a Delivery node, waits for the node's own `nodeStarted` event rather than
for `start` to return, has it report its peer id, publishes a message and waits
for the network to propagate it back; then starts a Storage node, uploads a file,
and asserts that the manifest fetched for the returned content address names that
file. Not in CI, and deliberately: building the libraries takes tens of minutes
and a green result would depend on public-network peer uptime — an amber job
teaches everyone to ignore it, and a skipped step counts as not run.

One trap is recorded because it fails silently: the event name you register with
is not the name that comes back — you subscribe to `onMessageSent` and the
payload carries `"eventType":"message_sent"`.

### Documents

- [`docs/limitations.md`](docs/limitations.md) — what does not work, with the
  sequencer's own log lines and file:line citations into the LEZ source. Read
  this first.
- [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) — what is deployed and how to
  re-verify it.
- [`docs/skills.md`](docs/skills.md) — the skill interface and the state of each
  skill.
- [`docs/recon.md`](docs/recon.md) — the Logos module contract, and why earlier
  submissions were closed.

### Environment

```
Network:            Public LEZ testnet — https://testnet.lez.logos.co
Explorer:           https://explorer.testnet.lez.logos.co  (≈1h45 behind the sequencer)
LEZ:                v0.2.4 (commit 47eba25)
spel:               v0.6.0 sources, repinned and ported to v0.2.4 (vendor/spel)
cargo-risczero:     3.0.5
RISC0_DEV_MODE:     0 throughout
```

The explorer is a separate index and lags the sequencer by roughly an hour and
three quarters; a transaction submitted minutes ago reads "not found" there while
`getTransaction` already returns it. The RPC is the source of truth, and every
verification command in this document uses it.

## Terms & Conditions

By submitting this solution, I confirm that I have read and agree to the
[Terms & Conditions](../TERMS.md).
