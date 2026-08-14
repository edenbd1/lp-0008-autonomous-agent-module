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
new_agent() {
  local home; home=$(mktemp -d)
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

deploy_agent() { # category per_tx per_period period_blocks
  local cat="$1" per_tx="$2" per_period="$3" period="$4"
  echo "[$cat] new shielded account"
  local agent; agent=$(new_agent)
  if [ -z "$agent" ]; then echo "  FAILED to create an account" >&2; return 1; fi
  echo "  agent $agent"

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

  export LEE_WALLET_HOME_DIR="$SIGNER_HOME" NSSA_WALLET_HOME_DIR="$SIGNER_HOME"
  local out; out=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
    -- create_policy --owner "Public/$SIGNER" \
    --policy-hash "$policy_hash" --owner-id "$OWNER_HEX" --agent-id "$agent_hex" \
    --per-tx "$per_tx" --per-period "$per_period" --period-blocks "$period" 2>&1)
  local tx; tx=$(echo "$out" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
  if [ -z "$tx" ]; then
    echo "  NO TRANSACTION — spel submitted nothing:" >&2
    echo "$out" | tail -8 >&2
    return 1
  fi
  for _ in $(seq 1 25); do sleep 6; confirmed "$tx" && break; done
  if confirmed "$tx"; then echo "  create_policy $tx  landed"
  else echo "  create_policy $tx  NOT CONFIRMED" >&2; return 1; fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$cat" "$agent" "$policy_hash" "$per_tx" "$per_period" "$period" "$tx" >> "$MANIFEST"
  echo
}

# One per default skill category, with envelopes sized to what each does: a
# storage agent pays small and often, a blockchain agent moves more per call.
deploy_agent storage    50   500  1000
deploy_agent messaging  25   250  1000
deploy_agent blockchain 200 1000  1000

echo "manifest: $MANIFEST"
column -t -s$'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
