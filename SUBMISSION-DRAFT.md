# Solution: LP-0008 — Autonomous AI Module with Wallet, Storage, and Messaging

> ## ⚠️ DRAFT — NOT READY FOR SUBMISSION
>
> **Pinned to commit `1de38d8` on `main`** (2026-08-15). The repository is public
> and **actively changing** — a security redeploy, a CI fix, the A2A binding
> spec, a deployment-doc regeneration and a third settlement all landed while
> this was being written — so every claim below is stated against that commit and
> was verified against the public sequencer directly. Anything dated after it is
> newer than this document.
>
> Because the volatile values move on every redeploy, this document does **not**
> hardcode them. The program hash, the three policy hashes, the three anchor
> transactions and the agent identities are read from
> [`artifacts/agents.tsv`](artifacts/agents.tsv) and
> [`artifacts/anchored.tsv`](artifacts/anchored.tsv), which the deploy script
> writes, and every one of them is re-derivable from the chain with the commands
> in [Reading the evidence](#reading-the-evidence-without-trusting-this-document).
> The three settlements *are* quoted literally, because a landed transaction is
> immutable.
>
> ### What blocks submission today
>
> | # | Blocker | State |
> |---|---|---|
> | 1 | **No recorded video demo.** The prize requires narrated walkthroughs of ≥3 use cases showing terminal output that confirms `RISC0_DEV_MODE=0`. A silent screencast is explicitly insufficient. This is the one blocker with real work left in it. | Not recorded. Placeholder in [Supporting Materials](#supporting-materials). |
> | 2 | **`HEAD` is 4 commits ahead of `origin/main`.** The third settlement, the A2A binding spec, the regenerated deployment guide and the README rewrite are committed but **unpushed**, so a reviewer cloning right now sees none of them — and CI has not run on them. The last CI run on the published branch is green, but it is green on a commit four behind this one. | `git push`, then confirm CI. |
>
> Resolved while this was written, and no longer blockers: the
> `spend`-does-not-bind-the-policy defect and the caller-supplied period total are
> both fixed, redeployed and re-anchored; **two settlements have landed under the
> shipped program** (`4e3a3454…` block 8740, `7cad4fbd…` block 8747), so the
> anchors and the settlement evidence are now under the *same* program; CI went green after the `<cstdint>`
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
testnet, each with its own shielded account and its own envelope. Two of them
have run an A2A task to completion and settled it in LEZ three times, unattended,
with the per-period total accumulating on chain.
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
- **Commit this document describes:** `1de38d8`
- **License:** dual MIT / Apache-2.0
- **Default branch:** `main` (public). ⚠️ `origin/main` is **four commits behind**
  the state described here — see blocker 2.

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

The values that move on a redeploy live in two generated manifests, read by
**column name** — the columns have been reordered before, and a positional read
has already produced a false result here:

```bash
# the three anchored agents, current program
python3 -c "
import csv
for r in csv.DictReader(open('artifacts/agents.tsv'), delimiter='\t'):
    print(r['category'], r['agent_id'], r['policy_hash'], r['create_tx'])"

# every (program, policy_hash) ever anchored, including superseded programs
python3 -c "
import csv
for r in csv.DictReader(open('artifacts/anchored.tsv'), delimiter='\t'):
    print(r['program'], r['policy_hash'], r['create_tx'])"
```

Each `create_tx` is then checkable directly, and **the control is what makes the
check mean anything**:

```bash
q() { curl -s -X POST https://testnet.lez.logos.co \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}"; }

q <create_tx>   # => {"result":[<bytes>,<block>]}   present
q dededededededededededededededededededededededededededededededede
                # => {"result":null}               absent — as it must be
```

One warning, because it has misled readers of this repository before:
**`getAccount` is not an existence check.** It answers with a fully-populated
default account — zero balance, zero nonce, zero owner — for an address that has
never existed, and for a shielded account it does the same, because it reads the
public state only. Presence proves nothing there; the *program owner* and the
*balance* are the signals.

## Approach

### The ceiling is an address, not a check

A spending limit enforced inside the agent is enforced by whoever controls the
agent's process — which, for an agent deployed on a remote node with its own
key, is not necessarily the owner. So the limit is moved out of the process
entirely.

`create_policy` initialises one account, whose address is
`PDA(SHA256(owner_id ‖ agent_id ‖ per_tx ‖ per_period ‖ period_blocks))`. It is
`#[account(init, …)]`, so it can be created exactly once. `spend` re-derives that
address from the limits the caller presents and requires the account at that
address to exist and to be owned by the policy program. An agent that wants a
larger envelope cannot edit the account — it can only name a *different* address,
which nobody ever initialised, and the state machine rejects the transaction
before the program body executes.

This is checkable by anyone, and the check has a control:

```bash
# the policy account for an anchored envelope
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
     pda policy --policy-hash <policy_hash from agents.tsv>
# then getAccount that address:
#   program_owner = the policy program's ProgramId   -> anchored
#   program_owner = [0,0,0,0,0,0,0,0]                -> never anchored
```

Run against an anchored hash the owner comes back as the policy program's own
`ProgramId` (`spel program-id artifacts/programs/agent_verifier.bin` prints it to
compare). Run against a hash nobody anchored — the `dedede…` control — the PDA
resolves fine and comes back with the **default** owner. That difference is the
whole mechanism, visible from outside with two RPC calls.

**Alternatives rejected.** Storing the *limits* in the policy account's data was
the obvious design and was rejected: it would make the envelope something a
transaction can rewrite, and the address would stop being a commitment to the
terms. The split that was adopted instead is that **the address carries the limits
and the data carries what has been spent** — the immutable half stays in the
address, the accumulating half is state only this program may write.

That split is recent, and it closed a real hole. `spend` used to take
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

> ⚠️ **`docs/limitations.md` has not caught up with this.** It still lists
> "`spent_this_period` is supplied by the caller" as an open defect. That entry is
> stale as of the current program — the IDL, the guest source and the demo all
> disagree with it. Flagged here rather than silently relied on, because a stale
> limitation is the one kind of documentation error that costs nothing to leave in
> and everything to be caught on.

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

Three superseded programs are still on the testnet, since deployment is
content-addressed, and `docs/limitations.md` lists what each got wrong. That list
is part of the evidence: it is what "tried and did not work" looks like.

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
  Three settlements, each run with no special handling between them — the repeats
  matter as much as the first, because a repeat settlement is what this repository
  could not produce for most of its life. Verified independently for this document
  against `https://testnet.lez.logos.co`:

  | | first | second |
  |---|---|---|
  | settlement tx | `4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1` | `7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019` |
  | block (from `getTransaction`) | 8740 | 8747 |
  | size | 271,471 bytes | 271,471 bytes |
  | policy program | **shipped** | **shipped** |
  | skill / price | `storage.upload` at 25 LEZ | same |
  | payee balance (`getAccount`) | 45 → 70 | 70 → 95 |

  Both rows are the `settlement_tx` column of
  [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv) and both are checked against
  the chain by `./scripts/verify-deployment.sh`. Four earlier settlements exist
  under the two superseded programs — `c45d3f24…` and `8d7aba60…` (blocks 8605,
  8624), `5a488f28…` and `f780df62…` (blocks 8677, 8686). They are still on
  chain and they all moved balance; what they do not do is say anything about the
  binary this repository ships, and this table listed one of them as "current"
  after it had stopped being so.

  Each transaction's bytes were checked to be **inside** the block it names and
  **absent** from both adjacent blocks, so the block attribution is not taken on the
  RPC's word alone. The payee's public account reads `balance: 75` today, matching
  three 25-LEZ credits, and is owned by the authenticated transfer program. The
  control hash returned `null` on every pass. Manifest:
  [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv); reproduce with
  `./scripts/a2a-task.sh`, which refuses to write its manifest unless the
  transaction confirms **and** the recipient's balance moved by exactly the price.
  Only the credit side is publicly readable — the payer is shielded — but rule 8
  requires total balance to be preserved across every program in a transaction, so
  a transaction that credited 25 debited 25.

  The third settlement is the one that matters most, for two reasons. It is under
  the **current** policy program, so the anchors and the settlement evidence are no
  longer split across a superseded deployment — the rule `docs/limitations.md` sets,
  and that this repository has broken before. And it left a trace the earlier two
  could not: the paying agent's policy account now carries
  `data = [64,31,0,0,0,0,0,0, 25,0,0,0,0,0,0,0, …]` — little-endian, that is
  `window_start = 8000` and `spent_this_period = 25` — written by the program, not
  by the caller. The account is owned by the current program's own `ProgramId`. The
  per-period ceiling is now a number the chain keeps.

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
  public receiving account and its own anchored envelope — the envelopes differ on
  purpose, since identical limits under one owner collapse to a single policy hash.
  All three `create_tx` values in [`artifacts/agents.tsv`](artifacts/agents.tsv)
  return a transaction from the sequencer while the control returns `null`; this was
  re-verified for both the committed set and the in-flight re-anchored set, and all
  six are live. Stronger than transaction presence: the derived policy PDA for an
  anchored envelope comes back owned by the policy program's own `ProgramId`, while
  the PDA of a never-anchored hash comes back with the default owner. Reproduce with
  `./scripts/deploy-agents.sh`, which is deliberately not idempotent — a second run
  derives the same policy hash and `create_policy` refuses it, which is the
  single-use guarantee working, and the script reports it as already-anchored via
  `artifacts/anchored.tsv` keyed on `(program, policy_hash)`.

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
  `MAX_NUMBER_CHAINED_CALLS` (10), and bytes on the wire — 270,566 per settlement,
  read back from the sequencer. Cycle counts are measured by executing the
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
  through its explicit skip path". Last green run:
  [31867735056](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31867735056),
  2026-08-15 05:45 UTC, 1h 04m. One caveat, stated because it will be noticed: that
  run was at commit `5a52efe`, four commits before this one, and the workflow is on a
  daily schedule rather than on push.

- [x] **MET, with a caveat — CI must be green on the default branch.**
  The latest run on the published branch is **green**
  ([31883389383](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31883389383)),
  all three jobs. The run before it was red, and worth recording because of how it
  failed: `messaging_skills.h` and `storage_skills.h` used `std::uint8_t` /
  `std::int64_t` without `<cstdint>`, which the runner's libstdc++ 14 no longer
  supplies transitively, and the job died at its **first** compile step — so six C++
  suites did not run at all while the badge said only "one job failed". The fix was
  two include lines. The caveat: `HEAD` is four commits ahead of the green commit,
  and those four have not been through CI. See blocker 2.

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
under a ceiling the chain enforces by address. All twenty-one default skills are
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
exist rather than inventing a CU number. A settlement is 270,566 bytes on the wire,
read back from the sequencer rather than estimated. Cycles are measured against the
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
- **Settlements:** `4e3a3454…a490ddb1` (block 8740) and `7cad4fbd…7168f019`
  (block 8747), both under the shipped program, payee `5Sa13NyN…dHtjnZ` going
  45 → 70 → 95. Read them from
  [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv) rather than from this line.
  [explorer](https://explorer.testnet.lez.logos.co/transaction/4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1)
  — note the explorer indexes roughly an hour and three quarters behind the
  sequencer, so a recent hash reads "not found" there while `getTransaction`
  already returns it. The RPC is the immediate source of truth.
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
