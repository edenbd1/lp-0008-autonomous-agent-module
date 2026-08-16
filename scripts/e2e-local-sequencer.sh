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
# lifecycle over JSON-RPC: deploy `agent_verifier.bin`, anchor a policy with the
# two signatures this program now requires (`claim_agent`, then `create_policy`),
# prove a second anchor is refused, and then spend twice — once inside the
# anchored envelope and once outside it — with the sequencer verifying a real
# Risc0 receipt each time. Nothing is mocked.
#
#   ./scripts/e2e-local-sequencer.sh
#
# THIS HEADER DESCRIBED A DIFFERENT SCRIPT, and the wrong half is recorded rather
# than quietly deleted, because every line of it read as a checkable claim:
#   - "deploy both programs, commit a distribution" — one program is deployed.
#     `authenticated_transfer.bin` is checked for existence and passed as
#     `--bin-auth-transfer`; it is never deployed, and there is no distribution
#     anywhere in this file. That vocabulary belongs to some other program.
#   - "PRIVACY_PRESERVING_CIRCUIT_ID … the resulting marker PDA" — neither name
#     occurs in the body. What steps 4 and 5 read back is the policy account.
#   - "COUNT1 / COUNT2" — neither variable is parsed. The claim count is fixed at
#     one spend inside the envelope and one outside it.
#
# Env:
#   LEZ_SRC     checkout of logos-execution-zone (default: ./_external/lez)
#   PORT        sequencer RPC port           (default: first free from 3141)
#   KEEP        set to 1 to leave the sequencer running for inspection
#
# RISC0_DEV_MODE is NOT set here. It is inherited from the caller, and the
# workflow that runs this sets it to 0; `demo.sh` exports it itself and this
# script deliberately does not, so a local run cannot be silently downgraded by
# this file. Check the environment, not this comment.
#
# Budget: each spend is a real proof, measured at ~150 s.

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

# ---------------------------------------------------------------------------
# SHOWING THE WAIT.
#
# Every proof in here is a `wallet_run … >/dev/null 2>&1` that returns when it
# returns. On a fast runner that is 25 minutes; on 2026-08-16 it was 1h42, three
# times over, and the job was killed at its 340-minute cap in the middle of the
# fourth. What the log held for those five hours was four lines. A run that
# prints nothing while it works cannot be told from one that has hung, and the
# only way to learn that runner was three times slower than usual was to read
# the timestamps after it was dead.
#
# So: a line a minute while a proof runs, the elapsed time of each one when it
# lands, and -- once the first one has landed and there is a rate to reason from
# -- what the rest of the run will cost at that rate, against the cap. That last
# line is the one that would have said "this run cannot finish" at minute forty,
# instead of leaving it to be discovered at hour five.
T_START=$(date +%s)
PROOFS_DONE=0; PROOFS_SECS=0
# How many `beat` calls a full run makes. Derived at the end of this file by the
# check that every stage name appears, not guessed: fund, claim, anchor, the
# refused second anchor, the spend inside the envelope, the spend outside it.
PROOFS_EXPECTED=6
CAP_SECS=$(( ${E2E_CAP_MINUTES:-340} * 60 ))
# A minute between heartbeats in a run that lasts hours. Overridable so the
# machinery can be exercised in seconds rather than trusted -- a heartbeat that
# has never been watched is a heartbeat nobody knows the shape of.
BEAT_SECS=${E2E_BEAT_SECONDS:-60}

hms() { printf '%dh%02dm%02ds' $(($1/3600)) $((($1%3600)/60)) $(($1%60)); }

# What the machine is. Printed once, at the top, because "the runner was slow"
# is a claim about hardware and it should be readable in the log rather than
# inferred from timestamps a day later.
machine_facts() {
  echo "  host      $(uname -s) $(uname -m), $( \
    (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?') ) cpu(s)"
  if [ -r /proc/cpuinfo ]; then
    echo "  cpu       $(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -1)"
  elif command -v sysctl >/dev/null 2>&1; then
    echo "  cpu       $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
  fi
  if [ -r /proc/meminfo ]; then
    echo "  memory    $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
  fi
  echo "  load      $(uptime 2>/dev/null | sed 's/.*load average[s]*: *//')"
  echo "  r0vm      $(r0vm --version 2>/dev/null || echo 'not on PATH')"
  echo "  dev mode  RISC0_DEV_MODE=${RISC0_DEV_MODE:-<unset>}   (0 = real proofs)"
}

# The chain's own tip, so a heartbeat says whether the sequencer is still making
# blocks or has stopped underneath a proof that will never land.
tip_block() {
  curl -s -m 5 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"getLastBlockId","params":[]}' 2>/dev/null \
  | grep -oE '"result":[0-9]+' | grep -oE '[0-9]+' | head -1
}

# CPU and resident size of a process AND its descendants. During a proof the
# work is in r0vm, which is a child of the wallet or of spel, so reporting only
# the process we launched would report a parent asleep on a wait().
procstat() {
  local root="$1"
  ps -eo pid,ppid,pcpu,rss 2>/dev/null | awk -v root="$root" '
    NR > 1 { cpu[$1] = $3; rss[$1] = $4; parent[$1] = $2; pids[n++] = $1 }
    END {
      want[root] = 1
      for (pass = 0; pass < 12; pass++)
        for (i = 0; i < n; i++)
          if (want[parent[pids[i]]]) want[pids[i]] = 1
      for (i = 0; i < n; i++)
        if (want[pids[i]]) { c += cpu[pids[i]]; r += rss[pids[i]]; k++ }
      if (k) printf "%d proc(s), %.0f%% cpu, %.1f GiB rss", k, c, r/1048576
    }'
}

beat() {   # beat <label> <command...>  -- run it quietly, narrate the wait
  local label="$1"; shift
  local t0 pid rc now
  t0=$(date +%s)
  "$@" </dev/null >/dev/null 2>&1 & pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    sleep "$BEAT_SECS"
    kill -0 "$pid" 2>/dev/null || break
    now=$(date +%s)
    printf '    %s: still proving, %s in (run %s) | block %s | %s\n' \
      "$label" "$(hms $((now - t0)))" "$(hms $((now - T_START)))" \
      "$(tip_block)" "$(procstat "$pid")"
    [ -f "${WORK:-}/sequencer.log" ] && \
      printf '      seq| %s\n' "$(tail -1 "$WORK/sequencer.log" 2>/dev/null | cut -c1-150)"
  done
  wait "$pid"; rc=$?
  now=$(date +%s)
  PROOFS_DONE=$((PROOFS_DONE + 1))
  PROOFS_SECS=$((PROOFS_SECS + now - t0))
  printf '    %s: %s (block %s)\n' "$label" "$(hms $((now - t0)))" "$(tip_block)"
  # The prediction, from measured rate rather than from the comment at the top.
  if [ "$PROOFS_DONE" -ge 1 ] && [ "$PROOFS_DONE" -lt "$PROOFS_EXPECTED" ]; then
    local rate=$((PROOFS_SECS / PROOFS_DONE))
    local left=$(( (PROOFS_EXPECTED - PROOFS_DONE) * rate ))
    local projected=$(( now - T_START + left ))
    if [ "$projected" -gt "$CAP_SECS" ]; then
      printf '    PACE: %s a proof, %d left -> about %s in total, past the %s cap.\n' \
        "$(hms "$rate")" "$((PROOFS_EXPECTED - PROOFS_DONE))" "$(hms "$projected")" "$(hms "$CAP_SECS")"
      printf '    This run is on a slow machine and will not finish. Re-dispatch it.\n'
    else
      printf '    pace: %s a proof, %d left, about %s in total against a %s cap\n' \
        "$(hms "$rate")" "$((PROOFS_EXPECTED - PROOFS_DONE))" "$(hms "$projected")" "$(hms "$CAP_SECS")"
    fi
  fi
  return $rc
}

# The same, for a call whose output the script goes on to read. `beat` throws
# stdout away, and three of these calls are parsed for a tx hash and for
# `Transaction confirmed` -- wrapping those in `beat` would have made the script
# quieter AND blinder.
beat_cap() {   # beat_cap <label> <outfile> <command...>
  local label="$1" out="$2"; shift 2
  local t0 pid rc now
  t0=$(date +%s)
  "$@" </dev/null >"$out" 2>&1 & pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    sleep "$BEAT_SECS"
    kill -0 "$pid" 2>/dev/null || break
    now=$(date +%s)
    printf '    %s: still proving, %s in (run %s) | block %s | %s\n' \
      "$label" "$(hms $((now - t0)))" "$(hms $((now - T_START)))" \
      "$(tip_block)" "$(procstat "$pid")"
    [ -f "${WORK:-}/sequencer.log" ] && \
      printf '      seq| %s\n' "$(tail -1 "$WORK/sequencer.log" 2>/dev/null | cut -c1-150)"
  done
  wait "$pid"; rc=$?
  now=$(date +%s)
  PROOFS_DONE=$((PROOFS_DONE + 1))
  PROOFS_SECS=$((PROOFS_SECS + now - t0))
  printf '    %s: %s (block %s)\n' "$label" "$(hms $((now - t0)))" "$(tip_block)"
  return $rc
}

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

say "[0/5] the machine this is being proved on"
machine_facts

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
beat "funding the agent" \
  wallet_run auth-transfer send --from "Public/$TEST_SIGNER" \
    --to-keys "$WORK/agent.keys" --amount 1000 \
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
# if the agent never claimed (6019). Before 8dee766 the agent's public id was
# enough, which meant anchoring over somebody else's agent needed only a value
# published in the manifest and in its Agent Card.
#
# The claim is signed from the AGENT's wallet home and the policy from the
# owner's, because that separation is the property: one instruction demanding
# both signatures would demand both keys in one wallet.
# The claim account is NOT resolved with `spel pda claim` here. That subcommand
# takes the first instruction in the IDL carrying an account of that name, which
# is `claim_agent`, and there the seed is the SIGNING ACCOUNT (kind "account")
# rather than an argument — so no --agent-id can satisfy it and the lookup
# returns nothing. `create_policy` reads the same account from an arg-seeded
# definition; the address is identical, but only one of the two is derivable
# from a value on this command line, and it is not the one `pda` picks.
#
# Nothing is lost by not having the address: what matters is that the claim
# LANDED, and spel says so itself. If it had not, the create_policy below reads
# an uninitialised account and refuses with 6019, loudly.
LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  wallet_run account sync-private </dev/null >/dev/null 2>&1
beat_cap "claim_agent" "$WORK/claim.out" \
  env LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
  spel --idl "$IDL" --program "$PROGRAM" -- claim_agent \
    --agent "Private/$AGENT" --owner-id "$OWNER_HEX"
COUT=$(cat "$WORK/claim.out")
CLAIM_TX=$(echo "$COUT" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
[ -n "$CLAIM_TX" ] \
  || { echo "$COUT" | tail -8; die "claim_agent submitted no transaction"; }
echo "$COUT" | grep -q 'Transaction confirmed' \
  || { echo "$COUT" | tail -8; die "claim_agent built $CLAIM_TX but it never confirmed"; }
echo "  agent claimed $TEST_SIGNER as the only account that may anchor it -> $CLAIM_TX"

beat "create_policy" \
  spel --idl "$IDL" --program "$PROGRAM" -- create_policy --owner "Public/$TEST_SIGNER" \
    --agent-id "$AGENT_HEX" \
    --per-tx "$PER_TX" --per-period "$PER_PERIOD" --period-blocks "$PERIOD" \
  || die "create_policy failed"
echo "  policy anchored"

# Anchoring is once per agent. A second create_policy for the same agent — any
# limits, any signer — resolves to the account just created and `init` refuses
# it. That is the anchoring bypass closed, checked against a real sequencer
# rather than argued: an attacker holding the agent's key gets this too.
# `grep -q 'Transaction confirmed'` is an ABSENCE, and this is the repository's
# central security claim — that a bigger ceiling has nowhere to go. Measured: a
# spel invocation that cannot possibly anchor anything (a --idl path that does
# not exist) prints `Error reading IDL ...` and no "Transaction confirmed", so
# the guard announced the refusal on a call that never reached the chain. A
# renamed flag, a moved binary, an unreachable sequencer, a typo in the amount:
# every one of them passed. Two positives are required instead.
SECOND=$(spel --idl "$IDL" --program "$PROGRAM" -- create_policy --owner "Public/$TEST_SIGNER" \
     --agent-id "$AGENT_HEX" \
     --per-tx 340282366920938463463374607431768211455 \
     --per-period 340282366920938463463374607431768211455 \
     --period-blocks "$PERIOD" 2>&1)
if echo "$SECOND" | grep -q 'Transaction confirmed'; then
  echo "$SECOND" | tail -8
  die "a SECOND, unlimited policy was anchored for the same agent"
fi
# It has to have been REFUSED, not merely not-confirmed. The first version of
# this asked spel for a program error string, and that is unavailable on this
# path: the sequencer DISCARDS a failing transaction at block-build time rather
# than returning a reason, so a refused anchor and a never-submitted one both
# come back "Transaction not found in preconfigured amount of blocks". Demanding
# a message the chain does not emit made a correct refusal look like a broken
# test.
#
# The positive that IS available is the submitted hash. spel prints one only
# after it has read the IDL, built the instruction against the program, and
# handed the transaction to the sequencer — the exact steps a renamed flag, a
# moved binary or a bad --idl path never reach, which is what the old guard was
# protecting against. So: a hash was submitted, that hash is NOT on chain, and
# the account still reads what the owner anchored. Three positives, and no
# single failure mode produces all three.
SECOND_TX=$(echo "$SECOND" | grep -oE 'tx_hash: [0-9a-f]{64}' | tail -1 | awk '{print $2}')
[ -n "$SECOND_TX" ] \
  || { echo "$SECOND" | tail -8
       die "the second anchor never even submitted a transaction, so it did not reach the program and nothing was demonstrated"; }
SECOND_ON_CHAIN=$(curl -s -m 10 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$SECOND_TX\"]}" \
  | python3 -c "import json,sys; print('yes' if (json.load(sys.stdin).get('result') is not None) else 'no')")
[ "$SECOND_ON_CHAIN" = "no" ] \
  || die "the second, unlimited anchor $SECOND_TX IS on chain — the bypass is open"
echo "  submitted $SECOND_TX, and the chain does not hold it"
# And the account still says what the OWNER anchored, which is the property the
# refusal is about. An unlimited policy that was refused and an unlimited policy
# that was written both leave "no Transaction confirmed" in some failure mode.
STILL=$(curl -s -m 10 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$POLICY\"]}" \
  | python3 -c "
import json,sys
r = json.load(sys.stdin).get('result') or {}
d = bytes(r.get('data') or b'')
print(int.from_bytes(d[33:49],'little') if len(d) == 97 and d[0] == 1 else 'unreadable')")
[ "$STILL" = "$PER_TX" ] \
  || die "after the second anchor the policy account reads per_tx=$STILL, not the owner's $PER_TX"
echo "  a second, unlimited policy for the same agent was refused, and the account still reads per_tx=$STILL"

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
# A BALANCE THAT WAS NOT READ IS NOT A BALANCE OF ZERO. `balance_of` prints
# nothing when curl times out or the answer does not parse, and every use of
# these two values below is `$((AFTER - BEFORE))`, where shell arithmetic reads
# an empty string as 0. So an unread BEFORE silently turned "the recipient went
# up by exactly 50" into "the recipient now holds exactly 50" — a delta measured
# against a baseline nobody took, on the assertion whose own comment says it is
# "the only reason that was caught rather than shipped as a green run". The same
# defect and the same fix are in scripts/a2a-task.sh; demonstrated there, with
# the before-read pointed at a closed port, the assertion went green on a
# settlement nothing had been read about.
balance_or_die() { # account what-for -> a decimal balance on stdout, or stop.
  # `X=$(balance_or_die …) || die …` at the call site: an `exit` inside a command
  # substitution ends only that subshell, so the status has to be taken there.
  local v; v=$(balance_of "$1")
  case "$v" in ''|*[!0-9]*) exit 1 ;; esac
  printf '%s\n' "$v"
}
BEFORE=$(balance_or_die "$RECIP") \
  || die "could not read the recipient's balance before the spend, so there is no baseline to compare against"

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
  # Resync the agent first, every time. A private account's on-chain state moves
  # whenever it signs — and this one has just signed `claim_agent` — while the
  # proof is built against the wallet's LOCAL view. Prove against a stale one
  # and the proof still builds, spel still prints a `tx_hash`, and the sequencer
  # simply never lands the transaction, with nothing reported anywhere.
  #
  # This is not hypothetical: it is how this script failed once the two-signature
  # anchoring was added. Preflight 31900267734 printed
  #   50 (inside the envelope) -> 713dd5cb2ccfa1d6b00a00acbb80e0771e68822d286…
  # and the recipient went 0 -> 0. The hash was real and meant nothing. The
  # balance assertion below is the only reason that was caught rather than
  # shipped as a green run, which is also why that assertion exists.
  LEE_WALLET_HOME_DIR="$AGENT_HOME" NSSA_WALLET_HOME_DIR="$AGENT_HOME" \
    wallet_run account sync-private </dev/null >/dev/null 2>&1
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
AFTER=""
for _ in $(seq 1 30); do
  AFTER=$(balance_of "$RECIP")
  case "$AFTER" in ''|*[!0-9]*) sleep 2; continue ;; esac
  [ "$((AFTER - BEFORE))" -eq 50 ] && break
  sleep 2
done
case "$AFTER" in
  ''|*[!0-9]*) die "the recipient's balance after the spend did not read back ('${AFTER}'), so nothing here says the money moved" ;;
esac
[ "$((AFTER - BEFORE))" -eq 50 ] \
  || die "the recipient went $BEFORE -> $AFTER: the spend did not transfer 50"
echo "  recipient balance $BEFORE -> $AFTER  (+50, read back from the chain)"

# The negative half, and BOTH the amount and the check are load-bearing.
#
# This asked for 5000 and accepted any refusal at all. The agent holds 1000 and
# has just paid 50, so 5000 is more than it has: a build with the ceiling
# deleted outright refuses that too, by E_INSUFFICIENT_BALANCE (6008) or by the
# transfer program's own checked_sub. The assertion could not fail while the
# property it exists to prove was absent, which is not a threshold test.
#
# So the amount is chosen to leave exactly one rule able to refuse it:
#
#   OVER=300  >  per_tx 100          the ceiling — the rule under test
#   OVER=300  <  950, what the agent still holds     so it cannot be 6008
#   OVER=300  <  450 left in this period             so it cannot be 6006
#
# and the refusal is checked BY CODE. "No tx_hash" is also what a spel that fell
# over for its own reasons produces, so the absence of a hash is necessary and
# nowhere near sufficient. Same discipline as use case 3's `attempt`.
OVER=300
OUT=$(spend_at "$OVER")
if echo "$OUT" | grep -q 'tx_hash: [0-9a-f]\{64\}'; then
  echo "$OUT" | tail -6
  die "$OVER was ACCEPTED without an owner approval — the threshold is not enforced"
fi
GOT=$(echo "$OUT" | grep -oE 'Program error [0-9]+: .*' | head -1)
echo "$GOT" | grep -q 'Program error 6005:' || {
  echo "$OUT" | tail -8
  die "$OVER was refused, but by '${GOT:-no program error at all}' — expected 6005, the per-transaction ceiling. This run proves something other than the threshold."
}
echo "  $OVER (over the per-tx ceiling of $PER_TX, and inside the balance) -> refused"
echo "         $GOT"
END=$(balance_or_die "$RECIP") \
  || die "the recipient's balance did not read back after the refusal, so nothing here says it is unchanged"
[ "$END" -eq "$AFTER" ] || die "the refused spend still moved balance: $AFTER -> $END"
echo "  recipient balance unchanged at $END after the refusal"

say "VERIFIED"
echo "A real sequencer, RISC0_DEV_MODE=0 throughout. The policy was anchored by"
echo "address, a payment inside the envelope moved balance between two parties"
echo "unattended, and a payment outside it was refused with no owner approval"
echo "present and no balance moved."
