#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Can a module that Logos Core LOADS obtain a working Delivery port?
#
#   ./scripts/delivery-in-plugin.sh            # both single-process harnesses
#   ./scripts/delivery-in-plugin.sh peers      # two loaded modules, two nodes
#
# This script owns the paths, the way scripts/exercise-nodes.sh does. Nothing
# below is a rebuild made for the occasion: every harness runs against the
# plugin unpacked from the committed `module/agent.lgx`.
#
# WHAT IT ANSWERS
#
# `docs/basecamp.md` used to record, as a limitation, that "a host that loads
# this as a plugin cannot wire [the ports], because a port is a `std::function`
# and there is no wire format for one". That is a true statement about what a
# HOST can PASS, and it was read for months as a statement about what a MODULE
# can HAVE. It is not: the module links `liblogosdelivery` and opens a node from
# its own configuration, so the ports are built on the far side of the boundary
# and nothing crosses it but `meta.configure("delivery","on")`.
#
# THREE HARNESSES, AND WHY THREE
#
#  1. `plugin_delivery_test … probe` — QPluginLoader, in this process. The Qt
#     half of the contract.
#  2. `logos_core_delivery_test` — the real runtime, out of the installed
#     Basecamp: `logos_core_load_module`, then everything over Qt Remote Objects
#     into the separate `logos_host` process the module actually runs in. A core
#     module is not in the host's address space, so (1) does not imply (2).
#  3. `plugin_delivery_test … peer`, twice — two loaded modules, two nodes, two
#     LEZ accounts, one public topic, each accepting only a card naming the
#     OTHER account. (1) and (2) cannot be evidence for the discovery criterion:
#     a Waku node receives its own published messages, so a single process can
#     satisfy any assertion about "a card arrived" with every other agent on
#     earth switched off.
#
# THE NEGATIVE CONTROL is not optional and is one line: build without
# `-DLOGOS_DELIVERY_ROOT` and run harness 1. It reports
# `{"linked":false,"state":"absent"}` and fails at step 1, which is the check
# discriminating between two builds rather than describing one.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

MODE="${1:-single}"
QT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/macos}"
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
APP="${BASECAMP_APP:-/Applications/LogosBasecamp.app}"
WORK="${TMPDIR:-/tmp}/lp0008-delivery-in-plugin"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

[ -d "$QT/lib/QtCore.framework" ] || die "Qt 6.9.2 not at $QT — see docs/basecamp.md"
[ -f "$SDK/cpp/logos_types.cpp" ] || die "logos-cpp-sdk not at $SDK"
[ -f "module/agent.lgx" ]         || die "module/agent.lgx is missing"

QTINC=(-F"$QT/lib" -I"$QT/lib/QtCore.framework/Headers"
       -I"$QT/lib/QtRemoteObjects.framework/Headers"
       -I"$QT/lib/QtNetwork.framework/Headers")

say "[1/4] unpack the committed package"
# The artefact a reviewer downloads, not build-delivery/. `liblogosdelivery.dylib`
# has to land beside the plugin: the rpath is @loader_path and the host resolves
# it there and nowhere else.
MODDIR="$WORK/modules/agent"
rm -rf "$WORK/modules"; mkdir -p "$MODDIR"
( cd "$MODDIR" && tar xzf "$ROOT/module/agent.lgx" \
    && mv variants/*/* . && rm -rf variants && printf 'darwin-arm64' > variant )
ls "$MODDIR"
[ -f "$MODDIR/liblogosdelivery.dylib" ] \
    || die "the package does not carry liblogosdelivery.dylib — it would install and load nowhere"

say "[2/4] build the harnesses"
mkdir -p "$WORK/bin" "$WORK/sdkobj"
clang++ -std=c++17 -o "$WORK/bin/plugin_delivery_test" \
    module/tests/plugin_delivery_test.cpp "$SDK/cpp/logos_types.cpp" \
    -I"$SDK/cpp" -I"$SDK/core" -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-rpath,"$QT/lib" || die "plugin_delivery_test did not compile"

# Harness 2 needs the SDK translation units the module itself compiles, minus
# token_manager.cpp — see docs/basecamp.md for why that one is the expensive
# mistake — plus logos_types.cpp for the LogosResult metatype on the wire.
( cd "$WORK/sdkobj"
  for h in logos_api logos_api_client logos_api_consumer logos_api_provider \
           module_proxy qt_provider_object; do
      "$QT/libexec/moc" "$SDK/cpp/$h.h" -o "moc_$h.cpp" -I"$SDK/cpp" -I"$SDK/core"
  done
  for f in "$SDK"/cpp/{logos_api,logos_api_client,logos_api_consumer,logos_api_provider,module_proxy,logos_provider_object,qt_provider_object,logos_types}.cpp \
           moc_logos_api.cpp moc_logos_api_client.cpp moc_logos_api_consumer.cpp \
           moc_logos_api_provider.cpp; do
      clang++ -std=c++17 -c -I. -I"$SDK/cpp" -I"$SDK/core" -I/opt/homebrew/include \
          "${QTINC[@]}" "$f" -o "$(basename "${f%.cpp}").o" || exit 1
  done ) || die "the SDK objects did not compile"

clang++ -std=c++17 -o "$WORK/bin/logos_core_delivery_test" \
    module/tests/logos_core_delivery_test.cpp "$WORK"/sdkobj/*.o \
    -I"$SDK/cpp" -I"$SDK/core" -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-undefined,dynamic_lookup -Wl,-rpath,"$APP/Contents/Frameworks" \
    || die "logos_core_delivery_test did not compile"

filter() { grep -vE "^(DBG|INF|WRN|NOT|NTC|TRC|ERR|\[) " ; }

if [ "$MODE" = "peers" ]; then
    # Two agents, two accounts, two working directories. The directories are the
    # part that is easy to get wrong and hard to diagnose: a Delivery node keeps
    # reliable-channel state in the CURRENT WORKING DIRECTORY, so two nodes
    # started from one directory share it silently and the symptom is an
    # unreliable network.
    A="${LP0008_A_ACCOUNT:-5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ}"
    B="${LP0008_B_ACCOUNT:-BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu}"
    AW="${LP0008_A_WALLET:-$HOME/.lp0008-agents/storage}"
    BW="${LP0008_B_WALLET:-$HOME/.lp0008-agents/blockchain}"
    RUN="r$(date +%H%M%S)"
    rm -rf "$WORK/a" "$WORK/b"; mkdir -p "$WORK/a" "$WORK/b"
    say "[3/4] two loaded modules, run id $RUN"
    ( cd "$WORK/a" && "$WORK/bin/plugin_delivery_test" "$MODDIR/agent_plugin.dylib" peer \
        "$RUN" "$A" "python3 $ROOT/scripts/sign-agent-card.py --wallet-home $AW --account $A --sign-input" "$B" \
        > "$WORK/a.log" 2>&1; echo "$?" > "$WORK/a.rc" ) &
    ( cd "$WORK/b" && "$WORK/bin/plugin_delivery_test" "$MODDIR/agent_plugin.dylib" peer \
        "$RUN" "$B" "python3 $ROOT/scripts/sign-agent-card.py --wallet-home $BW --account $B --sign-input" "$A" \
        > "$WORK/b.log" 2>&1; echo "$?" > "$WORK/b.rc" ) &
    wait
    say "agent A"; filter < "$WORK/a.log" | grep -E "^  (ok|FAIL)|^[0-9]\.|failure"
    say "agent B"; filter < "$WORK/b.log" | grep -E "^  (ok|FAIL)|^[0-9]\.|failure"
    rc=$(( $(cat "$WORK/a.rc") + $(cat "$WORK/b.rc") ))
    say "[4/4] result"
    [ "$rc" -eq 0 ] || { echo "FAILED — logs at $WORK/a.log and $WORK/b.log" >&2; exit 1; }
    echo "Two modules loaded through QPluginLoader, each with its own Delivery"
    echo "node, published a signed Agent Card on the public network and"
    echo "discovered the other's. Neither could satisfy its own assertion."
    exit 0
fi

say "[3/4] harness 1 — QPluginLoader"
rm -rf "$WORK/probe"; mkdir -p "$WORK/probe"
( cd "$WORK/probe" && "$WORK/bin/plugin_delivery_test" "$MODDIR/agent_plugin.dylib" probe ) \
    > "$WORK/probe.log" 2>&1; rc1=$?
filter < "$WORK/probe.log" | grep -E "^  (ok|FAIL|<-)|^[0-9]\.|failure"

say "[4/4] harness 2 — the runtime out of the installed Basecamp"
SKIPPED_CORE=0
if [ ! -f "$APP/Contents/Frameworks/liblogos_core.dylib" ]; then
    echo "  skipped: no LogosBasecamp.app at $APP" >&2
    rc2=0
    SKIPPED_CORE=1
else
    rm -rf "$WORK/core" "$WORK/persist"; mkdir -p "$WORK/core" "$WORK/persist"
    ( cd "$WORK/core" && \
      LOGOS_HOST_PATH="$APP/Contents/MacOS/logos_host" \
      QT_PLUGIN_PATH="$APP/Contents/Resources/qt/plugins" \
      "$WORK/bin/logos_core_delivery_test" \
          "$APP/Contents/Frameworks/liblogos_core.dylib" \
          "$APP/Contents/modules" "$WORK/modules" "$WORK/persist" agent ) \
        > "$WORK/core.log" 2>&1; rc2=$?
    filter < "$WORK/core.log" | grep -E "^  (ok|FAIL|<-)|^[0-9]\.|failure"
fi

[ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] || {
    echo "FAILED — logs at $WORK/probe.log and $WORK/core.log" >&2; exit 1; }
echo
# The closing sentence is harness 2's claim, and harness 2 sets rc2=0 when it
# was not run at all — so with no Basecamp installed this script printed "A
# module Logos Core loaded opened its own Logos Delivery node" having loaded
# nothing through Logos Core. What was not run is not described.
if [ "$SKIPPED_CORE" = "1" ]; then
    echo "A module QPluginLoader loaded opened its own Logos Delivery node and"
    echo "served the skills that used to refuse."
    echo
    echo "HARNESS 2 DID NOT RUN: there is no LogosBasecamp.app at $APP, so nothing"
    echo "here was loaded by the real Logos Core runtime. Install Basecamp (see"
    echo "docs/basecamp.md) and re-run to make that claim. Logs: $WORK"
else
    echo "A module Logos Core loaded opened its own Logos Delivery node and served"
    echo "the skills that used to refuse. Logs: $WORK"
fi
