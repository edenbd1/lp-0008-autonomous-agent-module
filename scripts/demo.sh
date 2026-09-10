#!/usr/bin/env bash
# LP-0008 from a clean clone. No funded account, no local sequencer, no keys.
#
# Everything here either runs locally or reads the public chain. Nothing is
# asserted that the script does not compute or fetch in front of you: a claim a
# reader cannot re-derive from the output in front of them is not evidence, it
# is a sentence.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }
export RISC0_DEV_MODE=0

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
# DERIVED, not written down. This was the literal
# `697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf`, and it was
# the last copy of it in the repository: verify-deployment.sh,
# submission-evidence.py, 02-services-marketplace.sh and 03-spending-threshold.sh
# all compute it. `.github/workflows/ci.yml` records removing exactly this
# pattern from itself -- "a content-addressed chain moves that hash on every
# deploy, so a copy of it in a workflow is a copy that has to be remembered" --
# and this copy outlived that edit. A rebuilt guest would have made this script
# say "computed X, expected Y", which reads as the binary being wrong when the
# stale thing was the literal.
#
# What the recorded value was FOR is checked below, against docs/DEPLOYMENT.md,
# which is where a deployment is recorded and which is already re-checked by
# scripts/verify-deployment.sh.
DEPLOY_TX=$(python3 -c "
import hashlib, struct
b = open('artifacts/programs/agent_verifier.bin', 'rb').read()
print(hashlib.sha256(struct.pack('<I', len(b)) + b).hexdigest())")
IMPOSSIBLE=dededededededededededededededededededededededededededededededede
# The attack: one `create_policy`, per_tx = per_period = u128::MAX, over the
# storage agent DE4jFQbN… — signed by a non-owner (the funder) that has never
# held that agent's key. Sent to the SHIPPED program it was SUBMITTED and NEVER
# INCLUDED — refused. artifacts/adversarial.tsv records it.
ATTACK_REFUSED=a16693f187b09461f2b398ad68f642f6af8b6f11d011f9856de7f0da61e8d2fd
# An EARLIER program version ACCEPTED the identical attack, creating an unlimited
# policy a stranger owned. That transaction and the account it created were on
# chain until this testnet was reset, which cleared them — getTransaction now
# returns null for them, indistinguishable from a hash never sent — so this
# script names them for the record rather than fetching them. Both are preserved
# in git history, and the refusal is proven deterministically, offline, by the
# agent-verifier-adversarial suite this section runs.
ATTACK_ACCEPTED_HISTORICAL=eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7
# The same storage agent, under the program deployed today: the account it signed
# to name its owner, its owner's live create_policy, and its policy account.
AGENT_CLAIM=9JLJKZLukYQbX1k4efUUkbj4Ux9D93Wsoe95yxmnwLnW
HONEST_ANCHOR=1868f89e19a6725c384af8d0c42a44e686d2473c7a68e985953318b2d566c22c
HONEST_POLICY=C7DFFFvvQvFWkWczQTyBP2q9PsBGQmeFACTvSV4NRZgi
# The account id the storage agent designated (owner 4AD8jMUy…), as the 32 bytes
# the claim holds.
HONEST_OWNER_HEX=2eef01d81d73d460882754d497ca6978dbd2c4337ea2e60f51075133b8a0fbf7

rule() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=1; }
# For the sentence a FAIL needs after it when the likeliest cause is that the
# world moved rather than that this repository is wrong. Same shape as the one
# in scripts/use-cases/lib.sh, so the two read alike.
note() { printf '       %s\n' "$1"; }
FAILED=0

rule "0. environment"
echo "RISC0_DEV_MODE=$RISC0_DEV_MODE  (0 = real proofs, no mock receipts)"

# WHAT THIS NEEDS, NAMED BEFORE ANYTHING RUNS.
#
# Run from a clean clone with no Rust toolchain, this used to print
#
#     ./scripts/demo.sh: line 62: rustc: command not found
#     ./scripts/demo.sh: line 76: cargo: command not found
#       FAIL the policy tests did not pass
#
# — a raw shell error a third of the way in, then a FAIL that reads as "this
# repository's tests are broken" when what is broken is that the machine has no
# compiler. A reader who sees that says the demo does not run, and they are
# right to.
#
# So it is asked here, all of it at once, and it REFUSES rather than continuing
# past a missing prerequisite. Not a skip: every section below either runs or
# this exits non-zero saying why. A demo that quietly leaves out the part it
# cannot do is worth less than one that says what it needs.
missing=0
need_cmd() { # command what-it-is how-to-get-it
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "  missing: $1 — $2" >&2
  echo "           $3" >&2
  missing=$((missing + 1))
}
need_cmd rustc  "the Rust compiler, for the policy tests in section 1" \
                "install it from https://rustup.rs — there is no flag to skip section 1"
need_cmd cargo  "Cargo, which runs those tests" \
                "ships with rustup; if rustc is present and cargo is not, the install is partial"
need_cmd curl   "curl, for every chain read below" \
                "preinstalled on macOS and in most Linux images"
need_cmd python3 "Python 3, which decodes the account bytes" \
                "preinstalled on macOS; apt install python3 on Debian/Ubuntu"
if [ "$missing" -gt 0 ]; then
  echo >&2
  echo "$missing prerequisite(s) are missing, so this demo would report failures" >&2
  echo "that are about this machine rather than about this repository. Nothing" >&2
  echo "has been checked. Install them and run it again." >&2
  exit 1
fi
rustc --version

rule "1. the spending policy, adversarially"
echo "The threshold is not an if-statement in the agent: the agent holds its own"
echo "keys on a remote node, so whoever takes the process takes the spending."
echo "It is an account address. These tests cover what that has to survive —"
echo "a per-transaction cap drained by repetition, a hostile period total that"
echo "must not overflow into 'plenty left', and an approval for one payment"
echo "being replayed onto another."
# Gate on the test process, not on whether a line matched. Piping straight into
# grep discards the exit status: a suite that failed prints nothing here, every
# later check still runs, and the script ends with "demo complete" and exit 0 —
# a demo that reports success precisely when the tests are broken.
TESTLOG=$(mktemp)
if cargo test -p agent-policy-core --release --locked --quiet > "$TESTLOG" 2>&1; then
  grep -E "result: ok\. [1-9]" "$TESTLOG" | sed 's/^/   /'
  # The exit code is not the whole assertion, and the grep above was only ever
  # display. `cargo test` exits 0 when it runs NO tests: emptying `mod tests`
  # in agent-policy-core gives `test result: ok. 0 passed` and this printed
  # "OK the policy tests pass" on it — the same shape as a `--list` filter that
  # matches nothing. So count what ran.
  PASSED=$(sed -n 's/^test result: ok\. \([0-9][0-9]*\) passed.*/\1/p' "$TESTLOG" \
           | awk '{n += $1} END {print n+0}')
  if [ "$PASSED" -ge 20 ]; then
    ok "the policy tests pass ($PASSED of them ran)"
  else
    bad "only $PASSED policy test(s) ran — a suite that is not there exits 0 too"
  fi
else
  tail -20 "$TESTLOG" | sed 's/^/   /'
  bad "the policy tests did not pass"
fi
rm -f "$TESTLOG"

rule "2. the deployed program is the program in this repository"
echo "A LEZ deploy tx hash is SHA256(borsh(bytecode)) — content addressed — so"
echo "the committed binary hashes to exactly its deploy transaction."
echo "   computed from artifacts/programs/agent_verifier.bin:"
echo "     $DEPLOY_TX"
# The derivation is the hash; what is checked is that the DEPLOYMENT this
# repository documents is that binary. A rebuild without a redeploy shows up
# here as a document naming a different program, which is what it is.
if [ -n "$DEPLOY_TX" ] && grep -q "$DEPLOY_TX" docs/DEPLOYMENT.md; then
  ok "docs/DEPLOYMENT.md records this binary as the deployed program"
else
  bad "docs/DEPLOYMENT.md does not mention $DEPLOY_TX"
  note "the binary in artifacts/programs/ was rebuilt or replaced and not redeployed,"
  note "or it was redeployed and the document was not updated. Neither is a defect"
  note "in the program: run ./scripts/verify-deployment.sh, which asks the chain."
fi

rule "3. that transaction is live on the public testnet"
q() {
  curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}"
}
if q "$DEPLOY_TX" | grep -q '"result":\['; then ok "getTransaction returns it"
else bad "the chain does not hold $DEPLOY_TX"; fi

# Without this control the check above proves nothing: an RPC that answered
# non-null to everything would pass it just as happily.
if q "$IMPOSSIBLE" | grep -q '"result":null'; then
  ok "control: a hash that cannot exist returns null"
else
  bad "the control hash did not return null — the check above is not meaningful"
fi

rule "4. the transfer program it chains into is the chain's own"
# `spend` moves no balance itself — LEZ rule 5 forbids a program from debiting
# an account it does not own — so it chains a call into the transfer program
# that owns the agent's account, and the privacy circuit needs that program's
# ELF to prove the inner call. A stale or substituted copy would be a real
# problem, so the copy is pinned by content and its ProgramId is read back from
# the chain rather than asserted here.
AT=artifacts/programs/authenticated_transfer.bin
AT_SHA=d0cfb36899c9100f089bbabae8b3ddf449a0bec0791c2955ba7fea1a976e5351
AT_ID='583309054,2344528779,3806558405,2890696795,2257354672,3978764116,2273929063,1518858078'
GOT_SHA=$(python3 -c "
import hashlib;print(hashlib.sha256(open('$AT','rb').read()).hexdigest())" 2>/dev/null)
if [ "$GOT_SHA" = "$AT_SHA" ]; then ok "the vendored transfer program is byte-for-byte the pinned one"
else bad "$AT hashed to ${GOT_SHA:-nothing}, expected $AT_SHA"; fi
CHAIN_ID=$(curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getProgramIds","params":[]}' \
  | python3 -c "
import json,sys
try: print(','.join(str(x) for x in json.load(sys.stdin)['result']['authenticated_transfer']))
except Exception: print('')" 2>/dev/null)
echo "   chain reports authenticated_transfer = ${CHAIN_ID:-<no answer>}"
if [ "$CHAIN_ID" = "$AT_ID" ]; then ok "and its ImageID is the ProgramId the chain runs"
else bad "the chain reports a different transfer program: ${CHAIN_ID:-<no answer>}"; fi

rule "5. the defect this was fixed for, and the fix, adversarially"
cat <<'TXT'
   The previous program made an agent's policy account a PDA of the agent alone,
   so that no second policy could exist. That was right about WHERE a policy goes
   and silent about WHO may put one there — and the answer turned out to be
   anybody. `create_policy` declared two accounts, the policy account and a
   signer it recorded as owner; the agent's own account was never declared, never
   read and never asked to sign, and `agent_id` was a free argument the body threw
   away. So anchoring a policy over somebody else's agent needed NO KEY. It needed
   the agent's public id, which this repository publishes in artifacts/agents.tsv
   and inside every signed Agent Card.

   That is not an argument about the source. Here is the same call, sent to both
   programs, from accounts that have never held the victim's key.
TXT
echo
echo "   create_policy for per_tx = per_period = u128::MAX over the storage agent"
echo "   DE4jFQbN…, signed by a non-owner (the funder) that has never held that"
echo "   agent's key, sent to the program this repository deploys today:"
echo "     $ATTACK_REFUSED"
# The shipped program REFUSES it: submitted, never included. On this chain a
# refused transaction, a pending one and one nobody ever sent all answer null to
# getTransaction, so "null" alone proves nothing — which is exactly what the
# impossible-hash control at the top of this run is for. The DISCRIMINATOR here
# is positive: the shipped program's OWN deploy transaction resolves, so the RPC
# is answering and the null below is the chain refusing the attack, not a dead
# port.
if q "$ATTACK_REFUSED" | grep -q '"result":null'; then
  ok "the shipped program did not include it — refused (submitted, never in a block)"
else
  bad "the attack anchor is on chain under the shipped program, which means it was not refused"
fi
if q "$DEPLOY_TX" | grep -q '"result":\['; then
  ok "the shipped program's own deploy transaction resolves, so that null is a refusal and not a silent RPC"
else
  bad "the shipped program's deploy transaction does not resolve — the RPC may be dead, so the null above proves nothing"
fi
cat <<TXT

   For the record: an earlier program version ACCEPTED the identical attack —
   $ATTACK_ACCEPTED_HISTORICAL,
   creating an unlimited policy a stranger owned. That transaction and the account
   it created were on chain until this testnet was reset, which cleared them;
   getTransaction now returns null for both, indistinguishable from a hash never
   sent, so this script names them rather than fetching them. Both are preserved
   in git history, and the refusal above is proven deterministically, offline, by
   the agent-verifier-adversarial suite run at the end of this section.
TXT
echo
echo "   what stopped it is an account the agent itself signed. $AGENT_CLAIM"
echo "   is PDA(program, [\"agent-owner/v1\", agent]) and holds one field:"
if curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
     -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$AGENT_CLAIM\"]}" \
   | python3 -c "
import json,sys
r=json.load(sys.stdin).get('result') or {}
d=bytes(r.get('data') or b'')
want='$HONEST_OWNER_HEX'
if len(d) != 33 or d[0] != 2:
    sys.exit('     no owner claim at that address')
if d[1:33].hex() != want:
    sys.exit('     the claim names %s, not the account that anchored' % d[1:33].hex())
print('     owner %s' % d[1:33].hex())
print('     — the only account create_policy will accept a signature from')"; then
  ok "the agent named its owner, and the stranger was not it"
else
  bad "the agent's owner claim did not read back"
fi
echo
# The honest anchor is a LIVE transaction on the shipped program; the attack's
# accepted form is not (the testnet reset cleared it, as noted above). So this
# checks the positive fact that survives: the storage agent's real policy was
# anchored by its owner, on chain, at an address the attack could never reach.
echo "   the honest anchor, on the shipped program:"
H_BLK=$(q "$HONEST_ANCHOR" | python3 -c "import sys,json;r=json.load(sys.stdin).get('result');print(r[1] if r else '')")
echo "     honest create_policy $HONEST_ANCHOR  block ${H_BLK:-<absent>}"
if [ -n "$H_BLK" ]; then
  ok "the storage agent's owner anchored its policy on chain, under the shipped program"
else
  bad "the honest anchor is not on chain"
fi
echo "   A policy account is a PDA of its program, so the address the earlier"
echo "   program's attack reached and the address the shipped program anchors are"
echo "   necessarily different accounts — the replacement is why the two can never"
echo "   name one policy."
if curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
     -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$HONEST_POLICY\"]}" \
   | python3 -c "
import json,sys
r=json.load(sys.stdin).get('result') or {}
d=bytes(r.get('data') or b'')
if len(d) != 97 or d[0] != 1:
    sys.exit('     the policy account does not hold a record this program wrote')
le = lambda a, b: int.from_bytes(d[a:b], 'little')
if d[1:33].hex() != '$HONEST_OWNER_HEX':
    sys.exit('     the record names %s, which is not the designated owner' % d[1:33].hex())
print('     owner  %s' % d[1:33].hex())
print('     per_tx %d, per_period %d, period %d blocks — not u128::MAX'
      % (le(33, 49), le(49, 65), le(65, 73)))"; then
  ok "the record is the owner's, and it is the owner the agent designated"
else
  bad "the honest policy account did not read back as the designated owner's record"
fi
cat <<'TXT'

   Absence is not evidence, though: a refused hash, a pending one and a hash
   nobody sent all answer null — which is exactly what the control above shows.
   The two accounts read back above are the positive half. The rest of the
   refusals are demonstrated where they can be, by running the committed binary
   itself: every hostile call below, and then the four-step attack end to end, in
   each of the three shapes an earlier program accepted. Each hostile call is
   paired with the honest call it differs from in one field, because a check that
   only ever refuses proves nothing about what is accepted.
TXT
# Gate on the checker's own exit status, not on whether grep matched a line.
# Piping straight into grep discards it: a suite that failed prints nothing,
# grep exits 1, and the message would be right by accident here and wrong the
# moment the output changes. Same trap as section 1.
ADVLOG=$(mktemp)
( cd crates/agent-verifier-adversarial && cargo run --quiet --release ) > "$ADVLOG" 2>&1
ADVRC=$?
# The refusals the guest prints as it panics are noise here; the checker prints
# its own "ok refused [...]" line for each one.
grep -Ev '^(thread|note:|account validation failed|Program error)' "$ADVLOG"
if [ "$ADVRC" -eq 0 ]; then
  ok "the committed program refuses each attack, with the documented code"
else
  bad "the adversarial cases did not behave as they must"
fi
rm -f "$ADVLOG"

rule "6. what the chain enforces, in one sentence"
cat <<'TXT'
   Binding an agent to a policy takes TWO signatures, in two transactions, from
   two wallets that never meet.

     claim_agent    the AGENT signs. The account it writes is addressed from the
                    signing account — PDA(program, ["agent-owner/v1", agent]) —
                    so there is no agent_id argument to substitute, and it records
                    the one id allowed to anchor. Once: #[account(init)].
     create_policy  that OWNER signs. It reads the claim and refuses any other
                    signer (6020), or refuses outright if nobody claimed (6019).

   A stranger holds neither key, so it can do neither step. That is the property
   section 5 executed against both programs rather than asserted.

   An agent still has exactly ONE policy account, PDA(program,
   ["agent-policy/v1", agent_id]), and everything the policy says — the owner,
   both limits, the period, and the running total — is that account's 97 bytes,
   which only this program may write. Nothing in the call is left to disagree
   with: create_policy takes no owner_id, it records the account that signed;
   spend takes no agent_id and no limits, deriving the policy address from the
   account that PAYS and reading the ceiling out of it.

   And losing the address is no longer permanent. LEZ rule 4 forbids changing an
   account's program owner, so a claimed account can never be released and no
   `close` can exist — the way back is in the record instead. update_policy
   re-fixes both limits and the period in place, on the signature of the owner
   the record names (6012). An owner who thinks the agent is compromised sets
   per_tx = 0 and it spends nothing unattended; the running total is carried
   through untouched, because a new ceiling is not forgiveness for the old one.

   The per-period total is not an argument either. It sits in the same 97 bytes as
   the limits, and the period it belongs to is pinned into the transaction's own
   block validity window — so a caller cannot reset its budget by naming a
   different period.

   Above the threshold the agent must present an approval account seeded by the
   exact payment (agent, recipient, amount, nonce), owned by this program, unspent
   — and NOT YET EXPIRED. An approval used to be valid in every block forever,
   which made it a bearer instrument redeemable the day the agent's key was
   stolen. The owner now names the block it dies at, the marker carries it, and
   spend_approved pins the transaction to [0, expiry): an expired approval is not
   refused, it is a transaction no block will include.

   What is NOT closed: an account holder can always call the program that owns
   its balance. The agent's LEZ is held by the authenticated transfer program, so
   whoever holds the agent's key can move it by calling that program directly,
   without this one. This program bounds what the agent moves THROUGH IT.
   docs/limitations.md states that at full strength.
TXT

echo
if [ "$FAILED" -eq 0 ]; then
  echo "demo complete — every claim above was computed or fetched here, not asserted."
  echo "Deployment and how to re-verify: docs/DEPLOYMENT.md"
  exit 0
else
  echo "DEMO FAILED — see the failures above." >&2
  exit 1
fi
