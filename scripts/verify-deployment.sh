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
#   artifacts/shielded-settlement.tsv
#                         ->  the chain (each settlement to a SHIELDED payee, in
#                             the block the row names, under the program it claims)
#
# Exit 0 only if all of it agrees. Anything else is a document that needs
# regenerating, not a warning to scroll past.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
PROGRAM=artifacts/programs/agent_verifier.bin
DOC=docs/DEPLOYMENT.md
AGENTS=artifacts/agents.tsv
TASKS=artifacts/a2a-task.tsv
SHIELDED=artifacts/shielded-settlement.tsv

fail=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*" >&2; fail=$((fail + 1)); }
# For the sentence a FAIL needs after it when the likeliest cause is that the
# chain moved rather than that this repository is wrong.
note() { printf '        %s\n' "$*"; }

rpc() { # method params-json
  # See the note on the same helper in scripts/use-cases/lib.sh: an unreachable
  # host must not read as an empty result, because every caller here treats an
  # empty result as a statement about the chain.
  local out rc
  out=$(curl -s -m 30 --retry 4 --retry-delay 2 --retry-all-errors \
        -X POST "$RPC" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}")
  rc=$?
  # kill -TERM $$ as well as exit: this is called inside pipelines and command
  # substitutions, which are subshells, so a bare `exit` would end the subshell
  # and let the caller read the failure as an empty result.
  [ "$rc" -eq 0 ] || { echo "could not reach $RPC after 5 attempts (curl exit $rc) asking $1" >&2
                       kill -TERM $$ 2>/dev/null; exit 1; }
  printf '%s' "$out"
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

# Counted, and the categories named, because the loop below is the whole section
# and A LOOP OVER NOTHING RUNS CLEAN. The two sections underneath this one both
# have a floor and this one had none — `a2a-task.tsv` needs two settlements,
# `shielded-settlement.tsv` needs one, and their comments say why. Measured on a
# copy of this tree: with `artifacts/agents.tsv` cut back to its header row, this
# section printed
#
#     policies (artifacts/agents.tsv)
#
# and nothing else, and contributed zero failures — on the manifest that is the
# only evidence for "three separate agents deployed on LEZ testnet, one per
# default skill category". The whole run was still red, but for reasons in the
# sections below it; nothing here noticed that it had checked no agent at all.
#
# The floor is the criterion's own sentence rather than a number: THREE, one per
# default skill category, and the categories are named so that three rows for one
# category cannot satisfy it. A row only counts when the chain agreed with the
# manifest about it, so this cannot be satisfied by rows that failed above.
pol_rows=0; pol_ok=0; pol_cats=""
if [ -n "$c_cat" ] && [ -n "$c_pol" ]; then
  while IFS=$'\t' read -r -a f; do
    pol_rows=$((pol_rows + 1))
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
          pol_ok=$((pol_ok + 1)); pol_cats="$pol_cats $cat"
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

# The positive statement this section exists to make, and it is a failure rather
# than a label for the same reason the two below it are: zero rows is not
# "nothing to disagree with", it is the claim having no evidence at all.
for want_cat in storage messaging blockchain; do
  case " $pol_cats " in
    *" $want_cat "*) ;;
    *) bad "no '$want_cat' agent in $AGENTS whose account the chain agrees with: the criterion asks for one agent per default skill category, and this run checked none for that one" ;;
  esac
done
if [ "$pol_ok" -ge 3 ]; then
  ok "$pol_ok of $pol_rows row(s) are agents whose anchored envelope the chain confirms ($pol_cats )"
else
  bad "only $pol_ok of $pol_rows row(s) in $AGENTS have an envelope the chain confirms, and three are needed — one per default skill category"
fi

echo
echo "settlements ($TASKS)"
#
# "It is on chain" was the whole check here, and it cannot fail in the way that
# matters. Every settlement this repository has ever produced is still on chain
# — deployment is content-addressed and nothing is ever removed — so four green
# lines were reporting that four transactions resolve, not that any of them was
# charged against the policy account of the program we ship. Two of these four
# are settlements under `a780003b…`, which `697746f5…` superseded. They moved
# real balance and they are real history; what they are not is evidence about
# the shipped binary, because the policy account they charged no longer exists.
#
# Each row is therefore attributed to a program TWICE, and the two routes must
# agree:
#
#   the manifest  — `a2a-task.tsv`'s `program` column, read BY NAME. That column
#                   was inserted at position 1 by the migration, which pushed
#                   `settlement_tx` from field 8 to field 9; a positional read
#                   now returns the nonce, confidently and silently.
#   the chain     — a privacy-preserving settlement composes the program call
#                   inside its proof and looks the callee up by ImageID, so the
#                   32-byte ImageID is present in the payload. The shipped one
#                   is not hardcoded either: it is read out of a `create_policy`
#                   payload on chain, where byte 0 is the transaction kind and
#                   bytes 1..33 are the ImageID being called.
#
# Agreement is the point. The manifest alone can be edited; the chain alone
# cannot say which program a maintainer *believes* a row belongs to. A row where
# they disagree is a misattribution and fails.
#
# Inclusion depth is deliberately NOT re-done here — `scripts/submission-evidence.py`
# already confirms each transaction against its neighbouring blocks, and two
# implementations of one check is how they come to disagree.
if [ -f "$TASKS" ]; then
  c_stx=$(col "$TASKS" settlement_tx)
  c_ba=$(col "$TASKS" balance_after)
  c_prog=$(col "$TASKS" program)
  if [ -z "$c_stx" ]; then
    bad "$TASKS has no settlement_tx column"
  elif [ -z "$c_prog" ]; then
    bad "$TASKS has no program column: a settlement that names no program cannot be told from an orphan"
  else
    # The shipped ImageID, from the chain. Any anchor of the shipped program
    # carries it, and the comment here said "the first one that resolves is
    # enough" while the code read row one and stopped. Those are different
    # claims: a first row that names an anchor under a SUPERSEDED program hands
    # this the old ImageID, and then every settlement below is attributed
    # backwards — each one labelled with confidence, none of them right, and the
    # failure text blaming the manifest. The comment is now what the loop does.
    SHIPPED_IMG=""; _IMGS=""
    if [ -n "$(col "$AGENTS" create_tx)" ]; then
      for _ctx in $(rows "$AGENTS" | awk -F'\t' -v c="$(col "$AGENTS" create_tx)" '{print $c}'); do
        [ -n "$_ctx" ] || continue
        SHIPPED_IMG=$(rpc getTransaction "[\"$_ctx\"]" | python3 -c "
import json,sys,base64
r=json.load(sys.stdin).get('result')
if not isinstance(r,list): raise SystemExit
d=base64.b64decode(r[0])
print(d[1:33].hex() if len(d)>33 and d[0]==0 else '')")
        [ -n "$SHIPPED_IMG" ] || continue
        case " $_IMGS " in *" $SHIPPED_IMG "*) ;; *) _IMGS="$_IMGS $SHIPPED_IMG" ;; esac
      done
      # And if the anchors do not agree with each other, say so instead of
      # picking one. Every settlement below is labelled SHIPPED or OTHER by
      # whether it embeds THIS value; silently taking row one's meant a manifest
      # spanning two deployments could be attributed wholesale to whichever
      # program happened to be anchored first, with every label confident and
      # every one of them possibly wrong.
      set -- $_IMGS
      if [ "$#" -gt 1 ]; then
        bad "the anchors in $AGENTS carry $# different ImageIDs ($_IMGS): this manifest spans deployments, so no single one of them can attribute the settlements below"
        SHIPPED_IMG=""
      else
        SHIPPED_IMG="${1:-}"
      fi
    fi
    if [ -z "$SHIPPED_IMG" ]; then
      bad "could not read the shipped ImageID off an anchor: settlements cannot be attributed from the chain"
    else
      echo "  shipped ImageID (read off an on-chain anchor) $SHIPPED_IMG"
    fi

    shipped=0
    while IFS=$'\t' read -r -a f; do
      stx="${f[$((c_stx-1))]}"; ba="${f[$((c_ba-1))]}"; prog="${f[$((c_prog-1))]}"
      # AN EMPTY NEEDLE IS FOUND IN EVERY HAYSTACK, and this is where that got
      # in: `bytes.fromhex('$SHIPPED_IMG')` is `b''` when the derivation above
      # failed, and `b'' in anything` is True in Python. So a run that could not
      # read the shipped ImageID off any anchor — the failure the `bad` twenty
      # lines up reports — went on to label EVERY settlement `SHIPPED` and
      # satisfy the `need 2` floor with it. Measured on a copy of this tree with
      # `artifacts/agents.tsv` emptied:
      #
      #   FAIL  could not read the shipped ImageID off an anchor: settlements
      #         cannot be attributed from the chain
      #   ok    11 settlement(s) under the shipped program (need 2)
      #
      # Eleven readings that were never taken, reported as agreement. The
      # shielded section a hundred lines below already guards exactly this with
      # `carries = bool(img) and …`; the guard is now on both of them, and here
      # it produces a THIRD answer rather than a false one — a row nobody could
      # attribute is not a row that agrees and not a row that disagrees, and it
      # does not count toward the floor either way.
      out=$(rpc getTransaction "[\"$stx\"]" | python3 -c "
import json,sys,base64
r=json.load(sys.stdin).get('result')
if not isinstance(r,list): print('ABSENT'); raise SystemExit
img='$SHIPPED_IMG'
if not img: print('%s UNATTRIBUTABLE' % r[1]); raise SystemExit
print('%s %s' % (r[1], 'SHIPPED' if bytes.fromhex(img) in base64.b64decode(r[0]) else 'OTHER'))")
      set -- $out
      if [ "${1:-}" = "ABSENT" ]; then
        bad "$stx is not on chain"
        continue
      fi
      # AND AN ANSWER THAT DID NOT COME BACK IS ITS OWN VERDICT. The decoder
      # above prints nothing at all when the RPC does not answer or the body does
      # not parse — `set --` then leaves no positional arguments and `b="$1"`
      # aborted the whole script on `$1: unbound variable`, part-way through the
      # manifest and before the shielded section had run. That failed closed, so
      # it was never wrong; it was just unreadable, and it stopped the two
      # sections below from being checked at all. Named instead, and the run
      # carries on to check everything else.
      if [ "$#" -lt 2 ]; then
        bad "$stx  the chain did not answer for this settlement, so nothing here says where it is or what it called"
        continue
      fi
      b="$1"; chain="$2"
      if [ "$chain" = UNATTRIBUTABLE ]; then
        bad "$stx  block $b  cannot be attributed: no shipped ImageID was derived (see above); this is a reading that could not be taken, not a settlement that agrees"
        continue
      fi
      # What the manifest claims, in the same vocabulary.
      if [ "$prog" = "$DEPLOY_TX" ]; then claim=SHIPPED; else claim=OTHER; fi

      if [ "$claim" != "$chain" ]; then
        bad "$stx  block $b  the manifest files it under ${prog:0:8}… but the chain says $chain: a settlement attributed to the wrong program is worse than one with no program at all"
      elif [ "$chain" = "SHIPPED" ]; then
        shipped=$((shipped + 1))
        ok "$stx  block $b  under the shipped program  (balance after $ba)"
      else
        ok "$stx  block $b  SUPERSEDED (${prog:0:8}…) — real history, not evidence for what ships"
      fi
    done < <(rows "$TASKS")

    # How many must be under the shipped program, and why this is a failure
    # rather than a label. The claim a reviewer checks is not "this chain has
    # processed settlements" — nobody doubts that — it is that the agent settles
    # repeatedly and unattended under the envelope THIS repository anchors. An
    # orphan row cannot support it: the policy account it charged does not exist
    # under the shipped program. Two, not one, because the documented claim is
    # specifically a *repeat* settlement — producing one was possible for most of
    # this repository's life; producing a second was the thing that was not.
    want="${MIN_SHIPPED_SETTLEMENTS:-2}"
    if [ "$shipped" -ge "$want" ]; then
      ok "$shipped settlement(s) under the shipped program (need $want)"
    else
      bad "only $shipped settlement(s) under the shipped program, need $want — the rest are under superseded programs and are not evidence that what this repository ships settles anything"
      # Said plainly, because the likeliest way to arrive here is a REDEPLOY and
      # not a defect: a new deployment moves the ImageID, every existing row in
      # $TASKS becomes SUPERSEDED at once, and `shipped` drops to zero on a
      # repository whose manifest is entirely correct and whose settlements are
      # all still in their blocks.
      note "if EVERY row above is SUPERSEDED, this program was redeployed. Those"
      note "settlements are unchanged and still on chain; they were made under the"
      note "previous ImageID. Make settlements under the one that ships now, or set"
      note "MIN_SHIPPED_SETTLEMENTS while that is outstanding."
    fi
  fi
else
  bad "$TASKS is missing"
fi

echo
echo "shielded settlements ($SHIELDED)"
#
# The rows where the PAYEE is shielded too. They are kept apart from
# `a2a-task.tsv` on purpose: every check that file runs on a settlement ends in
# "and the payee's public balance moved by the price", and there is no public
# balance here to move. Filing these among them would either weaken that check
# for all of them or produce rows it cannot evaluate.
#
# What a stranger can check, and what this therefore checks:
#
#   inclusion  — the transaction is in a block, and in the block the row names.
#                A row that has drifted from the chain is the failure this whole
#                script exists for.
#   program    — a row that claims the shipped program must carry the shipped
#                ImageID in its payload, read off an on-chain anchor exactly as
#                above. `builtin:` names a program LEZ itself ships, which has no
#                deploy transaction here to compare against, so that is recorded
#                rather than checked.
#
# What it CANNOT check, stated here rather than left as an absence: the AMOUNT.
# The payee is a commitment; `getAccount` answers with a default account for it.
# Reading the amount needs the payee's own viewing key and
# `tools/shielded-receipt`, which is the privacy property working, not a gap in
# the evidence. See docs/limitations.md.
if [ -f "$SHIELDED" ]; then
  c_tx=$(col "$SHIELDED" tx)
  c_blk=$(col "$SHIELDED" block)
  c_prog=$(col "$SHIELDED" program)
  c_note=$(col "$SHIELDED" note_account)
  c_role=$(col "$SHIELDED" role)
  if [ -z "$c_tx" ] || [ -z "$c_blk" ] || [ -z "$c_prog" ] || [ -z "$c_note" ] \
     || [ -z "$c_role" ]; then
    bad "$SHIELDED is missing one of the role/tx/block/program/note_account columns"
  else
    # Counted, because the loop below is the whole section and a loop over
    # nothing runs clean. Measured: truncating this manifest to its header left
    # this script exit 0, printing the section heading with nothing under it and
    # then "DEPLOYMENT.md agrees with artifacts/ and with the chain." — the
    # a2a-task.tsv section directly above has had a floor (`need 2`) for exactly
    # this reason and this one had none, on the manifest that is the ONLY
    # evidence for "an agent can be paid at its shielded account".
    sh_rows=0; sh_settlements=0
    while IFS=$'\t' read -r -a f; do
      sh_rows=$((sh_rows + 1))
      role="${f[$((c_role-1))]}"
      stx="${f[$((c_tx-1))]}"; want_b="${f[$((c_blk-1))]}"
      prog="${f[$((c_prog-1))]}"; note="${f[$((c_note-1))]}"
      out=$(rpc getTransaction "[\"$stx\"]" | python3 -c "
import json,sys,base64
r=json.load(sys.stdin).get('result')
if not isinstance(r,list): print('ABSENT'); raise SystemExit
img='${SHIPPED_IMG:-}'
carries = bool(img) and bytes.fromhex(img) in base64.b64decode(r[0])
print('%s %s' % (r[1], 'SHIPPED' if carries else 'OTHER'))")
      set -- $out
      if [ "${1:-}" = "ABSENT" ]; then
        bad "$stx is not on chain"
        continue
      fi
      # Same as the settlements loop above: no answer is a verdict of its own,
      # not an unbound variable that ends the run.
      if [ "$#" -lt 2 ]; then
        bad "$stx  the chain did not answer for this settlement, so nothing here says which block holds it"
        continue
      fi
      b="$1"; chain="$2"
      if [ "$b" != "$want_b" ]; then
        bad "$stx  the manifest says block $want_b, the chain says $b"
        continue
      fi
      case "$prog" in
        builtin:*)
          ok "$stx  block $b  ${prog#builtin:} — a program LEZ ships, no local binary to attribute against" ;;
        "$DEPLOY_TX")
          if [ "$chain" = SHIPPED ]; then
            [ "$role" = settlement ] && sh_settlements=$((sh_settlements + 1))
            ok "$stx  block $b  under the shipped program, paying Private/$note"
          elif [ -z "${SHIPPED_IMG:-}" ]; then
            # There is no ImageID to attribute against, so this row cannot be
            # judged either way. `SHIPPED_IMG` is derived in the settlements
            # section above, and is empty when artifacts/a2a-task.tsv is missing
            # or when its anchors disagree — both of which are already reported
            # there. Without this branch every shielded row was labelled "the
            # payload does not carry that ImageID", which is a verdict about the
            # transaction and blames the wrong file for a reading that was never
            # taken.
            bad "$stx  block $b  cannot be attributed: no shipped ImageID was derived (see above); this is a reading that could not be taken, not a disagreement"
          else
            bad "$stx  block $b  the manifest files it under the shipped program but the payload does not carry that ImageID"
          fi ;;
        *)
          bad "$stx  names program ${prog:0:8}…, which is neither the shipped one nor a builtin" ;;
      esac
    done < <(rows "$SHIELDED")

    # The positive statement, and the reason it is a failure rather than a
    # label. What this manifest is cited for — README §7, docs/DEPLOYMENT.md,
    # SUBMISSION-DRAFT.md — is that the SHIPPED program paid an agent at its
    # shielded account. A `builtin:` row is the money coming back out through
    # LEZ's own transfer program and cannot support that sentence, so it is
    # counted separately and does not satisfy this. Zero rows is not "nothing
    # to disagree with"; it is the claim having no evidence at all.
    if [ "$sh_settlements" -ge 1 ]; then
      ok "$sh_settlements of $sh_rows row(s) are settlements to a shielded payee under the shipped program"
    else
      bad "$SHIELDED holds $sh_rows row(s) and not one of them is a settlement under the shipped program: the shielded-payee claim has no evidence in it"
    fi
  fi
else
  bad "$SHIELDED is missing: the claim that an agent can be paid at its shielded account has no manifest"
fi

# --- the above-threshold path, which nothing re-checked --------------------
#
# `artifacts/approved-spend.tsv` records the one demonstration that the
# above-threshold branch EXECUTES: an owner claimed, a policy anchored, an
# `approve_spend` signed by that same owner as its SECOND program transaction,
# and a `spend_approved` that moved money. It was the strongest new artifact in
# the repository and the only one nothing re-read -- `verify-deployment.sh`
# checked agents.tsv, a2a-task.tsv and shielded-settlement.tsv and not this one.
# An artifact no gate reads is a hand-written claim with extra columns.
APPROVED=artifacts/approved-spend.tsv
echo
echo "the above-threshold path (${APPROVED})"
if [ ! -f "$APPROVED" ]; then
  bad "$APPROVED is missing: the claim that an approved spend executes has no manifest"
else
  a_rows=$(rows "$APPROVED" | wc -l | tr -d " ")
  c_tx=$(col "$APPROVED" tx); c_blk=$(col "$APPROVED" block)
  c_what=$(col "$APPROVED" what); c_out=$(col "$APPROVED" outcome)
  if [ -z "$c_tx" ] || [ -z "$c_blk" ] || [ -z "$c_what" ] || [ -z "$c_out" ]; then
    bad "$APPROVED has no tx/block/what/outcome column: read by header name, and a header moved"
  else
    a_confirmed=0; a_approve=0; a_spend=0
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      tx=$(printf "%s" "$line" | cut -f"$c_tx"); blk=$(printf "%s" "$line" | cut -f"$c_blk")
      what=$(printf "%s" "$line" | cut -f"$c_what"); out=$(printf "%s" "$line" | cut -f"$c_out")
      case "$tx" in
        *[!0-9a-f]*|"") continue ;;   # a step that submitted nothing, by design
      esac
      # The block the manifest claims, checked against the block the chain
      # names -- not merely "getTransaction is non-null", which this chain
      # answers identically for four different situations.
      got=$(rpc getTransaction "[\"$tx\"]" \
            | python3 -c "import json,sys; r=json.load(sys.stdin).get('result'); print(r[1] if r else '')" 2>/dev/null)
      if [ -z "$got" ]; then
        case "$out" in
          *never*included*|*not*included*|*frozen*)
            ok "  ${what}: not on chain, which is what this row records" ;;
          *) bad "  ${what}: $APPROVED says block $blk and the chain does not hold ${tx}" ;;
        esac
      elif [ "$got" != "$blk" ]; then
        bad "  ${what}: $APPROVED says block $blk, the chain says $got"
      else
        a_confirmed=$((a_confirmed + 1))
        case "$what" in *approve_spend*) a_approve=1 ;; esac
        case "$what" in *spend_approved*) a_spend=1 ;; esac
      fi
    done <<EOF
$(rows "$APPROVED")
EOF
    ok "$a_confirmed of $a_rows recorded step(s) are in the block this manifest names"
    # The two that carry the claim. Without both, the file is a deployment
    # transcript and not a demonstration that the above-threshold path runs.
    [ "$a_approve" -eq 1 ] \
      && ok "an approve_spend is among them, signed after the same owner had already anchored" \
      || bad "no confirmed approve_spend in $APPROVED: the owner half of the above-threshold path is unevidenced"
    [ "$a_spend" -eq 1 ] \
      && ok "and a spend_approved: the payment above the ceiling actually executed" \
      || bad "no confirmed spend_approved in $APPROVED: the approval was never redeemed"
  fi
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "DEPLOYMENT.md agrees with artifacts/ and with the chain."
  exit 0
fi
echo "$fail check(s) failed: docs/DEPLOYMENT.md no longer describes what this repository ships." >&2
exit 1
