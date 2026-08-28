# The security model

What the agent can do without asking, what it cannot, and what someone who
steals its keys gets. Every claim below names the instruction that enforces it
and the error code the chain returns, and the ones that can be executed have
been executed — against the deployed binary, and in two cases against the
deployed *chain*.

**Which program this describes.** `artifacts/programs/agent_verifier.bin`,
ImageID `778a9341…e670c4661`, deploy transaction `697746f5…cb5370bf`, block
8839. That matters more than it usually would: every account this program owns
is a PDA of the program id, so a rebuild moves all of them, and a claim about
"the program" is only meaningful against one ImageID. If the binary and this
document ever disagree, the binary is right — recompute its hash with the
command in [`DEPLOYMENT.md`](DEPLOYMENT.md) and check it against the chain, or
run [`../scripts/verify-deployment.sh`](../scripts/verify-deployment.sh), which
does exactly that and exits non-zero when this file has gone stale.

The design in one sentence: **an agent's spending authority is an account that
only two consenting parties can create, and only one of them can change.** The
rest of this document is what that buys, and — the longer half — what it does
not.

## 1. The envelope

The owner fixes three numbers when the agent is deployed: the largest single
transfer it may make unattended (`per_tx`), the largest total it may move in a
window (`per_period`), and the window's length in blocks (`period_blocks`).

| Action | Who has to agree |
|---|---|
| A payment at or below `per_tx` | nobody — the agent alone |
| A payment above `per_tx` | the owner, on an approval account naming that exact payment, redeemed before it expires |
| Binding an agent to a policy at all | **both** — the agent signs `claim_agent`, the account it names signs `create_policy` |
| Changing a limit on an existing policy | the owner the record names, through `update_policy` (6012) |
| A payment above the ceiling with no approval | refused by the chain, error 6005 |

## 2. Anchoring takes two signatures, and that is the whole property

An agent's policy account is `PDA(program, ["agent-policy/v1", agent_id])` — the
agent, and nothing else. That decides *where* a policy goes and says nothing
about *who* may put one there, and the deployment before this one answered the
second question with "anybody".

It declared two accounts on `create_policy`: the policy account, and a signer it
recorded as the owner. The agent's own account was never declared, never read
and never asked to sign; `agent_id` was a free `[u8; 32]` argument the body
discarded. So anchoring a policy over somebody else's agent needed **no key at
all**. It needed the agent's public id — which this repository publishes in
[`artifacts/agents.tsv`](../artifacts/agents.tsv) and inside every signed Agent
Card, because the card's `url` *is* the agent's account id.

That is on the chain rather than in an argument. Two transactions, the same
call, the same agent, the same limits, from accounts that have never held that
agent's key:

| Program | Signer | Result |
|---|---|---|
| `a780003b…` (superseded) | `RZmSLJAB…` | **accepted**, block 8869 — [`eedb3caf…`](https://explorer.testnet.lez.logos.co/transaction/eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7) |
| `697746f5…` (current) | `Acissmag…` | submitted, never included — `60de3fc6…`, error 6020 |

The accepted one is still readable, which is the point — this does not rest on a
missing transaction:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["5QAVJAMHkpLnAMht3bonFijyApPZfAccHAFbzByNq8VV"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
print('owner ', d[1:33].hex())
print('per_tx', int.from_bytes(d[33:49],'little'))"
# owner  064afcfc304a27f737331a4105b8dc967ae892403449ae80023811405344aa4e
# per_tx 340282366920938463463374607431768211455
```

Under the superseded program a stranger owns that agent's only policy account,
for the life of the agent's identity, with an unlimited ceiling — and the honest
owner's own anchor would then have been refused `AccountAlreadyInitialized` at
the only address the agent has, permanently. `per_tx = 0` instead of `u128::MAX`
makes the identical call a denial of service rather than a theft, and costs the
attacker nothing either way.

**The fix is consent, and it is two signatures in two transactions.**

- `claim_agent` — the **agent** signs. It writes an owner claim into
  `PDA(program, ["agent-owner/v1", agent])`, an address derived from the
  *signing* account, so there is no `agent_id` argument to substitute. The
  account holds `02 || owner`, 33 bytes, and `init` makes it once and for all.
  Designating the all-zero account is refused (6022): no key produces that id, so
  a policy keyed to it could never be signed by anybody.
- `create_policy` — the **designated owner** signs. It requires the claim account
  to be owned by this program (6019 if not — anyone can fund an address, and an
  account this program never created is not a claim), decodes it, and refuses
  any signer that is not the id the agent named (6020).

A third party holds neither key. It cannot claim an agent it cannot sign for —
the claim address comes from the signer, so its claim lands at its *own* address
and not the victim's — and it cannot anchor over an agent that has designated
somebody else.

**Why two instructions rather than two signers on one.** `spel` signs only for
accounts an instruction declares as signers, and only with keys the single
wallet home it is pointed at holds. One instruction demanding both signatures
would demand the owner's key and the agent's key in the same wallet, which is
exactly the arrangement §10 assumes away. Two single-signer instructions keep
the agent's key on the agent's node and the owner's key with the owner. Removing
a signer is not an option either: an instruction with no declared signer produces
a transaction with an empty witness set, which is how `create_policy` became
permissionless in the first place.

**Read both accounts for a live agent.** This is the check that ties
[`artifacts/agents.tsv`](../artifacts/agents.tsv) to the chain, and it is a byte
comparison rather than a derivation:

```bash
# 1. who the storage agent said may anchor over it — the claim_account column
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["EZSN69njgBixwniExyhRrjri1xUTX6iN7xxhcvG4Vvie"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
assert len(d)==33 and d[0]==2, 'not an owner claim'
print('owner', d[1:33].hex())"
# owner 181eee76d02339bbe8ce7abee778942d80b2546a8f204e19940311ff5bd46214
#     = 2dA9APZgzcoX65YhNMJmsDC2v838ufLSjPyUdMknWoZd, the `owner` column

# 2. what that owner then anchored — the policy_account column
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
print('record_prefix', d[:73].hex())"
# must equal the record_prefix column of artifacts/agents.tsv, byte for byte
```

`record_prefix` is the first 73 bytes of the policy record — version, owner,
`per_tx`, `per_period`, `period_blocks` — which is the whole of the part that
does not move. Bytes 73..97 are the running total, which does. That column
exists because the manifest had lost the ability to tie its advertised limits to
the chain: `policy_hash` used to commit to them and could be recomputed, and
`policy_account` commits only to the agent.

## 3. Why the limit is an account and not an `if`

A threshold checked inside the agent is worth nothing: the agent holds its own
keys on a remote node, so whoever takes the process takes the check.

So the policy is not a number in the agent's config. It is 97 bytes of account
data at an address the agent cannot choose:

```
version(1) || owner(32) || per_tx(16) || per_period(16) || period_blocks(8)
           || window_start(8) || spent(16)
```

little-endian throughout (`crates/agent-policy-core/src/lib.rs`,
`PolicyRecord`). LEZ rule 6 (`UnauthorizedDataModification`,
`lee/state_machine/core/src/program/mod.rs:718-728`) refuses a data write to an
account from any program but its owner, and this program is the owner. So the
ceiling cannot be edited by the agent and the running total cannot be forged by
anyone.

Three bindings make it stick, and all are exercised in §8:

- **The payer selects the policy, not the caller.** `spend` and `spend_approved`
  derive the policy account's address from the account that is *paying* — `pda =
  [literal("agent-policy/v1"), account("agent")]` — so there is no `agent_id`
  argument to lie about, and an agent presenting another agent's policy fails
  the macro's own PDA check before the body runs.
- **The policy account must be owned by this program** (6002). Merely existing at
  the right address proves nothing, because anyone can fund an address.
- **Its data must decode** (6016). Empty data is not "a fresh ledger": an
  anchored policy always carries a full record, so empty means the account was
  made some other way.

## 4. Losing the address is not permanent

LEZ rule 4 forbids changing an account's program owner at all
(`ModifiedProgramOwner`, `lee/state_machine/core/src/program/mod.rs:697-703`).
An account this program has claimed is this program's forever; no `close`
instruction can exist, and none does. So if the *terms* were immutable too, one
wrong anchor would burn the agent identity — and under the previous deployment a
stranger could inflict exactly that, for free, on any agent whose id it could
read.

The way back is therefore in the record rather than in the address:

- `update_policy` — the owner **the record names** signs (6012), and re-fixes
  `per_tx`, `per_period` and `period_blocks` in place. `period_blocks = 0` is
  still refused (6017).
- The agent does **not** sign it. An owner who has to reach a compromised agent
  for a signature cannot rein it in, and the agent already consented to this
  owner deciding its terms when it signed `claim_agent`.
- The running total is carried through untouched. Raising a ceiling is not
  forgiveness for what has already been spent under the old one, and lowering it
  must not be softened by a reset either: the ledger belongs to the period, not
  to the terms.

`update_policy` with `per_tx = 0` is the brake — the agent may then spend nothing
unattended and every payment needs an owner approval. **On the three agents
published here that brake cannot be pulled**, and the reason is in
`docs/limitations.md`: each of their owners spent its pristine state on
`create_policy`, so all three now read `nonce: 1` with the default program owner
and cannot sign again. Measured, not inferred — an `auth-transfer init` against
one of them was submitted and never included. The mechanism below is the
program's; the three shipped agents cannot reach it, and a fourth owner
provisioned before anchoring can. That state is a policy
rather than a mistake, which is why `create_policy` accepts `per_tx = 0` too, and
why accepting it is safe now and was not before: under the previous program the
identical value, from any stranger, was a permanent denial of service; here only
the designated owner can set it and only the owner can undo it.

`period_blocks = 1` is accepted and is worth understanding before choosing it. A
one-block period means `per_period` bounds a single block rather than a horizon,
so it stops constraining the aggregate at all, and the transaction must land in
exactly the block it names. Both are the owner's call to make; the program
refuses only 0 (6017), because a policy with no period cannot be accounted
against — `authorize` would have nothing to align a window to.

**What stays irreversible** is the agent's own designation of its owner. It has
to: an agent that could re-designate could name itself and then anchor its own
unlimited policy, which is the attack. The cost is bounded — a shielded account
costs nothing to create, and the agent's balance is held by the transfer program
and moves on the agent's own key, so nothing is trapped — and the remedy is a new
agent identity. No third party can force that remedy on anybody.

## 5. Above the threshold: an approval that names one payment, and dies

`approve_spend` creates an account whose address is derived from the payment
itself — agent, recipient, amount, nonce, via `compute_spend_ref` and
`compute_approval_marker` — and declares it `#[account(init, pda =
arg("marker_seed"))]`. Three properties follow from that one line:

- **Single issue.** `init` refuses to overwrite, so the same approval cannot be
  created twice.
- **Not transferable to another payment.** Change the recipient, the amount or
  the nonce and the seed changes, so the approval is for an account that does
  not exist.
- **Not replayable.** The nonce is what stops "send 100 to Bob", approved once,
  authorising the same transfer forever.

`init` gives single *issue*, which is not single *use*, and the difference was a
real defect: a marker that exists and is never touched authorises the same
payment on **every** later transaction that presents it. So `spend_approved`
stamps the marker as it consumes it and refuses one already stamped (6018).

**And an approval now expires.** This is the other half, and it was missing until
this deployment. A marker used to be valid in every block, forever: an owner who
approved one payment had written a bearer instrument, redeemable the day the
agent's key was stolen, months later, against a period ledger the approved path
does not draw on — and unrevocable, since `approve_spend` is `init` and the
marker cannot be re-issued to cancel it. The owner now names `expiry_block`; it
is written into the marker (`ApprovalRecord`: `version || expiry_block ||
spent`); and `spend_approved` pins the transaction's own block validity window to
`[0, expiry_block)`. So an expired approval is not something this program
refuses — it is a transaction no block will include (`OutOfValidityWindow`).
`expiry_block = 0` is refused at issue (6021), because `[0, 0)` is an empty range
and an approval that cannot be pinned to a window is the unbounded one again.

Two more bindings close the obvious ways round it. The account being paid is
checked against the account id the approval commits to (6011), so `recipient_id`
is a real destination rather than a label inside a hash. And `approve_spend`
checks that the signer is the owner the policy record names (6012) — without that
the agent signs its own approvals and steps over the threshold from the other
side, which makes the whole above-threshold path decorative.

## 6. Existence is not consent

The approval account is checked to be owned by *this program*, not merely to be
non-default:

```rust
if approval.account.program_owner != ctx.self_program_id {
    return Err(SpelError::custom(E_APPROVAL_NOT_ANCHORED, ...));
}
```

The weaker check — "an account exists at that address" — would make consent a
function of an account existing rather than of the owner having signed. The
marker seed is derivable by anyone who knows the payment, so anyone who could get
*some* account to exist at that address would have manufactured an approval:
credit it, and the transfer program claims a previously-default account on the
way in. Requiring this program to be the owner means the account can only have
come from `approve_spend`, which only runs when someone signed it.

The identical argument applies to the owner claim of §2 (6019) and to the policy
account of §3 (6002). Three accounts, one rule: an account this program did not
create carries no statement from anybody.

Measured, not argued: the "approval merely funded" and "claim merely funded" rows
in §8.

## 7. What someone who steals the agent's key can do

This is the half that matters, so it is stated in full rather than implied.

### The ceiling on all of it

Start with the thing no policy program on this chain can do anything about. **An
account holder can always call the program that owns its balance.** The agent's
LEZ is held by LEZ's authenticated transfer program — measured, not assumed:

```
Private/9KdQSJ2t…  {"program_owner":"J8otq1J8Zpjhhpp6FPfhFtWKTCkLjthdk12cwHiMZCTB"}
```

and that program's `transfer` asserts only that the sender is authorized
(`lez/programs/authenticated_transfer/src/main.rs`). So whoever holds an agent's
key can move that agent's balance by calling the transfer program directly, with
this program nowhere in the transaction. Nothing here prevents that and nothing
here claims to.

What this program bounds is what the agent moves **through it** — which is what
its skills do, what its Agent Card advertises, and what every settlement in this
repository is. The honest statement of the guarantee is therefore in two parts:
*a compromised agent cannot use the policy path to exceed the policy*, and *a
party who is not the agent cannot touch the policy at all*. Making the first part
unconditional needs the agent's balance to live in an account this program owns,
which is a different design; it is written up in
[`limitations.md`](limitations.md) rather than glossed here.

### Inside the policy path, they can

- **Spend up to `per_tx`, to any address they like, unattended.** There is no
  allowlist. The policy binds *how much*, never *to whom* — recipient binding
  exists only on the approved path (§5).
- **Repeat that up to `per_period` in each period, and again in the next one.**
  The period ceiling is real, but it is a rate limit, not a total: a patient
  attacker still drains the account over enough periods. What it buys is time,
  and time is what an owner watching the chain needs.
- **Consume an approval the owner has already signed**, for the payment it
  names — **once**, and **only before its expiry block** (§5).
- **Act as the agent everywhere else**: upload and delete under its storage
  identity, message its owner and its peers as itself, and publish a signed
  Agent Card advertising whatever it likes.

### Inside the policy path, they cannot

- **Anchor themselves a new ceiling.** This is the one that was open in every
  previous deployment and is closed here. The agent's policy account already
  exists and `init` refuses a second; its owner claim already exists and `init`
  refuses a second; and `create_policy` refuses any signer that is not the id
  that claim names (6020). Executed against the binary in §8, and against the
  chain in §2.
- **Re-fix the limits.** `update_policy` compares its signer to the owner the
  record names (6012). A compromised agent signing for itself is refused by the
  same record it would have had to overwrite.
- **Raise the limits by presenting different numbers.** There are no limits in
  `spend` at all; they are read out of the account.
- **Exceed `per_period` inside a period by splitting the payment up.** The
  running total is on chain, in the policy account's data, and the program that
  owns that account is the only thing that can write it. Error 6006.
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
- **Borrow another agent's policy.** The policy address is derived from the
  paying account, so presenting somebody else's fails the PDA check.
- **Forge an owner approval** (§6), sign their own (6012), point an approved
  payment at a different recipient (6011), present a spent one twice (6018), or
  redeem one after its expiry block.
- **Spend from the owner's account, or from another agent's.** The payer is
  whichever account signs, and they hold only the agent's keys.
- **Pass the local process check by lying to it.** The C++ envelope check fails
  closed: an unknown period total or an unknown envelope forces the approval path
  rather than the autonomous one. That is a property of the honest process only —
  see §9.

### And what a party who is NOT the agent can do

Nothing. That is what this deployment is for. Knowing an agent's id — which is
public by design, because it is the address other agents pay it at — no longer
lets a stranger anchor a policy over it, brick it with `per_tx = 0`, or take the
address the honest owner needs. Both accounts require a signature only the
agent's holder can produce, and the second one additionally requires a signature
only the agent's chosen owner can produce.

### The module shells out, and what keeps that from being a hole

`AgentModuleImpl::runConfiguredCommand`
(`module/src/agent_module_plugin.cpp`) runs an external program through `popen`,
and four settings name what it runs: `card_signer`, `pay_signer`,
`approved_pay_signer` and `policy_source`. Those settings are interpolated into
a shell. `meta.configure` can write all four. Stated plainly because anybody
auditing this will find the `popen` and should not have to reconstruct why it is
safe.

**What is not interpolated is the input.** The signing input contains the
peer-visible Agent Card, which carries every registered skill's name, and a
payment instruction carries a recipient that came out of a stranger's card. Both
are attacker-influenced, and both go in on **stdin**, through a file whose name
comes from `mkstemp` — not on the command line. A predictable name in a
world-writable directory would let anybody swap what gets signed, so there is no
predictable name, and the file is unlinked on every path out.

**What keeps the settings themselves safe is that nothing on the network can
reach them.** `meta.configure` is a skill, and skills are only reachable through
`AgentModuleImpl::invoke()`, which is callable only by a process that has loaded
this module — the Logos app, `logos_host`, or the CLI. Messages arriving over
Logos Messaging do not reach it: `module/src/owner_channel.cpp` never calls
`invoke`, and an inbound A2A task request is not dispatched to the skill it names
(see [`limitations.md`](limitations.md)). So the party who can set `card_signer`
to arbitrary shell is the party who can already run arbitrary code in that
process, because they are running it.

That is a trust boundary and not a proof of absence. If inbound A2A dispatch is
ever added — it is the obvious next feature — then `meta.configure` becomes
reachable by whoever can send this agent a task, and these four settings turn
into remote code execution with the agent's key on the other end. **The dispatch
step and an allow-list for these keys have to land in the same change.**

### The period ledger, on the chain, right now

The per-period ceiling is the claim this document got wrong for four
deployments, so it is stated with a command rather than a sentence. Note the
record is **97 bytes**, not the 24 an earlier version of this document described:
that layout held only `window_start` and `spent`, because the limits used to live
in the address.

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
assert len(d)==97 and d[0]==1, 'not a record this program wrote'
le=lambda a,b: int.from_bytes(d[a:b],'little')
print('owner       ', d[1:33].hex())
print('per_tx      ', le(33,49))
print('per_period  ', le(49,65))
print('period      ', le(65,73), 'blocks')
print('window_start', le(73,81))
print('spent       ', le(81,97))"
```

That is the messaging agent's policy account, the payer in
[`../scripts/a2a-task.sh`](../scripts/a2a-task.sh). Nothing the agent sends can
lower `spent`, because the write is the program's and rule 6 forbids anyone
else's; at `per_period` the next `spend` refuses with 6006 until the window rolls
and the budget starts again.

## 8. Checking it yourself

Everything below is the deployed binary — `artifacts/programs/agent_verifier.bin`,
the bytes that hash to deploy transaction `697746f5…cb5370bf`, ImageID
`778a9341…e670c4661` — executed under the risc0 executor with inputs written in
the order the sequencer writes them. Run it:

```bash
cd crates/agent-verifier-adversarial && cargo run --release
```

Every refusal is paired with the honest call it differs from in one field. A
table that only ever refuses says nothing about what is accepted, and a program
that refuses everything would pass it.

| Input | Result |
|---|---|
| `claim_agent` by the agent, naming its owner | **accepted** — the claim account holds that id |
| `claim_agent` by a stranger, naming itself, over another agent | refused, `PdaMismatch` — the claim address comes from the signer |
| `claim_agent` a second time for the same agent | refused, `AccountAlreadyInitialized` |
| `claim_agent` naming the all-zero account | refused, `6022` |
| `create_policy` with `per_tx = u128::MAX` over an **unclaimed** agent | refused, `6019` — *the call the previous suite asserted as correct* |
| …the same with `per_tx = 0`: the denial of service, not the theft | refused, `6019` |
| `create_policy` by a stranger over an agent that claimed somebody else | refused, `6020` |
| …signed by the agent's own public pay account | refused, `6020` |
| …signed by the compromised agent itself | refused, `6020` |
| `create_policy` against a claim account an outsider merely funded | refused, `6019` |
| `create_policy` against a claim holding data this program did not write | refused, `6016` |
| `create_policy` by the owner the agent designated | **accepted**, `Halted(0)` |
| …with `per_tx = per_period = 0`, which is a policy and not a mistake | **accepted** |
| `create_policy` a second time for that agent | refused, `AccountAlreadyInitialized` |
| `create_policy` whose policy account is not the PDA for the agent it names | refused, `PdaMismatch` |
| `create_policy` with `period_blocks = 0` | refused, `6017` |
| `update_policy` to unlimited, by a stranger | refused, `6012` |
| …by the compromised agent itself | refused, `6012` |
| `update_policy` to `per_tx = 0` by the owner the record names | **accepted** — ledger carried through unchanged |
| …then the agent spends 1 under the envelope its owner froze | refused, `6005` |
| `update_policy` against a policy account this program never created | refused, `6002` |
| `update_policy` leaving the policy with no period | refused, `6017` |
| `spend` inside the anchored envelope | **accepted**, `Halted(0)` |
| `spend` of the agent's whole balance | refused, `6005` |
| `spend` presenting another agent's policy account | refused, `PdaMismatch` |
| `spend` against a policy account nobody created | refused, `6002` |
| `spend` against data this program did not write | refused, `6016` |
| `spend` that would carry the period total past `per_period` | refused, `6006` |
| …the same spend in the next period | **accepted**, `Halted(0)` |
| `spend` naming a period that is not a multiple of `period_blocks` | refused, `6014` |
| `spend` naming a period older than the ledger's | refused, `6015` |
| `spend` from an agent account no program owns | refused, `6010` |
| `spend` inside the envelope and larger than the agent's balance | refused, `6008` |
| `spend` into a recipient one payment short of overflowing `u128` | refused, `6009` |
| `approve_spend` signed by the agent rather than the owner | refused, `6012` |
| …signed by the agent's own public pay account | refused, `6012` |
| …signed by the owner the record names | **accepted**, `Halted(0)` |
| `approve_spend` whose marker does not commit to the amount | refused, `6003` |
| `approve_spend` with no expiry — the old bearer instrument | refused, `6021` |
| `spend_approved` on an approval **funded by an outsider** | refused, `6007` |
| `spend_approved` on a real, unexpired approval | **accepted**, `Halted(0)` |
| `spend_approved` presenting an approval already stamped | refused, `6018` |
| `spend_approved` on an approval in the previous deployment's shape (empty data) | refused, `6016` |
| `spend_approved` pointed at a recipient the approval does not name | refused, `6011` |
| `spend_approved` by a different agent than the approval names | refused, `PdaMismatch` |

After the table the suite runs the whole attack in sequence, three times, once
per shape an earlier program accepted: the stranger's anchor is refused, **the
honest owner then anchors successfully** — which is the "losing the race is not
permanent" half — the agent's whole-balance spend is refused 6005 against that
policy, and the attacker's self-signed approval for 9,999,999 is refused 6012.

**Every code this program declares is now in that table.** Four of them were not
until this pass, and the gap is worth naming rather than quietly closing: 6008,
6009 and 6010 are the three refusals `delegated_transfer` raises — the guard the
guest's own comment says exists "so that an agent that cannot afford a task gets
error code 6008 instead of a guest panic inside a program it did not write" — and
6011 is the one this document lists in §7 among the things a key-holder cannot
do, "point an approved payment at a different recipient". All four were
assertions about branches no case reached. They are executed now, each as the
accepted call above it with exactly one account field changed: 19 declared codes,
19 exercised against the deployed binary.

One trap for anyone integrating against these numbers: the codes declared in the
guest as `6002…6022` reach a caller **offset by 6000**, because
`SpelError::error_code` returns `6000 + code` for a custom error
(`vendor/spel/spel-framework-core/src/error.rs:119`). What the log actually says
is `Program error [12005]: Program error 6005: …`. Matching the bracketed number
would match `6012` against `12012` and pass a case that halted for an entirely
different reason. Codes `6001`, `6004` and `6013` are retired rather than reused:
all three existed because the caller once chose the policy account's address, and
the disagreements they reported cannot be expressed any more.

Refusals from the macro's own account validation carry **no numeric code** at
all — the generated dispatcher panics with the `Debug` of the `SpelError`
variant — so `AccountAlreadyInitialized` and `PdaMismatch` are matched by name.

## 9. Where the enforcement is *not*

Three things in this repository look like controls and are not, and it is worth
being explicit about which is which.

- **The module's envelope check is a courtesy.** `wallet.send` compares the
  amount against a cached `SpendEnvelope` before building anything
  (`module/src/wallet_skills.cpp`), and its own header says why that is not the
  control: the limits it compares against are a cache of an on-chain fact. It
  saves proving cost on a payment the chain would refuse. It stops nobody who has
  replaced the process.
- **The owner channel's timeout is a property of the honest agent.** An
  above-threshold spend goes to the owner over a reliable channel as a
  `spend_approval_request`, is re-sent every 15 s, and after 120 s with no answer
  becomes `ApprovalVerdict::Unreachable` — terminal, never a fallback to acting
  alone, which is what the prize requires. A nice detail: a reply carrying
  `per_tx`, `per_period`, `period_blocks` or `policy` is refused outright,
  because "an approval names a payment, it cannot change a limit". All of that
  binds an agent that is still running our code. An attacker simply does not send
  the request.
- **The inference layer is advisory by construction.** It can only ever move an
  answer toward *decline*; the amount paid is always the offer's price, never a
  number read out of a model's reply. A hostile model cannot authorise a payment,
  because it is never asked to name one.

The pattern is the same in all three: anything the agent process decides, whoever
holds the agent process decides differently. Only §2 through §6 survive that, and
§7 says exactly how far they get.

## 10. Assumptions this rests on

- **The sequencer enforces the state machine.** Rule 4 forbids changing an
  account's program owner, which is why §4 exists at all; rule 5 forbids a
  program from debiting an account it does not own, which is why `spend` chains
  into the transfer program rather than moving balance itself; rule 6 forbids a
  data write from any program but the owner, which is what makes the policy
  record a record; rule 8 requires total balance to be preserved across a
  transaction. These are LEZ's checks, not ours, and we verify them by observing
  that transactions violating them do not land.
- **The privacy circuit is what the sequencer pins.** The chained call is
  re-executed and checked under `env::verify` inside the proof
  (`lee/privacy_preserving_circuit/src/execution_state.rs:149-155`).
- **The owner's key is not on the agent's machine.** If it is, §7's distinction
  between "the agent's keys" and "the owner's keys" disappears, and §2's two
  signatures collapse into one. This is why anchoring is two transactions from
  two wallet homes rather than one transaction signing for both.
- **The agent's key is not stolen before it claims an owner.** `claim_agent` is
  the agent's own act and happens when the owner creates it, before it has run.
  An attacker who takes the key first designates itself and is then the owner —
  but such an attacker already holds the balance directly (§7), so this residual
  costs nothing that was not already lost.
- **Above-threshold payments are the exception.** Almost every recorded
  settlement is inside the envelope, so the approved path is exercised mainly by
  execution against the deployed binary (§8). It has also landed once on the
  public testnet — `approve_spend` in block 10776, `spend_approved` in block
  10786 — against an agent whose owner was claimed before it anchored. For the
  three agents this repository ships that path stays closed, because their owners
  anchored while unclaimed; [`limitations.md`](limitations.md) carries both.

Known failures and retractions are kept in [`limitations.md`](limitations.md),
which is the document to read next.
