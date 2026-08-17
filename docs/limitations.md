# Limitations

What does not work, stated before anyone has to discover it.

## Closed: anchoring a policy over somebody else's agent needed no key at all

Kept because it was the most serious defect this program has had, and because
the previous entry under this heading claimed it was fixed when the fix had
traded one hole for a worse one. Both are recorded.

The previous deployment (`a780003b…`) made the policy account a PDA of the agent
alone, so that a second policy for an agent could not exist. That was right
about *where* a policy goes and silent about *who* may put one there. Its
`create_policy` declared two accounts — the policy account, and a signer it
recorded as the owner. The agent's own account was never declared, never read
and never asked to sign, and `agent_id` was a free `[u8; 32]` argument the body
discarded. So the only thing an attacker needed was the agent's **public id**,
which this repository prints in `artifacts/agents.tsv` and inside every signed
Agent Card, because the card's `url` is the account id.

The old design bounded this to "an attacker who holds the agent's key". The
replacement needed no key, and it was worse in both directions:

- `per_tx = per_period = u128::MAX` over an agent the attacker does not control
  is theft-in-waiting;
- `per_tx = 0` over the same agent is a **permanent denial of service** — the
  agent can never spend and the honest owner can never anchor, because `init`
  refuses every later write at the only address that agent has and LEZ rule 4
  (`ModifiedProgramOwner`) means no `close` instruction can ever exist;
- and because a policy account is a PDA of the *program*, every guest rebuild
  moves all three anchors to fresh, publicly computable addresses, so the race
  reopens on every redeploy.

**The fix is consent, and it is a second signature.** Anchoring is now two
transactions from two wallets:

- `claim_agent`, signed by the **agent**, writes the id of the one account
  allowed to anchor over it into `PDA(program, ["agent-owner/v1", agent])` — an
  address derived from the signing account, so there is no `agent_id` argument
  to substitute;
- `create_policy`, signed by that **owner**, refuses to run unless that claim
  account exists, was created by this program (6019), and names the signer
  (6020).

A stranger holds neither key. `update_policy`, signed by the owner the record
names (6012), is the way back from a wrong anchor and the brake on a suspect
agent — so losing an address is no longer permanent either.
[`security-model.md`](security-model.md) §2 and §4 carry the full argument.

**Executed against the chain, not asserted.** One `create_policy` for
`per_tx = per_period = u128::MAX` over the storage agent
`9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE`, from accounts created for the
purpose that have never held that agent's key:

| Program | Signer | Transaction | Result |
|---|---|---|---|
| `a780003b…` | `RZmSLJAB…` | `eedb3caf…` | **accepted**, block 8869 |
| `697746f5…` | `Acissmag…` | `60de3fc6…` | never included, 6020 |

The accepted one is still there and still says what it said — policy account
`5QAVJAMHkpLnAMht3bonFijyApPZfAccHAFbzByNq8VV`, owner `064afcfc…`, `per_tx =
2^128-1` — so this does not rest on the absence of a transaction. The honest
owner then anchored the same agent under the current program at the address the
attack aimed at (`6857ba23…`, block 8868), which is the other half of the
property: the race is not lost, because there is no race.

And demonstrated by execution against the binary, in
`crates/agent-verifier-adversarial`, where the previous suite had this very call
written down as **required behaviour** (`"an agent nobody has anchored yet still
can be — accepted"`):

```
ok  refused [6019]: an attacker anchors an UNLIMITED policy over an agent nobody
    has claimed
ok  refused [6019]: the same call with per_tx = 0 — the denial of service, not
    the theft
ok  refused [6020]: an attacker anchors over an agent that designated somebody
    else
ok  refused [6020]: the compromised agent itself anchors, as both owner and agent
ok  a separate account the attacker controls: the anchor refused [6020], the
    honest owner anchored afterwards, the whole-balance spend refused [6005]
    against that policy, and the attacker's own approval refused [6012]
```

## An approval used to be a bearer instrument, and now expires

Same class, found in the same pass. `spend_approved` returned a bare
`SpelOutput::execute` with no block validity window, so an owner approval was
valid in **every block, forever**: an owner who approved one payment had signed
something redeemable months later, the day the agent's key was stolen, against a
period ledger the approved path does not draw on. It could not be revoked
either, because `approve_spend` is `init` and the marker cannot be re-issued to
cancel it.

`approve_spend` now takes `expiry_block`, writes it into the marker, and
`spend_approved` pins the transaction to `[0, expiry_block)`. An expired
approval is not a refusal this program returns — it is a transaction no block
will include (`OutOfValidityWindow`). `expiry_block = 0` is refused at issue
(6021), because an empty range cannot be pinned and "no window" is the defect.

What this does **not** give is revocation before the expiry. An approval the
owner regrets stands until its block. Shortening the horizon is the only control
there is, and it is the owner's to choose.

## Anchors die with every guest change, and the guest is effectively frozen

Not currently broken, but the sequencing constraint is permanent and it has now
bitten twice.

Both of an agent's accounts are PDAs derived from the program id, so rebuilding
the guest changes the ImageID, changes the program, and orphans every claim and
every anchor made under the old one. That has now happened four times. The
current deployment is `697746f5…cb5370bf` (ImageID `778a9341…e670c4661`, block
8839), and the three agents are claimed and anchored under it — see
[`artifacts/anchored.tsv`](../artifacts/anchored.tsv), which is keyed by
`(program, instruction, agent)` for exactly this reason.

So criteria 1 and 2 must be satisfied under the *same* program, and the guest
has to be final before any anchor counts as evidence. Re-anchoring now costs
**two** transactions per agent rather than one, and the first of them is a
privacy-preserving proof signed by the agent itself, which takes minutes rather
than seconds. It also costs three public signers that have never signed, for the
reason below.

One consequence that is easy to miss until it bites: `claim_agent` is signed by
the agent's **shielded** account, so that account must hold a live note. An agent
whose balance has been spent to zero has no note to spend and cannot sign
anything, which is not a state a re-anchor can recover from by itself. That is
what happened to the storage agent here — it had been drained to 0 by an earlier
recycling — and the fix was to move 10 LEZ back to it from its own public pay
account before claiming. A shielded transfer mints a **new note with a new
account id** rather than crediting the old one, so the storage agent's id moved
from `7o9PT8uE…PGPEUM6` to `9Xpkkvos…jfv1FaE` in the process. Fund first, claim
second, anchor third, and never fund an agent after it has claimed.

**"Rebuilding the guest" includes editing a comment.** This is not a figure of
speech and not a margin of caution. `#[lez_program]` generates a `panic!` for a
refused instruction, and a Rust panic carries `core::panic::Location` — file,
line and column — into the binary; the executor prints it as
`agent_verifier.rs:176:1`, the line the macro sits on. Add a line anywhere above
it, including in the header comment, and that becomes 177, the ELF changes, the
ImageID changes, every policy PDA moves, and the committed binary no longer
hashes to its deploy transaction. Three agents then need re-anchoring, and each
needs a signer that has never signed.

The practical consequence is that **the guest source is frozen between
deployments**, and a documentation fix inside it is not free the way a
documentation fix in this directory is. Comments in the guest are therefore
written to last, and corrections that would otherwise live next to the code live
in `docs/` instead. That is a deliberate trade and the reason some explanation
here is longer than it would need to be if it could sit in the source.

## WITHDRAWN: "spel builds every transaction against nonce 0"

This heading was this file's diagnosis of the whole "submitted, returns a hash,
never lands" family, and it is wrong. It is kept rather than deleted because it
was confidently argued from real measurements, and because the measurements are
still true — it is the causal claim on top of them that is not.

What it said: that `spel` hardcodes nonce 0, that the sequencer checks the nonce
for exact equality (`lee/state_machine/src/validated_state_diff/mod.rs:499-505`,
which it does), and therefore that a public signer's second transaction is always
dropped — so **any flow that needs a public signer to act twice is unavailable**.

Two things falsify it:

- `spel` fetches the nonce. `vendor/spel/spel-cli/src/tx.rs:625-635` calls
  `get_accounts_nonces` for every signer and exits rather than guessing if the
  fetch fails; the value goes straight into `Message::new_preserialized` a few
  lines below. There is no literal 0 on that path.
- A public account reached **nonce 33** on this testnet (`DumJ4LCB…`), and the
  owner used in the section below went 1 → 2 → 3 across an `auth-transfer init`,
  a `create_policy` and an `approve_spend`. A client that always sent nonce 0
  could not have produced either.

The observation underneath — that the signers *in this repository* sit at nonce 1
and never advance — is accurate, and it has a different cause: those accounts are
unclaimed, so the SPEL macro drops them from the post-state and the transaction
is rejected before the nonce is ever compared. That mechanism, its file:line, and
the transactions that close it are in
[the section below](#closed-an-owner-can-approve-a-spend-and-the-approved-spend-executes).

`update_policy` is still the one instruction with no landed transaction on the
public testnet, and now for a stated reason rather than a supposed one: it is
exercised against the deployed binary in `crates/agent-verifier-adversarial`
(accepted for the owner the record names, refused 6012 for a stranger and for the
agent itself), and running it on chain needs an owner that was claimed *before* it
anchored. The three owners in `artifacts/agents.tsv` were not, and that is now
irreversible for them.

## CLOSED: an owner can approve a spend, and the approved spend executes

The above-threshold path now runs end to end on the public testnet. What this
section used to say — that it could not, ever — was true of the accounts it was
measured on and false as a general rule, and both halves are kept because the
constraint is real and anyone anchoring a policy still has to design around it.

### What this said, and what was right about it

It said the constraint was **one program transaction per public signer account**,
that `approve_spend` must be signed by the account named in the policy record's
`owner` field, and that the account had already spent its one transaction on
`create_policy` — so the owner who anchored a policy was, by construction, unable
to approve anything under it.

Every one of those observations reproduces. What was wrong was calling it a
property of *public signers*. It is a property of **unclaimed** public signers,
and the difference is one transaction taken before the anchor rather than after.

### The rule, as the state machine actually applies it

Three pieces, and none of them is about nonces:

1. The vendored SPEL macro filters accounts out of a program's post-state
   (`vendor/spel/spel-framework-macros/src/lib.rs:303-328`). It drops any
   `(pre, post)` pair where the pre-state's `program_owner` is the default **and**
   the pre-state is not `Account::default()` **and** the post carries no claim.
2. `create_policy` and `approve_spend` both *declare* `owner`, so a dropped owner
   is a declared account missing from the output, and the state machine rejects
   the whole transaction — `DeclaredAccountMissingFromOutput`
   (`lee/state_machine/src/validated_state_diff/mod.rs:310-319`).
3. The filter exists to dodge LEZ rule 7
   (`lee/state_machine/core/src/program/mod.rs:730-738`), which refuses a
   post-state that keeps the default program owner when the pre-state is no
   longer `Account::default()`.

A fresh public account *is* `Account::default()`, so its first program
transaction passes. That transaction moves it off default, and every later one is
dropped. **An account whose `program_owner` is not the default is never filtered
and rule 7 never fires, so it can sign indefinitely.**

### "Funded" was the wrong word — the owner needs to be *claimed*

The way out recorded here was "make the owner account program-owned before
anchoring (a funded account is exempt — `DumJ4LCB…` holds two landed anchors)".
The direction was right and the mechanism was mis-stated. `DumJ4LCB…` checks out
as far as it can be checked: `getAccount` reports it owned by the authenticated
transfer program and sitting at **nonce 33**, which is thirty-three landed
transactions from one public account. The two specific anchors it was said to
hold, at blocks 8050 and 8051, are not recorded in this repository and could not
be re-derived, so the count is cited from the account rather than from them.

What is *not* required is a balance. The owner used below holds **0 LEZ** and has
signed two program transactions:

```
HCV2Y4Vf…  {"program_owner":"J8otq1J8Zpjhhpp6FPfhFtWKTCkLjthdk12cwHiMZCTB",
            "balance":0, "nonce":3}
```

What moves an account off the default owner is a **claim**, and
`wallet auth-transfer init` is a claim with no transfer attached
(`lez/programs/authenticated_transfer/src/main.rs:10-19`). Crediting an account
claims it too (`:50-55`), but that is a side effect and not the point.

### The window to claim closes, and for the three shipped owners it has closed

This is the part the old "ways out" could not have known, and it is why the fix
is not retroactive.

`initialize_account` asserts the account is `Account::default()`
(`lez/programs/authenticated_transfer/src/main.rs:14-17`), so it will not claim an account
that has already done anything. A *credited* claim is checked no more loosely:
`validate_execution` runs before the claim loop rewrites `program_owner`
(`validated_state_diff/mod.rs:195-247`), so at the moment rule 7 is evaluated the
post-state still carries the default owner, and an account with a non-default
pre-state is refused.

So a public account can be claimed **only while it is still pristine**. The three
owners in [`artifacts/agents.tsv`](../artifacts/agents.tsv) each spent their
pristine state on `create_policy`, and all three now read `nonce: 1` with the
default program owner and no balance. Measured rather than inferred: an
`auth-transfer init` against the storage owner `2dA9APZ…` submitted
`a010f68242f120f167643836c586169daf29e5aff61886ea0faabe6d217d8d44` and **was
never included** — ten blocks, six hundred seconds, and the account still reads
exactly as it did before. Those three owners are frozen: they cannot sign again,
cannot be claimed, and cannot be credited. `update_policy` over the shipped three
is unreachable for the same reason and stays unreachable.

The fix is therefore **an ordering requirement on new deployments**: claim the
owner before it anchors. It does not rescue an owner that has already anchored.

### The demonstration

Run against the shipped program `697746f5…cb5370bf` — the same deployment the
three agents are anchored under — with a fourth agent provisioned solely for this,
because a ceiling of 1 LEZ makes "above the threshold" cost 2 LEZ instead of the
51 the storage agent's ceiling of 50 would have needed. No existing agent was
re-anchored and no policy was updated.

| step | tx | block |
|---|---|---|
| owner `HCV2Y4Vf…` claimed, `auth-transfer init` | `ce2a23ec7168b6c48bf31098748d3c7e653a3ded7c8c42503f6291769ffae35c` | 10759 |
| agent funded, 3 LEZ from `5Sa13Ny…` | `c33f9a567d0ef9bae1804c24d71641a445e13c95305bf9dbab79e99e22685ec1` | 10765 |
| `claim_agent`, signed by the agent | `06b8e870d01c25be973957842dca8100e3b920984920dcee0d992b7bcd184a0d` | 10774 |
| `create_policy`, the owner's **first** program transaction | `e684df9caa5012cdcbfdd0abc9c60ae53d352f8e23853413af323c7cd4cb2cc1` | 10775 |
| `approve_spend`, the owner's **second** | `4104dde4f504d42862ac89056e2476c414c4342dc145934c8a238382863841d8` | 10776 |
| `spend_approved`, signed by the agent | `c243eaedfcbba87dc11d5ad28aad4f8424916d087adf8c811747169668169597` | 10786 |

The agent is `Private/2irWK3sw…FRFP`, its policy account `DaFSZy2u…V4kJ` holds
`per_tx 1, per_period 10, period_blocks 1000`, and the approval marker is
`GXEHQssw…kXmC`.

**Holds above.** The autonomous path refuses the same 2 LEZ, and refuses it
before anything is submitted:

```
Program error 6005: the spend needs an owner approval: use spend_approved
```

**Executes below**, and now above. The payee `BzYks91a…wLnu` — the blockchain
agent's public receiving account, so `getAccount` can read it — went **4 → 6**,
and the paying agent went 3 → 1. The approval marker went `spent 0` → `spent 1`,
which is what makes it single-use.

One thing the ledger deliberately does not show: the policy account's running
total is still `spent 0`. `spend_approved` declares `policy` read-only and says
why — "an approved payment is bounded by the approval, not by the envelope". The
per-period budget meters the autonomous path; an owner approval is not drawn
against it. That is the program's design, not a miscount, but it does mean the
policy ledger is not where an approved payment is recorded — the marker is.

### Also recorded, because it was asserted here and is false

The `account get --account-id "Public/$signer"` refresh added to
`scripts/deploy-agents.sh`, with a comment claiming it "is what makes the next
anchor provable", does nothing. A second anchor was run with exactly that refresh
in place, against freshly fetched chain state, and still did not land.
Re-importing the signer into a completely fresh wallet home does not help either.
The gate reads the signer's on-chain state, not the wallet's — and what it reads
is the program owner.

### Exactly where this limitation begins, and what is above it

This is worth drawing a line through, because the module half and the chain half
of the approval path fail in different places and only one of them is broken.

The prize's Reliability criterion asks that "above-threshold transactions that
fail to reach the owner for approval are not executed — the agent retries
notification before timing out and reports the failure". **All three clauses are
module behaviour, and all three hold in the shipped module.** `wallet.send`
publishes the request under a correlation id, re-publishes the byte-identical
request every `approval_resend_ms` until `approval_timeout_ms`, and then answers
`{"submitted":false,"outcome":"owner_unreachable","attempts":N,…}` without ever
calling the wallet. Measured through Logos Core's own transport, against the
packaged module loaded into Basecamp 0.2.2's runtime:

```
wallet.send above threshold: {"attempts":8,"delivered":8,"outcome":"owner_unreachable",
  "submitted":false,"error":"the owner did not answer within 1500ms: 8 notification
  attempt(s), 8 of which the channel accepted; the spend was not submitted", …}
```

with eight `emitEvent: "ownerApprovalRequested"` lines in the runtime's log
between the call and the answer.

The step after a *successful* approval is the one that used to be missing, and it
is missing no longer. An approved above-threshold spend is submitted through the
policy program's `spend_approved`, which requires an approval account that only
the owner's own `approve_spend` signature can create. The module reaches that
instruction through `WalletPort::spendApproved`, wired by
`AgentModuleImpl::submitApprovedSpend` to a command an operator names under
`approved_pay_signer` — the same delegation `card_signer`, `pay_signer` and
`policy_source` use, because a 4 MB Qt plugin links no wallet and a host cannot
hand a `std::function` across the plugin boundary.

**What a caller supplies, and what the module supplies for it.** `wallet.send`
takes `recipient` and `amount` and nothing else; the nonce is minted by the
module, the correlation id is derived from the nonce, and the approval marker
seed is derived here by `approvalMarkerSeed` exactly as the chain derives it.
The operator supplies one setting, `approved_pay_signer`, naming a command. The
module runs it with **no arguments** and a JSON document on stdin —
`{"kind":"spend_approved","agent","recipient","amount","nonce","marker_seed",
"request_id"}` — and reads one line back.

**What it refuses.** Anything that is not 64 lower-case hex characters is not a
settlement and is reported as `{"submitted":false,"outcome":
"approved_not_submitted"}` with the reason: an unset `approved_pay_signer`, an
unset `agent_account` (the marker's address derives from it), a marker that
cannot be derived from the terms, a signer that printed a diagnostic, and a
receipt claiming success with no hash. `pay_signer` is deliberately **not**
accepted as a substitute: that command performs the *autonomous* `spend`
instruction, which the chain refuses above the anchored per-transaction ceiling
— and for a spend held for the owner because the *period total* was unknown
rather than because the amount was too large, it would succeed, meter the
payment against the per-period budget, and leave the owner's approval marker
unspent and redeemable until its expiry block. A submitter that does not know
about approvals is therefore absent here, never substituted.

**One attempt, and never a second.** The marker is single-use — `spend_approved`
sets its `spent` byte as it consumes it — so a retry of the same terms is either
a transaction the chain refuses or a duplicate of a payment already in flight.
Neither is the module's to choose, and the alternative a retry loop would need is
a fresh nonce, which is a fresh approval, which is the owner's to give. So the
submission happens once and whatever comes back is reported as it comes back.
The residual risk is stated rather than designed around: if the signer submits
and then dies before printing the hash, the module reports `submitted:false` for
a payment that may have settled. `wallet.send` has no task id to journal against,
so the task-payment journal below does not cover it; what covers it is that the
marker is spent, so the *next* attempt at those terms cannot move money again.

For the three **shipped** agents the chain half remains impossible, and for the
reason above: their owners anchored while unclaimed and can never sign
`approve_spend` again. The demonstration in the table above ran against a fourth
agent provisioned for it.

In one line: **the agent's side of the approval exchange works and is tested;
the chain's side works too, and is demonstrated above; and the module now
submits on approval instead of stopping at it — but only for a policy whose
owner was claimed before it anchored, which the shipped three were not.**

## The double-payment journal is called by the code that pays

Recorded here because for several releases it was not, and because the shape of
the miss is the one this file exists for: every part worked and the assembly did
not exist.

`module/src/task_persistence.h` opens with three things a restart must not do,
the first being "it must not pay twice", and describes the mechanism precisely —
`notePaymentIntent` made durable *before* the wallet is called, `mayPay`
refusing afterwards until a human reconciles. `TaskPersistence` implemented all
of it and 122 assertions covered it. **Nothing in the module called any of it.**
`agent.task` went straight to `TaskPort::pay` with no guard and no write-ahead
record, so `LoadReport::uncertainPayments` — which `meta.status` reports as
`durability.uncertain_payments` — could not be anything but zero, and the number
an operator was told to act on was a number nothing could produce.

It is wired now, through `TaskPort::journal`:

1. `mayPay` before the request goes out, so a peer is not asked to do work this
   agent already knows it will refuse to pay for;
2. `mayPay` again immediately before the wallet call, then `notePaymentIntent`,
   which does not return true until the record is on the medium. If it returns
   false the payment does **not** happen and the task fails, saying so;
3. `notePaymentSettled` once a hash comes back. A failure there does not fail the
   call — the money moved — it answers `settlement_recorded:false` beside the
   settlement and lets the module's own checkpoint retry the write.

The intent write does one more thing it is relied on for: it snapshots the task
store, so the task is on disk before the money moves. Before it, the first thing
ever written about a priced task was the checkpoint *after* `invoke` returned —
after the payment — so a process killed in between left a paid task no restart
had heard of, and a caller retrying the same id opened a fresh task and paid
again. `module/tests/restart_kill_test.cpp` kills a process inside `pay` with
SIGKILL and asks the restarted agent for that same task: the wallet is not
called a second time, and `meta.status` reports `uncertain_payments: 1`.

**What is deliberately not wired, and why.** `notePaymentAbandoned` still has no
production caller. It is the one call that makes `mayPay` say yes again for a
task that once reached the wallet, and its own header says why the agent must not
make it about itself: it requires evidence of what was checked on chain, and "the
transaction hash came back empty" is not evidence that nothing settled — only
that nothing was seen to. A signer that submitted and then died returns exactly
the same empty string. This process links no sequencer client and has no way to
look, so an empty hash leaves the intent standing and the answer says
`{"outcome":"payment_unresolved","reconcile":true}`.

The consequence, stated plainly: **a payment that does not settle leaves a
permanent `uncertain_payments` count that this module cannot clear.** The task
itself is closed — it goes to `failed`, which is terminal, so nothing will buy it
again — but the journal entry stays `intended` across every subsequent restart,
because clearing it honestly requires a human who has looked at the chain. There
is no way to supply that evidence through the module today: a reconciliation
entry point would have to be a registered skill or a module method, and both of
those are surfaces this change did not open. Until one exists, reconciliation is
an operator reading `durability.needing_reconciliation` out of `meta.status`,
checking the named account on chain, and — if the payment did not settle —
editing or removing the snapshot file with the node stopped. That is a worse
answer than a wired call and a better one than an agent that decides for itself
that its own money never moved.

Three things that used to be in this file are gone from it, because they were
fixed rather than reworded: `spend` moved no balance at all, a second
`create_policy` from one signer was silently dropped, and a repeat A2A
settlement could not be produced.

The last one was first shown by `5a488f28…` at block 8677 and `f780df62…` at
block 8686, with the recipient going 50 → 75 → 100 by `getAccount` (it had
already reached 50 in blocks 8605 and 8624, under the program before that).
**This paragraph called those two "under the current program", and they are
not.** Both carry the ImageID of `8c87cc9b…`, which is superseded — readable off
the transactions themselves, which is how it was caught, and which is the same
defect that once had four documents describing a dead deployment as live.
Repeat settlement under the *shipped* program `697746f5…` is a longer list now
and it is deliberately not counted here;
[`docs/DEPLOYMENT.md`](DEPLOYMENT.md)'s ledger carries every one with its block,
and `./scripts/verify-deployment.sh` re-attributes each from the chain rather
than from the manifest. What is still true is below.

## The policy bounds the policy path, not the agent's balance

The largest thing this program does not do, stated at the top of the list it
belongs to rather than implied by omission.

**An account holder can always call the program that owns its balance.** An
agent's LEZ is held by LEZ's authenticated transfer program — `getAccount` on a
funded agent reports
`"program_owner":"J8otq1J8Zpjhhpp6FPfhFtWKTCkLjthdk12cwHiMZCTB"` — and that
program's `transfer` asserts only that the sender is authorized
(`lez/programs/authenticated_transfer/src/main.rs`). So whoever holds an agent's
key can move that agent's balance by calling the transfer program directly, with
the policy program nowhere in the transaction. `wallet auth-transfer send --from
Private/<agent>` is that call, and it is the same command this repository uses
to fund agents.

That is not a bug in this program and it cannot be fixed inside it: LEZ rule 5
lets a program debit only accounts *it* owns, so a policy program can gate a
balance only if the balance lives in an account the policy program owns. Doing
that means the agent's money is held by `agent_verifier` rather than by
`authenticated_transfer`, which changes how funding works, what `getAccount`
shows, and what an agent can do with an unrelated wallet. It is a different
design, not a patch.

So the guarantee is in two parts, and the second is the one that changed today:

- **a compromised agent cannot use the policy path to exceed its policy** —
  every claim in [`security-model.md`](security-model.md) §7 is scoped to that
  path;
- **a party who is not the agent cannot touch the policy at all** — which is
  what the two-signature anchoring above establishes, and which is *not* scoped
  to the policy path, because a stranger has no other path either.

The residual the two-signature fix closes is therefore griefing and
impersonation rather than theft from a key-holder, and it is worth being exact
about that rather than claiming more.

**Where the module's `pay_signer` sits in that picture, since it is the obvious
place to look for a hole.** `agent.task` settles by running a command an operator
named in `meta.configure`, and the command runs `scripts/agent-spend.py --settle`
with the agent's wallet home. Two things follow, and they point in opposite
directions.

It does not widen the *policy* surface at all. The command performs the policy
program's `spend` instruction — the autonomous branch, the same call
`scripts/a2a-task.sh` makes — so the chain applies both anchored limits to it.
There is no branch in that script that reaches `authenticated_transfer` without
going through the policy account first, and `./scripts/delivery-in-plugin.sh
signers` pins the module's side of the same discipline: an envelope it cannot
read is *unknown*, unknown is outside, and a price outside never reaches the
signer.

It does widen who can *run* the key, and that is worth saying plainly: anyone who
can run the configured command can spend up to the envelope. But that is already
true of `card_signer`, which is a signing oracle over the same account key, and
it is bounded by the paragraphs above rather than by the delegate — a party who
can run commands in the agent's account can call the transfer program directly
and is not limited by any envelope at all. The delegate is strictly the more
constrained of the two things such a party could do.

That is also why `examples/agent-console/console.cpp` still leaves
`WalletPort::spend` null. Its argument — "a console that could sign would be a
second, unaudited spending path around the anchored policy" — is about a tool an
*operator* drives, where the spending would be additional to the agent's. The
module's own settlement is the agent's, through the policy, and is the same path
rather than a second one.

## FIXED: a shielded agent can now be paid at its shielded account

This section used to say the opposite, and the correction is worth keeping
rather than replacing, because the false claim was ours and it was load-bearing:
it is why every agent also keeps a public receiving account, and why the payee
end of a settlement was visible on chain when the design says it should not be.

What it said was that `spel` resolves a `Private/<id>` account only for accounts
the **signing** wallet holds keys for, so naming another agent's private account
as the recipient fails before anything is built:

```
❌ Failed to submit privacy-preserving transaction: KeyNotFoundError
```

That reproduces exactly, and it is still what an unpatched `spel` does. What was
wrong was the diagnosis. The sentence that followed it — "a private account's
state cannot be constructed without its viewing key" — is a true statement about
`PrivateOwned` and a false one about paying somebody. Crediting a shielded
account does not need its *state*: it mints a new note, from
`Account::default()`, and the only inputs are public.

`KeyNotFoundError` is raised in one place,
`private_key_tree_acc_preparation` (`lez/wallet/src/account_manager.rs:565`),
whose first act is `wallet.storage.key_chain().private_account(account_id)`. It
is **a key lookup in the local wallet**, reached because `spel` built
`AccountIdentity::PrivateOwned` for every `Private/` argument it was handed. The
wallet has had the right variant all along:

```rust
PrivateForeign { npk: NullifierPublicKey, vpk: ViewingPublicKey, identifier: Identifier }
```

which takes no secret at all, and whose circuit output `PrivateForeignInit` has
been part of `/LEE/v0.3` since before this repository existed. It is how every
agent here was funded in the first place: `wallet auth-transfer send --to-keys`
is that variant, and `9Xpkkvos…` is a note it minted (block 8847).

So the limitation was in **this repository's copy of `spel`**, which it builds
itself, and closing it did not need an upstream release. `vendor/spel` now
accepts a shielded recipient named by keys:

```
--recipient PrivateKeys/<keys-file>          # the two-line file `wallet account show-keys` writes
--recipient PrivateKeys/<npk-hex>:<vpk-hex>  # or inline, straight out of an Agent Card
```

with an optional `#<identifier>`, defaulting to random exactly as
`wallet --to-identifier` does. `vendor/spel/spel-cli/src/foreign.rs` parses it,
`vendor/spel/spel-cli/src/tx.rs` builds `PrivateForeign` from it, and nothing
else about an account argument changed.

### The transaction

| | |
|---|---|
| settlement | `5942d6cd6d223fd5bc7b5abd3bf34a1c1fc8e540e508232411e60e4d53a03d61` |
| block | 9360 |
| program | `697746f5…cb5370bf` — the shipped one, `spend` |
| payer | messaging agent `GpRdooEW…Zpe5FS`, signing from its own wallet home |
| payee | storage agent, **at its shielded keys** — no account id was named |
| amount | 1 LEZ |
| note minted | `Private/Bs8N2TXEzXG1RX4jopjkNM8t3wB4JQcjX1Ud6ijRNbZb` |

No owner key was involved on either side, and no public account appears in the
payee position. Both ends of the settlement are now shielded.

### Reading it back

The amount is not taken from any local file. `tools/shielded-receipt` fetches
the transaction from the sequencer, decrypts the note the transaction itself
carries with the payee's viewing secret key, and then **recomputes the
commitment** from the decrypted account — it has to reproduce one of the 32-byte
commitments the transaction published, so the balance printed is the only
balance consistent with what the chain stored:

```
$ LEE_WALLET_HOME_DIR=~/.lp0008-agents/storage \
    tools/shielded-receipt/target/release/shielded-receipt \
    --payee 9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE \
    --tx 5942d6cd6d223fd5bc7b5abd3bf34a1c1fc8e540e508232411e60e4d53a03d61 \
    --expect-amount 1
  included in block 9360
    account id     Private/Bs8N2TXEzXG1RX4jopjkNM8t3wB4JQcjX1Ud6ijRNbZb
    balance        1
    commitment     matches one committed in this transaction
OK: a note of 1 is committed to these keys, on chain.
```

The same command against block 8847 decodes the storage agent's original funding
note — 10 LEZ, account id `9Xpkkvos…`, commitment matching — which is how the
decoder was checked before it was pointed at anything new.

And the payee's id is reproducible from the published card alone, without a
wallet or a key, because it is a hash of things the card carries:

```bash
KEYS=$(python3 -c "import json;k=json.load(open('artifacts/agent-cards/storage.json'))['x-logos']['shieldedPaymentKeys'];print(k['npk']+':'+k['vpk'])")
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
     --bin-auth-transfer artifacts/programs/authenticated_transfer.bin --dry-run \
  -- spend --agent Private/GpRdooEWJjX4JmRyT2n5KzMnDKtCM2HrvZ8iwMZpe5FS \
     --recipient "PrivateKeys/$KEYS#223479114267873733415204510793202889598" \
     --amount 1 --window-start 9000
#   recipient → 0xa16c3c09d4b69a48b917f327610e0f2807c5eb76a6acd36b270047619923b7f2
#   = Bs8N2TXEzXG1RX4jopjkNM8t3wB4JQcjX1Ud6ijRNbZb, the note the settlement minted
```

The identifier is the one that settlement drew, recorded in
[`shielded-settlement.tsv`](../artifacts/shielded-settlement.tsv) via the account
it produced. So the chain from *signed card* to *the account the money went to*
is closed: the card is signed by the key that owns the advertised payment
account, the keys in it derive that id, and that id is what the transaction's own
decrypted note carries.

### What is still true

- **The payee's account id changes with every payment.** A shielded transfer
  mints a new note under the payee's keys with a payer-chosen `identifier`, and
  the account id is `hash(npk, vpk, identifier)`. So a payee is not paid *into*
  the note it already holds; it ends up holding one more. The agent's identity
  in [`agents.tsv`](../artifacts/agents.tsv) — the account its claim and its
  policy are keyed by — is untouched by being paid.
- **`getAccount` still cannot see it** (next section). That is why the check
  above exists at all, and why it needs the payee's own key. A third party can
  confirm the transaction is in a block and which program it called; only the
  payee can confirm the amount. That is the privacy property, not a gap in the
  evidence.
- **The public receiving account is still there**, still initialised, still in
  the manifest's `pay_account` column and still what the four earlier
  settlements paid. It is now a choice rather than the only option.
- **`npk` and `vpk` are publishable, and are now published.** `npk` is a
  nullifier *public* key and `vpk` is an ML-KEM-768 encapsulation key: together
  they let anyone mint a note the agent can open, and neither lets anyone spend
  one. The Agent Card carries them under
  `x-logos.shieldedPaymentKeys`, inside the signed payload, so a payer reads
  them from the same signed document that tells it the price.

## `getAccount` cannot see a private balance, so only half a payment is public

`getAccount` reads the public state only
(`lee/state_machine/src/state/mod.rs:266-271`). A private account is a
commitment in the private state, and the RPC answers with a **default account**
for it — zero balance, zero nonce, default owner — which looks exactly like an
account that does not exist:

```
$ getAccount 9KdQSJ2tB9CGDWKZYFLEuZ28enPhzb2erPwTYVVXicNe
{"program_owner":[0,...,0],"balance":0,"data":[],"nonce":0}
$ wallet account get --account-id Private/9KdQSJ2t…
{"balance":100,"program_owner":"J8otq1J8Zpjhhpp6FPfhFtWKTCkLjthdk12cwHiMZCTB",…}
```

The consequence for evidence: the **credit** side of a settlement is checkable
by anyone with `getAccount`, and the **debit** side is not. The debit is still
constrained — `validate_execution` rule 8 requires total balance to be preserved
across every program in the transaction, so an accepted transaction that credits
25 has debited 25 from an account in the same call — but "the payer's balance
went down" is a statement only the payer's wallet can show directly.

## Two defects this file used to carry, and what replaced them

Both were real, both were fixed by the current deployment rather than reworded,
and both are recorded here because the fix is checkable and the claim was
previously false in our favour.

- **`spend` did not bind the policy to the account presenting it.** Any funded
  account could present any anchored policy, including one anchored for a
  different agent with a larger envelope, so per-agent limits were not
  per-agent. `spend` and `spend_approved` now derive the policy account's address
  from the *paying* account, so a policy anchored for somebody else is not a
  policy this agent has an address for. The comparison that fixed it first (6013)
  is retired, because there is no longer an `agent_id` argument to compare.
- **`spent_this_period` was supplied by the caller.** The per-period ceiling was
  compared against a number the agent passed in, both callers passed 0, and the
  enforced ceiling was therefore `min(per_tx, per_period)` *per transaction* and
  unbounded in aggregate. The running total now lives in the policy account's
  data, written only by the program that owns the account (LEZ rule 6,
  `UnauthorizedDataModification`). Read it off the chain:

  ```bash
  curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp"]}' \
  | python3 -c "
  import json,sys
  d=bytes(json.load(sys.stdin)['result']['data'])
  assert len(d)==97 and d[0]==1, 'not a record this program wrote: %d bytes' % len(d)
  le=lambda a,b: int.from_bytes(d[a:b],'little')
  print('period', le(73,81), 'spent', le(81,97))"
  ```

  That is the messaging agent's policy account under the live program. In period
  8000 it read 0 at anchoring (block 8876), 25 after `e691f593…` (block 8892) and
  50 after `aef14146…` (block 8901), against an anchored `per_period` of 250.
  **No current output is pasted here on purpose.** `spent` is per *window*, so it
  does not only grow — it starts again at zero every `period_blocks`, and a
  printed `period 8000 spent 50` left under this command reads as today's answer
  and stops being one the moment the window rolls.
  `./scripts/verify-deployment.sh` prints the live `window … spent …` for all
  three agents, which is the form that cannot go stale.

  The record is 97 bytes —
  `version(1) owner(32) per_tx(16) per_period(16) period_blocks(8) window_start(8) spent(16)`
  — so `window_start` is at `[73:81]` and `spent` at `[81:97]`. The `d[:8]` /
  `d[8:]` decoder that used to be printed here is the **superseded** program's
  24-byte layout. It still reads correctly against that program's accounts,
  which is what made it hard to notice: pointed at `BLHNchq8…` it prints
  `period 8000 spent 50` to this day. Pointed at the live account above it does
  not fail either — it prints a 178-digit `spent`, because `d[8:]` of the new
  layout is most of the owner id read as one integer. Checking the version byte
  is what turns that into an error instead of a number.

The period itself needed a mechanism rather than a counter, because **no program
on this chain can read the block height** — `ProgramInput` is the program id,
the caller, the pre-states and the instruction, and nothing else. So the caller
names its period and the guest makes the name binding: the period must start on
a multiple of `period_blocks` (6014), may not be older than the one the ledger
records (6015), and the transaction is pinned to
`[window_start, window_start + period_blocks)` through `ProgramOutput`'s block
validity window, which the state machine rejects outside of with
`OutOfValidityWindow`. Sliding the window forward, replaying an old one and
naming a future one are each refused by a different one of the three.

What this does **not** give is a wall-clock period, or one that survives a chain
with irregular block times. It is a block-height window, and that is all it
claims to be.

## No model has been run against the inference port

The prize requires "pluggable inference (local or API-based)" and puts the model
itself out of scope. The port exists (`module/src/inference.h`) and both
backends are tested, but the word *inference* is doing no work in either of
them yet:

- `StubLocalBackend` is **a stub, and not a model**. No weights, no tokenizer,
  no inference: it reads two fields out of a JSON context and compares them. It
  is a rule table with an honest name, and it is the "local" half only in the
  sense that it satisfies the port offline.
- `OpenAiCompatibleBackend` builds a real chat-completions request and parses a
  real reply, but its transport is injected and the only transport it has ever
  been given is a fake. **No request has left this repository**, to a hosted
  endpoint or to a local runtime.

So what is demonstrated is the seam and its failure behaviour — that a backend
which is unreachable, slow, incoherent, or actively obeying an injected offer
cannot move money — and not that a model can usefully drive the agent. The
second claim needs a model, and no model has been run.

One further gap worth naming: the decision is not in the demo path.
`scripts/a2a-task.sh` still decides whether to accept a task price with a shell
`if`, and that `if` is what runs on testnet today. `decideTaskAcceptance` is
exercised by CI, not by the demo.

## `create_policy` needs a signer some program already owns

Reproduced against a local sequencer with `RISC0_DEV_MODE=0`, so this is the
sequencer's own log rather than an inference:

```
Transaction with hash 17d50d31… failed execution check with error:
  InvalidProgramBehavior(DeclaredAccountMissingFromOutput {
      account_id: NtWrSMxC4xDk6WSYtb6XA4ZqPXHaMzmohVHFH7CVKFX })
  , skipping it
```

The chain of events is not the obvious one. LEZ rule 7
(`lee/state_machine/core/src/program/mod.rs:730-738`) rejects a post-state that
keeps the default program owner when the pre-state is no longer
`Account::default()` — and a signer's nonce goes 0 → 1 on its first transaction.
But the sequencer never emits that error, because the vendored SPEL macro
**filters the signer out of the output first**
(`vendor/spel/spel-framework-macros/src/lib.rs:304-328`), precisely to dodge
rule 7. `create_policy` still *declares* `owner`, so the state machine rejects
it for being declared and missing instead
(`validated_state_diff/mod.rs:311-319`).

**The precondition is narrower than it looks.** This only bites a signer that
still has the **default** program owner. An account already owned by another
program is exempt from both halves — it is never filtered, and rule 7 only fires
on a default post-state owner. `DumJ4LCB…`, claimed and therefore owned by the
authenticated transfer program, reads **nonce 33** on the public testnet, and a
locally claimed signer anchored three in a row. (This used to cite two anchors by
that account at blocks 8050 and 8051. Those hashes are recorded nowhere in this
repository and could not be re-derived, so the count is now taken off the account
itself, which is the stronger statement anyway.)

`owner` is **not** removed, though removing it would also clear the rejection.
`spel` signs only for accounts an instruction declares as signers, so an
instruction with no signer produces a transaction with an empty witness set —
and `create_policy` would become permissionless. It is the account the program
records as the owner and the account the claim is compared against (6020), so
removing it would delete the binding that makes anchoring authenticated at all.
So the requirement stands and is documented instead: **anchor with a signer that
has already been claimed**, or accept one anchor per signer. A transfer is one
way to claim an account and not the cheapest — `wallet auth-transfer init`
claims it for nothing, and the worked owner below signs program transactions
holding a balance of 0. Whichever way it is done, it has to happen while the
account is still untouched; see
[the closed section above](#closed-an-owner-can-approve-a-spend-and-the-approved-spend-executes)
for why there is no second chance.

The same rule is why `create_policy` cannot simply declare the agent as a second
signer and be done in one transaction. `spel` signs only with keys the single
wallet home it is pointed at holds, so a two-signer instruction would require the
owner's key and the agent's key in the same wallet — the arrangement the whole
design exists to avoid. Two single-signer instructions from two homes is the
shape that both authenticates and deploys.

Also falsified while chasing this: stale local wallet state is not a factor.
`spel` refetches from the chain; an anchor landed with an older wallet snapshot
restored underneath it.

## deploy-agents.sh is not idempotent, and looks broken when it is right

Agent identities are stable once funded — `fund_agent` reuses the account that
already holds the balance rather than buying a new one. So a second run resolves
the *same* two addresses, and both `claim_agent` and `create_policy` refuse,
because both are declared `#[account(init, …)]` and `init` will not overwrite.
That is the single-use guarantee the design depends on, working exactly as
intended.

The script records what it did in `artifacts/anchored.tsv`, keyed by
`(program, instruction, agent)`, and reports a refused repeat as already-done
rather than as a failure. The key has to include the program, because both
accounts are PDAs of it and redeploying moves them to addresses that have never
been initialised; and it has to include the instruction, because there are now
two single-use steps per agent and a run can legitimately resume between them —
which is what happened here when the storage agent's claim landed in a separate
session from its anchor.

The manifest is written to a temporary file and moved into place only when all
three agents are done, so a failed run leaves the previous evidence intact
rather than truncating it to a header.

## Superseded programs are on the testnet

Deployment is content-addressed, so every version this repository has built is
still reachable by its own hash. Only the last one is referenced by anything:

| Deploy tx | What it was |
|---|---|
| `49f75568…b5f58b03` | returned two accounts for a three-account instruction; violates the SPEL invariant |
| `1ea86256…f18b6f3c` | `spend` without a recipient account — authorised payments, moved nothing |
| `6e4a2000…f365321a` | `spend` with a recipient, debiting the agent directly — no settlement was ever built under it, and rule 5 would have refused one |
| `b028eabf…b8c18549` | chains the transfer into the program that owns the accounts, but anchoring was unauthenticated and the period ceiling counted nothing — it accepted the attack transaction `c0b21ba6…` |
| `8c87cc9b…2d20ebbe` | identity bindings on anchoring and paying, and the period total on chain; an agent's own key could still anchor itself an unlimited ceiling, and did — `e530e0ba…`, then `7fc6c9af…` moved 65 against a ceiling of 25 |
| `a780003b…8576841e` | one policy account per agent, so the agent's own key could no longer anchor a second — but the agent was not a party to anchoring at all, so **anybody** could anchor the first. It accepted `eedb3caf…` at block 8869, from a stranger, `per_tx = 2^128-1` |
| `697746f5…cb5370bf` | current: anchoring takes the agent's signature and the designated owner's, and `update_policy` makes a wrong anchor recoverable |

`artifacts/programs/agent_verifier.bin` hashes to the last row. Recompute rather
than trust it:

```bash
python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())"
# 697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf
```

Everything the superseded rows point at is still on chain and still true of the
program it names. `scripts/demo.sh` reads two of those transactions back on
every run, because the evidence that a defect was real is a transaction that was
accepted, not a paragraph saying it would have been.

## An assertion that was right most of the time

A defect worth a section of its own, because it is the hardest kind to catch and
because this file is otherwise a list of its relatives.

`module/tests/plugin_delivery_test.cpp` peer mode asserted that the first
`messaging.receive` on an agent's own task topic comes back empty — "nothing has
been sent yet". It reads as a statement about the skill. It was a statement about
timing, and on 2026-08-16 it lost.

Nothing was broken. The two peers poll the discovery loop on a three-second
cadence and leave it up to one poll apart; whichever leaves second opened its
inbox *after* the other had already published its A2A request, and found it
waiting. The node's own log interleaves with the checks and says so — the frame
arrives on that exact content topic on the line above the read that was supposed
to find nothing:

```
DBG 01:13:26.376 received relay message  … contentTopic=/lp-0008/1/task-<buyer>-<task>/json
  ok    messaging.receive answers, and subscribes on first use
  FAIL  and its first answer is empty, because nothing has been sent yet
```

**Two things this cost, and both are the point.** It fails rarely, so it carried
the authority of a green check while proving nothing about the ordering it
depended on — and a check that is usually right is harder to distrust than one
that is usually wrong. And it sat four lines in front of `agent.task`, which in
`settle` mode spends real LEZ on a testnet with no faucet. A flaky assertion in
front of a step that costs money is worse than no assertion at all: it turns a
successful run red *after* the money is gone, and a reviewer reading the
transcript cannot tell that failure from a real one.

**The fix was ordering, not softening the claim.** The inbox is opened before the
discovery loop rather than after it, where neither agent has minted a task and
the answer is empty for a reason instead of by luck. What the assertion asserts
is unchanged.

Two facts behind it that generalise past this harness:

- **A Waku node buffers what its relay shard carries, so "subscribe" is not what
  gates receipt.** The first read is what subscribes, but the frames were already
  arriving — `no subscribed peers found` in the node log is about *filter*
  subscriptions and does not stop relay delivery. So any "the topic was empty
  before X" assertion is about when you looked, whether or not it was written
  that way.
- **In a two-process harness, ask of every assertion whether it would still hold
  if the other process got there first.** The ones that would not are not
  assertions about the system; they are assertions about the scheduler.

**The rule was applied again the same day, to the step that closed the
lifecycle.** That step has each agent publish a forged `completed` for its *own*
task onto the topic it is about to read, and assert that the poll counted it and
refused it — the frame a single process would use to satisfy every other
assertion in the step by itself. Written the obvious way, the poll loop exits as
soon as the task is `completed`, and the decoy is only counted if it happens to
come back off the relay before the peer's terminal update does. It did, both
times it was run. It would not always, and the failure would have arrived on an
afternoon when nothing was wrong.

So the loop exits on **both** facts — the peer's terminal update applied, and at
least one self-authored frame refused — and neither is a statement about which
process got somewhere first: one is about a frame the peer published, the other
about a frame this node published to itself. A bounded loop that never sees
either fails, which is the honest outcome, because if the decoy never arrived
then "it was refused" was never true.

### And then the same step cost a settlement anyway, in a mode nobody had run

The section above is about an assertion that was *sometimes* wrong. This is
about one that was *always* wrong in a mode that had never been executed, which
is worse, and it is the reason settlement 9 exists.

`./scripts/delivery-in-plugin.sh peers` and `… settle` run the same harness. The
difference is a price and a signer, and one consequence nobody had thought
through: in `settle` the buyer blocks inside `agent.task` for as long as the
proof takes — 434 s in this run — and the step opens its update topic *after*
that call returns. In `peers` that opening poll runs before the peer can have
answered, so it applies nothing and every update lands in the loop that follows.
In `settle` it runs seven minutes later, the peer's `working` and `completed` are
both already on the topic, and the opening poll applies them both. The loop then
counted its own iterations, found none, and printed:

```
ok    THIS agent's own TaskStore reached `completed`, and every transition into
      it came off the wire
FAIL  applying 0 status update(s) the peer published
ok    all of them published by A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu, the
      OTHER account — none by this one
```

Read those three lines together. The task really did walk
`submitted → working → completed` on the peer's frames; the assertion was
measuring **where** they were applied rather than **whether** they were. And the
line under the failure is worse than the failure: `!authors.contains(me) &&
authors.count(other) == authors.size()` is true of an EMPTY list, so it reported
that all applied updates came from the other account, about zero of them. A
fresh instance of the exact defect this repository had spent the day removing,
added by the person removing it.

**Two things this cost, and one that it did not.** It cost a settlement: 1 LEZ,
on a testnet with no faucet, for a run that proved what it was meant to prove and
reported failure. It cost the run's transcript, which now reads as a broken
lifecycle to anyone who does not read the history line. What it did *not* cost is
the second settlement — because the fix was verified without one.

**The stub that should have existed first.** A `pay_signer` is a command that
reads stdin and prints 64 hex characters. So a command that reads stdin, sleeps
120 seconds and prints 64 hex characters reproduces the entire asymmetry — one
module blocked in `agent.task`, its peer publishing into a topic nobody is
reading yet — with no chain, no proof and no money. Against the pre-fix binary
the buyer exits 1 with `applying 0`; against the fixed one both processes exit 0
with `applying 2` and `all 2 of them published by …`. That is a five-minute test
of the mode that costs 1 LEZ to run for real, and it did not exist until after
the LEZ was spent.

The general form, for the next person: **a harness with two modes has two
schedules, and an assertion is only as tested as the slowest one.** Anything that
blocks — a proof, a confirmation, an owner wait — moves every later step past
events that used to follow it, and the assertions written against the fast mode
keep their shape while losing their meaning.

## The Delivery node runs are local, not CI

Building `liblogosdelivery` takes tens of minutes — there is no prebuilt Linux
library anywhere public to download — and the run needs live peers, so
`scripts/exercise-nodes.sh` is a local command. The reasoning is in
[`docs/skills.md`](skills.md).

**The Storage half of that sentence was wrong and this heading used to carry
it.** `logos-storage-nim` publishes a full release asset matrix, so the
`storage-node` job in `.github/workflows/ci.yml` downloads a checksum-pinned
`libstorage`, builds `module/tests/storage_node_drive.c` against it, and drives a
real node on the runner with two negative controls under it. The libraries are
not alike in what upstream ships, and treating them as one is what produced a
false "not in CI" for both.
`scripts/owner-channel-live.sh` is local for the same reason and one more: it
needs *two* nodes to find each other through public relays, so it depends on the
health of a network this repository does not run. A required job that goes amber
on a bad afternoon teaches everyone to ignore it.

`./scripts/delivery-in-plugin.sh settle` is local for a third reason on top of
both of those: it moves real LEZ on a testnet whose faucet is gone, and a job
that spends money on every push would empty the agents inside a day. The half
of it that can be checked freely is split out — `delivery-in-plugin.sh signers`
needs no key, no second agent and no chain, and it is the one to run when either
delegate changes. It is still not in CI, because it starts a Delivery node and
so inherits the first reason.

So neither of them runs on a push, and the honest statement of what that costs
is this: a change that stopped the module refusing correctly would be caught by
nobody until somebody ran the harness. What CI does hold is narrower and worth
naming exactly — `scripts/check-package-fresh.py` refuses to accept the package
unless every source string literal of eight bytes or more is present in the
shipped binary, so the refusal *messages* those harnesses assert on cannot drift
away from the binary a reviewer downloads. That is a check on the words, not on
the behaviour.

## The owner in the live owner-channel run is a node — and, separately, a second Basecamp

`scripts/owner-channel-live.sh` answers the transport half of "the owner can
interact with the agent in real time from a separate Logos app instance using
Logos Messaging, with no intermediary server": two processes, two Delivery
nodes, a correlated approval round trip on the owner content topic in 312 ms on
the first attempt, and three watched failures showing the pass is not free.

It does not answer the *app instance* half, and the distance is worth stating
because it is easy to read the run as more than it is. The owner side is a
process this repository starts and links against `liblogosdelivery` — not Logos
Basecamp with a person in front of it.

The second half of that paragraph used to read: "they cannot yet: the class that
speaks Delivery, `OwnerChannel`, needs a `DeliveryPort`, which is a
`std::function` and cannot cross the plugin boundary into a Basecamp-loaded
module." **That was wrong, and it was wrong in a way worth keeping on the page.**
The premise is true — a host cannot PASS a `std::function` over Qt Remote
Objects — and the conclusion does not follow from it, because the module does
not need to be handed a port it can build. It now links `liblogosdelivery`,
opens a node from its own configuration on `meta.configure("delivery","on")`,
and constructs its own `DeliveryPort` and `OwnerChannelPort` on the far side of
the boundary; two strings cross, and no closure does. Measured through
`QPluginLoader`, through the real runtime out of the installed Basecamp, and
between two loaded modules on the public network —
[`scripts/delivery-in-plugin.sh`](../scripts/delivery-in-plugin.sh),
[`docs/basecamp.md`](basecamp.md).

What is still true is narrower, and is now one thing rather than two. The agent
end being a loaded module **has been watched**: `./scripts/delivery-in-plugin.sh
approval` puts an above-threshold `wallet.send` through a plugin's own
`OwnerChannel`, over the public network, to `module/tests/owner_responder.cpp`
on a second node — which re-derives the approval marker from the request's own
terms and refuses to answer one it cannot verify — and the module acts on the
answer in about 470 ms, approving on one run and denying on the control.

~~What remains is the *app instance*: the owner end is a program written for the
purpose, not Basecamp with a person in front of it.~~ **Closed, and by two
Basecamps rather than by an argument.** Two LogosBasecamp 0.2.2 processes, each
with its own `LOGOS_USER_DIR`, its own working directory, its own loaded copy of
the module in its own `logos_host`, and its own Delivery node: a spend minted in
one appeared in the other's window 573 ms later, was denied from there in 371 ms
and approved in 177 ms, and with the owner's app killed the same call ended
`owner_unreachable` with nothing submitted. The record, with the environment and
the transcripts read out of the two windows, is
[`docs/basecamp.md`](basecamp.md) §"Two Basecamps, and the owner in the second
one".

What that took was not a new transport but a way in: `invoke()` is the only
thing a `ui` plugin can call across the plugin boundary, so the owner's end of
the channel is now three skills — `owner.watch`, `owner.pending`,
`owner.answer` — rather than a class only a host that *links* the module could
construct.

**Two things that run measured differently from expectation, and both are the
kind that read as a broken network.**

- **A reliable channel opened mid-stream does not receive the backlog.** Run
  with the agent asking first and the owner's app watching afterwards, the
  owner's node *receives the frames* — its log carries them on the right content
  topic — and its module reports `{"relay_seen":2,"channel_seen":0}`: the bytes
  were in the process and the channel never delivered them. Open the owner's end
  first and every frame arrives. The runner scripts start the owner two seconds
  ahead for this reason, and a real deployment has the owner's app up anyway.
- **This transport hands a node its own relay messages and NOT its own
  reliable-channel frames.** `agent.discover` reads its own card back off the
  relay — this repository has assertions built on that — while the owner's
  `owner.pending` reports `self_refused: 0` with `frames: 1` in every live run.
  So the rule that refuses a self-authored *request* cannot be exercised against
  the network; it is exercised in `module/tests/owner_skills_test.cpp`, where a
  self-authored frame can be put in front of it, with the falsification beside
  it — the same bytes authored by the other account, which must be accepted.

**And what the second app still does not do: hold a key.** It is bound to the
owner's *account* and it does not sign the reply. That is what this channel is —
sender ids on it are self-declared — and the authority over an above-threshold
spend is the approval account on chain. The prize's Architecture wording is "any
Logos app instance that holds the owner's keys"; reach, correlation and real
time are demonstrated, key custody is not.

Two smaller things the run does not claim. Both processes were on one machine,
so the frames went out to public relays (DigitalOcean, Google Cloud, and others
named in the node logs) and may also have found a shorter path back; nothing
here proves a particular hop count, only that no server this repository operates
was in the middle, because there is none. And `verdict: approved` unlocks the
`spend_approved` path — it does not move money. Whether a payment happened is
the chain's answer, and the reason no owner on this testnet can give it is three
sections above.

## The owner channel inside Basecamp: two facts that had to be measured

The owner console (`app/`) completes a real approval round trip inside Basecamp
0.2.2 — `app/README.md` has the transcript. Getting there turned up two
properties of a *loaded* Logos Core module that no amount of reading the source
would have shown, and both are worth writing down because anything else built on
this transport will meet them.

**1. A module that blocks on a call cannot publish while it blocks, and the fix
is in the module.**

`wallet.send` above the envelope waits for the owner, polling for a verdict and
re-publishing `ownerApprovalRequested` on an interval. A core module runs in its
own `logos_host`, and the transport — both the events it emits and the calls it
receives — is dispatched by that process's Qt event loop. The wait ran *on* that
loop and slept between polls, so everything it published queued behind it.

Measured, before the fix, with the console open and a 30 s approval timeout: the
`wallet.send` call reached the module at T; six `ownerApprovalRequested` events
were delivered to the app within four milliseconds of each other at **T+30.0
s**; and a bare `status()` clicked at T+3 s also arrived at T+30.0 s. T+30 s is
when the wait gave up. The owner was told about the spend after the agent had
stopped waiting for an answer.

The fix is `AgentModuleImpl::setOwnerIdle`, installed by the module's Logos Core
export with a lambda that pumps the host's event loop instead of sleeping. It
is guarded on there *being* an event loop and on running on its thread, so a
unit test that constructs the impl directly still sleeps exactly as before. The
re-entrancy it buys was checked before it was relied on: `invoke()` drops the
mutex before calling a skill and `approveSpend()` takes it only to record a
verdict, so an approval dispatched inside the wait completes against the map the
wait is polling. A version that held the mutex across the skill call would
deadlock rather than time out.

After it: attempt 1 of the same request reaches the owner in **7 ms**.

**2. The verdict cannot come back on the connection that asked for it, and the
fix is in the UI.**

Pumping the loop was not enough. Qt disables a socket's read notifier for the
duration of the handler it is running, and `invoke("wallet.send")` *is* that
handler — so a verdict sent down the host's own connection is not read until the
call it answers has already returned. Measured: `approveSpend` clicked at T+23 s
of a 60 s wait reached the module at **T+60.0 s**, while the outgoing events
streamed live throughout.

The console therefore opens a **second** `LogosAPI` at connect time and answers
on that. The module's registry hands it a second socket with a notifier of its
own, which is not inside a handler and is still live while the first is blocked.
Both share the process's `TokenManager`, so the capability token Basecamp
obtained for the plugin covers it. With that, the verdict clicked at T+7.2 s
arrived at T+7.2 s and the module acted on it.

Neither is a general cure. A module method that blocks is still a module that
answers nothing else on that connection for the duration, and the second
connection is a workaround inside one process rather than a property of the
transport. The shape that would not need either is an `invoke` that returns
immediately with a pending-approval handle and reports the outcome as another
event — a change to the skill's contract, which `module/tests/`,
`docs/a2a-binding.md` and the A2A task lifecycle all describe in its current
form. Cost: that redesign, plus re-deriving the four assertions in harness 2
that are written against a synchronous `wallet.send`.

## Both packages are installed by hand, and a reviewer cannot avoid it

Basecamp 0.2.2's Package Manager installs from a configured package repository
only. There is no "install from file", so neither `module/agent.lgx` nor
`app/agent-ui.lgx` can be installed by clicking, and the procedures in
`docs/basecamp.md` and `app/README.md` unpack them into the user modules and
user plugins directories by hand. This is a property of the host, not of the
packages — both are real `lgx` packages, `lgx verify` passes on both, and
Basecamp installs each into the right directory when told to, by `type`.

Cost to close: publishing both to a package repository Basecamp can be pointed
at. Nothing in either package changes.

## CLOSED: both packages now carry every variant the Logos app is published for

This section used to say that `app/agent-ui.lgx` and `module/agent.lgx` each
carried a single variant — `darwin-arm64` — and that "a reviewer on Linux cannot
open either", and that closing it was "one container build per package". That
estimate was right about
the shape and wrong about the cost, and the difference is worth recording
because three separate things had to be found first.

Both packages now carry all three:

```
$ lgx manifest module/agent.lgx
Variants:       darwin-arm64, linux-amd64, linux-arm64
                darwin-arm64 -> agent_plugin.dylib
                linux-amd64 -> agent_plugin.so
                linux-arm64 -> agent_plugin.so
```

and `./scripts/logos-core-headless.sh storage` on **both** Linux architectures
loads, configures and starts the module out of that package with **all steps
confirmed (0 failure(s))** — the same 40 assertions the macOS run makes,
differing only where they name the variant. The full procedure is in
[`basecamp.md`](basecamp.md) under "Linux, and the Linux variants"; the three
findings, in the order they cost time:

1. **The runtime did not have to be installed.** See the next section — this is
   the one that also emptied the `[UPSTREAM]` half of the deployment criterion.
2. **Official Qt for Linux needs its `icu` archive.** It links ICU 73 by soname,
   no distribution ships that, and the failure surfaces inside the
   `cpp-generator` sub-build as `undefined reference to ucnv_open_73`.
3. **The plugin was missing a symbol on both platforms and only ELF said so.**
   `logos-module-builder` at `5396513` does not compile `logos_types.cpp`, so
   `operator<<(QDataStream&, const LogosResult&)` was undefined in every build
   this repository has ever shipped. Mach-O binds lazily and never faulted; ELF
   loads the module, joins it to the runtime, hands out a client, and then dies
   on the first call across the transport with `symbol lookup error`, looking
   exactly like the Qt-mismatch timeout. `module/CMakeLists.txt` compiles it in
   now, on both platforms, which is why the `darwin-arm64` binary changed in the
   same commit.

**What is still not covered — nothing, on this axis.** Basecamp 0.2.2 publishes
three artefacts: a macOS arm64 `.dmg` and Linux `x86_64` and `aarch64`
AppImages. All three are covered and all three were run. `linux-arm64` was the
last one open and it cost exactly what this section had estimated — one
container build per package — once the x86-64 route had found the three defects
above; on an Apple Silicon host a `linux/arm64` container is native, so it is
also the cheapest of the three to reproduce. What is left of "on any machine" is
not a platform.

**And one capability the Linux variants do not have.**
`logos-messaging/logos-delivery` publishes no `liblogosdelivery.so` and never
has — re-checked against the release API while this was written; the current
`nightly` carries `nwaku-amd64-linux-nightly.tar.gz`, which holds executables
and no library. Both Linux plugins therefore carry the delivery code
path (every one of its literals is in each binary, which
`scripts/check-package-fresh.py` asserts against all three variants) and no
library beside them, so `meta.configure("delivery","on")` there will report the file it
could not open rather than starting a node. That was **not** exercised on Linux
— what was exercised is everything in the load/configure/start path, which is
what the deployment criterion names. Closing it needs an upstream release asset
or a Nim build of the library, exactly as
[`module/third-party/liblogosdelivery/README.md`](../module/third-party/liblogosdelivery/README.md)
sets out.

## CLOSED: the deployment command no longer needs a build toolchain

This section used to be the last open item under "on any machine", and it was
one sentence long: **`logos-core-headless.sh` compiles a harness**, so running
it needed a C++17 compiler, Qt 6.9.2 with `qtremoteobjects`, a `logos-cpp-sdk`
checkout and `nlohmann/json` — on every platform. It compiles one because
`liblogos_core`'s C API can *load* a module and cannot *call a method* on one,
and the SDK is what speaks the runtime's transport. A machine that could run
Logos Core still could not run this, and that is not "any machine".

The harness is built once per variant now and committed, exactly as the plugin
is — `module/harness/{darwin-arm64,linux-amd64,linux-arm64}/logos_core_load_test`,
1.4 MB each — with `module/harness/harness.sources` recording what each was
built from.

**The measurement, and it is the whole point: a machine with no toolchain.**

```
$ ./scripts/harness-no-toolchain.sh linux-arm64
image     ubuntu:24.04 (linux/arm64), stock, nothing added but python3
  ok    no cc … no c++ … no gcc … no make … no cmake … no moc … no qmake
  ok    no Qt SDK, no nlohmann/json, no logos-cpp-sdk checkout
  ok    installed: python3
harness   /work/module/harness/linux-arm64/logos_core_load_test
          shipped, sha256 aa9c9ca53f732f39, as recorded in module/harness/harness.sources
          no compiler, no Qt SDK and no logos-cpp-sdk checkout were needed
  <-    ... 40 assertions
all steps confirmed (0 failure(s))
```

Both Linux variants, in a stock `ubuntu:24.04` container: 40 assertions, zero
failures, out of the committed package and the committed harness. The script
**refuses to report anything at all** if it finds a compiler on the machine it
is checking, and it asserts the two controls that make the green mean something
— the same command asked to build the harness there is refused, by name, for
all three things it lacks; and a harness whose bytes are not the recorded ones
is not run.

On macOS there is no toolchain-free container to run in, because the runtime
lives inside an installed `.app`. The equivalent measurement is: every compiler
on `PATH` replaced by a shim that exits non-zero and prints `COMPILER INVOKED`,
with `QT_ROOT`, `LOGOS_CPP_SDK_ROOT` and `EXTRA_INCLUDE_DIR` pointed at paths
that do not exist. `./scripts/logos-core-headless.sh storage` still reports
**all steps confirmed (0 failure(s))**, 40 assertions, and the shim is never
invoked. The shim is not decoration: with the same `PATH` and
`HARNESS_FROM_SOURCE=1`, the run stops at `COMPILER INVOKED: c++ -std=c++17 …`.

**What it took, and what is worth knowing before doing it again.**

- **The binary must carry no rpath.** Both platforms' harnesses used to link one
  to the Qt they were compiled against, which is a directory on the machine that
  compiled them. They now carry none, and find Qt through the environment the
  run step sets: `LD_LIBRARY_PATH=$APP/usr/lib` on Linux,
  `DYLD_FRAMEWORK_PATH=$APP/Contents/Frameworks` on macOS. That is the app's
  **own** Qt 6.9.2 — the same libraries `logos_host` loads the plugin under, so
  the harness cannot be running a different Qt from the runtime it is driving.
  It is load-bearing rather than belt-and-braces: run the macOS binary without
  the variable and dyld says `Library not loaded: @rpath/QtCore.framework/…
  Reason: no LC_RPATH's found`.
- **The dependency floor has to be measured, not assumed.** A shipped binary
  that needs a newer glibc than the runtime would make "any machine that can run
  Logos Core" false, silently, on somebody else's distribution. Read out of the
  binaries themselves: the harness needs at most `GLIBC_2.38`, `GLIBCXX_3.4.29`,
  `CXXABI_1.3.9`; `liblogos_core.so` needs `GLIBC_2.38`, `GLIBCXX_3.4.29`,
  `CXXABI_1.3.15`, and the app's Qt needs `GLIBCXX_3.4.32`. Below the runtime on
  every axis, and `scripts/check-package-fresh.py` goes red if that stops being
  true.
- **The build directory had to become per-variant.** The three variants are
  built out of one checkout shared into three containers. With one build
  directory the Mach-O `.o` files from the macOS build are newer than the SDK
  sources the Linux build reads, so the Linux build skips compiling them and
  hands Mach-O objects to `ld` — and a macOS harness is "up to date" for a Linux
  run, which then dies of `Exec format error` in the middle of a deployment.

**What this does NOT close, stated plainly.**

- **You are running a binary somebody else built.** That is a real cost and the
  answer to it is not "trust the record": `scripts/check-package-fresh.py`
  recomputes the record, requires every string literal of
  `module/tests/logos_core_load_test.cpp` to be inside each binary, requires each
  to be the architecture its variant claims, to carry no rpath, to link Qt (and,
  on ELF, `liblogos_core`), and to need no newer glibc than the runtime. All of
  that runs with nothing but `python3`, on the machine that is about to trust the
  binary — the toolchain-free run above does exactly that as its last step. What
  none of it proves is that the bytes are what *your* compiler would produce
  from that source: the harness is not byte-reproducible across toolchains any
  more than the plugin is. `HARNESS_FROM_SOURCE=1` builds your own instead, and
  `--build-harness` replaces the shipped one; both are one command and both need
  the four prerequisites this section removed from the default path.
- **A harness for a fourth platform would have to be built on that platform.**
  There are three because Logos Core is published for three. `check-package-fresh.py`
  fails if the package ever ships a plugin variant with no harness beside it, so
  the hole cannot open quietly.
- **The macOS binary is ad-hoc signed, by the linker, as every locally built
  Mach-O is.** A `git clone` carries it without quarantine and it runs. A
  download of a zip or tarball through a browser gets `com.apple.quarantine`,
  and Gatekeeper refuses a non-notarized executable until that attribute is
  removed (`xattr -d com.apple.quarantine`) — which is a property of how the
  file arrived, not of the file, and applies to every unsigned binary anyone
  publishes. Not measured here: this repository is cloned, not downloaded as a
  zip, in every procedure it documents.

## The Delivery library pinned for CI does not build, and ours came from elsewhere

`scripts/build-companion-modules.sh` pins `logos-delivery` at `f8b0365`, which
is what `logos-delivery-module`'s own `flake.lock` names — the right revision to
pin, on the face of it, because it is the one the module that consumes the
library was built against.

**It does not compile.** The `alongside` workflow, dispatched for the first time
on 2026-08-17, fetched that revision on a clean runner and Nim refused upstream's
own code:

```
logos_delivery/waku/rest_api/endpoint/relay/handlers.nim(233, 15)
  Error: waitFor withTimeout(publish(node, …), futTimeout)
         has an illegal effect: NestedPoll
```

That is an effect-tracking rejection inside `logos-delivery`, not a mistake in
anything here, and the `[OSError]` the nimble wrapper prints underneath it names
the link line rather than the compile that failed — which is why the first two
readings of this failure blamed a missing `librln_v2.0.2.a` archive.

**And the library this repository has actually been using was built from a
different revision.** `_external/logos-delivery` is checked out at `0d433ea`,
built on 15 August; `f8b0365` is not even an ancestor of it. So every local run
that links `liblogosdelivery` — including the `--alongside` evidence for "loads
alongside the wallet, storage and messaging modules" — used a library from a
revision the recipe does not pin, and nothing noticed, because the build script
skips the build entirely when the artefact is already there.

What is true, stated exactly:

- the four-module load itself is real and was run; it is in the transcript, and
  each companion answered across the runtime's transport;
- the Delivery library it linked was built here, from `0d433ea`, and is not
  reproducible by following this repository's own instructions;
- the revision those instructions name does not currently build from clean.

Neither pinning `0d433ea` nor keeping `f8b0365` is obviously right. The first
makes the recipe reproduce what was measured and diverges from the consuming
module's lock; the second keeps the lock and reproduces nothing. Resolving it
means either finding the dependency set under which `f8b0365` compiles — the
`taskpools` pin a few lines above it in the build script is the precedent — or
establishing that `0d433ea` is the revision the module is compatible with and
saying why. Until then this is written down rather than papered over, and the
`alongside` workflow is red for a real reason.

## One command, and what it needs before it will run

The prize asks that "the owner can deploy the agent and configure it with a
single CLI command on any machine using Logos Core headless". That sentence has
two halves, and this section used to be titled **"Two commands, not one"**
because this repository answered them with two — they deploy to two different
places:

```sh
SIGNER=<funded public account id> ./scripts/deploy-agents.sh   # on chain
./scripts/logos-core-headless.sh storage                       # in Logos Core
```

The first creates each agent's shielded identity, funds it, opens its receiving
account and anchors its spending envelope on LEZ. The second installs the
packaged module into Logos Core and runs the runtime **headless** — no GUI, no
window, no display — loading the module and calling `configure()` and `start()`
on it across the runtime's own transport, with the owner and policy account the
first command anchored.

**Both are now one command**, and the two above are still there because it is
composed out of them rather than instead of them:

```sh
SIGNER=<funded public account id> ./scripts/deploy-and-configure.sh storage
```

### What the count turned on, and why the old answer was wrong

This section used to argue that the wrapper should not be written:

> **A wrapper around both is writable and is not written.** It would be honest
> only on a machine that has both a funded LEZ wallet and a Logos Core runtime,
> and it would report one exit code for two unrelated failures — which hides the
> gap rather than closing it.

Both clauses are worth keeping, because the first is still true and the second
was the mistake.

The first is a statement of prerequisites, and prerequisites do not stop a
command being one command — every command in this document has them. It is
answered by listing them and checking them, which is what the rest of this
section already did for each half separately.

The second is the part that was wrong, and it was wrong about the specific
thing it named. **The two failures are not unrelated.** The second half's
prerequisites are knowable before the first half spends anything, and the first
half's output is the second half's input. Those are the two facts that make a
composition either safe or dangerous, and the wrapper is where they are acted
on:

- **Nothing is submitted until both halves say they can run.** The Logos Core
  half's own gate is asked first — `logos-core-headless.sh --check`, which runs
  that script's prerequisite list, its harness-against-record check and its
  package-variant check, and then stops one line short of starting a runtime.
  It is asked by *running that script*, not by keeping a second copy of its
  ninety-line platform table here, so the two cannot drift. This matters because
  anchoring is not idempotent and is not free: `claim_agent` and `create_policy`
  are both `#[account(init)]`, a landed claim cannot be rewritten, and a funding
  transfer that has left cannot be recalled. A machine that finds out it has no
  `liblogos_core` *after* three agents are anchored has paid real transactions
  for the discovery.
- **The second half is handed the file the first half wrote.** No owner and no
  policy account is passed on a command line. `deploy-agents.sh` records the
  agent id, the owner and the policy account in `artifacts/agents.tsv`;
  `logos-core-headless.sh` reads those three back **by header name** out of the
  same file; the wrapper exports one `MANIFEST` path so that both are certainly
  reading and writing the same file, and checks the row in between.
- **And the exit codes are three, not one.** `0` both halves ran. `1` a
  prerequisite was missing or the chain half failed — and in that case nothing
  was configured, which is the safe direction. `2` **the chain half succeeded
  and the Logos Core half did not**: agents are anchored, paid for, and not
  configured. That is the only outcome that costs money, it gets a code of its
  own, and the message names the free command that finishes the job
  (`--configure-only`, which submits nothing).

### The seam, which is the whole of the design

`artifacts/agents.tsv` is **committed in this repository** — it is the evidence
for the three agents already published. That is exactly what makes the handoff
dangerous rather than trivial: a chain half that failed leaves that committed
file in place, and a wrapper that simply carried on would install the module,
configure it against agents the operator does not own, print an owner and a
policy account that are genuinely on chain, and exit 0. Every individual
assertion would hold. So the row is established as this run's before it is used:

1. **The chain half's own last word.** `deploy-agents.sh` prints
   `manifest: <path>` after the `mv` that moves a fully anchored manifest over
   the real one, and reaches that line on no other path. Its stdout is teed and
   that line is required. (stderr is left alone, so a twenty-minute deployment
   is still watchable as it happens.)
2. **The file on disk is not older than the run**, compared with `python3`
   rather than with `[ -nt ]`. Bash 3.2 — what macOS ships — compares whole
   seconds, and against a stub chain half that wrote the manifest in the same
   second as the stamp, the first version of this guard refused four correct
   runs in a row while reporting "older than this run" about a file written a
   moment earlier.
3. **The row agrees with the chain.** `record_prefix` is the first 73 bytes of
   the policy account as the chain holds them — version, owner, per-transaction
   limit, per-period limit, period — which is the whole immutable part of the
   record; everything after byte 73 is the running total, which moves. One
   `getAccount` decides it. An account nothing has written has no data at all,
   so the comparison fails on silence too, which is the direction to fail in.

Each of those three was made to fail on purpose, against stubbed halves, and
each refuses: a chain half that exits 0 without writing a manifest, one that
writes it and never says so, one that writes a row the chain contradicts, one
that writes a manifest with no row for the requested category. And the two ends:
a Logos Core half that fails after a good seam exits 2, and a clean run exits 0.

### It is one command *given three inputs it cannot produce*

Stated precisely, because "one command" is worth nothing if the sentence quietly
assumes a prepared machine without saying so:

1. **A funded public account id in `SIGNER`.** Balance comes from a faucet and
   no script can mint it. This is the only prerequisite in this whole section
   that is a testnet's policy rather than a package's.
2. **A wallet home holding that account's key** (`LEE_WALLET_HOME_DIR`).
3. **A Logos Core runtime.** On Linux `scripts/fetch-logos-core.sh` fetches one
   in a checksum-pinned command; on macOS it ships inside LogosBasecamp.app and
   upstream publishes no headless build.

Everything else the command does itself. All three are checked by name before
anything is spent, and `--dry-run` runs every check and stops, so a machine can
be told whether it is one of the machines this works on without finding out by
spending. Measured on the machine this repository was deployed from, where the
funder is down to 10 LEZ after the settlements the use cases record:

```
  storage      holds 6 of the 5 it must, so nothing is transferred
  messaging    holds 5 of the 55 it must, so 50 would be transferred
  blockchain   holds 5 of the 5 it must, so nothing is transferred
  funder       <id> holds 10
missing: testnet balance on SIGNER: it holds 10 and 50 has to be transferred
         THIS IS THE ONE PREREQUISITE THAT CANNOT BE SCRIPTED.
```

Note what that figure is not. It is **not** the sum of the funding floors:
`fund_agent` sends nothing to an agent that already holds its floor, and on a
re-anchor it must not — a shielded transfer does not credit an existing note, it
mints a new one under a new account id, and an agent whose id moves after it has
claimed has both of its accounts at addresses nothing will look at again. So the
number a balance is compared against is the **shortfall**, read per agent the
same way `fund_agent` reads it. Comparing against 65 instead would refuse a
perfectly good re-run on a funder that has spent down since.

The floors and the category list are parsed out of `deploy-agents.sh`'s own
`deploy_agent` lines rather than copied here, so a fourth category or a changed
floor cannot become a silent disagreement between two files — the shape that
disagreement would take is this command approving a balance that is not enough,
which is a check passing when it should not.

### One command deploys three agents and configures one

Said plainly because it is the kind of thing a headline elides.
`deploy-agents.sh` deploys **three** agents — one per default skill category,
which is what the prize asks for — and the wrapper configures **one** of them in
Logos Core, the one named by the category argument. Both facts are printed, and
the closing message names the free commands that configure the other two.

### The prerequisites, per half

Neither half is one command on a *bare* machine, and the rest of this section is
the exact list of what "prepared" means. It is written out because the reviewers
for this programme clone the repository and follow the instructions.

### What used to be here, and why it was worse than a missing list

Until this was fixed, `deploy-agents.sh` could not be run by anybody who was not
the author, and no prerequisite list said so. The three `deploy_agent` lines at
the bottom of the script each passed a **hardcoded account id** as a sixth
argument, which made the `${6:-$SIGNER}` fallback beside them unreachable.
Setting `SIGNER` did nothing for anchoring: the script printed `owner $SIGNER`
as its first line and then anchored with ids only the author's wallet held.

It failed in the worst possible order, which is why it is worth recording rather
than quietly correcting. `claim_agent` is signed by the AGENT, so it lands
whatever id is passed in `--owner-id`; `create_policy` is signed by the OWNER, so
that is the one that fails when the key is missing. Run against a wallet that
did not hold those three ids, the script therefore:

1. created an agent and **funded it with real balance**;
2. landed `claim_agent`, permanently naming an owner the operator cannot sign for;
3. only then failed, on `create_policy`, with `KeyNotFoundError`.

`claim_agent` is declared `#[account(init)]`. A claim cannot be rewritten. So
every agent that got that far was finished — funded, and impossible for anyone
to ever anchor a policy over. Reproduced against stub `wallet`/`spel` binaries
and a stub sequencer: three claims landed, three anchors failed, three agents
lost, and the run reported `3 of 3 agents did not deploy` as though nothing had
happened.

The fix is not "use `$SIGNER` for all three" — one signer per agent is genuinely
required, for the reasons in the section above on `create_policy` needing a
signer some program already owns. It is that `resolve_signer` now provisions the
three itself, in the operator's own wallet home, and records them in a
`signers.tsv` beside the manifest — created on first run and gitignored,
because it names accounts only that operator's wallet holds — so a resumed run
reuses the same three rather than
minting new ones the landed claims would refuse (6020). And the key is checked
**before** anything is created, funded or claimed, so the failure above is now a
refusal that costs nothing:

```
[storage] FAILED: ~/.lez-wallet holds no key for 2dA9APZgzcoX65YhNMJmsDC2v838ufLSjPyUdMknWoZd
        Refusing to fund an agent or land a claim naming an owner
        this machine cannot sign for: claim_agent is #[account(init)]
        and a claim cannot be rewritten.
```

`SIGNER` is now honestly named in what the script prints: it is the **funder**
and the account that deploys the program, not the owner. The owner of each agent
is printed beside that agent and written to the `owner` column of the manifest.

### Prerequisites for `./scripts/deploy-agents.sh`

A stranger needs all five. Nothing here is fetched automatically and the script
names whichever is missing rather than failing somewhere inside a transaction.

1. **A `wallet` binary built from LEZ at the pinned revision**, on `PATH` or at
   `WALLET_BIN`. Pin it: a wallet home is not portable between LEZ builds, and a
   `wallet` from another revision refuses one it did not create with
   `missing field 'accounts'` or `missing field 'sequencer_addr'` — which reads
   like a corrupted home and is a version mismatch. Revision in
   [`DEPLOYMENT.md`](DEPLOYMENT.md).
2. **A `spel` binary**, built from `vendor/spel`, on `PATH` or at `SPEL_BIN`.
3. **A wallet home holding `SIGNER`'s key**, at `LEE_WALLET_HOME_DIR`
   (default `~/.lez-wallet`). The three anchoring signers are created here by
   the script; keep this directory, because those keys are what may later call
   `update_policy` and `approve_spend`.
4. **Testnet balance on `SIGNER`** for the three funding floors — 5 + 55 + 5 =
   65 LEZ, plus the transfers' own cost. **This is the one that cannot be
   scripted.** It needs a faucet, and on this testnet a request to one is the
   only way to get it. With `SIGNER` unset the script exits 1 saying
   `set SIGNER to a funded public account id`; with `SIGNER` set but empty, the
   funding step times out per agent and reports `FAILED to fund the agent`.
5. **Network reach to the sequencer** at `SEQUENCER_URL`
   (default `https://testnet.lez.logos.co`).

Measured, with the program already on chain and no LEZ toolchain installed:

```
funder  <id>   (pays the agents; deploys the program)
homes   signer ~/.lez-wallet

program  697746f5…cb5370bf  already on chain

[storage] FAILED to resolve an anchoring signer in ~/.lez-wallet
[messaging] FAILED to resolve an anchoring signer in ~/.lez-wallet
[blockchain] FAILED to resolve an anchoring signer in ~/.lez-wallet

3 of 3 agents did not deploy; artifacts/agents.tsv left unchanged
```

Exit 1, and the manifest is not written — it is built in a temporary file beside
the real one and moved over it only once every agent has anchored, because a run
that fails is not entitled to delete the record of the one that worked. This
used to fail three steps later, at `FAILED to create an account`, having already
created wallet homes; the signer is resolved first now precisely so that a
machine which cannot anchor never gets as far as spending.

If the sequencer itself is unreachable, the program-deploy step waits twelve
minutes before giving up. That is deliberate — blocks are 60 seconds apart and a
deploy that is merely slow must not be called dead — but it means an offline
machine looks like a hang for the first twelve minutes rather than an error.

### Prerequisites for `./scripts/logos-core-headless.sh`

This is the half that is actually *Logos Core*. Each item is checked before
anything is built, and the script prints the missing ones by name with what to
do about them, so a machine that cannot run this says which piece it lacks
instead of failing inside a compile.

**The list is now two items long, and it used to be six.** Four of them — a
C++17 compiler, Qt 6.9.2 with `qtremoteobjects`, a `logos-cpp-sdk` checkout and
`nlohmann/json` — were prerequisites of *running* this command, because it
compiled the harness that drives the runtime on every machine it ran on. They
are prerequisites of **rebuilding** the harness now, and of nothing else: it is
built once per variant and committed under `module/harness/`, the way the plugin
is, and the shipped one is checked against the source it came from before it is
run. See "CLOSED: the deployment command no longer needs a build toolchain"
above for the measurement, and
[`basecamp.md`](basecamp.md) for how to rebuild it.

**A retraction first, because this list used to open on it.** It said the half
"cannot be made to work on an arbitrary machine ... because `liblogos_core`
ships inside the Logos app and there is no headless distribution of it to
download". The first clause is true everywhere. The second was checked against
the macOS `.dmg` and never against the Linux build, and the Linux build of the
same app is an **AppImage**: an ELF runtime with a SquashFS image appended,
published on the same release page, which unpacks with no installer, no root, no
FUSE and no display. So on Linux the runtime *is* fetchable, and the criterion
entry that blamed upstream for it was blaming upstream for something upstream
publishes.

Run once, then nothing below needs setting:

```sh
./scripts/fetch-logos-core.sh      # Linux only; ~278 MB, checksum-pinned
```

1. **A Logos Core runtime** — `liblogos_core` (`LOGOS_CORE_LIB`), the
   `logos_host` binary that runs core modules in their own process
   (`LOGOS_HOST_PATH`), the app's embedded modules directory
   (`LOGOS_EMBEDDED_MODULES`), and its Qt plugins (`QT_PLUGIN_PATH`).
   - **Linux:** `./scripts/fetch-logos-core.sh`. It pins the AppImage by sha256,
     refuses to unpack anything that does not match, and leaves the tree at
     `_external/logos-core/squashfs-root`, which is where the script looks by
     default. `LOGOS_APP` points it at an installed app instead.
     Needs `squashfs-tools` only on a host that cannot execute the AppImage
     itself (a cross-architecture container); on a native host the AppImage
     unpacks itself.
   - **macOS:** install LogosBasecamp from the `.dmg`. There is no AppImage to
     unpack there and no headless build published, so this one really is an app
     install. Measured against Basecamp 0.2.2 on darwin-arm64.
2. **`artifacts/agents.tsv`**, i.e. the first command has been run — it is where
   the owner and policy account come from. **For the agents this repository
   publishes that file is committed**, so a fresh clone has it and this
   prerequisite costs nothing; it is only for your own agents that the on-chain
   command has to run first. Without a row for the requested category the script
   says so and exits 1.
3. **A glibc and a libstdc++ new enough for the runtime**, which is a floor
   upstream sets and not one this repository adds to. Measured, out of the
   published AppImage: `liblogos_core.so` needs `GLIBC_2.38`, `GLIBCXX_3.4.29`
   and `CXXABI_1.3.15`; the app's Qt needs `CXXABI_1.3.15` and `GLIBCXX_3.4.32`;
   its bundled `libsystemd` needs `GLIBC_2.39`. The **shipped harness needs at
   most `GLIBC_2.38`, `GLIBCXX_3.4.29`, `CXXABI_1.3.9`** on both architectures —
   below the runtime on every axis, which is asserted rather than hoped:
   `scripts/check-package-fresh.py` reads the version-requirement table out of
   each committed binary and goes red if one climbs past `liblogos_core`'s own.
   So any Linux machine that can run Logos Core can run this. Debian bookworm
   (glibc 2.36) is below that line and is upstream's floor, not ours: there the
   *runtime* fails to load with `version GLIBC_2.38 not found (required by
   liblogos_core.so)` before anything of this repository's runs. Ubuntu 24.04 is
   above it.

**Linux is no longer untested, on either architecture.** Exercised on x86-64
Linux and on aarch64 Linux — Ubuntu 24.04, `liblogos_core.so` out of the
matching `LogosBasecamp-Desktop-v0.2.2-d41a72-*.AppImage`, the committed
`module/agent.lgx` and the committed harness — with all steps confirmed and zero
failures on both, in a container that has **no compiler, no Qt SDK, no
`logos-cpp-sdk` checkout and no `nlohmann/json`**:
`./scripts/harness-no-toolchain.sh`. The transcripts are in
[`basecamp.md`](basecamp.md).

### Additional prerequisites for `--alongside`

`./scripts/logos-core-headless.sh storage --alongside` loads the wallet, storage
and messaging modules into the same runtime, and those have to be built first by
`./scripts/build-companion-modules.sh`. That script **builds three upstream
modules from source**, so it needs everything above plus a full toolchain — the
same four the shipped harness removed from the plain run, which is why they are
written out again here rather than referred to:

4. **A C++17 compiler**, **Qt 6.9.2** (`QT_ROOT`) including `qtremoteobjects`
   and a `libexec/moc`, a **`logos-cpp-sdk` checkout** (`LOGOS_CPP_SDK_ROOT`,
   pinned at `c87f343`) and **`nlohmann/json`** headers (`EXTRA_INCLUDE_DIR`).
   The `aqtinstall` lines are in [`basecamp.md`](basecamp.md); an install with
   only `lib` in it is the trap described there, and on Linux the `icu` archive
   is required or every link ends in `undefined reference to ucnv_open_73`.
   These four are also what `--build-harness` and `HARNESS_FROM_SOURCE=1` need.
5. **`lgx`** (`LGX_BIN`), the packager from `logos-co/logos-package` that
   `module/package-basecamp.sh` already needs.
6. **A `logos-module-builder` and a `logos-module` checkout**
   (`LOGOS_MODULE_BUILDER_ROOT`, `LOGOS_MODULE_ROOT`) — the same two the agent
   module's own build needs, and the same pinned revisions.
7. **Go**, for `status-im/go-wallet-sdk`'s `make static-library`, which is what
   `logos-wallet-module` links.
8. **Nim and nimble** on `PATH`, for `liblogosdelivery`. Nimble installs Nim
   into `~/.nimble/bin` and does not add it to `PATH`; the script does that
   itself and says so if it still cannot find it.
9. **A built Logos Storage library** (`STORAGE_SRC`), which
   `scripts/exercise-nodes.sh` already documents how to produce.

Each is checked by name before anything is built. The heaviest by far is
`liblogosdelivery`: a large Nim project, several minutes on a warm tree and
considerably more on a cold one, and it has to be built at the revision
`logos-delivery-module`'s own `flake.lock` pins rather than at the tip — see
[`basecamp.md`](basecamp.md) for why and for the one dependency resolution that
has to be corrected on the way.

**This does not make the criterion conditional.** It was run, it exits 0, and
the transcript is in `basecamp.md`. What the list above says is what a *reviewer*
has to install to re-run it, which is the same class of prerequisite as the rest
of this section and is longer than any of them.

### What the count actually turns on

- **Deploying and configuring the published agent in Logos Core headless is one
  command**, and that is the command the criterion's own instrument names. On a
  prepared machine, `./scripts/logos-core-headless.sh storage` on a fresh clone
  installs the packaged module, loads it, calls `configure()` and `start()` on
  it across the runtime's transport, and reads the owner and policy account back
  out of `meta.status`. It needs no chain access and no argument beyond the
  category, because `artifacts/agents.tsv` is committed.
  `./scripts/deploy-and-configure.sh storage --configure-only` is the same thing
  with the row checked against the chain first, and submits nothing either.
- **Deploying an agent of your own is also one command**, and it is
  `SIGNER=… ./scripts/deploy-and-configure.sh storage`. It was two, and the
  argument for leaving it at two is retracted in full at the top of this
  section. The chain half still does nothing a Logos Core runtime does — it
  creates a shielded identity, funds it, and anchors a spending envelope on LEZ
  — and the prize's own Scope lists that beside deployment rather than inside
  it: "a CLI for agent deployment, configuration, **and initial funding**". A
  reviewer can read that sentence either way and no longer has to: both readings
  are one command now.
- **What that command cannot do is mint testnet balance.** It names the
  shortfall and stops. That is a prerequisite, in the same class as a Logos Core
  runtime, and it is listed as one rather than counted as a second command.
- **"On any machine" — closed, on both clauses.**
  Every platform the Logos app is published for is covered and exercised:
  macOS arm64, Linux x86-64, Linux aarch64. There is no fourth; Logos Core has
  no Windows build. What is **no longer** true is the reason this section used
  to give — that the Logos Core half needs an installed GUI app because upstream
  publishes no headless build. On Linux it publishes an AppImage, and one
  command unpacks it.

  And the second clause, which this section carried as the last open item: the
  command used to **compile** a harness, so a C++17 compiler, Qt 6.9.2 with
  `qtremoteobjects`, a `logos-cpp-sdk` checkout and `nlohmann/json` were
  prerequisites of *running* it. The harness is built once per variant and
  committed now, and the command was run to `all steps confirmed (0
  failure(s))` in a stock `ubuntu:24.04` container holding none of those four —
  `./scripts/harness-no-toolchain.sh`, which refuses to report anything at all
  if it finds a compiler on the machine it is checking. The section above has
  the transcript and what it cost.

  (An agent of your own additionally needs faucet-funded balance, which is a
  testnet's policy and cannot be scripted. The agents this repository publishes
  need none of it.)

What *is* met: on a prepared machine, `./scripts/deploy-and-configure.sh
<category>` is a single command that takes no argument beyond the agent's
category, deploys, configures, and verifies itself by reading back what it wrote
— the policy record byte for byte off the chain, twice, once by the half that
anchored it and once by the wrapper before it hands the row on, and the owner
and policy account out of the running module's `meta.status`. Either half can
still be run on its own, and both are documented above, because a composition
whose parts cannot be run separately is a composition nobody can debug.

Two other things in this repository drive Logos Core, and neither is part of the
deployment path this section is about — both are checks:
`module/tests/logos_core_load_test.cpp`, which loads the module and asks it for
its skills, and `module/tests/logos_core_delivery_test.cpp`, which is where the
loaded module opens its own Logos Delivery node inside `logos_host`.
`scripts/delivery-in-plugin.sh` runs the second. So `grep -rn 'logos_core'
scripts/`, which used to return nothing at all, now returns both the deployment
half and the transport checks.
