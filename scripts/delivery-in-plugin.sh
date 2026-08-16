#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Can a module that Logos Core LOADS obtain a working Delivery port?
#
#   ./scripts/delivery-in-plugin.sh            # both single-process harnesses
#   ./scripts/delivery-in-plugin.sh peers      # two loaded modules, two nodes
#   ./scripts/delivery-in-plugin.sh approval   # an owner approves, then denies
#   ./scripts/delivery-in-plugin.sh signers    # the envelope and the signer, no chain
#   ./scripts/delivery-in-plugin.sh settle     # discover, serve and PAY, one flow
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
#     OTHER account, and each driving an A2A task to `completed` on status
#     updates the other one published. (1) and (2) cannot be evidence for the
#     discovery criterion: a Waku node receives its own published messages, so a
#     single process can satisfy any assertion about "a card arrived" — or about
#     "the peer said the task is done" — with every other agent on earth
#     switched off.
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

# THE WORK DIRECTORY IS PER-CHECKOUT, AND THAT IS NOT TIDINESS
#
# This was a fixed path — `$TMPDIR/lp0008-delivery-in-plugin` — shared by every
# checkout on the machine. Step [1/4] unpacks `module/agent.lgx` into it, and a
# concurrent run from a DIFFERENT checkout unpacks a different package to the
# same place. An audit hit exactly that: `plugin_load_test`, pointed at
# `$WORK/modules/agent/agent_plugin.dylib` minutes after this script had put the
# committed package there, ran against another tree's in-flight build and
# reported 23 skills where the package under test had 22. It read as the module
# drifting from its own test. Nothing warned, because nothing could: both runs
# did exactly what they were told.
#
# So the path now carries the checkout's identity. Two rules that came out of
# the same incident and are worth keeping:
#
#   1. anything that runs a harness against `$WORK/modules/agent` should compare
#      the binary's sha256 against the one `scripts/check-package-fresh.py`
#      names, before believing the result;
#   2. `$LP0008_WORK` overrides this outright, which is what a parallel run in
#      one checkout wants.
WORK="${LP0008_WORK:-${TMPDIR:-/tmp}/lp0008-delivery-in-plugin-$(
    printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)}"

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

if [ "$MODE" = "signers" ]; then
    # What the module does with what its two delegates SAY. Costs nothing, needs
    # no second agent and no key, and it is the mode to run when either delegate
    # changes — `settle` below moves real money on a testnet with no faucet, so
    # it can be run once and it only proves the happy path.
    A="${LP0008_AGENT:-5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ}"
    rm -rf "$WORK/signers"; mkdir -p "$WORK/signers"
    say "[3/4] the envelope and the signer, against stubs"
    ( cd "$WORK/signers" && "$WORK/bin/plugin_delivery_test" \
        "$MODDIR/agent_plugin.dylib" signers "$A" ) > "$WORK/signers.log" 2>&1; rc=$?
    filter < "$WORK/signers.log" | grep -E "^  (ok|FAIL|<-)|^[0-9]\.|failure"
    say "[4/4] result"
    [ "$rc" -eq 0 ] || { echo "FAILED — log at $WORK/signers.log" >&2; exit 1; }
    echo "Unknown limits are outside the envelope, a price over the anchored"
    echo "limits never reaches the signer, and neither an empty answer nor a"
    echo "diagnostic from the signer becomes a settlement."
    exit 0
fi

if [ "$MODE" = "approval" ]; then
    # The composition that was wired and unwatched: a loaded module holds an
    # above-threshold spend, publishes it over Logos Messaging to an owner on
    # another node, and acts on the answer. Run twice — approve, then deny —
    # because a channel that reported "approved" whatever came back would pass
    # the first run and only the first.
    A="${LP0008_AGENT:-5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ}"
    O="${LP0008_OWNER:-BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu}"
    PAYEE="${LP0008_PAYEE:-Dxh7ZLHFmhKdNVE69XWayqLrquMk9iLfFVpmiJdfpEwD}"
    # The responder is the one thing here that links the Delivery library
    # directly — it is the owner's app, not the module — so it needs the
    # checkout `scripts/exercise-nodes.sh` builds.
    D="${DELIVERY_SRC:-$ROOT/_external/logos-delivery}"
    [ -f "$D/library/liblogosdelivery.h" ] || die \
        "no logos-delivery checkout at $D (set DELIVERY_SRC). See scripts/exercise-nodes.sh"
    clang++ -std=c++17 -Wall -Wextra -o "$WORK/bin/owner_responder" \
        module/tests/owner_responder.cpp module/src/owner_channel.cpp \
        module/src/messaging_skills.cpp module/src/spend_marker.cpp \
        -I"$D/library" -I/opt/homebrew/include -L"$D/build" -llogosdelivery \
        -Wl,-rpath,"$D/build" || die "the owner responder did not compile"

    rc=0
    for run in approve deny; do
        say "[3/4] the owner will $run"
        denyflag=""; expect=""
        [ "$run" = "deny" ] && { denyflag="--deny"; expect="LP0008_EXPECT_DENY=1"; }
        # Each process in a directory of its own: a Delivery node keeps its
        # reliable-channel state in the CWD, and two nodes sharing one is an
        # unreliable network that is not the network's fault.
        rm -rf "$WORK/$run-owner" "$WORK/$run-agent"
        mkdir -p "$WORK/$run-owner" "$WORK/$run-agent"
        ( cd "$WORK/$run-owner" && "$WORK/bin/owner_responder" --owner "$O" --agent "$A" \
            --seconds 200 $denyflag > "$WORK/$run-owner.log" 2>&1 ) &
        sleep 2
        ( cd "$WORK/$run-agent" && env $expect "$WORK/bin/plugin_delivery_test" \
            "$MODDIR/agent_plugin.dylib" approval "$A" "$O" "$PAYEE" 250 \
            > "$WORK/$run-agent.log" 2>&1; echo $? > "$WORK/$run.rc" ) &
        wait
        echo "  --- owner ---"; filter < "$WORK/$run-owner.log" | grep -E "^  (ok|FAIL)|failure\)"
        echo "  --- agent ---"; filter < "$WORK/$run-agent.log" | grep -E "^  (ok|FAIL|<-)|failure\)"
        rc=$(( rc + $(cat "$WORK/$run.rc") ))
    done
    say "[4/4] result"
    [ "$rc" -eq 0 ] || { echo "FAILED — logs in $WORK" >&2; exit 1; }
    echo "A loaded module published an above-threshold spend to an owner on another"
    echo "node, over the public network, and acted on the answer — approved once and"
    echo "denied once, and nothing was submitted either time."
    exit 0
fi

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
    echo "node, published a signed Agent Card on the public network, discovered"
    echo "the other's, opened an A2A task with it, served the one it was asked"
    echo "for, and carried its OWN task to 'completed' on status updates the"
    echo "other account published. Neither could satisfy its own assertion: each"
    echo "put a forged 'completed' for its own task on the topic it reads, and"
    echo "counted it refused."
    exit 0
fi

if [ "$MODE" = "settle" ]; then
    # DISCOVER, SERVE AND PAY, IN ONE FLOW.
    #
    # `peers` above proves the first two: two loaded modules find each other's
    # signed cards on the public network and each reads the other's A2A request
    # off its own task topic. `scripts/a2a-task.sh` proves the third: a real
    # settlement on the public testnet, signed by an agent's own account with no
    # owner key anywhere in the path. What neither proves is that they are one
    # thing — and a criterion that reads "discover each other via Agent Cards,
    # execute a task following the A2A lifecycle, and transfer LEZ payment
    # autonomously" is one sentence with one subject.
    #
    # This mode is that sentence. The buyer is handed no price, no payee and no
    # instruction to pay: it reads all three off the seller's card, which arrived
    # over Waku, checks the price against the envelope its owner anchored on
    # chain, and settles through the policy program from inside the loaded
    # module. Both processes run the same binary and the same code path.
    #
    # WHAT THIS COSTS, WHICH IS WHY THE DEFAULT PRICE IS 1
    #
    # A settlement is real money on a testnet whose faucet is gone. The paying
    # agent held 10 LEZ when this was written and an account at zero cannot sign
    # at all, so the demonstration is priced at the smallest amount that is not
    # zero, and `agent-spend.py --max-amount` refuses more than it is told even
    # if a card asks for it.
    #
    # The buyer's identity is its SHIELDED account and its card is signed by the
    # PUBLIC one it is paid into — the split `artifacts/agents.tsv` records, and
    # the reason for it is that `spel` resolves `Private/<id>` only for accounts
    # the signing wallet holds keys for, while a card has to be checkable by a
    # stranger who holds nothing.
    #
    # WHICH TWO OF THE THREE AGENTS TRANSACT IS A FACT ABOUT WHO HOLDS LEZ.
    #
    # `scripts/a2a-task.sh` says the same thing and for the same reason: an
    # envelope is a ceiling, not a balance, and a payer that cannot afford the
    # task produces the least useful demonstration available — a policy check
    # that passes and a transfer that fails. All three agents are deployed and
    # anchored; the storage agent is the buyer here because it is the one with a
    # balance, and the blockchain agent is the seller because its receiving
    # account holds nothing, so the movement this produces is 0 -> price and
    # cannot be confused with anything already on that account.
    CLIENT="${LP0008_CLIENT:-9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE}"
    CLIENT_PAY="${LP0008_CLIENT_PAY:-5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ}"
    CLIENT_POLICY="${LP0008_CLIENT_POLICY:-6FscNXjNhamSCTbzLe67gU3noFHkQKDjRmD4tNj3ipSe}"
    CLIENT_WALLET="${LP0008_CLIENT_WALLET:-$HOME/.lp0008-agents/storage}"
    SERVER="${LP0008_SERVER:-A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu}"
    SERVER_PAY="${LP0008_SERVER_PAY:-BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu}"
    SERVER_WALLET="${LP0008_SERVER_WALLET:-$HOME/.lp0008-agents/blockchain}"
    PRICE="${LP0008_PRICE:-1}"
    # The v0.2.2+ wallet, not whatever is first on PATH. A wallet that cannot
    # read this wallet home cannot resync it, and `agent-spend.py` refuses to
    # prove against a state it could not resync — which is the honest failure,
    # but it is better to name the binary here than to discover it after a node
    # has spent four minutes joining the network.
    WALLET_BIN="${WALLET_BIN:-$HOME/logos/src/lez-v0.2.2/target/release/wallet}"
    [ -x "$WALLET_BIN" ] || die "no LEZ wallet at $WALLET_BIN (set WALLET_BIN)"
    command -v "${SPEL_BIN:-spel}" >/dev/null || die "no spel on PATH (set SPEL_BIN)"
    [ -d "$CLIENT_WALLET" ] || die "no payer wallet home at $CLIENT_WALLET"

    SPEND="python3 $ROOT/scripts/agent-spend.py --policy $CLIENT_POLICY"
    say "[3/4] the envelope the buyer's owner anchored, read off the chain"
    $SPEND --envelope || die "the anchored policy could not be read"

    # The payee's balance BEFORE, and it is the only thing here read with
    # `getAccount`. It has to be: this chain has no historical-state RPC, so a
    # balance as it stood before a settlement can only be captured before the
    # settlement happens. Everything after it comes out of the transaction.
    BEFORE=$(curl -s -m 25 -X POST "${SEQUENCER_URL:-https://testnet.lez.logos.co}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccount\",\"params\":[\"$SERVER_PAY\"]}" \
      | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['balance'])")
    [ -n "$BEFORE" ] || die "could not read the payee's balance before the run"
    echo "  payee  Public/$SERVER_PAY holds $BEFORE now"

    RUN="s$(date +%H%M%S)"
    rm -rf "$WORK/c" "$WORK/s"; mkdir -p "$WORK/c" "$WORK/s"
    say "[3/4] two loaded modules, run id $RUN, price $PRICE LEZ"
    echo "  buyer  $CLIENT  (paid from, shielded; card signed by $CLIENT_PAY)"
    echo "  seller $SERVER  (paid into Public/$SERVER_PAY)"
    ( cd "$WORK/c" && \
      LP0008_PAY_ACCOUNT="$CLIENT_PAY" \
      LP0008_POLICY_SOURCE="$SPEND --envelope" \
      LP0008_PAY_SIGNER="env WALLET_BIN=$WALLET_BIN $SPEND --wallet-home $CLIENT_WALLET --agent $CLIENT --max-amount $PRICE --settle" \
      "$WORK/bin/plugin_delivery_test" "$MODDIR/agent_plugin.dylib" peer \
        "$RUN" "$CLIENT" "python3 $ROOT/scripts/sign-agent-card.py --wallet-home $CLIENT_WALLET --account $CLIENT_PAY --sign-input" "$SERVER" \
        > "$WORK/c.log" 2>&1; echo "$?" > "$WORK/c.rc" ) &
    ( cd "$WORK/s" && \
      LP0008_PAY_ACCOUNT="$SERVER_PAY" \
      LP0008_PRICE="$PRICE" \
      "$WORK/bin/plugin_delivery_test" "$MODDIR/agent_plugin.dylib" peer \
        "$RUN" "$SERVER" "python3 $ROOT/scripts/sign-agent-card.py --wallet-home $SERVER_WALLET --account $SERVER_PAY --sign-input" "$CLIENT" \
        > "$WORK/s.log" 2>&1; echo "$?" > "$WORK/s.rc" ) &
    wait
    say "buyer";  filter < "$WORK/c.log" | grep -E "^  (ok|FAIL|<-)|^[0-9]\.|failure"
    say "seller"; filter < "$WORK/s.log" | grep -E "^  (ok|FAIL)|^[0-9]\.|failure"
    rc=$(( $(cat "$WORK/c.rc") + $(cat "$WORK/s.rc") ))
    TX=$(grep -oE 'settled it on chain.*: [0-9a-f]{64}' "$WORK/c.log" | tail -1 \
         | grep -oE '[0-9a-f]{64}')
    say "[4/4] what the chain says about it"
    [ "$rc" -eq 0 ] || { echo "FAILED — logs at $WORK/c.log and $WORK/s.log" >&2; exit 1; }
    [ -n "$TX" ] || die "the run passed but no settlement hash was printed"

    # THE LAST WORD BELONGS TO THE TRANSACTION, NOT TO THE HARNESS.
    #
    # The buyer asserted that its signer returned a hash, and the signer asserted
    # that a block holds it. Neither of those is "the money moved" — a confirmed,
    # on-chain proof that a policy PERMITTED a payment and moved nothing is a
    # thing this repository has produced before and wrote up as a settlement.
    # `record-settlement.py` decodes the payee's balance out of the settlement's
    # own committed post-state and refuses to record anything unless it rose by
    # exactly the price.
    #
    # A command rather than fifteen lines here, and the reason is this branch
    # itself: it takes half an hour and costs a settlement, so a bug in its last
    # fifteen lines would be found once, in the one run that cannot be repeated.
    AFTER=$(python3 "$ROOT/scripts/record-settlement.py" --tx "$TX" \
        --client "$CLIENT" --server "$SERVER" --recipient "$SERVER_PAY" \
        --policy "$CLIENT_POLICY" --price "$PRICE" --before "$BEFORE" \
        --task-id "t$RUN") || die "the settlement was not recorded, and this run claims nothing"

    echo
    echo "Two loaded modules discovered each other's signed Agent Cards on the"
    echo "public network, ran the A2A lifecycle across their own Delivery nodes,"
    echo "and the buyer paid the seller's advertised price on the public testnet"
    echo "from inside the plugin, with no owner in the path."
    echo
    echo "settlement $TX"
    echo "Public/$SERVER_PAY  $BEFORE -> $AFTER, decoded from the transaction itself"
    echo "manifest  ${A2A_MANIFEST:-artifacts/a2a-task.tsv}"
    echo "explorer  https://explorer.testnet.lez.logos.co/transaction/$TX"
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
