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
# Each agent gets its own shielded account and its own policy. There is exactly
# one policy account per agent — its address is PDA(program, ["agent-policy/v1",
# agent_id]) and nothing else — so anchoring is a once-per-agent act and the
# envelopes below differ because the agents do different work, not because
# different limits would give different addresses. They no longer would.
#
# ANCHORING IS TWO TRANSACTIONS, FROM TWO WALLETS
#
# One address per agent says WHERE a policy goes and nothing about who may put
# one there. The deployment before this one answered that with "anybody": the
# agent's account was never declared on `create_policy`, so anchoring over
# somebody else's agent needed only its public id — which is in the manifest
# this script writes and in the Agent Card it publishes. So each agent is now
# anchored in two steps, and the two keys never meet:
#
#   claim_agent    signed by the AGENT, from the agent's own wallet home. It
#                  writes the id of the one account allowed to anchor over it.
#   create_policy  signed by that OWNER, from the owner's wallet home. The
#                  program reads the claim and refuses any other signer (6020),
#                  or refuses outright if the agent never claimed (6019).
#
# `spel` only signs with keys the single wallet home it is pointed at holds, so
# one instruction demanding both signatures would demand both keys in one
# wallet. Two instructions is what keeps the agent's key on the agent's node.
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
# Anchoring is single-use by design, and now once per AGENT rather than once per
# (owner, agent, limits): create_policy is declared #[account(init)] on an
# address derived from the agent alone, and init refuses an account that already
# exists. That is the whole of the fix for the anchoring bypass — a second
# policy for an agent is not detected, it has nowhere to go. It also means a
# second run of this script is correctly refused, and without a record of what
# was already anchored that correct refusal looks identical to a failure and
# wipes the entry out of the manifest.
LEDGER="${LEDGER:-artifacts/anchored.tsv}"
[ -f "$LEDGER" ] || printf 'program\twhat\tagent_id\ttx\n' > "$LEDGER"
# Keyed by (program, instruction, agent), not by agent alone. Both accounts an
# agent has here are PDAs of the *program*, so redeploying moves them to
# addresses that have never been initialised; a ledger keyed on the agent alone
# would report those as already done and skip the very anchoring the new program
# needs. `what` is in the key because there are now two single-use steps per
# agent and a run can legitimately resume between them.
anchored_tx() { # program what agent -> tx, if it was already done
  awk -F'\t' -v p="$1" -v w="$2" -v a="$3" 'NR>1 && $1==p && $2==w && $3==a {print $4; exit}' "$LEDGER"
}
# Each agent is funded so it can pay for real; spend moves balance, not just proof.
FUND_AMOUNT="${FUND_AMOUNT:-40}"

mkdir -p artifacts
# The manifest is built in a temporary file and moved over the real one only
# once every agent has actually anchored. It used to be truncated here, before
# a single agent had been created, which made a failed run destructive: a
# reviewer who pasted the README's command with a placeholder signer watched
# three agents fail and was left with a manifest containing nothing but its
# header — the evidence for a deployment that is still perfectly good on chain.
# Nothing about a failed deployment justifies deleting the record of the one
# that succeeded, so the existing file is now touched only on success.
# Beside the manifest rather than in $TMPDIR, so the final move is a rename
# within one filesystem and cannot leave a half-written manifest behind.
MANIFEST_TMP="$(mktemp "$(dirname "$MANIFEST")/.agents.tsv.XXXXXX")"
trap 'rm -f "$MANIFEST_TMP"' EXIT
chmod 644 "$MANIFEST_TMP"
# Count of agents that did not anchor. A non-zero count leaves the manifest
# alone and exits non-zero, because a run that reports success while having
# deployed nothing is worse than one that fails loudly.
failures=0
# WHAT A READER CAN CHECK, AND HOW
#
# The column that made this manifest self-verifying used to be `policy_hash`: it
# committed to the limits, and a reader who disbelieved the numbers beside it
# could recompute the digest and compare. Replacing the hash-addressed policy
# with an account addressed by the agent alone took that away — `policy_account`
# commits to the agent and to nothing else, so the `per_tx` column became a
# claim this file makes about itself.
#
# `record_prefix` puts it back, and does it with a byte comparison rather than a
# derivation: it is the first 73 bytes of the policy account's data, hex —
# version, owner, per_tx, per_period, period_blocks — which is the whole of the
# immutable part of the record. Everything after byte 73 is the running total,
# which moves. So:
#
#   curl … getAccount <policy_account> | jq -r '.result.data' → first 73 bytes
#   must equal record_prefix, and there is nothing left to trust.
#
# `claim_account` is the other half. Its data is `02 || owner`, written by the
# AGENT, and it is what makes `owner` below a fact about consent rather than a
# note about who happened to sign: read it and you know which account this agent
# allowed to fix its envelope. `docs/security-model.md` §7 has both commands.
printf 'category\tagent_id\tpay_account\tclaim_account\tpolicy_account\tper_tx\tper_period\tperiod_blocks\trecord_prefix\tclaim_tx\tcreate_tx\towner\n' >> "$MANIFEST_TMP"

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

# An account id, base58 as the wallet prints it, as the 32 raw bytes the chain
# holds. `agent_id` is the PDA seed of the policy account, so these bytes are the
# address: get them wrong and `create_policy` anchors somewhere no `spend` will
# ever look. The owner needs no such conversion any more — `create_policy` takes
# no owner argument at all and records the account that signed.
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

# The immutable half of the policy record, as the chain will hold it: version,
# owner, per_tx, per_period, period_blocks, little-endian, exactly the layout
# `PolicyRecord::encode` writes (`crates/agent-policy-core/src/lib.rs`). Derived
# here and then compared against `getAccount` after the anchor lands, so the
# manifest carries a value that was checked rather than one that was asserted.
record_prefix() { # owner_hex per_tx per_period period_blocks
  python3 -c "
import sys
owner, per_tx, per_period, period = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
out = bytes([1]) + bytes.fromhex(owner)
out += per_tx.to_bytes(16,'little') + per_period.to_bytes(16,'little') + period.to_bytes(8,'little')
assert len(out) == 73, len(out)
print(out.hex())" "$1" "$2" "$3" "$4"
}

# The same 73 bytes as the chain actually holds them. A default account — which
# is what getAccount answers for an id nothing has ever written — has no data at
# all, so this prints nothing and the comparison fails, which is the behaviour we
# want: unknown must not read as agreeing.
chain_prefix() { # policy account id
  curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$1\"]}" \
  | python3 -c "
import json,sys
d=bytes(json.load(sys.stdin)['result']['data'])
print(d[:73].hex() if len(d)>=73 else '')"
}

echo "owner  $SIGNER"
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
  echo "  owner  $signer"
  echo "         create_policy takes no owner argument: this account signs, and"
  echo "         the program writes the signer's own id into the policy record"
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

  # The agent's id as the 32 bytes the chain holds. This is the PDA seed of both
  # of the agent's accounts, so it is an address rather than a label.
  local agent_hex; agent_hex=$(id_hex "$agent")
  local signer_hex; signer_hex=$(id_hex "$signer")

  # The two addresses, resolved by `spel` from the published IDL rather than
  # recomputed here. There is exactly one of each per agent, which is why
  # neither step can happen twice: both accounts are declared `#[account(init)]`
  # and `init` refuses an account that is already there.
  local claim_account policy_account
  claim_account=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
      pda claim --agent "$agent" 2>/dev/null | tail -n1)
  policy_account=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
      pda policy --agent-id "$agent_hex" 2>/dev/null | tail -n1)
  if [ -z "$claim_account" ] || [ -z "$policy_account" ]; then
    echo "  FAILED to resolve the agent's accounts" >&2; return 1
  fi
  echo "  claim  $claim_account  (the agent's own statement of who may anchor)"
  echo "  policy $policy_account  (per-tx $per_tx, per-period $per_period, window $period blocks)"
  echo "         one account per agent; the limits are its data, not its address"

  # ── step 1: the agent designates its owner ────────────────────────────
  #
  # Signed by the AGENT, from the agent's own home, which is the whole point:
  # a policy over this agent now requires this key, and nobody but the agent
  # holds it. The instruction takes no `agent_id` — the claim account's address
  # is derived from the account that signs — so there is nothing here for a
  # stranger to substitute.
  local claim_tx; claim_tx=$(anchored_tx "$DEPLOY_TX" claim_agent "$agent")
  if [ -n "$claim_tx" ] && confirmed "$claim_tx"; then
    echo "  claim_agent   $claim_tx  already claimed (init refused a second one)"
  else
    export LEE_WALLET_HOME_DIR="$AGENT_HOMES/$cat" NSSA_WALLET_HOME_DIR="$AGENT_HOMES/$cat"
    # A private account's on-chain state moves every time it signs, and the
    # proof is built against the wallet's local view. Prove against a stale one
    # and the proof still builds, spel still returns a hash, and the sequencer
    # simply never lands it — with nothing reported anywhere.
    "$WALLET" account sync-private </dev/null >/dev/null 2>&1
    local cout; cout=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
      -- claim_agent --agent "Private/$agent" --owner-id "$signer_hex" 2>&1)
    claim_tx=$(echo "$cout" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
    if [ -z "$claim_tx" ]; then
      echo "  NO TRANSACTION — claim_agent submitted nothing:" >&2
      echo "$cout" | tail -8 >&2
      return 1
    fi
    for _ in $(seq 1 24); do sleep 30; confirmed "$claim_tx" && break; done
    confirmed "$claim_tx" || { echo "  claim_agent $claim_tx  NOT CONFIRMED" >&2; return 1; }
    echo "  claim_agent   $claim_tx  landed — only $signer may anchor this agent"
    printf '%s\tclaim_agent\t%s\t%s\n' "$DEPLOY_TX" "$agent" "$claim_tx" >> "$LEDGER"
  fi

  # ── step 2: the designated owner anchors the envelope ─────────────────
  #
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
  # `--claim` is not passed and cannot be: the claim account is a PDA of
  # `agent_id`, so spel derives its address from the same argument that derives
  # the policy account's. Naming another agent therefore moves BOTH addresses
  # together, which is why `agent_id` is bound rather than merely present.
  local tx; tx=$(anchored_tx "$DEPLOY_TX" create_policy "$agent")
  if [ -n "$tx" ] && confirmed "$tx"; then
    echo "  create_policy $tx  already anchored (init refused a second one)"
  else
    local out; out=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
      -- create_policy --owner "Public/$signer" --agent-id "$agent_hex" \
      --per-tx "$per_tx" --per-period "$per_period" --period-blocks "$period" 2>&1)
    local rc=$?
    tx=$(echo "$out" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
    # spel already tells us when an anchor failed — it prints
    # "Transaction NOT confirmed" and exits 1. Grepping only for tx_hash threw
    # that line away and turned a reported failure into a silent one, which is
    # most of why this took so long to find.
    if [ $rc -ne 0 ] || echo "$out" | grep -q "NOT confirmed"; then
      echo "  spel reported the anchor failed:" >&2
      echo "$out" | grep -E "NOT confirmed|error|Error" | head -3 | sed 's/^/    /' >&2
    fi
    if [ -z "$tx" ]; then
      echo "  NO TRANSACTION — spel submitted nothing:" >&2
      echo "$out" | tail -8 >&2
      return 1
    fi
    # Blocks are exactly 60 seconds apart, so 150s gave a transaction two or
    # three chances to be included and then called it dead. Wait twelve blocks.
    for _ in $(seq 1 24); do sleep 30; confirmed "$tx" && break; done
    confirmed "$tx" || { echo "  create_policy $tx  NOT CONFIRMED" >&2; return 1; }
    echo "  create_policy $tx  landed"
    printf '%s\tcreate_policy\t%s\t%s\n' "$DEPLOY_TX" "$agent" "$tx" >> "$LEDGER"
  fi

  # ── and the manifest records what the chain says, not what we asked for ──
  #
  # The limits go into the manifest only after the bytes the chain holds have
  # been read back and matched against the record those limits imply. A run that
  # anchored something else — or anchored nothing, and read a default account —
  # fails here instead of publishing numbers nobody checked.
  local want got; want=$(record_prefix "$signer_hex" "$per_tx" "$per_period" "$period")
  got=$(chain_prefix "$policy_account")
  if [ "$want" != "$got" ]; then
    echo "  the policy account does not hold the record this anchor implies:" >&2
    echo "    expected $want" >&2
    echo "    on chain ${got:-<no data: the account has never been written>}" >&2
    return 1
  fi
  echo "  record $want"
  echo "         read back off chain and matched byte for byte"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$cat" "$agent" "$pay" "$claim_account" "$policy_account" \
    "$per_tx" "$per_period" "$period" "$want" "$claim_tx" "$tx" "$signer" >> "$MANIFEST_TMP"
  echo
}

# One per default skill category, with envelopes sized to what each does: a
# storage agent pays small and often, a blockchain agent moves more per call.
#
# Each anchor gets a signer of its own, each created by `wallet account new
# public` and never used for anything else. That is not tidiness: a signer still
# holding the DEFAULT program owner works exactly once, because on its second
# program transaction its pre-state is no longer `Account::default()`, the SPEL
# macro drops it from the output to dodge LEZ rule 7, and the state machine then
# rejects the transaction for an account being declared and missing. Make three
# more the same way if you are re-anchoring — the ids below are only useful to a
# wallet that holds their keys.
#
# THE FUNDING FIGURES ARE FLOORS, NOT TRANSFERS. `fund_agent` sends nothing when
# the agent already holds at least the amount named, and on a re-anchor it never
# should: a shielded transfer does not credit an existing note, it MINTS A NEW
# ONE with a new account id, and an agent whose id moves after it has claimed is
# an agent whose claim and policy accounts are both at addresses nothing will
# look at again. So fund first, claim second, anchor third, and on a re-anchor
# do not fund at all. The numbers below are what each agent must already hold
# for the demo path to work end to end: the messaging agent is the payer in
# `scripts/a2a-task.sh` and two settlements at its 25-per-transaction ceiling
# need 50 of them.
deploy_agent storage     50   500  1000   5  2dA9APZgzcoX65YhNMJmsDC2v838ufLSjPyUdMknWoZd || failures=$((failures + 1))
deploy_agent messaging   25   250  1000  55  H3VSrUkvPRqU1ruS2bpqrhETE9364hfeapXQReA9yaTZ || failures=$((failures + 1))
deploy_agent blockchain 200  1000  1000   5  G64pMjF9MR2vZjjwCyCFsC7DvG4uUPJC7quJiih9uvCc || failures=$((failures + 1))

# Nothing is written over the real manifest until every agent has anchored.
# A partial manifest is not a smaller success, it is a file that claims three
# agents are deployed and names fewer — so a failed run leaves the previous
# manifest exactly as it found it and says so.
if [ "$failures" -ne 0 ]; then
  echo >&2
  echo "$failures of 3 agents did not deploy; $MANIFEST left unchanged" >&2
  exit 1
fi

mv "$MANIFEST_TMP" "$MANIFEST"
trap - EXIT
echo "manifest: $MANIFEST"
column -t -s$'\t' "$MANIFEST" 2>/dev/null || cat "$MANIFEST"
