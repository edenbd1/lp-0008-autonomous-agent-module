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
> | 2 | **The end-to-end run against a real local sequencer is not green on `main`.** The workflow is in CI, has no skip path, and runs at `RISC0_DEV_MODE=0` — and the run it last completed on this branch failed at `create_policy`, because the script anchored the way the program used to require rather than the way it does now. The script has since been ported to the two-signature anchor; what has not yet happened is a completed green run **on `main`**. No run id is written in this row on purpose: `gh run list --workflow e2e-local-sequencer.yml --branch main --limit 1` answers it, and the last two versions of this row were wrong because a number was typed into them. | Dispatch it on `main` and read the conclusion. Two criteria turn on it. |
>
> Blocker 2 used to read "`HEAD` is ahead of `origin/main` and unpushed", and it
> is worth saying why that is gone rather than quietly deleting it: it stopped
> being true, and a stale blocker is more corrosive than a stale claim. It tells
> a reviewer that the tree they cloned is not the tree being described, which
> invites them to distrust everything else in the document — including the parts
> that are checkable. `HEAD` **is** `origin/main`.
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
> two of them discovered each other's signed Agent Cards on the public network
> and an owner approved and denied a spend over Logos Messaging.
>
> CI is **not** on that list, and it was, twice. It is green on `main` as this is
> written, and it has been red on `main` twice today — once because a job could
> not build `spel`, once because a coverage floor added by one piece of work read
> a line of output added by another. Both were gates catching something real, and
> neither is a reason to write "CI is green" into a document that outlives the
> run. Ask: `gh run list --repo edenbd1/lp-0008-autonomous-agent-module
> --branch main --limit 1`.
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
have **paid each other for a priced skill, unattended**, with the per-period
total accumulating on chain. That sentence used to read "have run an A2A task to
completion and settled it in LEZ", which implied one flow where there are two:
the lifecycle is driven through the real `TaskStore` and the settlement is a
separate step in the same script, and the difference is the whole of what the
agent-coordination criterion still asks for.

Agent-to-agent coordination is A2A-shaped: cards carry the A2A schema plus an
`x-logos` extension for the price and payment address that vanilla A2A has no
field for. Two modules that a host **loaded** have published those cards to a
public Logos Messaging topic and discovered each other's — each configured to
accept only a card naming the *other* account, because a Delivery node receives
its own published messages and a one-process demonstration of discovery is not
one.

**What is not delivered** is stated in the checklist and in
[`docs/limitations.md`](docs/limitations.md), and it is substantial: the owner
can never approve an above-threshold spend after anchoring a policy; the
**storage** skills have never been run against a live node, because nothing has
put a Logos Storage node inside the module's own process; no agent has yet
*served* another agent's task through to a terminal state, so discovery, the
lifecycle and the payment are three exercises rather than one; no model has ever
been run against the inference port; and there is no video.

The messaging half of that sentence used to be in it and has been taken out,
because it stopped being true: `messaging.send`, `messaging.join`,
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
| the deployed bytes | the transaction embeds this repository's copy of `agent_verifier.bin` verbatim |
| shipped ImageID, read off the on-chain `storage` anchor | `778a9341a00de46c4c056ac63a66f63156b068e61cce7f155a2b495e670c4661` |
| its ProgramId | `1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647` |

Every step of that is a chain fact, and none of it depends on a tool being installed. The deploy transaction is *derived* from the committed bytes rather than quoted, it resolves, and it contains those bytes — so the file in this repository is the program that was deployed. The anchor transaction that created these policy accounts names the ImageID it called, and `getAccount` reports that same ProgramId as the `program_owner` of every one of them. **The binary in this repository is the program enforcing these envelopes on chain.**

`spend` moves no balance itself — LEZ rule 5 refuses any post-state that decreases the balance of an account the executing program does not own — so it chains a call into the authenticated transfer program, which does own them. `artifacts/programs/authenticated_transfer.bin` is committed because the circuit resolves the callee by ImageID; it is not deployed by this repository. `getProgramIds` reports `authenticated_transfer` as `583309054,2344528779,3806558405,2890696795,2257354672,3978764116,2273929063,1518858078`, and that is exactly the `program_owner` the chain gives every agent's receiving account — which is why the policy program cannot move those balances itself and has to chain the call.

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

Every MET below names a command that was run and the output it produced. Nothing
here is upgraded on the strength of reading code: this repository has shipped
three separate claims that were true of the source and false of the binary, and
one of them — `meta.skills` — was documented in three headers before it existed.

### Functionality

- [ ] **UNMET — Module loads and runs inside Logos Core alongside the wallet,
  storage, and messaging modules without modifying them.**
  Half of this is demonstrated and half cannot be, on this host.

  **Loads and runs: yes, in the real runtime.** `./scripts/logos-core-headless.sh`
  (exit 0) `dlopen`s the real `liblogos_core` out of an installed
  `LogosBasecamp.app` and drives it through the same C API, in the same order, as
  Basecamp's own `app/main.cpp` — `logos_core_init` → `add_modules_dir` →
  `set_persistence_base_path` → `set_access_policy` → `logos_core_start` →
  `logos_core_load_module("agent", true)` — then `configure()` and `start()` over
  the runtime's transport:

  ```
  <- known modules: package_manager package_downloader capability_module agent
  ok logos_core_load_module() reports success
  <- loaded modules: capability_module agent
  ok configure() is accepted across the transport
  <- skills(): 23 entries
  ok it lists exactly 23 — no more, no fewer (got 23)
  ```

  A module that loads and offers nothing looks identical to one that works, which
  is why the skill count is asserted rather than the load. Basecamp itself is also
  running it: `ps` shows a `logos_host --name agent` whose parent is
  `LogosBasecamp.bin`.

  **Alongside those three: not possible here.** `ls
  /Applications/LogosBasecamp.app/Contents/modules/` returns `capability_module`,
  `package_downloader`, `package_manager`. Basecamp 0.2.2 ships no wallet, storage
  or messaging module, so there is nothing to load alongside — a statement about
  the host, checked by listing a directory, not an inference from a failed lookup.
  No submission can close this against this host.

- [ ] **UNMET — The agent has its own shielded LEZ account and can send and
  receive tokens independently of the owner's wallet.**
  *Send* is demonstrated: `./scripts/verify-deployment.sh` (exit 0) re-decodes 4
  settlements under the shipped program from the chain's own copy, each a
  privacy-preserving transaction signed by the agent's own shielded account, not
  the owner's. *Receive* at that same shielded account is impossible with the
  current `spel`, which fails with `KeyNotFoundError` before building anything, so
  each agent keeps a separate **public** account to be paid at — the `pay_account`
  column of [`artifacts/agents.tsv`](artifacts/agents.tsv) and the
  `server_pay_account` column of [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv),
  read by name. The criterion as written is not met, and closing it needs a
  `spel`/LEZ release rather than anything in this repository.
  See [`docs/limitations.md`](docs/limitations.md).

- [ ] **UNMET — The owner can deploy the agent and configure it with a single CLI
  command on any machine using Logos Core headless.**
  This has moved a long way and is still two commands, not one.

  **The Logos Core half now exists and is one command.**
  `./scripts/logos-core-headless.sh` (exit 0) installs `module/agent.lgx` into the
  user modules directory the way Basecamp's own installer does, builds the harness
  if it is not built, and runs load → `configure()` → `start()` headless — no GUI,
  no window, no display — binding the agent to the owner and policy account
  [`artifacts/agents.tsv`](artifacts/agents.tsv) actually records, then reading both
  back out of `meta.status`. So what is asserted is that the runtime is running
  *this* agent under *that* envelope, not merely that a module loaded. An earlier
  draft of this entry said `grep -rn 'logos_core' scripts/` returns nothing; it no
  longer does.

  **It is still two commands.** `SIGNER=… ./scripts/deploy-agents.sh` puts the
  identity and the spending envelope on chain; `./scripts/logos-core-headless.sh`
  runs the module in Logos Core. The criterion asks for one.

  **And still not "on any machine".** It needs an installed Basecamp for
  `liblogos_core` — there is no headless distribution of it to fetch — plus Qt
  6.9.2 and a `logos-cpp-sdk` checkout. Every one of those is checked before
  anything is compiled and named in the error when it is missing, so a machine
  that cannot run this says which piece it lacks instead of failing inside a
  compile. The on-chain half additionally needs a funded account, and a faucet
  cannot be scripted.

- [ ] **UNMET — The owner can interact with the agent in real time from a separate
  Logos app instance using Logos Messaging, with no intermediary server.**

  Four clauses. Three are now demonstrated and the fourth is not, and they must
  not be blurred.

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

  **From a separate Logos app instance — no, and that is the whole of what is
  left.** The owner in every run above is `module/tests/owner_responder.cpp`, a
  program written for the purpose with its own Delivery node, and the agent end is
  a harness rather than Basecamp. Two Logos apps have not talked to each other.
  Closing it means giving the `app/` console a Delivery-backed owner channel — it
  currently answers over Logos Core's own transport, which is the *other*
  criterion, below — and running two instances. The remaining gap is a host, not a
  transport, and the transport is no longer a claim: it is a script.

- [ ] **UNMET — The spending threshold holds above-threshold transactions for owner
  approval and executes below-threshold transactions autonomously.**
  The below-threshold half is demonstrated on the public testnet:
  `./scripts/use-cases/02-services-marketplace.sh` (exit 0) decodes six
  settlements from the chain's own copy, each price inside the paying agent's
  anchored per-transaction limit, and
  `./scripts/use-cases/03-spending-threshold.sh` (exit 0) reads the live ledger
  back at 60 of 1000 for period 8000 — the sum of the prices charged to it.

  The module's above-threshold half is demonstrated three ways: the loaded module
  holds and asks and gives up cleanly when nobody answers
  (`logos-core-headless.sh`, 7 attempts, terminal `owner_unreachable`,
  `submitted:false`); the owner answers over Logos Messaging and is obeyed in both
  directions (`delivery-in-plugin.sh approval`); and the owner answers from inside
  Basecamp (Usability, below).

  The **chain's** above-threshold half cannot currently work, and the reason is
  structural rather than a bug. The constraint measured on chain is one program
  transaction per public signer. `approve_spend` requires the owner as signer, and
  the policy commits `owner_id = sha256(owner account id)`, so the approval must
  come from the account that anchored the policy — which has already spent its one
  transaction on `create_policy`. **The owner who anchored a policy is, by
  construction, unable to approve anything under it.** Every approved spend above
  therefore returns `{"outcome":"approved","submitted":false}` and names the path
  it would take, rather than claiming a payment that did not happen. Refusal on
  chain is real and was watched: `Program error 6005: the spend needs an owner
  approval: use spend_approved`, with no transaction built. Two ways out are
  identified in [`docs/limitations.md`](docs/limitations.md) and neither has been
  tried.

- [x] **MET — All default skills implemented and documented.**
  All twenty-one are implemented **and registered**, which are different claims:
  thirteen skills were implemented here before anything registered them, and the
  module answered `skills()` with an empty card while looking perfectly healthy.
  `installBuiltinSkills` registers 23 skills — the prize's twenty-one, plus
  `agent.evaluate_task`, which the prize does not ask for and which is kept
  because it is the only skill on the pluggable-inference seam a *different*
  criterion requires, plus `messaging.receive`, which it does not ask for either
  and without which the A2A lifecycle only runs in one direction: `agent.task`
  puts a real request on the wire and, until it existed, the agent being asked
  had no skill that could read it — and `start()` calls it itself when no host wired the
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

  Asserted by execution against the **packaged artefact**, not a rebuild:
  `module/tests/plugin_load_test.cpp` loads the committed `module/agent.lgx`
  through `QPluginLoader` (exit 0) and `./scripts/logos-core-headless.sh` loads it
  through the installed Basecamp's own `liblogos_core` (exit 0). Both report 23
  entries, each with a parameter schema, `invoke()` dispatching to every one, and
  `meta.skills` listing all 23 — including itself — over the boundary.
  Documented in [`docs/skills.md`](docs/skills.md), and the count is gated:
  `./scripts/check-docs.py` (exit 0) reports `checked 19 skill-count mention(s)
  against the 23 the module registers`, and `examples/agent-console/run.sh`
  asserts `docs/skills.md §7 lists exactly the 23 skills the module registers`.

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
  reviewer's, and a card declaring `protocolVersion: "9.9.9"` would pass every
  check the repo itself has. And the 9×9 transition matrix is **the binding's
  own**: A2A v0.3.0 publishes no transition table, `docs/a2a-binding.md` §5.2 says
  so, and only 13 of the 81 cells are asserted anywhere.

- [ ] **UNMET — Two or more agents discover each other via Agent Cards, execute a
  task following the A2A lifecycle, and transfer LEZ payment autonomously, without
  owner intervention.**
  This is a conjunction of four things. Three hold, on the public network and on
  chain, and they have never yet held **in one flow** — which is what the sentence
  says.

  **Discovery: now real, and this is new.** `./scripts/delivery-in-plugin.sh peers`
  (exit 0) starts two modules loaded through `QPluginLoader`, each with its own
  Delivery node, its own LEZ account, its own wallet and its own working
  directory, on one public topic:

  ```
  ok this agent published its own signed card on the topic
  ok and discovered the OTHER agent's signed Agent Card over the public network
  ok which is signed — `require_signed` was on, so an unsigned card would not be
     in this list at all
  ok this agent opened an A2A task addressed to the other one
  ok and READ the other agent's A2A request off its own task topic
  ```

  The two-process shape is not decoration. A Delivery node receives its own
  published messages, so a single process can satisfy any "a card arrived"
  assertion with every other agent on earth switched off; each side here accepts
  only a card naming the **other** account, so neither could satisfy its own
  assertion. An earlier version of this entry claimed MET while the shipped plugin
  answered `agent.discover` with *no discovery transport is configured*. That is
  fixed, and the fix is the module building its own port rather than being handed
  one.

  **Payment: real, and independent of the owner.** The generated table below
  decodes four settlements under the shipped program out of the transactions
  themselves. `spend` takes no owner and no approval account; no agent wallet
  holds its owner's key.

  **What is still missing is the serving half.** The lifecycle transitions above
  are driven **locally by the client** through the real `TaskStore`; no agent has
  received a peer's request, moved it to a terminal state, published status back,
  and triggered the settlement. Discovery, the request crossing, the lifecycle and
  the payment are four exercises rather than one. `messaging.receive` — the skill
  that reads a request off a task topic — landed for exactly this reason and is
  the first half of it.

  The settlement table is **generated**, not transcribed; reproduce it with
  `./scripts/submission-evidence.py`.

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

**2 of the 6 settlements above predate the program this repository ships.** They are kept because they are on chain and a reviewer will find them, but the criterion they support is only supported by the 4 made under the current deployment.

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
    OK  6 settlement(s), each one decoded from the chain's own copy
    OK  it moved 5: payee and ledger both advanced by the advertised price
    OK  control: a transaction hash that cannot exist returns null

  NOTARY_VERIFY_ONLY=1 ./scripts/use-cases/04-privacy-notary.sh    # exit 0
    OK  1 notarisation(s), each verified from the chain against the document's own key
    OK  control: a document nobody notarised derives a key that appears nowhere

  ALERTER_VERIFY_ONLY=1 ./scripts/use-cases/05-event-alerter.sh    # exit 0
    OK  1 alert(s), each re-verified from the chain
    OK  control A: it reads as the default account, so the detector does not fire
    OK  control B: a transaction hash that cannot exist returns null
  ```

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

Each `create_policy` above was confirmed present in the block named and absent from both neighbours. The limits are the chain's own copy: the address of a policy account is `PDA(SHA256(owner ‖ agent ‖ per_tx ‖ per_period ‖ period_blocks))`, so raising a limit does not edit this record — it names a different address that `create_policy` never initialised, and the state machine rejects the spend before the program body runs.

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
  `./scripts/check-docs.py` (exit 0) reports `checked 334 paths, 142 link targets,
  7 line citations, 69 symbol citations … across 13 documents / every path, link
  target, line citation and symbol citation resolves`.

  One caveat a reviewer will see before they read any of it: the repository is
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
  ok  docs/skills.md §7 lists exactly the 23 skills the module registers
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
  beside it: `./scripts/check-package-fresh.py` (exit 0) reports `all 29 build
  inputs hash exactly as they did when the package was made` and `every one of the
  701 source literals of >= 8 bytes is in the darwin-arm64 binary` — a check that
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
  LP-0008 Agent, lp-0002-multisig, lp-0003-airdrop, Applications,
  Package Manager, Settings
  ```

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
  both packages are installed by hand, by a reviewer as much as by us. And both
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
  `module/tests/task_persistence_test.cpp` (exit 0, 121 assertions) covers the
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
  Program, three anchors and six settlements all live on the public testnet, each
  re-verified for this document with a null-returning control:
  `./scripts/verify-deployment.sh` (exit 0),
  `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md` (exit 0), and
  `./scripts/demo.sh` (exit 0) from a clean clone with only a Rust toolchain.

- [ ] **UNMET — End-to-end integration tests run against a LEZ sequencer (standalone
  mode) and are included in CI.**
  The workflow exists and is wired in. `.github/workflows/e2e-local-sequencer.yml`
  builds the LEZ workspace at pinned revision `47eba25`, installs `r0vm` 3.0.5, and
  runs the full lifecycle with `RISC0_DEV_MODE: 0`. It has **no skip path**,
  deliberately: a competing submission was closed with "the standalone-sequencer
  E2E did not run in CI; the job completed through its explicit skip path".

  **It is red on `main`, and that is the verdict.** The most recent run against
  this branch is
  [31903623142](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31903623142),
  `failure`, ending in `error: create_policy failed` after `RISC0_DEV_MODE: 0` and
  a successful deploy — the script calls `create_policy` without the `claim_agent`
  the current program requires. The last green run on this branch,
  [31867735056](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31867735056),
  is a long way back; `git rev-list --count 5a52efe..HEAD` says how far, and an
  earlier version of this line guessed "four", which was wrong by an order of
  magnitude. Green runs of the same workflow exist on other branches, and their
  commits are **not** ancestors of this one, so they are not evidence about this
  tree either. Do not check the badge; run
  `gh run list --workflow e2e-local-sequencer.yml --branch main --limit 1`.

- [x] **MET — CI must be green on the default branch.**
  All six jobs of the `CI` workflow are green at the tip of `main` — `Policy
  primitive and its adversarial tests`, `The committed program matches its
  recorded ImageID`, `The skills behave, against fake ports`, `The shipped .lgx
  was built from the committed source`, `A real Storage node takes a file and
  returns its address`, and `The illustrative use cases verify against the public
  testnet`. Do not quote a run id from this paragraph — "latest" moves, and the id
  written here was already two runs stale by the time anyone read it. Ask instead:
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.

  Two things stated because a reviewer will meet them. **This has been red
  recently and for real reasons**, each of which is worth more than the badge: a
  missing `<cstdint>` killed six C++ suites at the *first* compile step while the
  summary said only "one job failed"; the `use-cases` job could not build `spel`
  against the pinned LEZ revision; and the packaging job's own negative control —
  a package whose binary was swapped — failed. Each was a gate catching something,
  and the last two were fixed within three commits. **And the separate `e2e vs
  local sequencer` workflow is red on this branch** (above), so the Actions tab
  shows a red run against `main` even while `CI` is green. That is not a caveat on
  this criterion; it is the criterion above, and it is marked UNMET there.

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
  (exit 0) checks 334 paths and 142 link targets across 13 documents, so no
  command in the README names a file that is not there.

  The fourth clause — the Logos app owner channel — is what this entry failed on
  for most of this project's life, and it failed for a good reason: the walkthrough
  described a path the software said could not exist. It exists now, §8a documents
  it, and the round trip in the Usability entry above was driven by following it.

- [ ] **UNMET — A reproducible end-to-end demo script that works against a real
  local sequencer with `RISC0_DEV_MODE=0`.**
  `scripts/e2e-local-sequencer.sh` is that script, and it is the one that failed in
  run 31903623142 above — `error: create_policy failed`, at a commit that is an
  ancestor of this one, with no successful run on `main` since. So it is not
  currently a script that works, and saying otherwise on the strength of a run 148
  commits back is exactly the kind of staleness this document has been wrong about
  before.

  `./scripts/demo.sh` does run from a clean clone with only a Rust toolchain — no
  funded account, no keys, no local sequencer — and is green at this commit. It
  runs against the public testnet, so it answers a different question and is not
  offered as this one.

- [ ] **UNMET — A recorded video demo showing terminal output confirming
  `RISC0_DEV_MODE=0` was active.**
  Not recorded. Blocker 1, and the only blocker with irreducible work in it.

**Tally: 14 MET, 9 UNMET, of the 23 criteria the prize lists** — Functionality
5 of 11, Usability 2 of 2, Reliability 3 of 3, Performance 1 of 1, Supportability
3 of 6. The commit this tally describes is the one recorded at the top of this
document, and it is recorded there only — a count anchored to a commit id in two
places is two places to forget.

## FURPS Self-Assessment

### Functionality

The agent holds a shielded LEZ account, signs its own transactions, and spends
under a ceiling the chain keeps in state only its own program may write. All
twenty-one default skills are implemented and registered — `meta.skills`, the
last one missing, was documented in three headers while `invoke()` refused it,
and is now asserted against the loaded binary rather than against the source.

The A2A coordination path has to be described in parts, because it runs in parts.
**Cards and discovery run on the public network**: two modules a host loaded
published signed cards to a Logos Messaging topic and each found the other's.
**Settlement runs on chain**, unattended, inside an anchored envelope. **The task
lifecycle runs through the real `TaskStore`** and is driven by the client rather
than by the peer that received the request — so a task request crosses between
two agents and is read by the far side, and nothing yet serves it back to a
terminal state and pays on completion. This paragraph previously called the whole
path "the part that has actually run", which read as one flow and was four.

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
red. That control exists because `TaskPersistence` once had 121 green assertions
and no construction site in the plugin — the tests passed and the shipped module
persisted nothing.

Against that: the assertion counts in this repository have twice included
assertions that could not fail, and a sweep of 1,699 of them found 43. The number
of green checks is not the measure; the negative control beside each one is.

### Performance

No LEZ execution meter exists to measure on v0.2.4, so the document measures the
budgets that do exist rather than inventing a CU number. One qualification, because
a reviewer grepping the pinned tree will find it: `mantle::gas` **does** exist there,
as the bedrock L1 publish fee. It is not a compute meter for LEZ execution, and no
figure here is derived from it. A settlement's size on the wire is read back
from the sequencer rather than estimated, and is printed in the generated settlement
table above rather than restated here. Cycles are measured against the
32M public-execution cap by running the deployed binary; the settlements take the
privacy-preserving path, which is bounded by the prover rather than by that constant.
The real bottleneck is proving time, and the real operational cost is that anchoring
is one-shot per signer.

### Supportability

The `CI` workflow runs six jobs — the policy crate and its adversarial tests, the
committed program against its recorded ImageID, the C++ suites against fake ports,
the shipped `.lgx` against the source committed beside it, a real Storage node, and
the illustrative use cases against the public testnet. A seventh workflow runs the
end-to-end lifecycle against a real standalone sequencer at `RISC0_DEV_MODE=0` with
no skip path. The CI file documents, in comments, exactly which suites do **not**
run there and why — Qt and an installed Basecamp for the load tests, a Nim and
`librln` build for the node drives — because a suite silently absent from CI is
indistinguishable from one that was never written.

**Do not read a badge here; run the command in blocker 2's row.** CI has been red
on `main` three times in the recent past, and each failure is more useful than the
green either side of it. A missing `<cstdint>` killed the skills job at its *first*
compile step, so six suites did not run while the summary said only that one job
had failed. The use-case job could not build `spel` against the pinned LEZ
revision. And a coverage floor added by one piece of work began reading a line of
output added by another — it selected the first line starting with `checked ` and
found a skill count where it expected a path count, failing a healthy run. That
last one is this repository's own defect class pointed at itself: two guards, each
correct, one parsing the other. A job that fails early and a job that passes having
tested nothing look similar from the outside, which is why the workflow asserts on
the `SKIPPED` banner as well as on exit codes. Debuggability is otherwise deliberate —
every failure path returns JSON naming the skill and the half that failed, and
`share` takes both ports specifically so it can say whether storage or delivery
failed rather than blaming the wrong one.

## Supporting Materials

- 🎥 **VIDEO DEMO — PLACEHOLDER, NOT YET RECORDED**
  `<<< VIDEO URL TO BE INSERTED HERE >>>`
  Must be a narrated walkthrough — a silent screencast is explicitly insufficient —
  covering ≥3 illustrative use cases, with terminal output visible confirming
  `RISC0_DEV_MODE=0`, against the **public testnet** rather than a localnet. Three
  use cases are demonstrable end-to-end today — the agent services / paid skill
  marketplace, the privacy-preserving notary and the on-chain event alerter — plus
  the personal file vault against real Storage and Messaging nodes, which records
  nothing on chain. An earlier version of this line said only one was, which was
  true when written and has not been true for some time.

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
