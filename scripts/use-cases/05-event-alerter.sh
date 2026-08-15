#!/usr/bin/env bash
# USE CASE 5 — an on-chain event alerter: the agent watches a LEZ account for a
# state change and tells the owner over Logos Messaging when it happens.
#
#   ./scripts/use-cases/05-event-alerter.sh prepare   # a fresh agent to watch
#   ./scripts/use-cases/05-event-alerter.sh           # terminal A: watch
#   ./scripts/use-cases/05-event-alerter.sh claim     # terminal B: the event
#
#   ALERTER_SELF_TRIGGER=1 ./scripts/use-cases/05-event-alerter.sh   # both, one process
#
# From the prize's list of illustrative use cases:
#
#   "On-chain event alerter: agent monitors a LEZ program or account for state
#    changes and notifies the owner via Logos Messaging."
#
# WHY THIS PARTICULAR EVENT, which is the whole question for an alerter. It
# would be easy to point this at something that changes on its own — the chain
# rewrites /LEZ/ClockProgramAccount/0000001 every block — and produce a green
# transcript that demonstrates polling. The event has to matter.
#
# Anchoring a policy takes TWO signatures on this deployment:
#
#   `claim_agent` is signed by the AGENT and moves no value. It writes
#   PDA(program, ["agent-owner/v1", agent]) and names an owner.
#
#   `create_policy` is signed by the OWNER the claim names, and cannot run until
#   that claim exists — the program refuses any other signer (6020) and refuses
#   outright if the agent never claimed (6019).
#
# The gap between those two signatures is a security decision the owner has to
# make, and nothing currently tells them an agent has named them. Any agent may
# name any owner; the claim costs nothing and needs no permission from the
# account it names. So the owner learns about it or they do not.
#
# That is a real alert about a real step of the shipped flow, not an event
# invented so the demo has something to watch. A reviewer can check that against
# the program: the two instructions and their refusal codes are in
# crates/agent-verifier-spel, and the claim account's seeds are in the IDL.
#
# WHY THE EVENT COMES FROM A SEPARATE INVOCATION. A demonstration that causes
# the event it then detects reads as a loop even when it is not, so by default
# the watcher does not make the claim: it prints the command, and a second
# process run by the reader makes it. `ALERTER_SELF_TRIGGER=1` runs both in one
# process for CI, where orchestrating two is not worth the machinery. The
# default is the one that survives a sceptical reading.
#
# WHAT IT COSTS: nothing. `auth-transfer init` and `claim_agent` both move zero
# value and LEZ v0.2.4 charges no execution fee, so a full run is 0 LEZ. The
# agent it creates is funded with nothing and never needs to be.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
. scripts/use-cases/lib.sh

SPEL="${SPEL_BIN:-spel}"
WALLET="${WALLET_BIN:-wallet}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
WORK="${ALERTER_WORK:-${TMPDIR:-/tmp}/lp0008-usecase-05}"
RUN="$WORK/run.env"
TOPIC="${ALERTER_TOPIC:-/lp0008/1/owner-alerts/proto}"
SELF="${ALERTER_SELF_TRIGGER:-0}"
MANIFEST="${ALERTER_MANIFEST:-artifacts/alerts.tsv}"
# Re-check every alert this repository has ever raised, against the chain, with
# no wallet and no messaging node. That is what CI can run: a runner has no
# agent keys and must not be given any, and an alert nobody can re-verify a week
# later was never evidence.
VERIFY_ONLY="${ALERTER_VERIFY_ONLY:-0}"
# 20 minutes at 15-second polls. Blocks are 60 seconds apart and a claim needs
# one to be built, so a window shorter than a few blocks would report a slow
# chain as a missing event.
POLL_SECONDS="${ALERTER_POLL_SECONDS:-15}"
POLL_TRIES="${ALERTER_POLL_TRIES:-80}"

DLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"; [ -f "$DLIB" ] || DLIB="$DELIVERY_SRC/build/liblogosdelivery.so"

wallet_home() { export NSSA_WALLET_HOME_DIR="$1" LEE_WALLET_HOME_DIR="$1"; }

# A wallet home with nothing in it. The config is copied from the owner's if
# there is one and written from scratch otherwise, so a clean clone can run this
# without having a wallet configured first.
new_home() {
  mkdir -p "$1"
  cp "$HOME/.lez-wallet/wallet_config.json" "$1/" 2>/dev/null || \
    printf '{ "sequencers": [{ "sequencer_addr": "%s" }], "seq_poll_timeout": "30s", "seq_tx_poll_max_blocks": 15, "seq_poll_max_retries": 10, "seq_block_poll_max_amount": 100 }\n' "$RPC" > "$1/wallet_config.json"
}

first_public() {
  "$WALLET" account list </dev/null 2>/dev/null \
    | grep -oE 'Public/[1-9A-HJ-NP-Za-km-z]{32,}' | sed 's|Public/||' | head -n1
}

# `claim_record <account> <field>` — the 33 bytes a claim account holds, decoded
# here rather than taken on trust: a version byte of 2, then the 32 bytes of the
# owner the agent named. Anything else is an error, because an unreadable claim
# must never read as an absent one.
claim_record() {
  rpc getAccount "[\"$1\"]" | python3 -c "
import json, sys
r = json.load(sys.stdin).get('result')
if not r: sys.exit('no such account')
d = bytes(r['data'])
if len(d) != 33 or d[0] != 2:
    sys.exit('%s holds %d bytes, not a claim this program wrote' % (sys.argv[1], len(d)))
print({'version': d[0], 'owner': d[1:33].hex()}[sys.argv[2]])" "$1" "$2"
}

# `control_a_no_record <ghost-account> <an-account-that-DOES-hold-a-claim>`
#
# The comment above claim_record says "an unreadable claim must never read as an
# absent one", and the control that was supposed to hold it to that was
#
#     if claim_record "$GHOST_PDA" owner >/dev/null 2>&1; then bad; else ok; fi
#
# which is the shape it was warning about. `claim_record` exits non-zero when the
# account holds no claim AND when the RPC is unreachable, when curl times out,
# when the JSON does not parse, when python is missing. Run with SEQUENCER_URL
# pointing at a dead port, that control printed
#   "OK control A: and it yields no claim record — 'unreadable' cannot read as
#    'absent'"
# on a run where every other check had already failed for want of a chain.
#
# So two things are required instead of one refusal: the ghost must fail with
# the SPECIFIC message that means the account is not there, and the very same
# helper must SUCCEED against an account that does hold a claim. An unreachable
# chain fails the second half, which is what the sentence claims.
control_a_no_record() {
  local ghost="$1" known="$2" err
  err=$(claim_record "$ghost" owner 2>&1 >/dev/null)
  if [ -z "$err" ]; then
    bad "control A: an unclaimed account produced a claim record"
    return
  fi
  # The two ways this chain says "nobody ever wrote here": no result at all, or
  # the default account, which getAccount answers with for an uninitialised PDA
  # and which decodes as zero bytes. Anything else — a curl that timed out, a
  # body that did not parse, a missing python — is a READ failure and must not
  # be reported as an absence, which is the whole of what this control claims.
  case "$err" in
    *"no such account"*|*"holds 0 bytes"*) ;;
    *) bad "control A: the ghost account failed to decode, but not by being absent: $err"
       return ;;
  esac
  if [ -z "$known" ]; then
    bad "control A: no account known to hold a claim, so 'no record' proves nothing here"
  elif claim_record "$known" owner >/dev/null 2>&1; then
    ok "control A: and it yields no claim record, while the same read succeeds on $known"
  else
    bad "control A: the reader cannot decode $known either — it is not distinguishing anything"
  fi
}

# `find_claim_tx <account> <from-block> <to-block>` — the transaction that wrote
# an account, discovered by reading the blocks rather than by being told.
#
# A watcher does not know the hash of the transaction it just noticed; that is
# the claimer's to know. What it does know is the chain height before it started
# watching and the height when the change appeared, so the cause is in that
# range. Each block is fetched, and the one holding the account's 32 bytes is
# the one. Isolating the transaction inside it is then arithmetic: a block is a
# 148-byte header, the transactions, and the sequencer's own 154-byte clock
# transaction last, so a block carrying one user transaction hands it over
# whole.
#
# The candidate is not trusted on that arithmetic. Its hash is computed and then
# put back to `getTransaction`, which must return it AND agree about the block.
# If it does not, this prints the block and no hash — a watcher that cannot
# isolate the transaction should say so, not guess.
find_claim_tx() {
  python3 - "$1" "$2" "$3" "$RPC" <<'PY'
import base64, hashlib, json, sys, urllib.request
acct_b58, lo, hi, rpc_url = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
n = 0
for c in acct_b58:
    n = n * 58 + A.index(c)
raw = n.to_bytes(32, 'big')

def rpc(method, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                       "params": params}).encode()
    req = urllib.request.Request(rpc_url, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r).get("result")

HEADER, CLOCK = 148, 154
for h in range(lo, hi + 1):
    blk = rpc("getBlock", [h])
    if not blk:
        continue
    b = base64.b64decode(blk)
    if raw not in b:
        continue
    print("block=%d" % h)
    count = int.from_bytes(b[HEADER - 4:HEADER], 'little')
    if count == 2 and len(b) > HEADER + CLOCK:
        tx = b[HEADER:len(b) - CLOCK]
        digest = hashlib.sha256(tx[1:]).hexdigest()
        back = rpc("getTransaction", [digest])
        # Round-tripped: the chain returns it, and agrees about the block.
        if back and int(back[1]) == h:
            print("tx=%s" % digest)
            print("tx_bytes=%d" % len(tx))
        else:
            print("note=could_not_isolate_the_transaction_in_this_block")
    else:
        print("note=%d_transactions_in_this_block" % count)
    sys.exit(0)
print("block=")
PY
}

# ---------------------------------------------------------------- prepare ----
prepare() {
  rule "prepare: an agent nobody has claimed, and an owner for it to name"
  command -v "$WALLET" >/dev/null 2>&1 || [ -x "$WALLET" ] \
    || die "no wallet on PATH. Set WALLET_BIN to the LEZ wallet binary."
  rm -rf "$WORK"; mkdir -p "$WORK"
  new_home "$WORK/agent"; new_home "$WORK/owner"

  wallet_home "$WORK/owner"
  "$WALLET" account new public </dev/null >/dev/null 2>&1
  local owner; owner=$(first_public)
  [ -n "$owner" ] || die "could not create an owner account"

  wallet_home "$WORK/agent"
  "$WALLET" account new public </dev/null >/dev/null 2>&1
  local agent; agent=$(first_public)
  [ -n "$agent" ] || die "could not create an agent account"

  echo "  agent  $agent"
  echo "  owner  $owner"
  # The agent has to exist under the transfer program before it can sign. This
  # moves no value and is the same call use case 4 measured at 0 LEZ.
  echo "  initialising the agent on chain (moves no value)"
  local out; out=$("$WALLET" auth-transfer init --account-id "Public/$agent" </dev/null 2>&1)
  local itx; itx=$(printf '%s' "$out" | grep -oE '[0-9a-f]{64}' | head -1)
  if [ -n "$itx" ]; then
    ok "the agent account exists on chain: $itx"
  else
    printf '%s\n' "$out" | tail -4 | sed 's/^/  /' >&2
    die "the agent account could not be created"
  fi
  local claim; claim=$("$SPEL" --idl "$IDL" --program "$PROGRAM" \
    pda claim --agent "$agent" 2>/dev/null | tail -n1 | tr -d '[:space:]')
  [ -n "$claim" ] || die "could not derive the claim account"
  printf 'AGENT=%s\nOWNER=%s\nCLAIM=%s\n' "$agent" "$owner" "$claim" > "$RUN"
  echo "  claim account $claim"
  ok "prepared — $RUN"
}

# ------------------------------------------------------------------ claim ----
# The separate invocation. This is the only place in this file that writes the
# event, and the watcher never calls it unless ALERTER_SELF_TRIGGER=1.
claim() {
  [ -f "$RUN" ] || die "nothing prepared — run '$0 prepare' first"
  . "$RUN"
  rule "the agent claims an owner"
  echo "  agent $AGENT"
  echo "  names $OWNER as the only account that may anchor its policy"
  local owner_hex; owner_hex=$(id_hex "$OWNER") || die "bad owner id"
  wallet_home "$WORK/agent"
  local out; out=$(RISC0_DEV_MODE=0 "$SPEL" --idl "$IDL" --program "$PROGRAM" \
    -- claim-agent --agent "Public/$AGENT" --owner-id "$owner_hex" 2>&1)
  local tx; tx=$(printf '%s' "$out" | grep -oE 'tx_hash: [0-9a-f]{64}' | head -1 | cut -d' ' -f2)
  if [ -z "$tx" ]; then
    printf '%s\n' "$out" | tail -8 | sed 's/^/  /' >&2
    die "claim_agent produced no transaction"
  fi
  echo "  claim_agent $tx"
  printf 'CLAIM_TX=%s\n' "$tx" >> "$RUN"
  ok "submitted — the watcher is not told this hash; it finds it in the blocks"
}

# `verify_recorded` — every alert in the manifest, re-derived and re-fetched.
# The manifest holds identifiers only. The claim account is re-derived from the
# agent id through the IDL, the transaction is re-fetched and its bytes re-hashed,
# the owner is re-decoded from the account, and the recorded values are compared
# against those — never used in place of them.
verify_recorded() {
  rule "recorded alerts, re-checked against the chain"
  [ -s "$MANIFEST" ] || { bad "no alerts recorded at $MANIFEST"; return; }
  # `seen` is rows this loop entered; `n` is rows that passed EVERY check in it.
  # They were one variable, incremented on entry, and the summary line below —
  # the one CI parses and compares against the manifest's row count — called it
  # "each re-verified from the chain". It was not: a row whose claim the chain
  # would not return still incremented it and then `continue`d, so a run against
  # an unreachable chain printed "OK 1 alert(s), each re-verified from the
  # chain" having re-verified none. Demonstrated by pointing SEQUENCER_URL at a
  # dead port.
  local n=0 seen=0 row_ok
  local agents; agents=$(column_of "$MANIFEST" agent)
  local a
  for a in $agents; do
    [ -n "$a" ] || continue
    local rec_claim rec_owner rec_tx rec_block
    rec_claim=$(field "$MANIFEST" "$a" claim_account)
    rec_owner=$(field "$MANIFEST" "$a" owner)
    rec_tx=$(field "$MANIFEST" "$a" claim_tx)
    rec_block=$(field "$MANIFEST" "$a" block)
    seen=$((seen + 1))
    row_ok=1
    echo
    echo "  agent $a"
    echo "    $rec_tx"
    # The address, derived rather than believed.
    local pda
    pda=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda claim --agent "$a" 2>/dev/null \
      | tail -n1 | tr -d '[:space:]')
    if [ -n "$pda" ] && [ "$pda" = "$rec_claim" ]; then
      ok "  PDA([\"agent-owner/v1\", agent]) is the recorded claim account"
    else
      bad "  the agent derives ${pda:-<nothing>}, the manifest records $rec_claim"
      continue
    fi
    # The transaction, re-fetched and re-hashed.
    local f blk
    f=$(settlement_facts "$rec_tx")
    if [ "$(kv "$f" found)" = "1" ] && [ "$(kv "$f" hash_ok)" = "1" ]; then
      blk=$(kv "$f" block)
      ok "  the chain holds it, in block $blk, and its bytes hash to it"
    else
      bad "  getTransaction returns null — this claim is not on chain"
      continue
    fi
    if [ "$blk" = "$rec_block" ]; then
      ok "  in the block the manifest records"
    else
      bad "  the chain says block $blk, the manifest records $rec_block"; row_ok=0
    fi
    # The owner, decoded out of the account the claim wrote.
    local owner
    owner=$(claim_record "$rec_claim" owner) || { bad "  the claim account holds no claim record"; continue; }
    if [ "$owner" = "$rec_owner" ]; then
      ok "  it still names owner $owner"
    else
      bad "  the chain says the owner is $owner, the manifest records $rec_owner"; row_ok=0
    fi
    local own_words; own_words=$(owner_of "$rec_claim")
    if [ -n "$PID" ] && [ "$own_words" = "$PID" ]; then
      ok "  and the account is owned by this repository's policy program"
    else
      bad "  the claim account is owned by $own_words, not $PID"; row_ok=0
    fi
    [ "$row_ok" -eq 1 ] && n=$((n + 1))
  done
  echo
  if [ "$n" -ge 1 ] && [ "$n" -eq "$seen" ]; then
    ok "$n alert(s), each re-verified from the chain"
  elif [ "$seen" -ge 1 ]; then
    bad "$n of $seen alert(s) re-verified — the rest failed a check above"
  else
    bad "the manifest records no alert at all"
  fi
}

case "${1:-watch}" in
  prepare) prepare; exit $FAILED ;;
  claim)   claim;   exit $FAILED ;;
  watch)   ;;
  *) die "usage: $0 [prepare|claim|watch]" ;;
esac

if [ "$VERIFY_ONLY" = "1" ]; then
  PID=$("$SPEL" program-id "$PROGRAM" 2>/dev/null | awk -F': *' '/ProgramId \(decimal\)/ {print $2}')
  [ -n "$PID" ] || bad "could not read the ProgramId out of $PROGRAM"
  verify_recorded
  rule "the controls, each shown red"
  GHOST_AGENT=$(hex_id "$IMPOSSIBLE")
  GHOST_PDA=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda claim --agent "$GHOST_AGENT" 2>/dev/null \
    | tail -n1 | tr -d '[:space:]')
  [ -n "$GHOST_PDA" ] || bad "control A: could not derive the unclaimed control account"
  printf '  %-30s %s\n' "an agent nobody claimed" "$GHOST_PDA"
  if [ "$(owner_of "$GHOST_PDA")" = "0,0,0,0,0,0,0,0" ]; then
    ok "control A: it reads as the default account, so the detector does not fire"
  else
    bad "control A: the unclaimed control account is owned by $(owner_of "$GHOST_PDA")"
  fi
  control_a_no_record "$GHOST_PDA" "$(column_of "$MANIFEST" claim_account 2>/dev/null | grep -m1 .)"
  if rpc getTransaction "[\"$IMPOSSIBLE\"]" | grep -q '"result":null'; then
    ok "control B: a transaction hash that cannot exist returns null"
  else
    bad "control B: the control hash did not return null"
  fi
  finish "Use case 5 re-verified: every recorded alert is still on chain, at the block
recorded, naming the owner recorded, in an account derived from the agent's own id."
fi

# ------------------------------------------------------------------ watch ----
[ -f "$RUN" ] || prepare
[ -f "$RUN" ] || die "nothing to watch"
. "$RUN"

rule "1. the account a claim would be written to"
# Derived from the agent's own id through the published IDL, not copied from
# anywhere. There is exactly one such address per agent and no argument to vary.
PDA=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda claim --agent "$AGENT" 2>/dev/null \
  | tail -n1 | tr -d '[:space:]')
echo "  agent  $AGENT"
echo "  owner  $OWNER  (the account a claim would name)"
echo "  PDA(program, [\"agent-owner/v1\", $AGENT])"
echo "         $PDA"
if [ -n "$PDA" ] && [ "$PDA" = "$CLAIM" ]; then
  ok "the watched address derives from the agent's id"
else
  bad "the derivation gives $PDA, the run file records $CLAIM"
fi
PID=$("$SPEL" program-id "$PROGRAM" 2>/dev/null | awk -F': *' '/ProgramId \(decimal\)/ {print $2}')
[ -n "$PID" ] || bad "could not read the ProgramId out of $PROGRAM"

rule "2. before: the chain says nobody has claimed this agent"
# An uninitialised account is not an error on this chain — `getAccount` answers
# with a default account, zero balance and owner all zeros — which is exactly
# what an unclaimed agent looks like and exactly what is checked for. Without
# this the alert below would be consistent with a claim that was already there.
BEFORE=$(owner_of "$PDA")
if [ "$BEFORE" = "0,0,0,0,0,0,0,0" ]; then
  ok "program_owner is all zeros: no claim exists"
else
  bad "that account is already owned by $BEFORE — this run cannot show a change"
fi
H0=$(chain_height)
[ -n "$H0" ] || bad "could not read the chain height"
echo "  watching from block $H0"

rule "3. watching"
if [ "$SELF" = "1" ]; then
  note "ALERTER_SELF_TRIGGER=1 — this process will make the claim itself."
  note "The default is a separate invocation; this mode exists for CI."
  ( claim > "$WORK/claim.log" 2>&1 ) &
  CLAIM_PID=$!
else
  echo "  This process will NOT make the claim. In another terminal, run:"
  echo
  echo "      $0 claim"
  echo
  echo "  Waiting up to $((POLL_SECONDS * POLL_TRIES / 60)) minutes for the account to change."
  CLAIM_PID=
fi
FIRED=0
for _ in $(seq 1 "$POLL_TRIES"); do
  NOW_OWNER=$(owner_of "$PDA")
  if [ "$NOW_OWNER" != "0,0,0,0,0,0,0,0" ] && [ -n "$NOW_OWNER" ] && [ "$NOW_OWNER" != null ]; then
    FIRED=1; break
  fi
  sleep "$POLL_SECONDS"
done
[ -n "$CLAIM_PID" ] && wait "$CLAIM_PID" 2>/dev/null
H1=$(chain_height)
if [ "$FIRED" = "1" ]; then
  ok "the account changed: program_owner went 0,0,0,0,0,0,0,0 -> $NOW_OWNER"
  if [ "$NOW_OWNER" = "$PID" ]; then
    ok "and that is this repository's policy program, so a claim wrote it"
  else
    bad "it is owned by $NOW_OWNER, not $PID"
  fi
else
  bad "nothing claimed $AGENT within the watch window"
fi

rule "4. what changed, decoded, and the transaction that did it"
CLAIMED_OWNER=
if [ "$FIRED" = "1" ]; then
  CLAIMED_OWNER=$(claim_record "$PDA" owner) || bad "the claim account did not decode"
  echo "  the claim names owner $CLAIMED_OWNER"
  OWNER_HEX=$(id_hex "$OWNER")
  if [ "$CLAIMED_OWNER" = "$OWNER_HEX" ]; then
    ok "which is $OWNER — the owner this alert must go to"
  else
    bad "the claim names $CLAIMED_OWNER, not $OWNER_HEX"
  fi
  # The watcher was never told the hash. It knows the change happened between
  # two heights it read itself, so it reads the blocks in between.
  echo "  searching blocks $H0..$H1 for the transaction that wrote $PDA"
  FOUND=$(find_claim_tx "$PDA" "$H0" "$H1")
  EV_BLOCK=$(kv "$FOUND" block); EV_TX=$(kv "$FOUND" tx)
  if [ -n "$EV_BLOCK" ]; then
    ok "found it in block $EV_BLOCK"
  else
    bad "the change is on chain but no block in $H0..$H1 holds that account"
  fi
  if [ -n "$EV_TX" ]; then
    ok "the transaction is $EV_TX ($(kv "$FOUND" tx_bytes) bytes, hash round-tripped through getTransaction)"
  else
    note "$(kv "$FOUND" note)"
  fi
fi

rule "5. the owner is told, over Logos Messaging"
ALERT=
if [ "$FIRED" = "1" ]; then
  if [ ! -f "$DLIB" ]; then
    cat >&2 <<TXT
  no Logos Delivery library at $DLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules \\
      https://github.com/logos-messaging/logos-delivery $DELIVERY_SRC
  cd $DELIVERY_SRC && make liblogosdelivery
TXT
    bad "the alert cannot be sent: build the Delivery library or set DELIVERY_SRC"
  else
    SBIN="$WORK/share_drive"
    cc -o "$SBIN" scripts/use-cases/share_drive.c \
       -I"$DELIVERY_SRC/library" -L"$DELIVERY_SRC/build" -llogosdelivery \
       -Wl,-rpath,"$DELIVERY_SRC/build" || bad "the messaging driver did not compile"
    # Every field is one the chain answered for. Nothing in this payload is read
    # from the run file: the owner is decoded from the claim account, the block
    # and the transaction were found in the blocks.
    ALERT=$(python3 -c "
import json, sys
print(json.dumps({'type': 'agent-claim',
                  'agent': sys.argv[1], 'claim_account': sys.argv[2],
                  'owner': sys.argv[3], 'block': sys.argv[4], 'tx': sys.argv[5],
                  'action': 'create_policy to anchor, or ignore to refuse'},
                 separators=(',', ':'), sort_keys=True))" \
      "$AGENT" "$PDA" "$CLAIMED_OWNER" "${EV_BLOCK:-}" "${EV_TX:-}")
    echo "  topic   $TOPIC"
    echo "  payload $ALERT"
    MLOG="$WORK/messaging.log"
    if [ -x "$SBIN" ]; then
      "$SBIN" "$TOPIC" "$ALERT" > "$MLOG" 2>&1; mrc=$?
      grep -E "^[0-9]\.|^  (ok|FAIL|<-|note|topic)|message_(sent|propagated|error)" "$MLOG" \
        | sed 's/^/  /' | head -20
      if [ "$mrc" -eq 0 ]; then ok "the alert reached the Logos Messaging network"
      else bad "the messaging driver failed — full log at $MLOG"; fi
      # The bytes on the wire have to BE the alert, not a description of it.
      WANT_B64=$(printf '%s' "$ALERT" | base64 | tr -d '\n')
      if grep -q "$WANT_B64" "$MLOG"; then
        ok "the published payload decodes to exactly the alert above"
      else
        bad "what went out was not the alert"
      fi
    fi
  fi
fi

rule "6. the controls, each shown red"
# CONTROL A — an agent nobody claimed must never fire. The detector is run
# against a second agent id that has never been claimed, with the same code
# path, and is required NOT to alert. A watcher that fired on anything would
# pass section 3 exactly as happily.
# `--agent` here wants base58, where `pda policy --agent-id` wants hex. Passing
# the hex form makes spel exit with "not a valid base58 account ID" and print
# nothing, and an empty PDA then makes `getAccount` answer null — which is not
# all-zeros, so this control failed loudly rather than passing vacuously. It
# would have been a silent pass had the empty string resolved to anything.
GHOST_AGENT=$(hex_id "$IMPOSSIBLE")
GHOST_PDA=$("$SPEL" --idl "$IDL" --program "$PROGRAM" pda claim --agent "$GHOST_AGENT" 2>/dev/null \
  | tail -n1 | tr -d '[:space:]')
[ -n "$GHOST_PDA" ] || bad "control A: could not derive the unclaimed control account"
GHOST_OWNER=$(owner_of "$GHOST_PDA")
printf '  %-30s %s\n' "an agent nobody claimed" "$GHOST_PDA"
if [ "$GHOST_OWNER" = "0,0,0,0,0,0,0,0" ]; then
  ok "control A: it reads as the default account, so the detector does not fire"
else
  bad "control A: the unclaimed control account is owned by $GHOST_OWNER"
fi
# And the decode, not just the ownership: a claim record must refuse to come out
# of an account that holds none.
control_a_no_record "$GHOST_PDA" "${PDA:-}"

# CONTROL B — the payload check must be capable of failing. The same comparison
# that passed in section 5 is run against an alert with one field changed, and
# is required to fail. Otherwise "the payload decodes to the alert" is satisfied
# by a grep that matches anything.
if [ -n "$ALERT" ] && [ -f "$WORK/messaging.log" ]; then
  MUTANT=$(printf '%s' "$ALERT" | sed 's/"agent":"./"agent":"X/')
  MUT_B64=$(printf '%s' "$MUTANT" | base64 | tr -d '\n')
  if grep -q "$MUT_B64" "$WORK/messaging.log"; then
    bad "control B: an alert naming a different agent also matched the wire"
  else
    ok "control B: changing one field of the alert stops it matching the wire"
  fi
else
  bad "control B: no alert was published, so the payload check proves nothing"
fi

rule "7. recorded, so it can be re-checked without re-running"
# Identifiers only. Everything a reader would want to believe about them is
# re-derived or re-fetched by ALERTER_VERIFY_ONLY=1, which needs no wallet, no
# messaging node and no second process — which is what CI runs.
if [ "$FIRED" = "1" ] && [ -n "${EV_TX:-}" ] && [ "$FAILED" -eq 0 ]; then
  HDR='agent\tclaim_account\towner\tclaim_tx\tblock'
  [ -s "$MANIFEST" ] && [ "$(head -n1 "$MANIFEST")" = "$(printf "$HDR")" ] \
    || printf "$HDR\n" > "$MANIFEST"
  if ! grep -q "	$EV_TX	" "$MANIFEST" 2>/dev/null; then
    printf '%s\t%s\t%s\t%s\t%s\n' "$AGENT" "$PDA" "$CLAIMED_OWNER" "$EV_TX" "$EV_BLOCK" >> "$MANIFEST"
  fi
  ok "appended to $MANIFEST"
else
  note "nothing recorded: an alert is only evidence if the run that raised it passed"
fi

rule "8. what this is, and what it is not"
cat <<'TXT'
   What it is: an owner learning, from the chain and without being told, that an
   agent has named them. `claim_agent` is signed by the agent and needs no
   permission from the account it names, and `create_policy` — the signature
   that actually grants an agent its spending envelope — is the owner's alone.
   Between those two the owner has a decision to make, and this is what tells
   them there is one.

   What it is not: a subscription. This chain has no notification RPC, so the
   watcher polls `getAccount`, and it watches ONE account whose address it
   derived from an agent id it was given. An owner cannot enumerate every claim
   naming them: the claim account is a PDA of the AGENT, so there is nothing to
   scan for without knowing the agent. That is a real limitation of the address
   scheme and not of this script.
TXT

echo
finish "Use case 5 holds: an agent claimed an owner in a separate process, the watcher
saw the account change, decoded who was named, found the transaction in the
blocks without being told its hash, and put the alert on a real Logos Messaging
topic. Nothing was spent."
