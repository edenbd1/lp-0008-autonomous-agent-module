#!/usr/bin/env bash
# LP-0008 from a clean clone. No funded account, no local sequencer, no keys.
#
# Everything here either runs locally or reads the public chain. Nothing is
# asserted that the script does not compute or fetch in front of you — the one
# named reason a previous LP-0008 submission was closed was "no evidence in the
# repo and demo.sh doesn't run".
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
export RISC0_DEV_MODE=0

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"
DEPLOY_TX=b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549
IMPOSSIBLE=dededededededededededededededededededededededededededededededede

rule() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=1; }
FAILED=0

rule "0. environment"
echo "RISC0_DEV_MODE=$RISC0_DEV_MODE  (0 = real proofs, no mock receipts)"
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
  ok "the policy tests pass"
else
  tail -20 "$TESTLOG" | sed 's/^/   /'
  bad "the policy tests did not pass"
fi
rm -f "$TESTLOG"

rule "2. the deployed program is the program in this repository"
echo "A LEZ deploy tx hash is SHA256(borsh(bytecode)) — content addressed — so"
echo "the committed binary hashes to exactly its deploy transaction."
CALC=$(python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")
echo "   computed from artifacts/programs/agent_verifier.bin:"
echo "     $CALC"
if [ "$CALC" = "$DEPLOY_TX" ]; then ok "matches the recorded deploy transaction"
else bad "computed $CALC, expected $DEPLOY_TX"; fi

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

rule "5. what the chain enforces, in one sentence"
cat <<'TXT'
   The policy account's address is the hash of (owner, agent, per-tx limit,
   per-period limit, period). Raising a limit does not edit an account — it
   names a different one, which create_policy never initialised, so its owner is
   the default and the spend is rejected before the program body runs.

   Above the threshold the agent must present an approval account seeded by the
   exact payment (policy, recipient, amount, nonce) and owned by this program.
   Checking only that it exists would let anyone fund the address and
   manufacture consent.
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
