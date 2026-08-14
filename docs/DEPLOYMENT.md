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
| Agent spending policy (`agent_verifier.bin`) | `7629aa9c…e8712a0d` | `1ea86256…f18b6f3c` | [link](https://explorer.testnet.lez.logos.co/transaction/1ea86256ab621b623a3cdf1c50c1ac3ee2aa6ba1c7a66a89d68e5c26f18b6f3c) |

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
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["1ea86256ab621b623a3cdf1c50c1ac3ee2aa6ba1c7a66a89d68e5c26f18b6f3c"]}'
```

## The three agents

One per default skill category, as the prize asks. Each has its own shielded
account — a shared key would not be "indistinguishable on-chain from any other
account holder" — and its own anchored policy.

| Category | Agent | Policy hash | Limits | create_policy |
|---|---|---|---|---|
| storage | `Ed8AgbXRXvw3YRKDDnR5TMBZWHWiqTwLx4ycXGHq8qC1` | `9a845c95…48a3efe1` | 50 / 500 per 1000 blocks | [`835a2a8a…5f44afed`](https://explorer.testnet.lez.logos.co/transaction/835a2a8ab2086bc7099d4520ac05fa74804a6c6dc77d8f46e76c7d855f44afed) |
| messaging | `8DjSkchTCzCefFJp9C9YcfQZ1QGr6oAV3ZFNfooKhbVD` | `6a4ff90b…a07150bd` | 25 / 250 per 1000 blocks | [`77e644e4…9c907e16`](https://explorer.testnet.lez.logos.co/transaction/77e644e46b8979c929bfa1a71b6c8a37f0f692f544537ab0830a72d59c907e16) |
| blockchain | `GSukmyTRJvwz11XmNgLySh6nghvFJqA2ciLeeBSY8syv` | `d2a7b72c…a1fcb890` | 200 / 1000 per 1000 blocks | [`0e261ffd…b267e039`](https://explorer.testnet.lez.logos.co/transaction/0e261ffd70c681ea20571fe21333bfdc0b93c70831045ea95dc430bfb267e039) |

The full manifest is [`artifacts/agents.tsv`](../artifacts/agents.tsv).

The envelopes differ on purpose. Identical limits under one owner would produce
one policy hash for all three, and the point of anchoring by address is easier
to see when three envelopes give three addresses. Verify any of them:

```bash
./scripts/verify-agents.sh
```

which re-derives each policy account from the program's ImageID and the policy
hash, and checks the chain says the program owns it — the part no transaction
lookup can fake.

## A note on the explorer

The explorer is a separate index and reaches a transaction roughly an hour and
three quarters after the sequencer does. A hash submitted minutes ago shows
"Transaction not found" there while `getTransaction` already returns it — an
indexing delay, not a missing transaction. The RPC is the immediate source of
truth; the explorer link above is for a reader arriving later.
