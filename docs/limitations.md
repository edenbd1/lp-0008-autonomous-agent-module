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

Consequence: **any flow that needs a public signer to act twice is
unavailable** — re-anchoring, `update_policy`, and the owner approving a spend.
Only the autonomous below-threshold path survives, because each agent signs
once, and because a *private* account is not affected: it is a note rather than
a nonce, so an agent can sign as many settlements as it can afford.

This is why `update_policy` — the instruction that makes a wrong anchor
recoverable — has no landed transaction on the public testnet. It is exercised
against the deployed binary in `crates/agent-verifier-adversarial` (accepted for
the owner the record names, refused 6012 for a stranger and for the agent
itself), and to run it on chain the owner must be an account some program
already owns. Any owner that has received a transfer is exempt; `DumJ4LCB…` is
the worked example, with two landed anchors at nonces 29 and 30. The three
owners in `artifacts/agents.tsv` are fresh public accounts, so each of them can
anchor and nothing else — which is a property of the client, not of the program.

## The owner can never approve a spend after anchoring a policy

The most serious defect in the *tooling*, as against the program above, and it
is structural rather than a bug.

The constraint measured on chain is **one program transaction per public signer
account**, not one policy. Proven with a second instruction: `approve_spend`,
against a policy that genuinely exists and is program-owned, with a fresh marker
seed, also fails to land as its signer's second transaction.

`approve_spend` requires the owner as signer, and the program compares that
signer against the `owner` field of the **policy record on chain** — the
account's raw 32 bytes, read off the account rather than supplied in the call.
So the approval must come from the same account that anchored the policy. That
account has already spent its one transaction on `create_policy`. `update_policy`
inherits the identical constraint for the identical reason.

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

What is **not** available is the step after a *successful* approval. An approved
above-threshold spend is submitted through the policy program's `spend_approved`,
which requires an approval account that only the owner's own `approve_spend`
signature can create — and that is the transaction the constraint above makes
impossible for the account that anchored the policy. So the module deliberately
does not submit on approval either: it reports `{"outcome":"approved",
"submitted":false}` and names `spend_approved` as the path that would have to
carry it. That is a refusal to claim a payment that did not happen, not a bug in
the wait.

In one line: **the agent's side of the approval exchange works and is tested;
the chain's side is unreachable on testnet today for the reason above.**

Three things that used to be in this file are gone from it, because they were
fixed rather than reworded: `spend` moved no balance at all, a second
`create_policy` from one signer was silently dropped, and a repeat A2A
settlement could not be produced. The last one is now two settlements under the
current program, `5a488f28…` at block 8677 and `f780df62…` at block 8686, with
the recipient going 50 → 75 → 100 by `getAccount` (it had already reached 50
under the previous program, in blocks 8605 and 8624). What replaced them is
recorded in [`docs/DEPLOYMENT.md`](DEPLOYMENT.md); what is still true is below.

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
  # period 8000 spent 50
  ```

  That is the messaging agent's policy account under the live program: 0 at
  anchoring (block 8876), 25 after `e691f593…` (block 8892), 50 after
  `aef14146…` (block 8901), against an anchored `per_period` of 250.

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
on a default post-state owner. `DumJ4LCB…`, funded and therefore owned by the
authenticated transfer program, holds two landed anchors on the public testnet
at nonces 29 and 30 (blocks 8050 and 8051), and a locally claimed signer
anchored three in a row.

`owner` is **not** removed, though removing it would also clear the rejection.
`spel` signs only for accounts an instruction declares as signers, so an
instruction with no signer produces a transaction with an empty witness set —
and `create_policy` would become permissionless. It is the account the program
records as the owner and the account the claim is compared against (6020), so
removing it would delete the binding that makes anchoring authenticated at all.
So the requirement stands and is documented instead: **anchor with a signer that
has already received a transfer**, or accept one anchor per signer.

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

## The node runs are local, not CI

Building the Delivery and Storage libraries takes tens of minutes and the runs
need live peers, so `scripts/exercise-nodes.sh` is a local command. The
reasoning is in [`docs/skills.md`](skills.md).
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

## The owner in the live owner-channel run is a node, not a second Basecamp

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

What remains is the *app instance*: the owner end is a program written for the
purpose, not Basecamp with a person in front of it.

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

## Only `darwin-arm64`, for the UI plugin as well

`app/agent-ui.lgx` carries one variant, the same as `module/agent.lgx`, and a
reviewer on Linux cannot open either. The plugin itself is portable — plain Qt
Widgets, no macOS-specific code, and `app/CMakeLists.txt` already takes the ELF
branch for the link options — so this is a build-and-package job on a Linux
host or in a container, against a Qt at or below Basecamp's 6.9.2, not a design
problem. LP-0002 ships two variants from the same shape and its Linux half was
verified against the AppImage's own Qt.

Cost to close: one container build per package, plus `lgx add --variant
linux-amd64` on each.

## deploy-agents.sh is one command, plus four things it cannot do for you

## Two commands, not one, and what each needs before it will run

The prize asks that "the owner can deploy the agent and configure it with a
single CLI command on any machine using Logos Core headless". That sentence has
two halves and this repository answers them with two commands, because they
deploy to two different places:

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

Neither is one command on a *bare* machine, and the rest of this section is the
exact list of what "prepared" means. It is written out because the reviewers for
this programme clone the repository and follow the instructions.

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

This is the half that is actually *Logos Core*, and it is the half that cannot
be made to work on an arbitrary machine — not for want of scripting, but because
`liblogos_core` ships inside the Logos app and there is no headless
distribution of it to download. Each item below is checked before anything is
built, and the script prints the missing ones by name with what to do:

1. **Logos Basecamp installed**, for four things inside it:
   `liblogos_core` (`LOGOS_CORE_LIB`), the `logos_host` binary that runs core
   modules in their own process (`LOGOS_HOST_PATH`), the app's embedded modules
   directory (`LOGOS_EMBEDDED_MODULES`), and its Qt plugins
   (`QT_PLUGIN_PATH`). Measured against Basecamp 0.2.2 on macOS/darwin-arm64.
2. **Qt 6.9.2** (`QT_ROOT`), including `qtremoteobjects` and a `libexec/moc`.
   The `aqtinstall` line is in [`basecamp.md`](basecamp.md). An install with only
   `lib` in it is the trap described there.
3. **A `logos-cpp-sdk` checkout** (`LOGOS_CPP_SDK_ROOT`). The harness calls the
   loaded module over the runtime's transport, and `liblogos_core`'s C API has no
   "call a method" entry point.
4. **`nlohmann/json`** headers (`EXTRA_INCLUDE_DIR`, default
   `/opt/homebrew/include`).
5. **`artifacts/agents.tsv`**, i.e. the first command has been run — it is where
   the owner and policy account come from. Without a row for the requested
   category the script says so and exits 1.

**Linux is untested.** The paths in the script for it are the documented ones and
nothing in this repository has ever run it against a Basecamp install on Linux.

### What is still not one command

Two things, stated plainly:

- **The two halves are two commands.** They could be wrapped in a third, and
  that wrapper would be honest only on a machine that has both a funded LEZ
  wallet and an installed Basecamp. It is not written, because a command that
  requires everything both of the above require, and then reports one exit code
  for two unrelated failures, makes the gap harder to see rather than smaller.
- **"On any machine" is not met, and cannot be from here.** The chain half needs
  faucet-funded balance; the Logos Core half needs an installed Logos app. The
  first is a testnet's policy and the second is upstream's packaging.

What *is* met: on a prepared machine each half is a single command that takes no
arguments beyond the agent's category, deploys, configures, and verifies itself
by reading back what it wrote — the policy record byte for byte off the chain,
and the owner and policy account out of the running module's `meta.status`.

Two other things in this repository drive Logos Core, and neither is part of the
deployment path this section is about — both are checks:
`module/tests/logos_core_load_test.cpp`, which loads the module and asks it for
its skills, and `module/tests/logos_core_delivery_test.cpp`, which is where the
loaded module opens its own Logos Delivery node inside `logos_host`.
`scripts/delivery-in-plugin.sh` runs the second. So `grep -rn 'logos_core'
scripts/`, which used to return nothing at all, now returns both the deployment
half and the transport checks.
