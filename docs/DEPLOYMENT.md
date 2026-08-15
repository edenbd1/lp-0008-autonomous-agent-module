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

| Category | Agent | Policy hash | Limits | create_policy |
|---|---|---|---|---|
| storage | `26tmxC9X8dywy5x7oyr1VeBUyR1vPxksrtbA3PJKPP7t` | `40674979…5409a856` | 50 / 500 per 1000 blocks | [`dd7338f9…5d900514`](https://explorer.testnet.lez.logos.co/transaction/dd7338f91550b2ffeb9a4971eb1026752c0b24e0c48533f6078787965d900514) |
| messaging | `1Yyo4FscmgZkeiNP4gcCtczzoZncKaByxKThPp8nqAc` | `6020573e…261c03c0` | 25 / 250 per 1000 blocks | [`8addbd40…bf4c4ffa`](https://explorer.testnet.lez.logos.co/transaction/8addbd40a196d77e0b0fbe6109c40f5cfe8e913f8c2a5b0bcfc3c45ebf4c4ffa) |
| blockchain | `AeGj71T1cwEP2hFbnNU422qZu3C9JqzUkFCK2sLChn61` | `4f9aae3a…efbeb741` | 200 / 1000 per 1000 blocks | [`093d7cd6…e2f12c8e`](https://explorer.testnet.lez.logos.co/transaction/093d7cd6962acf4d21ef0d1fbd49ab3e4d0a5314c9b7f7615447b831e2f12c8e) |

Manifest: [`artifacts/agents.tsv`](../artifacts/agents.tsv). The envelopes differ
on purpose: identical limits under one owner collapse to one policy hash, and
anchoring-by-address is easier to see when three envelopes give three addresses.

Agent keys live outside the repository, under `~/.lp0008-agents/`. An agent
whose key is committed is not an agent, and one whose key is thrown away cannot
sign again — the first version of the deploy script created each account in a
temporary directory and lost it, which is why this is stated rather than assumed.

## Two agents settling a task in LEZ, unattended

The storage agent publishes an A2A Agent Card advertising `storage.upload` at a
LEZ price. The blockchain agent discovers it, runs the A2A task lifecycle, and
pays — signing with **its own** key, not the owner's.

| | |
|---|---|
| task | `3e350c46d2802475572596adbdc24472` |
| client | `AeGj71T1cwEP2hFbnNU422qZu3C9JqzUkFCK2sLChn61` |
| server | `26tmxC9X8dywy5x7oyr1VeBUyR1vPxksrtbA3PJKPP7t` |
| skill / price | `storage.upload` at 25 LEZ |
| settlement | [`aea80817…d98449e7`](https://explorer.testnet.lez.logos.co/transaction/aea80817f6c4283c79b21095596ce774e3638cef888a3cc7b705b61ed98449e7) |

The settlement is a **PrivacyPreserving** transaction of 270,566 bytes, so the
payment carries a real proof rather than being a public transfer with a note
attached.

What makes it autonomous is not that nobody was watching. It is that the chain
would have refused it otherwise: 25 LEZ is inside the client's anchored
per-transaction limit, so `spend` takes the autonomous branch. Raise the price
above that limit and the identical call fails without an owner approval account
seeded by the exact payment — which is the whole point of anchoring the envelope
by address.

Reproduce: `./scripts/a2a-task.sh`.

## A note on the explorer

The explorer is a separate index and reaches a transaction roughly an hour and
three quarters after the sequencer does. A hash submitted minutes ago shows
"Transaction not found" there while `getTransaction` already returns it — an
indexing delay, not a missing transaction. The RPC is the immediate source of
truth; the explorer link above is for a reader arriving later.
