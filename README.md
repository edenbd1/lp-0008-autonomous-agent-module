# LP-0008 — Autonomous AI Module for Logos Core

A Logos Core module that runs an autonomous agent as a first-class participant
in the Logos stack: it holds its own shielded LEZ account, stores and retrieves
files through Logos Storage, and talks to its owner and to other agents over
encrypted Logos Messaging. Agent-to-agent coordination is **A2A-compatible** —
Agent Cards follow the A2A schema and tasks follow the A2A lifecycle — with
Logos Messaging as the transport A2A leaves open and LEZ transfers as the
payment layer A2A deliberately omits. The spending threshold is not an `if` in
the agent's code but a policy account on chain — **one per agent**, at an
address seeded by the agent id alone, its limits written once by the owner and
writable afterwards by nothing but this program. An agent whose process has been
taken cannot raise its own ceiling: there is no second address to anchor a
larger one at, and the write itself is refused.

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
| call the agent's skills from a shell, and see it read the chain | [5](#5-talk-to-the-agent-and-add-a-skill-of-your-own) |
| write a skill of your own and load it into the module | [5](#adding-a-skill), [`docs/skills.md`](docs/skills.md) |
| run an A2A task between two agents and settle it in LEZ | [6](#6-run-an-a2a-task-and-settle-it-in-lez) |
| drive a real Logos Delivery and Logos Storage node | [7](#7-drive-a-real-delivery-and-storage-node) |
| load the module in the Logos app and reach it as its owner | [8](#8-load-the-module-in-the-logos-app-basecamp), [9](#9-the-owner-channel) |
| know what does not work | [11](#11-what-does-not-work) |

## 1. Prove it from a clean clone

```sh
git clone --depth 1 https://github.com/edenbd1/lp-0008-autonomous-agent-module
cd lp-0008-autonomous-agent-module
./scripts/demo.sh
```

**`--depth 1` on purpose, and it is worth the four characters.** Measured just
now, on this machine, against this repository: `--depth 1` takes **9 seconds and
52 MB**, a full clone takes **57 seconds and 222 MB**. The difference is
history, not content — `module/agent.lgx` is an 18 MB packaged plugin and
thirty-five versions of it have been committed as it was rebuilt. Nothing below
needs any of them. Drop the flag if you want the history; the working tree is
identical either way.

A Rust toolchain, `python3` and `curl`. No funded account, no keys, no local
sequencer, no Logos install, and nothing to configure. The script exports
`RISC0_DEV_MODE=0` itself, and refuses up front — naming what is missing and
where to get it — rather than failing partway through on a machine that has no
compiler.

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

`python3` and `curl`, plus **`spel` on `PATH`** for the address derivation at
the end of this section — the same binary §3 needs, pinned in
[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md). Everything above that runs without
it.

The program this checkout deploys, derived from the committed binary:

```sh
PROG=$(python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")
echo "$PROG"
```

Every manifest below is read **by column name**. Paste this first — the rest of
the section uses it, and it exits non-zero rather than printing the wrong field
when a column it names is not there:

```sh
col() {   # col <file> <column> [<category>] — one field, or the whole column
  awk -F'\t' -v want="$2" -v key="${3-}" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==want) c=i
            if (!c) { print "no \"" want "\" column in " FILENAME > "/dev/stderr"; exit 2 }
            next }
    key=="" { print $c; next }
    $1==key { print $c; exit }' "$1"
}
```

These manifests gain and lose columns as the program changes — `agents.tsv`
carried a `policy_hash` where it now carries `policy_account` — so a `$4` that
was right last deployment silently reads a different field this one. Three
false results published from this repository came from exactly that. Reading by
position is the bug; the helper is the fix, and it is used everywhere below.

What is anchored under *exactly* that program — the ledger is keyed by
`(program, what, agent_id)`, because a redeploy moves every account to an
address that has never been initialised, and because anchoring is now two
single-use steps per agent (`claim_agent`, then `create_policy`):

```sh
awk -F'\t' -v p="$PROG" '
  NR==1 { for (i=1;i<=NF;i++) if ($i=="program") c=i
          if (!c) { print "no \"program\" column" > "/dev/stderr"; exit 2 }
          print; next }
  $c==p' artifacts/anchored.tsv | column -t
```

The three agents, one per default skill category, and each one's envelope:

```sh
column -t -s$'\t' artifacts/agents.tsv
```

Every anchor in that manifest, checked against the chain rather than against
this file:

```sh
for cat in $(col artifacts/agents.tsv category); do
  tx=$(col artifacts/agents.tsv create_tx "$cat")
  printf '%-11s %s… ' "$cat" "${tx:0:12}"
  curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$tx\"]}" \
  | grep -q '"result":\[' && echo "on chain" || echo "MISSING"
done
```

And the part that matters most, because it is what makes a limit a limit. Each
agent has exactly **one** policy account, at `PDA(program, ["agent-policy/v1",
agent_id])` — the agent and nothing else. Both sides are computed here — the
address from the agent id in the manifest, the program id from the committed
binary — and the chain supplies the middle:

```sh
AGENT=$(col artifacts/agents.tsv agent_id blockchain)
AGENT_HEX=$(python3 -c "
import sys
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
n=0
for ch in sys.argv[1]: n = n*58 + A.index(ch)
print(n.to_bytes(32,'big').hex())" "$AGENT")
PDA=$(spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
        pda policy --agent-id "$AGENT_HEX" | tail -1)
echo "policy account $PDA"
[ "$PDA" = "$(col artifacts/agents.tsv policy_account blockchain)" ] \
  && echo "               and that is the address the manifest records" \
  || echo "               MANIFEST DISAGREES"
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$PDA\"]}" \
| python3 -c "
import json,sys
r=json.load(sys.stdin)
if 'result' not in r: sys.exit('the RPC returned no result: %s' % r)
print('owned by', r['result']['program_owner'])"
spel program-id artifacts/programs/agent_verifier.bin | grep decimal
```

The last two lines print the same eight numbers: the chain agrees that this
address belongs to the program these bytes build. An address no `create_policy`
ever reached answers `[0,0,0,0,0,0,0,0]` instead — which is what every policy
account becomes the moment the guest is rebuilt.

The limits are **not** in that address. They are the account's *data*, written
once by `create_policy` and afterwards only by `spend`, which is what advances
the running period total. Read the record back and compare it to the manifest —
the chain is the authority here, `agents.tsv` is only a note of what was sent:

```sh
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$PDA\"]}" \
| python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
if len(d)!=97 or d[0]!=1: sys.exit('not a record this program wrote: %r' % d)
n=lambda a,b: int.from_bytes(d[a:b],'little')   # version|owner|per_tx|per_period|period|window|spent
print('chain    per_tx %d  per_period %d  period %d blocks  (window %d, spent %d)'
      % (n(33,49), n(49,65), n(65,73), n(73,81), n(81,97)))"
printf 'manifest per_tx %s  per_period %s  period %s blocks\n' \
  "$(col artifacts/agents.tsv per_tx blockchain)" \
  "$(col artifacts/agents.tsv per_period blockchain)" \
  "$(col artifacts/agents.tsv period_blocks blockchain)"
```

That is the whole difference from the previous design, and it is the reason
this one holds. The limits used to be folded into the address, so raising one
*named a different account* — and anchoring a fresh, unlimited policy at that
address was always available to whoever held the agent's key. Now there is one
address per agent, `init` gives it to whoever writes first, and the owner writes
it when it creates the agent. A raised limit is no longer a different address;
it is a write this program refuses. The attack, the transaction that proves the
old program accepted it, and why the fix was not another comparison are in
[`docs/limitations.md`](docs/limitations.md).

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

**Before you run it, you need all five of these.** Nothing is fetched
automatically, and the fourth needs a faucet:

1. a LEZ `wallet` binary at the pinned revision, on `PATH` or at `WALLET_BIN`;
2. `spel`, built from `vendor/spel`, on `PATH` or at `SPEL_BIN`;
3. a wallet home holding `SIGNER`'s key, at `LEE_WALLET_HOME_DIR`
   (default `~/.lez-wallet`);
4. **testnet balance on `SIGNER`** — 65 LEZ covers the three funding floors
   (5 + 55 + 5). This is the step that cannot be scripted;
5. network reach to `SEQUENCER_URL`.

Pin the versions ([`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md)), because a wallet
home is not portable between LEZ builds: a `wallet` from a different revision
refuses one it did not create, with `missing field 'accounts'` or
`missing field 'sequencer_addr'`. That reads like a corrupted home and is a
version mismatch.

### Deploy *and* configure, in one command

The criterion this repository is answering asks for both halves at once — "the
owner can deploy the agent **and** configure it with a single CLI command on any
machine using Logos Core headless". The command above is the chain half; §4's is
the Logos Core half. This is the one command that is both:

```sh
SIGNER=<a funded public account id> ./scripts/deploy-and-configure.sh storage
```

It runs the two in order and owns the seam between them:

1. it checks every prerequisite of **both** halves first, and names each missing
   one. The Logos Core half is asked whether it can run — by running its own
   gate, `logos-core-headless.sh --check`, rather than by keeping a second copy
   of its platform table — **before** anything is submitted. Anchoring is not
   idempotent: `claim_agent` and `create_policy` are both `#[account(init)]` and
   a landed claim cannot be rewritten, so a machine that discovers it has no
   `liblogos_core` after three agents are anchored has paid real transactions to
   find that out;
2. it runs `deploy-agents.sh`;
3. it reads the seam. The agent id, the owner and the policy account are read
   **by header name out of the manifest the chain half just wrote** — never
   passed on a command line — and the row is checked against the chain before
   anything is configured: the first 73 bytes of the policy account must be the
   record the row claims. `artifacts/agents.tsv` is committed in this
   repository, so a chain half that failed would otherwise leave a perfectly
   valid manifest naming *somebody else's* agents;
4. it runs `logos-core-headless.sh`, which installs, loads, configures and
   starts the module against that row.

| Flag | What it does |
|---|---|
| `--dry-run` | check both halves and stop. Nothing is submitted, installed or started |
| `--configure-only` | skip the chain half — the agents in the manifest are already anchored. The row is still checked against the chain. This is the mode for the agents **this repository publishes**, whose manifest is committed, and it costs no transaction |
| `--alongside` | load the agent beside the wallet, storage and messaging modules |

Exit `0` means both halves ran; `1` means a prerequisite was missing or the
chain half failed, and nothing was configured; `2` is reserved for the one state
that costs money — the chain half succeeded and the Logos Core half did not, so
agents are anchored and the configuration is unfinished. `--configure-only`
finishes it for free, and the message says so.

**It is one command given three inputs it cannot produce**: a funded `SIGNER`
(the faucet — see the list above), a wallet home holding that key, and a Logos
Core runtime (§4). Each is checked by name before anything is spent, and
[`docs/limitations.md`](docs/limitations.md) lists all three as prose.

You do **not** need to edit this script. It used to require that — three
hardcoded account ids at the bottom of it made the `SIGNER` fallback beside them
unreachable — and the failure that caused was expensive rather than merely
annoying: it funded an agent, landed its single-use `claim_agent` naming an
owner your wallet cannot sign for, and only then failed. That agent could never
be anchored by anyone afterwards. The full account is in
[`docs/limitations.md`](docs/limitations.md).

For each agent the script creates a fresh **private** account in its own wallet
home, funds it with an `auth-transfer` to its keys, finds the account that ended
up holding the balance (a shielded transfer creates a new note under the same
keys rather than crediting an existing account, so funding must come before
anchoring), opens and claims a **public** receiving account, resolves that
agent's one policy account with `spel` from the published IDL — the same seeds
the guest declares, rather than a second implementation of them — and submits
`create_policy`.

| Variable | Default | What it is |
|---|---|---|
| `SIGNER` | *required* | funded public account id. The **funder**, and the account that deploys the program — not the owner |
| `FUNDER` | `$SIGNER` | account that pays the agents — see below |
| `SIGNER_STORAGE`, `SIGNER_MESSAGING`, `SIGNER_BLOCKCHAIN` | created for you | the account that anchors each agent's policy, and owns it afterwards |
| `AGENT_HOMES` | `~/.lp0008-agents` | where agent keys live, deliberately outside the repository |
| `LEE_WALLET_HOME_DIR` | `~/.lez-wallet` | the signer's wallet home; the anchoring signers are created here |
| `SEQUENCER_URL` | `https://testnet.lez.logos.co` | JSON-RPC endpoint |
| `WALLET_BIN`, `SPEL_BIN` | `wallet`, `spel` | binaries to use |
| `FUND_AMOUNT` | `40` | per-agent funding, overridden per category in the script |
| `MANIFEST`, `LEDGER` | `artifacts/agents.tsv`, `artifacts/anchored.tsv` | what it writes |
| `SIGNERS` | a `signers.tsv` beside the manifest | which local account anchors which agent. Created on first run, gitignored: it names accounts only your wallet holds |

### Each policy needs its own signer, and why

Each agent is anchored by a **separate public account**, created for you in
`LEE_WALLET_HOME_DIR` and recorded in a `signers.tsv` beside the manifest so a
resumed run reuses the same three. Set `SIGNER_STORAGE` / `SIGNER_MESSAGING` /
`SIGNER_BLOCKCHAIN` if you would rather supply your own — the script checks your
wallet home actually holds the key *before* it funds or claims anything, and
refuses the run if not. That is not tidiness, and it is the single thing most
likely to waste your afternoon if you skip it. Three independent constraints
stack up:

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

So: three public accounts that have never signed anything, and a `FUNDER` that
is none of them. `wallet account new public`, three times, is exactly what the
script does for you — the accounts are local and free to create, and creating
them submits nothing. Keep the wallet home afterwards: those three keys are what
may later call `update_policy` and `approve_spend` for each agent. The full
reasoning, with the sequencer's own error text, is in
[`docs/limitations.md`](docs/limitations.md).

### Re-running it

Anchoring is single-use by construction, twice over: `claim_agent` and
`create_policy` are both declared `#[account(init, …)]` and `init` refuses to
overwrite. An agent's identity is stable once funded, so a second run resolves
the *same* two accounts and is correctly refused — which looks exactly like a
failure. `artifacts/anchored.tsv` is what tells the two apart; the script reports
a refused repeat as already-done and leaves the manifest intact.

Fund before you claim, and never after. A shielded transfer does not credit an
existing note, it mints a **new** one with a new account id, so an agent funded
after it has claimed is an agent whose claim and policy sit at addresses nothing
will look at again.

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

They are not stored anywhere the agent can reach. Each agent has exactly **one**
policy account, at `PDA(program, ["agent-policy/v1", agent_id])` — seeded by the
agent id and nothing else — and the three numbers are that account's data, which
only this program may write (LEZ rule 6, `UnauthorizedDataModification`).

Writing that account takes **two signatures, in two transactions, from two
wallets**. The agent signs `claim_agent`, which writes the id of the one account
allowed to anchor over it into `PDA(program, ["agent-owner/v1", agent])` — an
address derived from the signing account, so there is nothing there to
substitute. That account then signs `create_policy`, which refuses any other
signer (6020) and refuses outright if the agent never claimed (6019). The owner
it records is the account that *signed*; there is no `owner_id` argument, so the
claim and the fact cannot differ, and `approve_spend` later compares its signer
against that recorded owner rather than against anything the caller supplies.

The previous deployment had only half of this. Its `create_policy` never
declared the agent's account at all, so anchoring a policy over somebody else's
agent needed only that agent's **public id** — which this file tells you how to
read out of the manifest. That is on chain: `eedb3caf…` at block 8869 is a
stranger anchoring `per_tx = u128::MAX` over an agent it does not control, and
the identical call to the program shipped here was never included.

Raising a limit is therefore not a different address an attacker anchors afresh
— that was the design two versions ago — and it is not a write the program
refuses outright either. `update_policy` re-fixes both limits and the period in
place, on the signature of the owner the record names, and carries the running
total through unchanged. It is the way back from an anchor that was wrong and
the brake on an agent that is suspect: `per_tx = 0` and it spends nothing
unattended. `spend` takes no `agent_id` and no limits at all — the policy
account's address is derived from the *paying* account, so the ceiling a payment
is measured against is whatever that account says, not what the caller typed.

Above `per_tx` the agent must present an approval account seeded by the exact
payment — agent, recipient, amount, nonce — and owned by this program. What
each of these buys and what it does not is
[`docs/security-model.md`](docs/security-model.md).

On the module side the same binding is one call, accepted once. Its second
argument is 64 hex characters: the policy account's 32-byte PDA seed, which is
now the agent's own account id. The parameter keeps its older name.

```
configure(ownerAddress, policyHashHex)   // then start()
```

A second `configure` is refused: the binding is the agent's identity, not a
preference. `status()` reports what it is bound to and whether it is running.
§5 is how to make both calls from a shell.

### Doing it in Logos Core, headless, in one command

The criterion this repository is answering names **Logos Core headless**, and
until recently nothing in `scripts/` touched Logos Core at all —
`grep -rn 'logos_core' scripts/` returned nothing. This is the command that
closes it:

```sh
./scripts/logos-core-headless.sh storage       # or messaging, or blockchain
```

This is the second of the two halves. `./scripts/deploy-and-configure.sh storage`
(§3) runs it after the chain half, hands it the manifest that half wrote, and
checks that row against the chain in between; `--check` is its prerequisite gate
on its own, which is what makes that ordering possible.

It installs `module/agent.lgx` into the user modules directory Logos Core reads
(flattening the platform variant, which is what an installed module is — Basecamp
0.2.2 has no "install from file" button), takes the headless harness for this
platform out of `module/harness/`, and then drives the **real `liblogos_core`
shipped inside the Logos app** through the same C API and in the same order as
the app's own `main.cpp`:

```
logos_core_init → logos_core_add_modules_dir (embedded, then user)
                → set_persistence_base_path → set_access_policy
                → logos_core_start → logos_core_load_module("agent")
                → configure(owner, policy_account) → start()
```

No GUI, no window, no display. The owner and policy account are read out of
`artifacts/agents.tsv` **by header name**, so what the module is configured with
is the envelope actually anchored on chain for that agent, and the harness reads
both back out of the running module's `meta.status` afterwards — so the run
asserts that Logos Core is running *this* agent under *that* envelope, not
merely that a module loaded. Recorded output:

```
agent     storage  <the storage agent's id, from the manifest>
owner     <its owner, from the manifest>
policy    <its policy account, from the manifest>
...
  ok    the runtime discovers the module in the user modules directory
  ok    logos_core_load_module() reports success
  ok    the module is in the runtime's loaded set
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  ok    the loaded module lists all 28 documented skills
  ok    the running module reports itself configured
  ok    and bound to the owner it was configured with
  ok    and to the policy account it was configured with

all steps confirmed (0 failure(s))
```

**It runs on every platform the Logos app is published for, out of the same
package.** `module/agent.lgx` carries `darwin-arm64`, `linux-amd64` and
`linux-arm64`, and the script installs the one this machine needs — the
transcript above is the macOS run; both Linux runs make the same 40 assertions
with the same result, and are in [`docs/basecamp.md`](docs/basecamp.md).

**It needs a Logos Core runtime and nothing else — no build toolchain.** It used
to compile that harness on every machine it ran on, so a C++17 compiler, Qt 6.9.2
with `qtremoteobjects`, a `logos-cpp-sdk` checkout and `nlohmann/json` were
prerequisites of *running* it. The harness is built once per variant and
committed now, next to the plugin and checked the same way: the same command
runs to `all steps confirmed (0 failure(s))` inside a stock `ubuntu:24.04`
container that has no compiler, no Qt SDK, no `logos-cpp-sdk` checkout and no
`nlohmann/json` — `./scripts/harness-no-toolchain.sh`, which refuses to report
anything if it finds a compiler on the machine it is checking. Rebuilding the
harness rather than trusting the shipped one is `--build-harness`, and running
your own is `HARNESS_FROM_SOURCE=1`; both need the four things above, and both
say which one is missing.

On **Linux** the runtime is one command and no install:

```sh
./scripts/fetch-logos-core.sh      # ~278 MB, checksum-pinned
```

because the Linux Logos app is an AppImage (both architectures) — a SquashFS image with an ELF
runtime in front of it — so `liblogos_core.so`, `logos_host`, the embedded
modules and Basecamp's own Qt come out of it with no installer, no root, no FUSE
and no display. On **macOS** it really is an app install, from the `.dmg` on the
same release page; this README used to say that of both platforms, and that was
a claim about the `.dmg` applied to a build nobody had looked at. The full list,
with what to set and where each comes from, is in
[`docs/limitations.md`](docs/limitations.md).

`meta.configure` exists as a skill and takes `per_tx`, `per_period` and
`period_blocks` among its keys — and it is worth being exact about what that
does, because the name invites the wrong reading. It changes what *this process*
bothers asking the owner about. It changes nothing about what the chain accepts:
the ceiling is the policy account's data, and LEZ rule 6
(`UnauthorizedDataModification`) means only the policy program may write it. An
agent that raised its own `per_tx` through `meta.configure` would still have
every spend above the anchored limit refused on chain.

## 5. Talk to the agent, and add a skill of your own

```sh
examples/agent-console/run.sh          # builds, then self-tests, offline
OUT=${TMPDIR:-/tmp}/lp0008-console     # where it puts the binaries
```

A C++17 compiler, a checkout of
[`logos-cpp-sdk`](https://github.com/logos-co/logos-cpp-sdk) (§10 pins the
revision CI uses) and `nlohmann/json.hpp`. No node, no keys, no Logos install.

`agent-console` links the agent module and gives its dispatcher a command line.
Three commands, and the first is how you find the other two's arguments:

```sh
$OUT/agent-console skills | python3 -m json.tool   # 28 skills, each with a JSON Schema
$OUT/agent-console status
$OUT/agent-console invoke <name> '<json>'
```

It wires the ports a command-line tool can honestly wire — the sequencer's
JSON-RPC — so the chain reads are real. Derive the program from the committed
binary and ask the chain about it, through the agent:

```sh
PROG=$(python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")

$OUT/agent-console invoke program.query "{\"program_id\":\"$PROG\",\"method\":\"getTransaction\"}"
# {"ok":true,"found":true,"included":true,"result":["<bytecode>", <block>]}

PAY=$(col artifacts/agents.tsv pay_account storage)        # the helper from §2
$OUT/agent-console --account "Public/$PAY" invoke wallet.balance '{}'
# {"ok":true,"account":"<base58>","balance":<n>,"shielded":false,"source":"sequencer.getAccount"}
```

No hash, block height or balance is quoted above. A redeploy moves the first
two and a settlement moves the third — that payee read 45 and then 95 while this
section was being written. `"source":"sequencer.getAccount"` is the part that
carries the claim: the module answered, and it answered from the chain rather
than from a file in this repository.

**It cannot move money, and that is deliberate.** Its `WalletPort::spend` is
null, so `wallet.send` refuses — `{"ok":false,"submitted":false,"outcome":
"owner_unreachable"}` — rather than submitting. A console that could sign would
be a second, unaudited spending path around the anchored policy this whole
submission is about. The CLIs that do hold keys are `scripts/deploy-agents.sh`
(§3) and `scripts/a2a-task.sh` (§6).

### Adding a skill

A skill is one interface — `ISkill` in
[`module/src/agent_module_interface.h`](module/src/agent_module_interface.h) —
with three methods: a name, a JSON Schema for its parameters, and an `invoke`
taking and returning a JSON string. That header is the *only* thing a skill
needs: not the plugin header, not the Logos SDK, not Qt, not a JSON library.

[`examples/skills/notary-digest`](examples/skills/notary-digest) is a complete
one, in about a page. Build it, load it, call it:

```sh
c++ -std=c++17 -Werror -fPIC -shared -I module/src \
    examples/skills/notary-digest/notary_digest_skill.cpp -o /tmp/libnotary.dylib

$OUT/agent-console --skill /tmp/libnotary.dylib invoke notary.digest '{"content":"hello"}'
# {"ok":true,"skill":"notary.digest","algorithm":"sha256","digest":"2cf24dba…","bytes":5}
```

`run.sh` does exactly that and then checks four things, each of which fails the
script: the returned digest equals `shasum -a 256` of the same input; it does
**not** equal the digest of altered input; `git status --porcelain module/` is
unchanged by the run, which is the prize's "without modifying the core agent
module" checked rather than asserted; and `docs/skills.md`'s reference table
still lists exactly the skills the module registers.

One limit, stated here rather than left to be found: **a skill cannot be added
to an already-installed `agent.lgx`.** `registerSkill` takes a
`std::shared_ptr<ISkill>`, Logos Core reaches a core module over Qt Remote
Objects in a separate process, and there is no wire format for a C++ object — so
skills are added by a host that *links* the module, which is what
`agent-console` is. The full interface specification, the loader convention, the
parameters of all 28 built-in skills, and what would have to change for the
plugin path are in [`docs/skills.md`](docs/skills.md).

## 6. Run an A2A task and settle it in LEZ

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
- `A2A_AGENTS` and `A2A_MANIFEST` override the input and output files, so a run
  can be pointed at a manifest of your own without touching the committed one.
  The reason this used to give — that `deploy-agents.sh` truncates
  `artifacts/agents.tsv` at the start of a run, so a settlement started
  mid-deploy reads a header and no agents — **is no longer true**: the script
  builds into a temporary file beside the manifest and `mv`s it over only once
  all three agents have anchored, precisely so a failed run cannot delete the
  record of the one that worked.

Results land in [`artifacts/a2a-task.tsv`](artifacts/a2a-task.tsv), with the
before/after balances the script read off the chain. Only the credit side is
publicly readable — the payer is a shielded account and `getAccount` answers
with the default account for those — which is why each agent also keeps a public
receiving account and why its Agent Card advertises one.

**A payee can be shielded too.** Its card carries
`x-logos.shieldedPaymentKeys` — an `npk`/`vpk` pair, both public keys — and a
payer names it with `--recipient PrivateKeys/<npk>:<vpk>` instead of an account
id. Then neither end of the settlement is visible, and the price is readable
only by the payee:

```sh
# what happened in transaction 5942d6cd…, block 9360
spel --idl idl/agent_verifier.idl.json --program artifacts/programs/agent_verifier.bin \
     --bin-auth-transfer artifacts/programs/authenticated_transfer.bin \
  -- spend --agent Private/<payer> --recipient PrivateKeys/<npk>:<vpk> \
     --amount 1 --window-start <period>

# and how the payee reads it back, from the chain rather than from its wallet file
LEE_WALLET_HOME_DIR=~/.lp0008-agents/storage \
  tools/shielded-receipt/target/release/shielded-receipt \
  --payee <payee> --tx 5942d6cd… --expect-amount 1
```

Those rows are in
[`artifacts/shielded-settlement.tsv`](artifacts/shielded-settlement.tsv), and
`verify-deployment.sh` checks each against the block it claims. The trade
between the two forms is written up in
[`docs/limitations.md`](docs/limitations.md).

## 7. Drive a real Delivery and Storage node

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
outside `PATH`). They are in [`docs/skills.md`](docs/skills.md).

**The two halves differ on CI, and the difference is upstream's, not a
preference.** The Storage half *is* a CI job — `storage-node` downloads
`logos-storage-nim`'s own checksum-pinned `libstorage` release asset, builds
`storage_node_drive.c` against it and runs a node on the runner, with two
negative controls. The Delivery half is not, because there is no prebuilt
`liblogosdelivery` for Linux anywhere public to download, so the only route is a
26-minute Nim compile on the runner, and the run itself would then depend on
live public peers. `.github/workflows/ci.yml` carries that audit release by
release.

Point `DELIVERY_SRC` and `STORAGE_SRC` at your checkouts if they are not under
`_external/`.

## 7b. The owner channel, between two processes, over the public network

```sh
./scripts/owner-channel-live.sh              # the exchange
./scripts/owner-channel-live.sh --negatives  # and the ways it fails
```

§7 proves a node is real. This proves the *owner channel* is: two processes,
each creating its own Delivery node on the public `logos.dev` network, sharing
nothing but a content topic. One runs `logos::agent::OwnerChannel` — the
module's own class, unmodified, with its port wired to `liblogosdelivery`
instead of to the fake the unit suite uses. The other is the owner: it joins the
same reliable channel from its own node, reads the request, and answers it.

```
agent:  <- decision: {"altered":0,"approved":true,"attempts":1,
                      "detail":"the owner approved these exact terms",
                      "owner_unreachable":false,"verdict":"approved"}
        ..  312 ms, 1 attempt(s) on the wire
```

`approved` is not "a send returned success". `OwnerChannel` returns it only for
a frame naming *this* request's id, policy hash, recipient, amount, nonce and
marker seed, so the pass is an assertion about bytes that crossed the network.
The relays in the middle are third-party public ones — DigitalOcean, Google
Cloud — named in the nodes' own logs; nothing here operates a broker. **312 ms
is what "in real time" has to mean.**

That number was 12.6 seconds over two attempts until the run stopped starting
both processes in one directory, and the reason is worth knowing before you
write your own: a Delivery node keeps the reliable channel's state in
`sds.db`, under a `data` directory it creates **in the working directory**, so two nodes started from the same
place share it. Nothing reports a conflict. The first frame is simply not
delivered — five sends and four deliveries in a bare probe — and the agent's
retry covers for it, which makes the whole thing look like an unreliable
network. Give each node a directory of its own and the retry stops being
load-bearing.

`--negatives` runs three failures and checks each: a node that never started
makes `open()` refuse; with nobody listening the agent reports `unreachable`
after five attempts and exits 1 — which matters, because a node receives its own
published messages, so that is the proof it cannot settle its own request; and
an owner answering `251` where `250` was asked yields
`{"verdict":"refused","altered":6}` and exit 1.

What this exercise does **not** claim: the owner here is a Delivery node this
repository starts, not a second Basecamp. That clause — "from a separate Logos
app instance" — is closed elsewhere and by something else: **two LogosBasecamp
0.2.2 processes**, each with its own user directory, its own loaded module and
its own Delivery node, one of them minting a spend and the other answering it
573 ms later from its own window. The transcripts, the latencies, the control
(the owner's app killed, and `owner_unreachable` with nothing submitted) and the
one ordering that does not work are in
[`docs/basecamp.md`](docs/basecamp.md) §"Two Basecamps, and the owner in the
second one". Keep the two apart when reading: this section is the transport, that
one is the host.

## 8. Load the module in the Logos app (Basecamp)

The loadable asset is committed: `module/agent.lgx`, with `darwin-arm64`,
`linux-amd64` and `linux-arm64` variants. Check it against itself rather than
trusting this page.

It is a binary built by hand from `module/src`, so the question that matters
about it is not whether it is a valid package — it always was, twice while
containing the wrong code — but whether it was built from the source committed
beside it. That is asserted rather than assumed, on every push, and needs
nothing but `python3`:

```sh
./scripts/check-package-fresh.py
#   ok    all 31 build inputs hash exactly as they did when the package was made
#   ok    every one of the 750 source literals of >= 8 bytes is in the darwin-arm64 binary
#   ok    every one of the 750 source literals of >= 8 bytes is in the linux-amd64 binary
#   ok    every one of the 750 source literals of >= 8 bytes is in the linux-arm64 binary
```

All three variants, and that matters more than it looks: the Linux plugins are
built from the same translation units as the macOS one, so "the delivery code
path is in there" is an assertion rather than a claim, and a variant built by a
different machine cannot ride along on another's provenance —
`scripts/write-package-record.py` re-checks the sibling's bytes and its literals
before it will carry its record forward.

The `package` job in `.github/workflows/ci.yml` runs it, together with four
controls that each put the repository back into a state it has actually shipped
and require a red — including the one where `agent.lgx` signed Agent Cards
`alg: EdDSA`, which [`scripts/use-cases/verify-agent-card.py`](scripts/use-cases/verify-agent-card.py)
rejects, for the eight commits between the source fix and the repackage.

**This step needs `lgx`, and nothing installs it for you** — it is not on a
reviewer's `PATH` and it is not in this repository. It is the packager
Basecamp's own packages are built with, `logos-co/logos-package` pinned at
`18b0075`; build it and either put it on `PATH` or point `$LGX_BIN` at
`logos-package/build/lgx`. [`docs/basecamp.md`](docs/basecamp.md) has the build.
The rest of §8 does not need it.

```sh
git clone https://github.com/logos-co/logos-package && \
  git -C logos-package checkout 18b0075     # then build it — docs/basecamp.md

lgx verify   module/agent.lgx     # contents match the manifest hashes
lgx manifest module/agent.lgx     # type: core; three platform variants
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
ls   # agent_plugin.dylib  liblogosdelivery.dylib  manifest.json  metadata.json
     # third-party/  variant
```

`liblogosdelivery.dylib` is in that listing and belongs there: the plugin opens
it by name and looks beside itself first, so the module directory is where it is
found. It rides only in the `darwin-arm64` variant — upstream publishes no
`liblogosdelivery.so`, so the two Linux variants carry the delivery code path and
no library beside it, and `meta.configure("delivery","on")` there reports the
file it could not open. `third-party/liblogosdelivery/` is upstream's licence,
which travels with the binary because this is redistribution.

On Linux the directory is `~/.local/share/Logos/LogosBasecamp/modules` and the
variant is `linux-amd64` or `linux-arm64` — `agent_plugin.so`, not the dylib,
and the two are not interchangeable. `LOGOS_USER_DIR`
overrides the base outright, which is the clean way to try this without touching
an existing install. `./scripts/logos-core-headless.sh` does all of this for you
and picks the variant for the machine it is on; doing it by hand is for a
reviewer who wants to see the tile in the app.

Building the plugin from source — the four pinned checkouts, the Qt version
that is a *ceiling* rather than a floor, and the `otool` check that costs
nothing and saves an afternoon — is [`docs/basecamp.md`](docs/basecamp.md).
That document also carries the two load harnesses and their recorded output:
`plugin_load_test` drives `QPluginLoader` over the shipped binary, and
`logos_core_load_test` `dlopen`s the real `liblogos_core` out of the installed
`LogosBasecamp.app`, loads the module through the same C API in the same order
as Basecamp's own `main.cpp`, and then calls back into it over the runtime's
own transport: 28 skills listed, each with a parameter schema, `invoke()`
dispatching to every one.

The loaded module also **opens its own Logos Delivery node**. That sentence used
to be a limitation instead: a port is a `std::function` and a host reaching a
core module over Qt Remote Objects has no wire format for one, so every skill on
the wire refused. The premise is right; the conclusion was not. A host cannot
*pass* a closure — a module can *build* one.
`module/src/delivery_runtime.cpp` opens `liblogosdelivery` and
`meta.configure("delivery","on")` — two strings, which the transport has always
carried — starts a node inside the module's own `logos_host` process.
`messaging.*`, `agent.discover`, `agent.task`, `agent.subscribe`, `agent.update`
and `agent.poll` then work, measured through `QPluginLoader`, through the real
runtime out of the installed Basecamp, and between two loaded modules that
discovered each other's signed Agent Cards on the public network:
`./scripts/delivery-in-plugin.sh`, and
[`docs/basecamp.md`](docs/basecamp.md) for the transcripts and the negative
control.

Those last two are the return path, and they are what lets a task move because
of what arrived rather than because this process said so. The server publishes an
A2A `TaskStatusUpdateEvent` on the task's topic (`agent.update`); the client
reads that topic and applies what the **peer** published to its own `TaskStore`
(`agent.poll`), refusing any frame that does not name the peer as its author.
Each agent first publishes a forged `completed` for its *own* task onto the very
topic it reads, and asserts that the poll counted it and refused it: a Delivery
node receives its own publications, so without that rule one process could
produce the whole transcript alone.

That runs in the **same call as the payment**. `./scripts/delivery-in-plugin.sh
settle` (exit 0, both processes) is discovery, an A2A task, a settlement on the
public testnet, and then both stores walking `submitted → working → completed` on
frames the other account published — one invocation.
`./scripts/delivery-in-plugin.sh peers` is the identical code path with no price
and no signer, which is the free way to re-check the lifecycle half.

A loaded module also **pays** for the task it was served, in the same flow and
the same call. `./scripts/delivery-in-plugin.sh settle` runs the two modules
above, and the buyer reads the price and the payee off the seller's discovered
card, checks them against the envelope its owner anchored on chain, and settles
on the public testnet — no owner key anywhere in the path. That works by the
mechanism the module already signs its Agent Card with: `card_signer`,
`pay_signer` and `policy_source` are three commands `meta.configure` names, all
three run by one function, each answer checked character by character before it
is believed. `scripts/agent-spend.py` is the last two, and what it performs is
the anchored policy program's own `spend` instruction, so the chain applies the
same limits to a module's payment as to `scripts/a2a-task.sh`'s.

Stated plainly, because a reviewer will check: the **sequencer and toolchain
skills have no ports wired** — `program.call` and `program.deploy` would have to
take a program id, an instruction and arguments from a stranger's A2A request
and put them on a command line, and the module's one delegation mechanism passes
input on stdin precisely to avoid that. The storage skills came off this list:
the module `dlopen`s `libstorage` and opens its own node on
`meta.configure("storage","on")`, exactly as it already did for Delivery. `wallet.send`
still cannot move money from a loaded module either: its envelope is fixed at
`start()` and there is no live read behind it, so only `agent.task` pays.
Each of those refuses *as itself* —
`{"ok":false,"error":"no account to read: …"}` — which is what the harness
asserts, and is the opposite of the failure worth hiding: a module that loads,
answers `skills()` with `[]`, and looks like it works.

### 8a. The window: `app/agent-ui.lgx`

A `core` module is not a surface. Basecamp gives windows to `ui` plugins, and
until `app/` existed this repository shipped none — so the module loaded,
answered, and was named nowhere a person watching the app could see. `grep -ci
agent` over Basecamp's whole output returned **0**.

`app/` is that plugin: Qt Widgets, implementing Basecamp's `IComponent`,
packaged `type: ui`, holding no agent logic of its own. Every button is one call
on the loaded module over Logos Core's transport. Install it into the user
**plugins** directory the same way — the exact commands, the Qt ceiling, and the
load harness are in [`app/README.md`](app/README.md):

```sh
DEST=~/Library/Application\ Support/Logos/LogosBasecamp/plugins/agent-ui
mkdir -p "$DEST" && cd "$DEST"
tar xzf /path/to/lp-0008/app/agent-ui.lgx
mv variants/darwin-arm64/* . && rm -rf variants
printf 'darwin-arm64' > variant
```

Restart Basecamp and the tile is in the left rail, labelled **LP-0008 Agent**.
`grep -ci agent` over Basecamp's output is still 0 at startup — nothing has
asked for the module yet — and clicking the tile is what asks. After one click
it is 30, and after an approval round trip in the same run, 62. The number is
not the point; **0 → nonzero** is, and these are the lines it is made of:

```
App launcher clicked: "agent-ui"
Loading core dependency for "agent-ui" : "agent"
[info] [logos] Module loaded: agent
MainContainer: Added plugin dock to WorkspaceArea: "LP-0008 Agent" (module: "agent-ui" )
Successfully loaded UI module: "agent-ui"
```

`Loading core dependency` is the mechanism: Basecamp's PluginLoader reads a `ui`
plugin's `dependencies`, loads each named core module, has `capability_module`
mint the plugin a token for it, and only then calls `createWidget(LogosAPI*)`.
So `"dependencies": ["agent"]` in `app/metadata.json` is what turns a click on a
tile into a loaded module.

From that window an owner binds the agent, starts it, reads its 28-skill card,
invokes any of them, and — the part the criterion is about — answers the spends
the agent asks it to approve. A `wallet.send` above the envelope published
`ownerApprovalRequested` to the window in **7 ms**, and the owner's `approved`
came back and released the spend 7.2 s later, inside a 60 s wait.

Two module-side facts had to be measured before that round trip worked at all —
a module that blocks on a call cannot publish while it blocks, and a verdict
cannot come back on the connection that asked for it. Both, with their
measurements and what is still bounded, are in
[`docs/limitations.md`](docs/limitations.md).

One more, and this paragraph used to end in the wrong place. The prize asks that
the module load "alongside the wallet, storage, and messaging modules", and what
stood here was that Logos Basecamp 0.2.2 ships no such modules — true, `ls
/Applications/LogosBasecamp.app/Contents/modules/` returns `capability_module`,
`package_downloader`, `package_manager` — followed by the conclusion that no
submission could therefore demonstrate it against this host. **That conclusion
was false, and the observation it rests on was never about Logos Core.** The
runtime loads from the *user* modules directory as well as the bundle's, which
is precisely how this repository's own module gets in.

The three modules exist and are public. They are built from their published
revisions and loaded beside the agent by one command:

```sh
./scripts/build-companion-modules.sh                    # storage, delivery, wallet
./scripts/logos-core-headless.sh storage --alongside
```

```
<- loaded modules: wallet_module storage_module delivery_module
                   capability_module agent
ok 'storage_module' answers across the runtime's transport with its own
   method table: it is running, not just loaded
ok 'delivery_module' answers …
ok 'wallet_module' answers …
all steps confirmed (0 failure(s))
```

Loaded is not the claim, which is why each companion has to *answer*: a plugin
built against too new a Qt reports a successful load and then times out, and
[`docs/basecamp.md`](docs/basecamp.md) quotes the run where that happened here.
The criterion's other half —
"without requiring modifications to those modules" — is checked rather than
asserted, by `git status --porcelain` against each upstream checkout, the same
way [`examples/agent-console/run.sh`](examples/agent-console/run.sh) checks
`module/`. [`docs/basecamp.md`](docs/basecamp.md) carries the build recipe and
the one upstream ABI break it has to route around.

### And the owner in a *second* Basecamp

That round trip is one app: the owner answers over Logos Core's own transport,
which reaches whatever loaded the module and never touches a network. The
criterion asks for something else — "the owner can interact with the agent in
real time from a **separate Logos app instance** using Logos Messaging, with no
intermediary server" — so:

```sh
# two bases, two working directories, the same two packages installed in each
mkdir -p /tmp/lp8/{agent,owner} /tmp/lp8/cwd-agent /tmp/lp8/cwd-owner
# …unpack module/agent.lgx into <base>/modules/agent and app/agent-ui.lgx into
#   <base>/plugins/agent-ui, per §8 above, for BOTH bases…
( cd /tmp/lp8/cwd-agent && LOGOS_USER_DIR=/tmp/lp8/agent \
    /Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp & )
( cd /tmp/lp8/cwd-owner && LOGOS_USER_DIR=/tmp/lp8/owner \
    /Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp & )
```

Two windows, two tiles, two loaded modules, two Delivery nodes. In **both**:
*Configure*, *Start*, *Join Messaging*. In the **owner's**: *Watch as owner* —
and do that before the agent's first request, because a reliable channel opened
mid-stream does not receive the backlog. Then invoke `wallet.send` in the
agent's window with an amount outside its envelope. The request appears in the
other app's window on its own; *Approve over Delivery* or *Deny over Delivery*
answers it.

Measured, with both transcripts read out of the two windows:

| | |
|---|---|
| call → request visible in the other app | **573 ms** |
| *Deny* pressed → verdict at the agent | **371 ms** |
| *Approve* pressed → verdict at the agent | **177 ms** |
| owner's app **killed**, same call | `owner_unreachable`, 2 attempts, nothing submitted |

The last row is the control, and it is the one that matters: a node receives its
own published messages, so the agent's own six request frames came back to it
off the public relays (`"relay_seen":6`) and produced no approval, while the two
channel frames it decoded are the owner's two answers. Headless, the same three
runs are `./scripts/delivery-in-plugin.sh two-modules`.

The whole record — environment, what could have stopped it, the ordering that
does not work, and what the second app still does not do (it holds the owner's
account, not a key) — is [`docs/basecamp.md`](docs/basecamp.md) §"Two Basecamps,
and the owner in the second one".

## 9. The owner channel

An above-threshold spend is the one case where the agent has to stop and ask.
`OwnerChannel` opens a **reliable** channel — not a bare topic, because a
dropped approval request looks exactly like a refusal — at

```
/lp-0008/1/owner-channel/<owner account>/<agent account>
```

and sends a `spend_approval_request` carrying the protocol id, a correlation
id, the agent, its policy account, the recipient, the amount as decimal digits
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

That exchange is exercised in CI against a fake owner — one that can be made
silent, late or hostile on demand, which a real one cannot.

**It is reachable from a loaded module, and this paragraph used to say it could
never be.** The claim was that `OwnerChannelPort` is a struct of `std::function`s
and a plugin cannot be handed one. The premise is true and the conclusion does not
follow: a *host* cannot pass a closure across a plugin boundary, which was never a
statement about what a *module* can construct. The module links
`liblogosdelivery`, opens a node from its own configuration, and builds the port
on its own side of the boundary — so nothing crosses it but
`meta.configure("delivery","on")`. Watched, rather than argued:

```sh
./scripts/delivery-in-plugin.sh approval    # exit 0
```

A plugin loaded through `QPluginLoader` holds an above-threshold `wallet.send`,
publishes it to an owner on a **separate** Delivery node over the public network,
and acts on the answer — approved in 438 ms, and, on the second run, denied in
661 ms:

```
  ok  the owner can derive a marker seed for these terms
  ok  and it is the seed the agent named — two independent derivations of the
      account `spend_approved` will look for, not one copied twice
  <-  wallet.send: {"amount":"250","approved":true,"attempts":1,
      "outcome":"approved","submitted":false,"waited_ms":438,…}
  ok  the owner was reached
  ok  and approved these exact terms: approved
```

It runs twice on purpose. A channel that returned "approved" whatever came back
would pass the first run and only the first, so the deny run is what makes the
approve run mean anything.

The owner in *that* exercise is `module/tests/owner_responder.cpp`, a program
written for the purpose rather than a second Logos app with a person in front of
it — which is why it is not the evidence for the app-instance clause. That
clause is closed separately, and by two Basecamps rather than by an argument:
§8a above, and [`docs/basecamp.md`](docs/basecamp.md) §"Two Basecamps, and the
owner in the second one". Keep the two apart when reading — this one is the
transport from a loaded module, that one is the host.

### What a loaded module does about that

The retry discipline is not left inside `OwnerChannel`, because then a loaded
module would have none. It is on the `wallet.send` path, where the decision not
to execute is made, and the owner channel a plugin actually has is built out of
the runtime's own surface:

| Direction | Carrier |
|---|---|
| agent → owner | the module event `ownerApprovalRequested(requestJson, attempt, timestamp)`, re-emitted byte-identically once per attempt |
| owner → agent | the module method `approveSpend(requestId, verdict)`, `"approved"` or `"denied"` |

Both are on Logos Core's own transport; neither needs an intermediary server.
`meta.configure` sets `approval_timeout_ms` and `approval_resend_ms` on the
running agent — the only two keys that skill reports as `effective`.

Measured against the packaged module inside Basecamp 0.2.2's runtime
(`module/tests/logos_core_load_test.cpp`, exit 0):

```
  ok    an above-threshold spend nobody approved is not submitted by the loaded module
  ok    and the outcome is the terminal owner-unreachable one, not a fallback to acting alone
  ok    the notification was retried before the timeout: 8 attempts
  ok    approveSpend is reachable, and refuses a request nobody is waiting on
```

Two things this does **not** claim. It is not Logos Messaging: *this* exchange
runs on Logos Core's own event/method transport, which is what a window inside
Basecamp uses. The Delivery-backed `OwnerChannel` is a different object, and the
module builds a port for it itself — §7b runs it between two processes on two real
Delivery nodes, and `./scripts/delivery-in-plugin.sh approval` runs it from a
module a host has **loaded**. So the transport clause of "using Logos Messaging,
with no intermediary server" is answered here, and the *app instance* clause is
answered somewhere else — in §8a, by two Basecamps. The owner in **both** of the
exercises named in this paragraph is a program written for the purpose rather
than a second Logos app, which is why neither of them is the evidence for that
clause. Keep those apart; [`docs/basecamp.md`](docs/basecamp.md) does. And an *approved*
above-threshold spend is still not submitted: submitting one goes through the
policy program's `spend_approved`, which needs an approval account only the
owner's own signature can create, and
[`docs/limitations.md`](docs/limitations.md) records why no owner on testnet can
create it today. The module reports `{"outcome":"approved","submitted":false}`
and names the path, rather than claiming a payment that did not happen.

The end-to-end sequencer job checks the other half, on chain: that a payment
outside the envelope is refused when no owner approval account exists for it.

## 8b. Surviving a restart

Pending A2A task state is written to one file under the per-instance directory
the host provisions, atomically — temp file, flush, rename, flush the parent —
and read back on `start()`. Three outcomes, not two: loaded, absent, or *could
not be read*. **The last one refuses the start.** Coming up with an empty task
list on top of an unreadable snapshot is how a paid task gets paid twice, so the
agent stops instead, naming the file.

`meta.status` carries a `durability` block — the path, the recovery outcome, how
many tasks came back, and how many payments the restart could not resolve — so
"this agent writes nothing down" and "this agent had nothing to write" are
distinguishable from outside. A module nobody gave a directory to reports
`"durability": null` rather than implying it is durable.

This is the part that was missing rather than the file format, which existed and
was tested: `TaskPersistence` had 121 green assertions and no construction site
in the plugin, so the shipped module persisted nothing. Measured before the fix
— task opened, `meta.status` `{"total":1}`, persistence directory empty, module
rebuilt, `{"total":0}`, no error anywhere.
`module/tests/module_recovery_test.cpp` is the suite that would have caught it,
and CI runs it with a negative control that puts the module back in that state
and asserts the suite goes red.

## 10. Tests and CI

```sh
cargo test --workspace --release --locked      # the policy crate, 24 tests
```

The module's C++ suites need no node, no network, no key and no model — which
is the point, since a real dependency cannot be made to fail on demand and a
fake can. They do need the SDK headers, which are not vendored here:
`logos-co/logos-cpp-sdk` at `c87f343` (CI clones exactly that, and
[`docs/basecamp.md`](docs/basecamp.md) has it in the pinned table):

```sh
git clone https://github.com/logos-co/logos-cpp-sdk _external/logos-cpp-sdk && \
  git -C _external/logos-cpp-sdk checkout c87f343

clang++ -std=c++17 -I_external/logos-cpp-sdk/cpp -I/opt/homebrew/include \
  module/tests/skills_test.cpp module/src/*.cpp -o skills_test && ./skills_test
```

The other nine suites (`inference`, `wallet_skills`, `program_skills`,
`agent_skills`, `owner_channel`, `owner_skills`, `task_persistence`,
`module_recovery`, `spend_marker`) each compile against their
own translation unit; [`.github/workflows/ci.yml`](.github/workflows/ci.yml)
carries every line, one step per suite so a red X names the suite that broke,
and accounts for every file under `module/tests/` so that a suite absent from
CI cannot be mistaken for one that passed. Three of those files are the
transport harnesses — `plugin_delivery_test.cpp`, `logos_core_delivery_test.cpp`
and the `owner_responder.cpp` they drive — which start a Delivery node and so
are local for the reason §7 gives. That workflow also runs `demo.sh`
from a clean clone and asserts the committed binary still hashes to the live
deploy transaction.

The `linux-headless` job is the one that keeps §4's second paragraph honest. It
fetches the published Logos Basecamp AppImage on `ubuntu-latest`, checks it
against a pinned sha256, unpacks it with no installer and no display, and runs
`./scripts/logos-core-headless.sh storage` against the **committed**
`module/agent.lgx` — the file a reviewer downloads — asserting the confirmation
banner rather than only the exit code. Three controls sit under it: a truncated
AppImage must be refused before anything is unpacked, a variant this machine
cannot use must be refused before anything is installed, and asking the runtime
for a module that is not there must go red at the load. It has been rehearsed
end to end in a bare `ubuntu:24.04` amd64 container, step for step, including
all three controls, and it has since run green on a GitHub runner — the job is
**"Logos Core loads and configures the shipped module, headless, on Linux"** in
`ci.yml`, and it passes on every push. This paragraph said "it has not yet run
on a GitHub runner, because nothing here has been pushed" for as long as that
was true, which was until the repository was made public. The `linux-arm64` leg
is still not in CI and was exercised the same way, in a native `linux/arm64`
container against the `aarch64` AppImage — 40 assertions, 0 failures — which is
stated rather than added as a second job nothing has watched run.

That job runs on a runner this repository *prepared*, which is the one thing it
cannot prove about "any machine". `./scripts/harness-no-toolchain.sh` is the
other side of it and is a script rather than a job, so it can be run by anyone
with docker: it runs the same command in a stock `ubuntu:24.04` container with
nothing installed but `python3`, refuses to report anything if it finds a
compiler there, and requires three things — the confirmation banner out of the
**shipped** harness, a refusal when the same command is asked to build one, and
a refusal to run a harness whose bytes are not the recorded ones. Watched pass
on both Linux architectures and watched fail in each of those three ways.

[`.github/workflows/e2e-local-sequencer.yml`](.github/workflows/e2e-local-sequencer.yml)
runs the whole policy lifecycle against a real standalone LEZ sequencer with
`RISC0_DEV_MODE=0`. It has no skip path, deliberately: a job that completes
through one reports green without having run, which is worse than red because
nobody looks again. Run
it locally with `./scripts/e2e-local-sequencer.sh` against a
`logos-execution-zone` checkout.

Compute-unit costs for every on-chain operation are measured in
[`docs/benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md).

## 11. What does not work

[`docs/limitations.md`](docs/limitations.md) is the honest state and is meant
to be read before the rest. It carries, among others: that `getAccount` cannot
see a private balance, so a payment into a shielded account is checkable by its
payee and by nobody else; that no model has
ever been run against the inference port — the local backend is a stub with an
honest name and the HTTP backend has never made a real request; and that
`scripts/exercise-nodes.sh` in §7 remains a local command, though a real
Delivery node does now run in CI — step 19 of the `alongside` workflow drives
the file-vault use case against one, and the Storage node has its own job.

Retractions live there too, with what replaced them. If you find a gap that is
not in that file, it is an omission rather than a decision.

## Layout

```
crates/agent-policy-core          the policy record and its period ledger, the
                                  spend reference, the approval marker — the one
                                  decision every spend turns on, in the crate the
                                  adversarial tests exercise
crates/agent-verifier-spel        the SPEL program that enforces the envelope
crates/agent-verifier-adversarial the hostile calls, run against the committed
                                  binary, each paired with the honest call it
                                  differs from in one field
module/src                        the Logos Core module: skill registry, owner
                                  channel, and the ports every skill calls
                                  through
module/tests                      the suites, plus the two load harnesses and
                                  the C node drivers
module/src/owner_skills.cpp       the OWNER's end of the approval channel, which
                                  is what lets a second Logos app answer a spend
module/agent.lgx                  the loadable package (darwin-arm64, linux-amd64,
                                  linux-arm64)
module/agent.lgx.sources          what that package was built from, written by
                                  package-basecamp.sh and checked by CI
module/harness                    the headless harness, built once per variant
                                  and committed, so §4 needs no toolchain
module/harness/harness.sources    what each of those was built from, written by
                                  --build-harness and checked by CI
app/src                           the Basecamp `ui` plugin: the owner console,
                                  which drives the loaded module and holds no
                                  agent logic of its own
app/tests                         the load harness that reproduces Basecamp's
                                  PluginLoader
app/agent-ui.lgx                  the loadable `ui` package (darwin-arm64,
                                  linux-amd64, linux-arm64)
examples/agent-console            §5 — the module's dispatcher, from a shell
examples/skills/notary-digest     §5 — a third-party skill, outside module/src
scripts/demo.sh                   §1 — the whole thing from a clean clone
scripts/deploy-agents.sh          §3 — three agents, funded and anchored
scripts/logos-core-headless.sh    §4 — installs the module and runs Logos Core
                                  headless: load, configure, start. With
                                  --alongside, the same run with the wallet,
                                  storage and messaging modules loaded too
scripts/deploy-and-configure.sh   §3 — both of the two above, in one command:
                                  both halves' prerequisites checked before
                                  either runs, and the manifest the first wrote
                                  checked against the chain before the second
                                  reads it
scripts/build-companion-modules.sh
                                  §8 — builds those three from their published
                                  sources, unmodified, and packages each as
                                  an .lgx
scripts/verify-deployment.sh      checks docs/DEPLOYMENT.md and artifacts/
                                  against the chain, and fails if they disagree
scripts/check-package-fresh.py    checks module/agent.lgx against module/src,
                                  and fails if the package is stale
scripts/a2a-task.sh               §6 — two agents, one A2A task, one settlement
scripts/exercise-nodes.sh         §7 — real Delivery and Storage nodes
scripts/owner-channel-live.sh     §7b — the owner channel between two processes
                                  on two Delivery nodes, and its negatives
scripts/e2e-local-sequencer.sh    §10 — the lifecycle against a real sequencer
scripts/use-cases/                the prize's illustrative use cases, one script
                                  each, every claim fetched from the chain —
                                  docs/use-cases.md
artifacts/                        the manifests: agents, anchors, settlements,
                                  Agent Cards, and the deployed program binaries
idl/                              the instruction ABI the CLI drives
docs/                             see the reading order below
```

### The documents, in reading order

| | |
|---|---|
| [`architecture.md`](docs/architecture.md) | the shape of the module, and where each decision is made |
| [`security-model.md`](docs/security-model.md) | what the agent may do alone, and what it may not — the "They can" / "They cannot" lists |
| [`skills.md`](docs/skills.md) | the skill interface spec: the contract, how to add one, and a reference for all 28 built-ins. Also which are wired to a running node and which are only compiled |
| [`a2a-binding.md`](docs/a2a-binding.md) | the A2A transport binding over Logos Messaging — the Agent Card schema, the task lifecycle, and a conformance table against A2A §11.1, including where this implementation does not conform |
| [`use-cases.md`](docs/use-cases.md) | the prize's illustrative use cases, and which of them this repository demonstrates |
| [`DEPLOYMENT.md`](docs/DEPLOYMENT.md) | what is live, how it got there, and how to reproduce it |
| [`benchmarks/cu-budget.md`](docs/benchmarks/cu-budget.md) | the cost of each on-chain operation, and why LEZ v0.2.4 does not meter compute units |
| [`basecamp.md`](docs/basecamp.md) | building, packaging and loading the module in the Logos app, with the two load harnesses and the record of the app naming it |
| [`app/README.md`](app/README.md) | the owner console: the Basecamp `ui` plugin, its build and install, and the transcript of an approval answered from inside the app |
| [`limitations.md`](docs/limitations.md) | what does not work. Meant to be read before the rest, not after |
| [`recon.md`](docs/recon.md) | notes from reading the Logos stack, kept because several conclusions in the others rest on them |

**One place per fact.** Where a document and a manifest disagree about what is
deployed, the manifest wins and the document is the bug:
[`artifacts/anchored.tsv`](artifacts/anchored.tsv) and
[`artifacts/agents.tsv`](artifacts/agents.tsv) are the record, and
`./scripts/verify-deployment.sh` checks both against the chain *and* against
`docs/DEPLOYMENT.md`, failing if any of the three has drifted. This repository
has shipped two contradictory accounts of its own deployment before; that script
is the fix, and running it is cheaper than reading for it.

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your
option — as `Cargo.toml` also declares (`license = "MIT OR Apache-2.0"`).

The root [`LICENSE`](LICENSE) is now the **verbatim MIT text** rather than a
pointer to the other two files, and the reason is worth a sentence. GitHub's
licence detector matches file contents against known texts; a file that merely
*describes* a dual grant matches nothing, so the repository sidebar read
**"Other"** — which is what a reviewer sees before they read a word of this. The
grant has not changed: Apache-2.0 remains available under
[`LICENSE-APACHE`](LICENSE-APACHE), at your option, and every crate declares both.
