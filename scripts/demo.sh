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
DEPLOY_TX=a780003b07204fc4d7445b5d88bbd2db8de248f0f1e5ffdbcd75fd268576841e
IMPOSSIBLE=dededededededededededededededededededededededededededededededede
# The previous deployment, kept here because the attack that it accepted is the
# evidence that the defect was real. See section 5.
PRIOR_TX=b028eabf205b1f05f488d164b3ad2e4c4c333bf01923752c3877ab9cb8c18549
ATTACK_ACCEPTED=c0b21ba6325e451d61408692b4f897ff6e5d22535abeecb671c59a4b1af554c2
ATTACK_REFUSED=30c93c6176ae14eb252d2a5ce9fe9e89b8db830d2ea9ab1bdf79eb0b46d243f8

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

rule "5. the defect this was fixed for, and the fix, adversarially"
cat <<'TXT'
   An address that encodes the limits stops an agent EDITING its policy. It does
   not stop it anchoring a different one, and until this deployment nothing did:
   create_policy took the owner as caller-supplied bytes and never compared them
   to the account that signed. So a compromised agent could not raise its
   ceiling — it could mint itself a new one.

   That is not an argument about the source. Here is the transaction.
TXT
echo
echo "   a create_policy for per_tx = u128::MAX, signed by 3KcCpbGL…fZ8pm2,"
echo "   naming owner aaaa…aaaa — an account nobody controls — sent to the"
echo "   PREVIOUS program $PRIOR_TX:"
echo "     $ATTACK_ACCEPTED"
if q "$ATTACK_ACCEPTED" | grep -q '"result":\['; then
  ok "the previous program accepted it: it is in a block"
else
  bad "the recorded attack transaction is not on chain"
fi
echo
echo "   the identical call, to the program this repository deploys today:"
echo "     $ATTACK_REFUSED"
if q "$ATTACK_REFUSED" | grep -q '"result":null'; then
  ok "never included — the sequencer refused it"
else
  bad "the refused attack is on chain, which means it was not refused"
fi
cat <<'TXT'

   Absence is not evidence, though: a refused hash, a pending one and a hash
   nobody sent all answer null — which is exactly what the control above shows.
   So the refusal is demonstrated where it can be, by running the committed
   binary itself. Each hostile call below is paired with the honest call it
   differs from in one field, because a check that only ever refuses proves
   nothing about what is accepted.
TXT
if ( cd crates/agent-verifier-adversarial && cargo run --quiet --release ) 2>&1 \
   | grep -Ev '^(thread|note:)' ; then
  ok "the committed program refuses each attack, with the documented code"
else
  bad "the adversarial cases did not behave as they must"
fi

rule "6. what the chain enforces, in one sentence"
cat <<'TXT'
   The policy account's address is the hash of (owner, agent, per-tx limit,
   per-period limit, period), and the program checks that the account that signs
   is that owner and the account that pays is that agent. Raising a limit does
   not edit an account — it names a different one, which create_policy never
   initialised — and anchoring a fresh one under an owner you control does not
   help, because the agent it names is not you.

   The per-period total is not an argument any more. It lives in the policy
   account's data, which only this program may write, and the period it belongs
   to is pinned into the transaction's own block validity window — so a caller
   cannot reset its budget by naming a different period.

   Above the threshold the agent must present an approval account seeded by the
   exact payment (policy, recipient, amount, nonce), owned by this program, and
   unspent. Checking only that it exists would let anyone fund the address and
   manufacture consent; not stamping it on the way through let one approval pay
   out again on every transaction that presented it.
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
