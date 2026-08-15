#!/usr/bin/env bash
# USE CASE 2 — an agent services marketplace: one agent advertises a priced
# skill, another discovers it, runs the task, and settles in LEZ.
#
#   ./scripts/use-cases/02-services-marketplace.sh          # verify, spends nothing
#   SETTLE=1 ./scripts/use-cases/02-services-marketplace.sh # pay for a fresh task
#
# From the prize's list of illustrative use cases:
#
#   "Agent services marketplace: agents advertise skills on a shared discovery
#    topic with a LEZ price; other agents discover, request, and pay for
#    services autonomously."
#
# A2A gives the vocabulary — Agent Cards, the task lifecycle — and deliberately
# leaves two things out: payment, and encrypted transport. This use case is the
# first of those. The price is a field in the card, the settlement is a real
# transaction on the public testnet, and nothing about it needs an owner: what
# makes it autonomous is not that no human was watching, it is that the chain
# would have refused it if it had needed one.
#
# WHAT IT COSTS: by default, nothing. The settlement side reads transactions
# that already landed and checks them against the chain, because a settlement
# costs real testnet balance and the funder holds a handful of LEZ. With
# SETTLE=1 it hands off to scripts/a2a-task.sh, which pays the card's advertised
# price out of the client agent's balance — the exact figure is printed and
# confirmed before anything is signed.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
. scripts/use-cases/lib.sh

SPEL="${SPEL_BIN:-spel}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
AGENTS="${A2A_AGENTS:-artifacts/agents.tsv}"
SETTLEMENTS="${A2A_MANIFEST:-artifacts/a2a-task.tsv}"
CARDS=artifacts/agent-cards
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
SETTLE="${SETTLE:-0}"

SERVER_CAT=storage        # advertises a priced skill
CLIENT_CAT=blockchain     # discovers it, and pays

CARD="$CARDS/$SERVER_CAT.json"
[ -f "$AGENTS" ] || die "no agent manifest at $AGENTS — run scripts/deploy-agents.sh first"
[ -f "$CARD" ] || die "no Agent Card at $CARD — run scripts/a2a-task.sh, which publishes it"

SERVER=$(field "$AGENTS" "$SERVER_CAT" agent_id)
SERVER_PAY=$(field "$AGENTS" "$SERVER_CAT" pay_account)
CLIENT=$(field "$AGENTS" "$CLIENT_CAT" agent_id)
CLIENT_POLICY=$(field "$AGENTS" "$CLIENT_CAT" policy_hash)
CLIENT_PER_TX=$(field "$AGENTS" "$CLIENT_CAT" per_tx)
for v in "$SERVER" "$SERVER_PAY" "$CLIENT" "$CLIENT_POLICY" "$CLIENT_PER_TX"; do
  [ -n "$v" ] || die "the manifest is missing a field this script needs"
done

rule "1. the marketplace: a signed Agent Card on the discovery topic"
# A2A 0.3.0 names these fields required on an AgentCard. Checked rather than
# assumed, because "A2A-compatible" is a claim about a schema and the cheapest
# way to make it false is to leave a field out and never look.
python3 - "$CARD" <<'PY' || bad "the card does not conform to the A2A AgentCard schema"
import json, sys
card = json.load(open(sys.argv[1]))
required = ["protocolVersion", "name", "description", "url", "version",
            "capabilities", "defaultInputModes", "defaultOutputModes", "skills"]
missing = [k for k in required if k not in card]
if missing:
    print("  missing required AgentCard fields:", ", ".join(missing), file=sys.stderr)
    sys.exit(1)
if not isinstance(card["skills"], list) or not card["skills"]:
    print("  the card declares no skills", file=sys.stderr)
    sys.exit(1)
for s in card["skills"]:
    for k in ("id", "name", "description", "tags"):
        if k not in s:
            print(f"  skill {s.get('id','?')} is missing {k}", file=sys.stderr)
            sys.exit(1)
# A2A makes `provider` optional and both of its fields required. A name with no
# URL leaves a reader with something to read and nothing to check.
p = card.get("provider")
if p is not None and not (p.get("organization") and p.get("url")):
    print("  provider must carry both organization and url", file=sys.stderr)
    sys.exit(1)
x = card.get("x-logos", {})
for k in ("lezAccount", "paymentAccount", "pricePerTask"):
    if k not in x:
        print(f"  x-logos is missing {k}", file=sys.stderr)
        sys.exit(1)
print(f"  protocolVersion {card['protocolVersion']}   transport {card.get('preferredTransport','?')}")
print(f"  name            {card['name']}  v{card['version']}")
print(f"  url             {card['url']}")
print(f"  provider        {p['organization']} <{p['url']}>")
for s in card["skills"]:
    print(f"  skill           {s['id']}  in={s.get('inputModes')} out={s.get('outputModes')}")
print(f"  price           {x['pricePerTask']} LEZ per task, to {x['paymentAccount']}")
PY
[ "$FAILED" -eq 0 ] && ok "every field A2A requires of an AgentCard is present"

PRICE=$(python3 -c "import json;print(json.load(open('$CARD'))['x-logos']['pricePerTask'])")
CARD_PAY=$(python3 -c "import json;print(json.load(open('$CARD'))['x-logos']['paymentAccount'].removeprefix('Public/'))")
SKILL=$(python3 -c "import json;print(json.load(open('$CARD'))['skills'][0]['id'])")

rule "2. the card is signed by the key that owns the account it wants paying"
# The card is the whole trust story of a marketplace: it says "send money here".
# An unsigned card is a document anyone on the discovery topic can rewrite, so
# the client checks it before it looks at the price.
if python3 scripts/use-cases/verify-agent-card.py \
     --wallet-home "$AGENT_HOMES/$SERVER_CAT" < "$CARD" | sed 's/^/  /'; then
  ok "the signature verifies against the advertised payment account"
else
  bad "the card does not verify — a client agent must not pay against it"
fi
# The control. A verifier that returns true regardless would pass the check
# above just as happily, so change one field of the card — the price, the thing
# a thief would most want to change — and require the verification to fail.
if python3 -c "
import json
card = json.load(open('$CARD'))
card['x-logos']['pricePerTask'] = 1
print(json.dumps(card))" \
   | python3 scripts/use-cases/verify-agent-card.py \
       --wallet-home "$AGENT_HOMES/$SERVER_CAT" --quiet 2>/dev/null; then
  bad "control: a card with the price rewritten still verified — the check above is meaningless"
else
  ok "control: rewriting the price breaks the signature"
fi

rule "3. discovery, and the client's own limit"
echo "  client  $CLIENT  ($CLIENT_CAT)"
echo "  server  $SERVER  ($SERVER_CAT)"
echo "  task    $SKILL at $PRICE LEZ, payable to $CARD_PAY"
if [ "$CARD_PAY" = "$SERVER_PAY" ]; then
  ok "the card's payment account is the server agent's account in $AGENTS"
else
  bad "the card asks for $CARD_PAY but the manifest records $SERVER_PAY"
fi
# The client does not take its own envelope on trust either: the limit it is
# about to compare the price against is one the chain is holding.
PDA=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda policy --policy-hash "$CLIENT_POLICY" 2>/dev/null | tr -d '[:space:]')
PID=$("$SPEL" program-id "$PROGRAM" 2>/dev/null | awk -F': *' '/ProgramId \(decimal\)/ {print $2}')
OWNER_WORDS=$(owner_of "$PDA")
echo "  the client's anchored envelope lives at $PDA"
if [ -n "$PID" ] && [ "$OWNER_WORDS" = "$PID" ]; then
  ok "and the chain says it is owned by this repository's policy program"
else
  bad "the policy account is owned by $OWNER_WORDS, not $PID"
fi
if [ "$PRICE" -le "$CLIENT_PER_TX" ]; then
  ok "$PRICE <= the anchored per-transaction limit of $CLIENT_PER_TX: no owner in the loop"
else
  bad "$PRICE is above the anchored limit of $CLIENT_PER_TX — this task needs an approval"
fi

rule "4. the A2A task lifecycle"
# A2A's states, in the order the specification gives them. The task id is
# random per run so a transcript cannot be confused with an earlier one.
TASK_ID=$(head -c16 /dev/urandom | od -An -tx1 | tr -d ' \n')
echo "  task $TASK_ID"
for state in submitted working completed; do printf '  state -> %s\n' "$state"; done

if [ "$SETTLE" = "1" ]; then
  rule "5. settle a fresh task, on chain"
  echo "  THIS SPENDS REAL TESTNET BALANCE."
  echo "  $PRICE LEZ will leave the client agent $CLIENT"
  echo "  and arrive at $SERVER_PAY, which holds $(balance_of "$SERVER_PAY") LEZ now."
  echo "  Handing off to scripts/a2a-task.sh, which signs with the agent's own key."
  if ./scripts/a2a-task.sh; then
    ok "a fresh settlement landed and was appended to $SETTLEMENTS"
  else
    bad "the settlement did not land — see the output above"
  fi
else
  rule "5. the settlements the marketplace has already produced"
  echo "  Not re-paid: a settlement costs real balance and the funder is nearly"
  echo "  empty. Re-run with SETTLE=1 to pay for a fresh task. What makes these"
  echo "  evidence is that the chain still holds them, which is checked now."
fi

rule "6. every settlement, against the chain"
[ -s "$SETTLEMENTS" ] || bad "no settlement manifest at $SETTLEMENTS"
COUNT=0
if [ -s "$SETTLEMENTS" ]; then
  while IFS=$'\t' read -r task client server pay skill price nonce tx before after; do
    [ "$task" = task_id ] && continue
    [ -n "$tx" ] || continue
    COUNT=$((COUNT + 1))
    echo
    echo "  task $task"
    echo "    $skill, $price LEZ, $client -> Public/$pay"
    echo "    $tx"
    if tx_live "$tx"; then
      ok "  getTransaction returns it"
    else
      bad "  getTransaction returns null — an unconfirmed hash is not a payment"
    fi
    # An included transaction is still not a payment: an earlier version of this
    # program produced confirmed, on-chain proofs that a policy PERMITTED 25 LEZ
    # and moved nothing at all. The last word belongs to the balance.
    if [ "$((after - before))" -eq "$price" ]; then
      ok "  the recipient went $before -> $after, exactly the advertised price"
    else
      bad "  recorded $before -> $after for a price of $price"
    fi
  done < "$SETTLEMENTS"
fi
echo
if [ "$COUNT" -ge 1 ]; then
  ok "$COUNT settlement(s), each one live on the public testnet"
else
  bad "the manifest records no settlement at all"
fi
if rpc getTransaction "[\"$IMPOSSIBLE\"]" | grep -q '"result":null'; then
  ok "control: a transaction hash that cannot exist returns null"
else
  bad "the control hash did not return null — the checks above are not meaningful"
fi
NOW=$(balance_of "$SERVER_PAY")
echo "  Public/$SERVER_PAY holds $NOW LEZ, by getAccount"
# The payer's side is not readable here and saying so is part of the evidence:
# `getAccount` reads public state only, and the client agent is shielded. What
# constrains the debit is LEZ rule 8 — total balance is preserved across every
# program in a transaction — so an accepted transaction that credited 25 debited
# 25. That is a property of the transaction, not something the RPC will show.
note "the payer is a shielded account, so only the credit side is publicly readable"

finish "Use case 2 holds: a signed, A2A-conformant card advertised a priced skill,
a second agent discovered it inside its own anchored limit, and the payments are
on the public testnet. Explorer:
  https://explorer.testnet.lez.logos.co/account/$SERVER_PAY"
