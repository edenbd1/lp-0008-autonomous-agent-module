# LP-0008 — Autonomous AI Module for Logos Core

A Logos Core module that runs an autonomous agent as a first-class participant
in the Logos stack: it holds its own shielded LEZ account, stores and retrieves
files through Logos Storage, and talks to its owner and to other agents over
encrypted Logos Messaging. Agent-to-agent coordination is **A2A-compatible** —
Agent Cards follow the A2A schema and tasks follow the A2A lifecycle — with
Logos Messaging as the transport A2A leaves open and LEZ transfers as the
payment layer A2A deliberately omits. The spending threshold is not an `if` in
the agent's code but the *address* of a policy account derived from the owner's
limits, so an agent whose process has been taken cannot raise its own ceiling.

> Built for [λPrize LP-0008](https://github.com/logos-co/lambda-prize/blob/master/prizes/LP-0008.md).
> Nothing here is claimed that has not been reproduced, and what does not work
> is written down in [`docs/limitations.md`](docs/limitations.md) rather than
> left to be discovered.

**No transaction hash appears on this page.** Changing the policy program
changes its ImageID, which changes every policy account address, which orphans
every anchor — so a README that quoted hashes would be wrong the first time the
guest is rebuilt. The live values live in [`artifacts/`](artifacts) and
[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md), and every command below *derives*
them from the checkout instead of quoting them.

## Start here

| You want to | Go to |
|---|---|
| check the claims from a clean clone, with nothing installed | [1](#1-prove-it-from-a-clean-clone) |
| read the live deployment back off the chain yourself | [2](#2-read-the-live-deployment-back-yourself) |
| deploy and configure your own three agents | [3](#3-deploy-your-own-agents-cli), [4](#4-configure-the-agent) |
| run an A2A task between two agents and settle it in LEZ | [5](#5-run-an-a2a-task-and-settle-it-in-lez) |
| drive a real Logos Delivery and Logos Storage node | [6](#6-drive-a-real-delivery-and-storage-node) |
| load the module in the Logos app and reach it as its owner | [7](#7-load-the-module-in-the-logos-app-basecamp), [8](#8-the-owner-channel) |
| know what does not work | [10](#10-what-does-not-work) |

## 1. Prove it from a clean clone

```sh
git clone https://github.com/edenbd1/lp-0008-autonomous-agent-module
cd lp-0008-autonomous-agent-module
./scripts/demo.sh
```

A Rust toolchain, `python3` and `curl`. No funded account, no keys, no local
sequencer, no Logos install, and nothing to configure. The script exports
`RISC0_DEV_MODE=0` itself.

It runs the policy crate's adversarial tests; recomputes the deployed program's
transaction hash from the committed binary (a LEZ deploy hash is
`SHA256(borsh(bytecode))`, so the bytes in this repository *are* their own
deployment); asks the public testnet whether that transaction is there;
recomputes the transfer program it chains into and reads that program's id back
off the chain; shows the create_policy attack the previous deployment accepted
and the identical call the current one refuses; and runs each hostile call
against the committed binary itself, paired with the honest call it differs
from in one field.

Two things make it evidence rather than output. Every check either computes a
value locally or fetches it over JSON-RPC in front of you — nothing is
asserted. And a hash that cannot exist is queried as a control, because an RPC
answering non-null to everything would pass the liveness check just as happily.
The exit code is the result; a failing test suite fails the script.

## 2. Read the live deployment back yourself

The program this checkout deploys, derived from the committed binary:

```sh
PROG=$(python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")
echo "$PROG"
```

What is anchored under *exactly* that program — the ledger is keyed by
`(program, policy_hash)`, because a redeploy moves every policy to an address
that has never been initialised:

```sh
awk -F'\t' -v p="$PROG" 'NR==1 || $1==p' artifacts/anchored.tsv | column -t
```

The three agents, one per default skill category, and each one's envelope:

```sh
column -t -s$'\t' artifacts/agents.tsv
```

Every anchor in that manifest, checked against the chain rather than against
this file:

```sh
while IFS=$'\t' read -r cat _ _ _ _ _ _ tx _; do
  [ "$cat" = category ] && continue
  printf '%-11s %s… ' "$cat" "${tx:0:12}"
  curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$tx\"]}" \
  | grep -q '"result":\[' && echo "on chain" || echo "MISSING"
done < artifacts/agents.tsv
```

And the part that matters most, because it is what makes a limit a limit: the
policy account's *address* is derived from the envelope, and the chain says
that account belongs to this program. Both sides are computed here — the
address from the manifest's policy hash, the program id from the committed
binary — and the chain supplies the middle:

```sh
HASH=$(awk -F'\t' '$1=="blockchain"{print $4}' artifacts/agents.tsv)
PDA=$(spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
        pda policy --policy-hash "$HASH" | tail -1)
echo "policy account $PDA"
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$PDA\"]}" \
  | python3 -c "import json,sys; print('owned by', json.load(sys.stdin)['result']['program_owner'])"
spel program-id artifacts/programs/agent_verifier.bin | grep decimal
```

The last two lines print the same eight numbers. An uninitialised account —
which is what a raised limit names — answers `[0,0,0,0,0,0,0,0]` instead.

Explorer links, the deploy reproduction and the settlement record are in
[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md). The explorer indexes roughly an
hour and three quarters behind the sequencer, so the RPC is the immediate
source of truth and the explorer is for a reader arriving later.

## 3. Deploy your own agents (CLI)

One command deploys all three — a Storage agent, a Messaging agent and a
Blockchain agent — funds each one, opens its receiving account, and anchors its
envelope on chain:

```sh
SIGNER=<a funded public account id> ./scripts/deploy-agents.sh
```

You need `spel` and a LEZ `wallet` on `PATH` (or `SPEL_BIN` / `WALLET_BIN`
pointing at them), a wallet home holding `SIGNER`'s key, and enough testnet
balance to fund three agents. Versions are pinned in
[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) — and pin them, because a wallet
home is not portable between LEZ builds: a `wallet` from a different revision
refuses one it did not create, with `missing field 'accounts'` or
`missing field 'sequencer_addr'`. That reads like a corrupted home and is a
version mismatch.

For each agent the script creates a fresh **private** account in its own wallet
home, funds it with an `auth-transfer` to its keys, finds the account that ended
up holding the balance (a shielded transfer creates a new note under the same
keys rather than crediting an existing account, so funding must come before
anchoring), opens and claims a **public** receiving account, derives the policy
hash with the same Rust code the on-chain guest runs, and submits
`create_policy`.

| Variable | Default | What it is |
|---|---|---|
| `SIGNER` | *required* | funded public account id; the default owner and funder |
| `FUNDER` | `$SIGNER` | account that pays the agents — see below |
| `AGENT_HOMES` | `~/.lp0008-agents` | where agent keys live, deliberately outside the repository |
| `LEE_WALLET_HOME_DIR` | `~/.lez-wallet` | the signer's wallet home |
| `SEQUENCER_URL` | `https://testnet.lez.logos.co` | JSON-RPC endpoint |
| `WALLET_BIN`, `SPEL_BIN` | `wallet`, `spel` | binaries to use |
| `FUND_AMOUNT` | `40` | per-agent funding, overridden per category in the script |
| `MANIFEST`, `LEDGER` | `artifacts/agents.tsv`, `artifacts/anchored.tsv` | what it writes |

### Each policy needs its own signer, and why

The three `deploy_agent` lines at the bottom of the script each pass a
**separate public account** as the anchoring signer. That is not tidiness, and
it is the single thing most likely to waste your afternoon if you skip it.
Three independent constraints stack up:

- **`spel` builds every transaction against nonce 0**, while the sequencer
  checks the nonce for exact equality. A signer's *second* program transaction
  is therefore built with a stale nonce, submitted, given a transaction hash,
  and then silently dropped. Nothing reports it.
- **A signer that still carries the default program owner anchors exactly
  once.** On its second anchor the SPEL macro drops its post-state to dodge
  LEZ's rule 7, and the state machine then rejects the transaction as
  `DeclaredAccountMissingFromOutput`. An account some program already owns —
  anything that has ever received a transfer — is exempt from both halves.
- **The funder must not be an anchoring signer.** `auth-transfer send` leaves
  its sender owned by the transfer program while `create_policy` returns its
  signer as a post-state, and a program cannot hand back an account another
  program owns. Sharing one account makes every anchor stop landing, silently,
  the moment the first agent is funded.

So: create three public accounts that have never signed anything
(`wallet account new public`), put their ids on the `deploy_agent` lines, and
keep `FUNDER` separate from all three. The full reasoning, with the sequencer's
own error text, is in [`docs/limitations.md`](docs/limitations.md).

### Re-running it

Anchoring is single-use by construction: `create_policy` is declared
`#[account(init, …)]` and `init` refuses to overwrite. An agent's identity is
stable once funded, so a second run derives the *same* policy hash and is
correctly refused — which looks exactly like a failure. `artifacts/anchored.tsv`
is what tells the two apart; the script reports a refused re-anchor as
already-anchored and leaves the manifest intact.

Agent keys are written under `$AGENT_HOMES`, never into the repository. An
agent whose key is committed is not an agent, and one whose key is thrown away
cannot sign again.

## 4. Configure the agent

Configuration is three numbers, fixed when the agent is deployed:

| | |
|---|---|
| `per_tx` | the largest single payment the agent may make unattended |
| `per_period` | the largest total it may move in one window |
| `period_blocks` | the window, in blocks |

They are not stored anywhere the agent can reach. `compute_policy_hash` folds
(owner account, agent account, all three limits) into one digest, and the
account a spend must present is the PDA seeded by that digest. Raising a limit
does not edit an account — it names a *different* account, which
`create_policy` never initialised, and the spend is refused before the program
body runs. Reconfiguring is therefore a new policy and a new anchor, not a
setting.

Above `per_tx` the agent must present an approval account seeded by the exact
payment — policy, recipient, amount, nonce — and owned by this program. What
each of these buys and what it does not is
[`docs/security-model.md`](docs/security-model.md).

On the module side the same binding is one call, accepted once:

```
configure(ownerAddress, policyHashHex)   // then start()
```

A second `configure` is refused: the binding is the agent's identity, not a
preference. `status()` reports what it is bound to and whether it is running.

## 5. Run an A2A task and settle it in LEZ

```sh
WALLET_BIN=<path to wallet> ./scripts/a2a-task.sh
```

The storage agent publishes a **signed** A2A Agent Card advertising
`storage.upload` at a LEZ price, with an `x-logos` extension block carrying its
LEZ account, its public payment account and `"settlement":
"lez-chained-authenticated-transfer"`. The blockchain agent discovers it, walks
the A2A lifecycle (`submitted → working → completed`), and pays — signing with
**its own** key, with no owner in the loop.

What makes that autonomous is not that nobody was watching. It is that the
chain would have refused it otherwise: the price is inside the client's
anchored per-transaction limit, so `spend` takes the autonomous branch. Raise
the price above that limit and the identical call fails without an owner
approval account seeded by the exact payment.

`spend` moves no balance itself and cannot — LEZ rule 5 refuses a post-state
that debits an account the executing program does not own, and an agent's
account belongs to LEZ's authenticated transfer program — so the policy check
gates a **chained call** into that program, and the privacy circuit proves both
programs and the composition.

Two refusals are worth knowing before you run it:

- The script **will not write its manifest** unless the settlement confirms
  *and* the recipient's balance moves by exactly the price. An earlier version
  of this instruction produced confirmed, on-chain proofs that a policy
  permitted a payment and moved nothing at all.
- `A2A_AGENTS` and `A2A_MANIFEST` override the input and output files.
  `deploy-agents.sh` truncates `artifacts/agents.tsv` at the start of a run and
  fills it in over the following minutes, so a settlement started mid-deploy
  reads a header and no agents.

Results land in [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv), with the
before/after balances the script read off the chain. Only the credit side is
publicly readable — the payer is a shielded account and `getAccount` answers
with the default account for those — which is why each agent keeps a public
receiving account and why its Agent Card advertises that as its payment
address. The trade is written up in [`docs/limitations.md`](docs/limitations.md).

## 6. Drive a real Delivery and Storage node

```sh
./scripts/exercise-nodes.sh
```

It builds `module/tests/delivery_node_drive.c` and
`module/tests/storage_node_drive.c` against `liblogosdelivery` and `libstorage`
and runs them against the live networks. Every step is an assertion and the
exit code is the result, so a node that started and then did nothing cannot
produce a passing transcript. A green run starts a Delivery node, has it report
its own peer id, subscribes, publishes, and waits for the network to propagate
the message back; then starts a Storage node, uploads a file as a session, and
checks that the returned content address resolves to a manifest naming that
file. A stub can return `RET_OK`; it cannot return a content address that
resolves back to the right filename and byte count.

Both libraries are ordinary Nim projects, built from source with no privileged
step and no Nix — two commands each, and one snag (`make` installs `nimble`
outside `PATH`). They are in [`docs/skills.md`](docs/skills.md), along with why
these runs are a local command rather than a CI job: the build takes tens of
minutes and the run depends on live peers, and a job that goes amber on a bad
afternoon teaches everyone to ignore it.

Point `DELIVERY_SRC` and `STORAGE_SRC` at your checkouts if they are not under
`_external/`.

## 7. Load the module in the Logos app (Basecamp)

The loadable asset is committed: `module/agent.lgx`, one `darwin-arm64`
variant. Check it against itself rather than trusting this page — `lgx` is the
packager Basecamp's own packages are built with,
[`docs/basecamp.md`](docs/basecamp.md) says where to get it:

```sh
lgx verify   module/agent.lgx     # contents match the manifest hashes
lgx manifest module/agent.lgx     # type: core, main: agent_plugin.dylib
```

Basecamp 0.2.2 has no "install from file" button — its Package Manager installs
from a configured package repository only — so the module directory is
populated by hand. An installed module is the variant **flattened**, plus a
`variant` file naming it; dropping the archive in does nothing.

```sh
DEST=~/Library/Application\ Support/Logos/LogosBasecamp/modules/agent
mkdir -p "$DEST" && cd "$DEST"
tar xzf /path/to/lp-0008/module/agent.lgx
mv variants/darwin-arm64/* . && rm -rf variants
printf 'darwin-arm64' > variant
ls   # agent_plugin.dylib  manifest.json  metadata.json  variant
```

On Linux the directory is `~/.local/share/Logos/LogosBasecamp/modules`;
`LOGOS_USER_DIR` overrides the base outright, which is the clean way to try
this without touching an existing install.

Building the plugin from source — the four pinned checkouts, the Qt version
that is a *ceiling* rather than a floor, and the `otool` check that costs
nothing and saves an afternoon — is [`docs/basecamp.md`](docs/basecamp.md).
That document also carries the two load harnesses and their recorded output:
`plugin_load_test` drives `QPluginLoader` over the shipped binary, and
`logos_core_load_test` `dlopen`s the real `liblogos_core` out of the installed
`LogosBasecamp.app`, loads the module through the same C API in the same order
as Basecamp's own `main.cpp`, and then calls back into it over the runtime's
own transport: 21 skills listed, each with a parameter schema, `invoke()`
dispatching to every one.

Stated plainly, because a reviewer will check: there is **no click in the
Basecamp GUI** behind that, only Logos Core's C API with Basecamp's own shipped
library; the package is **macOS arm64 only**; there is **no owner-facing UI
plugin**, only the `core` module; and the 21 registered skills **have no ports
wired**, because a port is a `std::function` and a host that loads this as a
plugin has no wire format for one. Each of them therefore refuses *as itself* —
`{"ok":false,"error":"no account to read: …"}` — which is what the harness
asserts, and is the opposite of the failure worth hiding: a module that loads,
answers `skills()` with `[]`, and looks like it works.

## 8. The owner channel

An above-threshold spend is the one case where the agent has to stop and ask.
`OwnerChannel` opens a **reliable** channel — not a bare topic, because a
dropped approval request looks exactly like a refusal — at

```
/lp-0008/1/owner-channel/<owner account>/<agent account>
```

and sends a `spend_approval_request` carrying the protocol id, a correlation
id, the agent, the policy hash, the recipient, the amount as decimal digits
(the chain's amounts are `u128`, which no JSON number holds), the nonce, the
approval marker seed, and an expiry. It re-sends every 15 s and, after 120 s
with no answer, returns `Unreachable` — terminal, never a quiet fallback to
acting alone, which is what the prize requires of an above-threshold payment
that cannot reach its owner.

The owner answers on the same channel with a `spend_approval_reply` carrying
the same `id` and the same terms plus `"decision": "approve"` or `"deny"`. A
reply is only an answer if it agrees on **every** field; one that names a
different policy, recipient, amount, nonce or marker seed is refused rather
than ignored. A reply carrying `per_tx`, `per_period`, `period_blocks` or
`policy` is refused outright — an approval names a payment, it cannot change a
limit — and the only reason to send one is to try.

Today that exchange is exercised in CI against a fake owner — one that can be
made silent, late or hostile on demand, which a real one cannot — and not from
a loaded Basecamp module: the module's delivery port is unwired in the plugin
path (§7). The end-to-end sequencer job checks the other half, on chain: that a
payment outside the envelope is refused when no owner approval account exists
for it. Driving the owner channel from inside the loaded module is the next
item on [`docs/basecamp.md`](docs/basecamp.md)'s open list, and it is listed
there rather than described here as done.

## 9. Tests and CI

```sh
cargo test --workspace --release --locked      # the policy crate, 19 tests
```

The module's C++ suites need no node, no network, no key and no model — which
is the point, since a real dependency cannot be made to fail on demand and a
fake can:

```sh
clang++ -std=c++17 -I<logos-cpp-sdk>/cpp -I/opt/homebrew/include \
  module/tests/skills_test.cpp module/src/*.cpp -o skills_test && ./skills_test
```

The other five suites (`inference`, `wallet_skills`, `program_skills`,
`agent_skills`, `owner_channel`) each compile against their own translation
unit; [`.github/workflows/ci.yml`](.github/workflows/ci.yml) carries every
line, one step per suite so a red X names the suite that broke. That workflow
also runs `demo.sh` from a clean clone and asserts the committed binary still
hashes to the live deploy transaction.

[`.github/workflows/e2e-local-sequencer.yml`](.github/workflows/e2e-local-sequencer.yml)
runs the whole policy lifecycle against a real standalone LEZ sequencer with
`RISC0_DEV_MODE=0`. It has no skip path, deliberately: a competing submission
was closed because its e2e job "completed through its explicit skip path". Run
it locally with `./scripts/e2e-local-sequencer.sh` against a
`logos-execution-zone` checkout.

Compute-unit costs for every on-chain operation are measured in
[`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md).

## 10. What does not work

[`docs/limitations.md`](docs/limitations.md) is the honest state and is meant
to be read before the rest. It carries, among others: that a shielded agent can
pay but cannot be paid at its shielded account, so the payee end of every
settlement is public; that `getAccount` cannot see a private balance, so only
the credit side of a payment is checkable by a third party; that no model has
ever been run against the inference port — the local backend is a stub with an
honest name and the HTTP backend has never made a real request; and that the
node runs in §6 are a local command rather than CI.

Retractions live there too, with what replaced them. If you find a gap that is
not in that file, it is an omission rather than a decision.

## Layout

```
crates/agent-policy-core          the policy hash, the spend reference, the
                                  approval marker — compiled into the guest and
                                  into the host scripts, so the address a script
                                  computes is the address the chain derives
crates/agent-verifier-spel        the SPEL program that enforces the envelope
crates/agent-verifier-adversarial the hostile calls, run against the committed
                                  binary, each paired with the honest call it
                                  differs from in one field
module/src                        the Logos Core module: skill registry, owner
                                  channel, and the ports every skill calls
                                  through
module/tests                      the suites, plus the two load harnesses and
                                  the C node drivers
module/agent.lgx                  the loadable package (darwin-arm64)
scripts/demo.sh                   §1 — the whole thing from a clean clone
scripts/deploy-agents.sh          §3 — three agents, funded and anchored
scripts/a2a-task.sh               §5 — two agents, one A2A task, one settlement
scripts/exercise-nodes.sh         §6 — real Delivery and Storage nodes
scripts/e2e-local-sequencer.sh    §9 — the lifecycle against a real sequencer
artifacts/                        the manifests: agents, anchors, settlements,
                                  Agent Cards, and the deployed program binaries
idl/                              the instruction ABI the CLI drives
docs/                             architecture, security model, skills,
                                  deployment, Basecamp, limitations
```

Reading order for the documents: [`architecture.md`](docs/architecture.md) for
the shape, [`security-model.md`](docs/security-model.md) for what the agent may
do alone, [`skills.md`](docs/skills.md) for which skills are wired to a running
node and which are only compiled, [`DEPLOYMENT.md`](docs/DEPLOYMENT.md) for
what is live, and [`limitations.md`](docs/limitations.md) for what is not.

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your
option.
