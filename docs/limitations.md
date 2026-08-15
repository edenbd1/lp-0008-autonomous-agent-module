# Limitations

What does not work, stated before anyone has to discover it.

Two things that used to be at the top of this file are gone from it, because
they were fixed rather than reworded: `spend` moved no balance at all, and a
second `create_policy` from one signer was silently dropped. What replaced them
is recorded in [`docs/DEPLOYMENT.md`](DEPLOYMENT.md); what is still true is
below.

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

## `spend` does not bind the policy to the account presenting it

`spend` re-derives the policy hash from `(owner_id, agent_id, limits)` and
requires the policy account to be the PDA of that hash and to have been
initialised. It does **not** check that the account signing as `agent` is the
account `agent_id` names.

So a funded account can present any anchored policy, including one anchored for
a different agent with a larger envelope. It can still only spend its own money
— it has to sign — but per-agent limits are not per-agent under an adversary
who reads the chain for the most generous anchored policy.

The fix is one comparison, `*agent.account_id.value() != agent_id`, and the cost
is that `agent_id` stops being an opaque label and becomes the account id, which
changes every policy hash and needs a redeploy and a re-anchor. It is the first
thing to do next, not something to describe as done.

## `spent_this_period` is supplied by the caller

The per-period ceiling is checked against a number the agent passes in. Nothing
on chain accumulates it — the policy account holds no data, deliberately, so
that its address alone encodes the envelope. An agent that always passes zero
has a per-transaction limit and no period limit.

Fixing it properly means the policy account carrying a counter and a window
start, which means it stops being a pure address commitment and starts being
mutable state. That is a design change with a real trade in it, so it is named
here rather than half-made.

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
and `create_policy` would become permissionless. Since `spend` accepts any
policy account that exists and `owner_id` is caller-supplied bytes, an agent
able to anchor a policy could anchor itself an unlimited one. The ceiling would
mean nothing. So the requirement stands and is documented instead: **anchor with
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
| `b028eabf…b8c18549` | current: chains the transfer into the program that owns the accounts |

`artifacts/programs/agent_verifier.bin` hashes to the last row.

## The node runs are local, not CI

Building the Delivery and Storage libraries takes tens of minutes and the runs
need live peers, so `scripts/exercise-nodes.sh` is a local command. The
reasoning is in [`docs/skills.md`](skills.md).
