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
#   2. takes the harness for this platform out of `module/harness/<variant>/`,
#      built and committed the way the plugin is, after checking it against the
#      record in `module/harness/harness.sources` — or compiles it from
#      `module/tests/logos_core_load_test.cpp` when there is none for this
#      variant, or when HARNESS_FROM_SOURCE=1 asks for that;
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
# Every platform the Logos app is published for — macOS arm64, x86-64 Linux and
# aarch64 Linux — out of the same `module/agent.lgx` and the same
# `module/harness`. It installs the variant for the machine it is on, and
# refuses rather than installing another platform's binary into a directory that
# would then look complete.
#
# WHAT IT DOES NOT DO, AND WILL NOT PRETEND TO
#
# It needs a Logos Core runtime, and on macOS that means an installed
# LogosBasecamp.app; on Linux `scripts/fetch-logos-core.sh` unpacks the
# published AppImage in one checksum-pinned command. Every prerequisite is
# checked below and named in the error when it is missing, so a machine that
# cannot run this says which piece it lacks instead of failing somewhere inside
# a compile. docs/limitations.md carries the same list as prose.
#
# What it no longer needs is a BUILD TOOLCHAIN. It used to compile the harness
# on every machine, so a C++17 compiler, Qt 6.9.2 with qtremoteobjects, a
# logos-cpp-sdk checkout and nlohmann/json were prerequisites of *running* it —
# and "a machine that can run Logos Core still cannot run this" is not "any
# machine". The harness is now built once per variant, committed under
# `module/harness/`, and checked against its source the way the plugin is
# (`scripts/check-package-fresh.py`). The four toolchain prerequisites are
# needed only by whoever rebuilds it:
#
#     ./scripts/logos-core-headless.sh --build-harness   # build and record it
#     HARNESS_FROM_SOURCE=1 ./scripts/logos-core-headless.sh storage
#
# The shipped harness resolves Qt from the Logos app's own bundled 6.9.2 —
# `$APP/usr/lib` on Linux, `$APP/Contents/Frameworks` on macOS — rather than
# from a Qt SDK that a machine without a toolchain does not have. That is the
# same Qt `logos_host` loads the plugin under, so it cannot skew from it.
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
#
# --check
#
# Every prerequisite above, named the same way, and then nothing: no install, no
# compile, no runtime started. It exists because this half is the SECOND half of
# `scripts/deploy-and-configure.sh`, and a machine that discovers it has no
# `liblogos_core` only after the first half has anchored three agents has paid
# real transactions for the discovery. So the wrapper asks this question before
# the chain half runs, and it asks it by running this script rather than by
# keeping a second copy of the platform table above — which would drift.
#
# The one prerequisite `--check` treats as soft is `artifacts/agents.tsv`: it is
# an INPUT PRODUCED BY THE OTHER HALF, so on a machine that has not run the
# chain half yet its absence is not a defect of this machine. It is reported and
# not counted. Everything the wrapper needs to be strict about — the row exists,
# and the envelope in it is the one on chain — the wrapper checks itself, after
# the half that writes it has exited 0.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

ALONGSIDE=0
BUILD_HARNESS=0
CHECK_ONLY=0
args=()
for a in "$@"; do
  case "$a" in
    --alongside) ALONGSIDE=1 ;;
    --build-harness) BUILD_HARNESS=1 ;;
    --check) CHECK_ONLY=1 ;;
    *) args+=("$a") ;;
  esac
done
set -- ${args[@]+"${args[@]}"}

CATEGORY="${1:-${AGENT_CATEGORY:-storage}}"
MANIFEST="${MANIFEST:-artifacts/agents.tsv}"
LGX="${LGX:-module/agent.lgx}"
MODULE_NAME="${MODULE_NAME:-agent}"
BUILD=""   # build-headless/<variant>; set once the variant is known, below
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
    QT_LINK=(-framework QtCore -framework QtRemoteObjects -framework QtNetwork)
    CORE_LINK=(-Wl,-undefined,dynamic_lookup)
    # Only for a harness built HERE, for here. The shipped one is linked with
    # neither of these: an LC_RPATH naming the build machine's Qt is a path that
    # exists on one computer, and scripts/check-package-fresh.py refuses a
    # shipped harness that carries one.
    LOCAL_RPATH=(-Wl,-rpath,"$QT_ROOT/lib" -Wl,-rpath,"$APP/Contents/Frameworks")
    EXTRA_INC="${EXTRA_INCLUDE_DIR:-/opt/homebrew/include}"
    DEFAULT_VARIANT="darwin-arm64"
    # Nothing to prepend: `-rpath QT_ROOT/lib` is linked into the harness ahead of
    # the app's Frameworks directory, so one Qt serves both.
    RUN_LD_PATH=""
    # The shipped harness has no rpath at all, so this is how it finds Qt — the
    # app's own frameworks, which is the Qt `logos_host` loads the plugin under.
    # dyld resolves an `@rpath/QtCore.framework/...` install name out of
    # DYLD_FRAMEWORK_PATH, and this process is not a restricted one, so the
    # variable is not stripped.
    RUN_FRAMEWORK_PATH="$APP/Contents/Frameworks"
    QT_RUNTIME_CHECK="$APP/Contents/Frameworks/QtRemoteObjects.framework"
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
    QT_LINK=(-L"$QT_ROOT/lib" -lQt6Core -lQt6RemoteObjects -lQt6Network)
    # The harness LINKS the runtime here, where macOS only defers to it.
    # `-undefined dynamic_lookup` is a Mach-O feature and has no ELF equivalent
    # for an executable: `--allow-shlib-undefined` governs undefined symbols in
    # shared libraries, not in the program, so the link ends in
    # `undefined reference to TokenManager::instance()` and six more. Every one
    # of them is exported by `liblogos_core.so`, so the honest fix is to say so.
    # This does not change what is being tested: the runtime is still opened by
    # path at run time, and `token_manager.cpp` is still not compiled in — a
    # second copy of that singleton is the failure the note below describes.
    CORE_LINK=(-Wl,--allow-shlib-undefined
               -L"$(dirname "$CORE_LIB")" -llogos_core)
    # Only for a harness built HERE, for here — see the macOS note above. Both
    # of these name directories on the machine that ran the compiler.
    LOCAL_RPATH=(-Wl,-rpath,"$QT_ROOT/lib" -Wl,-rpath,"$(dirname "$CORE_LIB")")
    EXTRA_INC="${EXTRA_INCLUDE_DIR:-/usr/include}"
    # Qt from QT_ROOT first, then the app's own directory for everything else it
    # bundles (boost, spdlog, its own OpenSSL). A dependency is resolved from
    # LD_LIBRARY_PATH before the object's own DT_RUNPATH, and `liblogos_core.so`
    # has `RUNPATH: $ORIGIN` — so without this line the process would load Qt
    # twice under one soname each and the first one to be needed would decide.
    # This is the same ordering macOS gets for free: there, `-rpath QT_ROOT/lib`
    # is linked into the harness ahead of the app's Frameworks directory.
    RUN_LD_PATH="$QT_ROOT/lib:$APP/usr/lib"
    # The shipped harness has no RUNPATH, and no Qt SDK is on the machine to put
    # in one. It runs against the app's own Qt 6.9.2 — the same libraries
    # `logos_host` loads the plugin under, which is the one Qt that cannot skew
    # from the runtime. liblogos_core.so is in the same directory.
    RUN_LD_PATH_SHIPPED="$APP/usr/lib"
    QT_RUNTIME_CHECK="$APP/usr/lib/libQt6RemoteObjects.so.6"
    ;;
  *) die "unsupported platform: $(uname -s)" ;;
esac
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"

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

# ── which harness runs, decided before anything is checked ────────────────
#
# The harness is a real program: it drives `liblogos_core`'s C API and then
# calls the loaded module across the runtime's own transport through
# logos-cpp-sdk, because that C API can *load* a module and cannot *call a
# method* on one. It used to be COMPILED on every run, which made a C++17
# compiler, Qt 6.9.2 with qtremoteobjects, a logos-cpp-sdk checkout and
# nlohmann/json prerequisites of *running this command at all* — so a machine
# that could run Logos Core still could not run this until it had a build
# toolchain.
#
# It is therefore built once per variant and committed, exactly as the plugin
# is: `module/harness/<variant>/logos_core_load_test`, with
# `module/harness/harness.sources` recording what each one was built from and
# `scripts/check-package-fresh.py` checking the shipped bytes against the
# committed source. This block decides which harness runs, and the prerequisite
# list below follows from that decision instead of being the same list every
# time.
#
# The variant is settled here rather than at install time because the harness is
# per-variant too, and because a machine this package has no variant for should
# say so before it checks for a Qt it does not need.
VARIANT="${LGX_VARIANT:-$DEFAULT_VARIANT}"

# Per variant, because the three variants are built out of ONE checkout shared
# into three containers — that is how they are built, and it is what
# docs/basecamp.md tells the next person to do. With a single build directory
# the Mach-O `.o` files from the macOS build are newer than the SDK sources the
# Linux build reads, so the Linux run skips compiling them and hands Mach-O
# objects to `ld`; and a harness built on macOS is "up to date" for a Linux run,
# which then dies of `Exec format error` several steps into a deployment.
BUILD="${HEADLESS_BUILD_DIR:-build-headless/$VARIANT}"
PERSISTENCE="${LOGOS_PERSISTENCE_DIR:-$BUILD/persistence}"
HARNESS_DIR="$ROOT/module/harness"
HARNESS_RECORD="$HARNESS_DIR/harness.sources"
SHIPPED_HARNESS="$HARNESS_DIR/$VARIANT/logos_core_load_test"
SRC=module/tests/logos_core_load_test.cpp

if [ -n "${HARNESS_BIN:-}" ]; then
  HARNESS="$HARNESS_BIN"; HARNESS_MODE=given
elif [ "$BUILD_HARNESS" -eq 1 ]; then
  HARNESS="$SHIPPED_HARNESS"; HARNESS_MODE=build
elif [ "${HARNESS_FROM_SOURCE:-0}" = 1 ]; then
  HARNESS="$BUILD/logos_core_load_test"; HARNESS_MODE=source
elif [ -x "$SHIPPED_HARNESS" ]; then
  HARNESS="$SHIPPED_HARNESS"; HARNESS_MODE=shipped
else
  HARNESS="$BUILD/logos_core_load_test"; HARNESS_MODE=source
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
need_cmd() { # command description how-to-get-it
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing: $2" >&2
    echo "         no '$1' on PATH" >&2
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
need "$LGX"       "the packaged module" \
     "module/agent.lgx is committed; run module/package-basecamp.sh to rebuild it."

# The toolchain is a prerequisite of BUILDING the harness and of nothing else.
# Checked only in the modes that build one, so that a machine which merely runs
# the shipped binary is not told it needs a compiler it will never invoke.
if [ "$HARNESS_MODE" = source ] || [ "$HARNESS_MODE" = build ]; then
  # Why this machine is being asked for a compiler at all, which is not the same
  # sentence in the three cases that get here: it asked to build one, there is
  # no committed harness for this variant, or there is one and it was declined.
  if [ -x "$SHIPPED_HARNESS" ]; then
    WHY_BUILD="module/harness/$VARIANT/logos_core_load_test is committed and needs no toolchain; unset HARNESS_FROM_SOURCE to use it."
  else
    WHY_BUILD="there is no committed harness for '$VARIANT' — module/harness holds $(ls "$HARNESS_DIR" 2>/dev/null | grep -v harness.sources | tr '\n' ' ')— so this variant can only be built here."
  fi
  need_cmd c++    "a C++17 compiler" \
       "install one (build-essential, or Xcode command line tools). $WHY_BUILD"
  need "$QT_ROOT" "Qt 6.9.2" \
       "aqtinstall: python3 -m aqt install-qt <host> desktop 6.9.2 <arch> -m qtremoteobjects — see docs/basecamp.md. Set QT_ROOT."
  need "$SDK/cpp/logos_api.cpp" "logos-cpp-sdk checkout" \
       "clone https://github.com/logos-co/logos-cpp-sdk and set LOGOS_CPP_SDK_ROOT."
else
  # What the shipped harness needs instead, and it is not a toolchain: the Qt
  # the Logos app itself bundles. Named here rather than left to the dynamic
  # loader, whose message for a missing one is `error while loading shared
  # libraries` with no mention of Logos at all.
  need "$QT_RUNTIME_CHECK" "the Logos app's own Qt 6.9.2 libraries" \
       "the shipped harness resolves Qt from the runtime you already have, so this is part of the Logos Core install above rather than a separate download. On Linux ./scripts/fetch-logos-core.sh unpacks it with the rest of the AppImage."
  need "$HARNESS_RECORD" "module/harness/harness.sources" \
       "it is committed beside the harness binaries and says which source each was built from; without it the shipped binary cannot be checked and is not run."
fi

[ "$missing" -eq 0 ] || die "
$missing prerequisite(s) missing. docs/limitations.md lists what this command
needs, and which of these can be fetched (on Linux, the runtime:
./scripts/fetch-logos-core.sh) and which cannot. Building the harness rather
than using the committed one additionally needs a toolchain; that is what
HARNESS_FROM_SOURCE=1 and --build-harness ask for."

# ── the harness: one recipe, two destinations ─────────────────────────────
#
# A plain compile, not a CMake target, so it cannot silently stop being built.
# `token_manager.cpp` is deliberately NOT among the SDK translation units: it
# holds a singleton, and compiling a second copy into this executable makes it
# win the symbol, start empty, and reject every call to the module with "auth
# token not recognized" — which reads like a permissions problem and is not.
#
# The only difference between the harness built here for the machine it is on
# and the one committed for every variant is the rpaths: the shipped binary
# carries none, because every rpath this script can offer names a directory on
# the machine that ran the compiler — `$QT_ROOT/lib`, or wherever this operator
# happened to unpack the AppImage. It finds Qt and the runtime through the
# environment the run step sets instead, out of the Logos app itself.
# `check-package-fresh.py` refuses a shipped harness that carries an rpath, so
# this is asserted rather than intended.
#
# One recipe, because a second one for the shipped binary is a second one to
# drift: what a reviewer builds from source and what this repository ships are
# then no longer the same program, and the "you can rebuild it yourself" answer
# stops meaning anything.
compile_harness() { # output-path with-local-rpaths(0|1)
  local out="$1" want_rpaths="$2" rpaths=()
  local MOC="$QT_ROOT/libexec/moc"
  [ -x "$MOC" ] || die "no moc at $MOC (an incomplete Qt install: see docs/basecamp.md)"
  mkdir -p "$BUILD/sdkobj" "$(dirname "$out")" || die "could not create $(dirname "$out")"
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

  [ "$want_rpaths" -eq 1 ] && rpaths=("${LOCAL_RPATH[@]}")
  c++ -std=c++17 -o "$out" "$SRC" "$BUILD"/sdkobj/*.o \
      -I"$SDK/cpp" -I"$SDK/core" -I"$EXTRA_INC" "${QT_INC[@]}" \
      "${QT_LINK[@]}" "${CORE_LINK[@]}" ${rpaths[@]+"${rpaths[@]}"} \
    || die "the harness did not build"
}

# ── --build-harness: build the committed one, for this variant, and stop ──
#
# One variant per machine, the way the plugin is packaged one variant per
# machine: this is what runs in each of the three containers docs/basecamp.md
# names. It writes module/harness/harness.sources, which records what the
# binary was built from — the same provenance record module/agent.lgx.sources
# is, checked by the same script.
if [ "$BUILD_HARNESS" -eq 1 ]; then
  echo "build     $SHIPPED_HARNESS"
  echo "          variant $VARIANT, no rpath, Qt resolved from the app at run time"
  rm -f "$SHIPPED_HARNESS"
  compile_harness "$SHIPPED_HARNESS" 0
  echo "          built"
  echo
  QT_VERSIONS="$("$QT_ROOT/libexec/moc" --version 2>/dev/null | head -n1)" \
  COMPILER="$(c++ --version 2>/dev/null | head -n1)" \
  SDK_REVISION="$(git -C "$SDK" rev-parse HEAD 2>/dev/null)" \
  BUILT_ON="$(uname -srm)" \
  LOGOS_CPP_SDK_ROOT="$SDK" \
    python3 "$ROOT/scripts/write-harness-record.py" "$VARIANT" "$SHIPPED_HARNESS" \
    || die "the harness was built and the record was not written; the binary is
not usable until it is, because the run step checks the binary against it."
  echo
  echo "Now check it the way CI does, from the source in this commit:"
  echo "  ./scripts/check-package-fresh.py"
  exit 0
fi

# ── the agent this run configures ─────────────────────────────────────────
#
# Read BY HEADER NAME. This manifest has gained columns twice — `claim_account`,
# `record_prefix`, `claim_tx` — and a positional read returns a plausible wrong
# value rather than failing.
# A HEADER NAME THAT IS NOT IN THE FILE WAS NOT A FAILURE — IT WAS THE WHOLE ROW.
#
# `c` is set only when the loop finds the name, so a manifest that had lost or
# renamed the column left it unset, and `$c` in awk is `$0`: the entire
# tab-separated line. The caller's only guard is `[ -n "$OWNER" ]`, and a whole
# line is very much non-empty. Measured under mawk, which is the awk in the
# `ubuntu:24.04` container `scripts/harness-no-toolchain.sh` runs this command
# in, against a copy of the manifest with `owner` renamed to `owner_account`:
#
#   VALUE=[storage  9Xpkkvos…  5Sa13Ny…  EZSN69nj…  6FscNXjN…  50  500  1000  …]
#
# The run then passes that to the harness as the owner to bind the module to.
# (BSD awk instead makes it a runtime error on stderr and an empty value, so the
# same drift reads as "no row for 'storage'" — a true-sounding message about a
# row that is right there.) Either way the one thing nobody is told is that the
# column is gone, which is precisely what this reader exists to notice: the
# comment above it says a positional read "returns a plausible wrong value rather
# than failing", and this was doing that under a by-name spelling.
#
# So a missing HEADER now exits 3, which is a different answer from a missing
# ROW, and both are checked at the call site.
col() { # file header row-key -> value on stdout; exit 3 if there is no such header
  awk -F'\t' -v w="$2" -v k="$3" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==w) c=i
            if (!c) exit 3
            next }
    $1==k { print $c; exit }' "$1"
}

# A manifest that is not there yet is not a fault of THIS machine when the
# command that writes it is about to run. `--check` is asked by
# `scripts/deploy-and-configure.sh` BEFORE the chain half, precisely so that a
# missing runtime is found while it still costs nothing — and at that moment a
# fresh operator has no manifest, because the chain half has not written one.
# So this is reported and not counted, and the wrapper is the thing that
# afterwards insists on a row, on the row being for this category, and on the
# envelope in it being the one the chain holds. Every other reading below is
# unchanged: with a manifest present, `--check` reads it exactly as a real run
# does, including the no_such_column control.
if [ "$CHECK_ONLY" -eq 1 ] && [ ! -f "$MANIFEST" ]; then
  OWNER=""; POLICY=""; AGENT_ID=""; POLICY_HEX=""
  echo "manifest  $MANIFEST is not here yet"
  echo "          it is the on-chain half's output, not this machine's to have:"
  echo "          ./scripts/deploy-and-configure.sh runs that half first and hands"
  echo "          this one the file it wrote"
  echo
else
[ -f "$MANIFEST" ] || die "no $MANIFEST: run ./scripts/deploy-agents.sh first, or set MANIFEST"
# THE CONTROL, and it is one line: the same reader, asked for a column that
# cannot be there, must refuse. Without it "the manifest has an `owner` column"
# is a sentence this script never tests — it only ever asks for names it expects
# to find, so a reader that answered something for every name would satisfy every
# call below and be caught by none of them.
if col "$MANIFEST" no_such_column "$CATEGORY" >/dev/null 2>&1; then
  die "$MANIFEST answered for a column called 'no_such_column', so the by-name
read below is not reading names and nothing it returns can be trusted."
fi
MANIFEST_COLS="run ./scripts/deploy-agents.sh, which writes it, or migrate the file by hand.
$MANIFEST has: $(head -n1 "$MANIFEST" | tr '\t' ' ')"
OWNER=$(col "$MANIFEST" owner "$CATEGORY")           || die "$MANIFEST has no 'owner' column. $MANIFEST_COLS"
POLICY=$(col "$MANIFEST" policy_account "$CATEGORY") || die "$MANIFEST has no 'policy_account' column. $MANIFEST_COLS"
AGENT_ID=$(col "$MANIFEST" agent_id "$CATEGORY")     || die "$MANIFEST has no 'agent_id' column. $MANIFEST_COLS"
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
fi

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
  # `--check` stops HERE, after the variant has been read out of the package and
  # before a single byte is written. The check that matters on a strange machine
  # is the one above — a package with no variant for this architecture is a
  # module directory that looks complete and can never load — and it costs a
  # `tar tzf`. Removing and re-unpacking somebody's installed module is not
  # something a question should do.
  if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "would install  $pkg  variant $VARIANT"
    echo "          -> $dest"
    return 0
  fi
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
    # The revision FIRST, and not `[ -d "$repo/.git" ]`. That test is satisfied
    # by an empty directory called `.git`, `git status` then writes its
    # complaint to stderr and hands back an empty string, and an empty string
    # reads as "clean" — measured: `mkdir -p fake/.git` printed
    # `unmodified storage_module @   (fake)` and exited 0. An assertion whose
    # pass condition is the absence of a bad string is satisfied by nothing
    # having run, and this one is half of a prize criterion.
    head="$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)"
    [ -n "$head" ] || die "
$name: $repo is not a git checkout, so 'unmodified' cannot be read off it.
Build the companion modules with ./scripts/build-companion-modules.sh, which
fetches each one at the revision it pins."
    dirty="$(git -C "$repo" status --porcelain --untracked-files=no)"
    [ -z "$dirty" ] || die "
$name: tracked files differ from the published revision in $repo:
$dirty
The criterion is 'without requiring modifications to those modules'. Revert
them, or the run below would be proving something else."
    # AND IT IS THE PUBLISHED REVISION, which this did not check. The two lines
    # above establish "a git checkout, with nothing modified in it" -- true of a
    # clean checkout of ANY commit, including one carrying changes that were
    # committed rather than left in the working tree. The header above claims
    # this compares against the published revision; until now only
    # build-companion-modules.sh knew what that was.
    #
    # The pin is read from that script rather than copied here, so there is one
    # place to change it. A pin that cannot be READ is a failure, not a skip:
    # otherwise renaming the variable upstream would silently retire the check.
    pinvar="$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')_REV"
    pin="$(sed -n "s/^${pinvar}=\"\${${pinvar}:-\([0-9a-f]\{7,40\}\)}\".*/\1/p" \
           "$ROOT/scripts/build-companion-modules.sh" | head -1)"
    [ -n "$pin" ] || die "
$name: no ${pinvar} could be read out of scripts/build-companion-modules.sh, so
'the published revision' has no value to compare against. That file is where the
revisions are pinned; if the variable was renamed, rename it here too rather
than letting this check pass on nothing."
    case "$head" in
      "$pin"*) : ;;
      *) die "
$name is at ${head:0:7} and the pinned revision is ${pin:0:7}.
The criterion is that the agent loads alongside these modules UNMODIFIED, and a
clean checkout of a different commit is not that. Re-run
./scripts/build-companion-modules.sh, which fetches each one at its pin." ;;
    esac
    echo "unmodified $name @ ${head:0:7}, the revision pinned for it  ($repo)"
  done
  echo
  for name in "${COMPANIONS[@]}"; do
    install_package "$PKGDIR/$name.lgx" "$name"
  done
  export LOGOS_ALONGSIDE="$(IFS=,; echo "${COMPANIONS[*]}")"
fi
echo

# ── 2. the harness for this variant ───────────────────────────────────────
#
# Shipped by default: `module/harness/<variant>/logos_core_load_test`, built
# once per variant and committed, so this step needs no compiler, no Qt SDK, no
# logos-cpp-sdk checkout and no nlohmann/json. What it does need is the record
# beside it, because a binary in a repository says nothing about where it came
# from — the same reason `module/agent.lgx.sources` exists.
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

case "$HARNESS_MODE" in
  given)
    [ -x "$HARNESS" ] || die "HARNESS_BIN=$HARNESS is not an executable file"
    echo "harness   $HARNESS  (HARNESS_BIN, not checked against any record)"
    ;;

  shipped)
    # The record is checked HERE and not only in CI, because the machine that
    # runs this command is usually not the machine that runs CI, and a binary
    # nobody checked is exactly what the .lgx defects taught this repository not
    # to run. This is the cheap half — the bytes are the bytes that were
    # recorded; `scripts/check-package-fresh.py` is the half that also reads the
    # source those bytes came from.
    want=$(python3 -c "
import json,sys
rec=json.load(open(sys.argv[1]))
v=(rec.get('variants') or {}).get(sys.argv[2])
print((v or {}).get('sha256') or '')" "$HARNESS_RECORD" "$VARIANT") \
      || die "could not read $HARNESS_RECORD"
    [ -n "$want" ] || die "
$HARNESS_RECORD records no '$VARIANT' harness, and there is a binary at
$SHIPPED_HARNESS.
An unrecorded binary is not run: build it with ./scripts/logos-core-headless.sh
--build-harness, which writes the record, or delete it and let this command
compile one from source."
    got=$(sha256_of "$HARNESS")
    [ "$got" = "$want" ] || die "
the shipped harness is not the binary the record was written for:
  $SHIPPED_HARNESS
  sha256 $got
  record $want
Refusing to run it. Either the file was modified after it was recorded, or the
record was. ./scripts/check-package-fresh.py says which."
    echo "harness   $SHIPPED_HARNESS"
    echo "          shipped, sha256 ${got:0:16}, as recorded in module/harness/harness.sources"
    echo "          no compiler, no Qt SDK and no logos-cpp-sdk checkout were needed"
    ;;

  source)
    if [ "$CHECK_ONLY" -eq 1 ]; then
      # The toolchain this needs was checked by name in the gate above; what a
      # check must not do is spend two minutes running it.
      echo "harness   $HARNESS  (would be compiled from $SRC)"
    else
      mkdir -p "$BUILD/sdkobj"
      if [ ! -x "$HARNESS" ] || [ "$SRC" -nt "$HARNESS" ]; then
        echo "build     $HARNESS  (from $SRC)"
        compile_harness "$HARNESS" 1
        echo "          built"
      else
        echo "build     $HARNESS  (up to date)"
      fi
    fi
    ;;
esac
echo

# ── 3. run Logos Core headless ────────────────────────────────────────────
#
# `--check` stops one line short of this, which is the only line that starts a
# runtime. Everything above it has run: the prerequisite gate, the harness
# against the record it was written for, the package against the variant this
# machine needs, and — when there is a manifest — the by-name read of the agent,
# its owner and its policy account, control included.
if [ "$CHECK_ONLY" -eq 1 ]; then
  echo "check     every prerequisite of this half is present on this machine"
  echo "          nothing was installed, compiled or started"
  exit 0
fi
mkdir -p "$PERSISTENCE"
if [ "$ALONGSIDE" -eq 1 ]; then
  echo "run       headless: init → add_modules_dir → start → load_module(${LOGOS_ALONGSIDE//,/) → load_module(}) → load_module($MODULE_NAME) → configure → start"
else
  echo "run       headless: init → add_modules_dir → start → load_module($MODULE_NAME) → configure → start"
fi
echo
# Where the harness finds Qt, and it differs by which harness this is. One built
# here carries an rpath to the Qt it was compiled against; the shipped one
# carries no rpath at all and takes the app's own Qt 6.9.2 — the same libraries
# `logos_host` loads the plugin under, which is the one Qt in this picture that
# cannot be a version other than the runtime's.
if [ "$HARNESS_MODE" = shipped ]; then
  [ -z "${RUN_LD_PATH_SHIPPED:-}" ] \
    || export LD_LIBRARY_PATH="$RUN_LD_PATH_SHIPPED${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  [ -z "${RUN_FRAMEWORK_PATH:-}" ] \
    || export DYLD_FRAMEWORK_PATH="$RUN_FRAMEWORK_PATH${DYLD_FRAMEWORK_PATH:+:$DYLD_FRAMEWORK_PATH}"
else
  [ -z "$RUN_LD_PATH" ] \
    || export LD_LIBRARY_PATH="$RUN_LD_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
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
