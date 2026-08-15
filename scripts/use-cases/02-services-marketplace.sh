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
# Without this, a missing spel is not an error: every derivation below returns
# the empty string, lib.sh swallows the failure, and the script goes on to
# report "the chain says per_tx , the manifest says 200" -- which reads like the
# chain disagreeing with us rather than like a tool that was never built. Script
# 03 has had this guard; 02 and 05 did not, and a reviewer following the docs on
# a fresh clone met five confident FAILs instead of one clear sentence.
command -v "$SPEL" >/dev/null 2>&1 || [ -x "$SPEL" ] \
  || { echo "error: no spel on PATH. Build it once:" >&2; \
       echo "  cargo build --release --locked -p spel --manifest-path vendor/spel/Cargo.toml" >&2; \
       echo "then re-run, or set SPEL_BIN=\$PWD/vendor/spel/target/release/spel" >&2; exit 1; }
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
CLIENT_POLICY=$(field "$AGENTS" "$CLIENT_CAT" policy_account)
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
#
# Where the verifying key comes from. A LEZ account id is not its public key, so
# a checker needs the 32 bytes from somewhere. A wallet that holds the account
# is the operator's path. A CI runner has no agent wallet and must not be given
# one, so the same public key is published beside the card and used instead —
# the same comparison against the same bytes, and a wrong key fails the
# signature, which is what checking a signature is for. If neither is available
# this fails rather than passing quietly: no key means no verification.
PUBKEY_FILE="$CARDS/$SERVER_CAT.pub"
CARD_KEY_KIND=
if [ -f "$AGENT_HOMES/$SERVER_CAT/storage.json" ]; then
  CARD_KEY_KIND=wallet
  note "verifying against the wallet that holds $SERVER_PAY"
elif [ -s "$PUBKEY_FILE" ]; then
  CARD_KEY_KIND=published
  note "no agent wallet here, so verifying against the published key $PUBKEY_FILE"
else
  bad "no key to verify the card against: no wallet home, and no $PUBKEY_FILE"
fi
verify_card() { # reads a card on stdin
  case "$CARD_KEY_KIND" in
    wallet)    python3 scripts/use-cases/verify-agent-card.py \
                 --wallet-home "$AGENT_HOMES/$SERVER_CAT" "$@" ;;
    published) python3 scripts/use-cases/verify-agent-card.py \
                 --public-key "$(cat "$PUBKEY_FILE")" "$@" ;;
    *)         return 1 ;;
  esac
}
if [ -n "$CARD_KEY_KIND" ]; then
  if verify_card < "$CARD" | sed 's/^/  /'; then
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
print(json.dumps(card))" | verify_card --quiet 2>/dev/null; then
    bad "control: a card with the price rewritten still verified — the check above is meaningless"
  else
    ok "control: rewriting the price breaks the signature"
  fi
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
# The client does not take its own envelope on trust either. The limit it is
# about to compare the price against is not in the manifest and not in the call
# it will make: it is 97 bytes of account data the chain is holding, at an
# address derived from the client agent's own id.
PDA=$(policy_account_of "$IDL" "$PROGRAM" "$CLIENT")
[ -n "$PDA" ] || bad "could not resolve the client's policy account"
if [ "$PDA" = "$CLIENT_POLICY" ]; then
  ok "the manifest's policy_account is the PDA of the client agent's own id"
else
  bad "the manifest records $CLIENT_POLICY, the agent's id derives $PDA"
fi
PID=$("$SPEL" program-id "$PROGRAM" 2>/dev/null | awk -F': *' '/ProgramId \(decimal\)/ {print $2}')
OWNER_WORDS=$(owner_of "$PDA")
echo "  the client's anchored envelope lives at $PDA"
if [ -n "$PID" ] && [ "$OWNER_WORDS" = "$PID" ]; then
  ok "and the chain says it is owned by this repository's policy program"
else
  bad "the policy account is owned by $OWNER_WORDS, not $PID"
fi
# And the ceiling itself, decoded rather than recited.
CHAIN_PER_TX=$(policy_record "$PDA" per_tx) || bad "could not read the anchored record"
if [ "$CHAIN_PER_TX" = "$CLIENT_PER_TX" ]; then
  ok "its per-transaction limit reads back as $CHAIN_PER_TX, the manifest's figure"
else
  bad "the chain says per_tx $CHAIN_PER_TX, the manifest says $CLIENT_PER_TX"
fi
if [ "$PRICE" -le "$CLIENT_PER_TX" ]; then
  ok "$PRICE <= the anchored per-transaction limit of $CLIENT_PER_TX: no owner in the loop"
else
  bad "$PRICE is above the anchored limit of $CLIENT_PER_TX — this task needs an approval"
fi

rule "4. the A2A task lifecycle, driven"
# "Driven", and that word is doing the work the word "printed" used to do here.
#
# Two versions of this section were both honest and both obsolete. First it was
#     for state in submitted working completed; do printf '  state -> %s\n' ...
# which claimed a lifecycle it did not run. Then it was the same loop relabelled
# "(printed, not a TaskStore transition)", which stopped claiming it — the right
# fix for a script that could not do better, and a standard docs/a2a-binding.md
# §7.1 set first: "this document will not describe a printed line as a
# transition".
#
# It can do better now. `a2a_lifecycle` builds module/tests/a2a_drive.cpp and
# runs the real `TaskStore`, so every line below is a transition the store
# accepted or REFUSED — including `input-required`, which the printed loop never
# had, and the refusal that matters most: a completed task cannot be reopened.
# The task is opened against the card discovered in step 1, so this runs against
# the agent this demo actually found.
#
# The driver needs a compiler and nlohmann/json, and nothing else — no node, no
# chain, no key. Where they are missing this reports that the lifecycle was not
# driven and prints no states at all, which is the same standard as before: what
# is not demonstrated is not described.
if a2a_lifecycle "$ROOT" "$CARD"; then
  ok "the lifecycle ran through TaskStore, ending terminal, with reopen refused"
else
  note "the lifecycle was not driven in this run — see the reason above."
  note "It is covered by module/tests/agent_skills_test.cpp, which CI runs."
  note "Nothing above claims a transition that did not happen."
fi

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

rule "6. every settlement, decoded out of the chain's own copy of it"
# What this section used to do, and why it was not evidence.
#
# It read `balance_before` and `balance_after` out of the manifest and asserted
# `after - before == price`, where `price` is a third column of the same file.
# Three fields of one local file agreeing with each other is a statement about
# the file, and it is a statement that cannot fail: a2a-task.sh writes the row
# only when the difference already equalled the price. Meanwhile the account it
# describes is live, and 50 LEZ has left it since — so the OK line was about to
# start printing a confident number that the chain contradicts.
#
# The honest difficulty is that `getAccount` answers with CURRENT state and this
# chain has no historical-state RPC at all: getAccountAtBlock, getBalance and
# getTransactionReceipt are all "Method not found", and the whole surface is five
# methods. A balance as it stood at block 8740 cannot be fetched. That is why
# those numbers were cached, and caching them is how the strongest sentence in
# this use case came to rest on a file. The current balance is no substitute
# either: funds have legitimately left the payee since, so "current balance
# equals the sum of the prices" is false for an honest reason.
#
# The way out is that a settlement carries the answer inside itself. A LEZ
# transaction commits to its POST-STATE — every account it touched, with the
# balance and the data that account holds once it has run — and `getTransaction`
# hands those bytes over. Two of the accounts in a settlement's post-state are
# the payee and the client's policy ledger, so the transaction states both the
# balance the payee ended on and the running total the program itself wrote.
#
# The bytes are content-addressed by the very hash the manifest names: a LEZ
# transaction hash is sha256 over the bytes with the leading kind byte dropped.
# settlement-facts.py checks that before it decodes anything, so what follows is
# this settlement or it is an error — never a plausible wrong number.
[ -s "$SETTLEMENTS" ] || bad "no settlement manifest at $SETTLEMENTS"
COUNT=0
LEDGER_ACCOUNT="${PDA:-$CLIENT_POLICY}"
PREV_BAL=; PREV_SPENT=; PREV_WINDOW=; PREV_LEDGER=; SUM=0
WORK6="${TMPDIR:-/tmp}/lp0008-usecase-02"; mkdir -p "$WORK6"
if [ -s "$SETTLEMENTS" ]; then
  # BY HEADER NAME. This loop used to be
  #   while IFS=$'\t' read -r task client server pay skill price nonce tx before after
  # which is exactly the positional read note 1 of lib.sh warns about — and the
  # file has since gained a `program` column AT POSITION 1, which would have
  # shifted every one of those ten variables by one place while the loop carried
  # on. It was then briefly keyed with `field`, which matches on the FIRST
  # column: correct while that column was `task_id`, and silently wrong the
  # moment `program` took the front. Columns are named here and nothing is keyed
  # on which one happens to be first.
  # Each column is named, and each ROW is read as `name=value` pairs rather than
  # destructured into positional variables.
  #
  # `paste <(column_of …) … | while IFS=$'\t' read -r a b c …` looks header-keyed
  # and is not safe: tab is an IFS *whitespace* character, so `read` collapses
  # consecutive tabs. One empty cell anywhere in a row and every field after it
  # shifts left, exactly as a positional read does — demonstrated with
  #     printf 'a\t\tc\n' | { IFS=$'\t' read -r x y z; }   ->  x=a  y=c  z=
  # An empty `settlement_tx` therefore did not skip its row; it slid the payee
  # into the price. `row_of` cannot do that: it emits one `header=value` line per
  # column straight from awk, so an empty cell stays an empty value under its own
  # name.
  for _c in task_id price settlement_tx server_pay_account client skill \
            balance_before balance_after; do
    column_of "$SETTLEMENTS" "$_c" >/dev/null || die "$SETTLEMENTS has no $_c column"
  done
  N_ROWS=$(rows_of "$SETTLEMENTS"); ROW_I=1
  while [ "$ROW_I" -le "$N_ROWS" ]; do
    R=$(row_of "$SETTLEMENTS" "$ROW_I") || die "could not read row $ROW_I of $SETTLEMENTS"
    ROW_I=$((ROW_I + 1))
    TASK=$(kv "$R" task_id);            PRICE_K=$(kv "$R" price)
    TX=$(kv "$R" settlement_tx);        PAY_K=$(kv "$R" server_pay_account)
    CLIENT_K=$(kv "$R" client);         SKILL_K=$(kv "$R" skill)
    REC_BEFORE=$(kv "$R" balance_before); REC_AFTER=$(kv "$R" balance_after)
    [ -n "$TX" ] || continue
    COUNT=$((COUNT + 1))
    echo
    echo "  task $TASK"
    echo "    $SKILL_K, $PRICE_K LEZ advertised, $CLIENT_K -> Public/$PAY_K"
    echo "    $TX"

    F=$(settlement_facts "$TX" "$PAY_K" "$LEDGER_ACCOUNT")
    if [ "$(kv "$F" found)" = "1" ]; then
      ok "  the chain holds it, in block $(kv "$F" block)"
    else
      bad "  getTransaction returns null — an unconfirmed hash is not a payment"
      continue
    fi
    if [ "$(kv "$F" hash_ok)" = "1" ]; then
      ok "  its $(kv "$F" bytes) bytes hash to exactly this settlement's hash"
    else
      bad "  the bytes the chain returned are not this transaction: $(kv "$F" error)"
      continue
    fi
    if [ "$(kv "$F" recipient_found)" != "1" ]; then
      bad "  the settlement's post-state never mentions Public/$PAY_K — it paid nobody"
      continue
    fi
    BAL=$(kv "$F" recipient_balance)
    # The ledger is FOUND in the settlement's own post-state, not named. Its
    # address is a PDA of the program, so the migration moved it and this
    # manifest now spans two ledgers — a caller that had to name the account
    # would have to know which program each settlement was made under before it
    # could read the settlement. Deltas are only taken within one ledger.
    LEDGER=$(kv "$F" ledger_account)
    SPENT=$(kv "$F" ledger_spent)
    WIN=$(kv "$F" ledger_window_start)
    echo "    the transaction commits to: payee on $BAL, ledger $LEDGER on ${SPENT:-<none>} for period ${WIN:-?}"

    # THE VERDICT, and both sides of it come from transaction bytes.
    #
    # Two independent figures have to move by the price: the payee's balance,
    # and the running total the policy program keeps for this agent. A
    # settlement that was included but moved nothing — which this program has
    # produced before, with a real proof, and which is the whole reason this
    # section exists — advances neither.
    if [ -n "$PREV_BAL" ] && [ -n "$SPENT" ] && [ -n "$PREV_SPENT" ] \
       && [ "$LEDGER" = "$PREV_LEDGER" ] && [ "$WIN" = "$PREV_WINDOW" ]; then
      D_BAL=$((BAL - PREV_BAL)); D_SPENT=$((SPENT - PREV_SPENT))
      if [ "$D_BAL" -eq "$PRICE_K" ] && [ "$D_SPENT" -eq "$PRICE_K" ]; then
        ok "  it moved $D_BAL: payee and ledger both advanced by the advertised price"
      else
        bad "  the chain says it moved $D_BAL and charged the ledger $D_SPENT, for an advertised price of $PRICE_K"
      fi
    elif [ -n "$SPENT" ]; then
      # The first settlement of a period has nothing before it to difference
      # against, and no RPC will give the state before it. What the chain does
      # say is the total, and a period's total opens at zero — so after the
      # first settlement of a period the total IS the amount that settlement
      # charged. Later settlements are differenced above and do not rely on it.
      if [ "$SPENT" -eq "$PRICE_K" ]; then
        ok "  it moved $SPENT: period $WIN opened at zero and its ledger reads $SPENT after this"
      else
        bad "  the ledger reads $SPENT after the first settlement of period $WIN, for an advertised price of $PRICE_K"
      fi
    else
      bad "  the post-state carries no policy ledger, so the amount cannot be read out of it"
    fi

    # The cached figures are a LOCAL RECORD — what a2a-task.sh saw with
    # getAccount at the time — and they are no longer what any verdict rests on.
    # They are still checked, because a recorded payment that the transaction
    # contradicts is a manifest making a claim about the chain that is false.
    if [ "$REC_AFTER" = "$BAL" ]; then
      ok "  local record $REC_BEFORE -> $REC_AFTER agrees with the transaction"
    else
      bad "  local record ends on $REC_AFTER; the transaction commits to $BAL"
    fi

    # The running sum restarts whenever the ledger does. A settlement made under
    # the superseded program charged a different account, and adding its price to
    # a total that account never saw would make the live comparison below false
    # for a reason that has nothing to do with anything moving.
    [ "$LEDGER" = "$PREV_LEDGER" ] || SUM=0
    PREV_BAL=$BAL; PREV_SPENT=$SPENT; PREV_WINDOW=$WIN; PREV_LEDGER=$LEDGER
    SUM=$((SUM + PRICE_K))
  done
fi
echo
if [ "$COUNT" -ge 1 ]; then
  ok "$COUNT settlement(s), each one decoded from the chain's own copy"
else
  bad "the manifest records no settlement at all"
fi
if rpc getTransaction "[\"$IMPOSSIBLE\"]" | grep -q '"result":null'; then
  ok "control: a transaction hash that cannot exist returns null"
else
  bad "the control hash did not return null — the checks above are not meaningful"
fi

# And the live ledger, which is the one figure a settlement writes that nothing
# else spends. The payee's balance is a wallet and moves for reasons that have
# nothing to do with this marketplace — it has been 45, 70, 95, 45 and 95 again
# in a day. The per-period total is written only by the policy program, only on
# a spend, and only upward, so it can be compared against the sum of the prices
# with getAccount, today.
#
# Compared against the ledger the last settlements ACTUALLY charged, read out of
# their post-state, rather than against the client agent's own. Those are not the
# same account and assuming they were is how this check would quietly stop
# testing anything: settlements in this manifest were made by two different
# agents under two different programs, which is four ledgers between them.
if [ -n "$PREV_LEDGER" ]; then
  LIVE_WINDOW=$(policy_record "$PREV_LEDGER" window_start) || LIVE_WINDOW=
  LIVE_SPENT=$(policy_record "$PREV_LEDGER" spent) || LIVE_SPENT=
  [ "$PREV_LEDGER" = "$LEDGER_ACCOUNT" ] \
    || note "the last settlements were paid by another agent, so this is its ledger"
  echo "  ledger $PREV_LEDGER"
  if [ -z "$LIVE_SPENT" ]; then
    bad "that policy ledger could not be read off the chain"
  elif [ "$LIVE_WINDOW" != "$PREV_WINDOW" ]; then
    note "period has rolled from $PREV_WINDOW to $LIVE_WINDOW, so the live total has"
    note "reset and is not comparable with settlements made in the old one"
  elif [ "$LIVE_SPENT" -eq "$SUM" ]; then
    ok "it still reads $LIVE_SPENT for period $LIVE_WINDOW: the sum of every price charged to it"
  else
    bad "it reads $LIVE_SPENT for period $LIVE_WINDOW; the prices charged to it sum to $SUM"
  fi
fi

NOW=$(balance_of "$SERVER_PAY")
echo "  Public/$SERVER_PAY holds $NOW LEZ right now, by getAccount"
# Said plainly rather than left to look like a discrepancy: this is current
# state, not a settlement figure. The payee is a live account that has been spent
# from since, which is precisely why the amounts above are decoded out of the
# settlements instead of differenced from a balance.
note "current state, not a settlement figure — this account has been spent from since"
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
