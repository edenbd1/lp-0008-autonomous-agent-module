#!/usr/bin/env bash
# Deploy three agents on the public LEZ testnet, one per skill category, and
# anchor each one's spending policy on chain.
#
#   SIGNER=<funded public id> ./scripts/deploy-agents.sh
#
# The prize asks for "three separate agents deployed on LEZ testnet — one per
# default skill category (Storage, Messaging, and Blockchain) — each with a
# demonstrated, reproducible deployment and evidence provided". This is that,
# and it writes a manifest so the evidence is a file rather than a screenshot.
#
# Each agent gets its own shielded account and its own policy. The policies
# differ deliberately: identical limits would produce the same policy hash for
# the same owner, and the point of anchoring by address is easier to see when
# three different envelopes give three different addresses.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

: "${SIGNER:?set SIGNER to a funded public account id}"
RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
WALLET="${WALLET_BIN:-wallet}"
SPEL="${SPEL_BIN:-spel}"
SIGNER_HOME="${LEE_WALLET_HOME_DIR:-$HOME/.lez-wallet}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
MANIFEST="${MANIFEST:-artifacts/agents.tsv}"
# Anchoring is single-use by design: create_policy is declared #[account(init)]
# and init refuses to overwrite. Once an agent is funded its identity is stable,
# so a second run derives the same policy hash and is correctly refused. Without
# a record of what was already anchored, that correct refusal looks identical to
# a failure and wipes the entry out of the manifest.
LEDGER="${LEDGER:-artifacts/anchored.tsv}"
[ -f "$LEDGER" ] || printf 'policy_hash\tcreate_tx\n' > "$LEDGER"
anchored_tx() { awk -F'\t' -v h="$1" 'NR>1 && $1==h {print $2; exit}' "$LEDGER"; }
# Each agent is funded so it can pay for real; spend moves balance, not just proof.
FUND_AMOUNT="${FUND_AMOUNT:-40}"

mkdir -p artifacts
: > "$MANIFEST"
printf 'category\tagent_id\tpolicy_hash\tper_tx\tper_period\tperiod_blocks\tcreate_tx\n' >> "$MANIFEST"

confirmed() {
  curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}" \
    | grep -qE '"result":\['
}

# A fresh private account per agent. The agent signs with its own shielded
# identity — the prize requires it to be "indistinguishable on-chain from any
# other account holder", which a shared key would not be.
#
# The home is persistent and lives OUTSIDE the repository: an agent that cannot
# sign again is not an agent, and its key must never be committed. AGENT_HOMES
# defaults under $HOME for that reason.
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
new_agent() {
  local home="$AGENT_HOMES/$1"
  mkdir -p "$home"
  printf '{ "sequencers": [{ "sequencer_addr": "%s" }], "seq_poll_timeout": "30s", "seq_tx_poll_max_blocks": 15, "seq_poll_max_retries": 10, "seq_block_poll_max_amount": 100, "calibration_limit": 100 }\n' "$RPC" > "$home/wallet_config.json"
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" account new private </dev/null >/dev/null 2>&1
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" account list </dev/null 2>/dev/null \
    | grep -oE 'Private/[1-9A-HJ-NP-Za-km-z]+' | sed 's|Private/||' | tail -n1
}

# Owner id as 32 bytes of hex, derived from the funded signer's base58 id so the
# policy hash commits to a real owner rather than a placeholder.
OWNER_HEX=$(python3 -c "
import hashlib,sys
print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$SIGNER")
echo "owner  $SIGNER"
echo "       committed into every policy hash as $OWNER_HEX"
echo

# The program has to be on chain before anything can call it, and spel does not
# put it there. Calling an undeployed program does not error usefully — every
# create_policy just fails to land, which reads like a network problem and is
# not one. Deploy is content-addressed and idempotent: re-deploying an identical
# binary is a no-op that costs one round trip.
DEPLOY_TX=$(python3 -c "
import hashlib,struct
b=open('$PROGRAM','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")
if confirmed "$DEPLOY_TX"; then
  echo "program  $DEPLOY_TX  already on chain"
else
  echo "program  $DEPLOY_TX  deploying"
  # The owner's wallet signs the deployment, and its home is only exported
  # further down inside deploy_agent — without it here the wallet has no config
  # and fails silently.
  LEE_WALLET_HOME_DIR="$SIGNER_HOME" NSSA_WALLET_HOME_DIR="$SIGNER_HOME" \
    "$WALLET" deploy-program "$PROGRAM" </dev/null >/dev/null 2>&1
  for _ in $(seq 1 25); do sleep 6; confirmed "$DEPLOY_TX" && break; done
  confirmed "$DEPLOY_TX" || { echo "  the program did not deploy" >&2; exit 1; }
  echo "         landed"
fi
echo

# Fund an agent, and report the account that ended up holding the money.
#
# A shielded transfer to (npk, vpk) does not credit an existing account — it
# creates a new note, with its own account id, under the same keys. So funding
# after anchoring would leave the policy committed to an account with nothing
# in it, and `spend` would fail on the balance it is supposed to move. Fund
# first, find the account that actually holds the balance, and anchor on that.
best_funded() { # home -> account id holding the most, and its balance on stdout
  local home="$1" best="" bestbal=0 a b
  for a in $(LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
               "$WALLET" account list </dev/null 2>/dev/null \
             | grep -oE 'Private/[1-9A-HJ-NP-Za-km-z]+'); do
    b=$(LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
          "$WALLET" account get --account-id "$a" </dev/null 2>/dev/null \
        | grep -o '"balance":[0-9]*' | cut -d: -f2 | head -1)
    [ -n "$b" ] && [ "$b" -gt "$bestbal" ] && { bestbal="$b"; best="${a#Private/}"; }
  done
  printf '%s %s\n' "$best" "$bestbal"
}

fund_agent() { # category seed_account amount
  local home="$AGENT_HOMES/$1" seed="$2" amount="$3"
  local keys="$home/funding.keys"

  # An agent funded by an earlier run is still funded: the keys are persistent
  # and so are its notes. Re-sending would spend the owner's balance to buy
  # something already owned, and the owner's balance is finite on a testnet.
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" account sync-private </dev/null >/dev/null 2>&1
  local have; have=$(best_funded "$home")
  if [ "${have##* }" -ge "$amount" ] 2>/dev/null; then
    echo "${have%% *}"; return 0
  fi
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" account show-keys --account-id "Private/$seed" </dev/null 2>/dev/null \
    | grep -E "^[0-9a-f]{64,}$" > "$keys"
  [ -s "$keys" ] || { echo "  could not export the agent's keys" >&2; return 1; }

  LEE_WALLET_HOME_DIR="$SIGNER_HOME" NSSA_WALLET_HOME_DIR="$SIGNER_HOME" \
    "$WALLET" auth-transfer send --from "Public/$SIGNER" \
      --to-keys "$keys" --amount "$amount" </dev/null >/dev/null 2>&1
  rm -f "$keys"

  # Give the sequencer a moment, then take whichever account holds the most.
  # Twenty rounds was not enough: a funding transfer landed and the agent it
  # created showed up minutes after the poll gave up, so the run reported a
  # failure that had actually succeeded and left the money stranded under keys
  # nothing referenced. The pre-check above now recovers those, and this waits
  # long enough not to create them.
  local i have
  for i in $(seq 1 60); do
    sleep 6
    LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
      "$WALLET" account sync-private </dev/null >/dev/null 2>&1
    have=$(best_funded "$home")
    if [ "${have##* }" -ge "$amount" ] 2>/dev/null; then echo "${have%% *}"; return 0; fi
  done
  return 1
}

deploy_agent() { # category per_tx per_period period_blocks fund
  local cat="$1" per_tx="$2" per_period="$3" period="$4" fund="${5:-$FUND_AMOUNT}"
  echo "[$cat] new shielded account"
  local seed; seed=$(new_agent "$cat")
  if [ -z "$seed" ]; then echo "  FAILED to create an account" >&2; return 1; fi

  echo "  funding it with $fund so it can actually pay"
  local agent; agent=$(fund_agent "$cat" "$seed" "$fund")
  if [ -z "$agent" ]; then echo "  FAILED to fund the agent" >&2; return 1; fi
  echo "  agent $agent  (holds at least $fund)"
  echo "  keys  $AGENT_HOMES/$cat  (outside the repository, never committed)"

  local agent_hex; agent_hex=$(python3 -c "
import hashlib,sys
print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$agent")

  # The policy hash is computed by the same code the on-chain program runs, so
  # a mismatch here would be caught by create_policy rather than silently
  # producing an account nobody can spend against.
  local policy_hash; policy_hash=$(cargo run --quiet --release -p agent-policy-core --example policy-hash -- \
      "$OWNER_HEX" "$agent_hex" "$per_tx" "$per_period" "$period" 2>/dev/null)
  if [ -z "$policy_hash" ]; then echo "  FAILED to compute the policy hash" >&2; return 1; fi
  echo "  policy $policy_hash  (per-tx $per_tx, per-period $per_period, window $period blocks)"

  # Resync the owner before proving. Funding this agent just changed the
  # owner's account on chain, and create_policy is proved against the wallet's
  # local view: prove against the state from before the transfer and the proof
  # builds, spel returns a hash, and the sequencer never lands it. Nothing
  # reports an error — which is what made three anchors fail at once after the
  # funding step was introduced, while the same script had worked before it.
  export LEE_WALLET_HOME_DIR="$SIGNER_HOME" NSSA_WALLET_HOME_DIR="$SIGNER_HOME"
  "$WALLET" account sync-private </dev/null >/dev/null 2>&1
  local out; out=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
    -- create_policy --owner "Public/$SIGNER" \
    --policy-hash "$policy_hash" --owner-id "$OWNER_HEX" --agent-id "$agent_hex" \
    --per-tx "$per_tx" --per-period "$per_period" --period-blocks "$period" 2>&1)
  local tx; tx=$(echo "$out" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
  if [ -z "$tx" ]; then
    # Before calling this a failure, ask whether this exact policy is already
    # anchored from an earlier run. If it is, that refusal is init doing its job.
    local prior; prior=$(anchored_tx "$policy_hash")
    if [ -n "$prior" ] && confirmed "$prior"; then
      echo "  create_policy $prior  already anchored (init refused a second one)"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$cat" "$agent" "$policy_hash" "$per_tx" "$per_period" "$period" "$prior" >> "$MANIFEST"
      echo
      return 0
    fi
    echo "  NO TRANSACTION — spel submitted nothing:" >&2
    echo "$out" | tail -8 >&2
    return 1
  fi
  for _ in $(seq 1 25); do sleep 6; confirmed "$tx" && break; done
  if confirmed "$tx"; then
    echo "  create_policy $tx  landed"
    printf '%s\t%s\n' "$policy_hash" "$tx" >> "$LEDGER"
  else echo "  create_policy $tx  NOT CONFIRMED" >&2; return 1; fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$cat" "$agent" "$policy_hash" "$per_tx" "$per_period" "$period" "$tx" >> "$MANIFEST"
  echo
}

# One per default skill category, with envelopes sized to what each does: a
# storage agent pays small and often, a blockchain agent moves more per call.
deploy_agent storage    50   500  1000  10
deploy_agent messaging  25   250  1000  10
deploy_agent blockchain 200 1000  1000  30

echo "manifest: $MANIFEST"
column -t -s$'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
