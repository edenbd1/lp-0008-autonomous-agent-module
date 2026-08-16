#!/usr/bin/env bash
# USE CASE 1 — a personal file vault: the owner gives the agent a file, the
# agent encrypts it, stores it on Logos Storage, and sends back the content
# address over Logos Messaging.
#
#   ./scripts/use-cases/01-file-vault.sh
#
# From the prize's list of illustrative use cases:
#
#   "Personal file vault: owner sends files to the agent via chat; agent
#    encrypts, stores on Logos Storage, and responds with a content address.
#    Owner can retrieve from any device."
#
# WHAT IT COSTS: nothing. Nothing here touches LEZ. This use case is Storage and
# Messaging, and both are driven against real nodes rather than a chain — which
# is why it is separate from the two on-chain scripts and why docs/use-cases.md
# says so in the first paragraph rather than the last.
#
# WHY IT IS NOT PART OF demo.sh: `demo.sh` must run from a clean clone with
# nothing installed. This needs the Logos Storage and Logos Delivery libraries
# built from source, exactly like scripts/exercise-nodes.sh, whose build
# instructions this reuses.
#
# WHAT IS HONEST TO SAY ABOUT "any device": the store and the retrieval below are
# driven against the same running node. What a second device needs is the
# ADDRESS, and the address is what Logos Messaging carries in step 4 — that part
# really does cross the network. A second Logos Storage node fetching the same
# address from the network is not demonstrated here.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
. scripts/use-cases/lib.sh

STORAGE_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-nim}"
DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
WORK="${TMPDIR:-/tmp}/lp0008-usecase-01"
TOPIC="${VAULT_TOPIC:-/lp0008/1/owner-vault/proto}"

SLIB="$STORAGE_SRC/build/libstorage.dylib"; [ -f "$SLIB" ] || SLIB="$STORAGE_SRC/build/libstorage.so"
DLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"; [ -f "$DLIB" ] || DLIB="$DELIVERY_SRC/build/liblogosdelivery.so"

rule "0. the two libraries this needs"
if [ ! -f "$SLIB" ]; then
  cat >&2 <<TXT
  no Logos Storage library at $SLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules -b v0.4.4 \\
      https://github.com/logos-storage/logos-storage-nim $STORAGE_SRC
  cd $STORAGE_SRC && export PATH="\$HOME/.nimble/bin:\$PATH" && make libstorage
TXT
  die "build the Storage library first, or set STORAGE_SRC"
fi
if [ ! -f "$DLIB" ]; then
  cat >&2 <<TXT
  no Logos Delivery library at $DLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules \\
      https://github.com/logos-messaging/logos-delivery $DELIVERY_SRC
  cd $DELIVERY_SRC && make liblogosdelivery

  nimble lands in ~/.nimble/bin and is not on PATH afterwards, so:
      export PATH="\$HOME/.nimble/bin:\$PATH"
TXT
  die "build the Delivery library first, or set DELIVERY_SRC"
fi
echo "  storage   $SLIB"
echo "  delivery  $DLIB"

rm -rf "$WORK"; mkdir -p "$WORK/store"
VBIN="$WORK/vault_drive"
SBIN="$WORK/share_drive"
cc -o "$VBIN" scripts/use-cases/vault_drive.c \
   -I"$STORAGE_SRC/library" -L"$STORAGE_SRC/build" -lstorage \
   -Wl,-rpath,"$STORAGE_SRC/build" || die "the storage driver did not compile"
cc -o "$SBIN" scripts/use-cases/share_drive.c \
   -I"$DELIVERY_SRC/library" -L"$DELIVERY_SRC/build" -llogosdelivery \
   -Wl,-rpath,"$DELIVERY_SRC/build" || die "the messaging driver did not compile"
ok "both drivers compiled against the real libraries"

rule "1. the owner's file"
# A marker that is not going to occur by chance, so "the plaintext is not in the
# ciphertext" is a check about this file and not about the alphabet.
MARKER="lp0008-vault-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
cat > "$WORK/secret.txt" <<TXT
LP-0008 owner document
marker: $MARKER
This is the sort of thing an owner hands an agent and does not want a storage
provider, or anyone watching the storage network, to be able to read.
TXT
PLAIN_SHA=$(shasum -a 256 "$WORK/secret.txt" | cut -d' ' -f1)
echo "  $WORK/secret.txt  ($(wc -c < "$WORK/secret.txt" | tr -d ' ') bytes)"
echo "  sha256  $PLAIN_SHA"
echo "  marker  $MARKER"

rule "2. the agent encrypts it before the node ever sees it"
# The key is generated here, per run, and never leaves this machine. This is the
# script's encryption and not the module's: `storage.upload` in module/src is a
# passthrough to the node today, so an agent that wants its vault encrypted has
# to encrypt before it calls the skill. That is a real gap between the prize's
# wording and this implementation, and it is written down in docs/use-cases.md
# rather than papered over by calling openssl "the agent".
head -c 32 /dev/urandom > "$WORK/vault.key"
openssl enc -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 -salt \
  -pass "file:$WORK/vault.key" -in "$WORK/secret.txt" -out "$WORK/secret.enc" \
  || die "encryption failed"
CIPHER_SHA=$(shasum -a 256 "$WORK/secret.enc" | cut -d' ' -f1)
echo "  aes-256-cbc, pbkdf2 200000 iterations, a 32-byte key made for this run"
echo "  sha256  $CIPHER_SHA"
# THE CIPHERTEXT HAS TO EXIST BEFORE IT CAN BE COMPARED. Both verdicts in this
# section are satisfied by a zero-byte file: sha256 of nothing is not sha256 of
# the document, so "not the plaintext" passes, and the marker is not in an empty
# file either, so "the marker does not appear" passes too. `openssl … || die`
# above catches a non-zero exit, and step 5's `[ -s retrieved.enc ]` catches the
# rest — so the SCRIPT could not go green on this, but these two lines could,
# and a check that reads true on an absence is not saying what it appears to.
# The size is asserted here rather than left to a later section to notice.
if [ ! -s "$WORK/secret.enc" ]; then
  bad "openssl produced no ciphertext at all, so nothing below is a comparison"
elif [ "$CIPHER_SHA" != "$PLAIN_SHA" ]; then ok "the ciphertext is not the plaintext"
else bad "the 'ciphertext' hashes to the same thing as the plaintext"; fi
# The grep has to be shown to be capable of finding the marker, or "not found in
# the ciphertext" is equally consistent with a grep that finds nothing anywhere.
if grep -q "$MARKER" "$WORK/secret.txt"; then
  ok "control: the marker is findable in the plaintext"
else
  bad "control: the marker is not even in the plaintext — this check is broken"
fi
if grep -q "$MARKER" "$WORK/secret.enc" 2>/dev/null; then
  bad "the marker survives into the ciphertext — this is not encryption"
else
  ok "the marker does not appear in the bytes the node is about to be handed"
fi

rule "3. the agent stores it on a real Logos Storage node"
# Every step in the driver is an assertion and its exit code is the result, so a
# node that silently failed to start cannot produce a passing transcript. The
# driver also runs the cannot-exist control: the real content address with one
# character changed, for which the node must be unable to produce the content.
#
# The control asks for the DATA, not for `storage_exists`. That flag answers
# `true` for the mutant — it is a datastore key lookup and a near miss can land
# on a key that is present — so the driver prints it and believes the manifest
# fetch instead, which fails with "Cid doesn't match the data". Written up in
# docs/use-cases.md; an exists-only control passes while proving nothing.
LOG="$WORK/storage.log"
( cd "$WORK" && "$VBIN" ./store ./secret.enc ./retrieved.enc ) > "$LOG" 2>&1; rc=$?
grep -E "^[0-9]\.|^  (ok|FAIL|<-)|^CID |failure" "$LOG" | sed 's/^/  /'
if [ "$rc" -eq 0 ]; then ok "the node stored it, and gave the address back"
else bad "the storage driver failed — full log at $LOG"; fi
CID=$(awk '/^CID /{print $2}' "$LOG")
if [ -n "$CID" ]; then ok "content address $CID"
else bad "no content address came back"; fi

rule "4. the agent sends the address to its owner over Logos Messaging"
# `storage.share(address, recipient)` is a messaging act, not a storage one:
# content addressing means there is nothing to copy, so sharing a file is
# sending its address. This is the only part of this use case that crosses a
# network to anyone else.
if [ -n "$CID" ]; then
  MLOG="$WORK/messaging.log"
  "$SBIN" "$TOPIC" "$CID" > "$MLOG" 2>&1; mrc=$?
  grep -E "^[0-9]\.|^  (ok|FAIL|<-|note|topic|payload|base64)|^  event .*message_(sent|propagated|error)" "$MLOG" \
    | sed 's/^/  /' | head -30
  if [ "$mrc" -eq 0 ]; then ok "the address reached the Logos Messaging network"
  else bad "the messaging driver failed — full log at $MLOG"; fi
  # The bytes on the wire have to be the address, not a description of it.
  WANT_B64=$(printf '%s' "$CID" | base64 | tr -d '\n')
  if grep -q "$WANT_B64" "$MLOG"; then
    ok "the published payload decodes to exactly the content address"
  else
    bad "what went out was not the content address"
  fi
else
  bad "nothing to share: there is no content address"
fi

rule "5. the owner retrieves it, by content address alone"
# The driver in step 3 already asked the node for the address back and streamed
# it to ./retrieved.enc. What is checked here is the part that matters to an
# owner: that the bytes are the same bytes, and that they decrypt.
if [ -s "$WORK/retrieved.enc" ]; then
  GOT_SHA=$(shasum -a 256 "$WORK/retrieved.enc" | cut -d' ' -f1)
  echo "  retrieved $(wc -c < "$WORK/retrieved.enc" | tr -d ' ') bytes, sha256 $GOT_SHA"
  if [ "$GOT_SHA" = "$CIPHER_SHA" ]; then ok "byte-for-byte the ciphertext that was stored"
  else bad "the node returned different bytes: $GOT_SHA vs $CIPHER_SHA"; fi
  if openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 \
       -pass "file:$WORK/vault.key" -in "$WORK/retrieved.enc" -out "$WORK/roundtrip.txt" 2>/dev/null; then
    ok "it decrypts with the owner's key"
  else
    bad "the retrieved file does not decrypt"
  fi
  RT_SHA=$(shasum -a 256 "$WORK/roundtrip.txt" 2>/dev/null | cut -d' ' -f1)
  if [ -n "$RT_SHA" ] && [ "$RT_SHA" = "$PLAIN_SHA" ]; then
    ok "and it is the owner's original document: sha256 $RT_SHA"
  else
    bad "the round trip produced ${RT_SHA:-nothing}, not $PLAIN_SHA"
  fi
  if grep -q "$MARKER" "$WORK/roundtrip.txt" 2>/dev/null; then
    ok "the marker is back"
  else
    bad "the decrypted file does not contain the marker"
  fi
else
  bad "the node streamed nothing back"
fi

rule "6. what was on chain here"
cat <<'TXT'
   Nothing. This use case is Logos Storage and Logos Messaging, driven against
   real nodes on the live dev network; no LEZ transaction was submitted and no
   balance moved. The on-chain half of the agent is use cases 2 and 3.
TXT

echo
echo "logs: $WORK"
finish "Use case 1 holds: the file was encrypted, stored under a content address on
a real Logos Storage node, that address travelled over a real Logos Messaging
topic, and the bytes came back and decrypted to the original."
