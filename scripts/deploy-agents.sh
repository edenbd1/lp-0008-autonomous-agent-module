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
# The account that pays the agents must not be the account that signs their
# policies. `auth-transfer send` leaves its sender owned by the transfer
# program, and create_policy returns its signer as a post-state — a program
# cannot hand back an account another program owns. Sharing one account makes
# every anchor stop landing, silently, the moment the first agent is funded.
FUNDER="${FUNDER:-$SIGNER}"
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
[ -f "$LEDGER" ] || printf 'program\tpolicy_hash\tcreate_tx\n' > "$LEDGER"
# Keyed by (program, policy_hash), not by policy_hash alone. A policy account is
# a PDA of the *program*, so redeploying the program moves every policy to a new
# address that has never been initialised. A ledger keyed on the hash alone would
# report those as already anchored and skip the anchoring the new program needs.
anchored_tx() { awk -F'\t' -v p="$1" -v h="$2" 'NR>1 && $1==p && $2==h {print $3; exit}' "$LEDGER"; }
# Each agent is funded so it can pay for real; spend moves balance, not just proof.
FUND_AMOUNT="${FUND_AMOUNT:-40}"

mkdir -p artifacts
: > "$MANIFEST"
printf 'category\tagent_id\tpay_account\tpolicy_hash\tper_tx\tper_period\tperiod_blocks\tcreate_tx\n' >> "$MANIFEST"

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
  for _ in $(seq 1 24); do sleep 30; confirmed "$DEPLOY_TX" && break; done
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
    "$WALLET" auth-transfer send --from "Public/$FUNDER" \
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

# The account another agent pays into.
#
# `spel` can only address a private account whose keys the *sending* wallet
# holds, so one shielded agent cannot name another one's private account as a
# recipient: it fails with `KeyNotFoundError` before anything is built. Each
# agent therefore also keeps a public receiving account, which is what its Agent
# Card advertises as its payment address.
#
# It has to be initialised under the transfer program before anyone pays it.
# Crediting an account that still has the default owner makes the transfer
# program claim it, and a claim on a public account the payer did not sign for is
# rejected (`ClaimedUnauthorizedAccount`). `auth-transfer init` is the agent
# signing for its own account, once, so every later payment is an ordinary
# credit. It is also what makes the balance readable with `getAccount`: private
# notes are commitments and the RPC returns the default account for them, so a
# payment into a shielded account cannot be checked from outside.
claimed() { # account id -> true if some program already owns it
  ! curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
      -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$1\"]}" \
    | grep -q '"program_owner":\[0,0,0,0,0,0,0,0\]'
}

pay_account() { # category -> public account id on stdout
  local home="$AGENT_HOMES/$1" id ids
  ids=$(LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
          "$WALLET" account list </dev/null 2>/dev/null \
        | grep -oE 'Public/[1-9A-HJ-NP-Za-km-z]{32,}' | sed 's|Public/||')
  # An account this agent already initialised, if there is one. Preferring it
  # over "the first one listed" keeps the manifest stable across runs: `account
  # list` does not promise an order, and a run that picked a different account
  # would advertise a payment address nobody has ever paid.
  for id in $ids; do
    if claimed "$id"; then echo "$id"; return 0; fi
  done
  id=$(printf '%s\n' $ids | head -n1)
  if [ -z "$id" ]; then
    LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
      "$WALLET" account new public </dev/null >/dev/null 2>&1
    id=$(LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
           "$WALLET" account list </dev/null 2>/dev/null \
         | grep -oE 'Public/[1-9A-HJ-NP-Za-km-z]{32,}' | sed 's|Public/||' | head -n1)
  fi
  [ -n "$id" ] || return 1
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" auth-transfer init --account-id "Public/$id" </dev/null >/dev/null 2>&1
  claimed "$id" || return 1
  echo "$id"
}

deploy_agent() { # category per_tx per_period period_blocks fund [signer]
  local cat="$1" per_tx="$2" per_period="$3" period="$4" fund="${5:-$FUND_AMOUNT}"
  # Whether one signer can anchor all three depends on the signer, and the rule
  # is narrow. A signer that still has the DEFAULT program owner anchors exactly
  # once: on its second anchor its nonce is no longer zero, the SPEL macro drops
  # its post-state to dodge rule 7, and the state machine then rejects the
  # transaction with `DeclaredAccountMissingFromOutput`. A signer already owned
  # by a program — anything that has ever received a transfer — is exempt from
  # both and can anchor repeatedly (`DumJ4LCB…`, nonces 29 and 30, blocks 8050
  # and 8051). Fresh signers are passed below because they are unambiguous;
  # `SIGNER` may be reused if it is program-owned.
  local signer="${6:-$SIGNER}"
  local OWNER_HEX; OWNER_HEX=$(python3 -c "
import hashlib,sys
print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$signer")
  echo "  owner  $signer"
  echo "[$cat] new shielded account"
  local seed; seed=$(new_agent "$cat")
  if [ -z "$seed" ]; then echo "  FAILED to create an account" >&2; return 1; fi

  echo "  funding it with $fund so it can actually pay"
  local agent; agent=$(fund_agent "$cat" "$seed" "$fund")
  if [ -z "$agent" ]; then echo "  FAILED to fund the agent" >&2; return 1; fi
  echo "  agent $agent  (holds at least $fund)"
  echo "  keys  $AGENT_HOMES/$cat  (outside the repository, never committed)"

  local pay; pay=$(pay_account "$cat")
  if [ -z "$pay" ]; then echo "  FAILED to open a receiving account" >&2; return 1; fi
  echo "  pays into Public/$pay  (advertised in its Agent Card)"

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
  # sync-private does nothing for a public account, and the signer is public.
  # Its state changes with every policy it signs, so the second anchor of a run
  # was being proved against the state from before the first — which submits,
  # returns a hash, and never lands. `account get` refetches from the chain and
  # rewrites the stored state, which is what makes the next anchor provable.
  "$WALLET" account get --account-id "Public/$signer" </dev/null >/dev/null 2>&1
  local out; out=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
    -- create_policy --owner "Public/$signer" \
    --policy-hash "$policy_hash" --owner-id "$OWNER_HEX" --agent-id "$agent_hex" \
    --per-tx "$per_tx" --per-period "$per_period" --period-blocks "$period" 2>&1)
  local rc=$?
  local tx; tx=$(echo "$out" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
  # spel already tells us when an anchor failed — it prints
  # "Transaction NOT confirmed" and exits 1. Grepping only for tx_hash threw
  # that line away and turned a reported failure into a silent one, which is
  # most of why this took so long to find.
  if [ $rc -ne 0 ] || echo "$out" | grep -q "NOT confirmed"; then
    echo "  spel reported the anchor failed:" >&2
    echo "$out" | grep -E "NOT confirmed|error|Error" | head -3 | sed 's/^/    /' >&2
  fi
  if [ -z "$tx" ]; then
    # Before calling this a failure, ask whether this exact policy is already
    # anchored from an earlier run. If it is, that refusal is init doing its job.
    local prior; prior=$(anchored_tx "$DEPLOY_TX" "$policy_hash")
    if [ -n "$prior" ] && confirmed "$prior"; then
      echo "  create_policy $prior  already anchored (init refused a second one)"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$cat" "$agent" "$pay" "$policy_hash" "$per_tx" "$per_period" "$period" "$prior" >> "$MANIFEST"
      echo
      return 0
    fi
    echo "  NO TRANSACTION — spel submitted nothing:" >&2
    echo "$out" | tail -8 >&2
    return 1
  fi
  # Blocks are exactly 60 seconds apart, so 150s gave a transaction two or
  # three chances to be included and then called it dead. Wait twelve blocks.
  for _ in $(seq 1 24); do sleep 30; confirmed "$tx" && break; done
  if confirmed "$tx"; then
    echo "  create_policy $tx  landed"
    printf '%s\t%s\t%s\n' "$DEPLOY_TX" "$policy_hash" "$tx" >> "$LEDGER"
  else
    # A policy that is already anchored does not make spel fail: init refuses
    # inside the program, so the transaction is built, submitted, given a hash,
    # and then simply never lands — the same shape as every other failure on
    # this chain. So the ledger has to be consulted here, on the unconfirmed
    # path, and not only when spel submitted nothing.
    local prior; prior=$(anchored_tx "$DEPLOY_TX" "$policy_hash")
    if [ -n "$prior" ] && confirmed "$prior"; then
      echo "  create_policy $prior  already anchored (init refused a second one)"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$cat" "$agent" "$pay" "$policy_hash" "$per_tx" "$per_period" "$period" "$prior" >> "$MANIFEST"
      echo
      return 0
    fi
    echo "  create_policy $tx  NOT CONFIRMED" >&2; return 1
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$cat" "$agent" "$pay" "$policy_hash" "$per_tx" "$per_period" "$period" "$tx" >> "$MANIFEST"
  echo
}

# One per default skill category, with envelopes sized to what each does: a
# storage agent pays small and often, a blockchain agent moves more per call.
deploy_agent storage    50   500  1000  10  74X2qWYq9ibq9BZgNrYc9ar2VrjvZEGjknrx21ypmXMi
deploy_agent messaging  25   250  1000  10  AX5t22nfuWV5hWroReYjP885SmdJRuUuM7h8vD1msEHH
deploy_agent blockchain 200 1000  1000  30  CD7UznmriALT8khbr2vCe46vN1YQyeMrAqZHy7Sfq7ct

echo "manifest: $MANIFEST"
column -t -s$'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
