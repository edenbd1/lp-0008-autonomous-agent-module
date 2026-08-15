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

## What was measured, and how

Cycle counts come from executing the **deployed binary** —
`artifacts/programs/agent_verifier.bin`, the bytes that hash to deploy
transaction `b028eabf…b8c18549` — under the risc0 executor, with no proving.
That is the same executor the sequencer runs (`default_executor().execute(env,
self.elf())`, `lee/state_machine/src/program/mod.rs:73-77`), and the harness
writes the guest's four inputs in exactly the order the state machine writes
them — program id, caller program id, pre-states, instruction words
(`Program::write_inputs`, `lee/state_machine/src/program/mod.rs:88-109`).

Measuring the committed artefact rather than a fresh build is deliberate: it is
the artefact that is on chain, and rebuilding it would measure something whose
identity we would then have to argue about.

| | |
|---|---|
| Machine | Apple M4 Pro, 24 GB, macOS 26.5.2, arm64 |
| rustc | 1.95.0 |
| r0vm / cargo-risczero | 3.0.5 (the version the guest pins) |
| Mode | `RISC0_DEV_MODE=0`, execution only, no proving |

### Cycles per instruction

`user_cycles` is the real work. `total_cycles` is that rounded up to the next
power of two, which is what a proof of the segment would actually cost — risc0
pads to a power-of-two segment, so 185,885 cycles and 262,143 cycles prove
identically.

| Program | Instruction | Accounts | user_cycles | total_cycles (2^po2) | % of the 32M cap |
|---|---|---:|---:|---:|---:|
| agent_verifier | `create_policy` | 2 | 119,782 | 262,144 | 0.36% |
| agent_verifier | `approve_spend` | 3 | 156,169 | 262,144 | 0.47% |
| agent_verifier | `spend` (autonomous) | 3 | 185,885 | 262,144 | 0.55% |
| agent_verifier | `spend_approved`, below threshold | 4 | 242,986 | 524,288 | 0.72% |
| agent_verifier | `spend_approved`, above threshold | 4 | 245,935 | 524,288 | 0.73% |
| authenticated_transfer | `Transfer`, payee already claimed | 2 | 81,391 | 131,072 | 0.24% |
| authenticated_transfer | `Transfer`, payee unclaimed | 2 | 81,751 | 131,072 | 0.24% |
| authenticated_transfer | `Transfer`, chained from `spend` | 2 | 83,980 | 131,072 | 0.25% |

Every one of those runs exits `Halted(0)`; they are successful executions, not
early rejections. A single segment each, so continuation overhead is zero.

**A token transfer by the agent is two of those rows, not one.** `spend` moves
no balance itself — LEZ rule 5 forbids a program from debiting an account it
does not own — so it checks the envelope and chains a call into the transfer
program, which does. The full autonomous settlement is therefore
185,885 + 83,980 = **269,865 user cycles across two program executions**, or
0.80% of one public-execution budget. Each execution in the chain is a separate
session with its own cap, and the chain may be 10 deep; ours is 1.

Three things fall out of the table:

- **Account count dominates.** Each declared account is a `AccountWithMetadata`
  deserialised on the way in and re-serialised into the journal on the way out.
  Going from three accounts to four is what pushes `spend_approved` over the
  2^18 boundary and doubles its padded cost, even though the extra logic — one
  hash, one comparison — is a rounding error.
- **The approval check is nearly free.** Above threshold `spend_approved` costs
  2,949 more cycles than below it (245,935 vs 242,986): a SHA-256 spend
  reference, a marker derivation, a program-owner comparison and a period
  comparison. The security argument in `docs/security-model.md` costs 1.2% of
  the instruction.
- **Chaining is cheap in cycles and expensive in proof time.** The callee costs
  2,589 cycles more when invoked as a chained call than when invoked directly
  (83,980 vs 81,391), because it carries a caller program id. What chaining
  actually costs is proving, and that is measured upstream rather than here —
  see the gaps below.

### The harness is cross-checked against LEZ's own numbers

LEZ publishes its own cycle bench for the built-in programs
(`logos-execution-zone/docs/benchmarks/cycle_bench.md` at rev `47eba25` — its
repository, not this one) and reports
`authenticated_transfer / Transfer` at **79,958 user cycles** on an Apple M2 Pro.
We measure **81,391** for the same instruction on different hardware with
different account contents — 1.8% higher, in the direction a non-empty payee
account explains. Cycle counts are deterministic per input, so agreement to
that margin against an independently produced number is the check that the
harness is feeding the guest something real, rather than measuring a program
that fell over on the first input it read.

### Bytes on chain, read off the public testnet

Sizes are the exact payload `getTransaction` returns, base64-decoded, for
transactions this repository actually produced.

| Operation | Transaction | Block | Bytes | Kind |
|---|---|---:|---:|---|
| Program deployment | `b028eabf…b8c18549` | 8590 | 413,917 | deploy |
| `create_policy` (anchor an envelope) | `ab017c9c…d67735f2` | 8591 | 653 | public |
| `create_policy` | `9373d809…92df8104` | 8594 | 653 | public |
| `spend` settlement, chained transfer | `c45d3f24…94cf7275` | 8605 | 270,566 | privacy-preserving |

Reproduce any row:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getTransaction","params":["<hash>"]}' \
| python3 -c "import sys,json,base64; r=json.load(sys.stdin)['result']; \
print('block', r[1], len(base64.b64decode(r[0])), 'bytes')"
```

What the three sizes mean:

- **A deployment is its own bytecode.** 413,917 = the 413,912-byte program
  binary plus a five-byte frame, and the deploy transaction hash is
  `SHA256(u32_le(len) || bytecode)` — content addressed, which is why
  `scripts/demo.sh` can recompute it from the committed file. Deployment is
  idempotent: redeploying identical bytes is a no-op.
- **A policy anchor is 653 bytes.** `create_policy` is signed by a public owner
  account and takes the public path, so it carries no proof — it re-executes on
  the sequencer instead. The first byte is the transaction kind (`00`), followed
  by the 32-byte ImageID of the program being called; you can read
  `15d234e5…` straight out of the payload.
- **A settlement is 270,566 bytes,** two orders of magnitude larger, and
  essentially all of it is the proof. That size is a property of the privacy
  circuit rather than of our instruction: the earlier settlement `aea80817…` — an
  earlier program, which [`limitations.md`](../limitations.md) records as having
  authorised a payment without moving one — is 270,566 bytes too. Upstream reports the same constancy — "`proof_bytes` is
  constant: the outer succinct proof has fixed size".

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
deliberately has one. It is small enough to state in full.

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
`ctx: ProgramContext` parameter of `spend_approved` is injected by the
dispatcher and is not part of the ABI.

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
the operation. For `spend`: the policy account is
`AccountId::for_public_pda(&program_id, &PdaSeed::new(policy_hash))` with its
`program_owner` set to the program itself, the agent account is
`is_authorized: true` and owned by the transfer program, and the recipient is
whatever the payment names. The same harness run against deliberately wrong
inputs is what produces the refusal table in
[`docs/security-model.md`](../security-model.md) — same binary, same method,
opposite expectation.
