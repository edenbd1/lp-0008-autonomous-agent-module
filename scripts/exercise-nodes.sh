#!/usr/bin/env bash
# Drive a real Logos Delivery node through the lifecycle the agent skills use.
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
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
DYLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"
[ -f "$DYLIB" ] || DYLIB="$DELIVERY_SRC/build/liblogosdelivery.so"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

say "[1/3] the Delivery library"
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

say "[2/3] build the driver against it"
BIN="${TMPDIR:-/tmp}/lp0008_delivery_drive"
cc -o "$BIN" module/tests/delivery_node_drive.c \
   -I"$DELIVERY_SRC/library" -L"$DELIVERY_SRC/build" \
   -llogosdelivery -Wl,-rpath,"$DELIVERY_SRC/build" \
  || die "the driver did not compile"
echo "  $BIN"

say "[3/3] run it against the live network"
# Every step in the driver is an assertion and the exit code is the result, so
# a node that silently failed to start cannot produce a passing transcript.
LOG="${TMPDIR:-/tmp}/lp0008_delivery_drive.log"
"$BIN" > "$LOG" 2>&1; rc=$?
grep -E "^[0-9]\.|^  (ok|FAIL|<-|event)|failure|confirmed" "$LOG"
echo
if [ $rc -ne 0 ]; then
  echo "FAILED — full log at $LOG" >&2
  exit 1
fi
echo "A real node started, joined the network, published a message that the"
echo "network propagated back, and shut down. Full log: $LOG"
