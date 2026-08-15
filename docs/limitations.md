# Limitations

What does not work, stated before anyone has to discover it.

## Anyone can anchor a policy for anybody's agent

**The most serious open defect in the program, and the one to read before
believing anything else in this repository about ceilings.**

`create_policy` now checks that the signer is the `owner_id` the policy commits
to (6012), and `spend` now checks that the account paying is the `agent_id` it
commits to (6013). Both were missing until the current deployment and both are
real improvements. Neither closes the hole, because `agent_id` is still a
caller-supplied argument that is never compared to anything:

1. An attacker holding the agent's key calls `create_policy`, signing with an
   account they control, naming that account as `owner_id`, the compromised
   agent as `agent_id`, and `per_tx = per_period = u128::MAX`. The owner check
   passes — the signer really is the owner it names.
2. They call `spend` with the agent's key. The agent check passes — the payer
   really is the agent that policy names. `per_tx` is `u128::MAX`, so every
   payment is autonomous and the approval path is never reached.

Both halves are accepted at halt 0 by the deployed binary; `cargo run --release`
in `crates/agent-verifier-adversarial` is the second row of its table. The two
accounts may be the same one, so a single stolen key is enough.

The predecessor of this defect is on chain rather than merely asserted:
`c0b21ba6…`, a `create_policy` with `per_tx = per_period = u128::MAX` naming an
owner nobody controls, was **accepted** by program `b028eabf…` at block 8652.
The identical call to the current program was submitted as `30c93c61…` and never
included. Absence is not evidence — a refused hash, a pending one and a hash
nobody sent all read `null` — so the refusal is demonstrated against the binary,
not inferred from the chain.

The fix is not another comparison; there is no id left in the instruction to
compare. It is to make the policy account's address depend on the agent alone,
so an agent has exactly **one** policy account and `init` refuses a second — the
limits moving out of the address and into the account's data, next to the
ledger. That trades the pure address commitment the design is built on for a
record this program writes once, and it changes the ImageID, the program and
every anchor. It is named here rather than half-made.

Until then: **the ceiling binds an honest agent and an outsider, not an attacker
who holds the agent's key.** Everything else in `security-model.md` §6 is
downstream of that sentence.

## Anchors die with every guest change, and the guest is effectively frozen

Not currently broken, but the sequencing constraint is permanent and it has now
bitten twice.

A policy account is a PDA derived from the program id, so rebuilding the guest
changes the ImageID, changes the program, and orphans every anchor made under
the old one. That has happened twice here: three agents were anchored, declared
as evidence, and invalidated an hour later by the chained-transfer rebuild; and
the same three were invalidated again by the rebuild that fixed the two defects
below. They are now anchored under program `8c87cc9b…2d20ebbe` (ImageID
`26ed1580…0bad50be`, deployed at block 8646) at blocks 8649, 8651 and 8652, and
verified live with the cannot-exist control returning null.

So criteria 1 and 2 must be satisfied under the *same* program, and the guest
has to be final before any anchor counts as evidence. Re-anchoring also costs
three signers that have never signed, for the reason below.

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

## spel builds every transaction against nonce 0

The single root cause behind the whole "submitted, returns a hash, never lands"
family, and it is a client bug rather than a chain rule.

Every public signer sits permanently at `nonce: 1` after its first landed
transaction and never advances again. Measured across five independent fresh
accounts, and corroborated by every signer in this repository: the ones that
landed something read nonce 1, the ones that never did read nonce 0.

The sequencer checks the nonce for exact equality
(`lee/state_machine/src/validated_state_diff/mod.rs:499-505`), so a second
transaction carrying nonce 0 against an account now at nonce 1 is dropped
silently. That explains why refreshing the wallet does not help, why
re-importing the key into a clean home does not help, and why the failure is
not specific to `create_policy` — `approve_spend` behaves identically.

Consequence: **any flow that needs a signer to act twice is unavailable** —
re-anchoring, rotating a policy, and the owner approving a spend. Only the
autonomous below-threshold path survives, because each agent signs once.

## The owner can never approve a spend after anchoring a policy

The most serious defect in the *tooling*, as against the program above, and it
is structural rather than a bug.

The constraint measured on chain is **one program transaction per public signer
account**, not one policy. Proven with a second instruction: `approve_spend`,
against a policy that genuinely exists and is program-owned, with a fresh marker
seed, also fails to land as its signer's second transaction.

`approve_spend` requires the owner as signer, and the current program compares
that signer against the `owner_id` the policy hash commits to — the account's
raw 32 bytes, not a hash of how they are printed. So the approval must come from
the same account that anchored the policy. That account has already spent its
one transaction on `create_policy`.

So the above-threshold path cannot work as designed: the owner who anchored a
policy is, by construction, unable to approve anything under it. The
below-threshold autonomous path is unaffected.

Ways out, none yet tried: make the owner account program-owned before anchoring
(a funded account is exempt from the rule — `DumJ4LCB…` holds two landed
anchors), or stop committing the signer's identity into `owner_id` so the
approval can come from a different account than the anchor.

Also recorded because it was asserted here and is false: the
`account get --account-id "Public/$signer"` refresh added to
`scripts/deploy-agents.sh`, with a comment claiming it "is what makes the next
anchor provable", does nothing. A second anchor was run with exactly that
refresh in place, against freshly fetched chain state, and still did not land.
Re-importing the signer into a completely fresh wallet home does not help
either. The gate reads the signer's on-chain state, not the wallet's.

Three things that used to be in this file are gone from it, because they were
fixed rather than reworded: `spend` moved no balance at all, a second
`create_policy` from one signer was silently dropped, and a repeat A2A
settlement could not be produced. The last one is now two settlements under the
current program, `5a488f28…` at block 8677 and `f780df62…` at block 8686, with
the recipient going 50 → 75 → 100 by `getAccount` (it had already reached 50
under the previous program, in blocks 8605 and 8624). What replaced them is
recorded in [`docs/DEPLOYMENT.md`](DEPLOYMENT.md); what is still true is below.

## A shielded agent can pay, but cannot be paid at its shielded account

`spel` resolves a `Private/<id>` account only for accounts the **signing**
wallet holds keys for — it builds them as `AccountIdentity::PrivateOwned`, and
a private account's state cannot be constructed without its viewing key. So one
agent naming another's private account as the recipient fails before anything
is built:

```
❌ Failed to submit privacy-preserving transaction: KeyNotFoundError
```

It is the same wall as funding: `auth-transfer` reaches a shielded account
through `--to-keys`, never through an account id.

So each agent also keeps a **public receiving account**, initialised once under
the transfer program, and its Agent Card advertises that as its payment address.
The payer stays shielded — the settlement is a privacy-preserving transaction
signed by the agent's own private account — and the payee is public. That is
half of what the design wants, and the honest description of the trade is that
the amount and the recipient of a task payment are visible, while the payer is
not.

The right fix needs work upstream, not here: the A2A card would carry the
recipient's `npk`/`vpk` under `x-logos`, and `spel` would build a
`PrivateForeign` recipient from them. The wallet already has that account kind
(`lez/wallet/src/account_manager.rs:30-34`); the CLI does not expose it.

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
  per-agent. Now `spend` and `spend_approved` compare the payer's account id to
  the policy's `agent_id` and refuse with 6013. The cost was the one predicted:
  `agent_id` stopped being an opaque label, every policy hash changed, and all
  three agents were re-anchored.
- **`spent_this_period` was supplied by the caller.** The per-period ceiling was
  compared against a number the agent passed in, both callers passed 0, and the
  enforced ceiling was therefore `min(per_tx, per_period)` *per transaction* and
  unbounded in aggregate. The running total now lives in the policy account's
  data, written only by the program that owns the account (LEZ rule 6,
  `UnauthorizedDataModification`). Read it off the chain:

  ```bash
  curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS"]}' \
  | python3 -c "
  import json,sys
  d=bytes(json.load(sys.stdin)['result']['data'])
  print('period', int.from_bytes(d[:8],'little'), 'spent', int.from_bytes(d[8:],'little'))"
  # period 8000 spent 50
  ```

  Empty at anchoring (block 8652), 25 after `5a488f28…` (block 8677), 50 after
  `f780df62…` (block 8686), against an anchored `per_period` of 1000.

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
on a default post-state owner. `DumJ4LCB…`, funded and therefore owned by the
authenticated transfer program, holds two landed anchors on the public testnet
at nonces 29 and 30 (blocks 8050 and 8051), and a locally claimed signer
anchored three in a row.

`owner` is **not** removed, though removing it would also clear the rejection.
`spel` signs only for accounts an instruction declares as signers, so an
instruction with no signer produces a transaction with an empty witness set —
and `create_policy` would become permissionless. It is also the account the
program now compares `owner_id` against (6012), so removing it would delete the
one binding that makes anchoring authenticated at all, and would widen the hole
in the first section of this file from "an attacker who holds the agent's key"
to "anybody". So the requirement stands and is documented instead: **anchor with
a signer that has already received a transfer.**

Also falsified while chasing this: stale local wallet state is not a factor.
`spel` refetches from the chain; an anchor landed with an older wallet snapshot
restored underneath it.

## deploy-agents.sh is not idempotent, and looks broken when it is right

Agent identities are stable once funded — `fund_agent` reuses the account that
already holds the balance rather than buying a new one. So a second run derives
the *same* policy hash, and `create_policy` refuses it, because the instruction
is declared `#[account(init, …)]` and `init` will not overwrite. That is the
single-use guarantee the design depends on, working exactly as intended.

The script now records what it anchored in `artifacts/anchored.tsv`, keyed by
`(program, policy_hash)`, and reports a refused re-anchor as already-anchored
rather than as a failure. The key has to include the program: a policy account
is a PDA of the program, so redeploying moves every policy to an address that
has never been initialised, and a ledger keyed on the hash alone would skip
exactly the anchoring the new program needs.

## Superseded programs are on the testnet

Deployment is content-addressed, so every version this repository has built is
still reachable by its own hash. Only the last one is referenced by anything:

| Deploy tx | What it was |
|---|---|
| `49f75568…b5f58b03` | returned two accounts for a three-account instruction; violates the SPEL invariant |
| `1ea86256…f18b6f3c` | `spend` without a recipient account — authorised payments, moved nothing |
| `6e4a2000…f365321a` | `spend` with a recipient, debiting the agent directly — no settlement was ever built under it, and rule 5 would have refused one |
| `b028eabf…b8c18549` | chains the transfer into the program that owns the accounts, but anchoring was unauthenticated and the period ceiling counted nothing — it accepted the attack transaction `c0b21ba6…` |
| `8c87cc9b…2d20ebbe` | current: identity bindings on anchoring and paying, and the period total on chain |

`artifacts/programs/agent_verifier.bin` hashes to the last row. Recompute rather
than trust it:

```bash
python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())"
# 8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe
```

## The node runs are local, not CI

Building the Delivery and Storage libraries takes tens of minutes and the runs
need live peers, so `scripts/exercise-nodes.sh` is a local command. The
reasoning is in [`docs/skills.md`](skills.md).
