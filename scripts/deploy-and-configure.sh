#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Deploy the agent on LEZ and configure it in Logos Core headless. One command.
#
#   SIGNER=<funded public account id> ./scripts/deploy-and-configure.sh [category]
#
# WHAT THIS IS, AND WHAT IT REPLACES
#
# The prize asks that "the owner can deploy the agent and configure it with a
# single CLI command on any machine using Logos Core headless". That sentence
# has two halves and this repository answered them with two commands:
#
#   scripts/deploy-agents.sh        the agent's identity, its funding and its
#                                   spending envelope, ON CHAIN
#   scripts/logos-core-headless.sh  the packaged module installed into Logos
#                                   Core, loaded, configured and started, IN THE
#                                   RUNTIME
#
# docs/limitations.md used to argue that a wrapper over the two "would report one
# exit code for two unrelated failures — which hides the gap rather than closing
# it". That is an argument against a wrapper that merely concatenates them, and
# it is correct about that wrapper. It is not an argument against this one. The
# two failures are not unrelated: the second half's prerequisites are knowable
# BEFORE the first half spends anything, and the first half's output is the
# second half's input. So the seam is the whole design and it has two rules:
#
#   1. NOTHING IS SUBMITTED UNTIL BOTH HALVES SAY THEY CAN RUN. Anchoring is not
#      idempotent and it is not free. `claim_agent` and `create_policy` are both
#      declared #[account(init)]; a claim, once landed, cannot be rewritten, and
#      a funding transfer that has left cannot be recalled. A machine that finds
#      out it has no `liblogos_core` after three agents are anchored has paid
#      real transactions to learn something a `[ -e ]` could have told it. So
#      this asks the Logos Core half whether it can run — `logos-core-headless.sh
#      --check`, which is that script's own prerequisite gate and not a second
#      copy of it — before the chain half is started at all.
#
#   2. THE SECOND HALF IS HANDED THE FILE THE FIRST HALF WROTE, NOT VALUES
#      TYPED IN AGAIN. `deploy-agents.sh` records the agent id, the owner and
#      the policy account in `artifacts/agents.tsv`; `logos-core-headless.sh`
#      reads those three BY HEADER NAME out of the same file. This command
#      exports one `MANIFEST` path so that both halves are certainly talking
#      about the same file, and then verifies, between them, that the row it is
#      about to configure with is a row the chain agrees with. It passes no
#      owner and no policy account on any command line: a value passed by hand
#      between two commands is a value that can be typed correctly and be stale.
#
# WHAT IT DOES, IN ORDER
#
#   0. reads the categories and the funding floors OUT OF deploy-agents.sh, so
#      the arithmetic below is that script's numbers rather than a copy of them
#   1. checks every prerequisite of BOTH halves, and names each missing one
#   2. runs the chain half:      SIGNER=… ./scripts/deploy-agents.sh
#   3. reads the seam:           the row for <category> in artifacts/agents.tsv,
#      by header name, written by step 2, and checked against the chain — the
#      first 73 bytes of the policy account must be the record the row claims
#   4. runs the Logos Core half: ./scripts/logos-core-headless.sh <category>
#
# THE HONEST CLAIM, WHICH IS NOT "ON A BARE MACHINE"
#
# This is one command GIVEN THREE INPUTS THAT CANNOT BE SCRIPTED FROM HERE:
#
#   * a funded public account id in $SIGNER. Balance comes from a faucet; a
#     script cannot mint it. This is the one prerequisite in the list that is a
#     testnet's policy rather than a package's.
#   * a wallet home holding that account's key (LEE_WALLET_HOME_DIR).
#   * a Logos Core runtime. On Linux `scripts/fetch-logos-core.sh` fetches one
#     in a checksum-pinned command; on macOS it ships inside LogosBasecamp.app
#     and there is no headless build published.
#
# Everything else it does itself. Each of the three is checked by name here,
# before anything is spent, and docs/limitations.md carries the same list as
# prose. `--dry-run` runs the checks and stops, so a machine can be told whether
# it is one of the machines this works on without finding out by spending.
#
# WHAT IT DOES NOT DO
#
# It does not deploy ONE agent. `deploy-agents.sh` deploys three — one per
# default skill category, which is what the prize asks for — and this command
# configures ONE of them in Logos Core, the one named by <category>. Both facts
# are printed. Configuring a second is `logos-core-headless.sh <other-category>`,
# or this command again with `--configure-only`, and neither costs a
# transaction.
#
# FLAGS
#
#   --dry-run          check both halves, submit nothing, install nothing, start
#                      nothing. Prints exactly what it would do.
#   --configure-only   skip the chain half: the agents in the manifest are
#                      already anchored. The seam is still verified against the
#                      chain, so this cannot configure the module with a row
#                      that no longer matches what the chain holds. This is the
#                      mode for the agents THIS REPOSITORY publishes, whose
#                      manifest is committed.
#   --alongside        passed to the Logos Core half: load the agent beside the
#                      wallet, storage and messaging modules.
#
# EXIT CODES, AND WHY THERE ARE THREE
#
#   0  both halves ran
#   1  a prerequisite was missing, or the chain half failed. In this case the
#      chain half's own reporting stands: it leaves `artifacts/agents.tsv`
#      untouched unless every agent anchored.
#   2  THE CHAIN HALF SUCCEEDED AND THE LOGOS CORE HALF DID NOT. This is the
#      state worth its own code, because it is the only one that costs money:
#      agents are anchored and nothing is configured. Re-running with
#      `--configure-only` is free — anchoring is recorded in
#      `artifacts/anchored.tsv` and never repeated — and the message says so.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }

DRY_RUN=0
CONFIGURE_ONLY=0
ALONGSIDE=0
args=()
for a in "$@"; do
  case "$a" in
    --dry-run|-n)     DRY_RUN=1 ;;
    --configure-only) CONFIGURE_ONLY=1 ;;
    --alongside)      ALONGSIDE=1 ;;
    -h|--help)
      # Written out rather than sliced out of the header with line numbers: a
      # `sed -n '3,6p'` into this file is a citation that silently starts
      # printing the wrong paragraph the first time a comment above it grows.
      cat <<'USAGE'
Deploy the agent on LEZ and configure it in Logos Core headless. One command.

  SIGNER=<funded public account id> ./scripts/deploy-and-configure.sh [category]

  category           storage, messaging or blockchain; storage by default. The
                     chain half deploys all three, and this is the one that is
                     configured in the runtime.

  --dry-run, -n      check every prerequisite of both halves and stop. Nothing
                     is submitted, installed or started.
  --configure-only   skip the chain half: the agents in the manifest are already
                     anchored. The row is still checked against the chain.
  --alongside        load the agent beside the wallet, storage and messaging
                     modules in the same runtime.

Exit codes: 0 both halves ran. 1 a prerequisite was missing or the chain half
failed, and nothing was configured. 2 the chain half succeeded and the Logos
Core half did not — agents are anchored and the configuration is unfinished;
`--configure-only` finishes it and costs no transaction.

The header of this file explains the seam and why the order is what it is.
docs/limitations.md lists every prerequisite as prose.
USAGE
      exit 0 ;;
    -*) echo "unknown option: $a  (try --help)" >&2; exit 1 ;;
    *) args+=("$a") ;;
  esac
done
set -- ${args[@]+"${args[@]}"}

CATEGORY="${1:-${AGENT_CATEGORY:-storage}}"

# The seam, named once and exported, so that neither half can be reading a
# different file from the one the other wrote. Both scripts default to this
# path; exporting it means that setting MANIFEST moves BOTH of them together,
# which is the property a wrapper has to have and a shell prompt does not.
MANIFEST="${MANIFEST:-artifacts/agents.tsv}"
export MANIFEST

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
WALLET="${WALLET_BIN:-wallet}"
SPEL="${SPEL_BIN:-spel}"
SIGNER_HOME="${LEE_WALLET_HOME_DIR:-$HOME/.lez-wallet}"
AGENT_HOMES="${AGENT_HOMES:-$HOME/.lp0008-agents}"
IDL=idl/agent_verifier.idl.json
PROGRAM=artifacts/programs/agent_verifier.bin
DEPLOY=scripts/deploy-agents.sh
HEADLESS=scripts/logos-core-headless.sh

die() { echo "$*" >&2; exit 1; }
rule() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# ── 0. the categories and the funding floors, read out of deploy-agents.sh ──
#
# DERIVED, NOT COPIED. This wrapper has to know two things the other script
# already decides: which categories it deploys, and how much each agent must
# hold. Writing them down here would make a fourth category, or a changed floor,
# a silent disagreement between two files — and the disagreement would surface
# as this command approving a balance that is not enough, which is a check that
# passes and should not have. So they are parsed off the `deploy_agent` lines
# themselves, and a parse that finds nothing is a failure rather than a skip.
FLOORS=$(awk '$1=="deploy_agent" && NF>=6 {print $2"\t"$6}' "$DEPLOY")
[ -n "$FLOORS" ] || die "could not read the categories and funding floors out of
$DEPLOY. They are the arguments of its own \`deploy_agent\` lines; if those were
renamed or reshaped, fix this reader rather than letting it approve a balance
against a floor of nothing."
CATEGORIES=$(printf '%s\n' "$FLOORS" | cut -f1 | tr '\n' ' ' | sed 's/ *$//')

# A herestring rather than `printf | grep -q`, for the reason spelled out on
# `home_holds` below: `grep -q` stops at the first match, and under this script's
# own `set -o pipefail` a writer that has not finished turns a match into a
# non-zero pipeline. The string here is short enough that it has never bitten,
# which is exactly how the same construct survived in deploy-agents.sh.
grep -qw "$CATEGORY" <<<"$CATEGORIES" || die "
'$CATEGORY' is not one of the categories $DEPLOY deploys.
It deploys: $CATEGORIES
Those are the prize's three default skill categories, one agent each."

echo "agent     $CATEGORY"
echo "manifest  $MANIFEST  (written by the chain half, read by the Logos Core half)"
echo "chain     $RPC"
if [ "$CONFIGURE_ONLY" -eq 1 ]; then
  echo "mode      --configure-only: the chain half is not run"
elif [ "$DRY_RUN" -eq 1 ]; then
  echo "mode      --dry-run: nothing is submitted, installed or started"
else
  echo "mode      deploy $CATEGORIES on chain, then configure $CATEGORY in Logos Core"
fi

# ── 1. the prerequisites of both halves, before either runs ────────────────
rule "1. prerequisites, both halves, before anything is submitted"
missing=0
need() { # path description how-to-get-it
  if [ ! -e "$1" ]; then
    echo "missing: $2" >&2
    echo "         expected at $1" >&2
    echo "         $3" >&2
    missing=$((missing + 1))
  fi
}
need_cmd() { # command description how-to-get-it
  if ! command -v "$1" >/dev/null 2>&1 && [ ! -x "$1" ]; then
    echo "missing: $2" >&2
    echo "         no '$1' on PATH, and it is not an executable path" >&2
    echo "         $3" >&2
    missing=$((missing + 1))
  fi
}
fail() { # description remedy
  echo "missing: $1" >&2
  echo "         $2" >&2
  missing=$((missing + 1))
}

wallet_in() { # home args... -> the wallet, pointed at one home, quietly
  local home="$1"; shift
  LEE_WALLET_HOME_DIR="$home" NSSA_WALLET_HOME_DIR="$home" \
    "$WALLET" "$@" </dev/null 2>/dev/null
}

# Does a wallet home hold the key for a public account id?
#
# NOT `wallet account list | grep -q`, and the reason is this script's own
# `set -o pipefail`. `grep -q` exits the instant it matches; the wallet is then
# still writing, takes SIGPIPE, and exits 101 — and pipefail makes the PIPELINE
# 101, which reads as "no such key" for a key the home is holding. It is
# order-dependent, which is why it can hide for a long time: a match on the last
# line of `account list` never triggers it, and a home whose first accounts were
# created before the interesting one always does. Measured here, against the
# wallet home this repository's own deployment was signed from — 28 accounts,
# the funder on line 13:
#
#   $ wallet account list | grep -qE "Public/DumJ4LCB…"   # pipefail on
#   rc=101                                                # and the key is there
#
# So the list is captured first and matched with a herestring, which is not a
# pipeline and cannot be broken by a reader that stops early.
home_holds() { # home account-id
  local list
  list=$(wallet_in "$1" account list)
  grep -qE "Public/$2([[:space:]]|$)" <<<"$list"
}

# The largest balance any private account in one agent home holds — which is the
# figure `fund_agent` decides on, so it is read the way `fund_agent` reads it
# (`best_funded` in deploy-agents.sh). A home that does not exist holds nothing,
# and says 0 rather than an empty string that arithmetic would treat as a syntax
# error several lines later.
best_private() { # home -> balance
  local home="$1" best=0 a b
  [ -d "$home" ] || { echo 0; return 0; }
  for a in $(wallet_in "$home" account list | grep -oE 'Private/[1-9A-HJ-NP-Za-km-z]+'); do
    b=$(wallet_in "$home" account get --account-id "$a" \
        | grep -o '"balance":[0-9]*' | cut -d: -f2 | head -1)
    [ -n "$b" ] && [ "$b" -gt "$best" ] && best="$b"
  done
  echo "$best"
}

need_cmd python3 "python3" \
     "every account id in this repository is decoded with it; install python3."
need_cmd curl "curl" \
     "the sequencer is reached over JSON-RPC; install curl."

if [ "$CONFIGURE_ONLY" -eq 0 ]; then
  # ── the chain half ──
  if [ -z "${SIGNER:-}" ]; then
    fail "SIGNER, the funded public account this deployment is paid from" \
         "export SIGNER=<public account id>. It is the FUNDER and the account
         that deploys the program — not the owner of any agent: $DEPLOY
         provisions one anchoring signer per agent in your own wallet home and
         records it in the manifest's 'owner' column."
  fi
  need_cmd "$WALLET" "the LEZ wallet binary" \
       "build it from LEZ at the revision docs/DEPLOYMENT.md pins and put it on
         PATH, or set WALLET_BIN. A wallet from another revision refuses a home
         it did not create, with \`missing field 'accounts'\` — which reads like
         a corrupted home and is a version mismatch."
  need_cmd "$SPEL" "spel" \
       "build it from vendor/spel and set SPEL_BIN, e.g.
         SPEL_BIN=\$PWD/vendor/spel/target/release/spel"
  need "$SIGNER_HOME" "a LEZ wallet home" \
       "set LEE_WALLET_HOME_DIR. It must hold SIGNER's key, and it is also where
         $DEPLOY creates the per-agent anchoring signers — keep it, those keys
         are what may later call update_policy and approve_spend."
  need "$IDL" "the program's IDL" "it is committed; you are not in a full checkout."
  need "$PROGRAM" "the compiled program" "it is committed; you are not in a full checkout."
fi

if [ "$missing" -eq 0 ] && [ "$CONFIGURE_ONLY" -eq 0 ]; then
  # These four cost a wallet call or an RPC round trip each, so they run only
  # once the cheap ones above have passed — there is nothing to ask the chain
  # about while `curl` is missing.

  # (a) THE KEY, and this is the check whose absence used to cost six minutes.
  # `auth-transfer send --from Public/$SIGNER` is signed out of $SIGNER_HOME. A
  # home that does not hold that key does not error: the transfer simply never
  # lands, `fund_agent` polls for sixty rounds, and the run reports "FAILED to
  # fund the agent" — after `wallet deploy-program` has already been submitted.
  if ! home_holds "$SIGNER_HOME" "${SIGNER:-nothing}"; then
    fail "SIGNER's key: $SIGNER_HOME holds no key for ${SIGNER:-<unset>}" \
         "point LEE_WALLET_HOME_DIR at the wallet home that holds it, or set
         SIGNER to an account this one does. \`$WALLET account list\` prints the
         ids this home has. Nothing here can be signed without it, and the
         funding step's way of saying so is a six-minute timeout per agent."
  fi

  # (b) AND EACH OVERRIDDEN ANCHORING SIGNER, ALL OF THEM, NOW.
  # $DEPLOY makes exactly this check — but per agent, as it reaches that agent.
  # So SIGNER_STORAGE naming a key you hold and SIGNER_MESSAGING naming one you
  # do not is a run that deploys the storage agent, spends on it, and then
  # refuses the second. Hoisting it here is the difference between a refusal and
  # a partial deployment.
  for cat in $CATEGORIES; do
    var="SIGNER_$(printf '%s' "$cat" | tr '[:lower:]' '[:upper:]')"
    id="${!var:-}"
    [ -n "$id" ] || continue
    if ! home_holds "$SIGNER_HOME" "$id"; then
      fail "$var=$id, and $SIGNER_HOME holds no key for it" \
           "unset $var and let $DEPLOY create the account, or point
         LEE_WALLET_HOME_DIR at the home that holds that key. claim_agent is
         signed by the AGENT and lands whatever owner it is given, so a missing
         owner key does not stop the claim — it makes the agent permanently
         unanchorable."
    fi
  done

  # (c) THE SEQUENCER, asked with the same call that answers (d). A body with no
  # "balance" in it is not a poor balance, it is no answer: an unreachable host,
  # a proxy's HTML, an error object. Those must not read as zero.
  BODY=$(curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
           -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"${SIGNER:-1}\"]}" 2>/dev/null)
  BALANCE=$(printf '%s' "$BODY" | python3 -c "
import json,sys
try:
    r = json.load(sys.stdin).get('result')
except Exception:
    r = None
print(r['balance'] if isinstance(r, dict) and 'balance' in r else '')" 2>/dev/null)
  if [ -z "$BALANCE" ]; then
    fail "an answer from the sequencer at $RPC" \
         "set SEQUENCER_URL, or check the network. getAccount for SIGNER came
         back with no balance field at all, which is silence and not a zero
         balance. $DEPLOY waits twelve minutes on the program-deploy step before
         it will call an unreachable chain dead, so an offline machine looks
         like a hang rather than an error — this is that error."
  else
    # (d) THE BALANCE, against what will actually be TRANSFERRED rather than
    # against the sum of the floors.
    #
    # The floors are floors: `fund_agent` sends nothing to an agent that already
    # holds its floor, and on a re-anchor it must not — a shielded transfer does
    # not credit an existing note, it mints a new one under a new account id, and
    # an agent whose id moves after it has claimed has both of its accounts at
    # addresses nothing will look at again. So the number to compare a balance
    # against is the SHORTFALL, and comparing against 65 instead would refuse a
    # perfectly good re-run on a funder that has spent down since.
    #
    # Read exactly the way `fund_agent` reads it, and deliberately in the
    # optimistic direction: the local view first, and a `sync-private` only for a
    # home that looks short. A reading that comes out too HIGH degrades to the
    # behaviour this command already has — $DEPLOY syncs, finds the agent short,
    # and transfers — while one that comes out too low would refuse a machine
    # that works.
    shortfall=0
    while IFS=$'\t' read -r cat floor; do
      [ -n "$cat" ] || continue
      home="$AGENT_HOMES/$cat"
      have=$(best_private "$home")
      if [ "${have:-0}" -lt "$floor" ]; then
        # Only now, and only for a home that looks short. `sync-private` is ten
        # seconds against this sequencer, and a home already reading above its
        # floor has no answer a sync could change that matters here.
        wallet_in "$home" account sync-private >/dev/null
        have=$(best_private "$home")
      fi
      have="${have:-0}"
      if [ "$have" -lt "$floor" ]; then
        shortfall=$((shortfall + floor - have))
        printf '  %-12s holds %s of the %s it must, so %s would be transferred\n' \
          "$cat" "$have" "$floor" "$((floor - have))"
      else
        printf '  %-12s holds %s of the %s it must, so nothing is transferred\n' \
          "$cat" "$have" "$floor"
      fi
    done <<EOF
$FLOORS
EOF
    echo "  funder       $SIGNER holds $BALANCE"
    if [ "$shortfall" -gt 0 ] && [ "$BALANCE" -lt "$shortfall" ]; then
      fail "testnet balance on SIGNER: it holds $BALANCE and $shortfall has to be transferred" \
           "THIS IS THE ONE PREREQUISITE THAT CANNOT BE SCRIPTED. Balance comes
         from the faucet for this testnet; no command in this repository can
         mint it. The figure is not the sum of the floors — it is what the
         agents are still short of them, so funding an agent directly reduces
         it. With too little, $DEPLOY reports 'FAILED to fund the agent' after
         polling for six minutes per agent."
    elif [ "$shortfall" -eq 0 ]; then
      echo "  every agent already holds its floor: no funding transfer is needed"
    fi
  fi
fi

# ── the Logos Core half, asked rather than assumed ─────────────────────────
#
# By RUNNING it, with --check. The platform table this needs — where
# liblogos_core lives on macOS and on each Linux architecture, which harness
# variant this machine takes, what the package must contain — is ninety lines of
# $HEADLESS, and a second copy of it here is a second copy to drift. So the
# question is put to the script that owns the answer, and its own words are what
# a reader sees.
rule "2. the Logos Core half, asked before the chain half spends anything"
core_args=("$CATEGORY" --check)
[ "$ALONGSIDE" -eq 1 ] && core_args+=(--alongside)
core_out=$("./$HEADLESS" "${core_args[@]}" 2>&1); core_rc=$?
printf '%s\n' "$core_out" | sed 's/^/  /'
if [ "$core_rc" -ne 0 ]; then
  missing=$((missing + 1))
  echo >&2
  echo "the Logos Core half cannot run on this machine (above, exit $core_rc)." >&2
  echo "Nothing has been submitted. That is the point of asking first: this" >&2
  echo "command will not anchor three agents on chain and then discover it has" >&2
  echo "nowhere to configure them." >&2
fi

[ "$missing" -eq 0 ] || die "
$missing prerequisite(s) missing; nothing was submitted, installed or started.
docs/limitations.md, 'One command, and what it needs before it will run', lists
every one of them as prose and says which can be fetched and which cannot."

echo
echo "every prerequisite of both halves is present on this machine"

# ── the seam reader, defined once and used twice ───────────────────────────
#
# The same by-name read $HEADLESS makes, with the same control. Reading the
# manifest positionally is what this repository has been bitten by twice — the
# file has gained three columns — and a reader that answers for a column that
# cannot exist is not reading names at all, so it is asked for one.
col() { # file header row-key -> value; exit 3 if there is no such header
  awk -F'\t' -v w="$2" -v k="$3" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==w) c=i
            if (!c) exit 3
            next }
    $1==k { print $c; exit }' "$1"
}

# Read the row for $CATEGORY, and CHECK IT AGAINST THE CHAIN rather than merely
# read it. Used by `--dry-run`, where it is a free read that tells an operator
# the row is good before they spend anything, and by the seam proper after the
# chain half has run. It sets OWNER, POLICY and AGENT_ID for the caller, and
# dies rather than returning a value nobody looked at.
read_and_check_row() {
  if col "$MANIFEST" no_such_column "$CATEGORY" >/dev/null 2>&1; then
    die "$MANIFEST answered for a column called 'no_such_column', so the by-name
read of the owner and the policy account is not reading names and nothing it
returns can be trusted."
  fi
  AGENT_ID=$(col "$MANIFEST" agent_id "$CATEGORY")     || die "$MANIFEST has no 'agent_id' column"
  OWNER=$(col "$MANIFEST" owner "$CATEGORY")           || die "$MANIFEST has no 'owner' column"
  POLICY=$(col "$MANIFEST" policy_account "$CATEGORY") || die "$MANIFEST has no 'policy_account' column"
  local record; record=$(col "$MANIFEST" record_prefix "$CATEGORY") \
                         || die "$MANIFEST has no 'record_prefix' column"
  local pair
  for pair in "agent_id:$AGENT_ID" "owner:$OWNER" "policy_account:$POLICY" \
              "record_prefix:$record"; do
    [ -n "${pair#*:}" ] || die "
$MANIFEST has no usable '${pair%%:*}' for '$CATEGORY'.
It has rows for: $(awk -F'\t' 'NR>1{printf "%s ", $1}' "$MANIFEST")"
  done
  echo "  agent   $AGENT_ID"
  echo "  owner   $OWNER"
  echo "  policy  $POLICY"

  # `record_prefix` is the first 73 bytes of the policy account's data as the
  # chain holds them — version, owner, per_tx, per_period, period_blocks, which
  # is the whole immutable part of the record; everything after byte 73 is the
  # running total, which moves. So one getAccount decides whether the row this
  # command is about to configure the module with is the envelope that is
  # actually anchored. An account nothing has written has no data at all, so
  # this comes back empty and the comparison fails — which is the behaviour to
  # want: unknown must not read as agreeing, and a chain that will not answer
  # must not read as agreeing either.
  local got
  got=$(curl -s -m 25 -X POST "$RPC" -H 'Content-Type: application/json' \
          -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$POLICY\"]}" \
        | python3 -c "
import json,sys
try:
    d = bytes(json.load(sys.stdin)['result']['data'])
except Exception:
    d = b''
print(d[:73].hex() if len(d) >= 73 else '')")
  if [ "$got" != "$record" ]; then
    die "
the policy account does not hold the record $MANIFEST claims for '$CATEGORY':
  account  $POLICY
  manifest $record
  on chain ${got:-<no data: the account has never been written, or the chain did not answer>}
Refusing to configure the module with an envelope the chain does not agree with."
  fi
  echo "  record  $record"
  echo "          read off chain and matched byte for byte, so the owner and policy"
  echo "          account are the ones anchored and not merely the ones written"
}

if [ "$DRY_RUN" -eq 1 ]; then
  rule "--dry-run: this is where it stops"
  if [ "$CONFIGURE_ONLY" -eq 0 ]; then
    echo "  would run  SIGNER=$SIGNER $DEPLOY"
    echo "             deploying $CATEGORIES, writing $MANIFEST"
  fi
  echo "  would run  $HEADLESS $CATEGORY$([ "$ALONGSIDE" -eq 1 ] && echo ' --alongside')"
  echo "             reading the agent, its owner and its policy account by header"
  echo "             name out of $MANIFEST"
  # And if there is already a manifest, the row is CHECKED here rather than
  # described — it costs one getAccount and nothing else, and it is the whole
  # question `--configure-only` turns on. With no manifest yet there is nothing
  # to check and the chain half is what produces it.
  echo
  if [ -f "$MANIFEST" ]; then
    echo "  the row already in $MANIFEST, checked against the chain:"
    read_and_check_row
  else
    echo "  $MANIFEST is not here yet; the chain half writes it"
  fi
  echo
  echo "Nothing was submitted, installed or started."
  exit 0
fi

# ── 2. the chain half ──────────────────────────────────────────────────────
#
# WHY THIS RUN IS WATCHED RATHER THAN JUST WAITED ON.
#
# `artifacts/agents.tsv` is COMMITTED in this repository — it is the evidence
# for the agents already published — and that is exactly what makes the seam
# dangerous. A chain half that did not produce a manifest leaves the committed
# one in place, and a wrapper that merely carried on would install and configure
# the module against somebody else's agents, print an owner and a policy account
# that are genuinely on chain, and exit 0. Every individual assertion would hold.
# The run would still have configured an agent this operator does not own.
#
# So two things are kept about this run, and the seam below needs both:
#
#   a STAMP, taken before it starts, against which the manifest's mtime is
#   compared with python3 rather than with `[ -nt ]` — bash 3.2, which is what
#   macOS ships and what this was first written against, compares whole seconds,
#   and a stub chain half that wrote the manifest in the same second as the
#   stamp was refused by a guard that was right about nothing. Measured: four
#   controls in a row, every one of them reporting "older than this run" about a
#   file written a moment earlier.
#
#   its OUTPUT, teed, because the strongest evidence that the manifest belongs
#   to this run is not a timestamp at all — it is that $DEPLOY said so. It
#   prints `manifest: <path>` as its last act, after the `mv` that moves a fully
#   anchored manifest over the real one, and it reaches that line on no other
#   path. stdout is teed and stderr is left alone, so the operator still sees a
#   twenty-minute deployment as it happens.
STAMP=$(mktemp "${TMPDIR:-/tmp}/lp0008-stamp.XXXXXX")
DEPLOY_LOG=$(mktemp "${TMPDIR:-/tmp}/lp0008-deploy.XXXXXX")
trap 'rm -f "$STAMP" "$DEPLOY_LOG"' EXIT

# mtime(a) >= mtime(b), to whatever precision the filesystem keeps. `>=` and not
# `>`: two writes in the same nanosecond are not a thing to be strict about, and
# the file this is guarding against is minutes or days old.
not_older() { # a b
  python3 -c "
import os, sys
sys.exit(0 if os.path.getmtime(sys.argv[1]) >= os.path.getmtime(sys.argv[2]) else 1)" \
    "$1" "$2"
}

if [ "$CONFIGURE_ONLY" -eq 0 ]; then
  rule "3. the chain half: identity, funding and the spending envelope"
  SIGNER="$SIGNER" "./$DEPLOY" | tee "$DEPLOY_LOG"
  drc=${PIPESTATUS[0]}
  if [ "$drc" -ne 0 ]; then
    echo >&2
    echo "the chain half exited $drc, so the Logos Core half is NOT run." >&2
    echo "$MANIFEST is left as it was — $DEPLOY moves a manifest into place only" >&2
    echo "when every agent has anchored — and configuring the module against the" >&2
    echo "row that was already in it would be configuring it for an agent this" >&2
    echo "run did not deploy." >&2
    echo >&2
    echo "What that run did land is recorded in artifacts/anchored.tsv, keyed by" >&2
    echo "(program, instruction, agent), and re-running this command resumes from" >&2
    echo "there rather than paying for it again." >&2
    exit 1
  fi
else
  rule "3. the chain half is not run (--configure-only)"
  echo "  the agents in $MANIFEST are taken as already anchored, and the row"
  echo "  below is checked against the chain before anything is configured"
fi

# ── 3. the seam ────────────────────────────────────────────────────────────
rule "4. the seam: the row the chain half wrote, checked against the chain"

[ -f "$MANIFEST" ] || die "$MANIFEST does not exist even though the chain half
reported success. That is not a state either half can produce; something removed
it between the two."

if [ "$CONFIGURE_ONLY" -eq 0 ]; then
  # The producer's own word for it, first. $DEPLOY prints this line after the
  # `mv` and on no other path, so its absence means the run exited 0 without
  # putting a manifest in place — which nothing in that script does, and which
  # is exactly what a substituted or half-finished chain half looks like.
  grep -qF "manifest: $MANIFEST" "$DEPLOY_LOG" || die "
$DEPLOY exited 0 and never said it had written $MANIFEST.

It prints \`manifest: <path>\` as its last act, after moving a fully anchored
manifest over the real one, and it reaches that line on no other path. Refusing
to configure the module from a file this run did not produce: in a checkout of
this repository that file is committed, and the agents in it are somebody
else's."
  not_older "$MANIFEST" "$STAMP" || die "
$MANIFEST is older than this run, although $DEPLOY reported writing it.

Refusing to configure the module from a manifest this run did not produce.
Something replaced the file between the two halves."
  echo "  $MANIFEST was written by the run above, and says so"
fi

# AND THE ROW IS CHECKED AGAINST THE CHAIN, not merely read. Same reader
# `--dry-run` uses, so what a dry run approves is what a real run acts on.
read_and_check_row

# ── 4. the Logos Core half ─────────────────────────────────────────────────
#
# No owner and no policy account on this command line. $HEADLESS reads both, by
# header name, out of $MANIFEST — the file exported at the top of this script, so
# the file it reads is certainly the file the chain half wrote. That is the whole
# of the handoff, and it is a handoff through a checked artefact rather than
# through a shell variable that happened to survive.
rule "5. the Logos Core half: install, load, configure, start — headless"
core_run=("$CATEGORY")
[ "$ALONGSIDE" -eq 1 ] && core_run+=(--alongside)
"./$HEADLESS" "${core_run[@]}"
hrc=$?

if [ "$hrc" -ne 0 ]; then
  echo >&2
  if [ "$CONFIGURE_ONLY" -eq 1 ]; then
    echo "the Logos Core half failed (exit $hrc). Nothing was spent: this run" >&2
    echo "did not touch the chain." >&2
    exit 1
  fi
  echo "THE CHAIN HALF SUCCEEDED AND THE LOGOS CORE HALF DID NOT (exit $hrc)." >&2
  echo >&2
  echo "The agents in $MANIFEST are anchored on chain and paid for. What is not" >&2
  echo "done is the configuration, and finishing it costs nothing:" >&2
  echo >&2
  echo "    ./scripts/deploy-and-configure.sh $CATEGORY --configure-only" >&2
  echo >&2
  echo "Its prerequisites were all present when this run started, so whatever" >&2
  echo "failed above happened during the run rather than being missing before it." >&2
  exit 2
fi

rule "done"
echo "$CATEGORY is deployed on LEZ and configured and started in Logos Core,"
echo "from one command."
if [ "$CONFIGURE_ONLY" -eq 0 ]; then
  echo
  echo "All of $CATEGORIES were deployed on chain; $CATEGORY is the one this run"
  echo "configured in the runtime. The others are already anchored, so configuring"
  echo "one of them costs no transaction:"
  for cat in $CATEGORIES; do
    [ "$cat" = "$CATEGORY" ] && continue
    echo "    ./scripts/deploy-and-configure.sh $cat --configure-only"
  done
fi
