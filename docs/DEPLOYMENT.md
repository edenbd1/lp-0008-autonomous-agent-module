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

| Program | ImageID | Deploy tx | Explorer |
|---|---|---|---|
| Agent spending policy (`agent_verifier.bin`) | `15d234e5…32062e6a` | `b028eabf…b8c18549` | [link](https://explorer.testnet.lez.logos.co/transaction/b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549) |

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
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549"]}'
```

Rebuild it and get the same ImageID:

```bash
cargo risczero build --manifest-path crates/agent-verifier-spel/methods/guest/Cargo.toml
# ImageID: 15d234e5d4199b3a0b98d6c6f5fb4540fa41e46eba5fa14fabbd323832062e6a
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

| Category | Agent (shielded) | Paid at (public) | Policy hash | Limits | create_policy |
|---|---|---|---|---|---|
| storage | `7o9PT8uE…PGPEUM6` | `5Sa13NyN…dHtjnZ` | `b040065d…ec749a87` | 50 / 500 per 1000 blocks | [`ab017c9c…d67735f2`](https://explorer.testnet.lez.logos.co/transaction/ab017c9c9d55ac6ea198e692c5ed2b1dea4a2a70a1863495e48e7a91d67735f2) |
| messaging | `25LLt4Zx…gMdsafw` | `Dxh7ZLHF…fpEwD` | `885981bf…e8e0f5aa` | 25 / 250 per 1000 blocks | [`9373d809…92df8104`](https://explorer.testnet.lez.logos.co/transaction/9373d8094e3eb4a7efac5ce2514fbb58d188e84ad45582f2fe60738192df8104) |
| blockchain | `9KdQSJ2t…VXicNe` | `BzYks91a…H2wLnu` | `a03fb8fb…0496725e` | 200 / 1000 per 1000 blocks | [`b4a73bef…390428bf`](https://explorer.testnet.lez.logos.co/transaction/b4a73befe7d653805588ddaf7eccba5020cd0576db40039373d79d2d390428bf) |

Blocks 8591, 8594 and 8596. Manifest, with the full ids and the account that
anchored each policy: [`artifacts/agents.tsv`](../artifacts/agents.tsv).

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
  pda policy --policy-hash a03fb8fb318b01d43c9d1a6c7a651210de14c1677fbdd83faa8488fc0496725e
# 8zsfnzAk… for storage; each envelope gives a different address

curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["<that address>"]}'
# program_owner = 2UBUEH2tvc9xrYy21ZcQ6Bm4thn86cs2NPQJJNozuisb, the policy program
```

Agent keys live outside the repository, under `~/.lp0008-agents/`. An agent
whose key is committed is not an agent, and one whose key is thrown away cannot
sign again — the first version of the deploy script created each account in a
temporary directory and lost it, which is why this is stated rather than assumed.

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
