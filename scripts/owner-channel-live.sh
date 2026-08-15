#!/usr/bin/env bash
# Run the owner channel between two separate processes, each with its own Logos
# Delivery node, on the public network.
#
#   ./scripts/owner-channel-live.sh              # the exchange
#   ./scripts/owner-channel-live.sh --negatives  # and the ways it can fail
#
# The prize asks that "the owner can interact with the agent in real time from a
# separate Logos app instance using Logos Messaging, with no intermediary
# server". Everything the module had before this was tested against a fake port:
# a std::function answering in-process, which cannot tell a working channel from
# a plausible one. This starts two processes that share nothing but a content
# topic on the public network, and runs the module's own
# `logos::agent::OwnerChannel` across the gap.
#
# Separate from demo.sh for the same reason exercise-nodes.sh is: demo.sh must
# run from a clean clone with nothing installed, and this needs the Delivery
# library built from source.
#
# WHERE THE TIME GOES
#
# Almost all of it is two nodes joining the public network. The exchange itself
# is sub-second once they are up: 312 ms, first attempt.
#
# It was not always, and the reason is worth keeping written down because it
# looked exactly like a flaky network. Run from one directory, both nodes write
# and read the same `data/sds.db` — the reliable channel's own state store — and
# the first frame is then routinely the one that does not arrive: five channel
# sends and four deliveries in a bare probe, and 12.6 seconds over two attempts
# here, twice. Nothing reports a conflict; the message is simply not delivered,
# and the agent's retry quietly covers for it. `run_in` below gives each process
# a directory of its own, and the retry stops being load-bearing.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
DYLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"
[ -f "$DYLIB" ] || DYLIB="$DELIVERY_SRC/build/liblogosdelivery.so"
INC="${NLOHMANN_INC:-/opt/homebrew/include}"
[ -d "$INC/nlohmann" ] || INC=/usr/include

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

say "[2/4] build the driver against the module's own owner channel"
# owner_channel.cpp is the module's, not a copy: what runs across the network is
# the class the plugin links, with its port wired to the real library instead of
# to a fake.
BIN="${TMPDIR:-/tmp}/lp0008_owner_channel_live"
clang++ -std=c++17 -Wall -Wextra -O1 -o "$BIN" \
    module/tests/owner_channel_live.cpp \
    module/src/owner_channel.cpp module/src/messaging_skills.cpp \
    -I"$INC" -I"$DELIVERY_SRC/library" -L"$DELIVERY_SRC/build" \
    -llogosdelivery -Wl,-rpath,"$DELIVERY_SRC/build" \
  || die "the driver did not compile"
echo "  $BIN"

LOGDIR="${TMPDIR:-/tmp}/lp0008-owner-live"; mkdir -p "$LOGDIR"
# Fresh per run: the terms are derived from it, so a frame left on the public
# topic by an earlier run names a different payment and is ignored rather than
# settling this one.
RUN_ID="${RUN_ID:-$(LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 10)}"

# Only the assertions and the wire lines. The node library logs a few thousand
# INF/WRN lines per run to stdout; the full log is kept and named at the end.
quiet() { grep -E '^(  (ok|FAIL|\.\.|<-|->)|role |agent:|owner:|[a-z].* failure\(s\))' || true; }

# Each process gets a working directory of its own, and that is not tidiness.
# A Delivery node writes `data/sds.db` into the CWD, so two nodes started from
# one directory share the reliable channel's own state store — which is the
# single thing these two processes must not share, since "each with its own
# node" is the claim being made. Run from the repository root they also leave a
# 6 MB datastore in the checkout.
run_in() { # dir binary-args...
  local dir="$1"; shift
  rm -rf "$dir"; mkdir -p "$dir"
  ( cd "$dir" && exec "$BIN" "$@" )
}

# Start the owner first: it is the one that has to be listening before there is
# anything to hear. `wait` on the pid, so the exit code is the process's own.
two_process() { # label extra-owner-args...
  local label="$1"; shift
  local olog="$LOGDIR/$label-owner.log" alog="$LOGDIR/$label-agent.log"
  local rid="${RUN_ID}${label//-/}"
  run_in "$LOGDIR/$label-owner.d" owner "$rid" "$@" > "$olog" 2>&1 &
  local opid=$!
  sleep 20
  run_in "$LOGDIR/$label-agent.d" agent "$rid" > "$alog" 2>&1 &
  local apid=$!
  wait "$apid"; arc=$?
  wait "$opid"; orc=$?
  echo "--- owner (pid $opid) ---"; quiet < "$olog"
  echo "--- agent (pid $apid) ---"; quiet < "$alog"
  echo "exit: owner=$orc agent=$arc"
}

say "[3/4] the exchange: two processes, two nodes, one owner topic"
two_process live
[ "$arc" -eq 0 ] && [ "$orc" -eq 0 ] \
  || { echo "FAILED — logs in $LOGDIR" >&2; exit 1; }

if [ "${1:-}" != "--negatives" ]; then
  say "[4/4] done"
  echo "The agent's verdict came back approved because a frame naming this"
  echo "request's id, policy hash, recipient, amount, nonce and marker seed"
  echo "arrived from the other process. Re-run with --negatives to watch the"
  echo "same harness fail."
  echo "Logs: $LOGDIR"
  exit 0
fi

say "[4/4] the ways it fails"
fails=0

echo
echo "(a) the node never started — open() must refuse rather than pretend"
run_in "$LOGDIR/nostart.d" agent "${RUN_ID}nostart" --no-start > "$LOGDIR/nostart.log" 2>&1; rc=$?
quiet < "$LOGDIR/nostart.log"; echo "exit: $rc"
[ "$rc" -eq 0 ] || { echo "  the refusal itself regressed" >&2; fails=$((fails+1)); }

echo
echo "(b) nobody is listening — the agent must time out, not settle itself"
echo "    (a node receives its own messages, so this is the check that the"
echo "     agent cannot approve its own request)"
LP0008_TIMEOUT_MS=70000 LP0008_RESEND_MS=15000 \
  run_in "$LOGDIR/alone.d" agent "${RUN_ID}alone" > "$LOGDIR/alone.log" 2>&1; rc=$?
quiet < "$LOGDIR/alone.log"; echo "exit: $rc"
[ "$rc" -ne 0 ] && grep -q '"verdict":"unreachable"' "$LOGDIR/alone.log" \
  || { echo "  EXPECTED a non-zero exit and verdict unreachable" >&2; fails=$((fails+1)); }

echo
echo "(c) the owner answers with a different amount — must be refused, not taken"
export LP0008_TIMEOUT_MS=90000 LP0008_RESEND_MS=12000
two_process altered --alter
unset LP0008_TIMEOUT_MS LP0008_RESEND_MS
grep -q '"verdict":"refused"' "$LOGDIR/altered-agent.log" \
  || { echo "  EXPECTED verdict refused" >&2; fails=$((fails+1)); }
[ "$arc" -ne 0 ] || { echo "  EXPECTED the agent to exit non-zero" >&2; fails=$((fails+1)); }

echo
if [ "$fails" -ne 0 ]; then
  echo "$fails negative case(s) did not fail the way they must" >&2
  echo "Logs: $LOGDIR" >&2
  exit 1
fi
echo "All three negatives behaved: the passing run above is not free."
echo "Logs: $LOGDIR"
