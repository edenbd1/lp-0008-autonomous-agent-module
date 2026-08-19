# Solution: LP-0008 — Autonomous AI Module with Wallet, Storage, and Messaging

> ## This is the working document, not the submission
>
> **The submission is [`solutions/LP-0008.md`](solutions/LP-0008.md)** — that is
> the file the pull request adds to the prize repository, and it is the one to
> read. This document is where the argument was worked out first. It is kept,
> and kept in CI, because its evidence sections are the ones
> `scripts/submission-evidence.py` generates from the chain and re-checks on
> every push, so it is the file that catches a figure going stale.
>
> It said **DRAFT — NOT READY FOR SUBMISSION** at the top until 2026-08-17,
> which was true when written and stopped being true without anybody moving it
> — the same failure this banner goes on to describe about pinned commit
> hashes, one level up.
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
> reader for checking the manifests without trusting the author both crashed,
> because they named a column that had been renamed. None of it was noticed for
> weeks. A hand-written fact does not announce that it has gone stale, and on a
> content-addressed chain every redeploy moves all of them at once.
>
> ### What blocks submission today
>
> | # | Blocker | State |
> |---|---|---|
> | 1 | **No recorded video demo.** The prize requires a narrated walkthrough of ≥3 use cases showing terminal output that confirms `RISC0_DEV_MODE=0`. A silent screencast is explicitly insufficient. | **Departed 2026-08-17.** One walkthrough, 21 m 57 s, four use cases, published with the submission and linked from [Supporting Materials](#supporting-materials). |
>
> This table has now held four different blockers, and each departure is recorded
> rather than quietly deleted, because a stale blocker is more corrosive than a
> stale claim — it tells a reader that the tree they cloned is not the tree
> being described, which invites them to distrust everything else, including the
> parts that are checkable. It read "`HEAD` is ahead of `origin/main` and
> unpushed"; `HEAD` **is** `origin/main`. It then read "the end-to-end run
> against a real local sequencer is not green on `main`"; that run has since
> completed green on `main` twice over, by dispatch and by schedule. It then read
> "`CI` is red on `main`. Five of six jobs pass", and named two defects: the Agent
> Card negative control rewrote the price to the literal `1` while the card had
> been re-signed *at* `1`, so the control mutated nothing; and the paying agent's
> period-9000 ledger read `2` on chain while `artifacts/a2a-task.tsv` recorded
> prices summing to `1`. **Both are fixed in the tree and both fixes are checked
> against the chain**, the workflow has ten jobs rather than six, and the eight
> that predate this pass are green. The evidence is in the CI criterion under
> [Supportability](#supportability), and the command that answers it without
> trusting this document is
> `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.
>
> Resolved while this was written, and no longer blockers: the
> `spend`-does-not-bind-the-policy defect, the caller-supplied period total and
> the caller-chosen policy address are all fixed, redeployed and re-anchored;
> **settlements have landed under the shipped program**, so the anchors and the
> settlement evidence are under the *same* program — the generated table says
> which, and how many, and this line deliberately does not, because the last
> three versions of it named transactions that had stopped being current; the
> Agent Card is signed (BIP-340 Schnorr, `scripts/sign-agent-card.py`,
> which self-verifies before emitting); `docs/a2a-binding.md` specifies the
> transport binding; `docs/DEPLOYMENT.md` has been regenerated; the agent module
> has a **window** in the Logos app and an owner has approved a spend from it;
> and a module Logos Core **loads** now builds its own Logos Delivery ports, so
> two of them discovered each other's signed Agent Cards on the public network,
> ran an A2A task lifecycle to `completed` on each other's status updates, and
> an owner approved and denied a spend over Logos Messaging.
>
> CI is on that list now, and it was not on the last three versions of it. The
> `CI` workflow defines **ten** jobs and the eight that existed when this was read
> are green on `main`: the last four completed,
> uncancelled runs each return eight jobs and eight `success` conclusions, and the
> two added since are why the command below is the answer rather than this. That is
> written here as an observation about the commits
> those runs ran on, not as a property of the repository, because it has been red repeatedly
> for real reasons — a job that could not build `spel`, a coverage floor added by
> one piece of work reading a line of output added by another, a negative control
> a re-signed Agent Card made vacuous. Every one was a gate catching something,
> and a green run is only ever a statement about the commit it ran on. So the
> command is given rather than a badge:
> `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main
> --limit 1`. Note that `ci.yml` sets `cancel-in-progress: true`, so a run
> superseded by the next push reports `cancelled` — which is neither red nor
> green, and is not evidence either way.
>
> The Success Criteria Checklist marks unmet criteria **UNMET**, including
> criteria for which working, tested code exists. Code existing is not the
> criterion, a test CI skips is not evidence, and neither is a document
> describing something the repository does not do.
>
> It also marks criteria **MET** where the prize's own sentence is satisfied,
> even when a related defect is disclosed elsewhere in this repository. Being
> stricter than the criterion is not caution; it is the same inaccuracy as
> overclaiming, pointed the other way, and it hides real work behind an empty
> box. Each verdict below quotes the words it was judged against.

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
have **paid each other for a priced skill, unattended**, with the per-period
total accumulating on chain, and one of those payments was made **by a module a
host loaded**, in the same call that discovered the payee's card and opened the
task: `./scripts/delivery-in-plugin.sh settle`. That sentence used to read "have
run an A2A task to completion and settled it in LEZ", which implied one flow
where there were two; the settlement and the discovery are now genuinely one, and
so is the rest of the lifecycle. The same invocation walks each agent's own
`TaskStore` `submitted → working → completed` on status updates the *other*
account published — `agent.update` publishes them, `agent.poll` ingests them —
so the peer that receives a request now does drive it to a terminal state over
the wire. What is still separate is named in the checklist entry rather than
smoothed over here: nothing **dispatches**, so the decision to serve is the
host's rather than the module's.

Agent-to-agent coordination is A2A-shaped: cards carry the A2A schema plus an
`x-logos` extension for the price and payment address that vanilla A2A has no
field for. Two modules that a host **loaded** have published those cards to a
public Logos Messaging topic and discovered each other's — each configured to
accept only a card naming the *other* account, because a Delivery node receives
its own published messages and a one-process demonstration of discovery is not
one.

**What is not delivered** is stated in the checklist and in
[`docs/limitations.md`](docs/limitations.md), and it is substantial: the owner
that anchored while unclaimed can never approve an above-threshold spend afterwards; the
**storage** skills now run against a live node from inside the module's own
process — it `dlopen`s `libstorage` and opens one on
`meta.configure("storage","on")` — and what they lack is a committed transcript
and a CI job rather than a node; nothing **dispatches**
an inbound request to the skill it names, so a serving agent is this module plus
a host that reads the request, decides, and publishes the states the work moves
through; and no model has ever been run against the inference port.

Two clauses have left that sentence since it was written, and both are recorded
rather than deleted: it ended "and there is no video" until one was published on
2026-08-17, and it said the storage skills had never reached a node until the
module gained a `StorageRuntime` of its own.

Two clauses have left that sentence, and both are recorded rather than quietly
dropped. The first was "no agent has yet *served* another agent's task through to
a terminal state, so the far side reads a request and never answers it". It
stopped being true: `agent.update` and `agent.poll` carry a peer's task
`submitted → working → completed` on frames the other account published, in the
same run as the payment. What is left of it is the narrower fact above — that
nothing dispatches — which is a smaller claim and the accurate one.

The second was the messaging half, and it went for the same reason:
`messaging.send`, `messaging.join`,
`messaging.receive`, `agent.discover`, `agent.task` and `agent.subscribe` all run
from a loaded plugin against real Logos Delivery nodes on the public network
(`./scripts/delivery-in-plugin.sh`).

## Repository

- **Repo:** <https://github.com/edenbd1/lp-0008-autonomous-agent-module>
- **License:** dual MIT / Apache-2.0
- **Default branch:** `main` (public), and what you cloned is what this document
  describes — there is no unpushed tail. Everything below is checkable from that
  clone plus the public sequencer; where a fact moves (CI, the e2e run, the
  per-period total) the command to ask is given instead of a number.

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
commits earlier, so a reader who ran them got `KeyError` and no reason to
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
| the deployed bytes | the transaction embeds this repository's copy of `agent_verifier.bin` verbatim |
| shipped ImageID, read off the on-chain `storage` anchor | `778a9341a00de46c4c056ac63a66f63156b068e61cce7f155a2b495e670c4661` |
| its ProgramId | `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647` |

Every step of that is a chain fact, and none of it depends on a tool being installed. The deploy transaction is *derived* from the committed bytes rather than quoted, it resolves, and it contains those bytes — so the file in this repository is the program that was deployed. The anchor transaction that created these policy accounts names the ImageID it called, and `getAccount` reports that same ProgramId as the `program_owner` of every one of them. **The binary in this repository is the program enforcing these envelopes on chain.**

`spend` moves no balance itself — LEZ rule 5 refuses any post-state that decreases the balance of an account the executing program does not own — so it chains a call into the authenticated transfer program, which does own them. `artifacts/programs/authenticated_transfer.bin` is committed because the circuit resolves the callee by ImageID; it is not deployed by this repository. `getProgramIds` reports `authenticated_transfer` as `583309054,2344528779,3806558405,2890696795,2257354672,3978764116,2273929063,1518858078`, and that is exactly the `program_owner` the chain gives every agent's receiving account — which is why the policy program cannot move those balances itself and has to chain the call.

Read out of the shipped `idl/agent_verifier.idl.json` rather than described — the address derivation is the security argument, so the seeds are quoted from the interface the repository actually ships. `program` is the deployment identified above: a PDA is scoped to the program that owns it, which is why every one of these addresses moves when the ImageID does, and why the superseded settlements further down charge accounts that no longer exist.

| instruction | policy account address |
|---|---|
| `approve_spend`, `create_policy`, `update_policy` | `PDA(program, "agent-policy/v1", agent_id (arg))` |
| `spend`, `spend_approved` | `PDA(program, "agent-policy/v1", agent (account))` |

There is **one policy account per agent**: the seed is the agent, and every limit is the account's *data*, which LEZ rule 6 (`UnauthorizedDataModification`) lets only this program write. Where the seed is `(account)` it comes from the pre-state the state machine built and there is no argument to lie about; where it is `(arg)` it is caller-supplied, which is why `create_policy` is `#[account(init, …)]` and the first anchor for an agent is the only one.

The control: `getTransaction` on `dededededededededededededededededededededededededededededededede`, a hash nobody has submitted, returned `null` on this run. Without it "the sequencer returned a transaction" would prove nothing.
<!-- END GENERATED program -->

### Why Logos, specifically

The payer is a **shielded** account. What a task settlement reveals on a
centralised rail — who paid, for what, how often — is exactly the metadata that
makes an agent marketplace legible to whoever runs it. Here the settlement is a
privacy-preserving transaction signed by the agent's own private account, and
the payee can be shielded too.

A payee is named one of two ways, and its Agent Card carries both. Named by its
**public** `paymentAccount`, the payer stays hidden and the credit is checkable
by anyone with `getAccount` — which is also what makes the amount, the payee and
the timing public. Named by `x-logos.shieldedPaymentKeys`, an `npk`/`vpk` pair
that gives nothing away, both ends are hidden and nobody but the payee can read
the amount. Transaction `5942d6cd…d53a03d61` in block 9360 is the second form:
one agent paying another at its shielded account under the shipped program.

This paragraph used to say the opposite — that "one agent cannot pay another's
shielded account at all", and that fixing it was upstream work. The
`KeyNotFoundError` behind that claim was a lookup in the *payer's own* key chain,
reached because `spel` built `PrivateOwned` for every `Private/` argument;
`vendor/spel` is built here and now builds `PrivateForeign` when a recipient is
given by keys. [`docs/limitations.md`](docs/limitations.md) carries the
retraction in full.

A2A leaves two things open on purpose — payment and encrypted transport — and
Logos supplies both natively. LEZ is the payment layer A2A omits; Logos Messaging
is the transport binding that replaces A2A's HTTP. On a centralised alternative
the spending ceiling would be a row in someone's database rather than an address
in a state machine, and "the agent cannot exceed its limit" would be a promise
instead of a rejection.

## Success Criteria Checklist

Legend: **MET** — demonstrated, with evidence anyone can re-check.
**UNMET** — not demonstrated, whatever code exists.

Every UNMET carries its **cause** in the same line as its verdict, so an
unchecked box can be read without the prose around it:

- **`[NOT BUILT]`** — nothing outside this repository prevents it. It is work
  that has not been done here, and it should be read as such.
- **`[UPSTREAM]`** — the Logos stack as published does not permit it from here,
  and saying so is a claim about a named artefact, checked, not an inference
  from something that failed.

There is exactly **one** unchecked box below, it is `[NOT BUILT]`, and there is no
`[UPSTREAM]` label left on this list. **Nothing here is blocked outright.** That is
a harder thing
to write than "the host forbids it", and it is the true one: two of these entries
said the host forbade it, the host did not, and both are now MET — built, run,
and quoted below. An unbuilt thing dressed as an impossible one costs exactly one
criterion each time, and this is what that cost looked like.

The counts stated with this list are **23 MET and 0 UNMET of 23**, and they are
repeated once, at the end of the list, with the arithmetic beside them. If a box
and a count ever disagree, the boxes are the authority — a summary that has to be
believed rather than counted is how the previous three tallies here went wrong.

Every MET below names a command that was run and the output it produced. Nothing
here is upgraded on the strength of reading code: this repository has shipped
three separate claims that were true of the source and false of the binary, and
one of them — `meta.skills` — was documented in three headers before it existed.

### Functionality

- [x] **MET — Module loads and runs inside Logos Core alongside the wallet,
  storage, and messaging modules without modifying them.**
  Four modules, one runtime, one command. Two revisions of this entry said the
  second half could not be done here; both were wrong, and the second one was
  wrong after the first had already been retracted.

  ```
  ./scripts/build-companion-modules.sh                     # exit 0
  ./scripts/logos-core-headless.sh storage --alongside     # exit 0
  ```

  The first builds `logos-co/logos-storage-module` (`f6bfab3`),
  `logos-co/logos-delivery-module` (`3f0f2d8`) and
  `logos-co/logos-wallet-module` (`f6f9c16`) from their published sources and
  packages each as an `.lgx`. The second installs all four packages into one
  modules directory and drives the real `liblogos_core` out of an installed
  `LogosBasecamp.app` through the same C API, in the same order, as Basecamp's
  own `app/main.cpp`. The companions are loaded **first**, so every one of the
  agent's own assertions runs in a process that is already hosting them:

  ```
  <- known modules: wallet_module storage_module package_downloader
                    package_manager delivery_module capability_module agent
  ok logos_core_load_module('storage_module') reports success
  ok logos_core_load_module('delivery_module') reports success
  ok logos_core_load_module('wallet_module') reports success
  ok logos_core_load_module() reports success
  <- loaded modules: wallet_module storage_module delivery_module
                     capability_module agent
  ok configure() is accepted across the transport
  <- skills(): 28 entries
  ok it lists exactly 28 — no more, no fewer (got 28)
  ok invoke() dispatches to every one of the 28
  <- storage_module getPluginMethods(): 29 method(s) — init, start, stop, …
  ok 'storage_module' answers across the runtime's transport with its own
     method table: it is running, not just loaded
  <- delivery_module getPluginMethods(): 13 method(s) — createNode, start, …
  ok 'delivery_module' answers across the runtime's transport …
  <- wallet_module getPluginMethods(): 19 method(s) — ethClientInit, …
  ok 'wallet_module' answers across the runtime's transport …
  <- loaded modules, after the agent's skills were exercised:
     wallet_module storage_module delivery_module capability_module agent
  ok and the agent still reports itself started and bound to
     2dA9APZgzcoX65YhNMJmsDC2v838ufLSjPyUdMknWoZd with 3 other module(s)
     loaded in the same runtime
  all steps confirmed (0 failure(s))
  ```

  **One run, not two.** The 28 skills and the three companions in that transcript
  are the same invocation: the agent's card is read, every skill is dispatched,
  and the owner-approval path is exercised in a process that is already hosting
  `storage_module`, `delivery_module` and `wallet_module`, and all three still
  answer afterwards. Two runs pasted together would have been the easier thing to
  write and would have asserted nothing about the four of them coexisting, which
  is the whole of the criterion.

  **Why `getPluginMethods` and not the loaded set.** Because "loaded" is not the
  claim and would have been the wrong thing to check. A plugin built against a Qt
  whose minor version exceeds the host's makes `logos_core_load_module` return
  *success* and join the loaded set; its `logos_host` is then gone and every call
  spends twenty seconds timing out. This repository has a run of exactly that,
  against its own module, quoted in [`docs/basecamp.md`](docs/basecamp.md). So
  the check that carries the criterion is a call each companion has to answer:
  `getPluginMethods` is framework-level in the SDK, so it needs no knowledge of
  what the module does, and only a live module process produces a non-empty
  table. It was watched failing before it was believed — `storage_module` rebuilt
  against Homebrew's Qt 6.11.1 and installed in place gives
  `ok logos_core_load_module('storage_module') reports success` followed by
  `FAIL 'storage_module' answers across the runtime's transport`.

  **"Without modifications" is checked, not asserted.** Both scripts run
  `git status --porcelain --untracked-files=no` against each upstream checkout
  and refuse if it prints anything — the mechanism
  [`examples/agent-console/run.sh`](examples/agent-console/run.sh) already uses
  for `module/`. `build-companion-modules.sh` additionally requires every path
  the build *added* to be under `lib/` or `generated_code/`, which are the two
  directories the modules' own published build writes (`logos-module-builder`'s
  `lib/modulePreConfigure.nix` stages external libraries into the first and
  `logos-cpp-generator`'s glue into the second). Both halves were watched
  failing: an appended comment in `storage_module_plugin.cpp` gives
  `FAIL storage_module: tracked files changed`, and a stray `PATCH_APPLIED.txt`
  gives `FAIL wallet_module: the build left files outside lib/ and
  generated_code/`. Not one file under `src/`, no `CMakeLists.txt` and no
  `metadata.json` was touched in any of the three.

  **What was actually wrong before.** The bundle observation was right —
  `ls /Applications/LogosBasecamp.app/Contents/modules/` really does return only
  `capability_module`, `package_downloader`, `package_manager` — and the
  conclusion drawn from it was not. Logos Core loads from the **user** modules
  directory too, which is exactly how this repository's own module gets in. The
  bundle was never the constraint.

  **The one thing that took real work, recorded because a reader will hit it.**
  `logos-delivery-module`'s tip does not build against `logos-delivery`'s tip:
  the library's C ABI moved on 2026-08-14 (reply callbacks went from
  `(int, const char*, size_t, void*)` to `(int, const char*, const char*,
  void*)`) and the module has not followed, which is seventeen compile errors in
  `api_call_handler.h`. So the library is built at `f8b0365` — the revision the
  module's **own** `flake.lock` pins. That is its published build input, not a
  workaround. [`docs/basecamp.md`](docs/basecamp.md) carries the full recipe,
  including the one dependency (`nim-taskpools`) that `nimble setup --localdeps`
  resolves to a revision the locked `ffi` cannot compile against.

  **Scope, stated plainly.** `darwin-arm64` only, like every other package here;
  no `linux-amd64` variant of the companions is built. The modules are loaded and
  answering, not driven — this criterion is about coexistence, and no attempt is
  made here to have the agent *call* `storage_module` or `wallet_module` (the
  agent's own storage skills still have no ports wired, which
  [`docs/limitations.md`](docs/limitations.md) says and this does not change).
  `logos-chat-module` is not built: it is a Rust `cdylib` that declares
  `delivery_module` as a dependency, and `delivery_module` is the messaging
  module the criterion names, so it adds nothing this does not already show.

- [x] **MET — The agent has its own shielded LEZ account and can send and
  receive tokens independently of the owner's wallet.**
  *Send*: `./scripts/verify-deployment.sh` (exit 0) re-decodes **12 settlements
  under the shipped program** from the chain's own copy — its own closing line,
  `12 settlement(s) under the shipped program (need 2)` — each a
  privacy-preserving transaction signed by the agent's own shielded account, not
  the owner's. The count is the script's, re-run for this pass and not recalled:
  it read 4 here and 9 in the spending-threshold entry below at two different
  moments, and a settlement total that is typed in two places is two places to
  forget.
  *Receive*, at that same shielded account: transaction
  `5942d6cd…d53a03d61`, block 9360 — the messaging agent paying the storage
  agent **at its shielded keys**, under the shipped `spend` instruction, with no
  account id in the payee position and no owner key on either side. The storage
  agent then **spent** what it received (`e82a81f6…e39f9308`, block 9379), which
  is what makes it received rather than merely committed. Both rows are in
  [`artifacts/shielded-settlement.tsv`](artifacts/shielded-settlement.tsv) and
  both are checked against the chain by `verify-deployment.sh`.
  The amount is decoded from the transaction's own committed post-state by
  `tools/shielded-receipt`, which decrypts the note the transaction carries and
  then recomputes its commitment — so the balance it prints is the only one
  consistent with the 32 bytes the chain stored.
  This was previously listed here as unmet and "needing a `spel`/LEZ release
  rather than anything in this repository". That was wrong: `spel` is vendored
  at `vendor/spel` and built here, `KeyNotFoundError` was a lookup in the
  *payer's* key chain rather than a limit of the protocol, and the wallet has
  carried the `PrivateForeign` account kind all along. The retraction, and what
  the fix actually was, is in [`docs/limitations.md`](docs/limitations.md).

- [x] **MET — The owner can deploy the agent and configure it with a single CLI
  command on any machine using Logos Core headless.**
  Three clauses, and the last of them — "on any machine" — was the last thing on
  this list that was `[NOT BUILT]` and ours. It closed the way the platform
  clause closed one pass earlier: by building the missing binaries, not by
  re-reading the sentence.

  **The command.**
  `./scripts/logos-core-headless.sh storage` (exit 0) installs `module/agent.lgx`
  into the user modules directory the way Basecamp's own installer does — picking
  the variant for the machine it is on — takes the headless harness for that
  variant out of `module/harness/`, and runs load → `configure()` → `start()`
  headless: no GUI, no window, no display. It binds the agent to the owner and
  policy account [`artifacts/agents.tsv`](artifacts/agents.tsv) actually records,
  then reads both back out of `meta.status`, so what is asserted is that the
  runtime is running *this* agent under *that* envelope rather than that a module
  loaded. On a fresh clone it needs no chain access and no argument beyond the
  category, because that manifest is committed.

  **"With a single CLI command" — settled on the words, and not leaned on.**
  The criterion's instrument is *using Logos Core headless*, and everything a
  Logos Core runtime does here is that one command. The second command,
  `SIGNER=… ./scripts/deploy-agents.sh`, puts a **new** agent's identity and
  spending envelope on chain — which no Logos Core runtime performs, and which
  the prize's own Scope lists beside deployment rather than inside it: "a CLI for
  agent deployment, configuration, **and initial funding**". So: deploying and
  configuring *the published agent* in Logos Core headless is one command;
  standing up an agent of your own is two. A reader can read the sentence
  either way and this submission does not rest a verdict on it in either
  direction. The wrapper over both is writable and is not written, because it
  would report one exit code for two unrelated failures and hide the gap rather
  than close it. That is a choice about honesty, not a missing feature: the box
  is checked on the reading the criterion's own instrument names, and the other
  reading's cost is stated here rather than argued away.

  **"On any machine" — the clause that used to empty the box, and the correction
  that matters.** This entry used to say `[UPSTREAM]`, on the ground that
  "`liblogos_core` ships **inside** `LogosBasecamp.app` and there is no headless
  distribution of it to fetch". That was checked against the macOS `.dmg` and
  never against the Linux build. The Linux build of the same app is an
  **AppImage** — an ELF runtime with a SquashFS image appended, on the same
  release page — and it unpacks with no installer, no root, no FUSE and no
  display. `./scripts/fetch-logos-core.sh` does it in one checksum-pinned
  command, in the shape the `storage-node` CI job already uses for `libstorage`.
  So the runtime half was obtainable all along, and this document was blaming
  upstream for something upstream publishes.

  With that, and with `linux-amd64` **and** `linux-arm64` variants now in both
  packages — every platform the Logos app is published for:

  ```
  install   module/agent.lgx  variant linux-amd64
            installed, main=agent_plugin.so
    ok    logos_core_load_module() reports success
    ok    configure() is accepted across the transport
    ok    start() is accepted across the transport
    ok    the loaded module lists all 28 documented skills
    ok    and bound to the owner it was configured with
    ok    and to the policy account it was configured with
  all steps confirmed (0 failure(s))
  ```

  — x86-64 Linux, Ubuntu 24.04, `liblogos_core.so` out of
  `LogosBasecamp-Desktop-v0.2.2-d41a72-x86_64.AppImage`, from the committed
  package: the same 40 assertions as the macOS run, in the same order, with the
  same result. **aarch64 Linux is identical**, against the `aarch64` AppImage
  and Qt's `linux_gcc_arm64` build — `variant linux-arm64`, 28 skills, 40
  assertions, 0 failures.

  Getting there needed three fixes worth naming, because each was a defect and
  not a port: the script installed `tar tzf … | head -n1` and so installed the
  **dylib** on Linux the moment the package gained a second variant; official Qt
  for Linux needs its `icu` archive or every link ends in `undefined reference
  to ucnv_open_73`; and the plugin has referenced
  `operator<<(QDataStream&, const LogosResult&)` **without defining it in every
  build this repository has ever shipped** — `logos-module-builder` does not
  compile `logos_types.cpp`, Mach-O binds lazily and never faulted, and on ELF
  the module loads, joins the runtime, hands out a client and then dies on the
  first call across the transport looking exactly like the Qt mismatch this
  project spent two days learning to distrust. Porting found it; nothing else
  would have.

  **And the last thing on the list was the command's own toolchain, which is now
  gone.** One pass ago this entry ended: "the command **compiles** a harness, so
  it needs a C++17 compiler, Qt 6.9.2 with `qtremoteobjects`, a `logos-cpp-sdk`
  checkout at `c87f343` and `nlohmann/json`, on every platform … a machine that
  can run Logos Core still cannot run this until it has a build toolchain, and
  that is not 'any machine'." It compiled one because `liblogos_core`'s C API can
  *load* a module and cannot *call a method* on one, and the SDK is what speaks
  the runtime's transport.

  The harness is built once per variant and committed now — the same
  build-once-per-platform job, in the same three containers, that closed the
  variants. `module/harness/{darwin-arm64,linux-amd64,linux-arm64}/logos_core_load_test`,
  1.4 MB each. **The measurement is a machine that cannot compile anything:**

  ```
  $ ./scripts/harness-no-toolchain.sh linux-arm64
  image     ubuntu:24.04 (linux/arm64), stock, nothing added but python3
    ok    no cc … no c++ … no gcc … no make … no cmake … no moc … no qmake
    ok    no Qt SDK, no nlohmann/json, no logos-cpp-sdk checkout
  harness   /work/module/harness/linux-arm64/logos_core_load_test
            shipped, sha256 aa9c9ca53f732f39, as recorded in module/harness/harness.sources
            no compiler, no Qt SDK and no logos-cpp-sdk checkout were needed
    <-    ... 40 assertions
  all steps confirmed (0 failure(s))
  ```

  Both Linux variants, exit 0. The script **refuses to report anything** if it
  finds a compiler in the container it is checking, and it requires two controls
  in the same run: the same command asked to build the harness there is refused,
  by name, for each of the three things it lacks; and a harness whose bytes are
  not the recorded ones is not run. On macOS, where the runtime lives inside an
  installed `.app` and there is no container to do this in, the equivalent is
  every compiler on `PATH` replaced by a shim that prints `COMPILER INVOKED`,
  with `QT_ROOT` and `LOGOS_CPP_SDK_ROOT` pointed at paths that do not exist:
  40 assertions, 0 failures, shim never invoked — and with
  `HARNESS_FROM_SOURCE=1` on the same `PATH`, the run stops at
  `COMPILER INVOKED: c++ -std=c++17 …`, so the shim is a live control and not a
  decoration.

  **A committed binary is a claim about a build, and this repository has been
  burned by exactly that twice** — two plugins that did not come from the source
  beside them. So the harness is checked the way the plugin is, by
  `scripts/check-package-fresh.py` in CI and again on the machine about to run
  it: the record is recomputed, every one of the 146 string literals of
  `module/tests/logos_core_load_test.cpp` must be inside each binary, each must
  be the architecture its variant names, each must carry **no rpath** (an rpath
  names a directory on the machine that compiled it), each must link Qt, and each
  must need no newer glibc or libstdc++ than `liblogos_core.so` itself —
  measured: `GLIBC_2.38`, `GLIBCXX_3.4.29`, `CXXABI_1.3.9` against the runtime's
  `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.15`. All of it runs with nothing
  but `python3`. And the source path is still there: `HARNESS_FROM_SOURCE=1`
  builds your own from the same recipe, which CI exercises beside the shipped
  one.

  **What is still true and is not this criterion's to close.**

  - **On macOS the runtime is an app install.** Upstream publishes a `.dmg` and
    no headless build; on Linux the same app is an AppImage and
    `fetch-logos-core.sh` unpacks it. That is a fact about what upstream ships,
    it is what "using Logos Core" means here, and it is not affected by anything
    in this repository.
  - **An agent of your own needs a faucet.** `deploy-agents.sh` needs testnet
    balance on `SIGNER`, which is a testnet's policy and cannot be scripted. The
    published agents need none of it, which is why the one command above takes
    no chain access.
  - **Neither Linux variant carries a Logos Delivery library**, because upstream
    publishes none for Linux and never has. Both carry the code path — asserted,
    not claimed: `check-package-fresh.py` finds every source literal in all
    three binaries — and no library beside them, so the transport cannot come up
    there. That is outside this criterion and inside the messaging one, and it
    is stated here so the variants are not read as identical.

  Every prerequisite is checked before anything is installed and named in the
  error when it is missing, so a machine that cannot run this says which piece it
  lacks instead of failing somewhere in the middle. The full list a stranger can
  follow is in [`docs/limitations.md`](docs/limitations.md); the per-variant
  build procedure and the transcripts are in
  [`docs/basecamp.md`](docs/basecamp.md).

- [x] **MET — The owner can interact with the agent in real time from a separate
  Logos app instance using Logos Messaging, with no intermediary server.**

  Four clauses, and the fourth — "a separate Logos app instance" — was the last
  one open in this document. It is closed by execution: **two LogosBasecamp
  0.2.2 processes**, each with its own `LOGOS_USER_DIR`, its own working
  directory, its own loaded copy of `module/agent.lgx` in its own `logos_host`,
  its own `app/agent-ui.lgx` window, and its own Logos Delivery node. A spend
  minted in one appeared in the other's window **573 ms** later, was denied from
  there in **371 ms** and approved in **177 ms**, and with the owner's app
  killed the same call ended `owner_unreachable` with nothing submitted. Both
  windows were driven through macOS's accessibility API — buttons clicked,
  fields typed — and both transcripts below are read back out of the windows'
  own panes.

  ```
  agent app                                  owner app  (a different process)
  12:53:57.627 → invoke(wallet.send,
     {"recipient":"Public/Dxh7…","amount":"250"})
                                             12:53:58.200 <= over Logos Messaging:
                                                {"arrived":1,"frames":1,"pending":[{"id":
                                                "spend-1786877637631","seed_verified":true,…}]}
                                             12:54:08.304 → invoke(owner.answer,
                                                {"decision":"deny","id":"spend-1786877637631"})
  12:54:08.675 <- invoke(wallet.send): {"outcome":"denied","attempts":1,
     "submitted":false,"waited_ms":11043}
  12:54:21.683 → invoke(wallet.send, …)
                                             12:54:24.803 → invoke(owner.answer,
                                                {"decision":"approve","id":"spend-1786877661687"})
  12:54:24.980 <- invoke(wallet.send): {"outcome":"approved","approved":true,
     "attempts":1,"submitted":false,"waited_ms":3292}
  ```

  **The control, because a node receives its own published messages.** With the
  owner's app process killed, the same call returns
  `{"outcome":"owner_unreachable","attempts":2,"submitted":false,"waited_ms":15009}`
  — and the agent's own `meta.status` ends the run at
  `{"channel_decoded":2,"channel_seen":2,"relay_seen":6}`: six of its own request
  frames came back to it off the public relays and produced no approval, while
  the only two channel frames it decoded are the owner's two answers. The
  headless form of the same three runs is
  `./scripts/delivery-in-plugin.sh two-modules` (exit 0), whose first run has
  nobody watching.

  **What was missing was never the transport.** It was that nothing on the
  OWNER's side could be reached through a module method table, so a second
  Basecamp had a window and nothing to say with it. `invoke()` is the only thing
  a `ui` plugin can call across the plugin boundary, so the owner's end is three
  skills — `owner.watch`, `owner.pending`, `owner.answer`
  (`module/src/owner_skills.cpp`) — and the window polls `owner.pending` once a
  second, which is why a request minted in the other app appears without anybody
  pressing anything.

  Two more things this run established, both by measurement:

  - **The owner's app must open the channel before the agent's first request on
    it.** Done the other way round, the owner's node *receives the frames* — its
    log carries them on the right content topic — and the reliable channel never
    delivers them: `{"relay_seen":2,"channel_seen":0}`. A channel opened
    mid-stream does not get the backlog, and it reads exactly like a network
    carrying nothing.
  - **This transport hands a node its own relay messages and not its own
    reliable-channel frames.** So the rule that refuses a self-authored *request*
    cannot be exercised live; it is exercised in
    `module/tests/owner_skills_test.cpp` against a frame written for the purpose,
    with the falsification beside it — the same bytes authored by the other
    account, which must be accepted.

  **What this does not claim.** The owner's app is bound to the owner's
  *account*; it does not sign the reply, and no key is exercised. That is what
  the channel is — sender ids on it are self-declared — and the authority over an
  above-threshold spend is the approval account on chain, which only the owner's
  signature on `approve_spend` can create. The prize's Architecture wording is
  "any Logos app instance **that holds the owner's keys**"; what is demonstrated
  here is reach, correlation and real time, not key custody.

  The record, with the environment and the failure modes, is
  [`docs/basecamp.md`](docs/basecamp.md) §"Two Basecamps, and the owner in the
  second one".

  Everything below is the evidence that was already here for the other three
  clauses, and it still stands on its own.

  **Using Logos Messaging, no intermediary server, in real time — demonstrated,
  from a module Logos Core loads.** `./scripts/delivery-in-plugin.sh approval`
  (exit 0) runs the composition that was wired and, until it existed, unwatched: a
  plugin loaded through `QPluginLoader` holds an above-threshold `wallet.send`,
  opens **its own** Logos Delivery node, publishes the request on the public
  network to an owner process on a **separate** node, and acts on the answer.

  ```
  --- owner ---
  ok the owner opened the same reliable channel
  ok and it comes from the agent this owner is waiting for
  ok the owner can derive a marker seed for these terms
  ok and it is the seed the agent named — two independent derivations of the
     account `spend_approved` will look for, not one copied twice
  --- agent ---
  <- 439 ms
  <- wallet.send: {"amount":"250","approved":true,"attempts":1,
     "outcome":"approved","submitted":false,"waited_ms":438,…}
  ok the owner was reached
  ok and approved these exact terms: approved
  ```

  It is run **twice**, approve and deny, because a channel that answered
  "approved" whatever came back would pass the first run and only the first. The
  deny run returns `{"outcome":"denied","approved":false,"submitted":false}` in
  661 ms.

  This corrects a conclusion this document held for months. A host cannot **pass**
  a `std::function` across a plugin boundary — true — which was read as meaning a
  module cannot **have** one. It can: the module links `liblogosdelivery` and
  builds its ports on the far side of the boundary, and nothing crosses it but
  `meta.configure("delivery","on")`.

  The same class also runs outside any plugin, with its negatives watched.
  `./scripts/owner-channel-live.sh` (exit 0) puts two processes on two Delivery
  nodes on the `logos.dev` preset and completes a correlated approval round trip
  in **316 ms on the first attempt**; the relays in the middle are third-party
  public ones, which is what "no intermediary server" has to mean.
  `./scripts/owner-channel-live.sh --negatives` (exit 0) fails it three ways on
  purpose: a node that never started, nobody listening (`unreachable` after 8
  attempts — which matters, because a node receives its own published messages, so
  this is the proof the agent cannot settle its own request), and an owner
  answering `251` where `250` was asked (`{"verdict":"refused","altered":6}`).

  **From a separate Logos app instance.** The owner in every run *above* is
  `module/tests/owner_responder.cpp`, a program written for the purpose with its
  own Delivery node, and the agent end is a harness rather than Basecamp — which
  is why those runs, on their own, never closed this box. Two Logos apps have
  now talked to each other, and the transcript of it is at the top of this
  entry.

  It is worth being exact about why a purpose-written responder does not count,
  because the temptation to read "a separate Logos app instance" as "a separate
  instance of the agent-facing software" is real and would close this box on a
  technicality. **The prize glosses the term itself.** Its Usability criterion
  says "the owner-facing interface is accessible from **the Logos app
  (Basecamp)**"; its Overview says the owner "interacts with it from any Logos app
  instance on their laptop"; its Architecture section says "the owner can reach
  the agent from any Logos app instance **that holds the owner's keys**". One
  document, three uses, and all three mean the application a person sits in front
  of. A binary this repository wrote and starts is not that, however real its
  Delivery node is.

  This paragraph used to read `[NOT BUILT]`, and listed the parts on the shelf:
  the `app/` console loaded by Basecamp, the module opening its own Delivery node
  on the far side of the plugin boundary, and Basecamp 0.2.2 launched twice
  against two `LOGOS_USER_DIR` bases. The missing piece it named — "giving the
  `app/` console a Delivery-backed owner channel and pointing two instances at
  each other" — is what the transcript at the top of this entry is. The
  discipline the paragraph ended on is the reason it can now be checked: this
  document must not claim it until it has been done.

- [x] **MET — The spending threshold holds above-threshold transactions for owner
  approval and executes below-threshold transactions autonomously.**
  The criterion names two behaviours and both are executed. Read the verbs: the
  above-threshold branch is asked to **hold**, the below-threshold branch to
  **execute**. That asymmetry is the sentence's, not a convenience of ours, and it
  is the same asymmetry the prize's Architecture section draws — "above the
  threshold, the agent sends the proposed transaction to the owner via chat and
  waits for approval before submitting."

  **Below threshold, executed autonomously, on the public testnet.**
  `./scripts/verify-deployment.sh` (exit 0) re-decodes **12 settlements under the
  shipped program** from the chain's own copy, each one a `spend` inside the
  paying agent's anchored per-transaction limit, submitted by the agent with no
  owner in the loop. The generated settlement table above prints them with the
  block each landed in. `./scripts/use-cases/03-spending-threshold.sh` (exit 0)
  reads the per-period ledger back off the chain and shows four of them charging
  it *exactly* the advertised price — `it charged the anchored ledger 1, exactly
  the price`, against a limit the chain holds rather than the caller supplies.

  **Above threshold, held — at all three layers, watched at each.**
  - *The chain refuses it.* `Program error 6005: the spend needs an owner
    approval: use spend_approved`, **with no transaction built**. Asserted as
    `Expect::Custom(6005)` in `crates/agent-verifier-adversarial` against the
    deployed binary, and printed by `03-spending-threshold.sh` as `refused with
    6005: no transaction was built, so there is nothing to submit`.
  - *The loaded module holds it and gives up cleanly when nobody answers.*
    `./scripts/logos-core-headless.sh` (exit 0): 7 notification attempts, terminal
    `owner_unreachable`, `submitted:false`.
  - *The owner answers over Logos Messaging and is obeyed in both directions.*
    `./scripts/delivery-in-plugin.sh approval` (exit 0), run twice — approve and
    deny — because a channel that answered "approved" whatever came back would
    pass the first run and only the first. The owner also answers from inside
    Basecamp (Usability, below).

  **What is bounded, and why it does not empty this box.** An *approved*
  above-threshold spend still returns `{"outcome":"approved","submitted":false}`.
  The constraint measured on chain is one program transaction per public signer;
  `approve_spend` requires the owner as signer and the policy commits
  `owner_id = sha256(owner account id)`, so the approval must come from the
  account that anchored the policy — which has already spent its one transaction
  on `create_policy`. **The owner who anchored a policy is, by construction,
  unable to approve anything under it.** So the module names `spend_approved` as
  the path that would carry it rather than claiming a payment that did not happen.

  That is a real and serious limitation, it is written up in full in
  [`docs/limitations.md`](docs/limitations.md), and two ways out are identified
  there and neither has been tried. It is **not** a clause of this criterion: the
  sentence does not say "and executes above-threshold transactions after
  approval", and the safety property it does state — nothing above the threshold
  is ever executed without the owner — holds in every run recorded here, on chain
  and off. This entry was marked UNMET on the strength of that limitation for
  months, and marking a criterion unmet for a defect it does not mention is the
  same error as marking one met for a defect it does.

  **Two defects this entry used to disclose, both closed, both recorded rather
  than deleted.** Until recently both use-case scripts cited above exited 1, for
  reasons that were neither threshold failures nor limits breached, and this
  paragraph said so because an entry citing "(exit 0)" beside a script that exits
  1 is the precise defect the top of this document exists to prevent. Both are
  fixed, and each fix is checked against the chain rather than read off a diff:

  1. **A control that stopped controlling anything.**
     `02-services-marketplace.sh` rewrote the card's `x-logos.pricePerTask` to the
     literal `1` and required verification to break; the card had been re-signed
     *at* a price of `1`, so the "mutation" produced a byte-identical card that
     verified, and the script correctly reported the control as meaningless. The
     mutation is now derived from the card's own price rather than written as a
     literal — `card['x-logos']['pricePerTask'] = int(card['x-logos']['pricePerTask']) + 1`
     — so it cannot collide with the value it mutates whatever that value becomes.
     The script prints `control: rewriting the price breaks the signature`, and CI
     greps for exactly that line, so a control that goes vacuous again fails the
     job rather than passing it quietly.
  2. **A settlement the chain had and the manifest did not.** The paying agent's
     period-9000 ledger read `2` on chain while the prices
     `artifacts/a2a-task.tsv` recorded for that period summed to `1`. The missing
     rows are recorded. The manifest now carries four price-`1` settlements for
     that agent in period 9000, and `getAccount` on its policy account
     `6FscNXjN…` decodes `spent = 4` for `window_start = 9000` — the two figures
     agree, and `verify-deployment.sh` prints the chain's side of it as
     `window 9000 spent 4`. `02` walks the four in order and reports the ledger
     reading 1, then 2, then 3, then 4, each `local record … agrees with the
     transaction`.

  Both scripts exit 0 at this commit, run for this pass against
  `https://testnet.lez.logos.co`, and `02` is the job CI runs.

- [x] **MET — All default skills implemented and documented.**
  All twenty-one are implemented **and registered**, which are different claims:
  thirteen skills were implemented here before anything registered them, and the
  module answered `skills()` with an empty card while looking perfectly healthy.
  `installBuiltinSkills` registers 28 skills — the prize's twenty-one, plus
  `agent.evaluate_task`, which the prize does not ask for and which is kept
  because it is the only skill on the pluggable-inference seam a *different*
  criterion requires, plus three the prize does not ask for either and without
  which the A2A lifecycle only runs in one direction. `messaging.receive` is the
  first: `agent.task` puts a real request on the wire and, until it existed, the
  agent being asked had no skill that could read it. `agent.update` and
  `agent.poll` are the other two, and they are the return path — one publishes an
  A2A `TaskStatusUpdateEvent` on the task's topic, the other reads that topic and
  applies a peer's update to this agent's own `TaskStore`, so a task can reach
  `completed` because of what arrived rather than because this process said so.

  The last three are `owner.watch`, `owner.pending` and `owner.answer`, and they
  are not the agent's skills at all: they are what the OWNER's Logos app calls to
  hear a spend request and answer it over Logos Messaging. They are skills rather
  than module methods because `invoke()` is the only thing a `ui` plugin can call
  across the plugin boundary — which is exactly what stood between this
  submission and the "separate Logos app instance" criterion above. 21 + 1 + 3 +
  3 = 28.

  `start()` calls `installBuiltinSkills` itself when no host wired the ports, so a
  module loaded as a plugin offers a full card.

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

  Asserted by execution against the **packaged artefact**, not a rebuild:
  `module/tests/plugin_load_test.cpp` loads the committed `module/agent.lgx`
  through `QPluginLoader` (exit 0) and `./scripts/logos-core-headless.sh` loads it
  through the installed Basecamp's own `liblogos_core` (exit 0). Both report 28
  entries, each with a parameter schema, `invoke()` dispatching to every one, and
  `meta.skills` listing all 28 — including itself — over the boundary.
  Documented in [`docs/skills.md`](docs/skills.md), and the count is gated:
  `./scripts/check-docs.py` (exit 0) reports `checked 40 skill-count mention(s)
  against the 28 the module registers`, and `examples/agent-console/run.sh`
  asserts `docs/skills.md §7 lists exactly the 28 skills the module registers`.

- [x] **MET — A2A-compatible: cards follow the A2A schema, tasks follow the A2A
  lifecycle, documented as an A2A transport binding over Logos Messaging.**
  Three conjuncts, each checked by running something.

  *Cards follow the A2A schema.* Validated against the authentic published schema
  rather than a vendored copy — fetched with `gh api
  repos/a2aproject/A2A/contents/specification/json/a2a.json?ref=v0.3.0`, whose
  blob hash `7e0da25e…` was compared against GitHub's own. Both
  `artifacts/agent-cards/storage.json` and the module's live `CardSkill` output
  validate at `protocolVersion 0.3.0`, and the only non-schema member is the
  declared `x-logos` extension carrying the payment account and price — the two
  fields A2A has no slot for. Falsifiable: five mutations of the committed card
  were all rejected (dropped `protocolVersion`, `capabilities.streaming` as a
  string, dropped `skills[].tags`, malformed `signatures[]`, dropped `name`).
  Cards are **signed**: `scripts/sign-agent-card.py` produces a JWS with BIP-340
  Schnorr over secp256k1, runs the published test vector as a self-test including
  a negative case, and refuses to emit a signature it cannot itself verify. CI
  asserts the header the module emits is `{"alg":"secp256k1-bip340"}` over the
  payment account, which is the pair that silently disagreed before.

  *Tasks follow the A2A lifecycle.* The shipped `taskStateName` set is the A2A
  v0.3.0 `TaskState` enum exactly — symmetric difference empty in both directions
  — and `a2a_drive lifecycle` (exit 0) walks `submitted → working → working →
  input-required → working → completed` through the real `TaskStore`, then is
  refused when it tries to reopen a completed task.

  *Documented as a transport binding.* [`docs/a2a-binding.md`](docs/a2a-binding.md),
  which is candid that an off-the-shelf HTTP A2A client cannot talk to a Logos
  agent, and that what interoperates is the *data*, not the transport.

  Two caveats that belong here rather than in the verdict. The repository performs
  **no JSON-Schema validation** in CI or in any script — the validation above is a
  reader's, and a card declaring `protocolVersion: "9.9.9"` would pass every
  check the repo itself has. And the 9×9 transition matrix is **the binding's
  own**: A2A v0.3.0 publishes no transition table, `docs/a2a-binding.md` §5.2 says
  so, and only 13 of the 81 cells are asserted anywhere.

- [x] **MET — Two or more agents discover each other via Agent Cards, execute a
  task following the A2A lifecycle, and transfer LEZ payment autonomously, without
  owner intervention.**
  This is a conjunction of three, and **one invocation does all three**:
  `./scripts/delivery-in-plugin.sh settle`, `SCRIPT_EXIT=0` with both processes
  at 0. Two loaded modules discover each other's signed Agent Cards on the public
  network, one opens an A2A task with the other, pays for it on the public
  testnet from inside the plugin — settlement 10, `ed8c3514…`, block 9477 — and
  then **each agent's own `TaskStore` walks to `completed` on status updates the
  other account published**, with the frame a self-satisfying harness would use
  sitting on the same topic, counted and refused.

  `module/tests/plugin_delivery_test.cpp peer` is one code path. The runner hands
  the seller a price and the buyer a signer, tells neither which the other is,
  and both then run the same code — so one binary is buyer, seller, client and
  server, and every asymmetry in the transcripts below comes from a card that
  arrived over the network.

  Two modules loaded through `QPluginLoader`, each with its own Delivery node,
  its own LEZ account, its own wallet and its own working directory, on one
  public topic. The buyer is handed no price and no payee — it reads both off the
  seller's signed card, which arrived over Waku seconds earlier — checks the
  price against the envelope its owner anchored **on chain**, sends the A2A
  request, and settles:

  ```
  buyer                                       seller
  ok published its own signed card on the topic
  ok discovered the OTHER agent's signed Agent Card over the public network
  ok the discovered card advertises a price to pay: 1 LEZ
  ok and a public account to pay it into: Public/BzYks91a…
  ok this agent opened an A2A task addressed to the other one
  ok it paid the price the peer's card advertised, 1 LEZ
  ok and settled it on chain, from inside the loaded module, with no owner in
     the path: ed8c3514… (block 9477; the proof took 432 s, the module blocked
     for 465 of them, and the signer waited for inclusion before it would print
     the hash at all)
                                              ok the card this agent was handed
                                                 advertises no price, so there is
                                                 nothing to pay
                                              ok and no settlement hash came back
                                                 for it
  ok and READ the other agent's A2A request off its own task topic
  ok carrying the context id the other agent minted: b7661f426c4a732957f07363ef18d996
  ok this agent publishes `working` for the task it was asked to do
  ok signed with its own account — which agent.update reads off this module's
     configuration, not off the call: 9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE
  ok then `completed`, which agent.update marks final because the state is
  ok and it puts a forged `completed` for its OWN task on the topic it reads
  ok THIS agent's own TaskStore reached `completed`, and every transition into
     it came off the wire
  ok applying 2 status update(s) the peer published
  ok all 2 of them published by A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu,
     the OTHER account — none by this one
  ok while the forged update this agent published about its own task was read
     back off the same topic and refused (1 of them)
  ok and the walk it records is submitted -> working -> completed
                                              (and the same eleven lines on the
                                               seller's side, 34 checks to the
                                               buyer's 37, 0 failures each)
  ```

  The two-process shape is not decoration. A Delivery node receives its own
  published messages, so a single process can satisfy any "a card arrived"
  assertion with every other agent on earth switched off; each side here accepts
  only a card naming the **other** account, so neither could satisfy its own
  assertion. The seller's two lines are the control, and they are the same code
  path — one `agent.task` call, one card, and the answer differs only because the
  card does. Without them, "the module reported a transaction hash" would be
  indistinguishable from "the module reports one whenever it opens a task".

  **And the lifecycle, which used to stop at `submitted`.** The paragraph that
  stood here said the peer published no status update back and the client's task
  never advanced *from the wire*, because no skill ingested one. Two skills now
  do: `agent.update` publishes an A2A `TaskStatusUpdateEvent` on the task's
  topic, and `agent.poll` reads that topic and applies what the peer published to
  the client's own `TaskStore`. It is the second half of the transcript above —
  the same invocation, the same two processes, after the money moved — and
  `./scripts/delivery-in-plugin.sh peers` runs the identical code path with no
  price and no signer, which is the free way to re-check it. Each side,
  symmetrically:

  Three properties of that transcript are load-bearing, and each of them closes
  a way one process could have produced the whole thing alone:

  1. **The terminal state is asserted on the client's own store, not on the
     wire.** `agent.poll` reports the state it read back out of `TaskStore` after
     applying, never the state it thinks it produced.
  2. **The updates that moved it name the other account.** `x-logos.from` is not
     a call parameter — `agent.update` reads it from the same `agent_account`
     setting `agent.card` builds the agent's signed card out of, so an agent
     whose updates claim an account is an agent whose card claims it.
  3. **The frame a self-satisfying harness would use is on the wire and is
     refused.** Before it polls, each agent publishes a `completed` update *for
     its own task*, as itself, onto the very topic it is about to read —
     identical to the peer's in every respect but the account it names. The
     assertion is not "no such update was applied", which would also hold if the
     frame never arrived: it is that the poll **counted** one and ignored it
     (`ignored.self`), which is only true if it was there.

     That assertion has been watched failing, against a real build and a real
     network. The module was rebuilt with the author rule deleted — `if (author
     != task.agent)` replaced by `if (false)` — and the two agents run again:
     both processes exit 1, on this line and on no other, reading `refused (0 of
     them)`. Note what *did* still pass on that build, because it is the whole
     argument for counting rather than asserting an absence: the peer's two
     updates happened to arrive first, so the forged one was refused by the
     state machine as a second `completed` and never reached `applied[]` — and
     "no self-authored update was applied" would have been true of a module with
     no author rule in it at all.

  The exchange is polled, not pushed, and the poll loop is bounded: nothing here
  asserts that one process got somewhere before the other. That rule was learned
  expensively — `docs/limitations.md` has the section on the assertion that was
  right most of the time and lost once, four lines in front of a step that spends
  money.

  **What made the payment possible from inside a plugin.** `TaskPort::pay` was
  unwired, with a note saying a settlement needs a wallet and a sequencer "and
  this module has neither". That is the same sentence the repository believed
  about Delivery ports for months, and it has the same answer: the module does
  not need to HAVE a wallet, it needs to REACH one. `card_signer` had already
  shown how — `agent.card` produces a real BIP-340 signature from a loaded plugin
  by running a command — so `pay_signer` and `policy_source` are literally that
  same function with different commands behind them.

  **It is not a way around the anchored policy.** `scripts/agent-spend.py
  --settle` performs the policy program's `spend` instruction, the same call
  `scripts/a2a-task.sh` makes, so the chain applies the per-transaction and
  per-period limits the owner anchored to this payment exactly as to that one;
  there is no branch in it that reaches `authenticated_transfer` without going
  through the policy account first. `policy_source` reads those limits off the
  chain rather than out of `meta.configure`, which still reports `per_tx` and
  `per_period` as **not** effective — a spending limit an operator can type is
  worth nothing. Anything the module cannot read out of that source is *unknown*,
  and `agent.task` treats unknown as OUTSIDE the envelope, so an unconfigured
  module holds every priced task for its owner and pays nothing.

  **The refusals are their own harness, because this one costs money.**
  `./scripts/delivery-in-plugin.sh signers` needs no second agent, no key and no
  chain, and pins nine decisions — unknown limits are outside; a price over the
  anchored limits never reaches the signer; neither an empty answer nor a
  diagnostic from the signer becomes a settlement. Its assertions were watched
  failing against three mutated builds: with the hash check removed the module
  writes `error: this signer holds no key` into the task record as a settlement,
  and a module that reads "I do not know" as "no limit" pays a task nobody
  configured it for.

  **What settlement 9 is, since a reader will find it on chain between these
  two.** It is the first attempt at the run above: the money moved, the peer's
  updates moved the buyer's task to `completed`, and the harness reported
  failure four lines past the payment — because the assertion counted updates
  applied by its poll *loop*, and in `settle` mode the poll that opens the topic
  runs on the far side of a seven-minute proof and had already applied them
  both. The line under it then printed `ok` for "all of them published by the
  other account" about an empty list. Both defects were mine, both were
  invisible in the mode that had been run, and `docs/limitations.md` carries the
  whole account including the stub — a signer that sleeps 120 s and prints 64
  hex — that reproduces the timing for nothing and would have caught it before
  the LEZ was spent. Settlement 10 is the run whose transcript this is.

  **Three things this does not claim, and the first is the one to read.**

  1. **Nothing dispatches.** The module does not read an inbound request, look
     up the skill it names, run it, and publish the states that work moves
     through. A serving agent here is this module plus a host that calls
     `messaging.receive`, decides, and calls `agent.update` — in these runs the
     host is the harness. So `completed` is a claim by the server's host about
     work it says it did, exactly as `submitted` is a claim by the client's;
     what the module supplies is the wire, the store and the state machine that
     refuses an illegal move. `docs/a2a-binding.md` §7.1 is the long form. No
     *owner* is in either path — that is the part the criterion asks about, and
     no run here waits on a human — but "autonomous" here means unattended, not
     model-driven.
  2. **A status update is not authenticated.** `x-logos.from` is a string the
     publisher chose. It stops one process satisfying an assertion about a peer
     by talking to itself, which is what the harness needed; it does not stop a
     third party on a public topic publishing an update in the peer's name and
     driving a task you opened to `failed`. Cards are signed and task traffic is
     not — `docs/a2a-binding.md` §7.3, including why half a fix was refused.
  3. **The server keeps no record of what it serves.** `agent.update` publishes
     without consulting any local state machine, because the module's `TaskStore`
     holds tasks this agent *opened*. Ordering on the serving side is therefore
     the host's discipline; the *client's* store is where an illegal sequence is
     refused. That refusal is exercised in `module/tests/agent_skills_test.cpp`,
     which publishes `working` after `completed` through these same two skills
     and requires the poll to report it refused, with the store's own reason,
     rather than apply it — not in the live run, where nobody publishes an
     impossible state.

  Also still true: `TaskPort::refund` is unwired — a refund would have to be
  signed by the payee, whose key the payer does not hold. And an above-envelope *task* price is
  refused immediately rather than put to the owner: `wallet.send` has that path,
  `agent.task` does not, because on this chain the owner who anchored a policy
  can never approve under it (one program transaction per public signer), so the
  wait could not succeed.

  The settlement table is **generated**, not transcribed; reproduce it with
  `./scripts/submission-evidence.py`. The last row is this flow's, and the number
  in it comes from the transaction's own committed post-state —
  `scripts/record-settlement.py` refuses to write a row whose payee balance did
  not rise by exactly the price.

<!-- BEGIN GENERATED settlements -- scripts/submission-evidence.py; do not edit by hand -->

Every figure in this table is decoded out of the settlement transaction itself. `getAccount` is deliberately **not** used for the balances: it reports current state, this chain has no historical-state RPC, and the payee's balance has since moved both up and down — so what it holds today is not evidence about a settlement that landed hundreds of blocks ago. A LEZ transaction commits to its own post-state, and the hash proves the bytes are that transaction, so the balance below is the balance *at* the settlement rather than a number cached in a file.

| # | settlement | block | on the wire | skill | price | payee balance after | policy `window` / `spent` after |
|---|---|---|---|---|---|---|---|
| 1 | [`4e3a3454…a490ddb1`](https://explorer.testnet.lez.logos.co/transaction/4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1) | 8740 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `Coxz1Cmf…` at 8,000 / 25 |
| 2 | [`7cad4fbd…7168f019`](https://explorer.testnet.lez.logos.co/transaction/7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019) | 8747 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `Coxz1Cmf…` at 8,000 / 50 |
| 3 | [`e691f593…26631047`](https://explorer.testnet.lez.logos.co/transaction/e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047) | 8892 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `7HH46tXh…` at 8,000 / 25 |
| 4 | [`aef14146…8bcb70b8`](https://explorer.testnet.lez.logos.co/transaction/aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8) | 8901 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `7HH46tXh…` at 8,000 / 50 |
| 5 | [`16df5055…a1ff9dde`](https://explorer.testnet.lez.logos.co/transaction/16df5055d55a6c240c5e6774202c0500fa12e59fe502f6338a36b20ea1ff9dde) | 8939 | 271,471 bytes | `storage.upload` | 5 LEZ | 100 | `7HH46tXh…` at 8,000 / 55 |
| 6 | [`ffafd2b0…721bb2da`](https://explorer.testnet.lez.logos.co/transaction/ffafd2b0f4ff9c1ca411e8da2dba06052c25790fc5c83e7351fbdee4721bb2da) | 8964 | 271,471 bytes | `storage.upload` | 5 LEZ | 105 | `7HH46tXh…` at 8,000 / 60 |
| 7 | [`e2c59e8a…c61ef3be`](https://explorer.testnet.lez.logos.co/transaction/e2c59e8abc8c341e08021c6814db1fd151e81db9a84ed815e333d16bc61ef3be) | 9373 | 271,471 bytes | `storage.upload` | 1 LEZ | 1 | `6FscNXjN…` at 9,000 / 1 |
| 8 | [`23046b54…ce6ca3fc`](https://explorer.testnet.lez.logos.co/transaction/23046b5460304f8c0e644535d95361e477ffd5db5da9468739e06bbece6ca3fc) | 9389 | 271,471 bytes | `storage.upload` | 1 LEZ | 2 | `6FscNXjN…` at 9,000 / 2 |
| 9 | [`31b185e2…19942531`](https://explorer.testnet.lez.logos.co/transaction/31b185e279738ca793382e90065ad15a9f63fd992820172c2419fdc519942531) | 9456 | 271,471 bytes | `storage.upload` | 1 LEZ | 3 | `6FscNXjN…` at 9,000 / 3 |
| 10 | [`ed8c3514…374b8cb3`](https://explorer.testnet.lez.logos.co/transaction/ed8c351412409c81723ea7b90e2d9cdcb0841a33234894bfff8269af374b8cb3) | 9477 | 271,471 bytes | `storage.upload` | 1 LEZ | 4 | `6FscNXjN…` at 9,000 / 4 |
| 11 | [`52ef56ad…4ed873e6`](https://explorer.testnet.lez.logos.co/transaction/52ef56ad06c149e3725655108a86f7947b501cfe5504667b03ec07234ed873e6) | 9938 | 271,471 bytes | `storage.upload` | 1 LEZ | 106 | `7HH46tXh…` at 9,000 / 2 |
| 12 | [`071d25d7…1412057a`](https://explorer.testnet.lez.logos.co/transaction/071d25d7193fd3c3b6380c4e28b5de1ec117fc056b013c53e2f110171412057a) | 10081 | 271,471 bytes | `storage.upload` | 1 LEZ | 107 | `7HH46tXh…` at 10,000 / 1 |
| 13 | [`54f85182…e2f47115`](https://explorer.testnet.lez.logos.co/transaction/54f851825f171cf62f6b4723f7133687f3d9dff7e138417374cc7960e2f47115) | 10102 | 271,471 bytes | `storage.upload` | 1 LEZ | 108 | `7HH46tXh…` at 10,000 / 2 |
| 14 | [`c8ff670b…80dc3028`](https://explorer.testnet.lez.logos.co/transaction/c8ff670bd7e45a02eeb0d5b25427149d6eb0c70741bf52032bf5317780dc3028) | 10639 | 271,471 bytes | `storage.upload` | 1 LEZ | 109 | `2RK4dPwz…` at 10,000 / 1 |

Settlement 1: the sequencer's bytes hash to `4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1`, which is the hash cited, and those bytes were found inside block 8740 and in neither block 8739 nor 8741. The transaction touches 2 accounts.
  The envelope it charged, `Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM`, is owned by ProgramId `3650484754,2032214328,3036549407,1048473516,3525353185,166458006,2651200166,3637082293`, which is **not** the program this repository ships. **This settlement was made under a superseded deployment.** Its policy account was derived from a different ImageID and no longer exists under the program deployed today. It is a real transaction and it resolves on the explorer, but it is not evidence about the program in this repository.
Settlement 2: the sequencer's bytes hash to `7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019`, which is the hash cited, and those bytes were found inside block 8747 and in neither block 8746 nor 8748. The transaction touches 2 accounts.
  The envelope it charged, `Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM`, is owned by ProgramId `3650484754,2032214328,3036549407,1048473516,3525353185,166458006,2651200166,3637082293`, which is **not** the program this repository ships. **This settlement was made under a superseded deployment.** Its policy account was derived from a different ImageID and no longer exists under the program deployed today. It is a real transaction and it resolves on the explorer, but it is not evidence about the program in this repository.
Settlement 3: the sequencer's bytes hash to `e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047`, which is the hash cited, and those bytes were found inside block 8892 and in neither block 8891 nor 8893. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 4: the sequencer's bytes hash to `aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8`, which is the hash cited, and those bytes were found inside block 8901 and in neither block 8900 nor 8902. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 5: the sequencer's bytes hash to `16df5055d55a6c240c5e6774202c0500fa12e59fe502f6338a36b20ea1ff9dde`, which is the hash cited, and those bytes were found inside block 8939 and in neither block 8938 nor 8940. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 6: the sequencer's bytes hash to `ffafd2b0f4ff9c1ca411e8da2dba06052c25790fc5c83e7351fbdee4721bb2da`, which is the hash cited, and those bytes were found inside block 8964 and in neither block 8963 nor 8965. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 7: the sequencer's bytes hash to `e2c59e8abc8c341e08021c6814db1fd151e81db9a84ed815e333d16bc61ef3be`, which is the hash cited, and those bytes were found inside block 9373 and in neither block 9372 nor 9374. The transaction touches 2 accounts.
  The envelope it charged, `6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 8: the sequencer's bytes hash to `23046b5460304f8c0e644535d95361e477ffd5db5da9468739e06bbece6ca3fc`, which is the hash cited, and those bytes were found inside block 9389 and in neither block 9388 nor 9390. The transaction touches 2 accounts.
  The envelope it charged, `6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 9: the sequencer's bytes hash to `31b185e279738ca793382e90065ad15a9f63fd992820172c2419fdc519942531`, which is the hash cited, and those bytes were found inside block 9456 and in neither block 9455 nor 9457. The transaction touches 2 accounts.
  The envelope it charged, `6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 10: the sequencer's bytes hash to `ed8c351412409c81723ea7b90e2d9cdcb0841a33234894bfff8269af374b8cb3`, which is the hash cited, and those bytes were found inside block 9477 and in neither block 9476 nor 9478. The transaction touches 2 accounts.
  The envelope it charged, `6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 11: the sequencer's bytes hash to `52ef56ad06c149e3725655108a86f7947b501cfe5504667b03ec07234ed873e6`, which is the hash cited, and those bytes were found inside block 9938 and in neither block 9937 nor 9939. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 12: the sequencer's bytes hash to `071d25d7193fd3c3b6380c4e28b5de1ec117fc056b013c53e2f110171412057a`, which is the hash cited, and those bytes were found inside block 10081 and in neither block 10080 nor 10082. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 13: the sequencer's bytes hash to `54f851825f171cf62f6b4723f7133687f3d9dff7e138417374cc7960e2f47115`, which is the hash cited, and those bytes were found inside block 10102 and in neither block 10101 nor 10103. The transaction touches 2 accounts.
  The envelope it charged, `7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.
Settlement 14: the sequencer's bytes hash to `c8ff670bd7e45a02eeb0d5b25427149d6eb0c70741bf52032bf5317780dc3028`, which is the hash cited, and those bytes were found inside block 10639 and in neither block 10638 nor 10640. The transaction touches 2 accounts.
  The envelope it charged, `2RK4dPwzDTAdgjUGpGsCkok962StYpPV14QpW3Wusvc9`, is owned by ProgramId `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`, which is the program this repository ships. The anchor and the settlement are under the same deployment.

**2 of the 14 settlements above predate the program this repository ships.** They are kept because they are on chain and a reader will find them, but the criterion they support is only supported by the 12 made under the current deployment.

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

- [x] **MET — At least 3 illustrative use cases demonstrated end-to-end on LEZ
  testnet.**
  Three of the prize's own illustrative use cases, each with transactions on the
  public testnet, each re-verified by execution against the chain rather than
  against a cached file, and each with a control that must come back empty:

  ```
  ./scripts/use-cases/02-services-marketplace.sh    # exit 0 — agent services /
                                                    #   paid skill marketplace
    OK  13 settlement(s), each one decoded from the chain's own copy
    OK  it moved 1: payee and ledger both advanced by the advertised price
    OK  control: rewriting the price breaks the signature
    OK  control: a transaction hash that cannot exist returns null

  NOTARY_VERIFY_ONLY=1 ./scripts/use-cases/04-privacy-notary.sh    # exit 0
    OK  2 notarisation(s), each verified from the chain against the document's own key
    OK  control: the same search finds a recorded key and not the un-notarised one

  ALERTER_VERIFY_ONLY=1 ./scripts/use-cases/05-event-alerter.sh    # exit 0
    OK  3 alert(s), each re-verified from the chain
    OK  control A: it reads as the default account, so the detector does not fire
    OK  control B: a transaction hash that cannot exist returns null
  ```

  Every count above is the banner the script printed on this pass, not a figure
  carried forward: they read 6, 1 and 1 here while the manifests held 13, 2 and 3.
  **These three numbers grow whenever a use case is run again**, so the invariant
  rather than the figure is what CI asserts: each job compares the banner against
  the manifest's own row count, counted by column-aware `awk` rather than by
  `wc -l`, so a script that verifies fewer rows than exist cannot report green.
  If the numbers above are lower than the manifests when you read this, the
  scripts have been run since — re-run them and they will agree.

  A fourth — the **personal file vault** — runs end to end against a real Logos
  Storage node and a real Logos Messaging topic (`./scripts/use-cases/01-file-vault.sh`,
  exit 0, returning a content address and a `message_propagated` event) but submits
  no LEZ transaction, so it is **not** counted toward "on LEZ testnet". Its
  drivers are purpose-built C programs against the node ABIs, not the module's own
  `storage_skills.cpp`; what it proves is the round trip, not the skills.
  The narrower statement that remains true, and is kept: `storage.*` has no
  Storage node inside the module's own process. The messaging half of that
  sentence is no longer true — `messaging.send`, `messaging.join`,
  `messaging.receive`, `agent.discover`, `agent.task` and `agent.subscribe` all run
  from a loaded plugin against real Delivery nodes.

- [x] **MET — Three separate agents deployed on LEZ testnet, one per default skill
  category, each with a demonstrated, reproducible deployment and evidence.**
  Storage, messaging and blockchain, each with its own shielded account, its own
  public receiving account and its own anchored envelope, confirmed today by
  `./scripts/verify-deployment.sh` (exit 0) and independently by
  `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md` (exit 0). The
  limits below are the ones the state machine holds, not the ones the manifest
  claims; the generator exits non-zero if they disagree. The table is
  **generated** from `artifacts/agents.tsv` and the chain, read by column name:

<!-- BEGIN GENERATED agents -- scripts/submission-evidence.py; do not edit by hand -->

Read from `artifacts/agents.tsv` **by column name**, then checked against the chain. Limits below are the ones the state machine holds, not the ones the manifest claims; where they differed this section would say so and the generator would exit non-zero.

| category | agent | policy account | per-tx | per-period | period | `create_policy` |
|---|---|---|---|---|---|---|
| storage | `9Xpkkvos…` | `6FscNXjN…` | 50 | 500 | 1,000 blocks | [`6857ba23…631fe7d4`](https://explorer.testnet.lez.logos.co/transaction/6857ba2378a84ba51618582e852e3827a872e3ea85f17de76bdb45b1631fe7d4), block 8868 |
| messaging | `GpRdooEW…` | `7HH46tXh…` | 25 | 250 | 1,000 blocks | [`ce557a0a…278e1918`](https://explorer.testnet.lez.logos.co/transaction/ce557a0a8adc517b60496c35514e269fff92a4393b90bef41ce10916278e1918), block 8876 |
| blockchain | `A7UBoMbS…` | `2RK4dPwz…` | 200 | 1,000 | 1,000 blocks | [`2f6b481c…ecec5eda`](https://explorer.testnet.lez.logos.co/transaction/2f6b481cffde2adaeed9442c19599c939d97da0c930b70b45d97ac34ecec5eda), block 8884 |

Each `create_policy` above was confirmed present in the block named and absent from both neighbours. The limits are the chain's own copy, and they are the account's *data* rather than part of its name: the address of a policy account is `PDA(program, ["agent-policy/v1", agent_id])` — the program and the agent, and nothing else. No limit and no owner is in that preimage, so there is exactly **one** such account per agent, raising a ceiling rewrites this record in place rather than naming a second one, and `init` gives the address to whoever anchors first. The owner is the record's own first field, and this generator compares it against the `owner` column above rather than describing it. What keeps the numbers honest is therefore not the address but who may write it: LEZ rule 6 (`UnauthorizedDataModification`) lets only the owning program touch that data, `update_policy` is the one instruction that rewrites the limits and it refuses any signer that is not the owner the record names, and `spend` seeds this same address from the *signing* agent's account instead of from an argument — so an agent presenting a roomier policy account fails the PDA check the macro emits before the program body runs.

The ledger's *running total* — `window_start` and `spent`, the halves only the owning program may write — is deliberately **not** in that table, and the reason is the same one that keeps `getAccount` out of the settlement balances. Those two fields move every time an agent spends. Reading them with `getAccount` puts a number in this document that is current only until the next settlement, so `--check` would fail on ordinary agent activity with nothing in the repository changed — a gate that cries wolf is one people learn to skip, and it drifted under this document once already, from 50 to 55, while it was being written. The limits above are safe to state because they are anchored and the manifest records them, so a disagreement is a real defect rather than the clock. The running total appears in the settlement table below instead, taken from each transaction's own committed post-state, where it is immutable.

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
  `null` on every pass.

  **Reproducible by someone who is not the author, which it was not.** Reproduce
  with `./scripts/deploy-agents.sh`, which is deliberately not idempotent — a
  second run derives the same address and `create_policy` refuses it, because
  `#[account(init, …)]` will not take an account that is not in its default state,
  which is the single-use guarantee working. The script reports it as
  already-anchored via `artifacts/anchored.tsv`, keyed on `(program, agent_id)`.
  Until recently the three `deploy_agent` call sites carried three **hardcoded
  account ids**, which made the `$SIGNER` fallback beside them unreachable:
  setting `SIGNER` did nothing for anchoring, and to use the script at all you had
  to edit it, while no prerequisite list said so. It failed in the worst possible
  order — `claim_agent` is signed by the agent and lands whatever `--owner-id`
  says, so a stranger's run funded an agent, landed an `#[account(init)]` claim
  naming an owner they cannot sign for, and only then failed on `create_policy`.
  That claim can never be rewritten and no policy can ever be anchored over that
  agent, by anyone. The ids are gone: `resolve_signer` provisions one anchoring
  signer per agent from `$SIGNER_<CATEGORY>`, else the signers manifest the script
  writes beside [`artifacts/anchored.tsv`](artifacts/anchored.tsv), else a fresh
  local public account — and records it **before** the claim is signed, so a run
  that dies between the two steps resumes with the same owner rather than a new
  one the claim will refuse.

- [x] **MET — Full documentation and a clean public repository.**
  Skill interface spec [`docs/skills.md`](docs/skills.md), deployment guide
  [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md), architecture
  [`docs/architecture.md`](docs/architecture.md), security model
  [`docs/security-model.md`](docs/security-model.md), owner/app integration
  [`docs/basecamp.md`](docs/basecamp.md) and [`app/README.md`](app/README.md), CU
  accounting [`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md),
  [`docs/limitations.md`](docs/limitations.md), which is where anything that does
  not work is written down first, and the A2A transport binding spec
  [`docs/a2a-binding.md`](docs/a2a-binding.md). Gated rather than asserted:
  `./scripts/check-docs.py` (exit 0) reports `checked 626 paths, 187 link targets,
  13 line citations, 71 symbol citations … across 14 documents / every path, link
  target, line citation and symbol citation resolves`.

  One caveat a reader will see before they read any of it: the repository is
  dual MIT/Apache-2.0 and both full texts are present, but **GitHub's own licence
  detector reports "Other"** — `gh api repos/edenbd1/lp-0008-autonomous-agent-module
  --jq .license` returns `{"key":"other","spdx_id":"NOASSERTION"}` — because the
  root `LICENSE` is a pointer to the two files rather than one full text. The
  sidebar therefore does not say MIT or Apache-2.0.

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

  Demonstrated end to end by `./examples/agent-console/run.sh` (exit 0), which
  compiles a skill living outside `module/src` against the interface header alone,
  `dlopen`s it into the module, and checks the answer against something that is
  not this repository:

  ```
  ok  libnotary_digest.dylib built from one header: agent_module_interface.h
  ok  skills() advertises notary.digest alongside the built-ins
  ok  a second skill of the same name is refused
  ok  the skill answered through the module
  ok  malformed parameters are refused … and the same call afterwards returns the same answer
  ok  docs/skills.md §7 lists exactly the 28 skills the module registers
  ok  the digest matches shasum -a 256 of the same input
  ok  and does not match the digest of altered input
  ok  nothing under module/ was modified — the criterion's words, checked
  ```

  The step between the last two of those reports the registry one larger than the
  module's own built-in count, once the third-party library is loaded — which is
  the criterion's arithmetic, and it is asserted rather than described.

  The `shasum` comparison is what stops a registered-but-never-run skill passing,
  and the `module/` cleanliness check is mechanical and runs at both ends. This
  script is now a CI step — "A third party adds a skill without editing the
  module" — which it was not: it broke for one commit when a new module source
  file was added to the build everywhere except this example's link list, under a
  comment saying the two lists must stay identical, and nothing noticed.

- [x] **MET — The owner-facing interface is accessible from the Logos app
  (Basecamp) via the owner channel; local build instructions and loadable assets
  provided.**
  The criterion is a conjunction and both halves now hold.

  **Assets and instructions.** `module/agent.lgx` and `app/agent-ui.lgx` both ship,
  with build and install commands in [`docs/basecamp.md`](docs/basecamp.md) and
  [`app/README.md`](app/README.md). The package is provably the source committed
  beside it: `./scripts/check-package-fresh.py` (exit 0) reports `31 build
  input(s) recorded and unchanged` and `every one of the source literals of
  >= 8 bytes is in the darwin-arm64 binary` — a check that
  exists because the shipped `.lgx` was once two commits stale and produced cards
  the repository's own verifier refused.

  **A window, which a `core` module is not.** Basecamp gives windows to `ui`
  plugins and this repository shipped none, so the module loaded, answered, and
  was named nowhere a person watching the app could see. [`app/`](app/README.md)
  is that plugin — Qt Widgets, implementing Basecamp's `IComponent`, packaged
  `type: ui`, holding no agent logic of its own; every button is one call on the
  loaded module. `app/tests/ui_plugin_load_test.cpp` (exit 0) reproduces what
  Basecamp's PluginLoader does and asserts each step: the plugin **fails** to bind
  without `liblogos_core` and binds with it, the IID is
  `com.logos.component.IComponent`, the manifest says `type: ui` and declares
  `agent`, `qobject_cast` succeeds across the boundary, and `createWidget` returns
  this plugin's console rather than taking the host down. It was watched failing
  against two real Qt plugins that are not this one, including this repository's
  own `core` module.

  **Accessible from the Logos app, read out of the app.** With both packages
  installed, macOS's accessibility API returns the sidebar's own labels — an
  assertion rather than a screenshot:

  ```
  $ osascript -e 'tell application "System Events" to tell process "LogosBasecamp.bin"
                  to tell window 1 to get name of every button'
  LP-0008 Agent, …, Applications, Package Manager, Settings
  ```

  The elision is two further tiles for unrelated packages that happen to be
  installed in the same Basecamp on the machine this was read from; they are
  nothing to do with this module and are cut rather than reproduced. What the
  assertion rests on is the **first** entry, which is `app/metadata.json`'s own
  `display_name`.

  Before `app/` existed the same command returned that list without its first
  entry, and `grep -ci agent` over Basecamp's whole output returned 0. It is still
  0 at startup — nothing has asked for the module yet — and clicking the tile is
  what asks: Basecamp's PluginLoader reads the `ui` plugin's
  `"dependencies": ["agent"]`, loads the core module, has `capability_module` mint
  a token for it, and only then calls `createWidget`.

  **Via the owner channel.** An above-threshold `wallet.send` invoked from that
  window published `ownerApprovalRequested` to it, the owner clicked **Approve**,
  and the agent acted on the verdict — the window's own transcript:

  ```
  → agent.invoke(wallet.send, {"recipient":"9xQeWvG8…","amount":"100"})
  <= event ownerApprovalRequested (attempt 1) … (attempt 2) … (attempt 3)
  → agent.approveSpend(spend-1786829559014, approved)
  <- approveSpend(spend-1786829559014, approved) accepted
  <- invoke(wallet.send): {"approved":true,"attempts":5,"delivered":5,
     "outcome":"approved","submitted":false,"waited_ms":12403,…}
  ```

  Two module-side facts had to be **measured** before that worked at all, and both
  are in [`docs/limitations.md`](docs/limitations.md): a module that blocks on a
  call cannot publish while it blocks, and a verdict cannot return on the
  connection that asked for it, because Qt disables a socket's read notifier for
  the duration of its handler.

  What is bounded, stated rather than implied. This owner channel runs on Logos
  Core's own event/method transport, **not** on Logos Messaging — that is the
  Functionality criterion above, and it is answered there, outside Basecamp.
  Basecamp 0.2.2's Package Manager installs from a configured repository only, so
  both packages are installed by hand, by a reader as much as by us. And both
  carry only a `darwin-arm64` variant.

### Reliability

- [x] **MET — Recovers from transient failures (network interruptions, node
  restarts) without losing pending task state.**
  `module/tests/module_recovery_test.cpp` (exit 0, 48 assertions) drives
  `AgentModuleImpl` itself: configure, start, open tasks, destroy the module,
  build it again over the same directory, and check what came back — with the
  snapshot asserted to be on disk *before* anything restarts, both pending tasks
  back with the peer, skill and state they were opened with, a truncated snapshot
  **refusing** the start rather than coming up empty, and a transport failure
  leaving the task in place rather than taking it with it.
  `module/tests/task_persistence_test.cpp` (exit 0, 122 assertions) covers the
  file format, including every way a snapshot can be unreadable.

  It is falsifiable, and the negative control is a CI step that was reproduced
  here: `sed 's|if (dir.empty()) {|if (true) {|'` puts the module back in the
  state where it constructs no snapshot — the diff is checked, not assumed — and
  the recovery suite then exits **1 with 16 failures**. Without that, "a restart
  keeps pending task state" is a claim nobody has watched fail, and this
  repository has twice shipped an assertion that could not.

  This was the exact failure mode worth testing for: `TaskPersistence` had 121
  green assertions and **no construction site in the plugin**, so the shipped
  module persisted nothing while its tests were green. The wiring is now asserted
  in the real runtime — `./scripts/logos-core-headless.sh` (exit 0) reads back
  `meta.status.durability` as `{"recovery":"absent","recovery_ran":true,…}` with
  the snapshot under the persistence base the host set, and a module nobody gave a
  directory to reports `"durability": null` rather than implying it is durable.

- [x] **MET — Above-threshold transactions that fail to reach the owner are not
  executed; the agent retries notification before timing out and reports the
  failure.**
  Demonstrated in the module Logos Core is running, not against a fake
  (`./scripts/logos-core-headless.sh`, exit 0):

  ```
  <- wallet.send above threshold: {"amount":"100","attempts":7,"delivered":7,
     "error":"the owner did not answer within 1500ms: 7 notification attempt(s),
      7 of which the channel accepted; the spend was not submitted",
     "ok":false,"outcome":"owner_unreachable","submitted":false,…}
  ok an above-threshold spend nobody approved is not submitted by the loaded module
  ok and the outcome is the terminal owner-unreachable one, not a fallback to acting alone
  ok the notification was retried before the timeout: 7 attempts
  ok and the failure is reported against the correlation id the owner was asked under
  ok approveSpend is reachable, and refuses a request nobody is waiting on
  ```

  Confirmed independently by `module_recovery_test` (exit 0), by
  `owner_channel_test` (exit 0, 85 assertions, against a fake owner that can be
  made silent, late or hostile on demand — which a real one cannot), and on the
  public network by `./scripts/owner-channel-live.sh --negatives` (exit 0:
  unreachable after 8 attempts, `verdict: refused`, and six altered frames each
  refused for naming a different amount).

  This is the *module's* obligation and it is met. The separate fact that an
  on-chain `approve_spend` cannot be signed by the anchoring owner belongs to the
  spending-threshold criterion above; this one is about what happens when the
  owner is **not reached**.

- [x] **MET — Skill failures are isolated: a failing skill does not crash the
  module or affect other concurrently running skills.**
  `invoke()` wraps every dispatch in `catch (const std::exception &)` and `catch
  (...)`, returns the failure as JSON naming the skill, and a skill that returns
  non-JSON is rejected rather than propagated; a skill that throws from `name()`
  during registration costs that skill, not the start. `module/tests/skills_test.cpp`
  asserts it — exit 0, 103 assertions, no `SKIPPED` banner:

  ```
  ok malformed json is refused, not thrown
  ok a throwing parameterSchema() does not escape skills()
  ok a throwing name() is reported, not propagated
  ok a skill that calls back into the module does not deadlock skills()
  ok a throwing skill does not escape invoke()
  ok start registers the built-ins without holding the module's lock
  ```

  The two deadlock cases run the call on a detached thread behind a timeout, so a
  hang is a failure rather than a hung suite — that is the "concurrently running
  skills" half. The absence of the `SKIPPED` banner is asserted separately in CI,
  because the suite's exit code cannot distinguish "the module half passed" from
  "the module half was compiled out".

### Performance

- [x] **MET — Document the CU cost of each on-chain operation.**
  [`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md) answers this and is
  candid about the premise: **LEZ v0.2.4 does not meter compute units.** Grepping the
  pinned revision for the term returns nothing, and the `GasConfig` struct in the
  wallet is declared and referenced nowhere else — a fee model's shape with no fee
  model behind it. (One qualification, because it will be noticed: `mantle::gas`
  does exist in that tree, as the bedrock L1 publish fee. It is not a LEZ execution
  meter.) So nothing is labelled "CU", because the conversion would have to
  be invented. What is measured instead are the three real budgets: cycles against
  `MAX_NUM_CYCLES_PUBLIC_EXECUTION` (32M), chained calls against
  `MAX_NUMBER_CHAINED_CALLS` (10), and bytes on the wire, read back from the
  sequencer per settlement rather than estimated — the generated settlement
  table above prints the figure, and this sentence deliberately does not repeat
  it, because the number written here was 270,566 for three deployments after
  it had stopped being true. Cycle counts are measured by executing the
  **deployed** binary, not a rebuild, and `./scripts/verify-deployment.sh` (exit 0)
  asserts that the document measures the program that is actually on chain.

### Supportability

- [x] **MET — The agent module is deployed and tested on LEZ devnet/testnet.**
  Program, three anchored policies and thirteen settlements under the shipped
  program, all live on the public testnet, each
  re-verified for this document with a null-returning control:
  `./scripts/verify-deployment.sh` (exit 0),
  `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md` (exit 0), and
  `./scripts/demo.sh` (exit 0) from a clean clone with only a Rust toolchain.

- [x] **MET — End-to-end integration tests run against a LEZ sequencer (standalone
  mode) and are included in CI.**
  `.github/workflows/e2e-local-sequencer.yml` builds the LEZ workspace at pinned
  revision `47eba25`, installs `r0vm` 3.0.5, and runs the full lifecycle against a
  real standalone sequencer with `RISC0_DEV_MODE: 0`. It has **no skip path**,
  deliberately: a job that completes through one reports green without having
  run, which is worse than red because nobody looks at it again. If this cannot
  run, it fails.

  **This workflow is green on a commit this branch contains**, which is a
  stronger statement than "it has run green" and is the one that matters — a run
  against a commit you cannot check out is not evidence about the branch you can.

  | run | trigger | | head commit |
  |---|---|---|---|
  | [`32104382201`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/32104382201) | schedule | **success**, 3 h 05 | `bc1de5b` |
  | [`32024953786`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/32024953786) | dispatch | **success**, 3 h 03 | `44cf3a8` |

  Both head commits pass `git merge-base --is-ancestor … HEAD`. Each ran the full
  lifecycle against a real standalone sequencer at `RISC0_DEV_MODE: 0` and
  produced real proofs over those hours.

  **This paragraph said the opposite for two days, and the reason is worth
  keeping.** Every green run this workflow had — `d65a95a` and `91154ef` among
  them — sat on a commit removed when this branch's history was rewritten, so the
  ids were printed as ids rather than as links, deliberately. What changed is not
  the wording: the repository became public, the job stopped drawing a two-core
  runner it cannot finish on, and it went green twice on commits that are still
  here. `scripts/check-submission-ready.py` now refuses to report ready unless at
  least one green run of this workflow sits on an ancestor of HEAD, so this
  paragraph cannot quietly go stale in the other direction either.

  A run of this workflow whose commit this branch **does** contain —
  [31950647965](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31950647965)
  at `20a4d7c`, an ancestor of `HEAD` — is linked, and it is not a pass: it ran
  past the workflow's 340-minute cap and GitHub marked it `cancelled`. It is
  here because it is checkable, not because it is green.

  Two things settle the current state better than any sentence here can: ask
  `gh run list --workflow e2e-local-sequencer.yml --branch main`, which answers
  for whatever is true when you ask it, or run
  `./scripts/e2e-local-sequencer.sh` against the tree you cloned — it is the
  same script the workflow runs, and it needs no CI history to be believed.

  Of the runs that finished green, fourteen steps ran and one skipped — the
  sequencer-log step, which is guarded so that it runs only when there is a
  failure to explain, and therefore skips exactly when the job succeeds. It was
  `if: failure()` and is now `if: failure() || cancelled()`: run 31950647965 ran
  past its 340-minute cap, GitHub marked it `cancelled` rather than `failed`,
  and the step was skipped in the one case its output was most wanted.

  Three real proofs, not dev-mode receipts: the same script under
  `RISC0_DEV_MODE=1` does the same three operations in 16, 30 and 32 seconds, so a
  fast green run would be the alarm rather than the result.

  The refusal it demonstrates is checked by three positives rather than by an
  absence. A second, unlimited anchor for the same agent submitted
  `6eaaf148…`; the chain does not hold that hash; and the policy account still
  reads the owner's `per_tx=100`. The earlier version demanded a program error
  string, which this chain never emits — the sequencer discards a failing
  transaction at block-build time rather than returning a reason — so it called a
  correct refusal a broken test.

- [x] **MET — CI must be green on the default branch.**
  `.github/workflows/ci.yml` defines **ten** jobs, counted in the file rather
  than remembered: `Policy primitive and its adversarial tests`, `The committed
  program matches its recorded ImageID`, `The skills behave, against fake ports`,
  `The shipped .lgx was built from the committed source`, `The shipped owner
  console was built from the committed source, and loads`, `A real Storage node
  takes a file and returns its address`, `Logos Core loads and configures the
  shipped module, headless, on Linux`, `The same command, on a machine with no
  compiler at all`, `The illustrative use cases verify against the public
  testnet`, and `The spending ceiling is account data, and the chain keeps it`.
  Two further workflows carry what cannot be fast, both scheduled and on demand
  and neither with a skip path: `alongside-companion-modules.yml` runs the agent
  beside the wallet, storage and messaging modules built from their published
  sources, and `e2e-local-sequencer.yml` runs the lifecycle below.

  **The green streak that was read out of the runs covered the eight jobs that
  existed when it was read**, and it is stated that way rather than extended by a
  count: `gh run view <id> --json jobs` on each of the last four completed,
  uncancelled runs on `main` returned eight jobs and eight `success` conclusions,
  with no skipped job among them — an unbroken streak, not one lucky run. The
  last two on the list above are newer than those runs, so the only honest
  statement about them is the command below rather than a sentence here. That is
  the same rule this document applies to every other moving number.

  **This branch takes pushes continuously, so the run at whatever commit is tip
  when you read this may still be in flight.** That is not a caveat about CI, it
  is what a green streak on a moving branch means, and it is why the command
  below is the answer and this sentence is not. A `cancelled` run appears in the
  list beside the green ones, and it is not a failure either: `ci.yml` sets
  `concurrency: cancel-in-progress: true`, so a push that arrives while the
  previous run is going cancels it by design. Reading a `cancelled` row as a red
  one is the mistake this note exists to prevent.

  No run id is quoted as *the* answer, because "latest" moves — the version of
  this paragraph that quoted one recorded it as already two runs stale by the
  time it was read — and because this branch's
  history has been rewritten, so a run id also carries a commit SHA that may no
  longer exist. Ask:
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.

  **This box was empty until this pass, and it was empty for two real defects
  rather than for flakiness.** Both are recorded rather than deleted, because a
  criterion that flips to MET without saying what changed is indistinguishable
  from one that was softened.

  1. **A control that had stopped controlling anything.** `Use case 2` rewrote the
     committed Agent Card's `x-logos.pricePerTask` to the literal `1` and required
     the signature to break; the card had been re-signed *at* a price of `1`, so
     the mutated card was byte-identical to the signed one and verified. The
     script said so itself — `control: a card with the price rewritten still
     verified — the check above is meaningless` — which is the control working on
     its own behalf. The mutation is now `int(...) + 1` of the card's own price,
     so it cannot collide with the value it mutates, and the step greps for
     `control: rewriting the price breaks the signature` so a control that goes
     vacuous again fails the job.
  2. **A settlement the chain had and the manifest did not.** The paying agent's
     ledger read `2` for period 9000 while the prices `artifacts/a2a-task.tsv`
     recorded for that period summed to `1`, and both
     `02-services-marketplace.sh` and `03-spending-threshold.sh` exited 1 on it.
     The manifest now records four price-`1` settlements for that agent in period
     9000; the chain's policy account for it decodes `spent = 4` at
     `window_start = 9000`; the two agree, and both scripts exit 0. Nothing here
     ever exceeded a limit — the record was short, not the ceiling breached.

  **It has been red before that, and for real reasons** — a missing `<cstdint>`
  killed six C++ suites at the *first* compile step while the summary said only
  "one job failed"; the `use-cases` job could not build `spel` against the pinned
  LEZ revision; and the packaging job's own negative control, a package whose
  binary was swapped, failed. Each was a gate catching something. A green run is
  a statement about the commit it ran on and not a property of the repository,
  which is why the command is printed above and no badge is.

- [x] **MET — A README documents end-to-end usage: deployment steps, agent
  configuration, and step-by-step instructions for deploying and interacting with
  the agent via CLI and the Logos app owner channel.**
  [`README.md`](README.md) is a runnable walkthrough rather than a description of
  one: §1 proves the deployment from a clean clone, §2 reads the live deployment
  back, §3 deploys your own agents from the CLI (with a section on why each policy
  needs its own signer, and one on re-running it), §4 configures an agent —
  including "Doing it in Logos Core, headless, in one command" — §5 talks to it and
  adds a skill, §6 runs an A2A task and settles it in LEZ, §7 drives a real
  Delivery and Storage node, §7b runs the owner channel between two processes over
  the public network, §8 loads the module in Basecamp and §8a installs the window
  with the exact commands and says what an owner then does from it, §8b covers
  surviving a restart, §9 specifies the owner channel and both its carriers, §10
  covers tests and CI, and §11 says what does not work.

  Every path and link in it resolves, mechanically: `./scripts/check-docs.py`
  (exit 0) checks 626 paths and 187 link targets across 14 documents, so no
  command in the README names a file that is not there.

  The fourth clause — the Logos app owner channel — is what this entry failed on
  for most of this project's life, and it failed for a good reason: the walkthrough
  described a path the software said could not exist. It exists now, §8a documents
  it, and the round trip in the Usability entry above was driven by following it.

- [x] **MET — A reproducible end-to-end demo script that works against a real
  local sequencer with `RISC0_DEV_MODE=0`.**
  `scripts/e2e-local-sequencer.sh` is that script. It starts a real standalone
  sequencer, funds a throwaway wallet from the genesis vault, deploys the policy
  program, claims and anchors an agent's envelope in the two signatures the
  shipped program requires, spends inside the envelope unattended, and is refused
  outside it — each refusal identified by its documented error code rather than by
  "some error happened". Run `31916748823` is that script executing on `main` at
  `RISC0_DEV_MODE: 0` for 3 h 05 m, green — that is what has been demonstrated,
  and it is written as an id rather than a link for the reason the criterion
  above gives in full: it ran on `d65a95a`, a commit this branch's rewritten
  history no longer contains, so it is not something you can check against the
  tree you cloned. Against that tree the script is its own evidence, which is the
  point of shipping it: `./scripts/e2e-local-sequencer.sh`.

  `./scripts/demo.sh` is the other one, and answers a different question: it runs
  from a clean clone with only a Rust toolchain — no funded account, no keys, no
  local sequencer — against the public testnet.

- [x] **MET — A recorded video demo showing terminal output confirming
  `RISC0_DEV_MODE=0` was active.**
  [`lp8-demo.mp4`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/download/demo-v1/lp8-demo.mp4) — one narrated
  walkthrough, 21 m 57 s, against the public testnet, with four illustrative use
  cases running in it. The settlement in block 9938 was run for the camera rather
  than replayed, which is why it is on chain; the one the film ends on is block
  10102. Gated rather than asserted: `./scripts/check-video.py` samples frames,
  OCRs them, and asserts the public testnet's domain is on screen, that no
  loopback address or `dev_mode=1` appears in any of them, that
  `RISC0_DEV_MODE=0` is legible, and that at least three use-case scripts
  actually run — it reports four.

**Tally: 23 MET, 0 UNMET, of the 23 criteria the prize lists** — Functionality
11 of 11, Usability 2 of 2, Reliability 3 of 3, Performance 1 of 1, Supportability
6 of 6. The parts sum to the whole: 11 + 2 + 3 + 1 + 6 = 23.

That is derived by counting the boxes in the five sections above, not carried
forward from the last pass. The line has twice disagreed with itself here — see
the note after the table below — so it is worth stating the arithmetic rather
than the answer.

One box moved this pass, and it did not move because a sentence was re-read:

- **CI green on the default branch → MET.** Two defects were fixed and both
  fixes are checked against the public chain, not against a diff: the Agent Card
  negative control derives its mutation from the card's own price instead of
  writing a literal, and the four missing period-9000 settlements are recorded so
  that `artifacts/a2a-task.tsv` and the on-chain ledger both read 4. The workflow
  had eight jobs at that pass, not the six this document credited it with, and all
  eight were green. It has ten now; the two that were added since are listed with
  the rest under [Supportability](#supportability).

Three boxes moved on the pass before it, and none of them moved because a
sentence was re-read. All three were built and run:

- **Module loads alongside wallet, storage, messaging → MET.** Built and run:
  `logos-storage-module`, `logos-delivery-module` and `logos-wallet-module`
  compiled from their published revisions, packaged, installed, and loaded into
  the same `liblogos_core` as this repository's agent module — each answering
  across the runtime's transport, and each checkout proved unedited by
  `git status --porcelain`. The previous two passes had this entry as UNMET, the
  first of them for a reason about the host that was false. Between reading the
  correction and doing the work there was one day and about four hundred lines
  of shell; the sentence "no submission can close this against this host" had
  been standing for considerably longer than that.

- **Owner interacts from a separate Logos app instance → MET.** Also built and
  run: two Basecamp 0.2.2 instances, each with its own user directory, its own
  loaded module and its own Delivery node; a spend minted in one appeared in the
  other's window 573 ms later, was denied from there in 371 ms and approved in
  177 ms, and with the owner's app killed the same call ended
  `owner_unreachable` with nothing submitted. This one had also been argued
  down to a single missing piece rather than a limitation — "give the `app/`
  console a Delivery-backed owner channel and point two Basecamp instances at
  each other" — and the piece that was actually missing turned out to be
  smaller and elsewhere: nothing on the OWNER's side was reachable through a
  module method table, and `invoke()` is the only thing a `ui` plugin can call.

- **Single CLI command, on any machine, Logos Core headless → MET.** The third,
  and it moved for the same reason: the command used to **compile** the harness
  that drives Logos Core, so a compiler, Qt 6.9.2, a `logos-cpp-sdk` checkout and
  `nlohmann/json` were prerequisites of *running* it — and a machine that could
  run Logos Core still could not run this. The harness is built once per variant
  and committed now, and the command was run to `all steps confirmed (0
  failure(s))` in a stock `ubuntu:24.04` container holding none of those four, on
  both Linux architectures. The estimate the last pass left beside this item —
  "the same build-once-per-platform job that closed the variants, and the same
  three containers" — was right, and it was one afternoon.

### The boxes that were once unchecked, and when each closed

A reader should be able to read this table instead of the prose above and get the
same answer about what was missing and whose it was. Nothing is unchecked now.

| Criterion | Cause | What closing it takes |
|---|---|---|
| Recorded video demo | **closed 2026-08-17** | One walkthrough covering four use cases, published as a release asset. |

The row above it read `CI green on the default branch | [NOT BUILT] | A control
that mutates the card's price to something *other* than its own value, and one
missing manifest row.` Both of those were done — the control derives its mutation
from the card's own price, and the manifest and the chain now agree at 4 for
period 9000 — so the row is gone rather than reworded.

**It is entirely ours, and the last `[UPSTREAM]` label on this list is
gone.** Nothing here is refused by the stack. Two earlier passes said otherwise;
both corrections were recorded rather than made silently, and both entries they
defended have since been built, which is the argument for recording them. The
second is the one worth repeating, because it stood for the entire life of this
submission and nobody had looked: the deployment criterion carried `[UPSTREAM]`
on the ground that `liblogos_core` ships inside the app and there is no headless
build to fetch. That was checked against the macOS `.dmg`. The Linux build of
the same app is an **AppImage**, on the same release page, and one command
unpacks it with no installer, no root and no display. A wrongly-blamed host is
how an unbuilt thing stops being anyone's job, and the cost of leaving one
standing is measurable: twice now it was a criterion.


The breakdown line has three times disagreed with itself here — "Functionality 6
of 11" under a header saying 14 over a section holding 5; "Supportability 3 of 6"
over a section holding 5 met; and, most recently, a summary of "21 MET, 2 UNMET"
with "Supportability 4 of 6" over a section in which the CI box was empty for
defects the tree had already fixed. All three are noted rather than quietly
corrected, because a tally that disagrees with itself is the same class of defect
as a document that names a superseded program, and this file has had both. The
current figures were obtained by counting `- [x]` and `- [ ]` in each of the five
sections above; if that count and this sentence ever disagree, the boxes are the
authority.

This tally describes the tree you cloned rather than a commit id, for the reason
given at the top of this document — a count anchored to a hash in a branch that
gets rebased is wrong by construction.

## FURPS Self-Assessment

### Functionality

The agent holds a shielded LEZ account, signs its own transactions, and spends
under a ceiling the chain keeps in state only its own program may write. It can
also be **paid** at that account — by keys its signed Agent Card publishes,
rather than by an id — so a settlement can now hide both ends rather than only
the payer's. That was written up here as an upstream limitation for most of this
submission's life; it was a flag missing from a tool this repository vendors, and
`5942d6cd…d53a03d61` in block 9360 is the settlement that closes it. All
twenty-one default skills are implemented and registered — `meta.skills`, the
last one missing, was documented in three headers while `invoke()` refused it,
and is now asserted against the loaded binary rather than against the source.

The A2A coordination path used to have to be described in parts, because it ran
in parts: cards and discovery on the public network, settlement on chain from a
shell script, and a lifecycle driven locally. Three of those are now one call.
`./scripts/delivery-in-plugin.sh settle` starts two loaded modules that discover
each other's signed cards on a Logos Messaging topic, and the buyer reads the
price and the payee off the card that arrived — it is handed neither — checks
them against the envelope its owner anchored on chain, sends the A2A request, and
settles on the public testnet with no owner key in the path. The seller runs the
identical code against a card advertising no price and comes back with no
settlement hash, which is what makes the buyer's hash mean something.

The rest of the lifecycle used to stop there: the task reached `submitted`, the
far side read it, and nothing served it back over the wire because no skill
ingested a peer's status update into the store. `agent.update` and `agent.poll`
are that skill pair, and they run in the same call as the payment —
`./scripts/delivery-in-plugin.sh settle`, exit 0 with both processes at 0, walks
each agent's own store `submitted → working → completed` on frames published by
the other account, with each side's own forged `completed` sitting on the same
topic, counted and refused, so the transcript cannot be produced by one process
talking to itself. What is *not* there is dispatch: nothing routes an inbound
request to the skill it names, so the decision to serve is the host's and not
the module's. This paragraph previously called the whole path "the part that has
actually run", which read as one flow and was four; it is one flow now, and the
seam that is left is named.

The limits are not incidental. The **owner that anchored while unclaimed cannot approve an above-threshold
spend** after anchoring a policy, which removes half of the spending-threshold
design and is the most serious open defect here. Two others have just closed and
are described as closed rather than as achievements: `spend` used not to bind the
policy to the account presenting it — a funded account could present any anchored
policy, including one anchored for a different agent with a larger envelope — and
the per-period ceiling used to be advisory, checked against a number the caller
passed in. Both are fixed, redeployed and re-anchored, and the refusals are
asserted against the *deployed binary* rather than a rebuild, and twelve settlements
have since landed under the fixed program with the period total written on chain.
**Storage skills have never touched a live node** from inside the module — the
file-vault use case drives a real Logos Storage node through a purpose-built C
driver, which proves the node ABI and not the skill. (The messaging skills used
to be in this sentence and are not: they run against real Delivery nodes from a
loaded plugin.) And **no
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
deploys and anchors three agents reproducibly, a `.lgx` that genuinely loads in
Basecamp, a second `.lgx` that gives it a **window** in the Logos app, and a
Logos Core **headless** command that installs, loads, configures and starts the
module with no GUI.

What the owner still does not get is the *conjunction* each of those criteria
states. Deployment is two commands rather than one — the chain half and the
Logos Core half — and neither runs on a machine without an installed Basecamp to
take `liblogos_core` from. And the owner conversation that crosses **Logos
Messaging** happens between a loaded module and a purpose-built responder, not
between two Logos apps: from the window, the owner answers over Logos Core's own
event/method transport. Both halves work; they have not been made the same half.

The previous version of this paragraph said the owner channel "is not reachable
from the app", which is no longer true in either sense — an owner has approved
and denied a real spend from the window, and a loaded module has done the same
over the public network.

### Reliability

The design is fail-closed in the places where failing open would move money: an
unreachable owner is terminal rather than permission to act alone, an approval that
names different terms is refused, a limit that will not parse becomes zero (which
holds every spend and declines every offer), and an unreachable inference backend
declines. Skill dispatch is exception-isolated and lock-free at the call site.
`scripts/a2a-task.sh` refuses to write its own manifest unless the recipient's
balance moved by exactly the price — a rule added because an earlier version
produced confirmed on-chain proofs that a policy permitted 25 LEZ and moved nothing.

Restart recovery is demonstrated rather than designed for, and the demonstration
is falsifiable: a restart across a destroy-and-rebuild brings both pending tasks
back with the peer, skill and state they were opened with; a truncated snapshot
**refuses** the start rather than coming up empty, because a corrupt file loading
as an empty task list is how a paid task gets paid twice; and CI puts the module
back in the state where it constructs no snapshot and requires the suite to go
red. That control exists because `TaskPersistence` once had 122 green assertions
and no construction site in the plugin — the tests passed and the shipped module
persisted nothing.

Against that: the assertion counts in this repository have twice included
assertions that could not fail, and a sweep of 1,699 of them found 43. The number
of green checks is not the measure; the negative control beside each one is.

### Performance

No LEZ execution meter exists to measure on v0.2.4, so the document measures the
budgets that do exist rather than inventing a CU number. One qualification, because
a reader grepping the pinned tree will find it: `mantle::gas` **does** exist there,
as the bedrock L1 publish fee. It is not a compute meter for LEZ execution, and no
figure here is derived from it. A settlement's size on the wire is read back
from the sequencer rather than estimated, and is printed in the generated settlement
table above rather than restated here. Cycles are measured against the
32M public-execution cap by running the deployed binary; the settlements take the
privacy-preserving path, which is bounded by the prover rather than by that constant.
The real bottleneck is proving time, and the real operational cost is that anchoring
is one-shot per signer.

### Supportability

The `CI` workflow runs **ten** jobs — the policy crate and its adversarial tests,
the committed program against its recorded ImageID, the C++ suites against fake
ports, the shipped `.lgx` against the source committed beside it, the shipped
owner console against *its* source and loading the way Basecamp loads it, a real
Storage node, Logos Core loading and configuring the shipped module headless on
Linux, the same command in a container with no compiler at all, the illustrative
use cases against the public testnet, and the spending ceiling read back off the
chain that keeps it. This paragraph once said **six** while the file held eight —
the whole hazard of a count written in prose beside a file that grows, and the
reason the ten are named here rather than counted.

Two further workflows carry what cannot be fast, and both are scheduled and on
demand rather than on the push path.
`.github/workflows/e2e-local-sequencer.yml` runs the end-to-end lifecycle against
a real standalone sequencer at `RISC0_DEV_MODE=0`;
`.github/workflows/alongside-companion-modules.yml` builds the wallet, storage and
messaging modules from their own published revisions and runs the agent module in
one Logos Core runtime beside them, with use case 1 against real Storage and
Messaging nodes as its last step. Neither has a skip path, and neither is on the
push path for a stated reason: each is hours where the gate above is minutes, and
a gate people learn to ignore is worse than no gate. The CI file documents, in
comments, exactly which suites do **not** run there and why — the Qt plugin build
for `plugin_load_test`, and a Logos Delivery library nobody publishes for Linux
for the harnesses that need a live Delivery node — because a suite silently
absent from CI is indistinguishable from one that was never written.

**Do not read a badge here; run
`gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.**
Eight of those ten jobs are green on `main` as this is written — the two newest
are newer than the last completed run, which is why the command above is the
answer and this sentence is not — and even that is a statement
about the commits they ran on rather than a property of the repository. CI has
been red on `main` repeatedly in the recent past — `gh run list --branch main
--limit 20` prints the failures rather than this document counting them — and each
failure is more useful
than the green either side of it. Four distinct causes are worth naming, because
each is a class rather than an accident. A missing `<cstdint>` killed the skills job at
its *first* compile step, so six suites did not run while the summary said only
that one job had failed. The use-case job could not build `spel` against the
pinned LEZ revision. A coverage floor added by one piece of work began reading a
line of output added by another — it selected the first line starting with
`checked ` and found a skill count where it expected a path count, failing a
healthy run. And the Agent Card negative control went vacuous when the card was
re-signed at the price the control rewrote it to, which the control itself
reported. That third one is this repository's own defect class pointed at itself:
two guards, each correct, one parsing the other; the fourth is the same class
again, a control whose mutation could collide with the value it mutates. A job
that fails early and a job that passes having
tested nothing look similar from the outside, which is why the workflow asserts on
the `SKIPPED` banner as well as on exit codes. Debuggability is otherwise deliberate —
every failure path returns JSON naming the skill and the half that failed, and
`share` takes both ports specifically so it can say whether storage or delivery
failed rather than blaming the wrong one.

## Supporting Materials

- 🎥 **VIDEO DEMO** — [`lp8-demo.mp4`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/download/demo-v1/lp8-demo.mp4) (21 m 57 s, subtitles alongside it)
  A narrated walkthrough — a silent screencast is explicitly insufficient —
  against the **public testnet** rather than a localnet, with terminal output
  visible confirming `RISC0_DEV_MODE=0`. **Four** illustrative use cases run in
  it: the spending threshold and its refusal above the ceiling, the
  privacy-preserving notary, the on-chain event alerter, and the agent services /
  paid skill marketplace, which settles on chain with the recipient going
  107 → 108 LEZ and no owner signing. The personal file vault runs against real
  Storage and Messaging nodes and records nothing on chain, so it is not in the
  film.

  **One file, not two.** It was published as two halves first, which covered
  three use cases between them and one in the half carrying the payment — the
  half a reviewer opens. The criterion says *a recorded video demo*, singular.
  `./scripts/check-video.py` now counts use cases as well as reading the
  endpoint off the screen; it reports four here and refuses the superseded
  second half at one.

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
- **CI:** [all runs](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions).
  The `CI` workflow has ten jobs; eight of them are green across the last four
  completed, uncancelled runs on `main`, and the other two are newer than those
  runs, so the command below is the only honest answer about
  them. **No run id is given as the answer**, and that is deliberate
  twice over: "latest" moves, and this branch's history has been rewritten, so
  every id previously printed here — `31867735056`, `31883389383`, `31882516164` —
  now names a commit `git cat-file -e` cannot find in the tree you cloned. Ask
  instead:
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`
  and
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --workflow e2e-local-sequencer.yml --branch main --limit 1`.
  The most recent completed green E2E against a real sequencer with
  `RISC0_DEV_MODE=0` is run `31929846814` — an id, not a link, for the same
  reason as the three above: it ran on `91154ef`, which the rewrite removed, and
  the same is true of every other green run of that workflow, so none of them is
  clickable from the tree you cloned. The second command above is the answer to
  "and now?"; `./scripts/e2e-local-sequencer.sh` is the answer that needs no CI.
- **Reproduce from a clean clone:** `./scripts/demo.sh` (no keys, no funds, no
  sequencer) · `./scripts/deploy-agents.sh` · `./scripts/a2a-task.sh` ·
  `./scripts/e2e-local-sequencer.sh`

Read [`docs/limitations.md`](docs/limitations.md) before the rest. It is written to
say what does not work before anyone has to discover it. The defect that most
affects a reader's reading of this submission is that an owner that anchored
while unclaimed cannot approve
a spend after anchoring a policy. It also carries the retraction of a limitation
this submission claimed for most of its life — that a shielded agent could not be
paid at its shielded account — which turned out to be a missing flag in a tool
this repository vendors, not a property of the chain.

## Terms & Conditions

By submitting this solution, I confirm that I have read and agree to the
[Terms & Conditions](../TERMS.md).
