# The security model

What the agent can do without asking, what it cannot, and what someone who
steals its keys gets. Every claim below names a file and a line, and the ones
that can be executed have been executed against the deployed binary.

**Which program this describes.** `artifacts/programs/agent_verifier.bin`,
ImageID `12fa95d9…b578c9d8`, deploy transaction `a780003b…8576841e` at block
8720. That matters more than it usually would: a policy account is a PDA of the
program id, so a rebuild moves every anchor, and a claim about "the program" is
only meaningful against one ImageID. Line references are to the guest source
that builds to that ImageID. If the two ever disagree, the binary is right —
recompute its hash with the command in [`DEPLOYMENT.md`](DEPLOYMENT.md) and
check it against the chain.

The design in one sentence: **an agent has exactly one policy account, its
address is the agent, and only the program that owns it can write what it
says.** The rest of this document is what that buys, and — the longer half —
what it does not.

## 1. The envelope

The owner fixes three numbers when the agent is deployed: the largest single
transfer it may make unattended (`per_tx`), the largest total it may move in a
window (`per_period`), and the window's length in blocks (`period_blocks`)
(`crates/agent-policy-core/src/lib.rs:131-140`).

| Action | Who has to agree |
|---|---|
| A payment at or below `per_tx` | nobody — the agent alone |
| A payment above `per_tx` | the owner, on an approval account naming that exact payment |
| Changing a limit on an existing policy | nobody can, including the owner: there is one policy account per agent and `create_policy` is the only writer of its limits, see §2 |
| Anchoring a *second*, looser policy for the same agent | nobody can: the address is a function of the agent, and `init` refuses an account that exists |
| A payment above the ceiling with no approval | refused by the chain, error 6005 |

## 2. One policy account per agent

A threshold checked inside the agent is worth nothing: the agent holds its own
keys on a remote node, so whoever takes the process takes the check
(`crates/agent-verifier-spel/methods/guest/src/bin/agent_verifier.rs:5-9`).

So the policy lives in an account, and the account's address is

```
PDA(agent_verifier, ["agent-policy/v1", agent_id])
```

and nothing else — `#[account(init, pda = [literal("agent-policy/v1"),
arg("agent_id")])]` at `agent_verifier.rs:244`. Everything the policy *says* —
the owner, both limits, the period, and the running total — is that account's
**data**, 97 bytes in a fixed layout (`agent-policy-core/src/lib.rs:256-311`).

Two consequences, and they are the whole security argument:

- **There is one address, so there is one policy.** `create_policy` declares the
  account `#[account(init)]`, and `init` refuses an account that is not in its
  default state. The first anchor for an agent is the only anchor for that
  agent. A second one is not *detected*; it has nowhere to go.
- **Only this program can write it.** The account is the program's PDA, so LEZ
  rule 6 refuses a data write from anyone else (`UnauthorizedDataModification`,
  `lee/state_machine/core/src/program/mod.rs:718-728`). The limits are written
  once, by `create_policy`; afterwards `spend` writes only the running total
  (`agent_verifier.rs:390-397`).

### What this replaced, and why the replacement was necessary

The three deployments before this one derived the address from the policy's
*contents* — a `compute_policy_hash` over (owner, agent, `per_tx`, `per_period`,
`period_blocks`). The reasoning was that an agent cannot edit a limit, because an
edited limit names a different address that `create_policy` never initialised.
That much was true, and it was never the attack.

Folding the limits into the address means every (owner, agent, limits) triple has
an address of its own, so there is no such thing as *the* policy account for an
agent — there are 2^256 of them, all uninitialised, and anchoring is first-come
at every single one. Three separate fixes were shipped against that, each adding
a comparison:

| Deployment | What it added | Why it did not close the hole |
|---|---|---|
| `b028eabf…` and before | — | `create_policy` never compared `owner_id` to the signer, so an agent anchored a policy naming an owner that does not exist |
| `8c87cc9b…` | the signer must be the `owner_id` it commits to (6012); the payer must be the `agent_id` (6013); `approve_spend`'s signer must be the owner (6012) | the attacker does not need to invent an owner or borrow a policy. **It is the owner.** |

The version that mattered is the last one, and it is worth stating as the
attacker would: holding the agent's key, anchor a *fresh* policy naming the
compromised agent as `agent_id` and an account you control as `owner_id`, with
`per_tx = per_period = u128::MAX`, then spend the balance under it. Every
comparison above passes, because every one of them is satisfied — the signer
really is the owner that policy names, and the payer really is the agent it
names. §6 has the transactions.

The fix could not be another comparison; there was no id left in the instruction
to compare. It was to take the choice of address away. That is what this
deployment does, and it is why the ImageID, the program and every anchored policy
changed with it.

### What the call still carries, and what it no longer can

Because the address now carries the identity and the account carries the terms,
most of the instruction arguments have gone, and each removal is a class of
disagreement that can no longer be expressed:

- `create_policy` has **no `owner_id`**. The program writes the signer's own
  account id into the record (`agent_verifier.rs:265-271`). The claim and the
  fact cannot differ, because there is no claim.
- `spend` has **no `agent_id` and no limits**. The policy account's address is
  derived from the *paying* account itself — `pda = [literal("agent-policy/v1"),
  account("agent")]`, `agent_verifier.rs:374` — and the ceiling is read out of
  that account. An agent presenting another agent's policy account fails the PDA
  check the macro emits before the body runs.
- `spend_approved` is the same, and reads the agent for the approval marker off
  the signing account rather than the call (`agent_verifier.rs:495-500`).
- `approve_spend` still names the agent, because the owner has to say which agent
  it is approving for, but the owner it compares the signer against comes out of
  the policy record (`agent_verifier.rs:318-324`, error 6012), not out of the
  instruction.

Three error codes were **retired rather than reused**: 6001 (`policy_hash` does
not commit to these limits), 6004 (marker seed mismatch at anchoring) and 6013
(the payer is not the agent this policy commits to). All three existed only
because the caller chose the address and the program had to check the choice.
Leaving the numbers unused keeps an integration that branches on them from
silently matching a different refusal (`agent_verifier.rs:196-202`).

## 3. Above the threshold: an approval that names one payment

`approve_spend` creates an account whose address is derived from the payment
itself — agent, recipient, amount, nonce — via `compute_spend_ref` and
`compute_approval_marker` (`crates/agent-policy-core/src/lib.rs:335-357`), and it
declares that account `#[account(init, pda = arg("marker_seed"))]`
(`agent_verifier.rs:296`). Three properties follow from that one line:

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
stamped (`agent_verifier.rs:526-534`, error 6018). Issuance is guarded by `init`;
consumption is guarded by the stamp. Both are in §7.

Two more bindings close the obvious ways round it. The account being paid is
checked against the account id the approval commits to
(`agent_verifier.rs:486-492`), so `recipient_id` is a real destination rather
than a label inside a hash. And `approve_spend` checks that the signer is the
owner **the policy record names** (`agent_verifier.rs:318-324`, error 6012) —
without that the agent signs its own approvals and steps over the threshold from
the other side, which makes the whole above-threshold path decorative. That the
owner now comes off the chain rather than out of the call is the strengthening
this deployment brought to that check: there is no `owner_id` argument left to
disagree with the record.

## 4. Existence is not consent

The approval account is checked to be owned by *this program*, not merely to be
non-default (`agent_verifier.rs:513-518`):

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

The same argument applies to the policy account, and `spend` makes the same check
(`agent_verifier.rs:383-388`, error 6002) — plus one more, added with the record:
the data must decode as a record this program wrote, or the spend is refused with
6016 (`agent_verifier.rs:554-562`). "The ceiling could not be read" fails closed
rather than decoding to something permissive. Empty data is an error too: an
anchored policy always carries a full record, so empty means the account was
made some other way.

Whether an outsider could in fact fund an address they do not control is a
separate question about LEZ's claiming rules — `scripts/deploy-agents.sh` records
that a claim on a public account the payer did not sign for is rejected as
`ClaimedUnauthorizedAccount` — and the point is that this program does not have
to depend on the answer.

Measured, not argued: the "funded by an outsider" rows in §7.

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

A second qualification, new with this deployment: **the policy account is
public.** Its address is a PDA of the agent's account id, so anyone who knows an
agent's id can read its owner and both its limits with `getAccount`. Under the
old scheme the address was a hash of the whole policy, which hid the limits from
anyone who had not already guessed them — weakly, since the search space of
plausible limits is small. This trades a little of that obscurity for the
property in §2, and the trade is deliberate: a ceiling that an observer can check
is worth more than one they have to brute-force.

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
- **Consume an approval the owner has already signed**, for the payment it
  names — **once**. The marker is stamped on use (§3), so an approval that has
  been paid out cannot be presented again (6018, §7).
- **Act as the agent everywhere else**: upload and delete under its storage
  identity, message its owner and its peers as itself, and publish a signed
  Agent Card advertising whatever it likes.

### They cannot

- **Anchor themselves a new ceiling.** This is the line that was in the "can"
  column of every previous version of this document, and closing it is what this
  deployment is for. There is one policy account per agent and `init` refuses a
  second (§2). Measured below, twice: once against the program this replaces,
  where it worked, and once against this one, where it does not.
- **Raise the limits of the policy the owner anchored.** `create_policy` is the
  only instruction that writes them and it can only run on an account that does
  not yet exist; `spend` writes the running total and nothing else.
- **Borrow another agent's policy.** The policy account's address is derived from
  the paying account, so presenting somebody else's is a PDA mismatch before the
  body runs (§2). The old 6013 check is gone because the disagreement it reported
  can no longer be constructed.
- **Exceed `per_period` inside a period by splitting the payment up.** The
  running total is on chain, in the policy account's data, and the program that
  owns that account is the only thing that can write it (§2). Error 6006,
  measured in §7 and readable on the chain right now — see below.
- **Reset the period early to get a fresh budget.** The period is named by the
  caller, because no program on this chain can read the block height, but naming
  it is not choosing it: it must be a multiple of `period_blocks` (6014), it may
  not be older than the period the record holds (6015), and the transaction is
  pinned to `[window_start, window_start + period_blocks)` through
  `ProgramOutput`'s block validity window, so naming a *future* period yields a
  transaction that no current block will include (`OutOfValidityWindow`,
  `lee/state_machine/src/validated_state_diff/mod.rs:202-208` public,
  `:393-398` privacy-preserving). Sliding forward, replaying backwards and
  jumping ahead are each refused by a different one of those three.
- **Forge an owner approval** for a payment above the ceiling (§4), sign their
  own approval (6012, §3), point an approved payment at a different recipient
  (6011, §3), or present a spent one twice (6018, §3).
- **Spend from the owner's account, or from another agent's.** The payer is
  whichever account signs, and they hold only the agent's keys.
- **Pass the local process check by lying to it.** The C++ envelope check fails
  closed: an unknown period total or an unknown envelope forces the approval
  path rather than the autonomous one (`module/src/wallet_skills.cpp:394-404`).
  That is a property of the honest process only — see §8.

### The attack, and the transactions on both sides of the fix

The bypass is stated as the attacker would run it, because that is the only form
in which it can be checked. The victim below is the **messaging agent**,
`GpRdooEW…Zpe5FS`, whose owner anchored `per_tx = 25`. It held 65 LEZ.

Against the program this release replaces, `8c87cc9b…2d20ebbe`:

1. The agent's **own public pay account**, `Dxh7ZLHF…fpEwD` — an account the
   attacker holds along with the agent's key — calls `create_policy` naming
   itself as `owner_id`, the compromised agent as `agent_id`, and
   `per_tx = per_period = u128::MAX`. The owner check passes: the signer really
   is the owner it names.

   [`e530e0ba…399d462d`](https://explorer.testnet.lez.logos.co/transaction/e530e0ba9a49c4ebacbfeaeac8fff3376f8bece24b71cb8f985b70c5399d462d)
   — **accepted, included in a block.**

2. The agent then calls `spend` for **its entire 65 LEZ in one transaction**,
   against a ceiling its owner had set at 25. The agent check passes: the payer
   really is the agent that policy names.

   [`7fc6c9af…f49a022228`](https://explorer.testnet.lez.logos.co/transaction/7fc6c9af06e590c7553af9d3090384e88a2780e38995117ca4e091f49a022228)
   — **accepted, included in a block.** The recipient's balance moved by 65.

The same two steps were run against the storage agent as well —
[`d7498d65…1fbdd09b`](https://explorer.testnet.lez.logos.co/transaction/d7498d65a77e9e0d550bf89ae16127d5bb328d42643c6eacd3e74a611fbdd09b)
and
[`0a9ac12c…15b0e170`](https://explorer.testnet.lez.logos.co/transaction/0a9ac12ce1442cd6d33c7eac02df8a120f13e558273e6a91a4289f4f15b0e170)
— and accepted there too. Four transactions, all `Halted(0)`, all on the public
testnet. This was never a theory about the source.

Against **this** program, `a780003b…8576841e`, step 1 is the identical call:

```
create_policy --owner Public/Dxh7ZLHF…fpEwD --agent-id <the messaging agent>
              --per-tx  340282366920938463463374607431768211455
              --per-period 340282366920938463463374607431768211455
              --period-blocks 1000
```

`a01ace40b839f89b7b662b5532521716bd0906fbeef73ed15dae8c6b2cfd5352` — submitted,
given a hash, **never included**. The same call aimed at the storage agent,
`ecfeb924e26aa563b3fa6948434542843c571a2a49b3510422321d867ec04eec`, likewise.
The policy account those calls address already exists, holds the owner's own
record, and `init` refuses it.

**Neither of those two hashes proves anything by itself, and they are not offered
as proof.** A refused transaction, a pending one and a hash nobody ever sent all
read `null` from `getTransaction`, which is exactly what `demo.sh`'s
cannot-exist-hash control demonstrates. A refusal is not an event on this chain.
The refusal is shown where it *can* be shown — by running the committed binary,
the bytes that hash to the deploy transaction, against both steps of the attack
in `crates/agent-verifier-adversarial`. That suite now runs the anchor **and**
the follow-up spend, in all three shapes that were accepted above, and requires
the balance to be out of reach at the end of each. §7 has the table.

### What is left, stated plainly

Three things, none of them hidden by the above.

**Anchoring is first-writer-wins, and the first writer needs no key.** This is
the sharpest thing left and it deserves to be stated at full strength rather
than as "the owner should anchor early". `create_policy` declares two accounts:
the policy account and a signer, recorded as the owner. **The agent's account is
not among them.** It is never declared, never read and never asked to sign —
`agent_id` is a `[u8; 32]` argument whose only job is to seed the address
(`agent_verifier.rs:243-249`). So anchoring a policy over somebody else's agent
requires no key at all, only the agent's *public id*, which this repository
publishes in `artifacts/agents.tsv` and inside every signed Agent Card.

Two consequences, and neither is theoretical:

- Whoever anchors first owns that agent's envelope for the life of the agent
  identity. If a third party gets there before the owner, the honest owner's
  anchor is refused `AccountAlreadyInitialized` and stays refused.
- `per_tx = 0` makes the same call a permanent denial of service: the agent can
  never spend unattended again, and no `close` instruction exists to undo it
  (LEZ rule 4 forbids changing an account's program owner, `ModifiedProgramOwner`,
  `program/mod.rs:697-703`, so an account this program has claimed is this
  program's forever).

What this is *not* is the attack §6 closes. That one is an attacker with the
agent's key turning a stolen process into an unlimited spender, and it is now
impossible rather than merely detectable. This is a race for an address, it
grants no spending authority to the racer, and it moves no money. It is
nonetheless worse than it needs to be — the predecessor design at least required
the agent's key to anchor over the agent — and the honest reading is that this
release traded a theft for a griefing surface.

The mitigation as shipped is procedural: `scripts/deploy-agents.sh` anchors
immediately after funding, before the agent id is published anywhere, and
records the policy account it anchored so that a reader can check it. The
structural fix is a second signature — the agent designating, in its own
transaction, which account may anchor for it — and it is being built rather than
argued about here.

**A policy cannot be changed.** One account per agent, written once. An owner who
wants a different envelope needs a new agent identity. That is a real cost, paid
deliberately: an `update_policy` instruction would be a second writer of the
limits, and a second writer is the thing that was just removed. It is also
strictly better than the previous design, where an owner *could* anchor a
second, looser policy — and so could anyone else.

**The rate limit is still a rate limit.** `per_period` bounds a window, not a
lifetime. A patient attacker inside the envelope still drains the account across
enough periods, publicly, one anchored ledger entry at a time.

### The period ledger, on the chain, right now

The per-period ceiling is the claim this document got wrong for four
deployments, so it is the one stated with a command rather than a sentence. The
blockchain agent's policy account is `Coxz1Cmf…Zk5rgM`, the PDA of that agent and
nothing else:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
assert len(d)==97 and d[0]==1, 'not a record this program wrote'
le=lambda a,b: int.from_bytes(d[a:b],'little')
print('owner bytes', d[1:33].hex())
print('per_tx', le(33,49), 'per_period', le(49,65), 'period_blocks', le(65,73))
print('period', le(73,81), 'spent', le(81,97))"
# owner bytes 9e54ba23…  (= Bf4MG9AW…s6Du, the account that signed create_policy)
# per_tx 200 per_period 1000 period_blocks 1000
# period 8000 spent 50
```

Two settlements of 25 each — `4e3a3454…a490ddb1` and `7cad4fbd…7168f019` — took
it 0 → 25 → 50, in a policy whose anchored `per_period` is 1000. Nothing the
agent sends can lower that number, or the two limits sitting next to it in the
same 97 bytes, because the write is the program's and rule 6 forbids anyone
else's. At 1000, `spend` refuses with 6006 until block 9000, when the window
rolls and the budget starts again.

The layout is `version || owner(32) || per_tx(16) || per_period(16) ||
period_blocks(8) || window_start(8) || spent(16)`, every integer little-endian
(`agent-policy-core/src/lib.rs:256-311`), and that is the whole format.

## 7. Checking it yourself

Everything below is the deployed binary — `artifacts/programs/agent_verifier.bin`,
the bytes that hash to deploy transaction `a780003b…8576841e`, ImageID
`12fa95d9…b578c9d8` — executed under the risc0 executor with inputs written in
the order the sequencer writes them. Run it:

```bash
cd crates/agent-verifier-adversarial && cargo run --release
```

Every refusal is paired with the honest call it differs from in one field. A
table that only ever refuses says nothing about what is accepted, and a program
that refuses everything would pass it.

| Input | Result |
|---|---|
| `create_policy` for an agent nobody has anchored | **accepted**, `Halted(0)`, and the record it writes is printed |
| an attacker anchors an **unlimited** policy over an agent that has one, naming itself as owner | refused, `AccountAlreadyInitialized` |
| …the same, signed by the agent's own program-owned public pay account | refused, `AccountAlreadyInitialized` |
| …the same, signed by the compromised agent itself | refused, `AccountAlreadyInitialized` |
| `create_policy` whose policy account is not the PDA for the agent it names | refused, `PdaMismatch` |
| `create_policy` with `period_blocks = 0` | refused, `6017` |
| `spend` inside the envelope | **accepted**, `Halted(0)` |
| `spend` of the agent's whole balance, as it would be under an unlimited policy | refused, `6005` — "the spend needs an owner approval" |
| `spend` where a **different** agent presents this agent's policy account | refused, `PdaMismatch` |
| `spend` against a policy account at the right address that this program never created | refused, `6002` |
| `spend` against a policy account holding data this program did not write | refused, `6016` |
| `spend` that would carry the period total past `per_period` | refused, `6006` |
| …the same spend in the next period, where the budget starts again | **accepted**, `Halted(0)` |
| `spend` naming a period that is not a multiple of `period_blocks` | refused, `6014` |
| `spend` naming a period older than the one the record holds | refused, `6015` |
| `approve_spend` signed by the compromised agent | refused, `6012` |
| …signed by the agent's own public pay account | refused, `6012` |
| …signed by the owner the record names | **accepted**, `Halted(0)` |
| `approve_spend` by the owner where the marker does not commit to the amount | refused, `6003` |
| `spend_approved` with an approval account **funded by an outsider** at the right address | refused, `6007` |
| `spend_approved` on the approval this program created | **accepted**, `Halted(0)` |
| `spend_approved` presenting an approval that has already been stamped | refused, `6018` |
| `spend_approved` where a different agent presents an approval granted for this one | refused, `PdaMismatch` |

and then, separately, the two-step attack end to end — anchor, then spend the
whole balance — in each of the three shapes that the previous binary accepted on
chain, followed by three consecutive spends that walk the period total 200 → 400
→ 600 out of the record the guest itself wrote.

**Two refusals in that table have no error code, and that is worth knowing before
you integrate against it.** `AccountAlreadyInitialized` and `PdaMismatch` come
from the macro's account validation, which the generated dispatcher consumes with
`.expect("account validation failed")` — so the guest panics with the variant's
`Debug` and no number:

```
account validation failed: AccountAlreadyInitialized { account_index: 0 }
```

`SpelError::error_code()` would call that 1002, but 1002 appears nowhere in what
the executor prints. The refusal that closes the anchoring bypass is one of
these, so the harness matches the variant name rather than a code, and says so.

The codes that *are* numbered have their own trap: the `6001…6018` declared in
the guest reach a caller **offset by 6000**, because `SpelError::error_code`
returns `6000 + code` for a custom error
(`vendor/spel/spel-framework-core/src/error.rs:119`). What the log actually says
is `Program error [12005]: Program error 6005: …`. Matching the bracketed number
would match `6012` against `12012` and pass a case that halted for an entirely
different reason.

**One row that used to be here and was wrong.** Earlier versions of this table
claimed that `spend_approved`, on a valid approval, is refused with `6006` if it
would burst the period total. It is not: executed against the binary it is
**accepted**. That is deliberate rather than an oversight — an approved payment
carries the owner's authorisation naming this recipient, this amount and this
nonce, and it does not draw on the *unattended* budget, which is the thing
`per_period` bounds (`agent_verifier.rs:434-441`). Making an owner's own approval
refusable because the agent had been busy would buy nothing, since the marker is
single-use anyway. But the document asserted a refusal that does not happen, and
the correction belongs in the open rather than in a silent edit.

**And one row that used to be here and was worse.** Until this release the
adversarial suite carried the case

> `create_policy` — "the same call, naming the account that actually signs" — **accepted**

as its *benign control*, `expect: None`. That call is step 1 of the attack in §6:
an attacker anchoring an unlimited policy over somebody else's agent while
honestly naming itself as the owner. The suite whose whole purpose was to prove
the bypass closed had the bypass written down as required behaviour, and never
followed it with the `spend` that empties the account. Both are fixed: that call
must now refuse, and the follow-up spend runs as a regression.

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
  chains into the transfer program rather than moving balance itself; rule 6
  forbids a data write to an account the executing program does not own, which is
  what makes the policy record a record; rule 8 requires total balance to be
  preserved across a transaction. These are LEZ's checks, not ours, and we verify
  them by observing that transactions violating them do not land.
- **`#[account(init)]` means what it says.** The macro refuses any account whose
  pre-state is not `Account::default()`
  (`vendor/spel/spel-framework-macros/src/lib.rs:1396-1410`), and the pre-states
  are built by the state machine from actual chain state rather than supplied by
  the caller. The whole of §2 rests on that one comparison, which is why it is
  named here rather than assumed.
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
