#!/usr/bin/env bash
# Check that docs/DEPLOYMENT.md still describes what this repository ships.
#
#   ./scripts/verify-deployment.sh
#
# DEPLOYMENT.md has twice gone stale in a way nothing caught: every figure in it
# was true on chain, and none of it described the program the repository
# shipped, because a redeploy moved the program and the document kept naming the
# old one. A reader following it got correct answers to the wrong questions.
#
# So the figures are checked rather than trusted. This compares, in order:
#
#   the committed binary  ->  its content-addressed deploy transaction
#   that transaction      ->  the chain (it must be included)
#   that transaction      ->  docs/DEPLOYMENT.md (it must be the one documented)
#   artifacts/agents.tsv  ->  the chain (each policy account, owned and decodable)
#   artifacts/a2a-task.tsv->  the chain (each settlement included)
#
# Exit 0 only if all of it agrees. Anything else is a document that needs
# regenerating, not a warning to scroll past.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
PROGRAM=artifacts/programs/agent_verifier.bin
DOC=docs/DEPLOYMENT.md
AGENTS=artifacts/agents.tsv
TASKS=artifacts/a2a-task.tsv

fail=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*" >&2; fail=$((fail + 1)); }

rpc() { # method params-json
  curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"
}

# Read a TSV column BY HEADER NAME. Never by position: the manifest's columns
# have been renamed and reordered before, and a positional read answers
# confidently with whatever now sits in that slot.
col() { # file header-name -> 1-based index
  awk -F'\t' -v w="$2" 'NR==1 { for (i=1;i<=NF;i++) if ($i==w) { print i; exit } }' "$1"
}
rows() { tail -n +2 "$1"; }

echo "program"
DEPLOY_TX=$(python3 -c "
import hashlib,struct
b=open('$PROGRAM','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())") || exit 1
echo "  committed binary hashes to $DEPLOY_TX"

BLOCK=$(rpc getTransaction "[\"$DEPLOY_TX\"]" \
        | python3 -c "import json,sys; r=json.load(sys.stdin).get('result'); print(r[1] if isinstance(r,list) else '')")
if [ -n "$BLOCK" ]; then
  ok "deployed, block $BLOCK"
else
  bad "the committed binary is NOT on chain — deploy it, or the repository ships a program nobody can call"
fi

if grep -q "$DEPLOY_TX" "$DOC"; then
  ok "$DOC documents this program"
else
  bad "$DOC does not mention $DEPLOY_TX — it documents a different deployment"
fi

# docs/benchmarks/cu-budget.md measures one specific binary, so it names one —
# and it has now drifted three program generations without anything going red.
# Not because the check is hard: because this script only ever looked at $DOC,
# while that document told its readers "the pointer is checkable: run the script,
# and it fails if the binary, the document and the chain stop agreeing". It did
# not. A promised guard that does not exist is worse than no guard, because it
# is why nobody looked.
#
# NOT a bare grep for the hash anywhere in the file. That document deliberately
# names superseded programs — its settlement-size comparison spans three
# generations — so matching one of those would go green while the cycle counts
# described something else entirely. What has to agree is the one row that says
# which binary produced the numbers.
#
# Hashes are elided there (`697746f5…cb5370bf`), so match head and tail rather
# than a middle the reader cannot see anyway.
CU=docs/benchmarks/cu-budget.md
if [ ! -f "$CU" ]; then
  bad "$CU is missing"
else
  CU_TX=$(awk -F'|' '/^\| *Deploy tx *\|/ { print $3; exit }' "$CU" | tr -d ' `')
  CU_HEAD=${CU_TX%%…*}
  CU_TAIL=${CU_TX##*…}
  if [ -z "$CU_TX" ]; then
    bad "$CU has no '| Deploy tx |' row: nothing in it states which program its cycle counts were measured against"
  elif [ "${DEPLOY_TX#"$CU_HEAD"}" != "$DEPLOY_TX" ] && [ "${DEPLOY_TX%"$CU_TAIL"}" != "$DEPLOY_TX" ]; then
    ok "$CU measures this program"
  else
    bad "$CU measures $CU_TX, not $DEPLOY_TX — its cycle counts describe a binary this repository no longer ships"
  fi
fi

# The control. A hash that cannot exist must answer null, otherwise "not found"
# proves nothing about the refusals this document relies on.
if [ "$(rpc getTransaction '["dededededededededededededededededededededededededededededededede"]' \
        | python3 -c "import json,sys; print(json.load(sys.stdin).get('result'))")" = "None" ]; then
  ok "control hash returns null"
else
  bad "the cannot-exist control hash returned something"
fi

echo
echo "policies ($AGENTS)"
c_cat=$(col "$AGENTS" category); c_pol=$(col "$AGENTS" policy_account)
c_tx=$(col "$AGENTS" create_tx);  c_ptx=$(col "$AGENTS" per_tx)
c_pp=$(col "$AGENTS" per_period);  c_pb=$(col "$AGENTS" period_blocks)
for v in c_cat c_pol c_tx c_ptx c_pp c_pb; do
  [ -n "${!v}" ] || { bad "$AGENTS is missing a column ($v)"; }
done

if [ -n "$c_cat" ] && [ -n "$c_pol" ]; then
  while IFS=$'\t' read -r -a f; do
    cat="${f[$((c_cat-1))]}"; pol="${f[$((c_pol-1))]}"
    ctx="${f[$((c_tx-1))]}";  ptx="${f[$((c_ptx-1))]}"
    pp="${f[$((c_pp-1))]}";   pb="${f[$((c_pb-1))]}"
    out=$(rpc getAccount "[\"$pol\"]" | python3 -c "
import json,sys
r=json.load(sys.stdin).get('result') or {}
d=bytes(r.get('data') or [])
if not any(r.get('program_owner') or []):
    print('MISSING'); raise SystemExit
if len(d)!=97 or d[0]!=1:
    print('BADRECORD %d' % len(d)); raise SystemExit
le=lambda a,b:int.from_bytes(d[a:b],'little')
print('OK %d %d %d %d %d' % (le(33,49),le(49,65),le(65,73),le(73,81),le(81,97)))")
    set -- $out
    case "${1:-}" in
      OK)
        if [ "$2" = "$ptx" ] && [ "$3" = "$pp" ] && [ "$4" = "$pb" ]; then
          ok "$cat  $pol  per_tx $2 / per_period $3 / $4 blocks; window $5 spent $6"
        else
          bad "$cat  $pol  chain says $2/$3/$4, manifest says $ptx/$pp/$pb"
        fi ;;
      MISSING)   bad "$cat  $pol  no such account on chain (or unowned)" ;;
      BADRECORD) bad "$cat  $pol  holds ${2:-?} bytes, not a 97-byte v1 record" ;;
      *)         bad "$cat  $pol  could not be read" ;;
    esac
    if [ -n "$(rpc getTransaction "[\"$ctx\"]" \
               | python3 -c "import json,sys; r=json.load(sys.stdin).get('result'); print(r[1] if isinstance(r,list) else '')")" ]; then
      ok "$cat  create_policy $ctx included"
    else
      bad "$cat  create_policy $ctx is not on chain"
    fi
  done < <(rows "$AGENTS")
fi

echo
echo "settlements ($TASKS)"
if [ -f "$TASKS" ]; then
  c_stx=$(col "$TASKS" settlement_tx)
  c_ba=$(col "$TASKS" balance_after)
  if [ -z "$c_stx" ]; then
    bad "$TASKS has no settlement_tx column"
  else
    while IFS=$'\t' read -r -a f; do
      stx="${f[$((c_stx-1))]}"; ba="${f[$((c_ba-1))]}"
      b=$(rpc getTransaction "[\"$stx\"]" \
          | python3 -c "import json,sys; r=json.load(sys.stdin).get('result'); print(r[1] if isinstance(r,list) else '')")
      if [ -n "$b" ]; then ok "$stx  block $b  (balance after $ba)"
      else bad "$stx is not on chain"; fi
    done < <(rows "$TASKS")
  fi
else
  bad "$TASKS is missing"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "DEPLOYMENT.md agrees with artifacts/ and with the chain."
  exit 0
fi
echo "$fail check(s) failed: docs/DEPLOYMENT.md no longer describes what this repository ships." >&2
exit 1
