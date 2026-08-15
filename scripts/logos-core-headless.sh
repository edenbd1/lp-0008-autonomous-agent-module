#!/usr/bin/env bash
# Install this repository's agent module into Logos Core and run it headless —
# load, configure, start — in one command.
#
#   ./scripts/logos-core-headless.sh [category]
#
# WHAT THIS IS FOR
#
# The prize asks that "the owner can deploy the agent and configure it with a
# single CLI command on any machine using Logos Core headless".
# `scripts/deploy-agents.sh` is the other half of that sentence: it deploys the
# agent's identity and its spending envelope ON CHAIN. Nothing in it touches
# Logos Core — `grep -rn 'logos_core' scripts/` returned nothing at all until
# this file existed, which meant the repository's answer to a criterion naming
# Logos Core headless had no Logos Core in it.
#
# This is that half. It:
#
#   1. installs `module/agent.lgx` into the user modules directory Logos Core
#      reads, flattening the variant for this platform the way Basecamp's own
#      installer does (dropping the archive in does nothing);
#   2. builds the headless harness if it is not already built, from
#      `module/tests/logos_core_load_test.cpp`;
#   3. runs it: `logos_core_init` → `logos_core_add_modules_dir` (embedded and
#      user) → `set_persistence_base_path` → `set_access_policy` →
#      `logos_core_start` → `logos_core_load_module`, then `configure()` and
#      `start()` on the loaded module across the runtime's own transport. That
#      is the same C API, in the same order, as the Logos app's `main.cpp`.
#      No GUI, no window, no display.
#
# and it configures the module with the owner and the policy account that
# `artifacts/agents.tsv` records for that agent — the ones actually anchored on
# chain — rather than with a placeholder. The harness reads both back out of
# `meta.status` afterwards, so what is asserted is that the runtime is running
# THIS agent under THAT envelope, not merely that a module loaded.
#
# WHAT IT DOES NOT DO, AND WILL NOT PRETEND TO
#
# It is one command on a machine that has Logos Basecamp installed, Qt 6.9.2,
# and a logos-cpp-sdk checkout. It cannot be one command on a bare machine:
# `liblogos_core` ships inside the app and there is no headless distribution of
# it to fetch. Every one of those is checked below and named in the error when
# it is missing, so a machine that cannot run this says which piece it lacks
# instead of failing somewhere inside a compile. docs/limitations.md carries the
# same list as prose.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

CATEGORY="${1:-${AGENT_CATEGORY:-storage}}"
MANIFEST="${MANIFEST:-artifacts/agents.tsv}"
LGX="${LGX:-module/agent.lgx}"
MODULE_NAME="${MODULE_NAME:-agent}"
BUILD="${HEADLESS_BUILD_DIR:-build-headless}"

die() { echo "$*" >&2; exit 1; }

# ── where everything lives ────────────────────────────────────────────────
case "$(uname -s)" in
  Darwin)
    APP="${LOGOS_APP:-/Applications/LogosBasecamp.app}"
    CORE_LIB="${LOGOS_CORE_LIB:-$APP/Contents/Frameworks/liblogos_core.dylib}"
    HOST_BIN="${LOGOS_HOST_PATH:-$APP/Contents/MacOS/logos_host}"
    EMBEDDED_DIR="${LOGOS_EMBEDDED_MODULES:-$APP/Contents/modules}"
    QT_PLUGINS="${QT_PLUGIN_PATH:-$APP/Contents/Resources/qt/plugins}"
    USER_MODULES="${LOGOS_MODULES_DIR:-$HOME/Library/Application Support/Logos/LogosBasecamp/modules}"
    QT_ROOT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/macos}"
    QT_INC=(-F"$QT_ROOT/lib"
            -I"$QT_ROOT/lib/QtCore.framework/Headers"
            -I"$QT_ROOT/lib/QtRemoteObjects.framework/Headers"
            -I"$QT_ROOT/lib/QtNetwork.framework/Headers")
    QT_LINK=(-framework QtCore -framework QtRemoteObjects -framework QtNetwork
             -Wl,-rpath,"$QT_ROOT/lib")
    CORE_RPATH=(-Wl,-undefined,dynamic_lookup -Wl,-rpath,"$APP/Contents/Frameworks")
    EXTRA_INC="${EXTRA_INCLUDE_DIR:-/opt/homebrew/include}"
    DEFAULT_VARIANT="darwin-arm64"
    ;;
  Linux)
    # Untested: this repository has never had a Logos Basecamp install on Linux
    # to run it against. The paths are the documented ones, and the checks below
    # will say which is missing rather than guessing.
    APP="${LOGOS_APP:-}"
    CORE_LIB="${LOGOS_CORE_LIB:-}"
    HOST_BIN="${LOGOS_HOST_PATH:-}"
    EMBEDDED_DIR="${LOGOS_EMBEDDED_MODULES:-}"
    QT_PLUGINS="${QT_PLUGIN_PATH:-}"
    USER_MODULES="${LOGOS_MODULES_DIR:-$HOME/.local/share/Logos/LogosBasecamp/modules}"
    QT_ROOT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/gcc_64}"
    QT_INC=(-I"$QT_ROOT/include" -I"$QT_ROOT/include/QtCore"
            -I"$QT_ROOT/include/QtRemoteObjects" -I"$QT_ROOT/include/QtNetwork")
    QT_LINK=(-L"$QT_ROOT/lib" -lQt6Core -lQt6RemoteObjects -lQt6Network
             -Wl,-rpath,"$QT_ROOT/lib")
    CORE_RPATH=(-Wl,--allow-shlib-undefined)
    EXTRA_INC="${EXTRA_INCLUDE_DIR:-/usr/include}"
    DEFAULT_VARIANT="linux-amd64"
    ;;
  *) die "unsupported platform: $(uname -s)" ;;
esac
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
PERSISTENCE="${LOGOS_PERSISTENCE_DIR:-$BUILD/persistence}"

# ── the prerequisites, each named individually ────────────────────────────
missing=0
need() { # path description how-to-get-it
  if [ ! -e "$1" ]; then
    echo "missing: $2" >&2
    echo "         expected at $1" >&2
    echo "         $3" >&2
    missing=$((missing + 1))
  fi
}
need "$CORE_LIB"  "liblogos_core (the Logos Core runtime)" \
     "install Logos Basecamp, or set LOGOS_CORE_LIB. It ships inside the app; there is no standalone headless build to download."
need "$HOST_BIN"  "logos_host (runs core modules in their own process)" \
     "set LOGOS_HOST_PATH. Without it the runtime logs 'logos_host not found' and the load fails."
need "$EMBEDDED_DIR" "the app's embedded modules directory" \
     "set LOGOS_EMBEDDED_MODULES."
need "$QT_ROOT"   "Qt 6.9.2" \
     "aqtinstall: python3 -m aqt install-qt <host> desktop 6.9.2 <arch> -m qtremoteobjects — see docs/basecamp.md. Set QT_ROOT."
need "$SDK/cpp/logos_api.cpp" "logos-cpp-sdk checkout" \
     "clone https://github.com/logos-co/logos-cpp-sdk and set LOGOS_CPP_SDK_ROOT."
need "$LGX"       "the packaged module" \
     "module/agent.lgx is committed; run module/package-basecamp.sh to rebuild it."
[ "$missing" -eq 0 ] || die "
$missing prerequisite(s) missing. This command is one command on a prepared
machine; docs/limitations.md lists what 'prepared' means and why none of it can
be fetched automatically."

# ── the agent this run configures ─────────────────────────────────────────
#
# Read BY HEADER NAME. This manifest has gained columns twice — `claim_account`,
# `record_prefix`, `claim_tx` — and a positional read returns a plausible wrong
# value rather than failing.
col() { # file header row-key -> value
  awk -F'\t' -v w="$2" -v k="$3" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==w) c=i; next }
    $1==k { print $c; exit }' "$1"
}
[ -f "$MANIFEST" ] || die "no $MANIFEST: run ./scripts/deploy-agents.sh first, or set MANIFEST"
OWNER=$(col "$MANIFEST" owner "$CATEGORY")
POLICY=$(col "$MANIFEST" policy_account "$CATEGORY")
AGENT_ID=$(col "$MANIFEST" agent_id "$CATEGORY")
[ -n "$OWNER" ] && [ -n "$POLICY" ] \
  || die "no row for '$CATEGORY' in $MANIFEST (have: $(awk -F'\t' 'NR>1{printf "%s ", $1}' "$MANIFEST"))"

# The policy account as the 32 bytes `configure()` wants, hex. It validates the
# length and refuses anything else, so a mistake here is an error at startup
# rather than a spend that fails on chain much later.
POLICY_HEX=$(python3 -c "
import sys
A='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
s=sys.argv[1]
n=0
for c in s: n = n*58 + A.index(c)
b=n.to_bytes((n.bit_length()+7)//8,'big')
b=b'\x00'*(len(s)-len(s.lstrip('1')))+b
assert len(b)==32, 'not a 32-byte account id: %r' % s
print(b.hex())" "$POLICY") || die "could not decode the policy account $POLICY"

echo "agent     $CATEGORY  $AGENT_ID"
echo "owner     $OWNER"
echo "policy    $POLICY"
echo "          = $POLICY_HEX"
echo

# ── 1. install ────────────────────────────────────────────────────────────
#
# An installed module is the variant FLATTENED, plus a `variant` file naming it.
# Basecamp 0.2.2 has no "install from file" button — its Package Manager reads a
# configured repository only — so this is the install path, not a shortcut past
# one.
VARIANT="${LGX_VARIANT:-}"
if [ -z "$VARIANT" ]; then
  VARIANT=$(tar tzf "$LGX" | sed -n 's|^variants/\([^/]*\)/$|\1|p' | head -n1)
  [ -n "$VARIANT" ] || VARIANT="$DEFAULT_VARIANT"
fi
DEST="$USER_MODULES/$MODULE_NAME"
echo "install   $LGX  variant $VARIANT"
echo "          -> $DEST"
rm -rf "$DEST"
mkdir -p "$DEST" || die "could not create $DEST"
tar xzf "$ROOT/$LGX" -C "$DEST" || die "could not unpack $LGX"
[ -d "$DEST/variants/$VARIANT" ] \
  || die "$LGX has no variant '$VARIANT' (has: $(tar tzf "$ROOT/$LGX" | sed -n 's|^variants/\([^/]*\)/$|\1|p' | tr '\n' ' '))"
mv "$DEST/variants/$VARIANT"/* "$DEST/" && rm -rf "$DEST/variants"
printf '%s' "$VARIANT" > "$DEST/variant"
# The manifest's `main` must name a file that is really there: a `main` naming a
# file the package does not contain is an invisible load failure — the host
# resolves it, finds nothing, and says nothing. The harness asserts this too;
# failing here says which half is wrong.
MAIN=$(python3 -c "
import json,sys
m=json.load(open('$DEST/manifest.json'))['main']
print(m['$VARIANT'] if isinstance(m,dict) else m)" 2>/dev/null)
[ -n "$MAIN" ] || die "manifest.json names no main for $VARIANT"
[ -e "$DEST/$MAIN" ] || die "manifest.json names main=$MAIN, which is not in the package"
echo "          installed, main=$MAIN"
echo

# ── 2. build the harness, if it is not already built ──────────────────────
#
# A plain compile, not a CMake target, so it cannot silently stop being built.
# `token_manager.cpp` is deliberately NOT among the SDK translation units: it
# holds a singleton, and compiling a second copy into this executable makes it
# win the symbol, start empty, and reject every call to the module with "auth
# token not recognized" — which reads like a permissions problem and is not.
HARNESS="$BUILD/logos_core_load_test"
SRC=module/tests/logos_core_load_test.cpp
mkdir -p "$BUILD/sdkobj"
if [ ! -x "$HARNESS" ] || [ "$SRC" -nt "$HARNESS" ]; then
  echo "build     $HARNESS"
  MOC="$QT_ROOT/libexec/moc"
  [ -x "$MOC" ] || die "no moc at $MOC (an incomplete Qt install: see docs/basecamp.md)"
  (
    cd "$BUILD/sdkobj" || exit 1
    for h in logos_api logos_api_client logos_api_consumer logos_api_provider \
             module_proxy qt_provider_object; do
      "$MOC" "$SDK/cpp/$h.h" -o "moc_$h.cpp" -I"$SDK/cpp" -I"$SDK/core" || exit 1
    done
    # module_proxy.cpp and qt_provider_object.cpp #include their own moc output,
    # so `-I.` finds it and their moc_*.cpp is not a translation unit of its own
    # — compiling it as one is a duplicate-symbol link error.
    for f in "$SDK/cpp/logos_api.cpp" "$SDK/cpp/logos_api_client.cpp" \
             "$SDK/cpp/logos_api_consumer.cpp" "$SDK/cpp/logos_api_provider.cpp" \
             "$SDK/cpp/module_proxy.cpp" "$SDK/cpp/logos_provider_object.cpp" \
             "$SDK/cpp/qt_provider_object.cpp" "$SDK/cpp/logos_types.cpp" \
             moc_logos_api.cpp moc_logos_api_client.cpp \
             moc_logos_api_consumer.cpp moc_logos_api_provider.cpp; do
      o="$(basename "${f%.cpp}").o"
      [ -f "$o" ] && [ "$o" -nt "$f" ] && continue
      c++ -std=c++17 -c -I. -I"$SDK/cpp" -I"$SDK/core" -I"$EXTRA_INC" \
          "${QT_INC[@]}" "$f" -o "$o" || exit 1
    done
  ) || die "the SDK translation units did not build"

  c++ -std=c++17 -o "$HARNESS" "$SRC" "$BUILD"/sdkobj/*.o \
      -I"$SDK/cpp" -I"$SDK/core" -I"$EXTRA_INC" "${QT_INC[@]}" \
      "${QT_LINK[@]}" "${CORE_RPATH[@]}" \
    || die "the harness did not build"
  echo "          built"
else
  echo "build     $HARNESS  (up to date)"
fi
echo

# ── 3. run Logos Core headless ────────────────────────────────────────────
mkdir -p "$PERSISTENCE"
echo "run       headless: init → add_modules_dir → start → load_module($MODULE_NAME) → configure → start"
echo
LOGOS_HOST_PATH="$HOST_BIN" QT_PLUGIN_PATH="$QT_PLUGINS" QT_QPA_PLATFORM=offscreen \
  "$HARNESS" "$CORE_LIB" "$EMBEDDED_DIR" "$USER_MODULES" "$PERSISTENCE" \
             "$MODULE_NAME" "$OWNER" "$POLICY_HEX"
rc=$?
echo
if [ $rc -eq 0 ]; then
  echo "Logos Core ran the module headless and it reports itself configured and"
  echo "started, bound to owner $OWNER"
  echo "and to policy account $POLICY."
else
  echo "the headless run failed (exit $rc)" >&2
fi
exit $rc
