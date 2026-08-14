#!/usr/bin/env bash
# Drive a real Delivery node and a real Storage node through the agent skills.
#
#   ./scripts/exercise-nodes.sh
#
# This is the step that turns "the skills compile and their logic is tested"
# into "a message went out and a file came back". Until it has run, docs/skills.md
# says the skills are not exercised, and that stays true.
#
# It is a separate script from demo.sh on purpose: demo.sh must run from a clean
# clone with nothing installed, and this needs two Logos modules built.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery-module}"
STORAGE_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-module}"

die() { echo "error: $*" >&2; exit 1; }
say() { printf '\n\033[1m%s\033[0m\n' "$*"; }

say "[1/4] the two modules"
for pair in "delivery:$DELIVERY_SRC" "storage:$STORAGE_SRC"; do
  name="${pair%%:*}"; src="${pair#*:}"
  if [ ! -d "$src" ]; then
    echo "  $name missing at $src"
    echo "    git clone https://github.com/logos-co/logos-$name-module $src"
    die "clone the modules first, or set DELIVERY_SRC / STORAGE_SRC"
  fi
  echo "  $name at $(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo '?')"
done

say "[2/4] a Delivery node on the test network"
# entryLayer "channels" is the default and the one create_group needs: a bare
# topic would drop group messages silently. `preset` picks the network and
# `mode` the protocol flags — Edge is a light node, which is what an agent is.
DELIVERY_CFG=$(cat <<'JSON'
{ "entryLayer": "channels", "mode": "Edge", "preset": "logos.test" }
JSON
)
echo "  config: $DELIVERY_CFG"
echo "  createNode -> start -> wait for nodeStarted"
echo "  (start returns once dispatched; waiting for the event is the whole point)"

say "[3/4] a Storage node"
echo "  init -> start, then uploadUrl a small file and read the content address back"

say "[4/4] through the skills, not around them"
cat <<'TXT'
   messaging.send      to the owner topic, and confirm messageSent
   messaging.join      subscribe to a discovery topic
   storage.upload      a temp file, keep the returned address
   storage.download    that address to a new path, and diff the bytes
   storage.share       the address to a second agent over Delivery
TXT

echo
echo "NOT IMPLEMENTED YET — this script records the shape of the run, not a result."
echo "Writing it as if it had run is exactly the failure that closed five earlier"
echo "LP-0008 submissions, so it exits non-zero until it really drives the nodes."
exit 1
