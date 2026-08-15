# The security model

What the agent can do without asking, what it cannot, and what someone who
steals its keys gets. Every claim below names a file and a line, and the ones
that can be executed have been executed against the deployed binary.

**Which program this describes.** `artifacts/programs/agent_verifier.bin`,
ImageID `26ed1580…0bad50be`, deploy transaction `8c87cc9b…2d20ebbe` at block
8646. That matters more than it usually would: a policy account is a PDA of the
program id, so a rebuild moves every anchor, and a claim about "the program" is
only meaningful against one ImageID. Line references are to the guest source
that builds to that ImageID. If the two ever disagree, the binary is right —
recompute its hash with the command in [`DEPLOYMENT.md`](DEPLOYMENT.md) and
check it against the chain.

The design in one sentence: **the agent's spending authority is an account
address, not a branch in its code.** The rest of this document is what that buys,
and — the longer half — what it does not.

## 1. The envelope

The owner fixes three numbers when the agent is deployed: the largest single
transfer it may make unattended (`per_tx`), the largest total it may move in a
window (`per_period`), and the window's length in blocks (`period_blocks`)
(`crates/agent-policy-core/src/lib.rs:45-52`).

| Action | Who has to agree |
|---|---|
| A payment at or below `per_tx` | nobody — the agent alone |
| A payment above `per_tx` | the owner, on an approval account naming that exact payment |
| Changing a limit on an existing policy | nobody can: the limits are the account's *address*, not its contents, see §2 |
| A payment above the ceiling with no approval | refused by the chain, error 6005 |

## 2. Why the limit is an address and not an `if`

A threshold checked inside the agent is worth nothing: the agent holds its own
keys on a remote node, so whoever takes the process takes the check
(`crates/agent-verifier-spel/methods/guest/src/bin/agent_verifier.rs:5-9`).

So the policy is not stored, it is *named*. `compute_policy_hash` folds the
owner, the agent and all three limits into one digest
(`crates/agent-policy-core/src/lib.rs:77-86`), and the account the spend must
present is the PDA seeded by that digest — `#[account(pda = arg("policy_hash"))]`
at `agent_verifier.rs:382` for `spend` and `:488` for `spend_approved`.

Raising a limit therefore does not edit an account. It names a *different*
account, which `create_policy` never initialised, whose `program_owner` is
still the default, and the spend is refused before the body runs
(`agent_verifier.rs:406-411`).

The division of labour inside that account is worth stating exactly, because an
earlier version of this document got it wrong in the direction that flatters us.
**The address carries the limits; the data carries what has been spent against
them.** The address is immutable by construction — it is a hash — and the data
is mutable by exactly one writer, this program, because the account is its PDA
and LEZ rule 6 refuses a data write from anyone else
(`UnauthorizedDataModification`, `lee/state_machine/core/src/program/mod.rs:718-728`).
So the ceiling cannot be edited and the running total cannot be forged. §6 is
where that second half is measured.

Three checks make the address binding stick, and all three are exercised in §7:

- The presented limits are re-derived into a hash and compared
  (`agent_verifier.rs:399-405`), so an agent cannot hand over generous numbers
  alongside a real policy account and have the address constraint alone miss it.
- The policy account must be owned by this program
  (`agent_verifier.rs:406-411`, `:523-528`).
- The account **paying** must be the agent the policy names
  (`agent_verifier.rs:417-422`, `:532-537`). Without this last one every
  anchored policy on the chain is a ceiling any agent may borrow, and the
  effective limit is whichever policy on the chain happens to be loosest. It was
  missing until this deployment; `limitations.md` records the version that
  shipped without it.

## 3. Above the threshold: an approval that names one payment

`approve_spend` creates an account whose address is derived from the payment
itself — policy, recipient, amount, nonce — via `compute_spend_ref` and
`compute_approval_marker` (`crates/agent-policy-core/src/lib.rs:94-116`), and it
declares that account `#[account(init, pda = arg("marker_seed"))]`
(`agent_verifier.rs:291-292`). Three properties follow from that one line:

- **Single issue.** `init` refuses to overwrite, so the same approval cannot be
  created twice.
- **Not transferable to another payment.** Change the recipient, the amount or
  the nonce and the seed changes, so the approval is for an account that does
  not exist.
- **Not replayable.** The nonce is what stops "send 100 to Bob", approved once,
  authorising the same transfer forever.

`init` gives single *issue*, which is not the same as single *use*, and the
difference was a real defect: a marker that exists and is never touched
authorises the same payment on **every** later transaction that presents it. So
`spend_approved` stamps the marker as it consumes it and refuses one already
stamped (`agent_verifier.rs:577-585`, error 6018). Issuance is guarded by `init`;
consumption is guarded by the stamp. Both are in §7.

Two more bindings close the obvious ways round it. The account being paid is
checked against the account id the approval commits to
(`agent_verifier.rs:542-547`), so `recipient_id` is a real destination rather
than a label inside a hash. And `approve_spend` checks that the signer is the
owner the policy commits to (`agent_verifier.rs:328-333`, error 6012) — without
that the agent signs its own approvals and steps over the threshold from the
other side, which makes the whole above-threshold path decorative.

## 4. Existence is not consent

The approval account is checked to be owned by *this program*, not merely to be
non-default (`agent_verifier.rs:564-569`):

```rust
if approval.account.program_owner != ctx.self_program_id {
    return Err(SpelError::custom(E_APPROVAL_NOT_ANCHORED, ...));
}
```

The weaker check — "an account exists at that address" — would make consent a
function of an account existing rather than of the owner having signed. The
marker seed is derivable by anyone who knows the payment, so anyone who could
get *some* account to exist at that address would have manufactured an approval:
credit it, and the transfer program claims a previously-default account on the
way in (`lez/programs/authenticated_transfer/src/main.rs:50-53`). Requiring this
program to be the owner means the account can only have come from
`approve_spend`, which only runs when someone signed it.

Whether an outsider could in fact fund an address they do not control is a
separate question about LEZ's claiming rules — `scripts/deploy-agents.sh` records
that a claim on a public account the payer did not sign for is rejected as
`ClaimedUnauthorizedAccount` — and the point is that this program does not have
to depend on the answer.

Measured, not argued: the "approval merely funded" row in §7.

## 5. The agent is an account holder like any other

Each agent gets its own shielded account, created in its own wallet home outside
the repository (`scripts/deploy-agents.sh`, `new_agent`); no key is shared and
none is committed. The settlement is a privacy-preserving transaction — about 270 kB,
almost all of it proof, see [`benchmarks/cu-budget.md`](benchmarks/cu-budget.md) — signed
by the agent, so what the chain sees is an account holder spending, with nothing
marking it as a robot.

**The honest qualification**: the *payee* is a public account. `spel` can only
address a private account whose keys the sending wallet holds, so one shielded
agent cannot name another's private account as a recipient. Each agent therefore
keeps a public receiving account, which its Agent Card advertises. The payer and
the amount's origin stay shielded; the credit side is readable by anyone with
`getAccount`. That is a real reduction in privacy relative to "both ends
shielded", it is deliberate, and it is written up in
[`limitations.md`](limitations.md) rather than glossed.

## 6. What someone who steals the agent's key can do

This is the half that matters, so it is stated in full rather than implied.

### They can

- **Spend up to `per_tx`, to any address they like, unattended.** There is no
  allowlist. The policy binds *how much*, never *to whom* — recipient binding
  exists only on the approved path (§3).
- **Repeat that up to `per_period` in each period, and again in the next one.**
  The period ceiling is real (below), but it is a rate limit, not a total: a
  patient attacker still drains the account over enough periods. What it buys is
  time, and time is what an owner watching the chain needs.
- **Anchor themselves a new ceiling, and spend the whole balance under it.**
  This is the one that is still open, and it is the reason the rest of this
  section is worth reading carefully rather than skimming. See "the gap this
  leaves" below for exactly how far it goes and exactly what now bounds it.
- **Consume an approval the owner has already signed**, for the payment it
  names — **once**. The marker is stamped on use (§3), so an approval that has
  been paid out cannot be presented again (6018, §7).
- **Act as the agent everywhere else**: upload and delete under its storage
  identity, message its owner and its peers as itself, and publish a signed
  Agent Card advertising whatever it likes.

### They cannot

**Read this list with its scope attached**, because otherwise it contradicts the
one above. Everything here is what the chain refuses *under the policy the owner
anchored*. The third bullet of "they can" is precisely the escape from all of
it: an attacker who anchors a fresh policy of their own is no longer inside
these constraints, they are inside a different account's. So this list describes
how tight the envelope is, and "the gap this leaves" describes how to get out of
it. Both are true; neither is the whole answer on its own.

- **Raise the limits of the policy the owner anchored.** Those numbers are the
  account's name; a different number is a different account (§2).
- **Exceed `per_period` inside a period by splitting the payment up.** This is
  the claim that was false in every version of this document before the current
  deployment, so it is the one to check rather than take: the running total is
  now on chain, in the policy account's data, and the program that owns that
  account is the only thing that can write it (§2). Error 6006, measured in §7
  and readable on the chain right now — see below.
- **Reset the period early to get a fresh budget.** The period is named by the
  caller, because no program on this chain can read the block height, but naming
  it is not choosing it: it must be a multiple of `period_blocks` (6014), it may
  not be older than the period the ledger records (6015), and the transaction is
  pinned to `[window_start, window_start + period_blocks)` through
  `ProgramOutput`'s block validity window, so naming a *future* period yields a
  transaction that no current block will include (`OutOfValidityWindow`,
  `lee/state_machine/src/validated_state_diff/mod.rs:202-208` public,
  `:393-398` privacy-preserving). Sliding forward, replaying backwards and
  jumping ahead are each refused by a different one of those three.
- **Borrow another agent's policy.** `spend` and `spend_approved` check that the
  account paying is the agent the policy commits to (6013, §2). Before this
  deployment they did not, and the effective ceiling was the loosest policy
  anchored anywhere on the chain.
- **Forge an owner approval** for a payment above an existing policy's ceiling
  (§4), sign their own approval (6012, §3), point an approved payment at a
  different recipient (6011, §3), or present a spent one twice (6018, §3).
- **Spend from the owner's account, or from another agent's.** The payer is
  whichever account signs, and they hold only the agent's keys.
- **Pass the local process check by lying to it.** The C++ envelope check fails
  closed: an unknown period total or an unknown envelope forces the approval
  path rather than the autonomous one (`module/src/wallet_skills.cpp:394-404`).
  That is a property of the honest process only — see §8.

### The period ledger, on the chain, right now

The per-period ceiling is the claim this document got wrong for four
deployments, so it is the one stated with a command rather than a sentence. The
blockchain agent's policy account is `BLHNchq8…GhpsS`; it was created with empty
data at block 8652 and every byte in it since was written by the policy program:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS"]}' \
| python3 -c "
import json,sys
r=json.load(sys.stdin)['result']; d=bytes(r['data'])
print('period', int.from_bytes(d[:8],'little'), 'spent', int.from_bytes(d[8:],'little'))"
# period 8000 spent 50
```

Two settlements of 25 each — `5a488f28…` at block 8677 and `f780df62…` at block
8686 — took it 0 → 25 → 50, in a policy whose anchored `per_period` is 1000.
Nothing the agent sends can lower that number, because the write is the
program's and rule 6 forbids anyone else's; at 1000 `spend` refuses with 6006
until block 9000, when the window rolls and the budget starts again.

The 24 bytes are `window_start` then `spent`, both little-endian
(`agent-policy-core/src/lib.rs:198-228`), which is the whole format.

### The gap this leaves, stated plainly

The design's headline claim is that a compromised agent cannot raise its own
ceiling. It is true of *the policy the owner anchored* and **still false of the
agent's authority overall**, and this deployment narrowed the hole without
closing it. Stated as the attack, because that is the only form in which it can
be checked:

1. The attacker holds the agent's key. They call `create_policy` signing with
   an account they control, naming that account as `owner_id` and the
   compromised agent as `agent_id`, with `per_tx = per_period = u128::MAX`.
   The owner check passes — the signer really is the owner it names.
2. They call `spend` under that policy with the agent's key. The agent check
   passes — the payer really is the agent that policy names. `per_tx` is
   `u128::MAX`, so every payment is autonomous, the period ceiling is never
   reached, and §3's approval machinery is never consulted.

Both halves are accepted by the deployed binary, at halt 0, in
`crates/agent-verifier-adversarial` — the second and fourth rows of §7. The
attacker's account may be the compromised agent's own, which is the cheapest
version: one stolen key is enough for both signatures.

What the three bindings did buy is narrower than "fixed", and it is worth being
exact about it. Before them the attacker could name an owner that does not
exist (`aaaa…aaaa`, an account nobody controls) and could point any agent at
any anchored policy. Now the anchoring account must be one they actually hold,
and the paying account must be the one the policy names. The residual defect is
that **nothing ties `agent_id` to the agent's real owner**: `create_policy` will
anchor a policy for any `agent_id` the caller writes down, so the set of
policies naming a given agent is open to anyone, and `spend` is happy with any
of them.

The fix is not another comparison — there is no id in the instruction left to
compare. It is to make the policy account's address depend on the agent alone,
so that an agent has exactly *one* policy account and `init` refuses a second.
That means seeding the PDA on `agent_id` rather than on the full policy hash and
moving the limits into the account's data next to the ledger, which trades the
pure address commitment of §2 for a record this program writes once. It changes
the ImageID, the deployed program and every anchored policy, so it is recorded
here rather than half-made.

**What has not been shown on chain** is the second half. `c0b21ba6…`, a
`create_policy` with `per_tx = per_period = u128::MAX` naming an owner nobody
controls, was accepted by the *previous* program at block 8652 — that is a real
transaction, and it is why this is a matter of record rather than a worry. The
identical call to the current program, `30c93c61…`, was submitted and never
included. That second hash proves nothing by itself: a refused transaction, a
pending one and a hash nobody ever sent all read `null` from `getTransaction`,
which is exactly what `demo.sh`'s cannot-exist-hash control demonstrates. The
refusal is shown where it can be — against the binary, in §7. The *surviving*
attack has not been submitted as a transaction either, and it should not be
reported as prevented on the strength of that.

What still holds, and is worth not throwing away: a second policy has to be
*anchored on chain* to be used, which is a public, permanent event an owner
watching the chain can see, and the ledger of §6 means the honest policy's own
spending is public too. The compromise is loud. It is not prevented.

## 7. Checking it yourself

Everything below is the deployed binary — `artifacts/programs/agent_verifier.bin`,
the bytes that hash to deploy transaction `8c87cc9b…2d20ebbe`, ImageID
`26ed1580…0bad50be` — executed under the risc0 executor with inputs written in
the order the sequencer writes them. Run it:

```bash
cd crates/agent-verifier-adversarial && cargo run --release
```

Every refusal is paired with the honest call it differs from in one field. A
table that only ever refuses says nothing about what is accepted, and a program
that refuses everything would pass it.

| Input | Result |
|---|---|
| `create_policy` naming an `owner_id` the signer does not control | refused, `6012` — "the signer is not the owner this policy commits to" |
| …the same call, naming the account that actually signs | **accepted**, `Halted(0)` — §6, still open |
| `spend` under a policy anchored for a **different** agent | refused, `6013` — "the paying account is not the agent this policy commits to" |
| …the agent that policy names, same amount | **accepted**, `Halted(0)` |
| `spend` of `per_tx + 1`, no approval | refused, `6005` — "the spend needs an owner approval" |
| `spend` that would carry the period total past `per_period` | refused, `6006` |
| …the same spend in the next period, where the budget starts again | **accepted**, `Halted(0)` |
| `spend` naming a period that is not a multiple of `period_blocks` | refused, `6014` |
| `spend` presenting raised limits with the real policy account | refused, `6001` — "policy_hash does not commit to these limits" |
| `spend` against a policy account nobody created | refused, `6002` — "no policy is committed for these limits" |
| `spend` where the agent is not a signer | refused, `Unauthorized: Account 'agent' (index 1) must be a signer` |
| `approve_spend` signed by the agent rather than the owner | refused, `6012` |
| …signed by the owner | **accepted**, `Halted(0)` |
| `spend_approved` with an approval account **funded by an outsider** at the right address | refused, `6007` — "no owner approval is anchored for this spend" |
| `spend_approved` with a real approval, redirected to another recipient | refused, `6011` — "the recipient account is not the one this spend names" |
| `spend_approved` presenting an approval that has already been stamped | refused, `6018` — "this owner approval has already been spent" |

**One row that used to be here and was wrong.** Earlier versions of this table
claimed that `spend_approved`, on a valid approval, is refused with `6006` if it
would burst the period total. It is not: executed against this binary it is
**accepted**. That is deliberate rather than an oversight — an approved payment
carries the owner's signature naming this recipient, this amount and this nonce,
and it does not draw on the *unattended* budget, which is the thing `per_period`
bounds (`agent_verifier.rs:466-471`). Making an owner's own approval refusable
because the agent had been busy would buy nothing, since the marker is
single-use anyway. But the document asserted a refusal that does not happen, and
the correction belongs in the open rather than in a silent edit.

One trap for anyone integrating against these numbers: the codes declared in the
guest as `6001…6018` reach a caller **offset by 6000**, because
`SpelError::error_code` returns `6000 + code` for a custom error
(`vendor/spel/spel-framework-core/src/error.rs:119`). What the log actually says
is `Program error [12005]: Program error 6005: …`. Matching the bracketed number
would match `6012` against `12012` and pass a case that halted for an entirely
different reason.

## 8. Where the enforcement is *not*

Three things in this repository look like controls and are not, and it is worth
being explicit about which is which.

- **The module's envelope check is a courtesy.** `wallet.send` compares the
  amount against a cached `SpendEnvelope` before building anything
  (`module/src/wallet_skills.cpp:394-404`), and its own header says why that is
  not the control: the limits it compares against are a cache of an on-chain
  fact (`module/src/wallet_skills.h:47`). It saves proving cost on a payment the
  chain would refuse. It stops nobody who has replaced the process.
- **The owner channel's timeout is a property of the honest agent.** An
  above-threshold spend goes to the owner over a reliable channel as a
  `spend_approval_request` (`module/src/owner_channel.cpp:333`), is re-sent every
  15 s, and after 120 s with no answer becomes `ApprovalVerdict::Unreachable`
  (`module/src/owner_channel.h:233`, `owner_channel.cpp:456`) — terminal, never a
  fallback to acting alone, which is what the prize requires. A nice detail: a
  reply carrying `per_tx`, `per_period`, `period_blocks` or `policy` is refused
  outright, because "an approval names a payment, it cannot change a limit"
  (`owner_channel.cpp:155-158`). All of that binds an agent that is still running
  our code. An attacker simply does not send the request.
- **The inference layer is advisory by construction.** It can only ever move an
  answer toward *decline*; the amount paid is always the offer's price, never a
  number read out of a model's reply (`module/src/inference.cpp:397-421`, the
  restatement check). A hostile model cannot authorise a payment, because it is
  never asked to name one.

The pattern is the same in all three: anything the agent process decides, whoever
holds the agent process decides differently. Only §2, §3 and §4 survive that,
and §6 says exactly how far they get.

## 9. Assumptions this rests on

- **The sequencer enforces the state machine.** Rule 5 forbids a program from
  debiting an account it does not own
  (`lee/state_machine/core/src/program/mod.rs:707-716`), which is why `spend`
  chains into the transfer program rather than moving balance itself; rule 8
  requires total balance to be preserved across a transaction. These are LEZ's
  checks, not ours, and we verify them by observing that transactions violating
  them do not land.
- **The privacy circuit is what the sequencer pins.** The chained call is
  re-executed and checked under `env::verify` inside the proof
  (`lee/privacy_preserving_circuit/src/execution_state.rs:149-155`).
- **The owner's key is not on the agent's machine.** If it is, §6's distinction
  between "the agent's keys" and "the owner's keys" disappears.
- **Above-threshold payments are the exception.** Every recorded settlement so
  far has been inside the envelope, so the approved path is exercised by
  execution against the deployed binary (§7) and by
  `scripts/e2e-local-sequencer.sh`, not yet by a landed above-threshold payment
  on the public testnet.

Known failures and retractions are kept in [`limitations.md`](limitations.md),
which is the document to read next.
