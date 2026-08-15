#!/usr/bin/env bash
# Full LP-0008 lifecycle against a REAL LEZ sequencer running locally in
# standalone mode, with RISC0_DEV_MODE=0.
#
# WHY THIS EXISTS SEPARATELY FROM demo.sh
#
# `demo.sh` is the fast tour: it runs the circuit and the built verifier through
# the sequencer's *executor*, which is the same code the chain runs but linked
# in-process. That is enough to show what is rejected and why, in seconds, with
# no network — but it is not the same claim as "works against a real sequencer".
#
# This script makes that one. It starts the actual `sequencer_service` binary in
# standalone mode, points a throwaway wallet at it, and drives the whole
# lifecycle over JSON-RPC — deploy both programs, commit a distribution, and
# submit a real privacy-preserving claim whose Risc0 receipt the sequencer
# verifies against PRIVACY_PRESERVING_CIRCUIT_ID — then reads the resulting
# marker PDA back off that local chain and checks it is owned by the verifier.
# Nothing is mocked and RISC0_DEV_MODE is 0 throughout.
#
#   ./scripts/e2e-local-sequencer.sh
#   COUNT1=1 COUNT2=0 ./scripts/e2e-local-sequencer.sh   # the CI shape, one proof
#
# Env:
#   LEZ_SRC     checkout of logos-execution-zone (default: ./_external/lez)
#   COUNT1      claims in distribution 1     (default 1)
#   COUNT2      claims in distribution 2     (default 0 = single distribution)
#   PORT        sequencer RPC port           (default: first free from 3141)
#   KEEP        set to 1 to leave the sequencer running for inspection
#
# Budget: one claim is a real proof, measured at ~150 s. COUNT1=1 COUNT2=0 is a
# single proof and is what CI uses; it still exercises every integration point.

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LEZ_SRC="${LEZ_SRC:-$ROOT/_external/lez}"
KEEP="${KEEP:-0}"

# The signing key of the genesis-funded test account, published in clear in
# LEZ's own justfile (`wallet-import-test-accounts`). It is a shared test
# account, not a secret, and it only exists on chains whose genesis lists it.
TEST_SIGNER_KEY=7f273098f25b71e6c005a9519f2678da8d1c7f01f6a27778e2d9948abdf901fb
TEST_SIGNER=CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r

die() { echo "error: $*" >&2; exit 1; }
say() { printf '\n\033[1m%s\033[0m\n' "$*"; }

[ -d "$LEZ_SRC" ] || die "no LEZ checkout at $LEZ_SRC. Clone logos-execution-zone there, or set LEZ_SRC."
CONFIG_SRC="$LEZ_SRC/lez/sequencer/service/configs/debug/sequencer_config.json"
[ -f "$CONFIG_SRC" ] || die "no standalone config at $CONFIG_SRC"

SEQ_BIN="$LEZ_SRC/target/release/sequencer_service"
if [ ! -x "$SEQ_BIN" ]; then
  say "building the standalone sequencer (once; a few minutes)"
  ( cd "$LEZ_SRC" && cargo build --release --features standalone -p sequencer_service ) \
    || die "sequencer build failed"
fi
WALLET_BIN="$LEZ_SRC/target/release/wallet"
[ -x "$WALLET_BIN" ] || ( cd "$LEZ_SRC" && cargo build --release -p wallet ) || die "wallet build failed"
command -v spel >/dev/null 2>&1 || die "spel not on PATH (cargo install --git https://github.com/logos-co/spel --locked spel-cli)"

# The v0.2.0 wallet links Python 3.9 and dies with `Library not loaded:
# @rpath/Python3.framework/...` on some macOS setups. macOS SIP strips DYLD_*
# whenever bash execs another script, so it has to be set on the wallet's own
# exec — same reason deploy-and-claim.sh needs it too, which is why we export a
# name it also reads.
WALLET_ENV=()
if [ "$(uname)" = "Darwin" ]; then
  export DYLD_FALLBACK_FRAMEWORK_PATH="${DYLD_FALLBACK_FRAMEWORK_PATH:-/Library/Developer/CommandLineTools/Library/Frameworks}"
  WALLET_ENV=(env "DYLD_FALLBACK_FRAMEWORK_PATH=$DYLD_FALLBACK_FRAMEWORK_PATH")
fi
wallet_run() { "${WALLET_ENV[@]}" "$WALLET_BIN" "$@"; }

# A free port, so this never fights a sequencer the developer already has up.
PORT="${PORT:-}"
if [ -z "$PORT" ]; then
  for p in $(seq 3141 3200); do
    if ! nc -z localhost "$p" 2>/dev/null; then PORT=$p; break; fi
  done
fi
[ -n "$PORT" ] || die "no free port in 3141-3200"

WORK="$(mktemp -d)"
SEQ_HOME="$WORK/sequencer"; mkdir -p "$SEQ_HOME"
WALLET_HOME="$WORK/wallet";  mkdir -p "$WALLET_HOME"
RPC="http://localhost:$PORT"
SEQ_PID=""

cleanup() {
  if [ -n "$SEQ_PID" ] && [ "$KEEP" != "1" ]; then
    kill "$SEQ_PID" 2>/dev/null
    wait "$SEQ_PID" 2>/dev/null
  fi
  if [ "$KEEP" = "1" ]; then
    echo
    echo "left running: sequencer pid $SEQ_PID on $RPC"
    echo "  wallet home $WALLET_HOME"
    echo "  logs        $WORK/sequencer.log"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT INT TERM

say "[1/5] starting a real sequencer in standalone mode on $RPC"
python3 - "$CONFIG_SRC" "$SEQ_HOME" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
cfg["home"] = sys.argv[2]
json.dump(cfg, open(sys.argv[2] + "/sequencer_config.json", "w"), indent=2)
PY
RUST_LOG=info "$SEQ_BIN" --port "$PORT" "$SEQ_HOME/sequencer_config.json" \
  > "$WORK/sequencer.log" 2>&1 &
SEQ_PID=$!

for _ in $(seq 1 60); do
  if curl -s -m 2 -X POST "$RPC" -H 'Content-Type: application/json' \
       -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$TEST_SIGNER"'"]}' \
       | grep -q '"result"'; then
    echo "  up, pid $SEQ_PID"; break
  fi
  kill -0 "$SEQ_PID" 2>/dev/null || { tail -20 "$WORK/sequencer.log"; die "sequencer died on startup"; }
  sleep 1
done
curl -s -m 3 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$TEST_SIGNER"'"]}' \
  | grep -q '"result"' || die "sequencer never answered on $RPC"

say "[2/5] a throwaway wallet pointed at it"
cat > "$WALLET_HOME/wallet_config.json" <<EOF
{ "sequencers": [{ "sequencer_addr": "$RPC" }], "seq_poll_timeout": "30s", "seq_tx_poll_max_blocks": 15, "seq_poll_max_retries": 10, "seq_block_poll_max_amount": 100, "calibration_limit": 100 }
EOF
export LEE_WALLET_HOME_DIR="$WALLET_HOME"
export NSSA_WALLET_HOME_DIR="$WALLET_HOME"
printf 'lp0008\n' | wallet_run account import public --private-key "$TEST_SIGNER_KEY" >/dev/null 2>&1 \
  || die "could not import the test signer"
echo "  imported Public/$TEST_SIGNER"

say "[3/5] funding it from the genesis vault"
wallet_run vault claim --account-id "Public/$TEST_SIGNER" --amount 5000 </dev/null >/dev/null 2>&1
for _ in $(seq 1 30); do
  bal=$(curl -s -m 3 -X POST "$RPC" -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$TEST_SIGNER"'"]}' \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("result",{}).get("balance",0))' 2>/dev/null)
  [ "${bal:-0}" -gt 0 ] 2>/dev/null && { echo "  balance $bal"; break; }
  sleep 2
done
[ "${bal:-0}" -gt 0 ] 2>/dev/null || die "vault claim did not land"

say "[4/5] deploy the policy program and anchor an agent's envelope"
PROGRAM="$ROOT/artifacts/programs/agent_verifier.bin"
IDL="$ROOT/idl/agent_verifier.idl.json"
[ -f "$PROGRAM" ] || die "no program at $PROGRAM"

wallet_run deploy-program "$PROGRAM" >/dev/null 2>&1 || die "deploy failed"
DEPLOY_TX=$(python3 -c "
import hashlib,struct,sys
b=open(sys.argv[1],'rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())" "$PROGRAM")
echo "  deployed $DEPLOY_TX"

# The agent signs for itself, so it needs its own shielded account on this chain.
AGENT_HOME="$WORK/agent"; mkdir -p "$AGENT_HOME"
cp "$WALLET_HOME/wallet_config.json" "$AGENT_HOME/"
LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  wallet_run account new private </dev/null >/dev/null 2>&1
SEED=$(LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  wallet_run account list </dev/null 2>/dev/null \
  | grep -oE 'Private/[1-9A-HJ-NP-Za-km-z]+' | sed 's|Private/||' | tail -n1)
[ -n "$SEED" ] || die "could not create the agent account"

# Fund it BEFORE anchoring, and anchor on whatever account ends up holding the
# money. A shielded transfer does not credit an existing account, it creates a
# new note with its own id under the same keys — so anchoring first would commit
# the policy to an empty account, and `spend` now fails on a balance it cannot
# move rather than passing a check and moving nothing.
LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  wallet_run account show-keys --account-id "Private/$SEED" </dev/null 2>/dev/null \
  | grep -E "^[0-9a-f]{64,}$" > "$WORK/agent.keys"
[ -s "$WORK/agent.keys" ] || die "could not export the agent's keys"
wallet_run auth-transfer send --from "Public/$TEST_SIGNER" \
  --to-keys "$WORK/agent.keys" --amount 1000 </dev/null >/dev/null 2>&1 \
  || die "could not fund the agent"
AGENT=""
for _ in $(seq 1 30); do
  LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
    wallet_run account sync-private </dev/null >/dev/null 2>&1
  for a in $(LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
               wallet_run account list </dev/null 2>/dev/null \
             | grep -oE 'Private/[1-9A-HJ-NP-Za-km-z]+'); do
    b=$(LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
          wallet_run account get --account-id "$a" </dev/null 2>/dev/null \
        | grep -o '"balance":[0-9]*' | cut -d: -f2 | head -1)
    [ "${b:-0}" -ge 1000 ] 2>/dev/null && { AGENT="${a#Private/}"; break; }
  done
  [ -n "$AGENT" ] && break
  sleep 2
done
[ -n "$AGENT" ] || die "the agent never received the funding"
echo "  agent $AGENT  (holds 1000)"

PER_TX=100; PER_PERIOD=500; PERIOD=1000
# The agent's id as the 32 bytes the chain holds. It is the PDA seed of the
# policy account — there is one per agent and no other address for it — so this
# cannot be a hash of the printed form. The owner needs no conversion:
# `create_policy` takes no owner argument and records the account that signs.
id_hex() {
  python3 -c "
import sys
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
s=sys.argv[1]
n=0
for c in s:
    n = n*58 + A.index(c)
b = n.to_bytes((n.bit_length()+7)//8,'big')
b = b'\x00'*(len(s)-len(s.lstrip('1'))) + b
assert len(b)==32, 'not a 32-byte account id: %r' % s
print(b.hex())" "$1"
}
AGENT_HEX=$(id_hex "$AGENT")
OWNER_HEX=$(id_hex "$TEST_SIGNER")
POLICY=$(spel --idl "$IDL" --program "$PROGRAM" pda policy --agent-id "$AGENT_HEX" 2>/dev/null | tail -n1)
[ -n "$POLICY" ] || die "could not resolve the policy account"
echo "  policy $POLICY  (per-tx $PER_TX, per-period $PER_PERIOD)"

# Anchoring is TWO signatures from two wallets that never meet. The agent signs
# first and names the one account allowed to anchor over it; `create_policy`
# then reads that claim and refuses any other signer (6020), or refuses outright
# if the agent never claimed (6019). Before 07fcee4 the agent's public id was
# enough, which meant anchoring over somebody else's agent needed only a value
# published in the manifest and in its Agent Card.
#
# The claim is signed from the AGENT's wallet home and the policy from the
# owner's, because that separation is the property: one instruction demanding
# both signatures would demand both keys in one wallet.
CLAIM=$(spel --idl "$IDL" --program "$PROGRAM" pda claim --agent-id "$AGENT_HEX" 2>/dev/null | tail -n1)
[ -n "$CLAIM" ] || die "could not resolve the claim account"
LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  wallet_run account sync-private </dev/null >/dev/null 2>&1
COUT=$(LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  spel --idl "$IDL" --program "$PROGRAM" -- claim_agent \
    --agent "Private/$AGENT" --owner-id "$OWNER_HEX" 2>&1)
echo "$COUT" | grep -q 'tx_hash: [0-9a-f]\{64\}' \
  || { echo "$COUT" | tail -8; die "claim_agent submitted no transaction"; }
# A hash is not a landed account. create_policy reads this one, so wait until
# the chain actually has it rather than letting a race read as a 6019.
for _ in $(seq 1 30); do
  curl -s -m 5 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$CLAIM"'"]}' \
  | grep -q '"result":null' || break
  sleep 2
done
curl -s -m 5 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$CLAIM"'"]}' \
| grep -q '"result":null' && die "the claim never landed at $CLAIM"
echo "  agent claimed $TEST_SIGNER as the only account that may anchor it"

spel --idl "$IDL" --program "$PROGRAM" -- create_policy --owner "Public/$TEST_SIGNER" \
  --agent-id "$AGENT_HEX" \
  --per-tx "$PER_TX" --per-period "$PER_PERIOD" --period-blocks "$PERIOD" >/dev/null 2>&1 \
  || die "create_policy failed"
echo "  policy anchored"

# Anchoring is once per agent. A second create_policy for the same agent — any
# limits, any signer — resolves to the account just created and `init` refuses
# it. That is the anchoring bypass closed, checked against a real sequencer
# rather than argued: an attacker holding the agent's key gets this too.
if spel --idl "$IDL" --program "$PROGRAM" -- create_policy --owner "Public/$TEST_SIGNER" \
     --agent-id "$AGENT_HEX" \
     --per-tx 340282366920938463463374607431768211455 \
     --per-period 340282366920938463463374607431768211455 \
     --period-blocks "$PERIOD" 2>&1 | grep -q 'Transaction confirmed'; then
  die "a SECOND, unlimited policy was anchored for the same agent"
fi
echo "  a second, unlimited policy for the same agent was refused"

say "[5/5] the agent pays inside its envelope, and is refused outside it"
# A second party, with its own wallet home, so the payer holds no key for the
# account it pays. Public, because a private account's state cannot be built
# without its viewing key and its balance cannot be read back with `getAccount`.
# It is initialised under the transfer program first: crediting a default-owned
# public account makes the transfer program claim it, and a claim nobody signed
# for is rejected.
SERVER_HOME="$WORK/server"; mkdir -p "$SERVER_HOME"
cp "$WALLET_HOME/wallet_config.json" "$SERVER_HOME/"
LEE_WALLET_HOME_DIR="$SERVER_HOME" NSSA_WALLET_HOME_DIR="$SERVER_HOME" \
  wallet_run account new public </dev/null >/dev/null 2>&1
RECIP=$(LEE_WALLET_HOME_DIR="$SERVER_HOME" NSSA_WALLET_HOME_DIR="$SERVER_HOME" \
  wallet_run account list </dev/null 2>/dev/null \
  | grep -oE 'Public/[1-9A-HJ-NP-Za-km-z]{32,}' | sed 's|Public/||' | head -n1)
[ -n "$RECIP" ] || die "could not create the recipient account"
LEE_WALLET_HOME_DIR="$SERVER_HOME" NSSA_WALLET_HOME_DIR="$SERVER_HOME" \
  wallet_run auth-transfer init --account-id "Public/$RECIP" </dev/null >/dev/null 2>&1 \
  || die "could not initialise the recipient account"
echo "  recipient Public/$RECIP  (separate wallet home; the payer has no key for it)"

balance_of() {
  curl -s -m 5 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"getAccount","params":["'"$1"'"]}' \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("result",{}).get("balance",0))'
}
BEFORE=$(balance_of "$RECIP")

# `--bin-auth-transfer` is not cosmetic. `spend` moves no balance itself — LEZ
# rule 5 forbids a program from debiting an account it does not own — so it
# chains a call into the transfer program that does own the agent's account, and
# the privacy circuit has to be handed that program's ELF to prove the inner
# call.
AUTH_TRANSFER="$ROOT/artifacts/programs/authenticated_transfer.bin"
[ -f "$AUTH_TRANSFER" ] || die "no transfer program at $AUTH_TRANSFER"
# The period the spend is accounted against. The guest refuses any window that
# is not a multiple of `period_blocks` and pins the transaction to it, so this
# is read off the chain rather than passed as a constant. On a chain that has
# just been started from genesis every block so far is inside period 0.
window_start() {
  local h
  h=$(curl -s -m 5 -X POST "$RPC" -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":1,"method":"getLastBlockId","params":[]}' \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"])')
  ( cd "$ROOT" && cargo run --quiet --release -p agent-policy-core \
      --example window-start -- "$h" "$PERIOD" ) | cut -d' ' -f1
}

spend_at() { # amount
  LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  spel --idl "$IDL" --program "$PROGRAM" --bin-auth-transfer "$AUTH_TRANSFER" \
    -- spend --agent "Private/$AGENT" --recipient "Public/$RECIP" \
    --amount "$1" --window-start "$(window_start)" 2>&1
}

OUT=$(spend_at 50)
TX=$(echo "$OUT" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
[ -n "$TX" ] || { echo "$OUT" | tail -8; die "the autonomous spend produced no transaction"; }
echo "  50 (inside the envelope) -> $TX"

# A transaction hash is not a payment. An earlier version of this instruction
# produced confirmed, on-chain proofs that a policy PERMITTED an amount and
# moved nothing at all, so the assertion is on the balance.
AFTER=0
for _ in $(seq 1 30); do
  AFTER=$(balance_of "$RECIP")
  [ "$((AFTER - BEFORE))" -eq 50 ] && break
  sleep 2
done
[ "$((AFTER - BEFORE))" -eq 50 ] \
  || die "the recipient went $BEFORE -> $AFTER: the spend did not transfer 50"
echo "  recipient balance $BEFORE -> $AFTER  (+50, read back from the chain)"

# The negative half. A spend over the per-transaction limit must fail without an
# owner approval — a run where both halves pass is not evidence of a threshold.
OUT=$(spend_at 5000)
if echo "$OUT" | grep -q 'tx_hash: [0-9a-f]\{64\}'; then
  echo "$OUT" | tail -6
  die "5000 was ACCEPTED without an owner approval — the threshold is not enforced"
fi
echo "  5000 (outside it) -> refused, as it must be"
END=$(balance_of "$RECIP")
[ "$END" -eq "$AFTER" ] || die "the refused spend still moved balance: $AFTER -> $END"
echo "  recipient balance unchanged at $END after the refusal"

say "VERIFIED"
echo "A real sequencer, RISC0_DEV_MODE=0 throughout. The policy was anchored by"
echo "address, a payment inside the envelope moved balance between two parties"
echo "unattended, and a payment outside it was refused with no owner approval"
echo "present and no balance moved."
