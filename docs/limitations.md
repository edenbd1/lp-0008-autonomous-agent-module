# Limitations

What does not work, stated before anyone has to discover it.

## The settlement authorises a payment; it does not move tokens

This is the significant one, and it was overstated here and in
`docs/DEPLOYMENT.md` before being caught.

`spend` checks that the anchored policy permits an amount and returns the
accounts unchanged: `SpelOutput::execute(vec![...], vec![])` with an empty
chained-call vector, and the recipient's account is not among the accounts it
declares. Nothing in the instruction can therefore move value. The transaction
on chain is a real privacy-preserving proof that the policy authorises 25 LEZ —
it is not a transfer of 25 LEZ, and calling it "the payment" was wrong.

The prize asks that agents "transfer LEZ payment autonomously". That is not
met today.

### What it takes to meet it

LEZ's own `authenticated_transfer` program
(`lez/programs/authenticated_transfer/src/main.rs:22-59`) shows the shape: a
program moves value by returning post-states with modified balances —
`checked_sub` on the sender, `checked_add` on the recipient, and
`Claim::Authorized` on the recipient when its `program_owner` is still the
default. `SpelOutput::execute` accepts `(Account, AutoClaim)` pairs, so this is
expressible directly rather than through a chained call.

So `spend` must declare the recipient account as well as `policy` and `agent`,
and return all three with the sender debited and the recipient credited. The
policy check is then what it always claimed to be: a gate in front of a real
transfer rather than a substitute for one.

## A repeat A2A settlement does not land

`scripts/a2a-task.sh` produced one settlement that is on chain
(`aea80817…d98449e7`). Running it again does not produce a second one, and the
failure is in the interaction between `spend`'s account shape and the privacy
circuit rather than anywhere convenient.

`spend` declares three accounts — `policy`, `approval`, `agent`. Two constraints
meet there and, as the instruction stands, cannot both be satisfied:

- **SPEL** requires one returned account per declared account. Return two and
  the guest panics with
  `execute_with_claims: accounts.len() (2) != claims.len() (3)`.
- **The privacy circuit** counts *distinct* account identities and expects two.
  Seed `approval` from the real spend marker and it names a third,
  never-initialised PDA:
  `Invalid account_identities length, left: 3, right: 2`.

The instruction's own doc comment says an autonomous spend should pass the
policy account in the `approval` slot. That collapses three slots onto two
identities and fails differently:
`Pre-state account IDs are not unique`.

All three failures happen while building the transaction, so nothing reaches
the chain and nothing reports an error there. An earlier variant did submit and
returned a transaction hash that simply never landed, which is worse — a hash
that looks like a receipt and is not one. `a2a-task.sh` now refuses to write its
manifest unless the settlement is confirmed on chain, so an unconfirmed hash
cannot be mistaken for evidence.

The fix is a design change, not a patch: `spend` needs to stop declaring an
account it does not use on the autonomous path, which means splitting it into
an autonomous instruction taking two accounts and an approved one taking three.
That changes the ImageID, the deployed program, and every anchored policy, so
it is recorded here rather than half-done.

### What this does not affect

The three agents and their anchored policies are unaffected — different
instruction, and their `create_policy` transactions are confirmed on chain. So
is the deployed program, and so is the settlement that did land. What is not
available today is a *reproducible* second settlement.

## An extra program is deployed

`49f75568…b5f58b03` is on the testnet from an attempt at the fix above that
returned two accounts and violated the SPEL invariant. Nothing references it and
nothing calls it. The program this repository uses is `1ea86256…f18b6f3c`, and
`artifacts/programs/agent_verifier.bin` hashes to that one.

## The node runs are local, not CI

Building the Delivery and Storage libraries takes tens of minutes and the runs
need live peers, so `scripts/exercise-nodes.sh` is a local command. The
reasoning is in [`docs/skills.md`](skills.md).
