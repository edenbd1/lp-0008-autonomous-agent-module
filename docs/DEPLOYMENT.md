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

## Deployed programs

A LEZ program-deployment tx hash is `SHA256(borsh(bytecode))`, content
addressed, so the binary committed under `artifacts/programs/` hashes to exactly
this transaction — recompute it rather than trusting the table.

| Program | ImageID | Deploy tx | Block | Explorer |
|---|---|---|---|---|
| Agent spending policy (`agent_verifier.bin`) | `26ed1580…0bad50be` | `8c87cc9b…2d20ebbe` | 8646 | [link](https://explorer.testnet.lez.logos.co/transaction/8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe) |

Its ProgramId — the id accounts record as their owner — is
`3cxAuaA7Xqy7gGrxPKXFDuRniatvnedkc8LvtjYQ1FgZ`, or
`2148920614,3576134543,3415609557,259224239,1770396588,3252552076,3201387284,3192958219`
in the decimal words `getAccount` answers with.

The program deployed before this one, `b028eabf…b8c18549`, is still on chain and
is referenced below: the attack it accepted is what makes the defect this
release fixes a matter of record rather than a claim.

Recompute the deploy hash from the repository:

```bash
python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())"
```

and read it back off the chain:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe"]}'
```

Rebuild it and get the same ImageID:

```bash
cargo risczero build --manifest-path crates/agent-verifier-spel/methods/guest/Cargo.toml
# ImageID: 26ed15808f7b27d5d51096cbaf72730fac1b86698c01dec1144bd1be0bad50be
```

### The second program

`spend` moves no balance itself. LEZ rule 5 (`UnauthorizedBalanceDecrease`)
refuses any post-state that decreases the balance of an account the executing
program does not own, and an agent's account is owned by LEZ's **authenticated
transfer** program. So the policy program checks the anchored envelope and then
chains a call into that program, which does own the accounts.

That program is not deployed by this repository — it is one the chain already
runs — but a byte-identical copy is committed as
`artifacts/programs/authenticated_transfer.bin`, because the privacy circuit
composes the inner call inside the proof and looks the callee up by ImageID.
Its identity is checked against the chain rather than asserted:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getProgramIds","params":[]}'
# "authenticated_transfer": [583309054,2344528779,3806558405,2890696795,
#                            2257354672,3978764116,2273929063,1518858078]

spel program-id artifacts/programs/authenticated_transfer.bin
#   ProgramId (decimal): 583309054,2344528779,...
```

`./scripts/demo.sh` performs both checks, along with a SHA-256 pin on the file.

## The three agents

One per default skill category. Each has its own shielded account — a shared key
would not be "indistinguishable on-chain from any other account holder" — and its
own anchored policy.

| Category | Agent (shielded) | Paid at (public) | Policy hash | Limits | create_policy | Block |
|---|---|---|---|---|---|---|
| storage | `7o9PT8uE…PGPEUM6` | `5Sa13NyN…dHtjnZ` | `610135ad…86047ac3` | 50 / 500 per 1000 blocks | [`d3a0fc9b…988db74b`](https://explorer.testnet.lez.logos.co/transaction/d3a0fc9b75d71440686ec55503172b7a81a0897e02cae47c8a982242988db74b) | 8649 |
| messaging | `GpRdooEW…Zpe5FS` | `Dxh7ZLHF…fpEwD` | `2a1e2940…815d3a60` | 25 / 250 per 1000 blocks | [`b0c78a6e…dcfe40ec`](https://explorer.testnet.lez.logos.co/transaction/b0c78a6e26e3f854386c7d1262e27cc03681ace18032fa92c1cba7ebdcfe40ec) | 8651 |
| blockchain | `9KdQSJ2t…VXicNe` | `BzYks91a…H2wLnu` | `1a317aae…c6e7c356` | 200 / 1000 per 1000 blocks | [`e68411fa…f218513c10`](https://explorer.testnet.lez.logos.co/transaction/e68411fa8aec0c8a3fff4d428a0e8705fbb7dc368e367fc8d2da96f218513c10) | 8652 |

Manifest, with the full ids and the account that anchored each policy:
[`artifacts/agents.tsv`](../artifacts/agents.tsv).

Every anchor has its **own** signer, and each of those was made by
`wallet account new public` and had never signed anything. `spel` builds each
transaction against nonce 0 while the sequencer checks the nonce for exact
equality, so a signer's second program transaction is built stale, submitted,
given a hash, and then silently dropped. Nothing reports it — which is why this
is written down rather than discovered again.

The ids committed into a policy hash are the **32 raw bytes of the accounts**,
not a hash of how they are printed. That changed with this deployment because
the program now compares them: `owner_id` against the account that signed
`create_policy`, `agent_id` against the account presenting itself as the payer
in `spend`. A sha256 of a base58 string is not something the chain can check
against anything.

The envelopes differ on purpose: identical limits under one owner collapse to
one policy hash, and anchoring-by-address is easier to see when three envelopes
give three addresses.

Each agent has **two** accounts, and the split is forced rather than chosen. The
shielded account is the agent: it holds the balance and signs its own payments.
The public account is where other agents pay it, because `spel` can resolve a
`Private/<id>` recipient only for accounts the *sending* wallet holds keys for,
and because `getAccount` reads the public state only — a payment into a shielded
account cannot be checked by anyone but its owner. See
[`docs/limitations.md`](limitations.md).

Read any of it back:

```bash
# the policy account for the blockchain agent's envelope
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda policy --policy-hash 1a317aae885143298b3b033539273a02ff9c0c4f55e586f979a22b15c6e7c356
# BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS
# 5fEo2TGt… for storage and 7fExSPpR… for messaging: each envelope, a different
# address

curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS"]}'
# program_owner = 3cxAuaA7Xqy7gGrxPKXFDuRniatvnedkc8LvtjYQ1FgZ, the policy program
# data         = the running total, 24 bytes: window_start then spent
```

That `data` field is worth a second look, because it is the second defect this
release fixes. It was empty at anchoring and the settlements below wrote it:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
print('period', int.from_bytes(d[:8],'little'), 'spent', int.from_bytes(d[8:],'little'))"
```

Agent keys live outside the repository, under `~/.lp0008-agents/`. An agent
whose key is committed is not an agent, and one whose key is thrown away cannot
sign again — the first version of the deploy script created each account in a
temporary directory and lost it, which is why this is stated rather than assumed.

## What this deployment fixes, and the transactions that show it

Two defects, both found by executing the deployed binary rather than by reading
it, and both of which made a headline claim in this repository false.

### 1. An agent could anchor its own policy

`create_policy` took `owner_id` as caller-supplied bytes and never compared them
to the account that signed. So "an attacker who takes the agent process cannot
raise the ceiling" was wrong: they could not *edit* a policy, but they could
anchor a fresh one and use that. Here is the transaction, against the previous
program:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["c0b21ba6325e451d61408692b4f897ff6e5d22535abeecb671c59a4b1af554c2"]}'
```

`c0b21ba6…` is a `create_policy` with `per_tx = per_period = u128::MAX`, signed
by `3KcCpbGL…fZ8pm2` and naming owner `aaaa…aaaa`, an account nobody controls.
Program `b028eabf…`, block 8652. Accepted.

The identical call to the program deployed today is `30c93c61…`, and
`getTransaction` answers `null` for it: submitted, never included.

That second half proves nothing by itself — a refused hash, a pending one and a
hash nobody ever sent all answer `null`, which is what `demo.sh`'s
cannot-exist-hash control has always demonstrated. The refusal is shown where it
can be, against the binary itself:

```bash
cd crates/agent-verifier-adversarial && cargo run --release
```

That runs `artifacts/programs/agent_verifier.bin` — the bytes whose SHA-256 is
the deploy transaction — in the risc0 executor, with pre-states built the way
the state machine builds them, and asserts the error code each hostile call
halts with:

| Call | Halts |
|---|---|
| anchoring a policy naming an owner the signer does not control | 6012 |
| spending under a policy anchored for a different agent | 6013 |
| a spend that would carry the period total past the per-period limit | 6006 |
| a period that does not start on a multiple of `period_blocks` | 6014 |

Each is paired with the honest call it differs from in one field, because a
check that only ever refuses says nothing about what is accepted. `demo.sh`
runs the whole thing.

Three bindings were needed, not one. Fixing `create_policy` alone would have
changed nothing: an attacker who cannot invent an owner id can anchor a policy
under an account it really controls and point the compromised agent at that
hash, so `spend` and `spend_approved` also had to check that the account paying
is the agent the policy names. And `approve_spend` took a signer without asking
whether it was the policy's owner, so the agent could sign its own approvals and
cross the threshold from the other side.

### 2. The per-period limit counted nothing

`spent_this_period` was an instruction argument. Both callers passed 0, the
policy account held no data, and `period_blocks` was folded into the address and
never compared to anything — so the enforced ceiling was `min(per_tx,
per_period)` **per transaction**, unbounded in aggregate.

The running total now lives in the policy account's `data`, written by the
program that owns that account (LEZ rule 6 permits it there and nowhere else).
The period is harder, because no program on this chain can read the block
height: `ProgramInput` carries the program id, the caller, the pre-states and
the instruction, and nothing else. What a program *can* do is constrain where
its transaction lands, so `spend` takes the period as an argument and makes the
argument binding — it must be a multiple of `period_blocks`, it may not be older
than the period the ledger records, and the transaction is pinned to
`[window_start, window_start + period_blocks)` via `ProgramOutput`'s block
validity window, which the state machine enforces with `OutOfValidityWindow`.
Naming a later period yields a transaction no current block accepts; naming an
earlier one is refused outright.

`data` on `BLHNchq8…`, above, is that ledger, and the two settlements below are
what wrote it.

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
| task | `53d4db43…8322d29d` | `b68c9e51…34b6a0a2` |
| client (pays, shielded) | `9KdQSJ2t…VXicNe` | same |
| server (paid, public) | `5Sa13NyN…dHtjnZ` | same |
| skill / price | `storage.upload` at 25 LEZ | same |
| settlement | [`c45d3f24…94cf7275`](https://explorer.testnet.lez.logos.co/transaction/c45d3f2441cf1d19d69ae4cc70cfd50308fc2f0ed89ec40310c5ea2a94cf7275) | [`8d7aba60…bb7502fb`](https://explorer.testnet.lez.logos.co/transaction/8d7aba60786d812d6e596624518a38813e7b9f4573d20b6efe802ac4bb7502fb) |
| block | 8605 | 8624 |
| kind / size | PrivacyPreserving, 270,566 bytes | PrivacyPreserving, 270,566 bytes |
| server balance | 0 → 25 | 25 → 50 |
| client balance | 100 → 75 | 75 → 50 |

Manifest: [`artifacts/a2a-task.tsv`](../artifacts/a2a-task.tsv).

Balances read from the chain, not from the script's own output:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ"]}'
```

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

## A note on the explorer

The explorer is a separate index and reaches a transaction roughly an hour and
three quarters after the sequencer does. A hash submitted minutes ago shows
"Transaction not found" there while `getTransaction` already returns it — an
indexing delay, not a missing transaction. The RPC is the immediate source of
truth; the explorer link above is for a reader arriving later.
