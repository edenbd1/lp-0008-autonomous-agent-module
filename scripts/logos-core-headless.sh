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
# WHERE IT RUNS
#
# macOS arm64 and x86-64 Linux, out of the same `module/agent.lgx` — it installs
# the variant for the machine it is on, and refuses rather than installing
# another platform's binary into a directory that would then look complete.
#
# WHAT IT DOES NOT DO, AND WILL NOT PRETEND TO
#
# It is one command on a PREPARED machine: a Logos Core runtime, Qt 6.9.2 and a
# logos-cpp-sdk checkout. Every one of those is checked below and named in the
# error when it is missing, so a machine that cannot run this says which piece
# it lacks instead of failing somewhere inside a compile. docs/limitations.md
# carries the same list as prose.
#
# This comment used to say the runtime half "cannot be one command on a bare
# machine: liblogos_core ships inside the app and there is no headless
# distribution of it to fetch". The first clause is true on every platform. The
# second was true of the macOS .dmg and was never checked against the Linux
# build, which is an AppImage — a SquashFS image behind an ELF runtime, on the
# same release page, that unpacks with no installer, no root, no FUSE and no
# display. `scripts/fetch-logos-core.sh` does that in one checksum-pinned
# command and leaves the tree where the Linux defaults below already look.
#
# --alongside
#
# The prize also asks that "the agent module loads and runs inside Logos Core
# alongside the wallet, storage, and messaging modules without requiring
# modifications to those modules". With `--alongside`, this command installs the
# packages `scripts/build-companion-modules.sh` built out of untouched
# `logos-co/logos-{storage,delivery,wallet}-module` checkouts into the same
# modules directory, and the harness loads all four into one runtime — asserting
# for each that it is not merely in the loaded set but ANSWERING across the
# runtime's transport, which is the only way to tell a running module from one
# whose host process died on a Qt version mismatch.
#
# It also re-checks, here rather than only at build time, that not one tracked
# file in those three checkouts differs from the published revision. That is the
# second half of the criterion and it is checked mechanically, the way
# `examples/agent-console/run.sh` checks `git status --porcelain module/`.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

ALONGSIDE=0
args=()
for a in "$@"; do
  case "$a" in
    --alongside) ALONGSIDE=1 ;;
    *) args+=("$a") ;;
  esac
done
set -- ${args[@]+"${args[@]}"}

CATEGORY="${1:-${AGENT_CATEGORY:-storage}}"
MANIFEST="${MANIFEST:-artifacts/agents.tsv}"
LGX="${LGX:-module/agent.lgx}"
MODULE_NAME="${MODULE_NAME:-agent}"
BUILD="${HEADLESS_BUILD_DIR:-build-headless}"
COMPANIONS_DIR="${COMPANIONS_DIR:-$ROOT/build-companions}"
# The modules the criterion names, by the name each one calls itself in its own
# metadata.json. Spelled out rather than globbed: a package that failed to build
# would silently drop out of a glob, and the run would then prove that the agent
# loads beside whatever happened to be lying around.
COMPANIONS=(storage_module delivery_module wallet_module)

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
    # Nothing to prepend: `-rpath QT_ROOT/lib` is linked into the harness ahead of
    # the app's Frameworks directory, so one Qt serves both.
    RUN_LD_PATH=""
    ;;
  Linux)
    # The Linux Logos app is an AppImage, and an unpacked AppImage and an
    # installed app are the same tree under a different root — `usr/lib`,
    # `usr/bin`, `usr/modules`. `scripts/fetch-logos-core.sh` unpacks one with a
    # checksum-pinned download and no installer, no root and no FUSE, and leaves
    # it exactly here, so the default needs no environment at all.
    APP="${LOGOS_APP:-$ROOT/_external/logos-core/squashfs-root}"
    CORE_LIB="${LOGOS_CORE_LIB:-$APP/usr/lib/liblogos_core.so}"
    HOST_BIN="${LOGOS_HOST_PATH:-$APP/usr/bin/logos_host}"
    EMBEDDED_DIR="${LOGOS_EMBEDDED_MODULES:-$APP/usr/modules}"
    QT_PLUGINS="${QT_PLUGIN_PATH:-$APP/usr/lib/qt/plugins}"
    USER_MODULES="${LOGOS_MODULES_DIR:-$HOME/.local/share/Logos/LogosBasecamp/modules}"
    # Basecamp publishes an AppImage for both Linux architectures, and both are
    # pinned in scripts/fetch-logos-core.sh, so neither the variant nor the Qt
    # directory can be a constant. aqt's own naming: `gcc_64` for x86-64,
    # `gcc_arm64` for aarch64.
    case "$(uname -m)" in
      x86_64|amd64)  DEFAULT_VARIANT="linux-amd64"; QT_ARCH_DIR=gcc_64 ;;
      aarch64|arm64) DEFAULT_VARIANT="linux-arm64"; QT_ARCH_DIR=gcc_arm64 ;;
      *) die "no Logos Basecamp AppImage is published for $(uname -m)" ;;
    esac
    QT_ROOT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/$QT_ARCH_DIR}"
    QT_INC=(-I"$QT_ROOT/include" -I"$QT_ROOT/include/QtCore"
            -I"$QT_ROOT/include/QtRemoteObjects" -I"$QT_ROOT/include/QtNetwork")
    QT_LINK=(-L"$QT_ROOT/lib" -lQt6Core -lQt6RemoteObjects -lQt6Network
             -Wl,-rpath,"$QT_ROOT/lib")
    # The harness LINKS the runtime here, where macOS only defers to it.
    # `-undefined dynamic_lookup` is a Mach-O feature and has no ELF equivalent
    # for an executable: `--allow-shlib-undefined` governs undefined symbols in
    # shared libraries, not in the program, so the link ends in
    # `undefined reference to TokenManager::instance()` and six more. Every one
    # of them is exported by `liblogos_core.so`, so the honest fix is to say so.
    # This does not change what is being tested: the runtime is still opened by
    # path at run time, and `token_manager.cpp` is still not compiled in — a
    # second copy of that singleton is the failure the note below describes.
    CORE_RPATH=(-Wl,--allow-shlib-undefined
                -L"$(dirname "$CORE_LIB")" -llogos_core
                -Wl,-rpath,"$(dirname "$CORE_LIB")")
    EXTRA_INC="${EXTRA_INCLUDE_DIR:-/usr/include}"
    # Qt from QT_ROOT first, then the app's own directory for everything else it
    # bundles (boost, spdlog, its own OpenSSL). A dependency is resolved from
    # LD_LIBRARY_PATH before the object's own DT_RUNPATH, and `liblogos_core.so`
    # has `RUNPATH: $ORIGIN` — so without this line the process would load Qt
    # twice under one soname each and the first one to be needed would decide.
    # This is the same ordering macOS gets for free: there, `-rpath QT_ROOT/lib`
    # is linked into the harness ahead of the app's Frameworks directory.
    RUN_LD_PATH="$QT_ROOT/lib:$APP/usr/lib"
    ;;
  *) die "unsupported platform: $(uname -s)" ;;
esac
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
PERSISTENCE="${LOGOS_PERSISTENCE_DIR:-$BUILD/persistence}"

# `--alongside` installs four modules rather than one, and it does that into a
# directory of its own unless told otherwise. Not because the real one would not
# work — it is the same code path, and `logos_core_add_modules_dir` is given
# whichever it is — but because a run that drops three third-party modules into
# somebody's Logos Basecamp install is not something a verification command
# should do behind their back. Point LOGOS_MODULES_DIR at the real directory to
# have them turn up in the app.
if [ "$ALONGSIDE" -eq 1 ] && [ -z "${LOGOS_MODULES_DIR:-}" ]; then
  USER_MODULES="$COMPANIONS_DIR/modules"
fi

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
case "$(uname -s)" in
  Linux) HOW_TO_GET_CORE="run ./scripts/fetch-logos-core.sh — it unpacks the published Linux AppImage (checksum-pinned, no installer, no root, no FUSE, no display). Or set LOGOS_APP/LOGOS_CORE_LIB at an existing install." ;;
  *)     HOW_TO_GET_CORE="install Logos Basecamp from the .dmg, or set LOGOS_CORE_LIB. On macOS it ships inside the app and there is no separate download; on Linux ./scripts/fetch-logos-core.sh unpacks it from the AppImage." ;;
esac
need "$CORE_LIB"  "liblogos_core (the Logos Core runtime)" \
     "$HOW_TO_GET_CORE"
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
machine; docs/limitations.md lists what 'prepared' means, and which of these can
be fetched (on Linux, the runtime: ./scripts/fetch-logos-core.sh) and which
cannot."

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
# THE VARIANT FOR THIS MACHINE, not the first one in the archive.
#
# This used to read `tar tzf … | head -n1`, which was right for exactly as long
# as every package carried one variant. The moment `module/agent.lgx` gained a
# `linux-amd64` variant beside the `darwin-arm64` one, a Linux run installed the
# **dylib** — tar lists them alphabetically — and got as far as a `main` the
# package really does contain before anything complained. There is no fallback
# to "whatever is in there": a variant for another platform is a module
# directory that looks complete and can never load, and Basecamp reports a
# module that fails to load to nobody.
#
# Checked per package rather than once, because `--alongside` installs four and
# the companions are built on the machine that is about to run them.
VARIANT="${LGX_VARIANT:-$DEFAULT_VARIANT}"

install_package() { # package-path module-name
  local pkg="$1" name="$2" dest="$USER_MODULES/$2" main have
  have=$(tar tzf "$pkg" | sed -n 's|^variants/\([^/]*\)/$|\1|p')
  printf '%s\n' "$have" | grep -qxF "$VARIANT" \
    || die "$pkg has no '$VARIANT' variant, which is the one this machine needs.
It carries: $(printf '%s ' $have)
Build and package one (docs/basecamp.md), or set LGX_VARIANT deliberately."
  echo "install   $pkg  variant $VARIANT"
  echo "          -> $dest"
  rm -rf "$dest"
  mkdir -p "$dest" || die "could not create $dest"
  tar xzf "$pkg" -C "$dest" || die "could not unpack $pkg"
  [ -d "$dest/variants/$VARIANT" ] \
    || die "$pkg has no variant '$VARIANT' (has: $(printf '%s ' $have))"
  mv "$dest/variants/$VARIANT"/* "$dest/" && rm -rf "$dest/variants"
  printf '%s' "$VARIANT" > "$dest/variant"
  # The manifest's `main` must name a file that is really there: a `main` naming
  # a file the package does not contain is an invisible load failure — the host
  # resolves it, finds nothing, and says nothing. The harness asserts this too;
  # failing here says which half is wrong.
  main=$(python3 -c "
import json,sys
m=json.load(open('$dest/manifest.json'))['main']
print(m['$VARIANT'] if isinstance(m,dict) else m)" 2>/dev/null)
  [ -n "$main" ] || die "$name: manifest.json names no main for $VARIANT"
  [ -e "$dest/$main" ] \
    || die "$name: manifest.json names main=$main, which is not in the package"
  echo "          installed, main=$main"
}

install_package "$ROOT/$LGX" "$MODULE_NAME"
echo

# ── 1b. and the modules the criterion says it has to run beside ───────────
if [ "$ALONGSIDE" -eq 1 ]; then
  PKGDIR="$COMPANIONS_DIR/pkg"
  for name in "${COMPANIONS[@]}"; do
    [ -f "$PKGDIR/$name.lgx" ] || die "
no $PKGDIR/$name.lgx.

Build the companion modules first — from their published sources, unmodified:

    ./scripts/build-companion-modules.sh

It fetches logos-co/logos-{storage,delivery,wallet}-module at pinned revisions,
stages the external libraries each one's metadata.json declares, and packages
each as an .lgx. docs/basecamp.md §'Alongside the wallet, storage and messaging
modules' lists what it needs on the machine."
  done

  # The other half of the criterion — "without requiring modifications to those
  # modules" — checked here and not only at build time, so that the single
  # command a reviewer runs is also the command that proves it. `git status
  # --porcelain` on a tracked file, exactly as examples/agent-console/run.sh
  # does for module/.
  echo
  for name in "${COMPANIONS[@]}"; do
    case "$name" in
      storage_module)  repo="$COMPANIONS_DIR/src/logos-storage-module" ;;
      delivery_module) repo="$COMPANIONS_DIR/src/logos-delivery-module" ;;
      wallet_module)   repo="$COMPANIONS_DIR/src/logos-wallet-module" ;;
    esac
    [ -d "$repo/.git" ] || die "$name: no checkout at $repo to check for modifications"
    dirty="$(git -C "$repo" status --porcelain --untracked-files=no)"
    [ -z "$dirty" ] || die "
$name: tracked files differ from the published revision in $repo:
$dirty
The criterion is 'without requiring modifications to those modules'. Revert
them, or the run below would be proving something else."
    echo "unmodified $name @ $(git -C "$repo" rev-parse --short HEAD)  ($repo)"
  done
  echo
  for name in "${COMPANIONS[@]}"; do
    install_package "$PKGDIR/$name.lgx" "$name"
  done
  export LOGOS_ALONGSIDE="$(IFS=,; echo "${COMPANIONS[*]}")"
fi
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
if [ "$ALONGSIDE" -eq 1 ]; then
  echo "run       headless: init → add_modules_dir → start → load_module(${LOGOS_ALONGSIDE//,/) → load_module(}) → load_module($MODULE_NAME) → configure → start"
else
  echo "run       headless: init → add_modules_dir → start → load_module($MODULE_NAME) → configure → start"
fi
echo
[ -z "$RUN_LD_PATH" ] \
  || export LD_LIBRARY_PATH="$RUN_LD_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
LOGOS_HOST_PATH="$HOST_BIN" QT_PLUGIN_PATH="$QT_PLUGINS" QT_QPA_PLATFORM=offscreen \
  "$HARNESS" "$CORE_LIB" "$EMBEDDED_DIR" "$USER_MODULES" "$PERSISTENCE" \
             "$MODULE_NAME" "$OWNER" "$POLICY_HEX"
rc=$?
echo
if [ $rc -eq 0 ]; then
  echo "Logos Core ran the module headless and it reports itself configured and"
  echo "started, bound to owner $OWNER"
  echo "and to policy account $POLICY."
  if [ "$ALONGSIDE" -eq 1 ]; then
    echo
    echo "It did so with ${LOGOS_ALONGSIDE//,/, } loaded in the same runtime, each"
    echo "built from an unmodified upstream checkout and each answering across the"
    echo "runtime's own transport."
  fi
else
  echo "the headless run failed (exit $rc)" >&2
fi
exit $rc
