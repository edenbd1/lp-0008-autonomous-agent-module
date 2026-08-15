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
right and this file is a bug — that has happened, which is why the
[ledger](#every-transaction-on-the-published-accounts) at the end exists.

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
| **live** | [`697746f5…cb5370bf`](https://explorer.testnet.lez.logos.co/transaction/697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf) | `778a9341…e670c4661` | 8839 | what `artifacts/programs/agent_verifier.bin` hashes to |
| superseded | [`a780003b…8576841e`](https://explorer.testnet.lez.logos.co/transaction/a780003b07204fc4d7445b5d88bbd2db8de248f0f1e5ffdbcd75fd268576841e) | `12fa95d9…b578c9d8` | 8720 | one policy account per agent — but **anybody** could anchor it, and did: `eedb3caf…`, block 8869 |
| superseded | [`8c87cc9b…2d20ebbe`](https://explorer.testnet.lez.logos.co/transaction/8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe) | `26ed1580…0bad50be` | 8646 | bound the ids; still counted nothing per period |
| first | [`b028eabf…b8c18549`](https://explorer.testnet.lez.logos.co/transaction/b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549) | — | 8590 | accepted a policy naming an owner the signer did not control |

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
| storage | `9Xpkkvos…jfv1FaE` | `5Sa13NyN…dHtjnZ` | `EZSN69nj…vG4Vvie` | [`88f9ec5c…dc292dd0`](https://explorer.testnet.lez.logos.co/transaction/88f9ec5c377dceeb5005336ecf358d778a30dc39d2ea49b1c166332cdc292dd0) | 8859 |
| messaging | `GpRdooEW…Zpe5FS` | `Dxh7ZLHF…fpEwD` | `Qg4NvAVr…J7hoW` | [`78ce43c9…adaa126c`](https://explorer.testnet.lez.logos.co/transaction/78ce43c977bcf9956d3c8f42836e65b2fc8159a18e04836214756cd0adaa126c) | 8875 |
| blockchain | `A7UBoMbS…c39JtMu` | `BzYks91a…H2wLnu` | `2kmd3L3f…LyMVjX` | [`0dd4e49e…e52921a2`](https://explorer.testnet.lez.logos.co/transaction/0dd4e49eeecac1366baf7a81a93639cadd8b6e013984979d99ebf63ae52921a2) | 8883 |

| Category | Policy account | Limits | Owner (signed create_policy) | create_policy | Block |
|---|---|---|---|---|---|
| storage | `6FscNXjN…Nj3ipSe` | 50 / 500 per 1000 blocks | `2dA9APZg…knWoZd` | [`6857ba23…631fe7d4`](https://explorer.testnet.lez.logos.co/transaction/6857ba2378a84ba51618582e852e3827a872e3ea85f17de76bdb45b1631fe7d4) | 8868 |
| messaging | `7HH46tXh…K1dA7bp` | 25 / 250 per 1000 blocks | `H3VSrUkv…A9yaTZ` | [`ce557a0a…278e1918`](https://explorer.testnet.lez.logos.co/transaction/ce557a0a8adc517b60496c35514e269fff92a4393b90bef41ce10916278e1918) | 8876 |
| blockchain | `2RK4dPwz…W3Wusvc9` | 200 / 1000 per 1000 blocks | `G64pMjF9…ih9uvCc` | [`2f6b481c…ecec5eda`](https://explorer.testnet.lez.logos.co/transaction/2f6b481cffde2adaeed9442c19599c939d97da0c930b70b45d97ac34ecec5eda) | 8884 |

Manifest, with the full ids and the account that anchored each policy:
[`artifacts/agents.tsv`](../artifacts/agents.tsv).

The **storage agent is a different identity** from the one the superseded
deployment used: `7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6` became
`9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE`. That was forced rather than
chosen. `claim_agent` is signed by the agent's **shielded** account, so that
account must hold a live note, and the storage agent's balance had been recycled
to zero — an agent with no note cannot sign anything. Moving 10 LEZ back to it
from its own public pay account (`6563e8d1…`) is what made it able to claim, and
a shielded transfer mints a **new note with a new account id** rather than
crediting the old one. Its public receiving account `5Sa13NyN…` is unchanged, so
the payment history below is continuous.

The blockchain agent is likewise a different identity from the one *two*
deployments ago — `9KdQSJ2t…VXicNe` became `A7UBoMbS…c39JtMu`, for the same
reason. The messaging agent has kept its identity throughout.

Every anchor has its **own** signer, and each of those was made by
`wallet account new public` and had never signed anything. `spel` builds each
transaction against nonce 0 while the sequencer checks the nonce for exact
equality, so a signer's second program transaction is built stale, submitted,
given a hash, and then silently dropped. Nothing reports it — which is why this
is written down rather than discovered again.

Each agent has **two** accounts, and the split is forced rather than chosen. The
shielded account is the agent: it holds the balance and signs its own payments.
The public account is where other agents pay it, because `spel` can resolve a
`Private/<id>` recipient only for accounts the *sending* wallet holds keys for,
and because `getAccount` reads the public state only — a payment into a shielded
account cannot be checked by anyone but its owner. See
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
AGENT=GpRdooEWJjX4JmRyT2n5KzMnDKtCM2HrvZ8iwMZpe5FS
AGENT_HEX=$(python3 -c "
import sys
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
n=0
for c in sys.argv[1]: n = n*58 + A.index(c)
print(n.to_bytes(32,'big').hex())" "$AGENT")

spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda policy --agent-id "$AGENT_HEX"
# 7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp

# and the claim account, whose seed the IDL declares as the signing account
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda claim --agent "$AGENT"
# Qg4NvAVrZ4fMwTAomeX7q8sbvnTHmuF9BvBsxUJ7hoW
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
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp"]}' \
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

which prints, today:

```
owner         H3VSrUkvPRqU1ruS2bpqrhETE9364hfeapXQReA9yaTZ
per_tx        25
per_period    250
period_blocks 1000
window_start  8000
spent         50
```

`spent 50` is the two settlements below — 0 → 25 → 50 against a `per_period` of
250 — and it is the per-period defect's fix measured rather than described.
`owner` is the account that signed `create_policy`, and it is the account the
messaging agent itself named in `claim_agent` before any policy existed: read
`Qg4NvAVrZ4fMwTAomeX7q8sbvnTHmuF9BvBsxUJ7hoW` and compare. The storage and
blockchain agents' policies are the same 97-byte shape with `window_start 0,
spent 0`: anchored, never spent under.

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
Here is that transaction, against `a780003b…`:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7"]}'
# [ "<the transaction>", 8869 ]
```

`eedb3caf…` is a `create_policy` with `per_tx = per_period = u128::MAX` over the
storage agent `9Xpkkvos…`, signed by `RZmSLJAB…` — an account created for the
purpose, which has never held that agent's key. Accepted, block 8869. It is not
an argument from absence either: the account it created is still there and still
says so.

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

Under that program a stranger owns that agent's only policy account, for the
life of the identity, and the honest owner's own anchor would then have been
refused `AccountAlreadyInitialized` — permanently, because LEZ rule 4 forbids
changing an account's program owner, so no `close` can exist. `per_tx = 0`
instead of `u128::MAX` is the same call as a denial of service.

The identical call to the program deployed here is `60de3fc6…`, and
`getTransaction` answers `null` for it: submitted, never included, error 6020.
The honest owner then anchored the same agent at the same address afterwards
(`6857ba23…`, block 8868), which is the other half of the property.

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
| client (pays, shielded) | `GpRdooEW…Zpe5FS` | same |
| server (paid, public) | `5Sa13NyN…dHtjnZ` | same |
| skill / price | `storage.upload` at 25 LEZ | same |
| settlement | [`e691f593…26631047`](https://explorer.testnet.lez.logos.co/transaction/e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047) | [`aef14146…8bcb70b8`](https://explorer.testnet.lez.logos.co/transaction/aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8) |
| block | 8892 | 8901 |
| period declared | 8000, valid in blocks 8000–8999 | same |
| server balance | 45 → 70 | 70 → 95 |
| policy ledger after | 25 spent in period 8000 | 50 spent in period 8000 |

The **payer is the messaging agent**, not the blockchain agent as in earlier
deployments. An envelope is a ceiling, not a balance: the blockchain agent's
ceiling is still the largest of the three, and its balance is 5 LEZ, because it
paid for the settlements of the deployment before this one. Picking a payer that
cannot afford the task would produce a policy check that passes and a transfer
that fails, which demonstrates nothing. All three agents are deployed and
anchored either way.

Manifest: [`artifacts/a2a-task.tsv`](../artifacts/a2a-task.tsv). It has exactly
two rows, and they are the two settlements above, both made under the live
program. The four settlements made under the two earlier programs are **not** in
it — they are in the [ledger](#every-transaction-on-the-published-accounts)
below, which is the only place in this repository that accounts for them.

The server's balance starts at 45 rather than 0, and that step is not an
accounting convention: the account held 100 from four earlier settlements and
then **spent 55 of it into shielded notes at block 8727** to fund this
deployment. That transaction is `1d983952…`, it is in the ledger, and it is the
reason this section can no longer be read without the ledger.

Balances read from the chain, not from the script's own output:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ"]}'
```

**This will not print 95.** It prints whatever that account holds now, and the
account has kept moving since the settlements — the ledger below is what
reconciles the two. `95` is the balance immediately after block 8747, which is
what `a2a-task.tsv` records and what the settlement transaction's own post-state
contains.

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

## Every transaction on the published accounts

The three public accounts above are published as payment evidence. An account
offered as evidence has to account for **everything** on it, not just the
transactions that flatter the claim — otherwise a reviewer who reads the chain
finds traffic the repository does not mention and has no way to tell an
experiment from a mistake.

These accounts were also used as working wallets during development: they funded
a deployment, they carried experiments against superseded programs, and they
moved balance between themselves. None of that is hidden here. Every transaction
that has ever touched any of the three is listed below in block order, with what
it was.

This was produced by scanning every block from 8000 to the chain head for the
accounts' raw 32-byte ids and hashing each block's transactions, then confirming
every hash with `getTransaction`; all of them resolve, and no other block in
that range touches these accounts. The three accounts' first transactions are
their own initialisations, so nothing precedes the table.

### storage, `5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ`

| Block | Transaction | What it is | Balance after |
|---|---|---|---|
| 8580 | `9ae7834d…834c1f06` | `auth-transfer init` — the agent claims its own receiving account | 0 |
| 8605 | `c45d3f24…94cf7275` | settlement, **first** program `b028eabf…` | 25 |
| 8624 | `8d7aba60…bb7502fb` | settlement, **first** program | 50 |
| 8677 | `5a488f28…aa00c554` | settlement, **superseded** program `8c87cc9b…` | 75 |
| 8686 | `f780df62…54ae8969` | settlement, **superseded** program | 100 |
| 8727 | `1d983952…b3651f4d` | **spends 55 into shielded notes — this is what funded the live deployment** | 45 |
| 8740 | `4e3a3454…a490ddb1` | settlement, program `a780003b…` — first row of `a2a-task.tsv` | 70 |
| 8747 | `7cad4fbd…7168f019` | settlement, program `a780003b…` — second row of `a2a-task.tsv` | 95 |
| 8749 | `d7498d65…1fbdd09b` | `create_policy` against the **superseded** program, `per_tx = per_period = u128::MAX`, creating `BP8zhGto…` — step 1 of the anchoring-bypass demonstration in [`artifacts/adversarial.tsv`](../artifacts/adversarial.tsv) | 95 |
| 8774 | `483dbe55…2304a164` | `authenticated_transfer` of **40** to the messaging pay account | 55 |
| 8794 | `7fc6c9af…9a022228` | receives **65** — step 2 of the bypass against the messaging agent: it moves its whole balance under the unlimited policy anchored at 8785, and this account is the recipient ([`adversarial.tsv`](../artifacts/adversarial.tsv)) | 120 |
| 8803 | `a7987634…42a1f493` | `authenticated_transfer` of **65** to the messaging pay account | 55 |
| 8847 | `6563e8d1…15f9a96e3` | spends **10** into shielded notes — **this is what let the storage agent claim an owner** under the live program. Its shielded balance had been recycled to zero, and `claim_agent` is signed by that account, so it had no note to spend. A shielded transfer mints a new note with a new id, which is why the storage agent is now `9Xpkkvos…` | 45 |
| 8892 | `e691f593…26631047` | settlement, **live** program `697746f5…` — third row of `a2a-task.tsv` | 70 |
| 8901 | `aef14146…8bcb70b8` | settlement, **live** program — fourth row of `a2a-task.tsv` | 95 |

Six of these were signed by the account itself — the initialisation, the 55
spend, the `create_policy`, the two transfers and the 10 that re-funded the
storage agent — which is exactly the nonce `getAccount` reports for it. The rest
are credits, which do not move a nonce.

### messaging, `Dxh7ZLHFmhKdNVE69XWayqLrquMk9iLfFVpmiJdfpEwD`

| Block | Transaction | What it is | Balance after |
|---|---|---|---|
| 8593 | `edcd794a…e2114e77` | `auth-transfer init` | 0 |
| 8614 | `e325c390…e20c9fa5` | receives **5** under the **first** program, via policy `872DUkRX…` | 5 |
| 8757 | `0a9ac12c…15b0e170` | receives **10** — step 2 of the bypass against the storage agent: a `spend` under the **superseded** program using `BP8zhGto…`, the unlimited policy anchored at 8749 ([`adversarial.tsv`](../artifacts/adversarial.tsv)) | 15 |
| 8772 | `8e2ec0d7…8100b544` | spends **15** into shielded notes | 0 |
| 8774 | `483dbe55…2304a164` | receives **40** from the storage pay account | 40 |
| 8783 | `83df9249…191443e5` | spends **40** into shielded notes | 0 |
| 8785 | `e530e0ba…399d462d` | `create_policy` against the **superseded** program, `per_tx = per_period = u128::MAX`, creating `AsvAU2Lf…` — the same bypass, against the messaging agent ([`adversarial.tsv`](../artifacts/adversarial.tsv)) | 0 |
| 8803 | `a7987634…42a1f493` | receives **65** from the storage pay account | 65 |

### blockchain, `BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu`

| Block | Transaction | What it is | Balance after |
|---|---|---|---|
| 8595 | `d97d6346…eaee9cd4` | `auth-transfer init` | 0 |

And nothing else, ever. **This account has never received a payment**: balance 0,
nonce 1. It is listed as the blockchain agent's receiving address because that is
what its Agent Card advertises and what a payment to it would credit, but in this
repository's flow the blockchain agent is the one that *pays* — it is the client
in both settlements — so nothing has ever been sent to it. The row above is the
whole of its history, and "Paid at" in the agents table means "would be paid at",
not "has been paid at".

### What the ledger says about the evidence

- The two settlements in `artifacts/a2a-task.tsv` are on chain, at blocks 8740
  and 8747, and moved the balance by exactly the price each time.
- Four earlier settlements, under two earlier programs, are also on chain. They
  are why the storage account had 100 before this deployment started.
- The four transactions at 8749, 8757, 8785 and 8794 are the anchoring-bypass
  demonstration, and they are evidence rather than debris:
  [`artifacts/adversarial.tsv`](../artifacts/adversarial.tsv) records each one
  with the step it plays. They are anchored against the **superseded** program,
  whose ProgramId is `3cxAuaA7…`, and cannot be replayed against the live
  program — a policy account is a PDA of the program, so the live program never
  looks at those addresses, and the identical calls to it are refused
  `AccountAlreadyInitialized`. That is why they were run where they were run.
- The transfers at 8774 and 8803 moved balance between two accounts this
  repository publishes. They are not payments for any task and no manifest
  claims they are.

Reproducing the deployment on **fresh** accounts — so that every transaction on
a published account is one this document describes, with nothing to explain
away — is the right way to close this, and it is not affordable on this testnet
today: the funding wallet `DumJ4LCB…` holds **10 LEZ**, and the three agents
alone need 50 before a single settlement. That is also why block 8727 exists at
all. Until a faucet refills it, this ledger is the honest form of the evidence.

## A note on the explorer

The explorer is a separate index and reaches a transaction roughly an hour and
three quarters after the sequencer does. A hash submitted minutes ago shows
"Transaction not found" there while `getTransaction` already returns it — an
indexing delay, not a missing transaction. The RPC is the immediate source of
truth; the explorer link above is for a reader arriving later.
