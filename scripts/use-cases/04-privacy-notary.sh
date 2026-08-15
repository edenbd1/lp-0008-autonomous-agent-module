#!/usr/bin/env bash
# USE CASE 4 — a privacy-preserving notary: the agent timestamps a document,
# stores it on Logos Storage, and records the content address on LEZ.
#
#   ./scripts/use-cases/04-privacy-notary.sh              # notarise a fresh document
#   NOTARY_DOC=path ./scripts/use-cases/04-privacy-notary.sh
#   NOTARY_VERIFY_ONLY=1 ./scripts/use-cases/04-privacy-notary.sh   # no chain write
#
# From the prize's list of illustrative use cases:
#
#   "Privacy-preserving notary: agent timestamps a document, uploads it to Logos
#    Storage, and records the content address on LEZ — providing a verifiable,
#    private proof of existence."
#
# The two words that do the work are "verifiable" and "private", and they pull
# against each other. Writing the content address on chain in the clear is
# verifiable and not private: the address is a hash of the document, so anyone
# who guesses the document confirms it, and everyone learns that this agent
# notarised something at that address. Writing nothing is private and proves
# nothing.
#
# What is recorded here is neither. The content address is turned into a KEY:
#
#     secret = sha256("lp0008-notary/v1" || content_address)
#     pubkey = the x-only secp256k1 public key of that secret
#
# and the notarisation is a transaction on the public testnet SIGNED BY THAT KEY.
# The chain stores the public key, in the witness, in a block. Thirty-two bytes
# that reveal nothing.
#
#   verifiable — a holder of the document recomputes the content address,
#     recomputes the key, and finds it in the transaction the notary points at.
#     Nobody without the document can put that key on chain, because the
#     sequencer will not accept a transaction that is not signed under it.
#
#   private — the chain never sees the document, its content address, or its
#     hash. A public key is not a commitment anyone can open by inspection.
#
#   timestamped — by the block, which is what a block is for. The chain's own
#     clock account converts a block to wall-clock time, and it is read here
#     rather than the machine's clock being believed.
#
# WHAT IT COSTS: nothing. The recording transaction is `auth-transfer init`,
# which creates an account and moves no value; LEZ v0.2.4 charges no execution
# fee, so a notarisation costs 0 LEZ. It was measured: the run that produced the
# first record in artifacts/notary.tsv left every balance in this repository
# exactly where it was.
#
# WHAT IS NOT DEMONSTRATED, said plainly: the deployed agent_verifier program has
# no instruction that writes arbitrary bytes, so the content address is not
# stored on chain as DATA — it is bound to the chain through the signing key.
# That is a weaker claim than "the CID is on chain" and it is the true one. See
# docs/use-cases.md.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
. scripts/use-cases/lib.sh

WALLET="${WALLET_BIN:-wallet}"
STORAGE_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-nim}"
WORK="${TMPDIR:-/tmp}/lp0008-usecase-04"
MANIFEST="${NOTARY_MANIFEST:-artifacts/notary.tsv}"
VERIFY_ONLY="${NOTARY_VERIFY_ONLY:-0}"
# The program every account on this chain is initialised under. Not asserted:
# compared below against what `getProgramIds` reports and against the program id
# inside the notarising transaction itself.
TRANSFER_PROGRAM=authenticated_transfer

SLIB="$STORAGE_SRC/build/libstorage.dylib"; [ -f "$SLIB" ] || SLIB="$STORAGE_SRC/build/libstorage.so"

# `tx_facts <hash>` — a public LEZ transaction, taken apart. Its shape is
# kind byte, program id (32), account count (u32), account ids, nonce count and
# nonces, instruction data, then the witness: a 64-byte signature and the 32-byte
# public key that signed it, last. The hash is checked first — a LEZ transaction
# hash is sha256 over the bytes without the leading kind byte — so what comes
# back describes this transaction or the function reports an error.
tx_facts() {
  rpc getTransaction "[\"$1\"]" | python3 -c "
import base64, hashlib, json, sys
want = sys.argv[1]
r = json.load(sys.stdin).get('result')
if not r:
    print('found=0'); sys.exit(0)
b = base64.b64decode(r[0])
print('found=1'); print('block=%s' % r[1]); print('bytes=%d' % len(b))
if hashlib.sha256(b[1:]).hexdigest() != want:
    print('hash_ok=0'); sys.exit(0)
print('hash_ok=1')
print('kind=%d' % b[0])
print('program_id=%s' % b[1:33].hex())
n = int.from_bytes(b[33:37], 'little')
A = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58(x):
    v = int.from_bytes(x, 'big'); s = ''
    while v: v, m = divmod(v, 58); s = A[m] + s
    return '1' * (len(x) - len(x.lstrip(b'\x00'))) + s
for i in range(n):
    print('account=%s' % b58(b[37 + 32 * i:69 + 32 * i]))
# The witness public key is the last 32 bytes, and the 64 before it are the
# signature over the message. Taken from the end rather than by walking the
# instruction data, because the tail is fixed and the middle is not.
print('signature=%s' % b[-96:-32].hex())
print('pubkey=%s' % b[-32:].hex())" "$1" 2>/dev/null
}

# The chain's own wall clock. /LEZ/ClockProgramAccount/0000001 is a real account
# whose 16 bytes are (block, milliseconds) and which the sequencer rewrites every
# block, so a block number can be turned into a time without trusting this
# machine's clock.
CLOCK_ACCOUNT=$(python3 -c "
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
s=b'/LEZ/ClockProgramAccount/0000001'
n=int.from_bytes(s,'big'); o=''
while n: n,r=divmod(n,58); o=A[r]+o
print(o)")
chain_clock() {
  rpc getAccount "[\"$CLOCK_ACCOUNT\"]" | python3 -c "
import datetime, json, sys
r = json.load(sys.stdin).get('result')
d = bytes(r['data']) if r else b''
if len(d) != 16:
    print('block=? time=?'); sys.exit(0)
blk = int.from_bytes(d[0:8], 'little'); ms = int.from_bytes(d[8:16], 'little')
print('block=%d time=%sZ' % (blk, datetime.datetime.utcfromtimestamp(ms / 1000)
                             .strftime('%Y-%m-%d %H:%M:%S')))" 2>/dev/null
}

rule "0. what this needs"
if [ "$VERIFY_ONLY" != "1" ]; then
  command -v "$WALLET" >/dev/null 2>&1 || [ -x "$WALLET" ] \
    || die "no wallet on PATH. Set WALLET_BIN to the LEZ wallet binary."
  if [ ! -f "$SLIB" ]; then
    cat >&2 <<TXT
  no Logos Storage library at $SLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules -b v0.4.4 \\
      https://github.com/logos-storage/logos-storage-nim $STORAGE_SRC
  cd $STORAGE_SRC && export PATH="\$HOME/.nimble/bin:\$PATH" && make libstorage
TXT
    die "build the Storage library first, or set STORAGE_SRC"
  fi
  echo "  storage  $SLIB"
  echo "  wallet   $WALLET"
fi
echo "  chain    $RPC, $(chain_clock)"
# The transfer program's id, from the chain, so the checks below compare against
# what the sequencer runs rather than against a constant in this file.
TRANSFER_ID=$(rpc getProgramIds '[]' | python3 -c "
import json,sys
r = json.load(sys.stdin).get('result') or {}
w = r.get('$TRANSFER_PROGRAM')
print(''.join('%08x' % x for x in w) if w else '')" 2>/dev/null)
TRANSFER_HEX=$(python3 -c "
import sys
w = sys.argv[1]
print(''.join(bytes.fromhex(w[i:i+8])[::-1].hex() for i in range(0, len(w), 8)))" "$TRANSFER_ID" 2>/dev/null)
if [ -n "$TRANSFER_HEX" ]; then
  ok "the chain reports its $TRANSFER_PROGRAM program as $TRANSFER_HEX"
else
  bad "getProgramIds does not report a $TRANSFER_PROGRAM program"
fi

if [ "$VERIFY_ONLY" != "1" ]; then
  rule "1. the document"
  rm -rf "$WORK"; mkdir -p "$WORK/store"
  if [ -n "${NOTARY_DOC:-}" ]; then
    [ -f "$NOTARY_DOC" ] || die "no such document: $NOTARY_DOC"
    cp "$NOTARY_DOC" "$WORK/doc"
  else
    # A fresh document per run, so a notarisation is a new fact each time rather
    # than a re-reading of an old one. Its marker is what step 6 proves the
    # retrieved bytes still carry.
    MARKER="lp0008-notary-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    cat > "$WORK/doc" <<TXT
LP-0008 notarised document
marker: $MARKER
The point of notarising this is that its existence at a moment can be proved
later without showing anyone what it says.
TXT
  fi
  DOC_SHA=$(shasum -a 256 "$WORK/doc" | cut -d' ' -f1)
  echo "  $(wc -c < "$WORK/doc" | tr -d ' ') bytes"
  echo "  sha256  $DOC_SHA"

  rule "2. the agent stores it on a real Logos Storage node"
  # Same driver as use case 1, against a real node, and its exit code is the
  # result: every step inside it is an assertion, so a node that failed to start
  # cannot produce a passing transcript.
  VBIN="$WORK/vault_drive"
  cc -o "$VBIN" scripts/use-cases/vault_drive.c \
     -I"$STORAGE_SRC/library" -L"$STORAGE_SRC/build" -lstorage \
     -Wl,-rpath,"$STORAGE_SRC/build" || die "the storage driver did not compile"
  LOG="$WORK/storage.log"
  ( cd "$WORK" && "$VBIN" ./store ./doc ./retrieved ) > "$LOG" 2>&1; rc=$?
  grep -E "^[0-9]\.|^  (ok|FAIL|<-)|^CID |failure" "$LOG" | sed 's/^/  /'
  CID=$(awk '/^CID /{print $2}' "$LOG")
  if [ "$rc" -eq 0 ] && [ -n "$CID" ]; then
    ok "stored, and the node gave back content address $CID"
  else
    bad "the storage driver failed — full log at $LOG"
  fi
  # The retrieved bytes have to be the document, or "this address names that
  # document" is a claim about nothing.
  if [ -s "$WORK/retrieved" ] \
     && [ "$(shasum -a 256 "$WORK/retrieved" | cut -d' ' -f1)" = "$DOC_SHA" ]; then
    ok "and reading that address back gives the document, byte for byte"
  else
    bad "the address did not read back as the document"
  fi

  rule "3. the content address becomes a key"
  [ -n "$CID" ] || die "no content address to notarise"
  KEYS=$(python3 scripts/use-cases/notary-key.py "$CID") || die "key derivation failed"
  SECRET=$(kv "$KEYS" secret); PUBKEY=$(kv "$KEYS" pubkey)
  echo "  secret = sha256(\"lp0008-notary/v1\" || content address)"
  echo "  pubkey = $PUBKEY"
  note "the chain will see the public key and never the address it came from"
  # Deriving twice and getting the same answer is the whole verification story,
  # so it is exercised here rather than asserted.
  if [ "$(kv "$(python3 scripts/use-cases/notary-key.py "$CID")" pubkey)" = "$PUBKEY" ]; then
    ok "the derivation is deterministic: the same address gives the same key"
  else
    bad "the derivation is not deterministic — nothing here could be verified"
  fi
  # And a different document must give a different key, or every notarisation
  # would be the same notarisation.
  OTHER=$(kv "$(python3 scripts/use-cases/notary-key.py "${CID}x")" pubkey)
  if [ "$OTHER" != "$PUBKEY" ]; then
    ok "control: a different content address gives a different key"
  else
    bad "control: two different addresses derived the same key"
  fi

  rule "4. before: the chain has never seen this notarisation"
  # Worth doing properly. "The account exists now" proves nothing unless it did
  # not exist a moment ago, and this chain answers for an account nobody created
  # with a default account rather than an error — zero balance, zero nonce, owner
  # all zeros — which is exactly what is checked for.
  HOME_DIR="$WORK/wallet"; mkdir -p "$HOME_DIR"
  cp "$HOME/.lez-wallet/wallet_config.json" "$HOME_DIR/" 2>/dev/null \
    || printf '{ "sequencers": [{ "sequencer_addr": "%s" }], "seq_poll_timeout": "30s", "seq_tx_poll_max_blocks": 15, "seq_poll_max_retries": 10, "seq_block_poll_max_amount": 100 }\n' "$RPC" > "$HOME_DIR/wallet_config.json"
  export NSSA_WALLET_HOME_DIR="$HOME_DIR" LEE_WALLET_HOME_DIR="$HOME_DIR"
  "$WALLET" account import public --private-key "$SECRET" </dev/null >/dev/null 2>&1 \
    || die "the wallet would not import the derived key"
  # The account id the wallet reports for that key. It is not a function of the
  # public key that this script can recompute, so it is read back rather than
  # derived — and it is not what the verification rests on. The public key is.
  ACCOUNT=$("$WALLET" account list </dev/null 2>/dev/null \
    | awk '/^Public\//{print substr($1,8); exit}')
  [ -n "$ACCOUNT" ] || die "the wallet did not report an account for the derived key"
  echo "  account $ACCOUNT"
  BEFORE_OWNER=$(owner_of "$ACCOUNT")
  if [ "$BEFORE_OWNER" = "0,0,0,0,0,0,0,0" ]; then
    ok "getAccount reports the default account: nothing has ever created it"
  else
    bad "that account already exists, owned by $BEFORE_OWNER — this run proves nothing new"
  fi

  rule "5. record it on LEZ"
  echo "  submitting an account-creation transaction signed by the derived key."
  echo "  It moves no value; LEZ v0.2.4 charges no execution fee, so this costs 0 LEZ."
  OUT=$("$WALLET" auth-transfer init --account-id "Public/$ACCOUNT" </dev/null 2>&1)
  TX=$(printf '%s' "$OUT" | grep -oE '[0-9a-f]{64}' | head -1)
  if [ -z "$TX" ]; then
    printf '%s\n' "$OUT" | tail -5 | sed 's/^/  /' >&2
    bad "no transaction was produced"
  else
    echo "  $TX"
  fi
else
  rule "1-5. skipped: NOTARY_VERIFY_ONLY=1, nothing is written to the chain"
  TX=
fi

rule "6. verified against the chain, and against the chain only"
# Every prior notarisation as well as this run's, because a proof of existence
# that stops being checkable a week later is not one. Nothing below is taken
# from the manifest except identifiers: the block, the public key, the account
# and the program are all fetched and decoded here.
if [ -n "${TX:-}" ]; then
  HEADER='content_address	doc_sha256	account	pubkey	notary_tx'
  [ -s "$MANIFEST" ] && [ "$(head -n1 "$MANIFEST")" = "$HEADER" ] || printf '%s\n' "$HEADER" > "$MANIFEST"
  # Written only after the transaction has a hash; whether it LANDED is decided
  # below, against the chain, for this row like every other.
  printf '%s\t%s\t%s\t%s\t%s\n' "$CID" "$DOC_SHA" "$ACCOUNT" "$PUBKEY" "$TX" >> "$MANIFEST"
fi
[ -s "$MANIFEST" ] || bad "no notarisations recorded at $MANIFEST"
# SEEN counts rows this loop entered; COUNT counts rows that passed EVERY check
# in it. They were one variable, incremented on entry, and the summary line
# below — the one CI parses and compares against the manifest's row count —
# called it "each verified". It was not: a row that failed every check still
# incremented it and then `continue`d, so a run against an unreachable chain
# printed "OK 1 notarisation(s), each verified from the chain" having verified
# none. Demonstrated by pointing SEQUENCER_URL at a dead port. `ROW_OK` is what
# makes the sentence true, and the seen/verified comparison at the end is what
# makes a silently skipped row a failure rather than a smaller number.
COUNT=0
SEEN=0
if [ -s "$MANIFEST" ]; then
  for ADDR in $(column_of "$MANIFEST" content_address); do
    NTX=$(field "$MANIFEST" "$ADDR" notary_tx)
    [ -n "$NTX" ] || continue
    NACC=$(field "$MANIFEST" "$ADDR" account)
    SEEN=$((SEEN + 1))
    ROW_OK=1
    echo
    echo "  content address $ADDR"
    echo "    $NTX"
    # Blocks are 60 seconds apart and a fresh transaction may not be indexed the
    # instant it is submitted, so a notarisation made in this run gets a couple
    # of blocks before absence is called absence.
    F=$(tx_facts "$NTX")
    if [ "$(kv "$F" found)" != "1" ] && [ "$NTX" = "${TX:-}" ]; then
      for _ in 1 2 3 4 5 6; do
        sleep 30
        F=$(tx_facts "$NTX")
        [ "$(kv "$F" found)" = "1" ] && break
      done
    fi
    if [ "$(kv "$F" found)" != "1" ]; then
      bad "  getTransaction returns null — this notarisation is not on chain"
      continue
    fi
    ok "  the chain holds it, in block $(kv "$F" block)"
    if [ "$(kv "$F" hash_ok)" = "1" ]; then
      ok "  its $(kv "$F" bytes) bytes hash to exactly that transaction hash"
    else
      bad "  the bytes the chain returned are not that transaction"
      continue
    fi
    # THE VERDICT. The key that signed the transaction on chain is the key the
    # content address derives — recomputed here from the address, compared
    # against bytes decoded out of the transaction. Neither side is the manifest.
    CHAIN_PUB=$(kv "$F" pubkey)
    V=$(python3 scripts/use-cases/notary-key.py "$ADDR" --verify --pubkey "$CHAIN_PUB")
    if [ "$(kv "$V" match)" = "1" ]; then
      ok "  signed by the key this content address derives: $CHAIN_PUB"
    else
      bad "  signed by $CHAIN_PUB; this address derives $(kv "$V" expected)"; ROW_OK=0
    fi
    # The account it created, and the program that owns it — both from the chain.
    if [ "$(kv "$F" account)" = "$NACC" ]; then
      ok "  and it created $NACC"
    else
      bad "  the transaction names $(kv "$F" account), the manifest records $NACC"; ROW_OK=0
    fi
    NOW_OWNER=$(owner_of "$NACC")
    if [ -n "$TRANSFER_ID" ] && [ "$(kv "$F" program_id)" = "$TRANSFER_HEX" ]; then
      ok "  under the chain's own $TRANSFER_PROGRAM program"
    else
      bad "  the transaction ran $(kv "$F" program_id), not the chain's $TRANSFER_PROGRAM"; ROW_OK=0
    fi
    if [ "$NOW_OWNER" != "0,0,0,0,0,0,0,0" ]; then
      ok "  getAccount still reports it as a real account today"
    else
      bad "  getAccount reports the default account — the record is gone"; ROW_OK=0
    fi
    [ "$ROW_OK" -eq 1 ] && COUNT=$((COUNT + 1))
  done
fi
echo
if [ "$COUNT" -ge 1 ] && [ "$COUNT" -eq "$SEEN" ]; then
  ok "$COUNT notarisation(s), each verified from the chain against the document's own key"
elif [ "$SEEN" -ge 1 ]; then
  bad "$COUNT of $SEEN notarisation(s) verified — the rest failed a check above"
else
  bad "no notarisation could be checked"
fi
# The controls. A check that something is on chain is worth nothing unless the
# same question, asked about something that is not, comes back empty.
if rpc getTransaction "[\"$IMPOSSIBLE\"]" | grep -q '"result":null'; then
  ok "control: a transaction hash that cannot exist returns null"
else
  bad "the control hash did not return null — the checks above are not meaningful"
fi
GHOST=$(kv "$(python3 scripts/use-cases/notary-key.py "a-document-nobody-notarised")" pubkey)
# The positive half FIRST, and it is not decoration. "The ghost key is not in
# the manifest" is an absence, and an absence is satisfied by an empty manifest,
# an unreadable one, a manifest with no pubkey column, and a grep that would
# find nothing whatever it was asked for. This control printed OK on a run
# against a dead chain that verified nothing at all. So show the same grep
# finding a key that IS there before believing it about one that is not — the
# shape 01-file-vault.sh already uses for its plaintext marker.
REAL=$(column_of "$MANIFEST" pubkey 2>/dev/null | grep -m1 .)
if [ -z "$GHOST" ]; then
  bad "control: the un-notarised document derived no key, so this control is empty"
elif [ -z "$REAL" ]; then
  bad "control: $MANIFEST carries no pubkey to search for — an absence here would prove nothing"
elif ! grep -q "$REAL" "$MANIFEST"; then
  bad "control: the search cannot even find a key the manifest records — it is broken"
elif grep -q "$GHOST" "$MANIFEST"; then
  bad "control: an un-notarised document's key is in the manifest"
else
  ok "control: the same search finds a recorded key and not the un-notarised one"
fi

rule "7. what is proved, and what is not"
# Quoted, because the text below names an instruction that does not exist and an
# unquoted heredoc would try to run it.
cat <<'TXT'
   Proved: a transaction signed by the key that this document's content address
   derives is in a block on the public testnet. Producing it required the
   content address, which required the document. The block orders it against
   every other transaction the chain has ever accepted, which is the timestamp.

   Private: the chain holds a public key and an account id. Neither reveals the
   document, its content address, or its hash. A verifier who is GIVEN the
   document can check the whole chain of reasoning; one who is not, cannot even
   tell what kind of thing was notarised.

   Not proved, and not claimed: the content address is not stored on chain as
   data. The deployed agent_verifier program has no instruction that writes
   arbitrary bytes, so the binding is through the signing key rather than
   through a stored record. A program with a `notarise(address)` instruction
   would make the address readable on chain — and would make it public, which
   is the property this use case is named for.
TXT

echo
finish "Use case 4 holds: a document was stored on a real Logos Storage node under a
content address, that address was turned into a key, and a transaction signed by
that key is in a block on the public testnet — verifiable by anyone holding the
document and opaque to everyone else. Manifest: $MANIFEST"
