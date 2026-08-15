# The security model

What the agent can do without asking, what it cannot, and what someone who
steals its keys gets. Every claim below names a file and a line, and the ones
that can be executed have been executed against the deployed binary.

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
| Changing a limit on an existing policy | nobody can: it is not data, see §2 |
| A payment above the ceiling with no approval | refused by the chain, error 6005 |

## 2. Why the limit is an address and not an `if`

A threshold checked inside the agent is worth nothing: the agent holds its own
keys on a remote node, so whoever takes the process takes the check
(`crates/agent-verifier-spel/methods/guest/src/bin/agent_verifier.rs:5-9`).

So the policy is not stored, it is *named*. `compute_policy_hash` folds the
owner, the agent and all three limits into one digest
(`crates/agent-policy-core/src/lib.rs:77-86`), and the account the spend must
present is the PDA seeded by that digest — `#[account(pda = arg("policy_hash"))]`
at `agent_verifier.rs:236` for `spend` and `:307` for `spend_approved`.

Raising a limit therefore does not edit an account. It names a *different*
account, which `create_policy` never initialised, whose `program_owner` is
still the default, and the spend is refused before the body runs
(`agent_verifier.rs:260-265`). Nothing is written into the policy account's
data at all — the address carries the whole meaning.

Two checks make that stick, and both are exercised in §7:

- The presented limits are re-derived into a hash and compared
  (`agent_verifier.rs:253-259`), so an agent cannot hand over generous numbers
  alongside a real policy account and have the address constraint alone miss it.
- The policy account must be owned by this program
  (`agent_verifier.rs:260-265`, `:342-347`).

## 3. Above the threshold: an approval that names one payment

`approve_spend` creates an account whose address is derived from the payment
itself — policy, recipient, amount, nonce — via `compute_spend_ref` and
`compute_approval_marker` (`crates/agent-policy-core/src/lib.rs:94-116`), and it
declares that account `#[account(init, pda = arg("marker_seed"))]`
(`agent_verifier.rs:181-182`). Three properties follow from that one line:

- **Single use.** `init` refuses to overwrite, so the same approval cannot be
  created twice.
- **Not transferable to another payment.** Change the recipient, the amount or
  the nonce and the seed changes, so the approval is for an account that does
  not exist.
- **Not replayable.** The nonce is what stops "send 100 to Bob", approved once,
  authorising the same transfer forever.

And the account being paid is checked against the account id the approval commits
to (`agent_verifier.rs:352-357`), so `recipient_id` is a real destination rather
than a label inside a hash.

## 4. Existence is not consent

The approval account is checked to be owned by *this program*, not merely to be
non-default (`agent_verifier.rs:379-384`):

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
none is committed. The settlement is a privacy-preserving transaction — 270,566
bytes of proof, see [`benchmarks/cu-budget.md`](benchmarks/cu-budget.md) — signed
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
- **Do that again, and again, with no cumulative limit.** The per-period ceiling
  is **not enforced on chain.** `spent_this_period` is an instruction argument
  (`agent_verifier.rs:249`, `:324`) supplied by the caller and passed straight
  into the comparison (`:268`, `:362`, `:388`). Nothing accumulates it: the
  policy account stores no data, `ProgramContext` carries only program ids and
  no block height (`vendor/spel/spel-framework-core/src/context.rs:27-34`), and
  `period_blocks` is folded into the policy hash and then never compared to
  anything. Both of this repository's own callers pass
  `--spent-this-period 0` literally (`scripts/a2a-task.sh:201`,
  `scripts/e2e-local-sequencer.sh:257`). So the
  ceiling the chain actually enforces is `min(per_tx, per_period)` **per
  transaction**, and the real bound on total loss is the agent's balance, not
  its policy.
- **Anchor themselves a new ceiling.** `create_policy` commits to an `owner_id`
  that is 32 caller-supplied bytes and never compares it to the account that
  signed (`agent_verifier.rs:137-165`); `spend` accepts any policy account that
  exists and whose hash matches the limits presented. Executed against the
  deployed binary: a `create_policy` signed by the *agent's own* account, for an
  invented `owner_id` with `per_tx = u128::MAX`, is accepted — and a `spend` of
  the agent's entire balance under that policy is accepted too. Both exit
  `Halted(0)`; see §7. Because a `per_tx` that large makes every payment
  autonomous, this also bypasses the approval machinery of §3 and §4 entirely.
- **Consume an approval the owner has already signed**, for the payment it
  names.
- **Act as the agent everywhere else**: upload and delete under its storage
  identity, message its owner and its peers as itself, and publish a signed
  Agent Card advertising whatever it likes.

### They cannot

- **Raise the limits of the policy the owner anchored.** Those numbers are the
  account's name; a different number is a different account (§2).
- **Forge an owner approval** for a payment above an existing policy's ceiling
  (§4), or point an approved payment at a different recipient (§3).
- **Spend from the owner's account, or from another agent's.** The payer is
  whichever account signs, and they hold only the agent's keys.
- **Pass the local process check by lying to it.** The C++ envelope check fails
  closed: an unknown period total or an unknown envelope forces the approval
  path rather than the autonomous one (`module/src/wallet_skills.cpp:394-404`).
  That is a property of the honest process only — see §8.

### The gap this leaves, stated plainly

The design's headline claim is that a compromised agent cannot raise its own
ceiling. That is true of *the policy the owner anchored* and false of *the agent's
authority overall*, because anchoring is not bound to the owner. The program's
own header comment names the requirement exactly — "anchoring has to be
authenticated: `spend` accepts any policy account that exists, and `owner_id` is
a caller-supplied 32 bytes, so an agent that could anchor a policy could anchor
itself an unlimited one and the ceiling would mean nothing"
(`agent_verifier.rs:119-122`) — and then meets it by declaring `owner` as a
signer. Declaring a signer proves that *somebody* signed. It does not prove that
the owner did, and the instruction never compares the two.

The fix is three lines and the precedent is in the same file: bind the
commitment to the signer the way `spend_approved` already binds the payment to
its recipient (`agent_verifier.rs:352-357`), by requiring
`*owner.account_id.value() == owner_id` in `create_policy` — and, for the same
reason, `*agent.account_id.value() == agent_id` in `spend`. It changes the
ImageID, the deployed program and every anchored policy, so it is recorded here
rather than half-done.

**What has not been shown** is the transaction. The two executions above are
program-level: they say the deployed program accepts these inputs. Actually
submitting them needs a signer the state machine will accept, and this
repository's own notes record that a signer still holding the default program
owner anchors exactly once, while a program-owned one can anchor repeatedly
([`limitations.md`](limitations.md)). Every agent has a program-owned account to
hand — the public receiving account its Agent Card advertises, claimed by
`auth-transfer init` in `scripts/deploy-agents.sh` — so the requirement looks
satisfiable, but it has not been executed end to end here. That is a question
about what the wallet will build, not a defence the policy program provides, and
it should not be reported as one.

What still holds in the meantime, and is worth not throwing away: a second
policy has to be *anchored on chain* to be used, which is a public, permanent
event an owner watching the chain can see. The compromise is loud. It is not
prevented.

## 7. Checking it yourself

Everything below is the deployed binary — `artifacts/programs/agent_verifier.bin`,
the bytes that hash to deploy transaction `b028eabf…b8c18549` — executed under
the risc0 executor with inputs written in the order the sequencer writes them.
The method, and how to rebuild the harness, is in
[`benchmarks/cu-budget.md`](benchmarks/cu-budget.md).

| Input | Result |
|---|---|
| `spend` of `per_tx + 1`, no approval | refused, `6005` — "the spend needs an owner approval" |
| `spend` presenting raised limits with the real policy account | refused, `6001` — "policy_hash does not commit to these limits" |
| `spend` against a policy account nobody created | refused, `6002` — "no policy is committed for these limits" |
| `spend_approved` with an approval account **funded by an outsider** at the right address | refused, `6007` — "no owner approval is anchored for this spend" |
| `spend_approved` with a real approval, redirected to another recipient | refused, `6011` — "the recipient account is not the one this spend names" |
| `spend_approved`, approved, that would still burst the period total | refused, `6006` |
| `spend` where the agent is not a signer | refused, `Unauthorized: Account 'agent' (index 1) must be a signer` |
| `create_policy` signed by the agent, `owner_id` invented, `per_tx = u128::MAX` | **accepted**, `Halted(0)` — §6 |
| `spend` of the agent's whole balance under that policy | **accepted**, `Halted(0)` — §6 |

One trap for anyone integrating against these numbers: the codes declared in the
guest as `6001…6011` reach a caller **offset by 6000**, because
`SpelError::error_code` returns `6000 + code` for a custom error
(`vendor/spel/spel-framework-core/src/error.rs:119`). What the log actually says
is `Program error [12005]: Program error 6005: …`.

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
