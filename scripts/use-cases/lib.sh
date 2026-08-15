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
# holds. The policy hash commits to the owner and the agent by account id, so
# these have to be the accounts themselves and not a hash of how they print.
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

# `policy_hash <owner-hex> <agent-hex> <per_tx> <per_period> <period_blocks>`
#
# Refuses to return anything that is not 64 hex characters. An earlier version
# of this called `cargo run --example policy-hash` with stderr redirected to
# /dev/null, and when that target stopped existing the substitution produced an
# empty string: the run then compared "" against the anchored hash, reported a
# mismatch, and derived the PDA of nothing. A missing tool has to look like a
# missing tool.
policy_hash() {
  local out
  out=$(python3 "$(dirname "${BASH_SOURCE[0]}")/policy-hash.py" "$@") || {
    echo "the policy-hash derivation failed for: $*" >&2; return 1; }
  case "$out" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) [ "${#out}" -eq 64 ] || {
      echo "the policy-hash derivation returned ${#out} characters, not 64" >&2; return 1; } ;;
    *) echo "the policy-hash derivation returned something that is not a hash: $out" >&2; return 1 ;;
  esac
  printf '%s\n' "$out"
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
