#!/usr/bin/env bash
# USE CASE 3 — the spending ceiling, accepted below it and refused above it.
#
#   ./scripts/use-cases/03-spending-threshold.sh
#   THRESHOLD_NO_AGENT_KEY=1 ./scripts/use-cases/03-spending-threshold.sh
#
# The second form is for a machine that does not hold the agent's shielded key —
# CI, for instance, which must not. It runs sections 1-5, which read the chain,
# and refuses to run section 6, which is the only one that needs the key, saying
# so in a line a caller can assert on. See the note above section 6.
#
# The prize asks for an agent that "acts autonomously below a threshold the
# owner configures, and above it sends the proposed transaction to the owner and
# waits for approval". Everything else in this repository rests on that
# sentence being true of the chain rather than of the agent's source code: the
# agent holds its own keys on a remote node, so whoever takes the process takes
# the spending, and an `if (amount > limit)` in the agent is worth exactly
# nothing against them.
#
# So the ceiling is not a number in the agent, and it is not a number in the
# call either. Every agent has exactly ONE policy account — its address is
# PDA(program, ["agent-policy/v1", agent_id]) and nothing else — and the whole
# policy, the owner and both limits and the period and the running total, is
# that account's data, written by the program that owns it. `spend` carries an
# amount and a period and nothing more; the ceiling it is measured against comes
# off the account. This script shows that, from both sides, with the chain
# answering.
#
# The previous design put the limits in the ADDRESS, a hash of (owner, agent,
# limits). It looked stronger and was weaker: every triple had an account of its
# own, all uninitialised, so an attacker holding the agent's key could anchor a
# fresh unlimited policy and spend under it — executed, accepted, and recorded in
# artifacts/adversarial.tsv. Section 4 is what replaced it.
#
# WHAT IT COSTS: nothing. It submits no transaction. The TWO refusals below fail
# while the proof is being built, so no transaction is ever produced to submit.
# The first asks for 201 LEZ against a ceiling of 200, from an agent that holds
# far less than either, so a ceiling that failed completely still could not move
# money; the second asks for 1 in a window that does not start on a period
# boundary, which no block will include. (This header said "three refusals" and
# "each of them asks for 201" for several commits after the third attempt was
# deleted — section 4 replaced it, because `spend` now carries no limits to
# disagree with. The body's own comment above the attempts says so.)
# The accepted side is not re-paid either: it reads settlements that already landed.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }
. scripts/use-cases/lib.sh

SPEL="${SPEL_BIN:-spel}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
AUTH_TRANSFER=artifacts/programs/authenticated_transfer.bin
AGENTS="${A2A_AGENTS:-artifacts/agents.tsv}"
SETTLEMENTS="${A2A_MANIFEST:-artifacts/a2a-task.tsv}"
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
WORK="${TMPDIR:-/tmp}/lp0008-usecase-03"

command -v "$SPEL" >/dev/null 2>&1 || [ -x "$SPEL" ] \
  || die "no spel on PATH. Set SPEL_BIN, e.g. SPEL_BIN=\$PWD/vendor/spel/target/release/spel"
[ -f "$AGENTS" ] || die "no agent manifest at $AGENTS — run scripts/deploy-agents.sh first"
[ -f "$AUTH_TRANSFER" ] || die "missing $AUTH_TRANSFER"

# The agent that pays. It is the one with landed settlements below its ceiling,
# so both sides of the threshold are shown for the SAME agent, the same policy
# and the same recipient — which is the only way the contrast means anything.
CAT=blockchain
AGENT=$(field "$AGENTS" "$CAT" agent_id)     || die "manifest unreadable"
POLICY=$(field "$AGENTS" "$CAT" policy_account) || die "manifest unreadable"
PER_TX=$(field "$AGENTS" "$CAT" per_tx)
PER_PERIOD=$(field "$AGENTS" "$CAT" per_period)
PERIOD=$(field "$AGENTS" "$CAT" period_blocks)
OWNER=$(field "$AGENTS" "$CAT" owner)
RECIPIENT=$(field "$AGENTS" storage pay_account)
for v in "$AGENT" "$POLICY" "$PER_TX" "$PER_PERIOD" "$PERIOD" "$OWNER" "$RECIPIENT"; do
  [ -n "$v" ] || die "the manifest is missing a field this script needs"
done

echo "agent      $AGENT  ($CAT)"
echo "owner      $OWNER"
echo "envelope   $PER_TX per transaction, $PER_PERIOD per $PERIOD blocks"
echo "recipient  $RECIPIENT"

rule "1. the envelope is account data, and this script decodes it"
# Not read from the manifest and believed. The 97 bytes are fetched from the
# chain and taken apart here — version, owner, both limits, the period, and the
# running total — and every one of them is compared against what
# artifacts/agents.tsv claims. A manifest is a file in a repository; this is what
# the program will enforce.
OWNER_HEX=$(id_hex "$OWNER")
AGENT_HEX=$(id_hex "$AGENT")
echo "  owner  $OWNER"
echo "         = $OWNER_HEX"
echo "  agent  $AGENT"
echo "         = $AGENT_HEX"
echo "  policy $POLICY"
REC_OK=1
for pair in "owner:$OWNER_HEX" "per_tx:$PER_TX" "per_period:$PER_PERIOD" "period_blocks:$PERIOD"; do
  f=${pair%%:*}; want=${pair#*:}
  got=$(policy_record "$POLICY" "$f") || { bad "could not read $f off the policy account"; REC_OK=0; continue; }
  printf '  %-14s %s\n' "$f" "$got"
  if [ "$got" = "$want" ]; then ok "  matches $AGENTS"
  else bad "  the chain says $got, the manifest says $want"; REC_OK=0; fi
done
SPENT=$(policy_record "$POLICY" spent)
WINDOW_REC=$(policy_record "$POLICY" window_start)
echo "  spent          $SPENT in period $WINDOW_REC"
[ "$REC_OK" -eq 1 ] && ok "the anchored envelope is what this repository says it is"
# The owner is the one field nobody can have supplied: create_policy takes no
# owner argument at all, so those 32 bytes are the account that actually signed
# the anchor. There is no derivation here to get wrong, which is the point —
# the previous design's owner was an argument, and that is how it was bypassed.

rule "2. the program that owns the ceiling is the program in this repository"
# The chain reports an account's owner as eight u32 words — the ImageID of the
# program. `spel program-id` derives exactly those words from an ELF, so the
# committed binary can be compared to what the chain says, with no trust in
# between.
PID=$("$SPEL" program-id "$PROGRAM" 2>/dev/null | awk -F': *' '/ProgramId \(decimal\)/ {print $2}')
[ -n "$PID" ] || bad "could not read the ProgramId out of $PROGRAM"
echo "  ProgramId($PROGRAM)"
echo "         = $PID"
DEPLOY_TX=$(python3 -c "
import hashlib, struct
b = open('$PROGRAM','rb').read()
print(hashlib.sha256(struct.pack('<I', len(b)) + b).hexdigest())")
echo "  deploy tx = sha256(len || bytecode) = $DEPLOY_TX"
if tx_live "$DEPLOY_TX"; then ok "the chain holds that deploy transaction"
else bad "the chain does not hold $DEPLOY_TX — this program is not deployed"; fi
# Without this the check above proves nothing: an RPC answering non-null to
# everything would pass it just as happily.
if rpc getTransaction "[\"$IMPOSSIBLE\"]" | grep -q '"result":null'; then
  ok "control: a hash that cannot exist returns null"
else
  bad "the control hash did not return null — the check above is not meaningful"
fi

rule "3. the anchored envelope exists on chain, at the agent's own address"
PDA=$(policy_account_of "$IDL" "$PROGRAM" "$AGENT")
[ -n "$PDA" ] || bad "could not derive the policy PDA"
echo "  PDA(program, [\"agent-policy/v1\", $AGENT])"
echo "         $PDA"
if [ "$PDA" = "$POLICY" ]; then
  ok "the same account the manifest records — derived, not copied"
else
  bad "the agent's id derives $PDA, the manifest records $POLICY"
fi
GOT=$(owner_of "$PDA")
echo "  getAccount(...).program_owner = $GOT"
if [ "$GOT" = "$PID" ]; then
  ok "owned by exactly the program above — the owner anchored this envelope"
else
  bad "expected $PID, got $GOT"
fi

rule "4. there is only one such address, so a bigger ceiling has nowhere to go"
# This is the whole mechanism, and it is the opposite of what it used to be.
#
# The address does not depend on the limits, so "raise a limit" cannot name a
# different account — it names THIS one, and this one's data says what the owner
# anchored. `spend` does not even carry limits any more, so there is nothing to
# present. And `create_policy` declares this account #[account(init)], which
# refuses an account that already exists: a second policy for this agent is not
# rejected on inspection, it has nowhere to be written.
# There is nothing to vary. `spel pda policy` takes one argument, --agent-id;
# the IDL declares the seeds as [literal("agent-policy/v1"), arg("agent_id")]
# and there is no limit among them. That is the demonstration: not that ten
# derivations agree, but that only one derivation exists.
echo "  the IDL's seeds for the policy account:"
python3 -c "
import json, sys
idl = json.load(open('$IDL'))
ix  = next(i for i in idl['instructions'] if i['name'] == 'create_policy')
acc = next(a for a in ix['accounts'] if a['name'] == 'policy')
for s in acc['pda']['seeds']:
    print('         %-8s %s' % (s['kind'], s.get('value') or s.get('path')))
names = [s.get('value') or s.get('path') for s in acc['pda']['seeds']]
sys.exit(0 if names == ['agent-policy/v1', 'agent_id'] else 1)" \
  && ok "  one agent, one policy account — no limit is a seed of the address" \
  || bad "  the policy account's seeds are not (prefix, agent_id)"

# An agent nobody has ever anchored. Its address exists as arithmetic and the
# account behind it does not, which is what an uninitialised PDA looks like and
# is exactly the state `init` requires before it will write.
GHOST=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda policy --agent-id "$IMPOSSIBLE" 2>/dev/null | tr -d '[:space:]')
GHOST_OWNER=$(owner_of "$GHOST")
printf '  %-28s %s\n' "an agent nobody anchored" "$GHOST"
# Through `owner_state` (lib.sh) so that a chain which did not answer is named as
# that. This comparison already fell on the failing side when the read came back
# empty — it is written `=` rather than `!=` — but it then reported the account as
# "owned by " with nothing after it, which reads as a chain contradicting the
# repository rather than as a chain that said nothing. Section 3 above requires
# the opposite verdict on a real policy account, so the two together show the
# reader saying both words.
case "$(owner_state "$GHOST_OWNER")" in
  unclaimed) ok "  program_owner is all zeros: never initialised, so init would accept it" ;;
  claimed)   bad "  the control policy account is owned by $GHOST_OWNER" ;;
  *)         bad "  the chain did not answer for $GHOST, so nothing here says it is uninitialised" ;;
esac

# And the refusal itself, as it was submitted to this program. A refused
# transaction is not an event on this chain — it answers null exactly as the
# control above does — so these hashes are recorded as facts, not offered as
# proof. The proof is crates/agent-verifier-adversarial, which runs the
# committed binary against both steps of the attack.
# THIS SECTION'S EVIDENCE HAS TO BE COUNTED, and it was not. Three separate ways
# it could stop testing and still go green, all of them reachable:
#
#   * The manifest missing, or holding nothing but its header. The loop ran zero
#     times, printed nothing at all, and the script finished "Use case 3 holds".
#     Demonstrated by truncating artifacts/adversarial.tsv to its header line:
#     section 4 emitted no note, no failure and no output, exit code 0.
#   * A row whose `outcome` matched neither `accepted*` nor `submitted*` fell
#     through the `case` silently, so a typo in one cell deleted that row's
#     evidence without deleting the row.
#   * The only `ok` this section can print asserts an ABSENCE — that a submitted
#     attack is NOT on chain — and `tx_live` is false when the chain does not
#     answer. Pointed at a closed port it printed
#         "OK  60de3fc6…: submitted, never included"
#     exactly as it does against the live chain. An absence proves nothing until
#     the same question is shown coming back the other way.
#
# So: the rows that carry each kind of evidence are counted, and each count has
# a floor. REFUSED is the claim of this section; ON_CHAIN is its control, because
# those rows are checked with the same `tx_live` against the same RPC and must
# come back true. A run where `tx_live` cannot say "yes" has not shown that its
# "no" means anything.
ADV_REFUSED=0     # attacks on the LIVE program, required to be absent from the chain
ADV_ON_CHAIN=0    # attacks on a superseded program, required to be present
if [ -s artifacts/adversarial.tsv ]; then
  LIVE_PROG=$DEPLOY_TX
  # By header name. This was eight positional variables against a file whose
  # first column is `step` and repeats, so `field` had nothing to key on — see
  # `row_of` in lib.sh. What each row SAYS about how much moved stays a recorded
  # description and is labelled as one: the checkable part is whether the chain
  # still holds the transaction, and that is what is checked.
  N_ADV=$(rows_of artifacts/adversarial.tsv)
  ADV_I=1
  while [ "$ADV_I" -le "$N_ADV" ]; do
    R=$(row_of artifacts/adversarial.tsv "$ADV_I")
    ADV_I=$((ADV_I + 1))
    what=$(kv "$R" what); prog=$(kv "$R" program)
    tx=$(kv "$R" tx); outcome=$(kv "$R" outcome)
    # A recorded attack with no transaction is a row that cannot be checked, and
    # skipping it quietly is how a manifest loses evidence without losing rows.
    [ -n "$tx" ] || { bad "  row $((ADV_I - 1)) of artifacts/adversarial.tsv records no transaction to check"; continue; }
    case "$outcome" in
      accepted*) if [ "$prog" = "$LIVE_PROG" ]; then
                   bad "  $tx is recorded as accepted by the LIVE program"
                 elif tx_live "$tx"; then
                   note "recorded, of the superseded program: $what"
                   printf '         %s  in block %s\n' "$tx" \
                     "$(kv "$(settlement_facts "$tx")" block)"
                   ADV_ON_CHAIN=$((ADV_ON_CHAIN + 1))
                 else
                   bad "  a recorded accepted attack, $tx, is not on chain"
                 fi ;;
      submitted*) if [ "$prog" != "$LIVE_PROG" ]; then continue; fi
                  printf '  %s\n' "$what"
                  if tx_live "$tx"; then
                    bad "  $tx IS on chain — the second anchor was not refused"
                  else
                    ok "  $tx: submitted, never included"
                    ADV_REFUSED=$((ADV_REFUSED + 1))
                  fi ;;
      # Neither word, so this row states no outcome this script knows how to
      # check. It used to fall out of the `case` without a sound.
      *) bad "  row $((ADV_I - 1)) records outcome \"${outcome:-<empty>}\", which is neither accepted nor submitted — its evidence was not checked" ;;
    esac
  done
else
  bad "no artifacts/adversarial.tsv: the refusal this section is about is unevidenced"
fi
if [ "$ADV_REFUSED" -ge 1 ]; then
  ok "  $ADV_REFUSED recorded attack(s) on the live program, none of them on chain"
else
  bad "  no recorded attack on the live program was checked, so nothing here shows one was refused"
fi
# The control, and it is the same reader answering the other way. Without a row
# that `tx_live` says YES to, "$ADV_REFUSED attacks are not on chain" is equally
# true of a chain that answers nothing at all — which is how it reads against a
# closed port.
if [ "$ADV_ON_CHAIN" -ge 1 ]; then
  ok "  control: the same getTransaction finds $ADV_ON_CHAIN recorded attack(s) that WERE accepted, so its silence above is a reading"
else
  bad "  control: no recorded attack could be found on chain, so 'not on chain' above is indistinguishable from a chain that is not answering"
fi

rule "5. below the ceiling: accepted, unattended, and already on chain"
# Not re-paid. A settlement costs real testnet balance and the funder holds a
# handful of LEZ; what makes these evidence is that the chain still holds them,
# which is checkable now and by anyone. `./scripts/a2a-task.sh` is what produced
# them and will produce another.
if [ ! -s "$SETTLEMENTS" ]; then
  bad "no settlement manifest at $SETTLEMENTS"
else
  # By header name, never by position. This loop used to destructure the row
  # with `read -r task client server pay skill price nonce tx …`, which is the
  # exact pattern `lib.sh` exists to replace: when `a2a-task.tsv` gained a
  # leading `program` column, every variable shifted by one and the script
  # reported a price of "skill" as being over the ceiling and a transaction hash
  # of "70" as not on chain. Two confident wrong answers from a file that was
  # perfectly correct.
  #
  # And the AMOUNT each settlement moved is decoded out of the settlement rather
  # than differenced from two columns of this file. `getAccount` answers with
  # current state and this chain has no historical-state RPC, so the balance as
  # it stood at the settlement's block cannot be fetched — but the transaction
  # commits to its own post-state, and `getTransaction` returns it. A ceiling is
  # only demonstrated by a payment that actually happened, and `balance_before`
  # minus `balance_after` compared against `price` is three fields of one file
  # agreeing with each other. See scripts/use-cases/settlement-facts.py.
  # Read as `name=value` per row, not destructured positionally. `paste` piped
  # into `read -r a b c` with IFS=$'\t' looks header-keyed but collapses
  # consecutive tabs — tab is IFS whitespace — so one empty cell shifts every
  # later field left, which is the same defect the header names were meant to
  # close. See the note in 02-services-marketplace.sh section 6.
  n=0; landed=0; prev_spent=; prev_window=; prev_ledger=; total=0
  mkdir -p "$WORK"
  for _c in price settlement_tx server_pay_account; do
    column_of "$SETTLEMENTS" "$_c" >/dev/null || die "$SETTLEMENTS has no $_c column"
  done
  # The ledger the shielded spends charge, if any. See lib.sh.
  SH_LEDGER=$(shielded_payer_ledger "$IDL" "$PROGRAM")
  n_rows=$(rows_of "$SETTLEMENTS"); row_i=1
  while [ "$row_i" -le "$n_rows" ]; do
    r=$(row_of "$SETTLEMENTS" "$row_i") || die "could not read row $row_i"
    row_i=$((row_i + 1))
    price=$(kv "$r" price); tx=$(kv "$r" settlement_tx); pay=$(kv "$r" server_pay_account)
    # A settled price with no transaction is a ceiling claim with nothing behind
    # it. This used to `continue` silently, so a row could leave the manifest's
    # evidence without leaving the manifest.
    if [ -z "$tx" ]; then
      bad "  row $((row_i - 1)) of $SETTLEMENTS records a price of $price and no settlement_tx"
      continue
    fi
    n=$((n + 1))
    printf '  %s LEZ  %s\n' "$price" "$tx"
    if [ "$price" -le "$PER_TX" ]; then
      note "$price <= $PER_TX, so spend takes the autonomous branch"
    else
      bad "  a settled price of $price is ABOVE the anchored ceiling of $PER_TX"
    fi
    f=$(settlement_facts "$tx" "$pay" "$POLICY")
    if [ "$(kv "$f" found)" = "1" ] && [ "$(kv "$f" hash_ok)" = "1" ]; then
      ok "  the chain holds it, in block $(kv "$f" block), and its bytes hash to it"
      landed=$((landed + 1))
    else
      bad "  getTransaction returns null — this is not evidence of a payment"
      continue
    fi
    # The ledger this settlement charged, discovered in its own post-state
    # rather than named: the ledger account is a PDA of the program, so the
    # settlements in this manifest span two of them, and only totals from the
    # same account are comparable.
    ledger=$(kv "$f" ledger_account)
    spent=$(kv "$f" ledger_spent); window=$(kv "$f" ledger_window_start)
    # What the ceiling is actually about: the running total this program keeps.
    # Every settlement has to charge it by exactly the price, or the ceiling is
    # bounding a number that does not track the spending.
    if [ -n "$prev_spent" ] && [ "$ledger" = "$prev_ledger" ] && [ "$window" = "$prev_window" ]; then
      d=$((spent - prev_spent))
      if [ "$d" -eq "$price" ]; then
        ok "  it charged the anchored ledger $d, exactly the price"
      else
        bad "  it charged the ledger $d for a price of $price"
      fi
    elif [ -n "$spent" ] && sb=$(shielded_before "$DEPLOY_TX" "$(kv "$f" block)" "$ledger" "$SH_LEDGER" "$PERIOD") \
         && [ "$spent" -eq "$((price + sb))" ]; then
      if [ "${sb:-0}" -gt 0 ]; then
        ok "  its ledger reads $spent after this: $price here, $sb charged by shielded spend before it"
      else
        ok "  period $window opened at zero and its ledger reads $spent after this"
      fi
    else
      bad "  the ledger reads ${spent:-<none>} after the first settlement of period ${window:-?}, for a price of $price plus $(shielded_before "$DEPLOY_TX" "$(kv "$f" block)" "$ledger" "$SH_LEDGER" "$PERIOD") charged before it"
    fi
    # Reset on a new PERIOD as well as a new ledger, for the same reason: the
    # live ledger counts one window, so a previous window's prices must not
    # be carried into the comparison.
    [ "$ledger" = "$prev_ledger" ] && [ "$window" = "$prev_window" ] || total=0
    prev_spent=$spent; prev_window=$window; prev_ledger=$ledger
    total=$((total + price))
  done
  [ "$landed" -gt 0 ] || bad "not one settlement in the manifest is on chain"
  # The ceiling is per period, so the figure it bounds is the running total —
  # read live, off the ledger the last settlements ACTUALLY charged rather than
  # off this agent's own. They are not always the same account: a ledger address
  # is a PDA of (program, agent), so the settlements in this manifest span two
  # programs and two paying agents between them.
  # The ledger counts every spend the program made through this policy, and the
  # A2A manifest is not the only place they are recorded: a shielded payment is
  # the same `spend` instruction and charges the same account, but it lands in
  # artifacts/shielded-settlement.tsv because a shielded payee has no public
  # balance to compare against. Summing one manifest against a total that counts
  # both reported a missing settlement that does not exist. Add the shielded
  # spends charged to this same ledger by the same payer.
  LIVE_SPENT=$(policy_record "$prev_ledger" spent)
  LIVE_WINDOW=$(policy_record "$prev_ledger" window_start)
  # After LIVE_WINDOW is known, because the window is what bounds the sum: the
  # ledger's running total is per period, so only shielded spends in the window
  # it is currently reporting belong in the comparison.
  SHIELDED=artifacts/shielded-settlement.tsv
  # AND THE REFUSAL IS CHECKED, which the sibling call at the top of the loop
  # already does with `&&` and this one did not. `shielded_before` prints 0 and
  # returns 1 when it cannot bound the sum to one period; taking the 0 and
  # dropping the 1 makes `total` too small, and too small lands in the branch
  # below that treats a high ledger as the chain having simply moved on — so a
  # shielded spend that could not be read came out as an OK.
  EXTRA_OK=1
  if [ -f "$SHIELDED" ] && [ -n "$LIVE_WINDOW" ]; then
    extra=$(shielded_before "$DEPLOY_TX" "$((LIVE_WINDOW + PERIOD))" "$prev_ledger" "$SH_LEDGER" "$PERIOD") || EXTRA_OK=0
    if [ "$EXTRA_OK" -eq 0 ]; then
      bad "the shielded spends charged to $prev_ledger in this period could not be read, so $total is not the figure to compare the ledger against"
    elif [ "${extra:-0}" -gt 0 ]; then
      note "plus $extra LEZ of shielded spend charged to the same ledger ($SHIELDED)"
      total=$((total + extra))
    fi
  fi
  [ "$prev_ledger" = "$POLICY" ] \
    || note "those were paid by another agent, so this is its ledger, not $CAT's"
  echo "  ledger $prev_ledger"
  if [ -z "$LIVE_SPENT" ]; then
    bad "that policy ledger could not be read off the chain"
  elif [ "$LIVE_WINDOW" != "$prev_window" ]; then
    note "period has rolled to $LIVE_WINDOW, so the live total has reset"
  elif [ "$LIVE_SPENT" -lt "$total" ]; then
    # LOW is the contradiction: the running total is monotonic within a period,
    # so it cannot hold less than the settlements recorded above charged it.
    bad "the live ledger reads $LIVE_SPENT for period $LIVE_WINDOW, LESS than the $total the settlements above charged it — the ledger cannot lose a spend within a period, so the manifest and the chain contradict each other"
  elif [ "$LIVE_SPENT" -gt "$total" ]; then
    # HIGH is not, and the asymmetry is the point. This is a live account on a
    # public testnet: anything that charges it moves this number, including a
    # settlement made after this checkout was taken. An equality here would make
    # a correct manifest red for a spend that had simply happened since — the
    # same shape as summing across a period boundary, which this file has
    # already been red for. Reported, and it says which side moved.
    ok "the live ledger reads $LIVE_SPENT of $PER_PERIOD for period $LIVE_WINDOW, which accounts for every one of the $total LEZ charged above"
    note "the extra $((LIVE_SPENT - total)) LEZ is a spend charged to this same ledger"
    note "in this same period that no manifest here records. The chain moved, this"
    note "repository did not. The ledger only goes up within a period, so this cannot"
    note "hide a settlement that failed to charge it: that would read LOW."
  else
    ok "the live ledger reads $LIVE_SPENT of $PER_PERIOD for period $LIVE_WINDOW — the sum of the prices charged to it"
  fi
  NOW=$(balance_of "$RECIPIENT")
  echo "  $RECIPIENT holds $NOW LEZ now, by getAccount"
  note "current state, not a settlement figure — this account has been spent from since"
fi

rule "6. above the ceiling: refused, two ways, before a transaction exists"
# THE ONE SECTION THAT NEEDS A KEY, AND WHAT A MACHINE WITHOUT ONE MUST SAY.
#
# Everything above this reads the chain: the anchored envelope, the program that
# owns it, the settlements, the ledger. This section is the only one that asks
# the agent to try something, and asking requires the agent's own shielded key —
# which lives in $AGENT_HOMES/<category>, deliberately outside this repository,
# and must never be on a shared machine. So CI cannot run this section, and the
# question is only whether it says so.
#
# `THRESHOLD_NO_AGENT_KEY=1` is that sentence, and it is deliberately loud. A
# skipped step that reads like a passing one is the exact defect this repository
# has been red for elsewhere, so the marker below is a line a caller has to
# assert on, the closing banner changes, and neither claims the refusal was
# demonstrated. What such a run does demonstrate is sections 1-5: that the
# envelope on chain is the one this repository publishes, that the program which
# owns it is the committed binary, that there is exactly one policy address per
# agent, and that every settlement charged its ledger by exactly its price.
if [ "${THRESHOLD_NO_AGENT_KEY:-0}" = 1 ]; then
  echo "  SECTION 6 NOT RUN: no agent key on this machine"
  note "THRESHOLD_NO_AGENT_KEY=1. The two refusals below are NOT attempted here."
  note "They need the agent's own shielded key ($AGENT_HOMES/$CAT), which is what"
  note "makes the attempt an attempt: spel builds the proof with it, and the"
  note "program refuses 6005 (over the per-transaction ceiling) and 6014 (a"
  note "window that is not on a period boundary) while it is being built."
  note "Nothing in sections 1-5 depends on it, and nothing here should be read as"
  note "showing the refusal. docs/use-cases.md carries the transcript of a run"
  note "that did, on the machine that holds the key."
  finish "Use case 3, partially: the ceiling is one account per agent and the chain
keeps it, and every claim in sections 1-5 was decoded or fetched here. The
refusal in section 6 was NOT exercised on this machine — it needs the agent's
key. Nothing was spent."
fi
# Run against a COPY of the agent's wallet home. A refused `spend` panics inside
# the guest while the proof is being built, and a panicking run leaves the wallet
# store in a state the next run cannot load — observed here, and it would take
# the live agent down with it. The copy also makes the point that nothing about
# this section touches the agent's real state.
rm -rf "$WORK"; mkdir -p "$WORK"
if [ -d "$AGENT_HOMES/$CAT" ]; then
  cp -R "$AGENT_HOMES/$CAT" "$WORK/home"
else
  bad "no wallet home at $AGENT_HOMES/$CAT — cannot attempt a refused spend"
fi
OVER=$((PER_TX + 1))
echo "  asking for $OVER LEZ, which is $((OVER - PER_TX)) above the ceiling of $PER_TX"
echo "  (and above the agent's balance, so nothing here can move money either way)"

# The period this spend would be accounted against. It is not optional and it is
# not zero: the policy account now carries the total it has spent and the period
# it spent it in, and naming an older period is refused on its own (error 6015)
# BEFORE the ceiling is ever looked at. An earlier version of this script passed
# window-start 0 and reported all three attempts as refused — correctly, and for
# the wrong reason, which is the failure mode every check in this repository is
# supposed to be built against.
HEIGHT=$(chain_height)
[ -n "$HEIGHT" ] || bad "could not read the chain height"
WINDOW=$(cargo run --quiet --release -p agent-policy-core --example window-start -- \
  "$HEIGHT" "$PERIOD" 2>/dev/null)
WINDOW_START=${WINDOW%% *}
echo "  block $HEIGHT, so the current period starts at $WINDOW_START"

# `attempt <what> <amount> <window-start> <expected-error>` — and the expected
# error code is the point. "Some program error happened" is satisfied by a typo
# in the arguments; naming the code is what makes each of these a demonstration
# of the mechanism it claims to be about.
#
# There used to be three attempts here and now there are two, because the third
# can no longer be expressed. It presented the anchored account while claiming
# bigger limits (6001), and then named the account those bigger limits hashed to
# (6002). `spend` carries neither an agent id nor any limits now — the policy
# address is derived from the paying account and the ceiling is read out of it —
# so there is no argument left to disagree with the chain. 6001 and 6013 are
# retired rather than reused for exactly that reason.
attempt() {
  local what="$1" amount="$2" window="$3" want="$4" out got
  out=$(LEE_WALLET_HOME_DIR="$WORK/home" NSSA_WALLET_HOME_DIR="$WORK/home" \
    "$SPEL" --idl "$IDL" --program "$PROGRAM" --bin-auth-transfer "$AUTH_TRANSFER" \
    -- spend --agent "Private/$AGENT" --recipient "Public/$RECIPIENT" \
    --amount "$amount" --window-start "$window" 2>&1)
  printf '\n  %s\n' "$what"
  got=$(echo "$out" | grep -oE 'Program error [0-9]+: .*' | head -1)
  [ -n "$got" ] && echo "         $got"
  # The refusal has to be the ABSENCE of a transaction, not an error message
  # next to one. A hash here would mean something was submitted.
  if echo "$out" | grep -qE 'tx_hash: [0-9a-f]{64}'; then
    bad "  a transaction was produced — $(echo "$out" | grep -oE 'tx_hash: [0-9a-f]{64}' | head -1)"
  elif echo "$got" | grep -q "Program error $want:"; then
    ok "  refused with $want: no transaction was built, so there is nothing to submit"
  elif [ -n "$got" ]; then
    bad "  refused, but by $got — expected error $want, so this proves something else"
  else
    bad "  neither a transaction nor a program error: $(echo "$out" | tail -1)"
  fi
  # A fresh copy per attempt, because the panic above poisons the store.
  rm -rf "$WORK/home"; cp -R "$AGENT_HOMES/$CAT" "$WORK/home"
}

if [ -d "$WORK/home" ]; then
  # 6005 — over the per-transaction ceiling, and the program says what to do
  # about it rather than merely refusing: use the approved path.
  attempt "the agent simply asks for more than its envelope allows" \
          "$OVER" "$WINDOW_START" 6005
  # 6014 — the other half of the envelope. The period is the one thing `spend`
  # still names, because no program on this chain can read the block height, so
  # the guest refuses any window that is not a multiple of period_blocks and
  # pins the transaction to the one it names. Sliding the window forward a block
  # at a time would otherwise reset the running total every block.
  attempt "the agent slides its period forward a block to reset the total" \
          1 "$((WINDOW_START + 1))" 6014
fi
rm -rf "$WORK"

rule "7. what the owner would have to do"
cat <<'TXT'
   An above-threshold payment is not forbidden — it is routed. `spend_approved`
   takes a fourth account: an approval PDA seeded by the exact payment (policy,
   recipient, amount, nonce) and owned by this program, which only the owner can
   create with `approve_spend`. Existence alone is not enough; anyone can fund an
   address, so the program checks the owner.

   That path is implemented and is NOT demonstrated on the public testnet here,
   and the reason is written down rather than glossed: an owner account that has
   anchored a policy has already spent its one program transaction and cannot
   sign a second. See docs/limitations.md, "The owner can never approve a spend
   after anchoring a policy". What this script demonstrates is the half that
   works: below the line the agent acts alone, above it the chain will not let it.
TXT

finish "Use case 3 holds: the ceiling is one account per agent, the chain keeps it,
and every claim above was decoded or fetched here. Nothing was spent."
