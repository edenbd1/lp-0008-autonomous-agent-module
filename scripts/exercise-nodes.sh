#!/usr/bin/env bash
# Drive a real Logos Delivery node AND a real Logos Storage node through the
# lifecycles the agent skills use. Four steps, and the Storage half is the
# second two — this line named only Delivery while step [4/4] has always driven
# storage_node_drive.c.
#
#   ./scripts/exercise-nodes.sh
#
# This is the step that turns "the skills compile and their logic is tested"
# into "a message went out". It is separate from demo.sh on purpose: demo.sh
# must run from a clean clone with nothing installed, and this needs the
# Delivery library built from source.
#
# No Nix. The module's flake pulls prebuilt artifacts from a binary cache, but
# the library underneath it is an ordinary Nim project with a Makefile, and
# `librln` is an ordinary cargo build. Both were reachable all along; the
# earlier note in docs/skills.md claiming this needed a Nix install was wrong,
# and wrong because the question "where does liblogosdelivery come from" was
# never asked.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }

DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
DYLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"
[ -f "$DYLIB" ] || DYLIB="$DELIVERY_SRC/build/liblogosdelivery.so"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

say "[1/4] the Delivery library"
if [ ! -f "$DYLIB" ]; then
  cat >&2 <<TXT
  not built at $DYLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules \\
      https://github.com/logos-messaging/logos-delivery $DELIVERY_SRC
  cd $DELIVERY_SRC && make liblogosdelivery

  nimble lands in ~/.nimble/bin and is not on PATH afterwards, so:
      export PATH="\$HOME/.nimble/bin:\$PATH"
TXT
  die "build the Delivery library first, or set DELIVERY_SRC"
fi
echo "  $DYLIB"
echo "  $(cd "$DELIVERY_SRC" && git rev-parse --short HEAD 2>/dev/null || echo '?')  $(du -h "$DYLIB" | cut -f1)"

say "[2/4] build the driver against it"
BIN="${TMPDIR:-/tmp}/lp0008_delivery_drive"
cc -o "$BIN" module/tests/delivery_node_drive.c \
   -I"$DELIVERY_SRC/library" -L"$DELIVERY_SRC/build" \
   -llogosdelivery -Wl,-rpath,"$DELIVERY_SRC/build" \
  || die "the driver did not compile"
echo "  $BIN"

say "[3/4] run it against the live network"
# Every step in the driver is an assertion and the exit code is the result, so
# a node that silently failed to start cannot produce a passing transcript.
LOG="${TMPDIR:-/tmp}/lp0008_delivery_drive.log"
"$BIN" > "$LOG" 2>&1; rc=$?
grep -E "^[0-9]\.|^  (ok|FAIL|<-|event)|failure|confirmed" "$LOG"
[ $rc -eq 0 ] || { echo "FAILED — full log at $LOG" >&2; exit 1; }

say "[4/4] the same, for Storage"
STORAGE_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-nim}"
SLIB="$STORAGE_SRC/build/libstorage.dylib"
[ -f "$SLIB" ] || SLIB="$STORAGE_SRC/build/libstorage.so"
if [ ! -f "$SLIB" ]; then
  cat >&2 <<TXT
  not built at $SLIB

  git clone --depth 1 --recurse-submodules --shallow-submodules -b v0.4.4 \\
      https://github.com/logos-storage/logos-storage-nim $STORAGE_SRC
  cd $STORAGE_SRC && export PATH="\$HOME/.nimble/bin:\$PATH" && make libstorage
TXT
  die "build the Storage library first, or set STORAGE_SRC"
fi
SBIN="${TMPDIR:-/tmp}/lp0008_storage_drive"
cc -o "$SBIN" module/tests/storage_node_drive.c \
   -I"$STORAGE_SRC/library" -L"$STORAGE_SRC/build" \
   -lstorage -Wl,-rpath,"$STORAGE_SRC/build" \
  || die "the Storage driver did not compile"

# It writes a repo and an upload into the working directory, so give it one of
# its own rather than scattering a datastore through the checkout.
SLOG="${TMPDIR:-/tmp}/lp0008_storage_drive.log"
SRUN="${TMPDIR:-/tmp}/lp0008-storage-run"; mkdir -p "$SRUN"
( cd "$SRUN" && "$SBIN" ) > "$SLOG" 2>&1; src=$?
grep -E "^[0-9]\.|^  (ok|FAIL|<-)|failure|confirmed" "$SLOG"
echo
[ $src -eq 0 ] || { echo "FAILED — full log at $SLOG" >&2; exit 1; }

echo "Both nodes are real. Delivery started, joined the network, published a"
echo "message the network propagated back. Storage started, took a file, and"
echo "returned a content address whose manifest names that file."
echo "Logs: $LOG"
echo "      $SLOG"
