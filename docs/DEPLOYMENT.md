# Deployment

Live on the public Logos Execution Zone testnet.

```
Network:            Public LEZ testnet
Sequencer JSON-RPC: https://testnet.lez.logos.co
Block explorer:     https://explorer.testnet.lez.logos.co
LEZ version:        v0.2.4 (commit 47eba25)
spel:               v0.6.0 sources, repinned and ported to v0.2.4 (vendor/spel)
cargo-risczero:     3.0.5
```

Every figure below was read back off the chain, and the commands that read it
are in the text. Where a number here disagrees with the chain, the chain is
right and this file is a bug — that has happened, which is why every figure here is re-checked against the chain by
`./scripts/verify-deployment.sh`.

This document has twice gone stale in the same way: every figure in it stayed
true on chain while a redeploy moved the program out from under it, so it went
on describing a deployment the repository no longer shipped. Reading it could
not detect that. Running it can:

```bash
./scripts/verify-deployment.sh
```

which recomputes the committed binary's deploy transaction, checks the chain has
it, checks **this file names it**, and then checks every policy account and
settlement in `artifacts/` against the chain. It exits non-zero when this
document and the repository have come apart. Run it before trusting the tables
below.

## Deployed programs

A LEZ program-deployment tx hash is `SHA256(borsh(bytecode))`, content
addressed, so the binary committed under `artifacts/programs/` hashes to exactly
this transaction — recompute it rather than trusting the table.

**The program this repository ships is `697746f5…`.** Three earlier programs are
still on chain and are referenced below, because the defects they contained are
what make this release's claims a matter of record rather than an assertion.

| | Deploy tx | ImageID | Block | Status |
|---|---|---|---|---|
| **live** | [`697746f5…cb5370bf`](https://explorer.testnet.lez.logos.co/transaction/697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf) | `778a9341…e670c4661` | 1358 | what `artifacts/programs/agent_verifier.bin` hashes to |

Earlier iterations of this program (one per agent but anchorable by anybody; an id-binding version; and a first cut) were deployed during development and are described in the git history. They lived on the pre-reset testnet and their deploy transactions no longer resolve, so only the shipped deployment is listed above.

The live program's ProgramId — the id accounts record as their owner — is
`93e5DRz2zkKQhxF6o5qb4mX3b6GwyhD5aUNSwBvodqoN`, or
`1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647`
in the decimal words `getAccount` answers with. The one before it is
`2H5xY4eoi225NpgLFgPF67EJFSQCXbedNzd4ajUAZwkK`, the one before that
`3cxAuaA7Xqy7gGrxPKXFDuRniatvnedkc8LvtjYQ1FgZ`, and the first
`2UBUEH2tvc9xrYy21ZcQ6Bm4thn86cs2NPQJJNozuisb`; all still own accounts, which is
how the ledger below tells one deployment's records from another's.

Recompute the deploy hash from the repository:

```bash
python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())"
# 697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf
```

and read it back off the chain:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf"]}'
# [ "<the transaction>", 8839 ]   <- the second element is the block
```

`getTransaction` answers `null` for a hash that was never included, which is
what makes the control in `demo.sh` — `de` repeated 32 times — meaningful.

Rebuild it and get the same ImageID:

```bash
cargo risczero build --manifest-path crates/agent-verifier-spel/methods/guest/Cargo.toml
# ImageID: 778a9341a00de46c4c056ac63a66f63156b068e61cce7f155a2b495e670c4661
```

**Editing a comment in the guest changes the ImageID.** Not a figure of speech:
`#[lez_program]` generates a `panic!` for a refused instruction, and a Rust panic
carries `core::panic::Location` — file, line, column — into the binary. Add a
line to a header comment, the line number moves, the ELF changes, the ImageID
changes, every policy PDA moves, and the committed binary no longer hashes to
the deploy transaction. The guest source is frozen between deployments for that
reason, and this note exists because it was nearly discovered the expensive way.

### The second program

`spend` moves no balance itself. LEZ rule 5 (`UnauthorizedBalanceDecrease`)
refuses any post-state that decreases the balance of an account the executing
program does not own, and an agent's account is owned by LEZ's **authenticated
transfer** program. So the policy program checks the anchored envelope and then
chains a call into that program, which does own the accounts.

That program is not deployed by this repository — it is one the chain already
runs, and it has no deploy transaction to look up because it is part of the
chain's initial state. A byte-identical copy is committed as
`artifacts/programs/authenticated_transfer.bin`, because the privacy circuit
composes the inner call inside the proof and looks the callee up by ImageID.
Its identity is therefore checked against the chain's own registry rather than
against a transaction:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getProgramIds","params":[]}'
# "authenticated_transfer": [583309054,2344528779,3806558405,2890696795,
#                            2257354672,3978764116,2273929063,1518858078]

spel program-id artifacts/programs/authenticated_transfer.bin
#   ProgramId (hex): 22c496fe,8bbeab8b,...   (the same words, in hex)
```

`./scripts/demo.sh` performs both checks, along with a SHA-256 pin on the file.

## The three agents

One per default skill category. Each has its own shielded account — a shared key
would not be "indistinguishable on-chain from any other account holder" — and its
own anchored policy.

Anchoring is **two transactions per agent** under this program. The agent signs
`claim_agent` first, naming the one account allowed to anchor over it; that
account then signs `create_policy`. A third party holds neither key, which is
the defect this deployment exists to fix.

| Category | Agent (shielded) | Paid at (public) | Owner claim | claim_agent | Block |
|---|---|---|---|---|---|
| storage | `DE4jFQbN…tNrw6L1` | [`2KyfEaAu…6V71o`](https://explorer.testnet.lez.logos.co/account/2KyfEaAuKw442zbRviJ2XeA9BWoBpFxNz8RGfWV6V71o) | [`9JLJKZLu…mnwLnW`](https://explorer.testnet.lez.logos.co/account/9JLJKZLukYQbX1k4efUUkbj4Ux9D93Wsoe95yxmnwLnW) | [`e47fec60…18a81971`](https://explorer.testnet.lez.logos.co/transaction/e47fec60aea2bb367a6393ec53f1d88c591e7359dac64ba43e9689c318a81971) | 2691 |
| messaging | `3meg13qB…oQAsmCG` | [`FduQnojv…oJWZtp`](https://explorer.testnet.lez.logos.co/account/FduQnojvAkyD55j7cmVycr5VB44vMsK7bDd6x1oJWZtp) | [`451Wgg17…uTbZSxN`](https://explorer.testnet.lez.logos.co/account/451Wgg17bUPmxki4rxH9WcfgBq2FbmCK1XiRSuTbZSxN) | [`c0e66362…a14ca086`](https://explorer.testnet.lez.logos.co/transaction/c0e6636245caf2f2369918283d456d55db06a1b5fe3b5493daa4b424a14ca086) | 2706 |
| blockchain | `94VUZEyE…V1fpZV4` | [`CJZzkWnT…hyGbX`](https://explorer.testnet.lez.logos.co/account/CJZzkWnTEE7MSRbdbA1pxD8EfRbNt1bPFgjxkuYhyGbX) | [`69RLYvpj…ifeoWx`](https://explorer.testnet.lez.logos.co/account/69RLYvpjhz98rr2wmRqf6hLsAmE14mGFSi65R4ifeoWx) | [`e676bf87…d23339b9`](https://explorer.testnet.lez.logos.co/transaction/e676bf870bfc87448071a5315be6bc4c9019c1b0d385a7ec4bae350ad23339b9) | 2721 |

| Category | Policy account | Limits | Owner (signed create_policy) | create_policy | Block |
|---|---|---|---|---|---|
| storage | [`C7DFFFvv…V4NRZgi`](https://explorer.testnet.lez.logos.co/account/C7DFFFvvQvFWkWczQTyBP2q9PsBGQmeFACTvSV4NRZgi) | 50 / 500 per 1000 blocks | [`4AD8jMUy…av98qWn`](https://explorer.testnet.lez.logos.co/account/4AD8jMUy1ZBkYmEy32yCK2RN5Q3z7rHk7h8bAav98qWn) | [`1868f89e…d566c22c`](https://explorer.testnet.lez.logos.co/transaction/1868f89e19a6725c384af8d0c42a44e686d2473c7a68e985953318b2d566c22c) | 2692 |
| messaging | [`Eqpqkr9V…1AZHz59`](https://explorer.testnet.lez.logos.co/account/Eqpqkr9VjqqE2GEHonZAF5cQbTs7TVpwECDuh1AZHz59) | 25 / 250 per 1000 blocks | [`Gf26xFak…ez5Sdt`](https://explorer.testnet.lez.logos.co/account/Gf26xFakbQEN7DxaDsNiLVtu2TN1KjvniUyGa5ez5Sdt) | [`d2f2822c…c104de4e`](https://explorer.testnet.lez.logos.co/transaction/d2f2822c692963a9e1afbe4021382eacbb398eeb649712c41fd2bf0ac104de4e) | 2707 |
| blockchain | [`4vtZYSdi…SubkjVx`](https://explorer.testnet.lez.logos.co/account/4vtZYSdiCaZyQP1x41qf328wdsFtmVpY7itLRSubkjVx) | 200 / 1000 per 1000 blocks | [`6ePxXkXn…iQ5fQRP`](https://explorer.testnet.lez.logos.co/account/6ePxXkXn3W9FQGquMnBb5KTfnBmb6YxSqfRAXiQ5fQRP) | [`1a99ab3e…6fa17cc4`](https://explorer.testnet.lez.logos.co/transaction/1a99ab3e2ef2398acdeecb7e3b305a9fcb12c90b4ccd21b1d32b1d556fa17cc4) | 2722 |

Manifest, with the full ids and the account that anchored each policy:
[`artifacts/agents.tsv`](../artifacts/agents.tsv).

An agent's **shielded** account id is whichever note it holds: `claim_agent` is signed
by that account, and a shielded transfer mints a *new* note with a new id rather than
crediting an old one, so re-funding an agent between deployments changes its shielded
identity. That is why the three shielded ids above (`DE4jFQbN…`, `3meg13qB…`,
`94VUZEyE…`) are forced by which note each held when it signed `claim_agent`, not chosen.
Each agent's **public** receiving account is separate from that shielded identity and is
the one listed in the table above.

Every anchor has its **own** signer, and each of those was made by
`wallet account new public` and had never signed anything. The reason is that a
signer still carrying the *default* program owner is filtered out of a program's
post-state by the SPEL macro on its second transaction, and the state machine
then rejects the whole thing as `DeclaredAccountMissingFromOutput`: submitted,
given a hash, and silently dropped, with nothing reporting it.

**This paragraph used to blame `spel` for building every transaction against
nonce 0, and that was wrong.** `spel` fetches each signer's nonce
(`vendor/spel/spel-cli/src/tx.rs:629`) and exits rather than guessing; a public
account on this testnet has reached nonce 33. The dropped transactions were real
and the cause was not the one named. The full retraction, and the way out — claim
the owner *before* it anchors — are in [`limitations.md`](limitations.md).

The three signers for the anchors recorded here were made by hand, and their ids
were written into `scripts/deploy-agents.sh` as literals. That is no longer how
the script works, and the change matters to anyone reproducing this: those three
ids are in the `owner` column of `artifacts/agents.tsv` as **evidence**, not as
input. The script now creates one signer per agent in the operator's own wallet
home, records them so a resumed run reuses them, and refuses to fund or claim
anything when it is handed a signer whose key that wallet does not hold — which
is what the literals silently did to every reader who was not the author. See
[`limitations.md`](limitations.md).

Each agent has **two** accounts, and the split is a choice with a reason rather
than a constraint. The shielded account is the agent: it holds the balance,
signs its own payments, and — since `d04f5e46…1e675cd1` in block 3018 — can be
paid at directly, by keys rather than by id. The public account exists because
`getAccount` reads the public state only, so a credit into it is checkable by a
stranger and a credit into the shielded one is not. Which of the two a payer
uses is a privacy decision, set out in
[`docs/limitations.md`](limitations.md).

### The two accounts an agent has, and where their addresses come from

There is no policy hash. Each agent has exactly two accounts under this program,
and both are addressed from the agent alone:

```
PDA(program, ["agent-owner/v1",  agent])   the owner claim — who may anchor
PDA(program, ["agent-policy/v1", agent])   the policy      — what was anchored
```

so anchoring is a once-per-agent act and a second policy for an agent is not
detected, it has nowhere to go. The limits are the policy account's **data**, not
part of its address: re-fixing a ceiling does not name a different account, it
writes a different record into the same one, and only `update_policy` signed by
the owner the record names can do it.

Resolve the address with the seed the IDL actually declares — the argument is
`--agent-id`, and it takes the agent's 32 raw bytes as hex:

```bash
AGENT=3meg13qBn2Xg7AC2TXzkvxA5BDCA6ezrN1WCJoQAsmCG
AGENT_HEX=$(python3 -c "
import sys
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
n=0
for c in sys.argv[1]: n = n*58 + A.index(c)
print(n.to_bytes(32,'big').hex())" "$AGENT")

spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda policy --agent-id "$AGENT_HEX"
# Eqpqkr9VjqqE2GEHonZAF5cQbTs7TVpwECDuh1AZHz59

# and the claim account, whose seed the IDL declares as the signing account
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda claim --agent "$AGENT"
# 451Wgg17bUPmxki4rxH9WcfgBq2FbmCK1XiRSuTbZSxN
```

Agent keys live outside the repository, under `~/.lp0008-agents/`. An agent
whose key is committed is not an agent, and one whose key is thrown away cannot
sign again — the first version of the deploy script created each account in a
temporary directory and lost it, which is why this is stated rather than assumed.

## Reading the policy record back

The record is **97 bytes**, and byte 0 is a layout version:

```
version(1) owner(32) per_tx(16) per_period(16) period_blocks(8) window_start(8) spent(16)
```

every integer little-endian. A record that is not 97 bytes, or whose version
byte is not 1, decodes to an error rather than to a policy — the difference
between "no ceiling" and "refuse".

The **paying** agent's policy is the one that carries a running total, so the
account to read is the messaging agent's — it is the payer in
`scripts/a2a-task.sh` under this deployment:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["Eqpqkr9VjqqE2GEHonZAF5cQbTs7TVpwECDuh1AZHz59"]}' \
| python3 -c "
import json,sys
r=json.load(sys.stdin)['result']
d=bytes(r['data'])
assert len(d)==97 and d[0]==1, 'not a record this program wrote: %d bytes' % len(d)
le=lambda a,b: int.from_bytes(d[a:b],'little')
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58(x):
    n=int.from_bytes(x,'big'); s=''
    while n: n,r=divmod(n,58); s=A[r]+s
    return s
print('owner        ', b58(d[1:33]))
print('per_tx       ', le(33,49))
print('per_period   ', le(49,65))
print('period_blocks', le(65,73))
print('window_start ', le(73,81))
print('spent        ', le(81,97))"
```

which prints the shape below. **The two numbers at the bottom are deliberately
left as `<…>`, and that is the correction this block needed:**

```
owner         Gf26xFakbQEN7DxaDsNiLVtu2TN1KjvniUyGa5ez5Sdt
per_tx        25
per_period    250
period_blocks 1000
window_start  <the window the last spend declared>
spent         <that window's running total>
```

`spent` is the sum of every price this ledger has been charged inside
`window_start`, against a `per_period` of 250, and it is the per-period defect's
fix measured rather than described.

This block used to print `window_start 8000, spent 50` and say "it read 50 when
this decode was first pasted here and it reads more now". The first half was a
snapshot, which the page admitted; the second half was **wrong about the
mechanism**, and that is worse. `spent` is per *window*, not cumulative — when
the window rolls, the ledger starts again at zero, so the number goes **down**.
It reads a smaller figure in a later window than it did in period 8000, and a
document that told a reader to expect a larger one taught them to distrust the
chain instead of the page. `./scripts/verify-deployment.sh` prints the live
`window … spent …` for all three agents; run that rather than believing any
figure here.

`owner` is the account that signed `create_policy`, and it is the account the
messaging agent itself named in `claim_agent` before any policy existed: read
`451Wgg17bUPmxki4rxH9WcfgBq2FbmCK1XiRSuTbZSxN` and compare. The storage and
blockchain agents' policies are the same 97-byte shape. The blockchain agent's
has never been spent under (`window_start 0, spent 0`); **the storage agent's
has**, since it began buying tasks from the blockchain agent — which is another
line this page carried as "anchored, never spent under" after it had stopped
being true of one of the two.

**The 24-byte decoder that used to be printed here belonged to the superseded
program.** Its records were `window_start(8) spent(16)` with no version and no
envelope, and two of them are still on chain — `BP8zhGto…` and `AsvAU2Lf…`, both
owned by `3cxAuaA7…`. Running the old snippet against a live 97-byte record does
not fail; it prints a 180-digit number, because `d[8:]` of the new layout is
most of the owner id read as an integer. A decoder that cannot tell the two
apart is worse than none, which is why the version byte is checked above.

## What this deployment fixes, and the transactions that show it

One defect, found by executing the deployed binary rather than by reading it,
and it made the headline claim of the previous release false in a worse way than
the defect that release had fixed.

### Anchoring a policy over somebody else's agent needed no key at all

`a780003b…` made the policy account a PDA of the agent alone, so that a second
policy for an agent could not exist. That was right about *where* a policy goes
and silent about *who* may put one there. Its `create_policy` declared two
accounts — the policy account and a signer it recorded as the owner. The agent's
own account was never declared, never read and never asked to sign, and
`agent_id` was a free argument the body discarded.

So the only thing an attacker needed was the agent's **public id**, which this
repository prints in `artifacts/agents.tsv` and inside every signed Agent Card.
On the superseded program `a780003b…` this attack was **accepted** on the pre-reset
testnet: a `create_policy` with `per_tx = per_period = u128::MAX` over the storage agent,
signed by an account created for the purpose that had never held the agent's key, took
ownership of that agent's only policy account. Those transactions were cleared by the
testnet reset and no longer resolve.

The program deployed here **refuses** the identical call: `getTransaction` answers `null`
for it — submitted, never included, guest error 6020, `create_policy` refusing any signer
the agent's own `claim_agent` did not name. `crates/agent-verifier-adversarial` runs
exactly this attack against the deployed binary and asserts that halt, and `demo.sh`
replays it on the public chain. The honest owner's anchor for the storage agent is a live
`create_policy`, [`1868f89e…d566c22c`](https://explorer.testnet.lez.logos.co/transaction/1868f89e19a6725c384af8d0c42a44e686d2473c7a68e985953318b2d566c22c)
in block 2692, at the PDA the live program derives — a different program from the one that
accepted the attack, so the two can never name one account.

**The fix is a second signature.** Anchoring is now two transactions from two
wallets: `claim_agent`, signed by the agent, writes the id of the one account
allowed to anchor over it; `create_policy`, signed by that account, refuses any
other signer (6020) or refuses outright when there is no claim (6019). And
`update_policy`, signed by the owner the record names (6012), means a wrong
anchor is recoverable rather than terminal.

That second half proves nothing from the chain by itself — a refused hash, a
pending one and a hash nobody ever sent all answer `null`, which is what
`demo.sh`'s cannot-exist-hash control has always demonstrated. The refusals are
shown where they can be, against the binary itself:

```bash
cd crates/agent-verifier-adversarial && cargo run --release
```

That runs `artifacts/programs/agent_verifier.bin` — the bytes whose SHA-256 is
the deploy transaction — in the risc0 executor, with pre-states built the way
the state machine builds them, and asserts the error code each hostile call
halts with. Forty-odd cases; the ones this release exists for:

| Call | Halts |
|---|---|
| anchoring an unlimited policy over an agent nobody has claimed | 6019 |
| the same call with `per_tx = 0` — the denial of service, not the theft | 6019 |
| anchoring over an agent that designated somebody else | 6020 |
| a stranger claiming an agent whose key it lacks | `PdaMismatch` |
| re-fixing an anchored envelope without being the owner it records | 6012 |
| an owner approval with no expiry block | 6021 |
| an agent signing its own owner approval | 6012 |
| an above-threshold spend on an approval account anyone could have funded | 6007 |
| presenting an approval a second time, after it was stamped | 6018 |

Each is paired with the honest call it differs from in one field, because a
check that only ever refuses says nothing about what is accepted — and the suite
then runs the whole attack in sequence, including the honest owner anchoring
*after* the stranger was refused. `demo.sh` runs the whole thing.

### Two defects earlier deployments fixed, kept because their evidence is on chain

- **An agent could anchor its own policy.** `b028eabf…` took `owner_id` as
  caller-supplied bytes and never compared them to the signer; `c0b21ba6…`, a
  `create_policy` with `per_tx = u128::MAX` naming an owner nobody controls, was
  accepted at block 8652. `8c87cc9b…` bound the ids but still let the agent's own
  key anchor a second, looser policy — `e530e0ba…`, and then `7fc6c9af…` moved 65
  LEZ against a ceiling of 25.
- **The per-period limit counted nothing.** `spent_this_period` was an
  instruction argument, both callers passed 0, and the enforced ceiling was
  `min(per_tx, per_period)` **per transaction**, unbounded in aggregate. The
  running total now lives in the policy account's `data`, written by the program
  that owns that account (LEZ rule 6 permits it there and nowhere else). The
  period is harder, because no program on this chain can read the block height:
  `ProgramInput` carries the program id, the caller, the pre-states and the
  instruction, and nothing else. What a program *can* do is constrain where its
  transaction lands, so `spend` takes the period as an argument and makes the
  argument binding — it must be a multiple of `period_blocks`, it may not be
  older than the period the ledger records, and the transaction is pinned to
  `[window_start, window_start + period_blocks)` via `ProgramOutput`'s block
  validity window, which the state machine enforces with `OutOfValidityWindow`.

The `spent` figure printed above is that ledger, under the current program.

## Two agents settling a task in LEZ, unattended

The storage agent publishes an A2A Agent Card advertising `storage.upload` at a
LEZ price. The blockchain agent discovers it, runs the A2A task lifecycle, and
pays — signing with **its own** key, not the owner's.

The payment is not a post-state this program writes. It cannot be: LEZ rule 5
refuses a post-state that debits an account the executing program does not own,
and the agent's account belongs to the transfer program. `spend` checks the
anchored envelope and then **chains a call** into that program, which does own
the account. The privacy circuit proves both programs and the composition.

Two settlements, run one after the other with no special handling between them.
The second one matters as much as the first: a repeat settlement is what this
repository could not produce before.

| | first | second |
|---|---|---|
| task | `d31ded5a…480cecd5` | `13191d7c…214dea97` |
| client (pays, shielded) | `3meg13qB…oQAsmCG` | same |
| server (paid, public) | [`2KyfEaAu…6V71o`](https://explorer.testnet.lez.logos.co/account/2KyfEaAuKw442zbRviJ2XeA9BWoBpFxNz8RGfWV6V71o) | same |
| skill / price | `storage.upload` at 1 LEZ | same |
| settlement | [`ed60240d…03f7910d`](https://explorer.testnet.lez.logos.co/transaction/ed60240d22e8aa11fe85c06edb893b9eb9b17d9fedacdcf9f745b8bf03f7910d) | [`a6e4e1f6…1e880824`](https://explorer.testnet.lez.logos.co/transaction/a6e4e1f6be39f13c54b445012db45988453c34e287a379cda5c3391e1e880824) |
| block | 2735 | 2759 |
| period declared | 2000, valid in blocks 2000–2999 | same |
| server balance | 0 → 1 | 1 → 2 |
| policy ledger after | 1 spent in period 2000 | 2 spent in period 2000 |

The **payer is the messaging agent**, not the blockchain agent as in earlier
deployments. An envelope is a ceiling, not a balance: the blockchain agent's
ceiling is still the largest of the three, and its balance is 5 LEZ, because it
paid for the settlements of the deployment before this one. Picking a payer that
cannot afford the task would produce a policy check that passes and a transfer
that fails, which demonstrates nothing. All three agents are deployed and
anchored either way.

Manifest: [`artifacts/a2a-task.tsv`](../artifacts/a2a-task.tsv). The two above
are its first two rows under the live program; it has since accumulated more,
and `./scripts/verify-deployment.sh` prints every row with the block and the
program the chain attributes it to rather than the one the file claims. Rows
under superseded programs are marked as such there, and the settlements made under
superseded programs are marked as such there; the pre-reset per-account histories
that once listed them have been cleared, as the reset cleared the transactions.

### A settlement whose payee is shielded too

The table above pays a **public** account. This one does not, and it is recorded
apart from those because nothing in `a2a-task.tsv`'s vocabulary can describe it:
its checks end in "and the payee's public balance moved by the price", and here
there is no public balance to move.

| | |
|---|---|
| client (pays, shielded) | `3meg13qB…oQAsmCG` |
| server (paid, **shielded**) | storage agent, by its `npk` `5ec166cf…` — no account id was named |
| price | 1 LEZ |
| settlement | [`d04f5e46…1e675cd1`](https://explorer.testnet.lez.logos.co/transaction/d04f5e46792a8c0114f8cc2e89b7d2c30441a16b8ed7ce80b8f72deb1e675cd1), shielded `spend`, `PrivateForeign` recipient |
| block | 3018 |
| period declared | 3000, valid in blocks 3000–3999 |
| note minted | `6cDcWhcW…8k8Y4`, holding 1 |
| read back by the payee | storage decodes the note with its own viewing key: balance 1 |

The last row is what makes this a receipt rather than a commitment. The storage agent
decodes the note it was paid — with its own viewing key, the only key that can — and
reads exactly 1 LEZ; a credit no one can read is not a receipt, and a stranger, lacking
that key, cannot read it at all.

The amount cannot be read with `getAccount`, and this is not a gap to be
apologised for — it is what paying a shielded payee buys. What a stranger checks
is inclusion and attribution (`verify-deployment.sh`, against
[`artifacts/shielded-settlement.tsv`](../artifacts/shielded-settlement.tsv));
what the payee checks, and only the payee, is the amount:

```bash
LEE_WALLET_HOME_DIR=~/.lp0008-agents/storage \
  tools/shielded-receipt/target/release/shielded-receipt \
  --payee DE4jFQbNVrjS5hGEVCE1txDfkdySvrgbGde8EtNrw6L1 \
  --tx d04f5e46792a8c0114f8cc2e89b7d2c30441a16b8ed7ce80b8f72deb1e675cd1 \
  --expect-amount 1
```

That decrypts the note the transaction carries and recomputes its commitment
against the ones the transaction published, so the balance it prints is the only
balance consistent with what the chain stored — not a number read back out of a
wallet file.

The server's balance starts at 45 rather than 0, and that step is not an
accounting convention: the account held 100 from four earlier settlements and
then **spent 55 of it into shielded notes at block 8727** to fund this
deployment. That transaction is `1d983952…`, it is in the ledger, and it is the
reason this section can no longer be read without the ledger.

Balances read from the chain, not from the script's own output:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["2KyfEaAuKw442zbRviJ2XeA9BWoBpFxNz8RGfWV6V71o"]}'
```

**This will not print any of the balances quoted on this page.** It prints
whatever that account holds now, and the account has kept moving since — up and
also *down* — so a balance here is a statement about a block, never about today.
Each `balance_after` in `a2a-task.tsv` is the figure that settlement's own
committed post-state contains, which is why it stays true after the account has
moved on; `95`, for instance, is the balance immediately after block 8747, one of
the two settlements made under the **superseded** program. The ledger below is
what reconciles the two.

Only the credit side is publicly readable: the payer is a shielded account and
`getAccount` answers with the default account for those. The debit is
constrained anyway — rule 8 requires total balance to be preserved across every
program in a transaction, so a transaction that credits 25 debited 25 — but it
is the payer's wallet, not the RPC, that can show it directly. This is stated at
length in [`docs/limitations.md`](limitations.md) rather than glossed.

What makes it autonomous is not that nobody was watching. It is that the chain
would have refused it otherwise: 25 LEZ is inside the client's anchored
per-transaction limit, so `spend` takes the autonomous branch. Raise the price
above that limit and the identical call fails without an owner approval account
seeded by the exact payment — which is the whole point of anchoring the envelope
by address.

Reproduce: `./scripts/a2a-task.sh`. It refuses to write its manifest unless the
transaction confirms **and** the recipient's balance moved by exactly the price,
because an earlier version of this instruction produced confirmed, on-chain
proofs that a policy permitted 25 LEZ and moved nothing at all.

## The settlement ledger

The **seven autonomous marketplace settlements** — the messaging agent paying the storage
agent's public account, signed by its own shielded key with no owner in the loop — are
recorded in `artifacts/a2a-task.tsv` (rows 1–7, the single-payee `0 → 7` sequence
`scripts/use-cases/02-services-marketplace.sh` verifies). Rows 8–9 are **two settlements
paid from inside a loaded module** (the `./scripts/delivery-in-plugin.sh settle` flow),
the messaging agent paying the *blockchain* agent's public account; they are cited here as
on-chain evidence rather than carried in the marketplace manifest, since their payee
differs. All nine are under the shipped program `697746f5…` and resolve on chain.

| # | settlement | block | path | public payee balance after |
|---|---|---|---|---|
| 1 | [`ed60240d…03f7910d`](https://explorer.testnet.lez.logos.co/transaction/ed60240d22e8aa11fe85c06edb893b9eb9b17d9fedacdcf9f745b8bf03f7910d) | 2735 | autonomous | `2KyfEaAu…` 0 → 1 |
| 2 | [`a6e4e1f6…1e880824`](https://explorer.testnet.lez.logos.co/transaction/a6e4e1f6be39f13c54b445012db45988453c34e287a379cda5c3391e1e880824) | 2759 | autonomous | 1 → 2 |
| 3 | [`b734b46f…968e0030`](https://explorer.testnet.lez.logos.co/transaction/b734b46f9b47eec197ff0ce9e804553358883b203a2b813fb3b3737f968e0030) | 2767 | autonomous | 2 → 3 |
| 4 | [`00ba48a5…121e91ae`](https://explorer.testnet.lez.logos.co/transaction/00ba48a5c08216894c120a993652aff17fe3416cc2ff980225d9a07c121e91ae) | 2776 | autonomous | 3 → 4 |
| 5 | [`31702b6a…2e83715b`](https://explorer.testnet.lez.logos.co/transaction/31702b6ab92c9772d8970bab67a8fa006b30fc3137262efbc9ac236d2e83715b) | 2785 | autonomous | 4 → 5 |
| 6 | [`cf9ac8bc…92626351`](https://explorer.testnet.lez.logos.co/transaction/cf9ac8bc506600db1ffc00bfef1815931879e94135a3c56528f9981992626351) | 2792 | autonomous | 5 → 6 |
| 7 | [`86f49935…263f4d4b`](https://explorer.testnet.lez.logos.co/transaction/86f4993527b66a03a7d0f52efe29efee0f9a8d4f05d7899639093db0263f4d4b) | 2802 | autonomous | 6 → 7 |
| 8 | [`59e3086e…929849b1`](https://explorer.testnet.lez.logos.co/transaction/59e3086e5febf3051bbeaff7b80634f8c41ab26ef43e63b0dfcef7a4929849b1) | 2863 | loaded module | `CJZzkWnT…` 0 → 1 |
| 9 | [`0ef0f3f9…1a3f88f9`](https://explorer.testnet.lez.logos.co/transaction/0ef0f3f9d1ac987e08ff9d7a265ebb94df6570b5ad15dbe44cca49d41a3f88f9) | 2874 | loaded module | 1 → 2 |

Each is confirmed by `getTransaction`, its bytes hash to the cited hash, and the public
payee balance moved by exactly the price. The payer is a shielded account, so only the
credit side is publicly readable. No count is fixed in the prose — settlements are
appended by `scripts/a2a-task.sh`, so `verify-deployment.sh` is the live source of the
list and marks any row under a superseded program.

### A shielded settlement, live on the current deployment

The module also pays a **shielded** payee — the messaging agent paying the storage agent
at its foreign shielded keys under the shipped `spend` instruction, so neither the payee
nor the amount is publicly readable. It is live on the current deployment:
[`d04f5e46…1e675cd1`](https://explorer.testnet.lez.logos.co/transaction/d04f5e46792a8c0114f8cc2e89b7d2c30441a16b8ed7ce80b8f72deb1e675cd1), block 3018, amount 1, minting note `6cDcWhcW…8k8Y4` — recorded
in `artifacts/shielded-settlement.tsv` and recognised by `verify-deployment.sh`. There is
no public account to credit, so `getAccount` shows nothing; the storage agent reads the
received note's balance (1) with its own viewing key. This is the same `spend` instruction
the public settlements above use, differing only in a `PrivateForeign` recipient built
from the payee's Agent Card.

## A note on the explorer

The explorer is a separate index and reaches a transaction roughly an hour and
three quarters after the sequencer does. A hash submitted minutes ago shows
"Transaction not found" there while `getTransaction` already returns it — an
indexing delay, not a missing transaction. The RPC is the immediate source of
truth; the explorer link above is for a reader arriving later.
