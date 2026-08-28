#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# The five skills that had never been run against a node, run against nodes —
# and called as SKILLS, through the module's own `invoke()`.
#
#   ./scripts/skills-live.sh
#
# WHAT WAS MISSING
#
# `docs/skills.md` disclosed it rather than hiding it, and the disclosure was
# narrower than the consequence: `storage.upload`, `storage.download`,
# `storage.list`, `storage.share` and `messaging.create_group` were written
# against the real APIs, compiled, and tested against fake ports. The two things
# that existed sat either side of them. `scripts/exercise-nodes.sh` drives the
# Storage and Delivery LIBRARIES through C drivers — the node proven, not the
# skill. `scripts/use-cases/01-file-vault.sh` drives the storage library through
# `vault_drive.c`, so the "personal file vault" use case never called a storage
# skill at all. Between the two, nothing had ever asked
# `AgentModuleImpl::invoke("storage.upload", …)` and watched a content address
# come back.
#
# WHAT RUNNING IT FOUND
#
# `messaging.create_group` and `messaging.join` used to disagree about which
# topic a group lives on. `deliveryPort()` passed the group id where the CONTENT
# TOPIC goes and `JoinSkill` subscribed `discoveryTopic(group)`, so a joiner never
# met the channel. Both were one-line source changes; both are made, which makes
# the shipped `.lgx` stale and forces a repackage — done, all three variants, and
# `docs/skills.md` states the resulting position. `owner_channel.cpp` passes
# `ownerTopic(account)` and has always worked live, which is why nothing else
# caught either one.
#
# So there are two passes, and they answer two different questions:
#
#   A. module-port  the storage skills against a real Storage node, and
#                   `messaging.create_group` against the port the module builds
#                   for itself — which now OPENS the channel on `groupTopic`, and
#                   that success is asserted.
#   B. host-port    `messaging.create_group` against a DeliveryPort a host
#                   supplies. The channel opens, both members are invited, and the
#                   invitations come back off the members' own topics.
#
# WHAT IT COSTS: nothing. Storage and Messaging, no chain, no transaction, no
# balance moved. It is separate from `demo.sh` for the same reason
# `exercise-nodes.sh` is: `demo.sh` must run from a clean clone with nothing
# installed, and this needs both libraries built from source.
#
# WHAT THIS DOES NOT PROVE, stated here rather than left to be noticed:
#
#   - THAT A LOADED PLUGIN WORKS. This driver LINKS the module; it does not load
#     `module/agent.lgx`. The distinction mattered enormously when this header
#     was written, because `installBuiltinSkills` then had nothing to fill
#     `ports.storage` from and a loaded plugin refused every `storage.*` call
#     while this driver went green. That gap is closed — `StorageRuntime` now
#     matches `DeliveryRuntime` and the module opens its own node on
#     `meta.configure("storage","on")` — but the closing is not proved HERE. It
#     is proved by `scripts/logos-core-headless.sh`, which installs the package
#     and drives the extracted plugin through the runtime's own transport.
#     What this driver proves is that the behaviour holds in the sources the
#     package is built from; `scripts/check-package-fresh.py` ties those sources
#     to the shipped bytes.
#   - The shares and invitations below are read back off the topics they were
#     published on, by the node that published them. A Waku node receives its own
#     publications, so that proves the frame went through the node's relay path
#     and came back — not that a second peer got it. Two processes, two nodes, is
#     `./scripts/delivery-in-plugin.sh peers`.
#
# Environment:
#   STORAGE_SRC          logos-storage-nim checkout. Default _external/logos-storage-nim
#   DELIVERY_SRC         logos-delivery checkout.    Default _external/logos-delivery
#   LOGOS_CPP_SDK_ROOT   logos-cpp-sdk checkout.     Default $HOME/logos/src/logos-cpp-sdk
#   NLOHMANN_INCLUDE     directory holding nlohmann/json.hpp
#   CXX                  default: c++
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `cd ""` SUCCEEDS in bash, so `cd "$ROOT" || exit` cannot fire on the failure
# that can actually happen: the subshell failing leaves ROOT empty and every
# relative path below resolves against wherever the caller stood. Guard the
# variable, not the cd.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }
cd "$ROOT" || { echo "cannot enter $ROOT" >&2; exit 1; }
# `rule`/`ok`/`bad`/`note`/`die`/`finish`, and the house rule that a check which
# cannot fail is not a check. Sourced from the use-case plumbing because this
# transcript should read like use case 1's — the script whose gap this closes.
. scripts/use-cases/lib.sh

STORAGE_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-nim}"
DELIVERY_SRC="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
CXX="${CXX:-c++}"

# `DL` is a plain string and is expanded unquoted below. It was an array, which
# is the tidier form everywhere except here: under `set -u`, bash 3.2 — the bash
# macOS ships and the one this runs under — treats `"${DL[@]}"` on an EMPTY array
# as an unbound variable and exits. The symptom is a build that never starts, on
# the platform both libraries are built for.
case "$(uname -s)" in
  Darwin) NLOHMANN_INCLUDE="${NLOHMANN_INCLUDE:-/opt/homebrew/include}"; DL="" ;;
  *)      NLOHMANN_INCLUDE="${NLOHMANN_INCLUDE:-/usr/include}";          DL="-ldl" ;;
esac

SLIB="$STORAGE_SRC/build/libstorage.dylib";        [ -f "$SLIB" ] || SLIB="$STORAGE_SRC/build/libstorage.so"
DLIB="$DELIVERY_SRC/build/liblogosdelivery.dylib"; [ -f "$DLIB" ] || DLIB="$DELIVERY_SRC/build/liblogosdelivery.so"

rule "0. what this needs"
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
[ -f "$SDK/cpp/logos_module_context.h" ] || die "logos-cpp-sdk not at $SDK — set LOGOS_CPP_SDK_ROOT"
[ -f "$NLOHMANN_INCLUDE/nlohmann/json.hpp" ] || [ -f /usr/include/nlohmann/json.hpp ] \
  || die "nlohmann/json.hpp not found; set NLOHMANN_INCLUDE"
echo "  storage   $SLIB"
echo "  delivery  $DLIB"
echo "  sdk       $SDK"

# THE WORK DIRECTORY IS RESOLVED WITH `pwd -P`, AND THAT IS NOT TIDINESS.
#
# `$TMPDIR` on macOS ends in a slash, so `${TMPDIR}/lp0008-skills-live` contains
# `//`. Hand that to a Logos Storage node as its `data-dir` and EVERY upload
# fails with
#
#   Unable to store block … err="Path is outside of `root` directory!"
#
# because the datastore keeps the root as given and compares it against a block
# path that has been normalised. Reproduced on `vault_drive.c` outside this
# script, with and without the double slash, changing nothing else. The
# use-case script never hit it only because it passes `./store` and lets the node
# resolve it against the working directory. This is a node that is up, answering,
# reporting a peer id, and refusing every write — so it reads as the skill being
# broken.
WORK="${TMPDIR:-/tmp}/lp0008-skills-live"
rm -rf "$WORK"; mkdir -p "$WORK/store"
WORK="$(cd "$WORK" && pwd -P)"

rule "1. build the driver against the module, unmodified"
# The source list is `module/CMakeLists.txt`'s, whole. That is what makes this an
# exercise of the shipped module rather than of a subset compiled for the
# occasion — and `-DLP0008_WITH_DELIVERY` is what lets the module open its own
# Delivery node, exactly as the packaged plugin is built.
#
# NOTHING LINKS THE STORAGE LIBRARY, AND THAT IS THE POINT.
#
# `-lstorage` and `-I$STORAGE_SRC/library` used to be on this line, because the
# driver opened a Storage node of its own and handed the module a port. It does
# not any more: `module/src/storage_skills.cpp` opens the node, and it opens it
# with `dlopen` — the same run-time resolution a plugin loaded by Basecamp uses,
# and for the same reason (`liblogosdelivery.h`'s note: a link-time dependency
# turns one missing file into a plugin that will not load at all, which Basecamp
# reports to nobody). So the driver here has no storage symbols in it and could
# not open a node if it tried. `LP0008_STORAGE_LIB` below is what tells the
# module where to look, exactly as an operator would.
BIN="$WORK/skills-live-drive"
before="$(git status --porcelain module/ 2>/dev/null || true)"
"$CXX" -std=c++17 -Wall -Wextra -O1 -DLP0008_WITH_DELIVERY \
  -I"$ROOT/module/src" -I"$SDK/cpp" -I"$NLOHMANN_INCLUDE" \
  -I"$DELIVERY_SRC/library" \
  scripts/skills-live-drive.cpp \
  module/src/agent_module_plugin.cpp \
  module/src/delivery_runtime.cpp \
  module/src/spend_marker.cpp \
  module/src/messaging_skills.cpp \
  module/src/storage_skills.cpp \
  module/src/inference.cpp \
  module/src/wallet_skills.cpp \
  module/src/program_skills.cpp \
  module/src/agent_skills.cpp \
  module/src/owner_channel.cpp \
  module/src/owner_skills.cpp \
  module/src/task_persistence.cpp \
  $DL \
  -o "$BIN" || die "the driver did not compile"
ok "$BIN"

rule "2. the file the owner hands the agent"
# A marker that will not occur by chance, so "the bytes came back" is a claim
# about this file and not about the alphabet.
MARKER="lp0008-skills-$(head -c8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
RUN_ID="$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
IN="$WORK/owner-document.txt"
OUT="$WORK/retrieved.txt"
CONTROL_OUT="$WORK/control-retrieved.txt"
cat > "$IN" <<TXT
LP-0008 owner document
marker: $MARKER
run: $RUN_ID
The agent is asked to put this on Logos Storage and to give the address back.
TXT
IN_SHA=$(shasum -a 256 "$IN" | cut -d' ' -f1)
IN_BYTES=$(wc -c < "$IN" | tr -d ' ')
echo "  $IN  ($IN_BYTES bytes)"
echo "  sha256  $IN_SHA"
echo "  marker  $MARKER"
# An empty input would make the digest comparison in step 4 true of two empty
# files, which is the shape 01-file-vault.sh's own comments warn about: a verdict
# that reads true on an absence.
if [ "$IN_BYTES" -gt 0 ]; then ok "there is something to store"
else bad "the input file is empty, so nothing below is a comparison"; fi

SHOW='^[0-9]+\.|^mode: |^  (ok|FAIL|<-)|^(ADDRESS|GHOST|RETRIEVED|CONTROL|SHARED|GROUP|MODULE-PORT-CREATE-GROUP) |failure'

rule "3. pass A — the storage skills, and create_group through the module's own port"
# Every step in the driver is an assertion and its exit code is the result, so a
# node that silently failed to start cannot produce a passing transcript. This
# takes a few minutes: joining the public Delivery network is tens of seconds,
# and the storage node is started, written to and read back before that.
LOG_A="$WORK/pass-a.log"
export LP0008_DELIVERY_LIB="$DLIB"
export LP0008_STORAGE_LIB="$SLIB"
( cd "$WORK" && "$BIN" module-port "$RUN_ID" "$WORK/store" "$IN" "$OUT" "$CONTROL_OUT" ) \
  > "$LOG_A" 2>&1; rc_a=$?
grep -E "$SHOW" "$LOG_A" | sed 's/^/  /'
if [ "$rc_a" -eq 0 ]; then ok "every assertion in pass A held"
else bad "pass A failed — full log at $LOG_A"; fi

ADDRESS=$(awk '/^ADDRESS /{print $2}' "$LOG_A")
GHOST=$(awk '/^GHOST /{print $2}' "$LOG_A")
if [ -n "$ADDRESS" ]; then ok "storage.upload answered with $ADDRESS"
else bad "storage.upload produced no content address"; fi
if [ -n "$GHOST" ] && [ "$GHOST" != "$ADDRESS" ]; then
  ok "the control address differs from it: $GHOST"
else
  bad "there is no control address, so the refusals above prove nothing"
fi

rule "4. the digest, computed outside the driver"
# The part the driver cannot be trusted with: `shasum -a 256` over the file that
# went in and the file that came back, and the same comparison shown refusing
# altered input. A driver comparing its own bytes to its own bytes would pass
# whether or not a node was ever involved.
if [ -s "$OUT" ]; then
  OUT_SHA=$(shasum -a 256 "$OUT" | cut -d' ' -f1)
  echo "  retrieved $(wc -c < "$OUT" | tr -d ' ') bytes, sha256 $OUT_SHA"
  if [ "$OUT_SHA" = "$IN_SHA" ]; then
    ok "storage.download returned byte-for-byte what storage.upload was given"
  else
    bad "the bytes differ: $OUT_SHA vs $IN_SHA"
  fi
  # THE CONTROL. Alter the retrieved copy and run the identical comparison: it
  # must now refuse. Without this, the line above is equally consistent with a
  # comparison that says yes to everything.
  cp "$OUT" "$WORK/tampered.txt"
  printf 'x' >> "$WORK/tampered.txt"
  TAMPERED_SHA=$(shasum -a 256 "$WORK/tampered.txt" | cut -d' ' -f1)
  if [ "$TAMPERED_SHA" != "$IN_SHA" ]; then
    ok "control: the same comparison refuses altered bytes ($TAMPERED_SHA)"
  else
    bad "control: altered bytes hash the same, so the comparison is broken"
  fi
  # And the grep has to be shown capable of finding the marker at all.
  if grep -q "$MARKER" "$IN"; then ok "control: the marker is findable in the original"
  else bad "control: the marker is not even in the original — this check is broken"; fi
  if grep -q "$MARKER" "$OUT"; then ok "the marker is in what came back off the node"
  else bad "what came back does not contain the marker"; fi
else
  bad "storage.download wrote nothing to $OUT"
fi

rule "5. and the address that cannot resolve wrote nothing"
# `storage.download` refused it — the driver asserted that. What is checked here
# is the other half: that the refusal was not a refusal *after* writing a file. A
# skill that answered ok:false and left bytes on disk would pass every assertion
# in the driver.
if [ -e "$CONTROL_OUT" ]; then
  bad "the refused download still created $CONTROL_OUT ($(wc -c < "$CONTROL_OUT" | tr -d ' ') bytes)"
else
  ok "no file exists at $CONTROL_OUT"
fi

rule "6. pass B — create_group through a DeliveryPort a host supplies"
# One node again, and this time no storage node at all. The port differs from
# `DeliveryRuntime::deliveryPort()`'s in one line — the content topic the channel
# is opened on. The skill opens a channel, invites two members, and their
# invitations come back off their own owner topics, the same as it does through
# the module's own port in pass A. The bare-id call is made directly on the same
# node, so the difference is measured rather than argued.
LOG_B="$WORK/pass-b.log"
( cd "$WORK" && "$BIN" host-port "$RUN_ID" "$WORK/store" "$IN" "$OUT" "$CONTROL_OUT" ) \
  > "$LOG_B" 2>&1; rc_b=$?
grep -E "$SHOW" "$LOG_B" | sed 's/^/  /'
if [ "$rc_b" -eq 0 ]; then ok "every assertion in pass B held"
else bad "pass B failed — full log at $LOG_B"; fi
GROUP=$(awk '/^GROUP /{print $2}' "$LOG_B")
if [ -n "$GROUP" ]; then ok "messaging.create_group opened channel $GROUP"
else bad "no channel was opened"; fi

rule "7. the negative control — pass A can fail"
# Everything above is a claim that the MODULE wires its own storage port. The
# claim is only worth what its falsification is worth, so: put the module back in
# the state it was in before that wiring existed — `ports.storage` consumed
# verbatim, which is exactly what `installBuiltinSkills` did — rebuild, and
# assert the run goes red.
#
# Without this, "the module self-wires storage" is consistent with a driver that
# would pass against any module at all, and this repository has twice shipped an
# assertion that could not fail.
#
# The mutation is written OUTSIDE module/, so step 8 below still means what it
# says. `! diff` on a line of its own is not a guard — under `set -e` bash does
# not exit when a return value is inverted with `!` — so it is written as
# `! diff … || { …; exit 1; }`, which is what makes the inversion load-bearing.
MUT="$WORK/unwired_plugin.cpp"
sed 's|        ports.storage = store->storagePort();|        (void)store;|' \
  module/src/agent_module_plugin.cpp > "$MUT"
if ! diff -q module/src/agent_module_plugin.cpp "$MUT" > /dev/null; then
  ok "the control removes the storage self-wiring"
else
  bad "the control changed nothing, so it tests nothing"
fi
MUT_BIN="$WORK/skills-live-drive-unwired"
"$CXX" -std=c++17 -w -O1 -DLP0008_WITH_DELIVERY \
  -I"$ROOT/module/src" -I"$SDK/cpp" -I"$NLOHMANN_INCLUDE" \
  -I"$DELIVERY_SRC/library" \
  scripts/skills-live-drive.cpp \
  "$MUT" \
  module/src/delivery_runtime.cpp \
  module/src/spend_marker.cpp \
  module/src/messaging_skills.cpp \
  module/src/storage_skills.cpp \
  module/src/inference.cpp \
  module/src/wallet_skills.cpp \
  module/src/program_skills.cpp \
  module/src/agent_skills.cpp \
  module/src/owner_channel.cpp \
  module/src/owner_skills.cpp \
  module/src/task_persistence.cpp \
  $DL \
  -o "$MUT_BIN" || die "the control did not compile"
LOG_C="$WORK/pass-control.log"
rm -rf "$WORK/store-control"; mkdir -p "$WORK/store-control"
( cd "$WORK" && "$MUT_BIN" module-port "${RUN_ID}c" "$WORK/store-control" "$IN" \
    "$WORK/control-out.txt" "$WORK/control-ctl.txt" ) > "$LOG_C" 2>&1; rc_c=$?
# Red FOR THE RIGHT REASON. A non-zero exit alone is satisfied by a binary that
# dies before it runs a single assertion — a missing library, an abort in a
# static initialiser — so the failure count and the failing line are both read.
fails_c=$(grep -c '^  FAIL  ' "$LOG_C" || true)
if [ "$rc_c" -ne 0 ] && [ "$fails_c" -gt 0 ]; then
  ok "the unwired module fails pass A: $fails_c assertion(s), exit $rc_c"
  grep -m3 '^  FAIL  ' "$LOG_C" | sed 's/^/    /'
else
  bad "a module with no storage self-wiring PASSED pass A — the run above proves nothing"
fi
# And it must fail for the RIGHT reason, which is a precise and unusual pair: the
# node still comes up — `meta.status` is wired to the runtime either way, so the
# control leaves that half alone — and the skill still cannot reach it. That is
# the signature of a missing port and of nothing else. A control that only
# asserted "it went red" would be satisfied by a node that failed to start.
if grep -qF "ok    the module's own Storage node came up and reported it ready" "$LOG_C" \
   && grep -qF 'FAIL  the skill answered ok' "$LOG_C"; then
  ok "and for the right reason: the node came up, and storage.upload still could not reach it"
  grep -m1 -F 'storage.upload: ' "$LOG_C" | sed 's/^/    /'
else
  bad "it failed somewhere other than the storage port — see $LOG_C"
fi

rule "8. nothing under module/ was touched"
# The same check `examples/agent-console/run.sh` makes, for the same reason: this
# is evidence about the module as it ships, and a run that edited it would be
# evidence about something else.
after="$(git status --porcelain module/ 2>/dev/null || true)"
if [ "$before" = "$after" ]; then ok "module/ is as it was before the run"
else bad "the run modified module/: $(diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | tr '\n' ' ')"; fi

rule "9. what was on chain here"
cat <<'TXT'
   Nothing. This is Logos Storage and Logos Messaging against real nodes on the
   live dev network; no LEZ transaction was submitted and no balance moved.
TXT

echo
echo "logs: $LOG_A"
echo "      $LOG_B"
finish "storage.upload, storage.download, storage.list and storage.share were each
called through AgentModuleImpl::invoke() against a Logos Storage node THE MODULE
OPENED FOR ITSELF, from a SkillPorts that was completely empty: an address came
back, the bytes came back and matched shasum computed outside the driver, an
address the node does not hold was refused by both the download and the share
and left no file behind, and the shared address travelled over a real Delivery
node the module also opened. messaging.create_group opened a real reliable
channel and invited two members through the module's OWN port, which it could not
do before, and the bare-id control keeps that difference measured on a live node.
The driver links no storage library: the module found one with dlopen, which is
the path a plugin loaded by Basecamp takes."
