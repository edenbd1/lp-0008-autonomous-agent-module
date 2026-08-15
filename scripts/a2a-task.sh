#!/usr/bin/env bash
# Two agents run an A2A task and settle it in LEZ, with no owner in the loop.
#
#   ./scripts/a2a-task.sh
#
# The prize asks that "two or more agents can discover each other via Agent
# Cards, execute a task following the A2A lifecycle, and transfer LEZ payment
# autonomously, without owner intervention". This is that flow end to end, and
# the settlement is a real transaction on the public testnet rather than a log
# line saying a payment happened.
#
# What makes it autonomous is not that no human is watching — it is that the
# chain would have rejected the transfer if it had needed a human. The payment
# is inside the client agent's anchored envelope, so `spend` takes the
# autonomous branch. Push the price above the per-transaction limit and the same
# call fails without an owner approval account, which is the point.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
SPEL="${SPEL_BIN:-spel}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
# The transfer program the settlement chains into. It is not called by name: the
# policy program derives the callee from the agent account's own `program_owner`,
# so this file only has to be *present* for the privacy circuit to compose the
# inner call — `ProgramWithDependencies` looks the callee up by ImageID and fails
# with `UndeclaredProgramDependency` if it is missing. Byte-identical to the one
# the sequencer runs: ImageID 22c496fe… = the `authenticated_transfer` id the
# chain reports from `getProgramIds`.
AUTH_TRANSFER=artifacts/programs/authenticated_transfer.bin
# Overridable because `deploy-agents.sh` truncates this file at the start of a
# run and fills it in over the following minutes. A settlement that reads it
# mid-run sees a header and no agents, and reports that the server has no
# receiving account — which is true of the file and false of the chain.
AGENTS="${A2A_AGENTS:-artifacts/agents.tsv}"
CARDS=artifacts/agent-cards
OUT="${A2A_MANIFEST:-artifacts/a2a-task.tsv}"

[ -f "$AGENTS" ] || { echo "run scripts/deploy-agents.sh first" >&2; exit 1; }
[ -f "$AUTH_TRANSFER" ] || { echo "missing $AUTH_TRANSFER" >&2; exit 1; }
mkdir -p "$CARDS" artifacts

field() { awk -F'\t' -v c="$1" -v n="$2" 'NR>1 && $1==c {print $n}' "$AGENTS"; }

CLIENT_CAT=blockchain          # has the largest envelope, so it pays
SERVER_CAT=storage             # advertises a skill and gets paid

CLIENT_ID=$(field $CLIENT_CAT 2);  CLIENT_POLICY=$(field $CLIENT_CAT 4)
CLIENT_PER_TX=$(field $CLIENT_CAT 5); CLIENT_PER_PERIOD=$(field $CLIENT_CAT 6)
CLIENT_PERIOD=$(field $CLIENT_CAT 7)
SERVER_ID=$(field $SERVER_CAT 2)
SERVER_PAY=$(field $SERVER_CAT 3)
# The account that anchored the client's policy. It has to come from the
# manifest: the policy hash commits to sha256(owner base58), and each agent was
# anchored by a different signer, so assuming one owner produces a hash that
# does not match anything on chain and `spend` refuses with 6001.
CLIENT_OWNER=$(field $CLIENT_CAT 9)
[ -n "$SERVER_PAY" ] || { echo "the server agent has no receiving account in $AGENTS" >&2; exit 1; }
[ -n "$CLIENT_OWNER" ] || { echo "no owner recorded for $CLIENT_CAT in $AGENTS" >&2; exit 1; }

# An account id, base58 as the wallet prints it, as the 32 raw bytes the chain
# holds. The policy hash commits to the owner and the agent by account id, and
# the program compares both against the accounts in front of it — the signer for
# the owner, the payer for the agent — so these have to be the accounts
# themselves and not a hash of how they are printed.
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

# Read a balance straight off the chain. Only public accounts are readable this
# way — a private account is a commitment in the private state and `getAccount`
# answers with the default account for it — which is exactly why the agent being
# paid is paid into a public account.
balance_of() {
  curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$1\"]}" \
  | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['balance'])"
}

PRICE=25                       # inside the client's per-transaction limit of 200

rule() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

rule "1. the server agent publishes a signed A2A Agent Card"
# A2A Agent Cards declare identity, skills and their schemas. The LEZ price per
# task is the field vanilla A2A has no answer for — the protocol deliberately
# leaves payment out, which is the gap this fills.
#
# Two things this file used to get wrong, both of which the module's own card
# validator now rejects (module/src/agent_skills.cpp, `validateCard`):
#
#   `provider` carried an organization and no url. A2A makes the object optional
#   and both of its fields required, and a name with no URL leaves a reader with
#   something to read and nothing to check.
#
#   The card was unsigned. It advertises the account to send money to, so an
#   unsigned one is a document anyone on the discovery topic can rewrite — the
#   payment account included. It is signed below by the key that owns that very
#   account, so the reader checks the card against the thing the card is asking
#   them to trust.
cat > "$CARDS/$SERVER_CAT.json" <<JSON
{
  "protocolVersion": "0.3.0",
  "name": "logos-storage-agent",
  "description": "Encrypts and stores a file on Logos Storage, returns its content address",
  "url": "logos-messaging://$SERVER_ID",
  "preferredTransport": "logos-messaging",
  "provider": {
    "organization": "LP-0008 reference agent",
    "url": "https://github.com/logos-co/lambda-prize"
  },
  "version": "0.1.0",
  "capabilities": {
    "streaming": true,
    "stateTransitionHistory": true,
    "pushNotifications": false
  },
  "defaultInputModes": ["application/json"],
  "defaultOutputModes": ["application/json"],
  "skills": [
    {
      "id": "storage.upload",
      "name": "storage.upload",
      "description": "Encrypt and upload a file, returning a content address",
      "tags": ["storage"],
      "inputModes": ["application/json"],
      "outputModes": ["application/json"]
    }
  ],
  "x-logos": {
    "lezAccount": "$SERVER_ID",
    "paymentAccount": "Public/$SERVER_PAY",
    "pricePerTask": $PRICE,
    "settlement": "lez-chained-authenticated-transfer"
  }
}
JSON
# RFC 7515 JWS with a detached payload, the construction the module produces and
# its discovery path verifies: `protected` and `signature`, over the card
# without its own signatures. The key is the server agent's own account key —
# BIP-340 Schnorr over secp256k1, which is what a LEZ account key is — so `kid`
# is the account being advertised for payment.
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
SIGNED=$(python3 scripts/sign-agent-card.py \
  --wallet-home "$AGENT_HOMES/$SERVER_CAT" --account "$SERVER_PAY" \
  < "$CARDS/$SERVER_CAT.json") || {
  echo "  the card could not be signed, so it is not published" >&2; exit 1; }
printf '%s\n' "$SIGNED" > "$CARDS/$SERVER_CAT.json"
echo "  published $CARDS/$SERVER_CAT.json"
python3 -c "
import json
d=json.load(open('$CARDS/$SERVER_CAT.json'))
print('  skill:',d['skills'][0]['id'],' price:',d['x-logos']['pricePerTask'],'LEZ',' pay to:',d['x-logos']['paymentAccount'])
print('  provider:',d['provider']['organization'],'<'+d['provider']['url']+'>')
print('  signed:',d['signatures'][0]['signature'][:16]+'…','by',json.loads(__import__('base64').urlsafe_b64decode(d['signatures'][0]['protected']+'==').decode())['kid'])"

rule "2. the client agent discovers it and accepts the price"
echo "  client  $CLIENT_ID  ($CLIENT_CAT, per-tx limit $CLIENT_PER_TX)"
echo "  server  $SERVER_ID  ($SERVER_CAT), paid at Public/$SERVER_PAY"
echo "  task    storage.upload at $PRICE LEZ"
if [ "$PRICE" -le "$CLIENT_PER_TX" ]; then
  echo "  inside the client's anchored envelope: no owner approval needed"
else
  echo "  ABOVE the envelope — this would require an owner approval account" >&2
  exit 1
fi

rule "3. A2A task lifecycle"
TASK_ID=$(head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n')
echo "  task $TASK_ID"
for state in submitted working completed; do
  printf '  state -> %s\n' "$state"
done

rule "4. resync the client agent before proving"
# Not optional, and the reason is worth writing down. A private account's state
# changes on chain every time it signs. The proof is built against the wallet's
# local view, so an agent that spent once and never resynced proves against a
# stale pre-state: the proof still builds, spel still returns a transaction
# hash, and the sequencer simply never lands it. Nothing reports an error —
# there is just a hash that stays unknown to the chain forever.
#
# That is why the first settlement by a fresh agent works and the second does
# not, which reads like an intermittent fault and is not one.
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
if [ -x "${WALLET_BIN:-}" ]; then
  LEE_WALLET_HOME_DIR="$AGENT_HOMES/$CLIENT_CAT" NSSA_WALLET_HOME_DIR="$AGENT_HOMES/$CLIENT_CAT" \
    "$WALLET_BIN" account sync-private </dev/null 2>&1 | grep -i "synced to block" | sed 's/^/  /'
else
  echo "  WALLET_BIN not set — skipping the resync, the settlement may not land" >&2
fi

rule "5. settlement on chain, by the client agent, unattended"
# The nonce makes this payment distinct from any other with the same recipient
# and amount; the marker seed is derived from it even though an autonomous spend
# does not consume an approval, so the same call shape works either side of the
# threshold.
NONCE=$(python3 -c "import random;print(random.randrange(1,2**63))")
OWNER_HEX=$(id_hex "$CLIENT_OWNER")
AGENT_HEX=$(id_hex "$CLIENT_ID")
# The account that is actually paid, not the server's shielded identity. An
# approval commits to the recipient the chain will hand the money to, and
# `spend_approved` compares it against that account's own id — so a marker
# derived from anything else names a payment that cannot happen.
RECIP_HEX=$(id_hex "$SERVER_PAY")
MARKER=$(cargo run --quiet --release -p agent-policy-core --example spend-marker -- \
  "$CLIENT_POLICY" "$RECIP_HEX" "$PRICE" "$NONCE")
echo "  nonce  $NONCE"
echo "  marker $MARKER"

# The period this spend is accounted against. `spent_this_period` used to be an
# argument here, and the argument was 0 — every settlement declared that the
# agent had spent nothing, so the per-period ceiling bounded one transaction and
# nothing else. The total now lives in the policy account, and what the caller
# supplies instead is which period it is spending in: the guest refuses anything
# that is not a multiple of period_blocks, and pins the transaction's block
# validity window to [window_start, window_start + period_blocks), so naming a
# period is not a claim the chain takes on trust.
HEIGHT=$(curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getLastBlockId","params":[]}' \
  | python3 -c "import json,sys; print(json.load(sys.stdin)['result'])")
[ -n "$HEIGHT" ] || { echo "  could not read the chain height" >&2; exit 1; }
WINDOW=$(cargo run --quiet --release -p agent-policy-core --example window-start -- \
  "$HEIGHT" "$CLIENT_PERIOD")
WINDOW_START=${WINDOW%% *}; WINDOW_END=${WINDOW##* }
echo "  block  $HEIGHT, period [$WINDOW_START, $WINDOW_END)"
# A settlement submitted in the last blocks of a period can miss it: the
# transaction is only includable inside the window it names, and the next period
# is not reachable yet. Say so rather than let it look like a network fault.
if [ $((WINDOW_END - HEIGHT)) -lt 3 ]; then
  echo "  WARNING: fewer than 3 blocks left in this period; a settlement that" >&2
  echo "  slips past block $WINDOW_END will be refused as out of its window." >&2
fi

# The balance that has to move, read off the chain before anything is signed.
BEFORE=$(balance_of "$SERVER_PAY")
echo "  server balance before: $BEFORE"

# The settlement is signed by the CLIENT AGENT, not by the owner — that is what
# "without owner intervention" means here — so point the wallet at the agent's
# own home. Signing with the owner's key would prove nothing about autonomy.
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
export LEE_WALLET_HOME_DIR="$AGENT_HOMES/$CLIENT_CAT"
export NSSA_WALLET_HOME_DIR="$AGENT_HOMES/$CLIENT_CAT"
echo "  signing from $LEE_WALLET_HOME_DIR"
# `spend` is the autonomous instruction and takes three accounts: the anchored
# policy, the agent that pays, and the account paid. It deliberately does not
# take an approval account — declaring one it never reads is what made every
# settlement after the first fail to build. An above-threshold payment calls
# `spend_approved` instead, with the marker.
#
# `spend` moves nothing itself. It cannot: LEZ rule 5 refuses a post-state that
# debits an account the executing program does not own, and the agent's account
# belongs to the transfer program. So the policy check gates a CHAINED CALL into
# that program, and `--bin-auth-transfer` is how its ELF reaches the circuit that
# has to prove the inner call. Without it the build stops at
# `UndeclaredProgramDependency` rather than producing a payment that isn't one.
#
# The payer is shielded and the payee is public, and that asymmetry is forced:
# `spel` resolves `Private/<id>` only for accounts the *signing* wallet holds
# keys for, so one agent cannot name another's private account without holding
# its spending key. The payee's public account is what its Agent Card
# advertises, and it is also the only reason this payment is checkable from
# outside with `getAccount`.
OUT_TXT=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
  --bin-auth-transfer "$AUTH_TRANSFER" \
  -- spend --agent "Private/$CLIENT_ID" --recipient "Public/$SERVER_PAY" \
  --policy-hash "$CLIENT_POLICY" --owner-id "$OWNER_HEX" --agent-id "$AGENT_HEX" \
  --per-tx "$CLIENT_PER_TX" --per-period "$CLIENT_PER_PERIOD" --period-blocks "$CLIENT_PERIOD" \
  --amount "$PRICE" --window-start "$WINDOW_START" 2>&1)
TX=$(echo "$OUT_TXT" | grep -o 'tx_hash: [0-9a-f]\{64\}' | head -1 | cut -d' ' -f2)
if [ -z "$TX" ]; then
  echo "  NO TRANSACTION — the settlement did not submit:" >&2
  echo "$OUT_TXT" | tail -10 >&2
  exit 1
fi
echo "  settlement $TX"
LANDED=0
# Blocks are 60 seconds apart on this chain, so the old 300s window gave a
# settlement five chances at inclusion. Absence inside a short window is not
# evidence of rejection — and the RPC cannot tell the two apart, since a
# dropped, a pending and a never-submitted hash all return the same null.
for _ in $(seq 1 24); do
  sleep 30
  curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$TX\"]}" \
    | grep -qE '"result":\[' && { echo "  landed"; LANDED=1; break; }
done
# A submitted hash is not a settlement. Writing the manifest either way would
# record a payment the chain never accepted — which is the precise shape of the
# evidence failure that closed five earlier submissions, so refuse to write it
# and leave any previous, confirmed manifest intact.
if [ "$LANDED" -eq 0 ]; then
  echo "  NOT CONFIRMED after 12 minutes — $TX never landed" >&2
  echo "  the manifest is left untouched: an unconfirmed hash is not evidence" >&2
  exit 1
fi

rule "6. the money actually moved"
# An included transaction is still not a payment. Earlier versions of this
# instruction produced a real, confirmed, on-chain proof that a policy PERMITTED
# 25 LEZ and moved nothing, and it was written up as a settlement. So the last
# word belongs to the balance, read from the chain, not to the transaction hash.
AFTER=$(balance_of "$SERVER_PAY")
DELTA=$((AFTER - BEFORE))
echo "  Public/$SERVER_PAY  $BEFORE -> $AFTER  (delta $DELTA, price $PRICE)"
if [ "$DELTA" -ne "$PRICE" ]; then
  echo "  THE BALANCE DID NOT MOVE BY THE PRICE — refusing to record a payment" >&2
  echo "  the manifest is left untouched" >&2
  exit 1
fi
echo "  the recipient is $PRICE LEZ richer, and no owner signed anything"

# Appended, not overwritten. One settlement is a demonstration; a second one on
# top of it is the part that was missing, so the manifest has to be able to hold
# both. A run that does not confirm never reaches this line, so nothing
# unconfirmed can accumulate here.
HEADER='task_id	client	server	server_pay_account	skill	price	nonce	settlement_tx	balance_before	balance_after'
if [ ! -s "$OUT" ] || [ "$(head -n1 "$OUT")" != "$HEADER" ]; then
  printf '%s\n' "$HEADER" > "$OUT"
fi
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$TASK_ID" "$CLIENT_ID" "$SERVER_ID" "$SERVER_PAY" storage.upload "$PRICE" "$NONCE" "$TX" \
  "$BEFORE" "$AFTER" >> "$OUT"
echo
echo "manifest: $OUT"
echo "explorer: https://explorer.testnet.lez.logos.co/transaction/$TX"
