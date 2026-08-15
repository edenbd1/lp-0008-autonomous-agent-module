# What each on-chain operation costs

The prize asks us to

> Document the compute unit (CU) cost of each on-chain operation the agent
> performs (token transfers, program calls, deployments) on LEZ devnet/testnet.

This document answers that as far as it can be answered, and says plainly where
it cannot.

## The headline, first

**LEZ v0.2.4 does not meter compute units.** There is no CU, no per-instruction
price, and no fee charged for execution. Searching the pinned revision for the
term returns nothing at all:

```console
$ LEE=~/.cargo/git/checkouts/logos-execution-zone-*/47eba25
$ grep -rniI "compute unit\|compute-unit\|\bCU\b" $LEE --include="*.rs" --include="*.md" | wc -l
0
```

So no number in this document is labelled "CU", because converting to one would
mean inventing the conversion. What LEZ has instead are three real, checkable
budgets, and this document measures our operations against them:

| Budget | Value | Where it lives |
|---|---|---|
| Cycles per public program execution | 33,554,432 (32M) | `MAX_NUM_CYCLES_PUBLIC_EXECUTION`, `lee/state_machine/src/program/mod.rs:15`, applied as a `session_limit` at `:63` |
| Chained calls per transaction | 10 | `MAX_NUMBER_CHAINED_CALLS`, `lee/state_machine/core/src/program/mod.rs:14`, enforced at `validated_state_diff/mod.rs:108-112` |
| Bytes on the wire | no cap; measured below | the sequencer's own `getTransaction` |

The cycle cap carries its own comment on the state of the fee model:

```rust
/// Maximum number of cycles for a public execution.
/// TODO: Make this variable when fees are implemented.
const MAX_NUM_CYCLES_PUBLIC_EXECUTION: u64 = 1024 * 1024 * 32; // 32M cycles
```

The wallet does carry a `GasConfig` — `gas_fee_per_byte_deploy`,
`gas_cost_runtime`, `gas_limit_runtime` and four more
(`lez/wallet/src/config.rs:22-38`) — but at this revision that struct is
declared and referenced nowhere else in the tree. It is a fee model's shape
without a fee model behind it.

Two consequences worth stating rather than leaving for a reader to discover:

- Nothing here is a *price*. On this testnet an operation costs cycles and
  bytes, not tokens. The only balance that moves in a settlement is the payment
  itself.
- The 32M cycle cap is on **public** execution. A privacy-preserving
  transaction is proved by the sender's wallet and verified by the sequencer, so
  it is bounded by what the prover will do rather than by that constant. Our
  settlements take the privacy-preserving path.

## Which program these numbers belong to

A cycle count belongs to one ImageID. Carrying one across a rebuild would be a
claim nobody measured, so the program is named once, here, and every figure
below was produced against it:

| | |
|---|---|
| Binary | `artifacts/programs/agent_verifier.bin`, 417,348 bytes |
| Deploy tx | `a780003b…8576841e` |
| ImageID | `12fa95d9…b578c9d8` |
| Block | 8720 |

**Those four facts are not maintained here.**
[`docs/DEPLOYMENT.md`](../DEPLOYMENT.md) is where the deployment is recorded and
`scripts/verify-deployment.sh` is what checks it against the committed binary
and against the chain. This table is a pointer at that one, and the pointer is
checkable: run the script, and it fails if the binary, the document and the
chain stop agreeing. Everything below quotes the ImageID and the deploy hash by
their short form only, so a redeploy changes this table and nothing else in this
file.

That matters because this document has already been wrong in exactly that way.
It measured and named `8c87cc9b…2d20ebbe` / ImageID `26ed1580…0bad50be` —
correct figures for a program the repository had stopped shipping. Every number
was true on chain and none of them described the binary in the tree, which is
the failure that is invisible to a reader checking the numbers against the
explorer: the answers are right and the question is the wrong one.

## What was measured, and how

Cycle counts come from executing the **deployed binary** above under the risc0
executor, with no proving. That is the same executor the sequencer runs
(`default_executor().execute(env, self.elf())`,
`lee/state_machine/src/program/mod.rs:73-77`), and the harness writes the
guest's four inputs in exactly the order the state machine writes them — program
id, caller program id, pre-states, instruction words (`Program::write_inputs`,
`lee/state_machine/src/program/mod.rs:88-109`).

Measuring the committed artefact rather than a fresh build is deliberate: it is
the artefact that is on chain, and rebuilding it would measure something whose
identity we would then have to argue about. The harness derives the ImageID from
the committed bytes with `compute_image_id` rather than reading it out of a
document, so a binary that is not the deployed one produces a program id that
matches nothing and every PDA below misses.

| | |
|---|---|
| Machine | Apple M4 Pro, 24 GB, macOS 26.5.2, arm64 |
| rustc | 1.95.0 |
| r0vm / cargo-risczero | 3.0.5 (the version the guest pins) |
| Mode | `RISC0_DEV_MODE=0`, execution only, no proving |

### Cycles per instruction

`user_cycles` is the real work. `total_cycles` is that rounded up to the next
power of two, which is what a proof of the segment would actually cost — risc0
pads to a power-of-two segment, so 194,621 cycles and 262,143 cycles prove
identically.

| Program | Instruction | Accounts | user_cycles | total_cycles (2^po2) | % of the 32M cap |
|---|---|---:|---:|---:|---:|
| agent_verifier | `create_policy` | 2 | 116,093 | 262,144 | 0.35% |
| agent_verifier | `approve_spend` | 3 | 199,903 | 262,144 | 0.60% |
| agent_verifier | `spend` (autonomous, fresh period) | 3 | 194,621 | 262,144 | 0.58% |
| agent_verifier | `spend` (autonomous, period already open) | 3 | 194,623 | 262,144 | 0.58% |
| agent_verifier | `spend_approved` | 4 | 253,426 | 524,288 | 0.76% |
| authenticated_transfer | `Transfer`, payee already claimed | 2 | 81,767 | 131,072 | 0.24% |
| authenticated_transfer | `Transfer`, payee unclaimed | 2 | 82,127 | 131,072 | 0.24% |
| authenticated_transfer | `Transfer`, chained from `spend` | 2 | 84,349 | 131,072 | 0.25% |

Every one of those runs exits `Halted(0)`; they are successful executions, not
early rejections. A single segment each, so continuation overhead is zero.

The three `authenticated_transfer` rows are unchanged from the previous
revision of this table, and that is a check rather than a coincidence:
`artifacts/programs/authenticated_transfer.bin` did not change across the
redeploy, so its cycle counts must not either. Cycle counts are deterministic
per input, so a transfer row that had moved would mean the harness had changed
under us rather than the program.

**A token transfer by the agent is two of those rows, not one.** `spend` moves
no balance itself — LEZ rule 5 forbids a program from debiting an account it
does not own — so it checks the envelope and chains a call into the transfer
program, which does. The full autonomous settlement is therefore
194,621 + 84,349 = **278,970 user cycles across two program executions**, or
0.83% of one public-execution budget. Each execution in the chain is a separate
session with its own cap, and the chain may be 10 deep; ours is 1.

Three things fall out of the table:

- **Account count dominates.** Each declared account is a `AccountWithMetadata`
  deserialised on the way in and re-serialised into the journal on the way out.
  Going from three accounts to four is what pushes `spend_approved` over the
  2^18 boundary and doubles its padded cost, even though the extra logic — one
  hash, one comparison — is a rounding error.
- **The period ledger is now free, and that is a change from the previous
  program rather than a re-measurement of it.** `spend` against a policy account
  whose ledger is empty costs 194,621; against one already carrying a total for
  the current period, 194,623. Two cycles, for a branch.

  Under `8c87cc9b…` the same two rows differed by 6,666, and the difference was
  real: that program decoded a 24-byte ledger only when there was one to decode.
  The record this program writes is a fixed 97-byte `PolicyRecord` — version,
  owner, three limits, window and total — so the ledger is deserialised on every
  `spend` whether it holds anything or not, and the two paths then differ by one
  window comparison. The per-period accounting did not get cheaper; it moved
  into a cost that was already being paid. Anyone carrying the old 6,666 forward
  would be describing a program this repository no longer ships.
- **`spend_approved` no longer has two prices.** Earlier revisions of this table
  listed it twice, below and above threshold, because the instruction used to
  skip the marker checks when the payment happened to fall inside the envelope.
  It does not any more — the approval is required whatever the amount, and
  anything inside the envelope belongs in `spend`, where it is accounted against
  the period. One path, one number.
- **Chaining is cheap in cycles and expensive in proof time.** The callee costs
  2,582 cycles more when invoked as a chained call than when invoked directly
  (84,349 vs 81,767), because it carries a caller program id. What chaining
  actually costs is proving, and that is measured upstream rather than here —
  see the gaps below.

### The harness is cross-checked against LEZ's own numbers

LEZ publishes its own cycle bench for the built-in programs
(`logos-execution-zone/docs/benchmarks/cycle_bench.md` at rev `47eba25` — its
repository, not this one) and reports
`authenticated_transfer / Transfer` at **79,958 user cycles** on an Apple M2 Pro.
We measure **81,767** for the same instruction on different hardware with
different account contents — 2.3% higher, in the direction a non-empty payee
account explains. Cycle counts are deterministic per input, so agreement to
that margin against an independently produced number is the check that the
harness is feeding the guest something real, rather than measuring a program
that fell over on the first input it read.

### Bytes on chain, read off the public testnet

Sizes are the exact payload `getTransaction` returns, base64-decoded, for
transactions this repository actually produced.

Every row is under the program named at the top of this document. The three
anchors are the `create_tx` column of [`artifacts/agents.tsv`](../../artifacts/agents.tsv)
and the two settlements are the `settlement_tx` column of
[`artifacts/a2a-task.tsv`](../../artifacts/a2a-task.tsv), so a redeploy
regenerates the hashes in one place and this table is read out of it rather than
maintained alongside it.

| Operation | Transaction | Block | Bytes | Kind |
|---|---|---:|---:|---|
| Program deployment | `a780003b…8576841e` | 8720 | 417,353 | deploy |
| `create_policy` (anchor an envelope) | `79c91ec7…a24b70f5` | 8729 | 397 | public |
| `create_policy` | `eb294055…38e53249` | 8731 | 397 | public |
| `create_policy` | `0266f48b…d3cae867` | 8732 | 397 | public |
| `spend` settlement, chained transfer | `4e3a3454…a490ddb1` | 8740 | 271,471 | privacy-preserving |
| `spend` settlement, chained transfer | `7cad4fbd…7168f019` | 8747 | 271,471 | privacy-preserving |

Reproduce any row:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["<hash>"]}' \
| python3 -c "import sys,json,base64; r=json.load(sys.stdin)['result']; \
print('block', r[1], len(base64.b64decode(r[0])), 'bytes')"
```

What the sizes mean:

- **A deployment is its own bytecode.** 417,353 = the 417,348-byte program
  binary plus a five-byte frame, and the deploy transaction hash is
  `SHA256(u32_le(len) || bytecode)` — content addressed, which is why
  `scripts/demo.sh` can recompute it from the committed file. Deployment is
  idempotent: redeploying identical bytes is a no-op.
- **A policy anchor is 397 bytes.** `create_policy` is signed by a public owner
  account and takes the public path, so it carries no proof — it re-executes on
  the sequencer instead. The first byte is the transaction kind (`00`), followed
  by the 32-byte ImageID of the program being called, so the anchor names the
  program it anchors under and you can read that ImageID straight out of the
  payload:

  ```console
  $ curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
      -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["79c91ec796a14b7c0c2df11ac96ff944f915fe767db397b31331c48fa24b70f5"]}' \
    | python3 -c "import sys,json,base64; b=base64.b64decode(json.load(sys.stdin)['result'][0]); \
  print('kind', b[0], 'imageid', b[1:33].hex())"
  kind 0 imageid 12fa95d9382121791f11feb4ac6f7e3ee19e20d296f2eb09a61a069eb578c9d8
  ```

  That is the check worth running rather than the size: an anchor made under a
  superseded program is a valid transaction that reads correctly and secures
  nothing this repository ships, and the ImageID in its payload is the only
  thing that says which.
- **A settlement is about 271 kB,** two orders of magnitude larger, and
  essentially all of it is the proof. That the size is a property of the privacy
  circuit rather than of our instruction is visible in how little it moves
  across three generations of the program:

  | Under program | Settlements | Bytes |
  |---|---|---:|
  | `b028eabf…` | `c45d3f24…` (8605), `8d7aba60…` (8624) | 270,566 each |
  | `8c87cc9b…` | `5a488f28…` (8677), `f780df62…` (8686) | 270,718 and 270,814 |
  | `a780003b…` (shipped) | `4e3a3454…` (8740), `7cad4fbd…` (8747) | 271,471 each |

  A 905-byte spread across two program rebuilds, under 0.34%, while the guest
  gained identity bindings on anchoring and paying and moved the period total on
  chain. Upstream reports the proof itself as fixed-size — "`proof_bytes` is
  constant: the outer succinct proof has fixed size" — and the residue is the
  rest of the transaction rather than the proof. Under `8c87cc9b…` the two
  settlements differed from *each other* by 96 bytes; under the shipped program
  they are byte-identical in length. So "constant" is a claim about the proof,
  not about the transaction carrying it, and the transaction's own variation is
  what the payload happens to hold.

Latency, for scale: blocks on this chain are 60 seconds apart, and each of the
transactions above landed in the block after it was submitted. The wall-clock
cost of a settlement is dominated by proving before submission, not by
inclusion.

## What this does not establish

Stated as gaps, not buried.

1. **No CU figure, and no conversion to one.** LEZ does not define the unit at
   this revision. If a later testnet phase introduces one, the cycle counts
   above are the input it will be computed from — that is precisely what LEZ's
   own bench calls them, "inputs for the fee model's `G_executor`, `G_prove`,
   `G_verify` and `S_agg` parameters" — but the multiplier does not exist yet
   and is not guessed here.
2. **Proving cost is not measured by us.** The numbers above are execution.
   Producing the privacy-preserving proof is the expensive half and it happens
   in the sender's wallet, over the whole composed execution, not over our guest
   alone; measuring our guest standalone would produce a number that is not the
   cost of anything anyone pays. For scale, upstream measured on an M2 Pro:
   ≈ 13.7 s to prove `authenticated_transfer / Transfer` standalone, ≈ 61.5 s
   wrapped in the privacy circuit, and ≈ 53 s per additional chained call
   (`logos-execution-zone/docs/benchmarks/cycle_bench.md`). Those are upstream's
   numbers on upstream's hardware for upstream's programs, quoted for scale and
   not reproduced here.
3. **Sequencer-side verification cost is not measured by us.** Upstream reports
   ≈ 12.2 ms to verify one privacy-preserving receipt.
4. **No devnet numbers.** Everything above is the public testnet at
   `https://testnet.lez.logos.co`.
5. **The other on-chain operations the agent can perform are the same two rows.**
   A "program call" by this agent *is* `spend` or `spend_approved`; a "token
   transfer" is the chained `authenticated_transfer / Transfer`. The agent has
   no third kind of on-chain operation, so the table is complete rather than
   selective.

## Reproducing the cycle counts

The measurement harness is not committed — it depends on `lee_core` at a pinned
git revision and would add a second Rust workspace to a repository that
deliberately has one. It is small enough to state in full, and most of it is
already in the tree: `crates/agent-verifier-adversarial` loads the same binary
into the same executor with pre-states built the same way, and differs only in
asserting error codes instead of summing cycles. Start from that file if you
would rather not retype this one. Note that it enables risc0's `prove` feature,
which drags in GPU kernels; for execution-only measurement the feature set below
is enough and builds without a Metal or CUDA toolchain.

`Cargo.toml`:

```toml
[dependencies]
risc0-zkvm = { version = "=3.0.5", default-features = false, features = ["client", "std"] }
nssa_core = { git = "https://github.com/logos-blockchain/logos-execution-zone.git", rev = "47eba256479f6f785acbd138834340703cd03401", features = ["host"], package = "lee_core" }
agent-policy-core = { path = "crates/agent-policy-core", features = ["std"] }
serde = { version = "1", features = ["derive"] }
```

The instruction enum is mirrored from the guest. Only the **order** of the
variants and of their fields is on the wire — `#[lez_program]` generates the
enum from the `#[instruction]` functions in declaration order, so
`create_policy` is 0, `approve_spend` 1, `spend` 2, `spend_approved` 3. The
`ctx: ProgramContext` parameter — taken by `approve_spend`, `spend` and
`spend_approved`, though not by `create_policy` — is injected by the dispatcher
from the trusted `ProgramInput` and is not part of the ABI, so it does not
appear in the mirrored enum or in the published IDL.

```rust
let words: Vec<u32> = risc0_zkvm::serde::to_vec(&instruction)?;
let env = ExecutorEnv::builder()
    .write(&program_id)?                    // ImageID, from `spel program-id`
    .write(&Option::<ProgramId>::None)?     // caller: None at top level
    .write(&pre_states)?                    // Vec<AccountWithMetadata>
    .write(&words)?
    .build()?;
let session = default_executor().execute(env, &elf)?;
let user: u64 = session.segments.iter().map(|s| u64::from(s.cycles)).sum();
let total: u64 = session.segments.iter().map(|s| 1u64 << s.po2).sum();
```

The accounts have to be the ones the instruction expects or the guest refuses
before doing the work, and a cycle count for a refusal is not a cycle count for
the operation — every row in the table above exits `Halted(0)`. For `spend`: the
policy account is
`AccountId::for_public_pda(&program_id, &PdaSeed::new(policy_hash))` with its
`program_owner` set to the program itself, the agent account is
`is_authorized: true`, owned by the transfer program, and its **account id must
equal the `agent_id` the policy commits to** or the run halts with 6013; the
recipient is whatever the payment names. `window_start` must be a multiple of
`period_blocks`, or 6014.

The two `spend` rows use the same anchored policy — `per_tx` 200, `per_period`
1000, `period_blocks` 1000 — and spend 200 at `window_start` 8000. They differ
only in the `SpendLedger` inside the 97-byte `PolicyRecord` the policy account
holds: `SpendLedger::default()` for the fresh row, and
`{ window_start: 8000, spent: 100 }` for the open one, which leaves room for the
200 so that row also halts 0. A ledger that left no room would halt with 6006,
and a refusal's cycle count is not the operation's.

The same harness run against deliberately wrong inputs is what produces the
refusal table in [`docs/security-model.md`](../security-model.md) — same binary,
same method, opposite expectation.
