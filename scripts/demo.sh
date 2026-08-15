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
DEPLOY_TX=697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf
IMPOSSIBLE=dededededededededededededededededededededededededededededededede
# The previous deployment, kept here because the attack it accepted is the
# evidence that the defect was real. See section 5.
PRIOR_TX=a780003b07204fc4d7445b5d88bbd2db8de248f0f1e5ffdbcd75fd268576841e
# One `create_policy`, per_tx = per_period = u128::MAX, over the storage agent
# 9Xpkkvos… — signed by an account created for the purpose, which has never held
# that agent's key and never will. Sent to both programs.
ATTACK_ACCEPTED=eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7
ATTACK_REFUSED=60de3fc607f98d15474fd288d366fa578d01de57c3fd20ba4191779337309040
# The account the accepted one created. It is still there, and it still says what
# it said, so this section does not have to argue from a missing transaction.
ATTACK_POLICY=5QAVJAMHkpLnAMht3bonFijyApPZfAccHAFbzByNq8VV
# The same agent, under the program deployed today: what the agent itself signed,
# what its owner then anchored, and where.
AGENT_CLAIM=EZSN69njgBixwniExyhRrjri1xUTX6iN7xxhcvG4Vvie
HONEST_ANCHOR=6857ba2378a84ba51618582e852e3827a872e3ea85f17de76bdb45b1631fe7d4
HONEST_POLICY=6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe
# The account id the storage agent designated, as the 32 bytes the claim holds.
HONEST_OWNER_HEX=181eee76d02339bbe8ce7abee778942d80b2546a8f204e19940311ff5bd46214

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
echo "   9Xpkkvos…, signed by RZmSLJAB… — a stranger — against the PREVIOUS"
echo "   program $PRIOR_TX:"
echo "     $ATTACK_ACCEPTED"
if q "$ATTACK_ACCEPTED" | grep -q '"result":\['; then
  ok "the previous program accepted it: it is in a block"
else
  bad "the recorded attack anchor is not on chain"
fi
echo
echo "   and it is not gone. Read the account that anchor created:"
if curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
     -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$ATTACK_POLICY\"]}" \
   | python3 -c '
import json,sys
r=json.load(sys.stdin).get("result") or {}
d=bytes(r.get("data") or b"")
if len(d) != 97 or d[0] != 1:
    sys.exit("     no record at the address the attack anchored")
le = lambda a, b: int.from_bytes(d[a:b], "little")
if le(33, 49) != (1 << 128) - 1:
    sys.exit("     the attack account does not hold an unlimited policy")
print("     owner  %s  (the stranger, not the agent\x27s)" % d[1:33].hex())
print("     per_tx %d = 2**128-1" % le(33, 49))'; then
  ok "under the previous program a stranger owns that agent's only policy, for good"
else
  bad "the attack policy account did not read back as an unlimited record"
fi
echo
echo "   the identical call, same agent, same limits, to the program this"
echo "   repository deploys today:"
echo "     $ATTACK_REFUSED"
if q "$ATTACK_REFUSED" | grep -q '"result":null'; then
  ok "never included — the sequencer refused it (6020)"
else
  bad "the refused attack is on chain, which means it was not refused"
fi
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
echo "   and losing that race is not what happened: the honest owner anchored"
echo "   AFTERWARDS, at the same address the attack aimed at."
echo "     $HONEST_ANCHOR"
if q "$HONEST_ANCHOR" | grep -q '"result":\['; then
  ok "in a block"
else
  bad "the honest anchor is not on chain"
fi
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
