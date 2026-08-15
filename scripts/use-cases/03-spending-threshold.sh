#!/usr/bin/env bash
# USE CASE 3 — the spending ceiling, accepted below it and refused above it.
#
#   ./scripts/use-cases/03-spending-threshold.sh
#
# The prize asks for an agent that "acts autonomously below a threshold the
# owner configures, and above it sends the proposed transaction to the owner and
# waits for approval". Everything else in this repository rests on that
# sentence being true of the chain rather than of the agent's source code: the
# agent holds its own keys on a remote node, so whoever takes the process takes
# the spending, and an `if (amount > limit)` in the agent is worth exactly
# nothing against them.
#
# So the ceiling is not a number stored anywhere. It is an ADDRESS. The policy
# account is a program-derived address of sha256(owner, agent, per-tx limit,
# per-period limit, period) — raising a limit does not edit an account, it names
# a different account, one that `create_policy` never initialised. This script
# shows that, from both sides, with the chain answering.
#
# WHAT IT COSTS: nothing. It submits no transaction. The three refusals below
# fail while the proof is being built, so no transaction is ever produced to
# submit — and each of them asks for 201 LEZ from an agent that holds far less
# than that, so even a ceiling that failed completely could not move money. The
# accepted side is not re-paid either: it reads settlements that already landed.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
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
POLICY=$(field "$AGENTS" "$CAT" policy_hash) || die "manifest unreadable"
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

rule "1. the envelope is a hash, and this script recomputes every one of them"
# Not read from the manifest and believed. The derivation lives in
# scripts/use-cases/policy-hash.py and is checked against EVERY policy this
# repository has anchored before it is used for anything — three independent
# (owner, agent, limits) tuples, whose accounts section 3 then finds on chain
# owned by the policy program. Agreeing with all three is agreeing with the code
# that anchored them.
if python3 scripts/use-cases/policy-hash.py --self-check "$AGENTS"; then
  ok "the derivation reproduces every anchored policy in $AGENTS"
else
  bad "the derivation disagrees with an anchored policy — nothing below is trustworthy"
fi
# And when the crate still ships its own derivation as a runnable example, the
# two are compared. Two implementations of one hash is how a system drifts; this
# is the check that turns drift into a failed run instead of a wrong address.
OWNER_HEX=$(id_hex "$OWNER")
AGENT_HEX=$(id_hex "$AGENT")
COMPUTED=$(policy_hash "$OWNER_HEX" "$AGENT_HEX" "$PER_TX" "$PER_PERIOD" "$PERIOD") \
  || die "cannot derive a policy hash, so this script cannot check anything"
if cargo run --quiet --release -p agent-policy-core --example policy-hash -- \
     "$OWNER_HEX" "$AGENT_HEX" "$PER_TX" "$PER_PERIOD" "$PERIOD" > "$WORK.crate" 2>/dev/null; then
  CRATE=$(tr -d '[:space:]' < "$WORK.crate")
  if [ "$CRATE" = "$COMPUTED" ]; then ok "and the crate's own example agrees with it"
  else bad "the crate computes $CRATE, this script computes $COMPUTED"; fi
else
  note "the crate ships no policy-hash example to cross-check against"
fi
rm -f "$WORK.crate"
echo "  owner  $OWNER"
echo "         = $OWNER_HEX"
echo "  agent  $AGENT"
echo "         = $AGENT_HEX"
echo "  sha256(prefix, owner, agent, $PER_TX, $PER_PERIOD, $PERIOD)"
echo "         = $COMPUTED"
if [ "$COMPUTED" = "$POLICY" ]; then ok "matches the policy hash in $AGENTS"
else bad "computed $COMPUTED, but the manifest records $POLICY"; fi

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

rule "3. the anchored envelope exists on chain, at its own address"
PDA=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda policy --policy-hash "$POLICY" 2>/dev/null | tr -d '[:space:]')
[ -n "$PDA" ] || bad "could not derive the policy PDA"
echo "  policy account for $PER_TX/$PER_PERIOD per $PERIOD blocks:"
echo "         $PDA"
GOT=$(owner_of "$PDA")
echo "  getAccount(...).program_owner = $GOT"
if [ "$GOT" = "$PID" ]; then
  ok "owned by exactly the program above — the owner anchored this envelope"
else
  bad "expected $PID, got $GOT"
fi

rule "4. a bigger ceiling is a different address, and nobody created it"
# This is the whole mechanism. An attacker who owns the agent process can pass
# any numbers it likes; what it cannot do is make an account exist.
check_absent() {
  local what="$1" tx="$2" period_total="$3" blocks="$4"
  local h p o
  h=$(policy_hash "$OWNER_HEX" "$AGENT_HEX" "$tx" "$period_total" "$blocks") || {
    bad "  could not derive the hash for $what"; return; }
  p=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda policy --policy-hash "$h" 2>/dev/null | tr -d '[:space:]')
  o=$(owner_of "$p")
  printf '  %-28s %s\n' "$what" "$p"
  if [ "$o" = "0,0,0,0,0,0,0,0" ]; then
    ok "  program_owner is all zeros: never initialised"
  else
    bad "  $p is owned by $o — an envelope nobody anchored has an owner"
  fi
}
check_absent "per-tx $((PER_TX * 10))"        "$((PER_TX * 10))"   "$PER_PERIOD"          "$PERIOD"
check_absent "per-period $((PER_PERIOD * 10))" "$PER_TX"           "$((PER_PERIOD * 10))" "$PERIOD"
check_absent "period $((PERIOD / 10)) blocks"  "$PER_TX"           "$PER_PERIOD"          "$((PERIOD / 10))"
# And the control, one more time, at the address level rather than the hash
# level: the PDA of a policy hash that cannot have been committed.
GHOST=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda policy --policy-hash "$IMPOSSIBLE" 2>/dev/null | tr -d '[:space:]')
GHOST_OWNER=$(owner_of "$GHOST")
printf '  %-28s %s\n' "control policy hash" "$GHOST"
if [ "$GHOST_OWNER" = "0,0,0,0,0,0,0,0" ]; then ok "  program_owner is all zeros"
else bad "  the control policy account is owned by $GHOST_OWNER"; fi

rule "5. below the ceiling: accepted, unattended, and already on chain"
# Not re-paid. A settlement costs real testnet balance and the funder holds a
# handful of LEZ; what makes these evidence is that the chain still holds them,
# which is checkable now and by anyone. `./scripts/a2a-task.sh` is what produced
# them and will produce another.
if [ ! -s "$SETTLEMENTS" ]; then
  bad "no settlement manifest at $SETTLEMENTS"
else
  n=0; landed=0
  while IFS=$'\t' read -r task client server pay skill price nonce tx before after; do
    [ "$task" = task_id ] && continue
    [ -n "$tx" ] || continue
    n=$((n + 1))
    printf '  %s LEZ  %s\n' "$price" "$tx"
    if [ "$price" -le "$PER_TX" ]; then
      note "$price <= $PER_TX, so spend takes the autonomous branch"
    else
      bad "  a settled price of $price is ABOVE the anchored ceiling of $PER_TX"
    fi
    if tx_live "$tx"; then ok "  the chain holds it"; landed=$((landed + 1))
    else bad "  getTransaction returns null — this is not evidence of a payment"; fi
    if [ "$((after - before))" -eq "$price" ]; then
      ok "  the recipient went $before -> $after, exactly the price"
    else
      bad "  recorded $before -> $after for a price of $price"
    fi
  done < "$SETTLEMENTS"
  [ "$landed" -gt 0 ] || bad "not one settlement in the manifest is on chain"
  NOW=$(balance_of "$RECIPIENT")
  echo "  $RECIPIENT holds $NOW LEZ now, by getAccount"
fi

rule "6. above the ceiling: refused, three ways, before a transaction exists"
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

# `attempt <what> <policy-hash> <claimed-per-tx> <expected-error>` — and the
# expected error code is the point. "Some program error happened" is satisfied by
# a typo in the arguments; naming the code is what makes each of these three a
# demonstration of the mechanism it claims to be about.
attempt() {
  local what="$1" hash="$2" tx="$3" want="$4" out got
  out=$(LEE_WALLET_HOME_DIR="$WORK/home" NSSA_WALLET_HOME_DIR="$WORK/home" \
    "$SPEL" --idl "$IDL" --program "$PROGRAM" --bin-auth-transfer "$AUTH_TRANSFER" \
    -- spend --agent "Private/$AGENT" --recipient "Public/$RECIPIENT" \
    --policy-hash "$hash" --owner-id "$OWNER_HEX" --agent-id "$AGENT_HEX" \
    --per-tx "$tx" --per-period "$PER_PERIOD" --period-blocks "$PERIOD" \
    --amount "$OVER" --window-start "$WINDOW_START" 2>&1)
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
          "$POLICY" "$PER_TX" 6005
  # 6001 — the numbers do not hash to the account being presented. This is the
  # attack an agent's own process can mount: pass bigger limits.
  attempt "the agent presents the anchored account but claims a bigger ceiling" \
          "$POLICY" "$((PER_TX * 10))" 6001
  # 6002 — so name the account those bigger limits DO hash to. It exists as an
  # address and has never been initialised, which is section 4 above, reached
  # from inside the program instead of from getAccount.
  RAISED=$(policy_hash "$OWNER_HEX" "$AGENT_HEX" "$((PER_TX * 10))" "$PER_PERIOD" "$PERIOD") \
    || die "cannot derive the raised envelope's hash"
  attempt "the agent names the bigger envelope's own account instead" \
          "$RAISED" "$((PER_TX * 10))" 6002
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

finish "Use case 3 holds: the ceiling is an address, the chain keeps it, and every
claim above was computed or fetched here. Nothing was spent."
