#!/usr/bin/env bash
# Shared plumbing for the use-case scripts. Sourced, never run.
#
# Three things live here because getting any of them subtly wrong is how a
# demonstration reports success while proving nothing.
#
#   1. The manifest is read BY HEADER NAME. `artifacts/agents.tsv` has gained
#      columns twice, and a script that says `awk '{print $4}'` keeps running
#      after a column moves — it just starts reading the per-transaction limit
#      out of the policy hash. Every field access below names the column.
#
#   2. Every RPC answer is checked for the shape that means "live". This chain
#      answers `"result":null` for a transaction that was dropped, one that is
#      still pending, and one that was never submitted. Only `"result":[` means
#      the chain holds it.
#
#   3. There is a control hash that cannot exist. A check that something is on
#      chain is worth nothing unless the same query, asked about something that
#      is not, comes back empty — an RPC that answered non-null to everything
#      would pass the first check just as happily.

RPC="${SEQUENCER_URL:-https://testnet.lez.logos.co}"

# 32 bytes of 0xde. Nothing can hash to it, so `getTransaction` on it must be
# null, and the PDA derived from it must be an account nobody ever created.
IMPOSSIBLE=dededededededededededededededededededededededededededededededede

FAILED=0
rule() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1" >&2; FAILED=1; }
note() { printf '       %s\n' "$1"; }
die()  { echo "error: $*" >&2; exit 1; }

# `field <tsv> <key> <column-name>` — the value in the named column of the row
# whose first column is <key>. Both the key and the column are matched exactly,
# and a missing column is an error rather than an empty string, because an empty
# string flows onward and produces a wrong answer several steps later.
field() {
  awk -F'\t' -v key="$2" -v want="$3" '
    NR==1 { for (i = 1; i <= NF; i++) if ($i == want) col = i
            if (!col) { print "no column \"" want "\" in " FILENAME > "/dev/stderr"; exit 3 }
            next }
    $1 == key { print $col; found = 1; exit }
    END { if (!found && col) { print "no row \"" key "\" in " FILENAME > "/dev/stderr"; exit 4 } }
  ' "$1"
}

# `rows_of <tsv>` / `row_of <tsv> <n>` — how many data rows a manifest has, and
# the nth of them as `key=value` lines keyed by the header.
#
# `field` keys on the first column, which needs that column to identify a row.
# artifacts/adversarial.tsv's first column is `step` and holds 1,2,1,2,1,1 — so
# there is nothing to key on and the loop that reads it had fallen back to eight
# positional variables. These two plus `kv` give it the header names back.
rows_of() { awk -F'\t' 'NR>1 && NF>1 {n++} END {print n+0}' "$1"; }
row_of() {
  awk -F'\t' -v want="$2" '
    NR==1 { for (i = 1; i <= NF; i++) h[i] = $i; n = NF; next }
    NF>1  { r++ }
    r == want { for (i = 1; i <= n; i++) print h[i] "=" $i; found = 1; exit }
    END { if (!found) { print "no row " want " in " FILENAME > "/dev/stderr"; exit 4 } }
  ' "$1"
}

# Every row of a TSV as `name=value` assignments is more than this needs; what
# the scripts want is one named column of every row, in order.
column_of() {
  awk -F'\t' -v want="$2" '
    NR==1 { for (i = 1; i <= NF; i++) if ($i == want) col = i
            if (!col) { print "no column \"" want "\" in " FILENAME > "/dev/stderr"; exit 3 }
            next }
    { print $col }
  ' "$1"
}

# `kv <blob> <key>` — the value of a named key in `key=value` output. Named and
# not positional, for the same reason every TSV read above is: output grows
# fields, and `cut -d= -f2` on line 7 keeps working while meaning something else.
kv() { printf '%s\n' "$1" | awk -F= -v k="$2" '$1==k {print $2; exit}'; }

# `settlement_facts <tx-hash> <recipient> [policy]` — what the chain says a
# settlement DID, as `key=value` lines: the block it is in, the recipient's
# balance immediately after it, and the policy ledger's running total immediately
# after it. All three come out of the transaction's own committed post-state,
# which `getTransaction` returns and which is content-addressed by the hash being
# asked about. See scripts/use-cases/settlement-facts.py for why this is the only
# route to a historical balance on a chain whose RPC has five methods and no
# getAccountAtBlock among them.
settlement_facts() {
  python3 scripts/use-cases/settlement-facts.py \
    --rpc "$RPC" --tx "$1" ${2:+--recipient "$2"} ${3:+--policy "$3"} 2>&1
}

rpc() {
  local method="$1" params="$2"
  curl -s -m 30 -X POST "$RPC" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}"
}

# True when the chain holds this transaction. `"result":[` and nothing else:
# see note 2 above.
tx_live() { rpc getTransaction "[\"$1\"]" | grep -qE '"result":\['; }

# The public balance of an account. A private account is a commitment in the
# private state and the RPC answers with a default account for it — zero
# balance, zero nonce, default owner — which is indistinguishable from an
# account that does not exist. That is why the accounts checked here are the
# public receiving accounts and not the agents' shielded identities.
balance_of() {
  rpc getAccount "[\"$1\"]" \
    | python3 -c "import json,sys; r=json.load(sys.stdin).get('result'); print(r['balance'] if r else 'null')"
}

# The program that owns an account, as the chain reports it: eight u32 words,
# the same encoding `spel program-id` prints for a compiled program. An account
# nobody has initialised reports eight zeros, and that is the whole mechanism
# behind the spending ceiling — see 03-spending-threshold.sh.
owner_of() {
  rpc getAccount "[\"$1\"]" \
    | python3 -c "
import json,sys
r = json.load(sys.stdin).get('result')
print(','.join(str(x) for x in r['program_owner']) if r else 'null')"
}

chain_height() {
  rpc getLastBlockId '[]' | python3 -c "import json,sys; print(json.load(sys.stdin)['result'])"
}

# An account id, base58 as the wallet prints it, as the 32 raw bytes the chain
# holds. The agent's 32 bytes are the seed of its policy account's address, so
# this has to be the account itself and not a hash of how it prints.
id_hex() {
  python3 -c "
import sys
A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
s = sys.argv[1]
n = 0
for c in s:
    n = n * 58 + A.index(c)
b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
b = b'\x00' * (len(s) - len(s.lstrip('1'))) + b
assert len(b) == 32, 'not a 32-byte account id: %r' % s
print(b.hex())" "$1"
}

# The inverse of `id_hex`: 32 bytes as the base58 an account id is written in.
# Both directions are needed because the CLI is not consistent about which it
# takes — `pda policy --agent-id` wants hex and `pda claim --agent` wants
# base58, and handing either the other one is an error rather than a wrong
# answer, which is the only reason this was cheap to find.
hex_id() {
  python3 -c "
import sys
A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
b = bytes.fromhex(sys.argv[1])
assert len(b) == 32, 'not 32 bytes: %r' % sys.argv[1]
n = int.from_bytes(b, 'big')
s = ''
while n:
    n, r = divmod(n, 58)
    s = A[r] + s
print('1' * (len(b) - len(b.lstrip(b'\x00'))) + s)" "$1"
}

# `policy_record <policy-account> <field>` — one field of the 97 bytes an
# anchored policy account holds, decoded here rather than taken from a manifest.
#
# This replaced a `policy_hash` helper that recomputed sha256(owner, agent,
# limits) and compared it to the address. There is no policy hash any more: the
# address is PDA(program, ["agent-policy/v1", agent_id]) and every term of the
# policy — the owner, both limits, the period and the running total — is the
# account's data, written by the program that owns it. A derivation that agreed
# with the manifest proved the manifest was self-consistent; reading the account
# proves what the chain will actually enforce.
#
# Fields: owner, per_tx, per_period, period_blocks, window_start, spent.
# Anything that is not a 97-byte record with version byte 1 is an error, because
# an unreadable ceiling must never read as a permissive one.
policy_record() {
  rpc getAccount "[\"$1\"]" | python3 -c "
import json, sys
r = json.load(sys.stdin).get('result')
if not r:
    sys.exit('no such account: ' + sys.argv[1])
d = bytes(r['data'])
if len(d) != 97 or d[0] != 1:
    sys.exit('%s holds %d bytes, not a record this program wrote' % (sys.argv[1], len(d)))
le = lambda a, b: int.from_bytes(d[a:b], 'little')
print({'owner': d[1:33].hex(), 'per_tx': le(33,49), 'per_period': le(49,65),
       'period_blocks': le(65,73), 'window_start': le(73,81),
       'spent': le(81,97)}[sys.argv[2]])" "$1" "$2"
}

# `policy_account_of <idl> <program> <agent-base58>` — the one address that
# agent's policy can live at, resolved by spel from the published IDL rather
# than recomputed here. There is exactly one per agent and it does not depend on
# the limits, which is the whole of the fix this deployment carries.
policy_account_of() {
  local hex
  hex=$(id_hex "$3") || return 1
  "${SPEL:-spel}" --idl "$1" --program "$2" pda policy --agent-id "$hex" 2>/dev/null \
    | tr -d '[:space:]'
}

# Run the A2A task lifecycle through the module's real `TaskStore`, or say
# plainly that it was not run. Never print a state that did not happen.
#
# This replaces the loop both demos used to carry:
#
#     for state in submitted working completed; do printf '  state -> %s\n' ...
#
# which printed three strings, drove nothing, and skipped `input-required`, the
# failure paths and the terminal rule entirely. `docs/a2a-binding.md` §7.1 says
# of it, in its own words, "this document will not describe a printed line as a
# transition" — so neither does this.
#
# The driver needs a C++ compiler and nlohmann/json and nothing else: no node,
# no chain, no key. When they are absent the honest answer is that the lifecycle
# was not exercised here, plus where it *is* exercised — not a prettier printf.
a2a_lifecycle() {           # a2a_lifecycle <repo-root> [card-file]
  local root="$1" card="${2:-}" src bin cxx inc
  src="$root/module/tests/a2a_drive.cpp"
  bin="${TMPDIR:-/tmp}/a2a_drive.$$"
  [ -f "$src" ] || { note "the lifecycle driver $src is missing; nothing was run"; return 1; }

  cxx=$(command -v c++ || command -v clang++ || command -v g++) || true
  if [ -z "$cxx" ]; then
    note "no C++ compiler here, so the task lifecycle was NOT driven in this run."
    note "It is covered by module/tests/agent_skills_test.cpp ('task lifecycle'),"
    note "which CI runs. No state is printed above, because none was taken."
    return 1
  fi
  # nlohmann/json is a header; find it rather than assume a system install.
  inc=""
  for d in /usr/include /usr/local/include /opt/homebrew/include "$root/_external/include"; do
    [ -f "$d/nlohmann/json.hpp" ] && { inc="-I$d"; break; }
  done
  if [ -z "$inc" ] && ! echo '#include <nlohmann/json.hpp>' | "$cxx" -std=c++17 -fsyntax-only -x c++ - 2>/dev/null; then
    note "nlohmann/json is not installed here, so the task lifecycle was NOT driven"
    note "in this run. It is covered by module/tests/agent_skills_test.cpp, which CI"
    note "runs. No state is printed above, because none was taken."
    return 1
  fi
  if ! "$cxx" -std=c++17 $inc "$src" "$root/module/src/agent_skills.cpp" -o "$bin" 2>/dev/null; then
    note "the lifecycle driver did not build here, so no state was driven."
    note "It is covered by module/tests/agent_skills_test.cpp, which CI runs."
    return 1
  fi
  local rc=0
  if [ -n "$card" ] && [ -f "$card" ]; then
    "$bin" lifecycle --card "$card" || rc=$?
  else
    "$bin" lifecycle || rc=$?
  fi
  rm -f "$bin"
  return $rc
}

finish() {
  echo
  if [ "$FAILED" -eq 0 ]; then
    echo "$1"
    exit 0
  fi
  echo "FAILED — see the failures above." >&2
  exit 1
}

# --- spends recorded outside the A2A manifest ----------------------------------
# A shielded payment is the same `spend` instruction against the same policy
# account, but it lands in artifacts/shielded-settlement.tsv, because a shielded
# payee has no public balance to compare a delta against. Any check that reads
# the ledger and compares it to a running total must count those too, or it
# reports a settlement the chain has and the repository does not -- which is a
# defect in the reading, not in the record.
#
# Two things it must get right, and both were wrong before they were separated:
# a ledger address is PDA(program, agent), so a spend by one agent says nothing
# about another's ledger; and a shielded spend can land BETWEEN two settlements,
# so only the ones before a given block are in that settlement's reading.
shielded_payer_ledger() {  # <idl> <program-bin> -> the ledger those spends charge
  local f=artifacts/shielded-settlement.tsv payer
  [ -f "$f" ] || { echo ""; return; }
  payer=$(awk -F'\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
                       $h["role"]=="settlement"{print $h["payer_account"];exit}' "$f")
  [ -n "$payer" ] || { echo ""; return; }
  policy_account_of "$1" "$2" "$payer" 2>/dev/null
}

shielded_before() {  # <deploy-tx> <block> <ledger> <shielded-ledger> -> amount
  local f=artifacts/shielded-settlement.tsv
  [ -f "$f" ] && [ -n "$4" ] && [ "$3" = "$4" ] || { echo 0; return; }
  awk -F'\t' -v prog="$1" -v before="$2" '
    NR==1 { for (i=1;i<=NF;i++) h[$i]=i; next }
    $h["role"]=="settlement" && $h["program"]==prog && ($h["block"]+0) < (before+0) { s += $h["amount"] }
    END { print s+0 }' "$f" 2>/dev/null
}
