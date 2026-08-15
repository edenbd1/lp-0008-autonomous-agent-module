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
# The previous deployment, kept here because the attack it accepted is the
# evidence that the defect was real. See section 5.
PRIOR_TX=8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe
# Step 1: the messaging agent's own public pay account anchors an unlimited
# policy over the agent, honestly naming itself as the owner.
ATTACK_ANCHOR=e530e0ba9a49c4ebacbfeaeac8fff3376f8bece24b71cb8f985b70c5399d462d
# Step 2: the agent then moves its entire 65 LEZ in one transaction, against a
# ceiling its owner anchored at 25.
ATTACK_DRAIN=7fc6c9af06e590c7553af9d3090384e88a2780e38995117ca4e091f49a022228
# The identical step 1, sent to the program this repository deploys today.
ATTACK_REFUSED=a01ace40b839f89b7b662b5532521716bd0906fbeef73ed15dae8c6b2cfd5352
# The account that ceiling was anchored for, and the account that holds it.
VICTIM_POLICY=7ewsGn9SMYUXjHg9ezbzPh9nssxZqD6w7XrL7HubbMyN

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
   An address that encodes the limits stops an agent EDITING its policy. It never
   stopped it anchoring ANOTHER one, and three deployments of comparisons did not
   either — because an attacker holding the agent's key does not have to invent an
   owner or borrow somebody's policy. It IS the owner. It anchors a fresh policy
   naming the compromised agent, with per_tx = u128::MAX, and spends under that.

   That is not an argument about the source. Here are both halves of it.
TXT
echo
echo "   the messaging agent's OWN public pay account anchors an unlimited policy"
echo "   over the agent, against the PREVIOUS program $PRIOR_TX:"
echo "     $ATTACK_ANCHOR"
if q "$ATTACK_ANCHOR" | grep -q '"result":\['; then
  ok "the previous program accepted it: it is in a block"
else
  bad "the recorded attack anchor is not on chain"
fi
echo
echo "   and then the agent moves its ENTIRE 65 LEZ in one transaction, against a"
echo "   ceiling its owner had anchored at 25:"
echo "     $ATTACK_DRAIN"
if q "$ATTACK_DRAIN" | grep -q '"result":\['; then
  ok "accepted too — the ceiling was not a ceiling"
else
  bad "the recorded drain transaction is not on chain"
fi
echo
echo "   the identical anchor, to the program this repository deploys today:"
echo "     $ATTACK_REFUSED"
if q "$ATTACK_REFUSED" | grep -q '"result":null'; then
  ok "never included — the sequencer refused it"
else
  bad "the refused attack is on chain, which means it was not refused"
fi
echo
echo "   because there is exactly ONE policy account for that agent, it already"
echo "   exists, and it holds the owner's own limits:"
if curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
     -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$VICTIM_POLICY\"]}" \
   | python3 -c '
import json,sys
r=json.load(sys.stdin).get("result") or {}
d=bytes(r.get("data") or b"")
if len(d) != 97 or d[0] != 1:
    sys.exit("     the policy account does not hold a record this program wrote")
le = lambda a, b: int.from_bytes(d[a:b], "little")
print("     per_tx %d, per_period %d, period %d blocks - the owner\x27s numbers,"
      % (le(33, 49), le(49, 65), le(65, 73)))
print("     not u128::MAX")'; then
  ok "the address the attack aimed at is taken, and holds the owner's limits"
else
  bad "the victim policy account did not read back as an anchored record"
fi
cat <<'TXT'

   Absence is not evidence, though: a refused hash, a pending one and a hash
   nobody sent all answer null — which is exactly what the control above shows.
   So the refusal is demonstrated where it can be, by running the committed
   binary itself: every hostile call below, and then the two-step attack end to
   end, in each of the three shapes the previous program accepted. Each hostile
   call is paired with the honest call it differs from in one field, because a
   check that only ever refuses proves nothing about what is accepted.
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
   An agent has exactly ONE policy account. Its address is the agent —
   PDA(program, ["agent-policy/v1", agent_id]) — and everything the policy says,
   the owner and both limits and the period, is that account's data. create_policy
   declares it #[account(init)], so the first anchor for an agent is the only
   anchor for that agent: a second one is not detected, it has nowhere to go.
   That is the whole fix. The three deployments before it added a comparison each
   and the attack walked round all three, because the attacker was never lying —
   it really was the owner of the policy it anchored.

   Nothing in the call is left to disagree with. create_policy takes no owner_id:
   it records the account that signed. spend takes no agent_id and no limits: the
   policy account's address is derived from the account that PAYS, and the ceiling
   is read out of that account. Codes 6001, 6004 and 6013 are retired rather than
   reused, because the disagreements they reported can no longer be expressed.

   The per-period total is not an argument either. It sits in the same 97 bytes as
   the limits, which only this program may write, and the period it belongs to is
   pinned into the transaction's own block validity window — so a caller cannot
   reset its budget by naming a different period.

   Above the threshold the agent must present an approval account seeded by the
   exact payment (agent, recipient, amount, nonce), owned by this program, and
   unspent. Checking only that it exists would let anyone fund the address and
   manufacture consent; not stamping it on the way through let one approval pay
   out again on every transaction that presented it.

   What is NOT closed: anchoring is first-come and needs no key from the agent,
   so a third party who knows an agent's public id can take its policy address
   first. That grants no spending authority and moves no money, but it is a
   denial of service. docs/security-model.md §6 states it at full strength.
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
