# Solution: LP-0008 — Autonomous AI Module with Wallet, Storage, and Messaging

> ## ⚠️ DRAFT — NOT READY FOR SUBMISSION
>
> **This document does not pin a commit hash, and the reason is worth stating.**
> The repository is public and **actively changing** — a security redeploy, a CI
> fix, the A2A binding spec, a deployment-doc regeneration, settlements and two
> further redeploys all landed while this was being written.
>
> A pin was carried here through three values, `1de38d8`, `51e57fc` and
> `9e98d24`, and **every one of them was wrong by the time it was read**: twice
> because the claims underneath were edited without moving it, and once because
> a rebase rewrote the hash it named out of existence. A hash written into a
> branch that will be rebased is wrong by construction, and a document that
> states its own commit and then describes a later one has the same defect as a
> benchmark naming a superseded program — everything in it is true of something,
> and the reader cannot tell of what.
>
> So the state this document describes is identified the way it can be checked
> rather than asserted. `git rev-parse --short HEAD` names the commit you have.
> `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md` re-fetches every
> figure in the evidence sections from the chain and **exits non-zero if any has
> moved**, which is a stronger guarantee than a pin ever gave: a pin tells you
> which commit the author meant, this tells you whether the numbers are true
> now. It runs in CI, on every push.
>
> **Every figure in the evidence sections is generated, not typed.**
> `./scripts/submission-evidence.py` fetches them from the committed binary and
> from `https://testnet.lez.logos.co`, and splices them into the three blocks
> below marked `BEGIN GENERATED`. It derives the deploy transaction from the
> committed bytes rather than quoting it, reads the manifests by column name,
> confirms every transaction it cites is inside the block it names and absent
> from both neighbours, and exits non-zero if any of that fails — so a stale
> version of this document cannot be produced. `--check` re-runs the comparison
> without writing.
>
> That machinery exists because the previous draft did the opposite and it went
> badly. It carried a settlement table whose columns were headed `(getAccount)`
> and `(from getTransaction)` under the caption "Verified independently for this
> document", in which every value was a literal somebody had typed; its balances
> had never been true; the three transactions it led with are ones
> `docs/DEPLOYMENT.md` disowns by name; and the two snippets it offered a
> reviewer for checking the manifests without trusting the author both crashed,
> because they named a column that had been renamed. None of it was noticed for
> weeks. A hand-written fact does not announce that it has gone stale, and on a
> content-addressed chain every redeploy moves all of them at once.
>
> ### What blocks submission today
>
> | # | Blocker | State |
> |---|---|---|
> | 1 | **No recorded video demo.** The prize requires narrated walkthroughs of ≥3 use cases showing terminal output that confirms `RISC0_DEV_MODE=0`. A silent screencast is explicitly insufficient. This is the one blocker with real work left in it. | Not recorded. Placeholder in [Supporting Materials](#supporting-materials). |
> | 2 | **`HEAD` is ahead of `origin/main` and unpushed.** A reviewer cloning right now gets the published branch, which does not have the regenerated evidence sections or the CI steps that keep them honest, and CI has not run on the commits that are only here. No count is written in this row on purpose — `git rev-list --left-right --count origin/main...HEAD` answers it, and any number typed here is wrong the moment either side moves, which is how the previous two versions of this row came to be wrong. | `git push`, then confirm CI. |
>
> Resolved while this was written, and no longer blockers: the
> `spend`-does-not-bind-the-policy defect, the caller-supplied period total and
> the caller-chosen policy address are all fixed, redeployed and re-anchored;
> **settlements have landed under the shipped program**, so the anchors and the
> settlement evidence are under the *same* program — the generated table says
> which, and how many, and this line deliberately does not, because the last
> three versions of it named transactions that had stopped being current; CI
> went green after the `<cstdint>`
> fix; the Agent Card is signed (BIP-340 Schnorr, `scripts/sign-agent-card.py`,
> which self-verifies before emitting); `docs/a2a-binding.md` specifies the
> transport binding; and `docs/DEPLOYMENT.md` has been regenerated.
>
> The Success Criteria Checklist marks unmet criteria **UNMET**, including
> criteria for which working, tested code exists. Code existing is not the
> criterion, a test CI skips is not evidence, and neither is a document
> describing something the repository does not do.

**Submitted by:** edenbd1

## Summary

An agent that participates in the Logos stack directly rather than through an
API key: it holds its own shielded LEZ account, and the limit on what it may
spend is not a check inside the agent process but **state on chain that the
agent's own program is the only thing permitted to write**.

The core idea is one design decision. An agent runs unattended on a remote node
and holds its own signing key, so any spending rule the agent evaluates can be
evaluated differently by whoever holds the process. So the rule is not evaluated
by the agent at all. Each agent has exactly **one** policy account, at
`PDA(["agent-policy/v1", agent_id])` — the agent and nothing else — and the
owner, both limits, the period and the running total all live in that account's
data, which LEZ rule 6 (`UnauthorizedDataModification`) lets only this program
write. `create_policy` declares it `#[account(init, …)]`, so the first anchor for
an agent is the only anchor for that agent; a second is not detected, it is
impossible. `spend` derives the same address from the *paying* account's id out
of the pre-state, so there is no argument to lie about, and it reads the limits
off the account rather than from the call.

That is the second design. The first three deployments seeded the address with
the policy *contents* — owner, agent and both limits — so that raising a limit
named a different, uninitialised account. **It was broken, and it is worth
saying how, because the repair is the interesting part.** Each of three
successive fixes added a comparison, and each time the attack moved; the version
that mattered needed no missing comparison at all. An attacker holding a
compromised agent's key does not have to impersonate an owner or borrow a
policy — *it is* the owner: it anchors a fresh policy naming the compromised
agent and itself, with `per_tx = per_period = u128::MAX`, and every check
passes, because every check is satisfied. That was executed against the deployed
binary in three variants. The defect was that folding the limits into the
address let the caller choose the address, so an uninitialised account was
always available. Removing the choice — one address per agent — is the fix.
`crates/agent-verifier-spel/methods/guest/src/bin/agent_verifier.rs` opens with
the whole account.

Three agents — one per default skill category — are anchored on the public LEZ
testnet, each with its own shielded account and its own envelope. Two of them
have run an A2A task to completion and settled it in LEZ, unattended, with the
per-period total accumulating on chain.
Agent-to-agent coordination is A2A-shaped: cards carry the A2A schema plus an
`x-logos` extension for the price and payment address that vanilla A2A has no
field for.

**What is not delivered** is stated in the checklist and in
[`docs/limitations.md`](docs/limitations.md), and it is substantial: the owner
can never approve an above-threshold spend after anchoring a policy, the
messaging and storage skills have never been run against a live node, no model
has ever been run against the inference port, and there is no video.

## Repository

- **Repo:** <https://github.com/edenbd1/lp-0008-autonomous-agent-module>
- **License:** dual MIT / Apache-2.0
- **Default branch:** `main` (public). ⚠️ See blocker 2 for how this tree
  relates to `origin/main`, and run the command there rather than believing a
  count written down here.

Everything asserted below is verifiable from a clean clone plus the public
sequencer. No claim in this document depends on trusting the author.

### The one command that checks the rest

```bash
./scripts/demo.sh
```

It runs the policy tests, recomputes the deployed program's hash from the
committed binary, asks the public sequencer whether that transaction exists, and
asks the same sequencer about a hash that cannot exist. Without that control the
first question proves nothing — an RPC that answered non-null to everything would
pass it just as happily. It is green at this commit.

### Reading the evidence without trusting this document

Regenerate the evidence sections yourself. They are produced by, and only by,
this command:

```bash
./scripts/submission-evidence.py                       # print them
./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md   # or just check
```

`--check` re-fetches every figure and exits non-zero if this document has
drifted from what the chain now says, which is the only reason to believe the
numbers below. It refuses to write anything it could not fetch: there is no
"TBD" path, no blank cell and no placeholder in it, and a fact the chain cannot
show is emitted as a sentence saying so.

Two earlier commands lived here, offered for exactly this purpose, and **both of
them crashed** — they read `artifacts/agents.tsv` and `artifacts/anchored.tsv`
for a `policy_hash` column that had been renamed to `policy_account` some
commits earlier, so a reviewer who ran them got `KeyError` and no reason to
believe anything else here. That is why the check is now a script that runs in
CI rather than a snippet nobody executes. The manifests are still read by
**column name**, never by position; positional reads have produced three
separate false results in this repository.

If you would rather ask the sequencer directly, **the control is what makes the
check mean anything**:

```bash
q() { curl -s -X POST https://testnet.lez.logos.co \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}"; }

q <any create_tx from artifacts/anchored.tsv>
                # => {"result":[<bytes>,<block>]}   present
q dededededededededededededededededededededededededededededededede
                # => {"result":null}               absent — as it must be
```

A returned block number is the sequencer's word for it, and this repository has
been wrong about block attribution before, so the generator does not stop there:
it fetches `getBlock` for that block and for both neighbours and requires the
transaction's own bytes to be present in the first and absent from the other
two. Earlier drafts claimed that check in prose while no script performed it.

One warning, because it has misled readers of this repository before:
**`getAccount` is not an existence check.** It answers with a fully-populated
default account — zero balance, zero nonce, zero owner — for an address that has
never existed, and for a shielded account it does the same, because it reads the
public state only. Presence proves nothing there; the *program owner* and the
*balance* are the signals.

## Approach

### One policy account per agent, and the limits are its data

A spending limit enforced inside the agent is enforced by whoever controls the
agent's process — which, for an agent deployed on a remote node with its own
key, is not necessarily the owner. So the limit is moved out of the process
entirely.

`create_policy` initialises exactly one account per agent, at
`PDA(["agent-policy/v1", agent_id])`. It is `#[account(init, …)]`, and `init`
refuses an account that is not in its default state, so the first anchor for an
agent is the only anchor for that agent — a second is not *detected*, it is
impossible. The owner, both limits, the period and the running total live in
that account's data, which LEZ rule 6 (`UnauthorizedDataModification`) permits
only this program to write. `spend` derives the same address from the *paying*
account's id, taken from the pre-state the state machine built rather than from
the instruction the agent serialised, so there is no `agent_id` argument to lie
about, and it reads the limits off the account rather than from the call.

The address derivation above is not typed into this document: it is printed from
the shipped `idl/agent_verifier.idl.json` by the generator, in the table under
[the program](#the-program-on-chain). It was typed once, and this document then
spent three deployments describing a derivation the program had abandoned.

This is checkable by anyone, and the check has a control:

```bash
# the policy account for an anchored agent, from the IDL the repo ships
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
     pda policy --agent-id <agent_id from artifacts/agents.tsv>
# then getAccount that address:
#   program_owner = the policy program's ProgramId   -> anchored
#   program_owner = [0,0,0,0,0,0,0,0]                -> never anchored
```

It should reproduce the `policy_account` column of
[`artifacts/agents.tsv`](artifacts/agents.tsv), and `./scripts/verify-deployment.sh`
checks that it does. Run against an anchored agent the owner comes back as the
policy program's own `ProgramId` (`spel program-id
artifacts/programs/agent_verifier.bin` prints it to compare, and the generated
section below compares them for you). Run against an id nobody anchored, the PDA
resolves fine and comes back with the **default** owner. That difference is the
whole mechanism, visible from outside with two RPC calls.

**The alternative, and why it was abandoned rather than rejected.** Putting the
limits in the *address* — `PDA(SHA256(owner ‖ agent ‖ per_tx ‖ per_period ‖
period_blocks))`, so that raising a limit named a different, never-initialised
account — was the original design, and this document recommended it at length.
It shipped in three deployments and it does not work. The reason is worth more
than the design was: an attacker holding a compromised agent's key does not need
to impersonate an owner or reuse someone else's policy, because **it is** the
owner. It anchors a *fresh* policy naming the compromised agent as `agent_id`
and itself as `owner_id` with `per_tx = per_period = u128::MAX`, and spends the
balance under that. Every comparison in the program passes, because every one of
them is satisfied. Three earlier fixes had each added one more comparison and
each time the attack had simply moved. Folding the limits into the address is
what made this available: every `(owner, agent, limits)` triple had an account of
its own, all uninitialised, so "anchor a new policy" was always on the table.
One address per agent removes the choice, and `init` gives that address to
whoever writes first — the owner, when it creates the agent, before the agent has
run at all. `crates/agent-verifier-adversarial` executes the attack against the
deployed binary and asserts the halt code it now stops at.

The per-period total closed a second hole. `spend` used to take
`spent_this_period` as an argument, which both callers passed as `0`, so the
per-period ceiling was advisory and an agent that always passed zero had a
per-transaction limit and no period limit. It now takes `window_start` instead,
and does not trust it: the window must begin on a multiple of `period_blocks`, so
windows cannot be slid to dodge a total, and the transaction is pinned to
`[window_start, window_start + period_blocks)` by its own block validity range, so
a caller cannot reset its budget by naming a different period. `./scripts/demo.sh`
executes the committed guest against that: three spends of 200 accumulate to 600
within one period, and a window that does not start on a multiple is refused with
code 6014.

`docs/limitations.md` records both of these under "Two defects this file used to
carry, and what replaced them", in the past tense and with the replacement
named. An earlier version of this document warned that it had *not* caught up
and still listed `spent_this_period` as open. That warning was itself out of
date when it was written, which is the same failure one level up.

### `spend` moves no balance itself, and cannot

The first working version of the program authorised payments and moved nothing —
it returned a confirmed, on-chain proof that a policy permitted 25 LEZ, while
every balance stayed exactly where it was. LEZ rule 5
(`UnauthorizedBalanceDecrease`) refuses any post-state that decreases the balance
of an account the executing program does not own, and an agent's account is owned
by LEZ's **authenticated transfer** program.

So the policy program checks the anchored envelope and then **chains a call** into
the transfer program, which does own the accounts. The privacy circuit proves both
programs and the composition. `artifacts/programs/authenticated_transfer.bin` is a
byte-identical copy of the chain's own program — not deployed by this repository —
committed because the circuit looks the callee up by ImageID; `scripts/demo.sh`
checks its ImageID against `getProgramIds` rather than asserting it.

Superseded programs are still on the testnet, since deployment is
content-addressed, and `docs/limitations.md` lists what each got wrong. That list
is part of the evidence: it is what "tried and did not work" looks like.

### The program on chain

<!-- BEGIN GENERATED program -- scripts/submission-evidence.py; do not edit by hand -->

Every figure in this section was fetched by `./scripts/submission-evidence.py` at generation time. Nothing in it is transcribed from another document, and the transaction hash is not quoted from anywhere — it is derived from the committed bytes.

`artifacts/programs/agent_verifier.bin` is 440,876 bytes. Deployment on LEZ is content-addressed, so the deploy transaction is `SHA256(u32_le(len) ‖ bytecode)` of exactly those bytes:

| | |
|---|---|
| deploy transaction | [`697746f5…cb5370bf`](https://explorer.testnet.lez.logos.co/transaction/697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf) |
| block | 8839 |
| on the wire | 440,881 bytes |
| bytes found in block | 8839, and in neither 8838 nor 8840 |
| ImageID recomputed from the committed binary | `778a9341a00de46c4c056ac63a66f63156b068e61cce7f155a2b495e670c4661` |
| ProgramId owning every policy account below | `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647` |

The ImageID recomputed from the committed ELF and the `program_owner` the chain reports for the anchored policy accounts agree. The binary in this repository is the program enforcing these envelopes on chain.

`spend` moves no balance itself — LEZ rule 5 refuses any post-state that decreases the balance of an account the executing program does not own — so it chains a call into the authenticated transfer program, which does own them. `artifacts/programs/authenticated_transfer.bin` is committed because the circuit resolves the callee by ImageID; it is not deployed by this repository. Its ImageID recomputes to `583309054,2344528779,3806558405,2890696795,2257354672,3978764116,2273929063,1518858078`, and `getProgramIds` reports `authenticated_transfer` as `583309054,2344528779,3806558405,2890696795,2257354672,3978764116,2273929063,1518858078` — the same program.

Read out of the shipped `idl/agent_verifier.idl.json` rather than described — the address derivation is the security argument, so it is quoted from the interface the repository actually ships:

| instruction | policy account address |
|---|---|
| `approve_spend`, `create_policy`, `update_policy` | `PDA("agent-policy/v1", agent_id (arg))` |
| `spend`, `spend_approved` | `PDA("agent-policy/v1", agent (account))` |

There is **one policy account per agent**: the seed is the agent, and every limit is the account's *data*, which LEZ rule 6 (`UnauthorizedDataModification`) lets only this program write. Where the seed is `(account)` it comes from the pre-state the state machine built and there is no argument to lie about; where it is `(arg)` it is caller-supplied, which is why `create_policy` is `#[account(init, …)]` and the first anchor for an agent is the only one.

The control: `getTransaction` on `dededededededededededededededededededededededededededededededede`, a hash nobody has submitted, returned `null` on this run. Without it "the sequencer returned a transaction" would prove nothing.
<!-- END GENERATED program -->

### Why Logos, specifically

The payer is a **shielded** account. What a task settlement reveals on a
centralised rail — who paid, for what, how often — is exactly the metadata that
makes an agent marketplace legible to whoever runs it. Here the settlement is a
privacy-preserving transaction signed by the agent's own private account: the
amount and the payee are visible, the payer is not.

That asymmetry is not the design's intent, and it is worth being precise about.
`spel` resolves a `Private/<id>` recipient only for accounts the *sending* wallet
holds keys for, so one agent cannot pay another's shielded account at all. Each
agent therefore also keeps a **public receiving account**, which its Agent Card
advertises. Half of the privacy the design wants is delivered; the other half
needs `spel` to expose the `PrivateForeign` account kind the wallet already has
(`lez/wallet/src/account_manager.rs:30-34`). That is upstream work, and it is
recorded in the limitations rather than glossed.

A2A leaves two things open on purpose — payment and encrypted transport — and
Logos supplies both natively. LEZ is the payment layer A2A omits; Logos Messaging
is the transport binding that replaces A2A's HTTP. On a centralised alternative
the spending ceiling would be a row in someone's database rather than an address
in a state machine, and "the agent cannot exceed its limit" would be a promise
instead of a rejection.

## Success Criteria Checklist

Legend: **MET** — demonstrated, with evidence anyone can re-check.
**UNMET** — not demonstrated, whatever code exists.

### Functionality

- [ ] **UNMET — Module loads and runs inside Logos Core alongside the wallet,
  storage, and messaging modules without modifying them.**
  Half of this is demonstrated and half is not. `module/tests/logos_core_load_test.cpp`
  `dlopen`s the real `liblogos_core` out of an installed `LogosBasecamp.app` and
  drives it through the same C API, in the same order, as Basecamp's own
  `app/main.cpp`, ending in `logos_core_load_module("agent", true)` — the call
  that runs when a user enables a module. The module loads and answers `skills()`
  with 22 entries rather than `[]`, which was the failure mode worth testing
  against: a module that loads and offers nothing looks identical to one that
  works. What is **not** demonstrated is co-residency — the wallet, storage and
  messaging modules were never loaded alongside it, because the storage and
  delivery node libraries have never been run here at all (below). Recorded
  output: [`docs/basecamp.md`](docs/basecamp.md).

- [ ] **UNMET — The agent has its own shielded LEZ account and can send and
  receive tokens independently of the owner's wallet.**
  *Send* is demonstrated: both settlements are privacy-preserving transactions
  signed by the agent's own shielded account, not the owner's. *Receive* at that
  same shielded account is impossible with the current `spel`, which fails with
  `KeyNotFoundError` before building anything. Each agent keeps a separate public
  account to be paid at. The criterion as written is not met.

- [ ] **UNMET — The owner can deploy the agent and configure it with a single CLI
  command on any machine using Logos Core headless.**
  `scripts/deploy-agents.sh` deploys and anchors all three agents reproducibly and
  is the source of the manifests, but it drives `spel` and the LEZ wallet — not
  Logos Core headless. No headless Logos Core deployment has been run.

- [ ] **UNMET — The owner can interact with the agent in real time from a separate
  Logos app instance using Logos Messaging, with no intermediary server.**
  `module/src/owner_channel.{h,cpp}` implements the channel against Delivery's
  real reliable-channel API, and its suite covers the cases that decide whether
  money moves: a node that has not reported `nodeStarted` refuses to open, a
  channel that will not open is reported rather than pretended, an answer to a
  different request cannot settle this one, an approval naming different terms is
  refused, and an owner who never replies is terminal rather than a quiet fallback
  to acting alone. All of that is against a **fake** port. No message has been sent
  between two Logos app instances.

- [ ] **UNMET — The spending threshold holds above-threshold transactions for owner
  approval and executes below-threshold transactions autonomously.**
  The below-threshold half is demonstrated on the public testnet twice (see the
  settlement evidence below): 25 LEZ sits inside the client's anchored
  per-transaction limit, so `spend` takes the autonomous branch, and the chain
  would have refused it otherwise. The above-threshold half **cannot currently
  work**, and the reason is structural rather than a bug. The constraint measured
  on chain is one program transaction per public signer account. `approve_spend`
  requires the owner as signer, and the policy hash commits
  `owner_id = sha256(owner account id)`, so the approval must come from the same
  account that anchored the policy — which has already spent its one transaction on
  `create_policy`. **The owner who anchored a policy is, by construction, unable to
  approve anything under it.** Two ways out are identified and neither has been
  tried. See `docs/limitations.md`.

- [x] **MET — All default skills implemented and documented.**
  All twenty-one are implemented **and registered**, which are different claims:
  thirteen skills were implemented here before anything registered them, and the
  module answered `skills()` with an empty card while looking perfectly healthy.
  `installBuiltinSkills` registers 22 skills — the prize's twenty-one plus
  `agent.evaluate_task`, which the prize does not ask for and which is kept
  because it is the only skill on the pluggable-inference seam a *different*
  criterion requires — and `start()` calls it itself when no host wired the
  ports, so a module loaded as a plugin offers a full card.

  `meta.skills` was the last one missing, and it was missing in the way that is
  hardest to see: `invoke()` is a plain map lookup with no special case, so
  `invoke("meta.skills")` returned *no skill named 'meta.skills' is registered*
  while three C++ doc comments (`agent_module_interface.h:55`,
  `agent_module_plugin.h:198`, `agent_skills.h:202`) described it as existing and
  `AgentModuleImpl::skills()` really did produce the catalogue. The *information*
  was reachable in-process; the *skill* was not, and a host that loads this module
  reaches it through `invoke()` and nothing else. It is now registered like any
  other skill — not special-cased in `invoke()` — and reads the same registry
  `agent.card` does, so the catalogue and the card are one answer.

  Asserted by execution rather than by reading: `module/tests/plugin_load_test.cpp`
  loads the packaged `module/agent.lgx` through `QPluginLoader` and
  `module/tests/logos_core_load_test.cpp` loads it through the installed
  Basecamp's own `liblogos_core`. Both report 22 entries, each with a parameter
  schema, `invoke()` dispatching to every one, and `meta.skills` listing all 22
  — including itself — over the boundary. Recorded output in `docs/basecamp.md`.

- [ ] **UNMET — A2A-compatible: cards follow the A2A schema, tasks follow the A2A
  lifecycle, documented as an A2A transport binding over Logos Messaging.**
  Most of this is now in place. The card carries `protocolVersion 0.3.0`,
  `preferredTransport`, `capabilities`, `defaultInputModes`/`defaultOutputModes`
  and a `skills` array in A2A shape, plus an `x-logos` extension carrying the
  payment account, price and settlement kind — the two fields A2A has no slot for.
  It is **signed** as of `342997c`: `scripts/sign-agent-card.py` produces a JWS with
  BIP-340 Schnorr over secp256k1, runs the published test vector as a self-test
  (including a negative case, asserting a tampered digest does *not* verify), and
  refuses to emit a signature it cannot itself verify. The lifecycle
  (`working → input-required → completed/failed`) is implemented in
  `module/src/agent_skills.h`, and the binding is specified in `docs/a2a-binding.md`
  — which is candid that an off-the-shelf HTTP A2A client cannot talk to a Logos
  agent, and that what interoperates is the *data*, not the transport.
  Marked UNMET on the transport, which is the half the criterion names: **no A2A
  task has ever crossed a live Logos Messaging node.** The settlements were driven
  by `scripts/a2a-task.sh` over the chain; the messaging skills that would carry the
  card and the task have never been run against a running node. The binding is
  specified and not exercised, which the spec itself says.

- [x] **MET — Two or more agents discover each other via Agent Cards, execute a
  task following the A2A lifecycle, and transfer LEZ payment autonomously, without
  owner intervention.**
  Settlements run with no special handling between them — the repeats matter as
  much as the first, because a repeat settlement is what this repository could
  not produce for most of its life. The table below is **generated**, not
  transcribed; reproduce it with `./scripts/submission-evidence.py`.

<!-- BEGIN GENERATED settlements -- scripts/submission-evidence.py; do not edit by hand -->

Every figure in this table is decoded out of the settlement transaction itself. `getAccount` is deliberately **not** used for the balances: it reports current state, this chain has no historical-state RPC, and the payee's balance has since moved both up and down — so what it holds today is not evidence about a settlement that landed hundreds of blocks ago. A LEZ transaction commits to its own post-state, and the hash proves the bytes are that transaction, so the balance below is the balance *at* the settlement rather than a number cached in a file.

| # | settlement | block | on the wire | skill | price | payee balance after | policy `window` / `spent` after |
|---|---|---|---|---|---|---|---|
| 1 | [`4e3a3454…a490ddb1`](https://explorer.testnet.lez.logos.co/transaction/4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1) | 8740 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `Coxz1Cmf…` at 8,000 / 25 |
| 2 | [`7cad4fbd…7168f019`](https://explorer.testnet.lez.logos.co/transaction/7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019) | 8747 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `Coxz1Cmf…` at 8,000 / 50 |
| 3 | [`e691f593…26631047`](https://explorer.testnet.lez.logos.co/transaction/e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047) | 8892 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `7HH46tXh…` at 8,000 / 25 |
| 4 | [`aef14146…8bcb70b8`](https://explorer.testnet.lez.logos.co/transaction/aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8) | 8901 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `7HH46tXh…` at 8,000 / 50 |

Settlement 1: the sequencer's bytes hash to `4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1`, which is the hash cited, and those bytes were found inside block 8740 and in neither block 8739 nor 8741. The transaction touches 2 accounts.
  The envelope it charged, `Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM`, is owned by ProgramId `3650484754,2032214328,3036549407,1048473516,3525353185,166458006,2651200166,3637082293`, which is **not** the program this repository ships. **This settlement was made under a superseded deployment.** Its policy account was derived from a different ImageID and no longer exists under the program deployed today. It is a real transaction and it resolves on the explorer, but it is not evidence about the program in this repository.
Settlement 2: the sequencer's bytes hash to `7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019`, which is the hash cited, and those bytes were found inside block 8747 and in neither block 8746 nor 8748. The transaction touches 2 accounts.
  The envelope it charged, `Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM`, is owned by ProgramId `3650484754,2032214328,3036549407,1048473516,3525353185,166458006,2651200166,3637082293`, which is **not** the program this repository ships. **This settlement was made under a superseded deployment.** Its policy account was derived from a different ImageID and no longer exists under the program deployed today. It is a real transaction and it resolves on the explorer, but it is not evidence about the program in this repository.
Settlement 3: the sequencer's bytes hash to `e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047`, which is the hash cited, and those bytes were found inside block 8892 and in neither block 8891 nor 8893. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 4: the sequencer's bytes hash to `aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8`, which is the hash cited, and those bytes were found inside block 8901 and in neither block 8900 nor 8902. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.

**2 of the 4 settlements above predate the program this repository ships.** They are kept because they are on chain and a reviewer will find them, but the criterion they support is only supported by the 2 made under the current deployment.

What the chain cannot show, stated rather than implied: the payer is a shielded account, so only the credit side of each settlement is publicly readable. `getAccount` answers with a fully-populated default account — zero balance, zero nonce, zero owner — for a shielded address exactly as it does for one that has never existed, so it is not an existence check and no debit is quoted here. The debit is constrained anyway: LEZ rule 8 requires total balance to be preserved across every program in a transaction, so a transaction that credited 25 LEZ debited 25 LEZ.

The explorer indexes roughly an hour and three quarters behind the sequencer, so a settlement that landed in the last hour or two reads "not found" at the links above while `getTransaction` already returns it. That is an indexing lag, not a missing transaction; the RPC is the immediate source of truth and this document was generated from it.
<!-- END GENERATED settlements -->

  Reproduce the settlements themselves with `./scripts/a2a-task.sh`, which
  refuses to write its manifest unless the transaction confirms **and** the
  recipient's balance moved by exactly the price.

  Two things this criterion used to claim, withdrawn because they were not true.
  It said each transaction's bytes had been checked inside its own block and
  absent from both neighbours — a good check that **no script in this repository
  performed**; `scripts/submission-evidence.py` performs it now, and the
  generated notes above report the block numbers it compared. And it read the
  payee's balance out of `getAccount` as though that were evidence about a past
  settlement. It is not: `getAccount` returns current state, this chain has no
  historical-state RPC, and the payee's balance has since gone *down* as well as
  up, so the running total the old table showed could never have been rechecked.
  The balances above come out of each transaction's own committed post-state
  instead, which is what makes them provable.

- [ ] **UNMET — At least 3 illustrative use cases demonstrated end-to-end on LEZ
  testnet.**
  One is: the paid skill marketplace / agent services marketplace, above. The
  storage-backed cases (personal file vault, privacy-preserving notary) and the
  messaging-backed ones cannot be claimed, because **the storage and messaging
  skills have never been exercised against a running node** — they are written
  against the real Storage and Delivery ABIs, read off the module headers rather
  than guessed, and they compile, but compiling is not working. `docs/skills.md`
  says so in its own status table.

- [x] **MET — Three separate agents deployed on LEZ testnet, one per default skill
  category, each with a demonstrated, reproducible deployment and evidence.**
  Storage, messaging and blockchain, each with its own shielded account, its own
  public receiving account and its own anchored envelope. The table below is
  **generated** from `artifacts/agents.tsv` and the chain, read by column name:

<!-- BEGIN GENERATED agents -- scripts/submission-evidence.py; do not edit by hand -->

Read from `artifacts/agents.tsv` **by column name**, then checked against the chain. Limits below are the ones the state machine holds, not the ones the manifest claims; where they differed this section would say so and the generator would exit non-zero.

| category | agent | policy account | per-tx | per-period | period | window | spent | `create_policy` |
|---|---|---|---|---|---|---|---|---|
| storage | `9Xpkkvos…` | `6FscNXjN…` | 50 | 500 | 1,000 blocks | 0 | 0 | [`6857ba23…631fe7d4`](https://explorer.testnet.lez.logos.co/transaction/6857ba2378a84ba51618582e852e3827a872e3ea85f17de76bdb45b1631fe7d4), block 8868 |
| messaging | `GpRdooEW…` | `7HH46tXh…` | 25 | 250 | 1,000 blocks | 8,000 | 50 | [`ce557a0a…278e1918`](https://explorer.testnet.lez.logos.co/transaction/ce557a0a8adc517b60496c35514e269fff92a4393b90bef41ce10916278e1918), block 8876 |
| blockchain | `A7UBoMbS…` | `2RK4dPwz…` | 200 | 1,000 | 1,000 blocks | 0 | 0 | [`2f6b481c…ecec5eda`](https://explorer.testnet.lez.logos.co/transaction/2f6b481cffde2adaeed9442c19599c939d97da0c930b70b45d97ac34ecec5eda), block 8884 |

Each `create_policy` above was confirmed present in the block named and absent from both neighbours. The limits are the chain's own copy: the address of a policy account is `PDA(SHA256(owner ‖ agent ‖ per_tx ‖ per_period ‖ period_blocks))`, so raising a limit does not edit this record — it names a different address that `create_policy` never initialised, and the state machine rejects the spend before the program body runs. `window` and `spent` are the halves only the owning program may write.

`artifacts/anchored.tsv` records every `(program, anchor)` pair this repository has ever written, keyed on the program, which is why a redeploy shows up in it rather than overwriting it. Under the program deployed above there are 6:

| what | agent | transaction | block |
|---|---|---|---|
| `claim_agent` | `9Xpkkvos…` | [`88f9ec5c…dc292dd0`](https://explorer.testnet.lez.logos.co/transaction/88f9ec5c377dceeb5005336ecf358d778a30dc39d2ea49b1c166332cdc292dd0) | 8859 |
| `create_policy` | `9Xpkkvos…` | [`6857ba23…631fe7d4`](https://explorer.testnet.lez.logos.co/transaction/6857ba2378a84ba51618582e852e3827a872e3ea85f17de76bdb45b1631fe7d4) | 8868 |
| `claim_agent` | `GpRdooEW…` | [`78ce43c9…adaa126c`](https://explorer.testnet.lez.logos.co/transaction/78ce43c977bcf9956d3c8f42836e65b2fc8159a18e04836214756cd0adaa126c) | 8875 |
| `create_policy` | `GpRdooEW…` | [`ce557a0a…278e1918`](https://explorer.testnet.lez.logos.co/transaction/ce557a0a8adc517b60496c35514e269fff92a4393b90bef41ce10916278e1918) | 8876 |
| `claim_agent` | `A7UBoMbS…` | [`0dd4e49e…e52921a2`](https://explorer.testnet.lez.logos.co/transaction/0dd4e49eeecac1366baf7a81a93639cadd8b6e013984979d99ebf63ae52921a2) | 8883 |
| `create_policy` | `A7UBoMbS…` | [`2f6b481c…ecec5eda`](https://explorer.testnet.lez.logos.co/transaction/2f6b481cffde2adaeed9442c19599c939d97da0c930b70b45d97ac34ecec5eda) | 8884 |

A further 3 rows belong to superseded programs — `a780003b…` (3). Those transactions are still on chain and still resolve, but the policy accounts they created were derived from a different ImageID and no longer exist under the current one. They are evidence of what was redeployed, not of what is enforceable today.
<!-- END GENERATED agents -->

  Stronger than transaction presence: each policy account comes back owned by
  the policy program's own `ProgramId`, while the PDA of an agent nobody
  anchored comes back with the default owner, and the `dedede…` control returns
  `null` on every pass. Reproduce with `./scripts/deploy-agents.sh`, which is
  deliberately not idempotent — a second run derives the same address and
  `create_policy` refuses it, because `#[account(init, …)]` will not take an
  account that is not in its default state, which is the single-use guarantee
  working. The script reports it as already-anchored via
  `artifacts/anchored.tsv`, keyed on `(program, agent_id)`.

- [x] **MET — Full documentation and a clean public repository.**
  Skill interface spec [`docs/skills.md`](docs/skills.md), deployment guide
  [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md), architecture
  [`docs/architecture.md`](docs/architecture.md), security model
  [`docs/security-model.md`](docs/security-model.md), owner/app integration
  [`docs/basecamp.md`](docs/basecamp.md), CU accounting
  [`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md), and
  [`docs/limitations.md`](docs/limitations.md), which is where anything that does
  not work is written down first, plus the A2A transport binding spec
  [`docs/a2a-binding.md`](docs/a2a-binding.md). Public repo, dual MIT/Apache-2.0.
  `docs/DEPLOYMENT.md` was regenerated against the current program at this commit,
  after a window in which it and the manifests described two different deployments —
  worth noting because that window is exactly what a redeploy costs, and it will
  reopen if the settlements are re-run without regenerating it again.

### Usability

- [x] **MET — A documented skill interface that adds skills without modifying the
  core module.**
  `logos::agent::ISkill` — `name()`, `parameterSchema()`, `invoke(json)` — plus
  `registerSkill()`, specified in [`docs/skills.md`](docs/skills.md). The interface
  is defensive by design because third-party code is the whole point: `name()` is
  called *before* the module takes its lock, so a skill that calls back in cannot
  deadlock a non-recursive mutex; a throwing `name()` is caught and reported rather
  than escaping into the host; a duplicate name is **refused rather than
  overwritten**, so one plugin cannot shadow another's `wallet.send`; and `invoke()`
  drops the lock before dispatching and rejects a non-JSON return, so a skill
  cannot corrupt a document the caller splices its answer into.

- [ ] **UNMET — The owner-facing interface is accessible from the Logos app
  (Basecamp) via the owner channel; local build instructions and loadable assets
  provided.**
  The loadable asset ships (`module/agent.lgx`) and is verified to load — both by
  `QPluginLoader` against the exact packaged artefact and by real `liblogos_core`
  from an installed Basecamp, with the build instructions in
  [`docs/basecamp.md`](docs/basecamp.md). But the criterion says *via the owner
  channel*, and the owner channel is **not reachable from Basecamp**: wiring it
  needs `registerBuiltinSkills`, which takes `std::function` ports that cannot cross
  a remoteable boundary (`docs/basecamp.md`). A Basecamp-loaded module registers its
  built-ins itself with empty ports, so it offers the full card and the owner channel
  has nothing behind it.

### Reliability

- [ ] **UNMET — Recovers from transient failures (network interruptions, node
  restarts) without losing pending task state.**
  `TaskStore` has `snapshot()`/restore precisely so pending task state can outlive a
  restart, and the module lets the host own the store for that reason. No restart
  recovery has been demonstrated: no node has been restarted under a running agent,
  and no test drives a snapshot back into a fresh module and resumes a pending task.
  A persistence layer (`module/src/task_persistence.*`) is in progress and untracked
  at this commit, so it is not credited here.

- [ ] **UNMET — Above-threshold transactions that fail to reach the owner are not
  executed; the agent retries notification before timing out and reports the
  failure.**
  The behaviour is implemented and tested fail-closed against a fake owner that can
  be made silent, late or hostile on demand: a channel that will not open is
  reported rather than pretended, an approval naming different terms is refused, and
  a silent owner is terminal instead of a quiet fallback to acting alone. Marked
  UNMET for two independent reasons: those assertions did not execute in the latest
  CI run, and the above-threshold path cannot run on chain at all while the owner
  cannot approve.

- [ ] **UNMET — Skill failures are isolated: a failing skill does not crash the
  module or affect other concurrently running skills.**
  `invoke()` wraps every dispatch in `catch (const std::exception &)` and `catch
  (...)`, returns the failure as JSON naming the skill, and a skill that returns
  non-JSON is rejected rather than propagated; a skill that throws from `name()`
  during registration costs that skill, not the start. Marked UNMET on evidence, not
  on design: this is exactly what the skills suite asserts, and that suite did not
  compile in the latest CI run.

### Performance

- [x] **MET — Document the CU cost of each on-chain operation.**
  [`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md) answers this and is
  candid about the premise: **LEZ v0.2.4 does not meter compute units.** Grepping the
  pinned revision for the term returns nothing, and the `GasConfig` struct in the
  wallet is declared and referenced nowhere else — a fee model's shape with no fee
  model behind it. So nothing is labelled "CU", because the conversion would have to
  be invented. What is measured instead are the three real budgets: cycles against
  `MAX_NUM_CYCLES_PUBLIC_EXECUTION` (32M), chained calls against
  `MAX_NUMBER_CHAINED_CALLS` (10), and bytes on the wire, read back from the
  sequencer per settlement rather than estimated — the generated settlement
  table above prints the figure, and this sentence deliberately does not repeat
  it, because the number written here was 270,566 for three deployments after
  it had stopped being true. Cycle counts are measured by executing the
  **deployed** binary, not a rebuild.

### Supportability

- [x] **MET — The agent module is deployed and tested on LEZ devnet/testnet.**
  Program, three anchors and three settlements all live on the public testnet, each
  re-verified for this document with a null-returning control.

- [x] **MET — End-to-end integration tests run against a LEZ sequencer (standalone
  mode) and are included in CI.**
  `.github/workflows/e2e-local-sequencer.yml` builds the LEZ workspace at pinned
  revision `47eba25`, installs `r0vm` 3.0.5, and runs the full lifecycle — deploy,
  anchor, spend inside the envelope, be refused outside it — with
  `RISC0_DEV_MODE: 0`. It has **no skip path**, deliberately: a competing submission
  was closed with "the standalone-sequencer E2E did not run in CI; the job completed
  through its explicit skip path". Last green run on this branch's lineage:
  [31867735056](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31867735056),
  `success`, commit `5a52efe`, 2026-08-15 05:45 UTC, 1h 03m 58s. Two caveats,
  both stated because they will be noticed. That run is a long way back —
  `git rev-list --count 5a52efe..HEAD` says how far, and an earlier version of
  this line guessed "four", which was wrong by an order of magnitude. And the
  workflow runs on a daily schedule rather than on push, so a green badge here
  is never evidence about `HEAD`. A more recent green run of the same workflow
  exists ([31890967866](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31890967866),
  18m 49s) but it is on the `e2e-preflight` branch at a commit that is **not** an
  ancestor of this one, so it is not evidence about this tree either.

- [x] **MET, with a caveat — CI must be green on the default branch.**
  CI is green on the published branch. Do not quote a run id from this
  paragraph — "latest" moves, and the id written here was already two runs stale
  by the time anyone read it. Ask instead:
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.
  One red run is worth recording because of *how* it failed
  ([31882516164](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31882516164),
  `failure`): `messaging_skills.h` and `storage_skills.h` used `std::uint8_t` /
  `std::int64_t` without `<cstdint>`, which the runner's libstdc++ 14 no longer
  supplies transitively, and the job died at its **first** compile step — so six C++
  suites did not run at all while the badge said only "one job failed". The fix was
  two include lines. The caveat that keeps this from being a clean MET: the
  commits at `HEAD` are not on the published branch and have not been through
  CI. See blocker 2.

- [ ] **UNMET — A README documenting end-to-end usage: deployment steps, agent
  configuration, and step-by-step instructions for deploying and interacting with
  the agent via CLI and the Logos app owner channel.**
  The README covers the demo, the layout and the evidence bar, and the deployment
  steps are in `docs/DEPLOYMENT.md`, but the owner-channel interaction walkthrough
  the criterion asks for describes a path that does not work from Basecamp yet.

- [x] **MET — A reproducible end-to-end demo script that works against a real local
  sequencer with `RISC0_DEV_MODE=0`.**
  `scripts/e2e-local-sequencer.sh`, green in CI as above. Separately,
  `./scripts/demo.sh` runs from a clean clone with only a Rust toolchain — no funded
  account, no keys, no local sequencer — and is green at this commit.

- [ ] **UNMET — A recorded video demo showing terminal output confirming
  `RISC0_DEV_MODE=0` was active.**
  Not recorded. Blocker 1.

**Tally: 10 MET, 13 UNMET, of the 23 criteria the prize lists.** The commit this
tally describes is the one recorded at the top of this document, and it is
recorded there only — a count anchored to a commit id in two places is two
places to forget.

## FURPS Self-Assessment

### Functionality

The agent holds a shielded LEZ account, signs its own transactions, and spends
under a ceiling the chain keeps in state only its own program may write. All twenty-one default skills are
implemented and registered — `meta.skills`, the last one missing, was documented
in three headers while `invoke()` refused it, and is now asserted against the
loaded binary rather than against the source. The A2A coordination
path — card, discovery, task lifecycle, settlement — is the part that has actually
run on the public testnet, twice, unattended.

The limits are not incidental. The **owner cannot approve an above-threshold
spend** after anchoring a policy, which removes half of the spending-threshold
design and is the most serious open defect here. Two others have just closed and
are described as closed rather than as achievements: `spend` used not to bind the
policy to the account presenting it — a funded account could present any anchored
policy, including one anchored for a different agent with a larger envelope — and
the per-period ceiling used to be advisory, checked against a number the caller
passed in. Both are fixed, redeployed and re-anchored, and the refusals are
asserted against the *deployed binary* rather than a rebuild, and a third settlement
has since landed under the fixed program with the period total written on chain.
**Storage and messaging skills have never touched a live node.** And **no
model has ever been run against the inference port**: `StubLocalBackend` is a rule
table with an honest name, `OpenAiCompatibleBackend` has never made a request that
left this repository, and the acceptance decision is not even in the demo path —
`scripts/a2a-task.sh` decides with a shell `if`. What the inference work
demonstrates is the seam and its failure behaviour, which is worth something, but it
is not "an agent driven by a model".

### Usability

Two audiences. A **skill author** gets a three-method interface, a registration call
that refuses to let one plugin shadow another's `wallet.send`, and a documented
spec — this is the strongest usability story here. An **owner** gets a CLI that
deploys and anchors three agents reproducibly, and a `.lgx` that genuinely loads in
Basecamp. What the owner does not get is the thing the prize actually asks for:
deployment through Logos Core headless in one command, and a conversation with the
agent from the Logos app. The owner channel is built and tested, and is not
reachable from the app.

### Reliability

The design is fail-closed in the places where failing open would move money: an
unreachable owner is terminal rather than permission to act alone, an approval that
names different terms is refused, a limit that will not parse becomes zero (which
holds every spend and declines every offer), and an unreachable inference backend
declines. Skill dispatch is exception-isolated and lock-free at the call site.
`scripts/a2a-task.sh` refuses to write its own manifest unless the recipient's
balance moved by exactly the price — a rule added because an earlier version
produced confirmed on-chain proofs that a policy permitted 25 LEZ and moved nothing.

Against that: restart recovery is designed for and not demonstrated, and — the
honest headline — the entire C++ suite did not run in the latest CI, so at this
commit these are design claims plus a previously-green run, not current evidence.

### Performance

No fees exist to measure on LEZ v0.2.4, so the document measures the budgets that do
exist rather than inventing a CU number. A settlement's size on the wire is read back
from the sequencer rather than estimated, and is printed in the generated settlement
table above rather than restated here. Cycles are measured against the
32M public-execution cap by running the deployed binary; the settlements take the
privacy-preserving path, which is bounded by the prover rather than by that constant.
The real bottleneck is proving time, and the real operational cost is that anchoring
is one-shot per signer.

### Supportability

Six C++ suites, a Rust policy crate with adversarial tests, and an end-to-end job
against a real standalone sequencer with `RISC0_DEV_MODE=0` and no skip path. The CI
file documents, in comments, exactly which four suites do **not** run there and why —
Qt and an installed Basecamp for the two load tests, a Nim and `librln` build for the
node drives — because a suite silently absent from CI is indistinguishable from one
that was never written.

CI is green on the published branch as of the latest run, after a red one whose
failure mode is the more useful fact: a missing `<cstdint>` killed the skills job at
its first compile step, so six suites did not run while the summary said only that
one job had failed. A job that fails early and a job that passes having tested
nothing look similar from the outside, which is why the workflow asserts on the
`SKIPPED` banner as well as on exit codes. The four commits at `HEAD` have not been
through CI yet (blocker 2). Debuggability is otherwise deliberate —
every failure path returns JSON naming the skill and the half that failed, and
`share` takes both ports specifically so it can say whether storage or delivery
failed rather than blaming the wrong one.

## Supporting Materials

- 🎥 **VIDEO DEMO — PLACEHOLDER, NOT YET RECORDED**
  `<<< VIDEO URL TO BE INSERTED HERE >>>`
  Must be a narrated walkthrough — a silent screencast is explicitly insufficient —
  covering ≥3 illustrative use cases, with terminal output visible confirming
  `RISC0_DEV_MODE=0`, against the **public testnet** rather than a localnet. Only
  one use case is currently demonstrable end-to-end (blocker: see criterion 9).

- **Live evidence, re-derivable:** [`artifacts/agents.tsv`](artifacts/agents.tsv),
  [`artifacts/anchored.tsv`](artifacts/anchored.tsv),
  [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv),
  [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md). Read the TSVs **by column name**.
- **Settlements, anchors and the deployed program:** every hash, block, balance
  and explorer link is in the three generated sections above, with the checks
  that were run to get them. They are deliberately **not** repeated here: this
  bullet used to restate them, and restating a fact in a second place is how
  this document came to cite three transactions its own deployment guide
  disowns. Regenerate with `./scripts/submission-evidence.py`, or verify the
  document still matches the chain with
  `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md`.
- **Documentation:** [architecture](docs/architecture.md) ·
  [skill interface](docs/skills.md) · [security model](docs/security-model.md) ·
  [deployment](docs/DEPLOYMENT.md) · [Logos app integration](docs/basecamp.md) ·
  [CU accounting](docs/benchmarks/cu-budget.md) ·
  [**limitations**](docs/limitations.md) · [stack recon](docs/recon.md)
- **CI:** [all runs](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions)
  · last green E2E vs a real sequencer with `RISC0_DEV_MODE=0`:
  [31867735056](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31867735056)
  · latest CI run on the published branch, **green**:
  [31883389383](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31883389383)
  · the red run before it, whose `<cstdint>` compile failure silently took six
  suites with it:
  [31882516164](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31882516164)
- **Reproduce from a clean clone:** `./scripts/demo.sh` (no keys, no funds, no
  sequencer) · `./scripts/deploy-agents.sh` · `./scripts/a2a-task.sh` ·
  `./scripts/e2e-local-sequencer.sh`

Read [`docs/limitations.md`](docs/limitations.md) before the rest. It is written to
say what does not work before anyone has to discover it, and it contains the two
defects that most affect a reviewer's reading of this submission: the owner cannot
approve a spend after anchoring a policy, and a shielded agent can pay but cannot be
paid at its shielded account.

## Terms & Conditions

By submitting this solution, I confirm that I have read and agree to the
[Terms & Conditions](../TERMS.md).
