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
| Agent spending policy (`agent_verifier.bin`) | `12fa95d9…b578c9d8` | `a780003b…8576841e` | 8720 | [link](https://explorer.testnet.lez.logos.co/transaction/a780003b07204fc4d7445b5d88bbd2db8de248f0f1e5ffdbcd75fd268576841e) |

Its ProgramId — the id accounts record as their owner — is
`2H5xY4eoi225NpgLFgPF67EJFSQCXbedNzd4ajUAZwkK`, or
`3650484754,2032214328,3036549407,1048473516,3525353185,166458006,2651200166,3637082293`
in the decimal words `getAccount` answers with.

The program deployed before this one, `8c87cc9b…2d20ebbe`, is still on chain and
is referenced below: the attack it accepted — executed, in four transactions, on
this testnet — is what makes the defect this release fixes a matter of record
rather than a claim.

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
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["a780003b07204fc4d7445b5d88bbd2db8de248f0f1e5ffdbcd75fd268576841e"]}'
```

Rebuild it and get the same ImageID:

```bash
cargo risczero build --manifest-path crates/agent-verifier-spel/methods/guest/Cargo.toml
# ImageID: 12fa95d9382121791f11feb4ac6f7e3ee19e20d296f2eb09a61a069eb578c9d8
```

**Editing a comment in the guest changes the ImageID.** Not a figure of speech:
`#[lez_program]` generates a `panic!` for a refused instruction, and a Rust panic
carries `core::panic::Location` — file, line, column — into the binary. The
executor prints it, `agent_verifier.rs:176:1`, which is the line the macro sits
on. Add a line to the header comment and that becomes 177, the ELF changes, the
ImageID changes, every policy PDA moves, and the committed binary no longer
hashes to the deploy transaction. The guest source is frozen between
deployments for that reason, and this note exists because it was nearly
discovered the expensive way.

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

| Category | Agent (shielded) | Paid at (public) | Policy account | Limits | create_policy | Block |
|---|---|---|---|---|---|---|
| storage | `7o9PT8uE…PGPEUM6` | `5Sa13NyN…dHtjnZ` | `HHhRoBfv…2mPYRbF` | 50 / 500 per 1000 blocks | [`79c91ec7…a24b70f5`](https://explorer.testnet.lez.logos.co/transaction/79c91ec796a14b7c0c2df11ac96ff944f915fe767db397b31331c48fa24b70f5) | 8729 |
| messaging | `GpRdooEW…Zpe5FS` | `Dxh7ZLHF…fpEwD` | `7ewsGn9S…HubbMyN` | 25 / 250 per 1000 blocks | [`eb294055…38e53249`](https://explorer.testnet.lez.logos.co/transaction/eb294055f61645852e03fb96cc794a01b421b7dd714358c4e1a5000838e53249) | 8731 |
| blockchain | `A7UBoMbS…c39JtMu` | `BzYks91a…H2wLnu` | `Coxz1Cmf…xZk5rgM` | 200 / 1000 per 1000 blocks | [`0266f48b…d3cae867`](https://explorer.testnet.lez.logos.co/transaction/0266f48bcef250fc5c9fd68c6ebdd7e46d33e4e84c40a2cbbe7c7174d3cae867) | 8732 |

Manifest, with the full ids and the account that anchored each policy:
[`artifacts/agents.tsv`](../artifacts/agents.tsv). The `policy_hash` column it
used to carry is gone: there is no policy hash any more, and the column that
replaced it, `policy_account`, is an address a reader can put straight into
`getAccount`.

The blockchain agent's shielded id changed with this deployment and the other
two did not. A shielded transfer creates a new note with its own account id
rather than crediting an existing one, so re-funding that agent — its balance
was spent down by the settlements under the previous program — moved it to
`A7UBoMbS…c39JtMu`. The policy is anchored on the account that actually holds
the money, which is the only account `spend` will accept as the payer.

Every anchor has its **own** signer, and each of those was made by
`wallet account new public` and had never signed anything. `spel` builds each
transaction against nonce 0 while the sequencer checks the nonce for exact
equality, so a signer's second program transaction is built stale, submitted,
given a hash, and then silently dropped. Nothing reports it — which is why this
is written down rather than discovered again.

`create_policy` takes **no owner argument at all** any more. The signer's own
account id is what the program writes into the policy record, so the `owner`
column of the manifest is a fact to check rather than an input to reproduce —
decode the policy account's data and the first 32 bytes after the version byte
are that account.

The envelopes differ because the agents do different work. They no longer *have*
to differ: identical limits under one owner used to collapse to one policy hash,
and the address no longer depends on the limits at all.

Each agent has **two** accounts, and the split is forced rather than chosen. The
shielded account is the agent: it holds the balance and signs its own payments.
The public account is where other agents pay it, because `spel` can resolve a
`Private/<id>` recipient only for accounts the *sending* wallet holds keys for,
and because `getAccount` reads the public state only — a payment into a shielded
account cannot be checked by anyone but its owner. See
[`docs/limitations.md`](limitations.md).

Read any of it back. The seed argument is `--agent-id`, the agent's 32 bytes,
because that — with a constant prefix — is the whole of the address:

```bash
# the policy account for the blockchain agent, from its account id
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
  pda policy --agent-id 8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90
# Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM

curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM"]}'
# program_owner = 2H5xY4eoi225NpgLFgPF67EJFSQCXbedNzd4ajUAZwkK, the policy program
# data         = 97 bytes: version, owner, both limits, the period, and the
#                running total
```

That `data` field is the policy. It is not a hash of anything and it is not
supplied by any caller: `create_policy` writes it once and `spend` updates only
the last 24 bytes of it thereafter.

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM"]}' \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
assert len(d)==97 and d[0]==1, 'not a record this program wrote'
le=lambda a,b: int.from_bytes(d[a:b],'little')
print('owner        ', d[1:33].hex())
print('per_tx       ', le(33,49))
print('per_period   ', le(49,65))
print('period_blocks', le(65,73))
print('window_start ', le(73,81))
print('spent        ', le(81,97))"
# owner         9e54ba239fbaa7f930abe2f9d6480240482679e5b02e205459142de1184a6270
# per_tx        200
# per_period    1000
# period_blocks 1000
# window_start  8000
# spent         50
```

Agent keys live outside the repository, under `~/.lp0008-agents/`. An agent
whose key is committed is not an agent, and one whose key is thrown away cannot
sign again — the first version of the deploy script created each account in a
temporary directory and lost it, which is why this is stated rather than assumed.

## What this deployment fixes, and the transactions that show it

Two defects, both found by executing the deployed binary rather than by reading
it, and both of which made a headline claim in this repository false.

### 1. An agent could anchor its own policy

Not by lying. That is what made this survive three fixes.

The policy account's address used to be a hash of (owner, agent, `per_tx`,
`per_period`, `period_blocks`). Every triple therefore had an address of its own,
all of them uninitialised, so "anchor a new policy" was always available. The
last deployment added the two comparisons that look like they close it — the
signer must be the `owner_id` it commits to, the payer must be the `agent_id` —
and they do not, because an attacker holding the agent's key does not have to
invent an owner. **It is the owner.** It anchors a fresh policy naming the
compromised agent and an account it controls, with `per_tx = u128::MAX`, and both
comparisons pass because both are true.

Executed, in two transactions, against the program this release replaces
(`8c87cc9b…2d20ebbe`). The victim is the messaging agent, whose owner anchored
`per_tx = 25`; it held 65 LEZ:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["e530e0ba9a49c4ebacbfeaeac8fff3376f8bece24b71cb8f985b70c5399d462d"]}'
```

`e530e0ba…` is a `create_policy` with `per_tx = per_period = u128::MAX`, signed
by the agent's **own public pay account** `Dxh7ZLHF…fpEwD` and honestly naming
that account as the owner. Accepted, included in a block.

`7fc6c9af…f49a022228` is the `spend` that follows: the agent moves its **entire
65 LEZ in one transaction**, against a ceiling its owner set at 25. Accepted,
and the recipient's balance moved by 65. The same two steps against the storage
agent, `d7498d65…1fbdd09b` and `0a9ac12c…15b0e170`, were accepted too. Four
transactions, all on the public testnet, all in
[`artifacts/adversarial.tsv`](../artifacts/adversarial.tsv).

There was no id left in the instruction to compare, so the **address** changed
instead. A policy account is now `PDA(program, ["agent-policy/v1", agent_id])` —
one per agent — and `create_policy` declares it `#[account(init)]`, which refuses
an account that already exists. The limits moved into that account's data, beside
the running ledger the program was already keeping there. A hostile anchor is not
rejected; it has nowhere to go.

The identical call to the program deployed today is
`a01ace40b839f89b7b662b5532521716bd0906fbeef73ed15dae8c6b2cfd5352`, and
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
the state machine builds them, and asserts what each hostile call halts with:

| Call | Halts |
|---|---|
| an attacker anchors an unlimited policy over an agent that has one | `AccountAlreadyInitialized` |
| …signed by the agent's own program-owned pay account | `AccountAlreadyInitialized` |
| …signed by the compromised agent itself | `AccountAlreadyInitialized` |
| a different agent presents this agent's policy account | `PdaMismatch` |
| a spend of the whole balance under the honest policy | 6005 |
| a spend that would carry the period total past the per-period limit | 6006 |
| a period that does not start on a multiple of `period_blocks` | 6014 |
| a period older than the one the record holds | 6015 |
| a policy account holding data this program did not write | 6016 |
| an agent signing its own owner approval | 6012 |
| an above-threshold spend on an approval account anyone could have funded | 6007 |
| presenting an approval a second time, after it was stamped | 6018 |

and then the two-step attack end to end — anchor, then spend — in each of the
three shapes the previous program accepted.

The first four have no numeric code: they come from the macro's account
validation, which the generated dispatcher consumes with
`.expect("account validation failed")`, so the guest panics with the variant's
`Debug` and nothing else. The refusal that closes this defect is one of them.

Each hostile call is paired with the honest call it differs from in one field,
because a check that only ever refuses says nothing about what is accepted.
`demo.sh` runs the whole thing.

The approval rows are also the only execution the above-threshold path gets
anywhere: `scripts/a2a-task.sh` settles inside the envelope and
`scripts/e2e-local-sequencer.sh` exercises `spend`. The instruction whose job is
to let an agent spend *more* than its ceiling is the last one that should go
unexercised.

Three error codes are **retired rather than reused** — 6001, 6004 and 6013. All
three existed because the caller chose the policy account's address and the
program had to check the choice; the address is a function of the agent now, so
the disagreements they reported cannot be expressed. Leaving the numbers unused
keeps an integration that branches on them from silently matching a different
refusal.

**What is not closed**, stated here as well as in the security model because it
is the kind of thing that gets read once: `create_policy` never declares the
agent's account. Anchoring over somebody else's agent needs no key, only the
agent's public id, which this repository publishes. Whoever anchors first holds
that envelope for the life of the agent identity, and `per_tx = 0` is a permanent
denial of service. It grants no spending authority and moves no money — it is a
race for an address, not a theft — but it is a real regression against the
predecessor design, which at least required the agent's key.
[`security-model.md`](security-model.md) §6 has it at full length.

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

`data` on `Coxz1Cmf…`, above, is that ledger — the last 24 of its 97 bytes — and
the two settlements below are what wrote it.

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
| task | `192c7dce…4f55e2715a` | `b9a7ca40…1d00572fe20d` |
| client (pays, shielded) | `A7UBoMbS…c39JtMu` | same |
| server (paid, public) | `5Sa13NyN…dHtjnZ` | same |
| skill / price | `storage.upload` at 25 LEZ | same |
| settlement | [`4e3a3454…a490ddb1`](https://explorer.testnet.lez.logos.co/transaction/4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1) | [`7cad4fbd…7168f019`](https://explorer.testnet.lez.logos.co/transaction/7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019) |
| block | 8740 | 8747 |
| period declared | 8000, valid in blocks 8000–8999 | same |
| server balance | 45 → 70 | 70 → 95 |
| policy ledger after | 25 spent in period 8000 | 50 spent in period 8000 |

That last row is the second defect's fix, measured rather than described: the
policy account's data held `spent 0` when it was anchored at block 8732, held
`period 8000, spent 25` after the first settlement, and holds `period 8000,
spent 50` now — in the same 97 bytes as the two limits it is measured against.
Nothing the agent sends can lower any of it, and at 1000 — the anchored
`per_period` — `spend` starts refusing with 6006 until block 9000.

The client agent reads its own ceiling off that account before it offers to pay
(`scripts/a2a-task.sh`, `policy_field`), rather than out of the manifest. A
manifest that disagreed with the chain would be caught there instead of at the
settlement.

Manifest: [`artifacts/a2a-task.tsv`](../artifacts/a2a-task.tsv). It holds only
settlements under the program deployed today. Four earlier ones — `c45d3f24…`,
`8d7aba60…`, `5a488f28…` and `f780df62…` — are real transactions on the same
chain and are why the server's balance does not start at 0, but they were made
against policy accounts that no longer exist. Redeploying moved every policy
PDA, which is the whole reason the anchors above were redone, and keeping
orphaned rows in the same table as live ones is the kind of thing this
repository has been closed for before.

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
seeded by the exact payment. The ceiling it is compared against is not in the
call — `spend` carries only an amount and a period now — it is read out of the
one policy account that agent has.

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
