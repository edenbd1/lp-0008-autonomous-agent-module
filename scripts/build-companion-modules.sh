#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Build the wallet, storage and messaging modules that Logos Core is supposed to
# run this agent beside — from their published sources, unmodified — and package
# each one as an `.lgx` that `scripts/logos-core-headless.sh --alongside`
# installs next to `module/agent.lgx`.
#
#   ./scripts/build-companion-modules.sh
#
# WHY THIS EXISTS, AND WHAT IT REPLACES
#
# The prize asks that "the agent module loads and runs inside Logos Core
# alongside the wallet, storage, and messaging modules without requiring
# modifications to those modules". This repository said for a long time that no
# submission could close that against Logos Basecamp 0.2.2, because the app
# bundle ships only `capability_module`, `package_manager` and
# `package_downloader`. **That conclusion was wrong.** Basecamp's *bundle* ships
# three modules; Logos Core loads from the USER modules directory as well, which
# is exactly how this repository's own agent module gets in
# (`scripts/logos-core-headless.sh`). The modules named in the criterion exist
# and are public — `logos-co/logos-modules` aggregates them as submodules — so
# the criterion was unbuilt, not impossible.
#
# WHAT "WITHOUT MODIFICATIONS" MEANS HERE, AND HOW IT IS CHECKED
#
# Each module is fetched into `build-companions/src/<repo>` at the pinned
# revision below and built there. Afterwards this script runs
#
#     git -C <checkout> status --porcelain --untracked-files=no
#
# and refuses to package if it prints anything — the same mechanical check
# `examples/agent-console/run.sh` makes against `module/`. It then lists what
# the build *added*, and requires every added path to be under `lib/` or
# `generated_code/`, which are the two directories the modules' own published
# build creates: `logos-module-builder`'s `lib/modulePreConfigure.nix` stages
# external libraries into `lib/` (`copyExternalLibsToLib`) and writes
# `logos-cpp-generator`'s glue into `./generated_code` (`universalCodegen`).
# Nothing under `src/`, no `CMakeLists.txt`, no `metadata.json` is touched, and
# the check fails if one ever is.
#
# THE PART THAT IS NOT PRETTY, STATED HERE RATHER THAN DISCOVERED LATER
#
# `logos-delivery-module` does not build against the newest `logos-delivery`.
# Its C ABI moved on 2026-08-14 — the reply callbacks went from
# `(int, const char*, size_t, void*)` to `(int err, const char* reply,
# const char* err_msg, void*)` — and the module has not followed. So the library
# is built at `f8b0365`, which is the revision the module's OWN `flake.lock`
# pins, not at the tip. That is the module's published build input; using the
# tip instead is what produces seventeen compile errors in `api_call_handler.h`.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

OUT="${COMPANIONS_DIR:-$ROOT/build-companions}"
SRC="$OUT/src"; BUILD="$OUT/build"; PKG="$OUT/pkg"

# ── the pinned revisions ──────────────────────────────────────────────────
#
# Pinned to what has been proven, not to what is newest — docs/recon.md's rule,
# and the delivery ABI break above is what it is for. Each is the default-branch
# tip at the time this was built; `logos-co/logos-modules` aggregates older
# submodule pointers (April 2026), which predate the `universal` interface these
# revisions use and would need a different SDK.
STORAGE_MODULE_REV="${STORAGE_MODULE_REV:-f6bfab346040e1a757f8528258819552e83503c7}"
DELIVERY_MODULE_REV="${DELIVERY_MODULE_REV:-3f0f2d8b202f427a96179407bbf18b449935da7c}"
WALLET_MODULE_REV="${WALLET_MODULE_REV:-f6f9c160410db824531f42dd8b27ccecd39ca589}"
# github:status-im/go-wallet-sdk, as pinned by logos-wallet-module's flake.nix.
GO_WALLET_SDK_REV="${GO_WALLET_SDK_REV:-f6c0924394c5bdf361bd16b739a80432e68291f5}"
# github:logos-messaging/logos-delivery, as pinned by logos-delivery-module's
# own flake.lock. NOT the tip; see the note above.
DELIVERY_LIB_REV="${DELIVERY_LIB_REV:-f8b036594ea2a36b529e10b584b7d2851a3ac5c8}"
# nim-taskpools, as pinned by logos-delivery's nimble.lock at that revision.
# `nimble setup --localdeps` resolves a different one (7d96007), which has no
# `taskpools/channels_spsc_single` — the module the locked `ffi` imports — so the
# build fails with `cannot open file: taskpools/channels_spsc_single` unless the
# lock's revision is put in place. This is a dependency of the library, not of
# any module, and nothing in the three module checkouts is affected by it.
TASKPOOLS_REV="${TASKPOOLS_REV:-9e8ccc754631ac55ac2fd495e167e74e86293edb}"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  VARIANT=darwin-arm64; SO=dylib ;;
  Darwin-x86_64) VARIANT=darwin-amd64; SO=dylib ;;
  Linux-x86_64)  VARIANT=linux-amd64;  SO=so ;;
  Linux-aarch64) VARIANT=linux-arm64;  SO=so ;;
  *) echo "unsupported host: $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
VARIANT="${LGX_VARIANT:-$VARIANT}"

case "$(uname -s)" in
  Darwin) QT_ROOT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/macos}"
          EXTRA_INC="${EXTRA_INCLUDE_DIR:-/opt/homebrew/include}" ;;
  *)      QT_ROOT="${QT_ROOT:-$HOME/logos/Qt/6.9.2/gcc_64}"
          EXTRA_INC="${EXTRA_INCLUDE_DIR:-/usr/include}" ;;
esac
SDK="${LOGOS_CPP_SDK_ROOT:-$HOME/logos/src/logos-cpp-sdk}"
BUILDER="${LOGOS_MODULE_BUILDER_ROOT:-$HOME/logos/src/logos-module-builder}"
MODULE_LIB="${LOGOS_MODULE_ROOT:-$HOME/logos/src/logos-module}"
GEN="${LOGOS_CPP_GENERATOR:-$HOME/logos/src/build/cpp-generator/bin/logos-cpp-generator}"
STORAGE_LIB_SRC="${STORAGE_SRC:-$ROOT/_external/logos-storage-nim}"
DELIVERY_LIB_SRC="${DELIVERY_LIB_SRC:-$OUT/logos-delivery}"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# ── prerequisites, each named individually ────────────────────────────────
missing=0
need() { # path description how-to-get-it
  if [ ! -e "$1" ]; then
    echo "missing: $2" >&2
    echo "         expected at $1" >&2
    echo "         $3" >&2
    missing=$((missing + 1))
  fi
}
need "$QT_ROOT/lib/cmake/Qt6/Qt6Config.cmake" "Qt 6.9.2, complete (with its CMake config)" \
     "see docs/basecamp.md — the version is a CEILING, not a floor. Set QT_ROOT."
need "$SDK/cpp/logos_api.cpp" "logos-cpp-sdk checkout" \
     "clone https://github.com/logos-co/logos-cpp-sdk and set LOGOS_CPP_SDK_ROOT."
need "$BUILDER/cmake/LogosModule.cmake" "logos-module-builder checkout" \
     "clone https://github.com/logos-co/logos-module-builder and set LOGOS_MODULE_BUILDER_ROOT."
need "$MODULE_LIB/src/interface.h" "logos-module checkout" \
     "clone https://github.com/logos-co/logos-module and set LOGOS_MODULE_ROOT."
need "$STORAGE_LIB_SRC/build/libstorage.$SO" "the Logos Storage library, built" \
     "scripts/exercise-nodes.sh names the clone-and-make; or set STORAGE_SRC."
command -v cmake >/dev/null || { echo "missing: cmake" >&2; missing=$((missing+1)); }
command -v go    >/dev/null || {
  echo "missing: go (the wallet module links go-wallet-sdk's c-archive)" >&2
  missing=$((missing+1)); }
LGX_BIN="${LGX_BIN:-}"
for candidate in "$LGX_BIN" "$HOME/logos/src/logos-package/build/lgx" "$(command -v lgx || true)"; do
  if [ -n "$candidate" ] && [ -x "$candidate" ]; then LGX_BIN="$candidate"; break; fi
done
[ -n "$LGX_BIN" ] && [ -x "$LGX_BIN" ] || {
  echo "missing: lgx (the packager Basecamp's own packages are built with)" >&2
  echo "         build it from https://github.com/logos-co/logos-package, set LGX_BIN" >&2
  missing=$((missing+1)); }
[ "$missing" -eq 0 ] || die "$missing prerequisite(s) missing; docs/limitations.md lists them as prose"

export LOGOS_MODULE_BUILDER_ROOT="$BUILDER"
export LOGOS_MODULE_ROOT="$MODULE_LIB"
export LOGOS_CPP_SDK_ROOT="$SDK"

mkdir -p "$SRC" "$BUILD" "$PKG"

# Sets FETCHED. Not a command substitution: `die` inside one only kills the
# subshell, and this script would then carry on packaging whatever was left over
# from the previous run — which is the shape of "shipped is not built from what
# is committed" that module/package-basecamp.sh already exists to refuse.
FETCHED=""
fetch() { # url rev
  local url="$1" rev="$2" name dir
  name="$(basename "$url")"; dir="$SRC/$name"
  if [ ! -d "$dir/.git" ]; then
    mkdir -p "$dir"
    git -C "$dir" init -q . || die "could not init $dir"
    git -C "$dir" remote add origin "$url" || die "could not set origin on $dir"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$rev" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$rev" || die "could not fetch $url@$rev"
    git -C "$dir" checkout -q "$rev" || die "could not check out $url@$rev"
  fi
  FETCHED="$dir"
}

# ── 1. the Logos Delivery library, at the module's own pin ────────────────
say "[1/6] liblogosdelivery @ ${DELIVERY_LIB_REV:0:7} (logos-delivery-module's flake.lock pin)"
DELIVERY_DYLIB="$DELIVERY_LIB_SRC/build/liblogosdelivery.$SO"
if [ ! -f "$DELIVERY_DYLIB" ]; then
  command -v nim >/dev/null || export PATH="$HOME/.nimble/bin:$PATH"
  command -v nim >/dev/null \
    || die "no nim on PATH (nimble installs it into ~/.nimble/bin and does not add it)"
  if [ ! -d "$DELIVERY_LIB_SRC/.git" ]; then
    echo "  cloning (this is a large repository and the build is not quick)"
    git clone -q --recurse-submodules --shallow-submodules \
        https://github.com/logos-messaging/logos-delivery "$DELIVERY_LIB_SRC" \
      || die "could not clone logos-delivery"
  fi
  git -C "$DELIVERY_LIB_SRC" fetch -q --depth 200 origin "$DELIVERY_LIB_REV" \
    || die "could not fetch logos-delivery@$DELIVERY_LIB_REV"
  git -C "$DELIVERY_LIB_SRC" checkout -qf "$DELIVERY_LIB_REV" \
    || die "could not check out logos-delivery@$DELIVERY_LIB_REV"
  ( cd "$DELIVERY_LIB_SRC" && make -j"${JOBS:-8}" build-deps ) \
    || die "logos-delivery's build-deps failed"
  # The taskpools pin, see the header note. Done after `build-deps` because that
  # is what populates nimbledeps/ — and undone by nothing, because a rebuilt
  # nimbledeps re-resolves it wrong every time.
  tp="$(ls -d "$DELIVERY_LIB_SRC"/nimbledeps/pkgs2/taskpools-* 2>/dev/null | head -1)"
  if [ -n "$tp" ] && [ ! -f "$tp/taskpools/channels_spsc_single.nim" ]; then
    echo "  nimble resolved taskpools without channels_spsc_single; installing the"
    echo "  nimble.lock revision ${TASKPOOLS_REV:0:7} over it"
    meta="$(cat "$tp/nimblemeta.json" 2>/dev/null || echo '{}')"
    rm -rf "$tp.locked"
    git clone -q https://github.com/status-im/nim-taskpools "$tp.locked" \
      || die "could not clone nim-taskpools"
    git -C "$tp.locked" checkout -q "$TASKPOOLS_REV" || die "no such taskpools revision"
    rm -rf "$tp.locked/.git" && rm -rf "$tp" && mv "$tp.locked" "$tp"
    printf '%s' "$meta" > "$tp/nimblemeta.json"
  fi
  ( cd "$DELIVERY_LIB_SRC" && make -j"${JOBS:-8}" liblogosdelivery ) \
    || die "liblogosdelivery did not build; the log above says where"
fi
[ -f "$DELIVERY_DYLIB" ] || die "no $DELIVERY_DYLIB after building"
echo "  $DELIVERY_DYLIB  ($(du -h "$DELIVERY_DYLIB" | cut -f1))"

# ── 2. go-wallet-sdk's c-archive ──────────────────────────────────────────
say "[2/6] libgowalletsdk @ ${GO_WALLET_SDK_REV:0:7} (logos-wallet-module's flake.nix pin)"
fetch https://github.com/status-im/go-wallet-sdk "$GO_WALLET_SDK_REV"; GOSDK="$FETCHED"
if [ ! -f "$GOSDK/build/libgowalletsdk.a" ]; then
  ( cd "$GOSDK" && make static-library ) || die "go-wallet-sdk's static-library target failed"
fi
echo "  $GOSDK/build/libgowalletsdk.a  ($(du -h "$GOSDK/build/libgowalletsdk.a" | cut -f1))"

# ── the per-module build ──────────────────────────────────────────────────
#
# `logos-module-builder` locates external libraries in `<module>/lib` and the
# generator's glue in `<module>/generated_code`. Both are the module's own build
# recipe, restated here because Nix is not installed; see the header.
build_module() { # name dir impl-header impl-class [extra cmake args...]
  local name="$1" dir="$2" header="$3" class="$4"; shift 4
  local build="$BUILD/$name"

  ( cd "$dir" && "$GEN" --from-header "src/$header" --backend qt \
         --impl-class "$class" --impl-header "$header" \
         --metadata metadata.json --output-dir ./generated_code >/dev/null ) \
    || die "$name: logos-cpp-generator failed"

  cmake -S "$dir" -B "$build" \
        -DCMAKE_PREFIX_PATH="$QT_ROOT" \
        -DCMAKE_CXX_FLAGS="-I$EXTRA_INC" \
        -DLOGOS_API_STYLE=std "$@" >/dev/null \
    || die "$name: cmake configure failed"

  # The check that costs nothing and saves an afternoon. Qt's minor version is a
  # ceiling: a plugin built against 6.11 makes `logos_core_load_module` return
  # SUCCESS and then answers nothing for twenty seconds. CMake falls through to
  # the system Qt without a word when it cannot find the pinned one, so this is
  # read out of the cache rather than assumed.
  local qtdir; qtdir="$(awk -F= '/^Qt6Core_DIR:/{print $2}' "$build/CMakeCache.txt")"
  case "$qtdir" in
    "$QT_ROOT"/*) ;;
    *) die "$name: cmake found Qt at $qtdir, not under $QT_ROOT — see docs/basecamp.md" ;;
  esac

  cmake --build "$build" -j"${JOBS:-8}" >/dev/null || die "$name: build failed"
  local plugin="$build/modules/${name}_plugin.$SO"
  [ -f "$plugin" ] || die "$name: no $plugin after building"

  if [ "$(uname -s)" = Darwin ]; then
    local refs; refs="$(otool -L "$plugin" | awk '/Qt[A-Za-z]*\.framework/ {print $1, $NF}')"
    [ -n "$refs" ] || die "$name: the plugin references no Qt at all"
    if printf '%s\n' "$refs" | grep -qv '^@rpath/'; then
      echo "$refs" | grep -v '^@rpath/' >&2
      die "$name: a Qt framework is referenced by absolute path — it resolves here and nowhere else"
    fi
    # EVERY framework, not one of them. `grep -q '6\.9\.'` over the whole block
    # is satisfied by a SINGLE matching line, so a plugin linking QtCore 6.9.2
    # and QtRemoteObjects 6.11.0 passed a check whose own failure message reads
    # "the plugin is not linked against Qt 6.9.x". Measured, with otool's output
    # supplied verbatim to the same three lines:
    #
    #   @rpath/QtCore.framework/Versions/A/QtCore 6.9.2)
    #   @rpath/QtRemoteObjects.framework/Versions/A/QtRemoteObjects 6.11.0)
    #   ->  ok    (the script's verdict) Qt through @rpath, 6.9.x
    #
    # and a mixed link is exactly the state the comment above this function
    # exists for: Qt's minor version is a ceiling per module, and the module that
    # exceeds it is the one whose calls time out. So the question is inverted —
    # any line that is NOT 6.9.x is named and fails — which is the same shape as
    # the `@rpath` test three lines up, and which needs `$refs` to be non-empty,
    # already established above.
    local skew; skew="$(printf '%s\n' "$refs" | grep -v ' 6\.9\.' || true)"
    [ -z "$skew" ] || {
      printf '%s\n' "$skew" >&2
      die "$name: the framework(s) above are not Qt 6.9.x, and Basecamp 0.2.2 bundles 6.9.2 — a plugin over that ceiling loads 'successfully' and answers nothing"; }
  fi
  echo "  ok    $plugin"
  echo "        Qt through @rpath, 6.9.x, from $qtdir"
}

package_module() { # name dir
  local name="$1" dir="$2" build="$BUILD/$1"
  local stage; stage="$(mktemp -d)"
  mkdir -p "$stage/$VARIANT"
  cp "$build/modules/${name}_plugin.$SO" "$stage/$VARIANT/"
  for sib in "$build"/modules/*."$SO"; do
    [ -f "$sib" ] || continue
    [ "$(basename "$sib")" = "${name}_plugin.$SO" ] && continue
    cp "$sib" "$stage/$VARIANT/"
  done
  cp "$dir/metadata.json" "$stage/$VARIANT/"
  rm -f "$PKG/$name.lgx"
  ( cd "$PKG" && "$LGX_BIN" create "$name" >/dev/null ) || die "$name: lgx create failed"
  "$LGX_BIN" add "$PKG/$name.lgx" --variant "$VARIANT" --files "$stage/$VARIANT" \
             --main "${name}_plugin.$SO" -y >/dev/null || die "$name: lgx add failed"
  # `lgx add` never reads metadata.json, so author/description/type/category are
  # empty in the manifest it writes. `type` is the one that matters: Logos Core
  # installs a `core` module into its modules directory and a `ui` one into its
  # plugins directory, and an unset type lands in neither. Same patch
  # module/package-basecamp.sh applies, and the same one nix-bundle-lgx's
  # bundle.sh does.
  python3 - "$PKG/$name.lgx" "$dir/metadata.json" <<'PY' || die "manifest patch failed"
import io, json, os, sys, tarfile
pkg, metadata_path = sys.argv[1], sys.argv[2]
meta = json.load(open(metadata_path))
with tarfile.open(pkg, "r:gz") as tar:
    members = [(m, tar.extractfile(m).read() if m.isfile() else None)
               for m in tar.getmembers()]
tmp = pkg + ".tmp"
with tarfile.open(tmp, "w:gz") as tar:
    for member, data in members:
        if member.name == "manifest.json":
            manifest = json.loads(data)
            for key in ("author", "description", "type", "category",
                        "version", "dependencies"):
                if key in meta:
                    manifest[key] = meta[key]
            data = json.dumps(manifest, indent=2, sort_keys=True).encode()
            member.size = len(data)
        tar.addfile(member, io.BytesIO(data) if data is not None else None)
os.replace(tmp, pkg)
PY
  rm -rf "$stage"
  echo "  $PKG/$name.lgx  ($(du -h "$PKG/$name.lgx" | cut -f1))"
}

# ── 3. storage_module ─────────────────────────────────────────────────────
say "[3/6] logos-storage-module @ ${STORAGE_MODULE_REV:0:7}"
fetch https://github.com/logos-co/logos-storage-module "$STORAGE_MODULE_REV"; SM="$FETCHED"
mkdir -p "$SM/lib"
cp -f "$STORAGE_LIB_SRC/build/libstorage.$SO" "$SM/lib/" || die "no built libstorage"
cp -f "$STORAGE_LIB_SRC/library/libstorage.h" "$SM/lib/" || die "no libstorage.h"
if [ "$(uname -s)" = Darwin ]; then
  install_name_tool -id "@rpath/libstorage.$SO" "$SM/lib/libstorage.$SO"
fi
build_module storage_module "$SM" storage_module_plugin.h StorageModuleImpl
package_module storage_module "$SM"

# ── 4. delivery_module (the messaging module) ─────────────────────────────
say "[4/6] logos-delivery-module @ ${DELIVERY_MODULE_REV:0:7}"
fetch https://github.com/logos-co/logos-delivery-module "$DELIVERY_MODULE_REV"; DM="$FETCHED"
mkdir -p "$DM/lib/generated"
cp -f "$DELIVERY_DYLIB" "$DM/lib/" || die "no built liblogosdelivery"
cp -f "$DELIVERY_LIB_SRC/library/liblogosdelivery.h" \
      "$DELIVERY_LIB_SRC/library/liblogosdelivery_kernel.h" "$DM/lib/" \
  || die "no liblogosdelivery headers"
# `library/generated/logosdelivery.h` is a BUILD ARTEFACT, not a tracked file,
# and at the revision this script pins — `f8b0365`, which is the module's own
# `flake.lock` pin — `make liblogosdelivery` does not write one. Checked twice:
# `git cat-file -e f8b0365:library/generated/logosdelivery.h` says the path
# "exists on disk, but not in" that commit in a checkout that has it, and a
# clean clone at f8b0365 built here has no `library/generated` at all.
#
# This step used to `die` on that, which made the script unable to complete on a
# machine whose delivery checkout is at the revision the script itself pins. It
# is not fatal, and the reason is checkable rather than argued: the module
# includes `<liblogosdelivery.h>` and nothing else — `src/api_call_handler.h:14`,
# `src/delivery_module_plugin.cpp:18` — and its `metadata.json` adds `lib` to the
# include path, not `lib/generated`. So the header is staged when the library's
# build produced one, and its absence is reported and carried on from. The
# assertion that it was not needed is downstream and is the strong one: the
# module compiles, packages, loads into `liblogos_core`, and answers
# `getPluginMethods` across the transport.
if [ -f "$DELIVERY_LIB_SRC/library/generated/logosdelivery.h" ]; then
  cp -f "$DELIVERY_LIB_SRC/library/generated/logosdelivery.h" "$DM/lib/generated/"
else
  echo "  note: this liblogosdelivery build wrote no library/generated/logosdelivery.h."
  echo "        The module includes <liblogosdelivery.h> only, so nothing here needs it;"
  echo "        if the build below fails on a missing header, that is where to look."
fi
if [ "$(uname -s)" = Darwin ]; then
  install_name_tool -id "@rpath/liblogosdelivery.$SO" "$DM/lib/liblogosdelivery.$SO"
fi
build_module delivery_module "$DM" delivery_module_plugin.h DeliveryModuleImpl
package_module delivery_module "$DM"

# ── 5. wallet_module ──────────────────────────────────────────────────────
say "[5/6] logos-wallet-module @ ${WALLET_MODULE_REV:0:7}"
fetch https://github.com/logos-co/logos-wallet-module "$WALLET_MODULE_REV"; WM="$FETCHED"
mkdir -p "$WM/lib"
cp -f "$GOSDK/build/libgowalletsdk.a" "$GOSDK/build/libgowalletsdk.h" "$WM/lib/" \
  || die "no go-wallet-sdk c-archive"
build_module wallet_module "$WM" wallet_module_impl.h WalletModuleImpl \
      -DLOGOS_MODULE_GO_STATIC_LIBS=gowalletsdk
package_module wallet_module "$WM"

# ── 6. and that not one of them was edited to make any of that work ───────
#
# The criterion's own words are "without requiring modifications to those
# modules", so this is the half that has to be checked rather than asserted.
# Same mechanism as examples/agent-console/run.sh's `git status --porcelain
# module/`: a tracked file that changed fails the script.
#
# WHAT THIS BLOCK USED TO BE, AND WHY IT IS NOT THAT ANY MORE
#
# It read `git status --porcelain` into a variable and passed when the variable
# was empty. `git status` writes its diagnostics to stderr and its exit code
# went nowhere, so a directory that is not a git checkout — or one that is not
# there at all — produced an empty string and printed
#
#   ok    storage_module @ : no tracked file changed; 0 added path(s), all
#         under lib/ or generated_code/
#
# Measured, both cases. That is the criterion's own half ("without requiring
# modifications to those modules") reporting a pass having examined nothing, in
# the shape this repository has now removed twice: a claim whose pass condition
# is the absence of a bad string is satisfied by nothing having run.
#
# So the revision is resolved FIRST and has to be the one this script pinned,
# and the added-path count has to be non-zero: a build that staged no external
# library and generated no glue is a build that did not happen, and its clean
# `git status` is a fact about an empty directory.
say "[6/6] the three checkouts, unmodified"
dirty=0
for pair in "storage_module:$SM:$STORAGE_MODULE_REV" \
            "delivery_module:$DM:$DELIVERY_MODULE_REV" \
            "wallet_module:$WM:$WALLET_MODULE_REV"; do
  name="${pair%%:*}"; rest="${pair#*:}"; dir="${rest%%:*}"; want="${rest#*:}"
  head="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
  if [ -z "$head" ]; then
    echo "  FAIL  $name: $dir is not a git checkout, so 'unmodified' cannot be read off it" >&2
    dirty=1
    continue
  fi
  if [ "$head" != "$want" ]; then
    echo "  FAIL  $name: $dir is at $head, not the pinned $want" >&2
    dirty=1
    continue
  fi
  tracked="$(git -C "$dir" status --porcelain --untracked-files=no)"
  if [ -n "$tracked" ]; then
    echo "  FAIL  $name: tracked files changed in $dir" >&2
    printf '%s\n' "$tracked" | sed 's/^/        /' >&2
    dirty=1
    continue
  fi
  # And what the build ADDED, which has to be confined to the two directories
  # the module's own published build writes.
  stray="$(git -C "$dir" status --porcelain --untracked-files=all \
           | awk '{print $2}' | grep -Ev '^(lib/|generated_code/)' || true)"
  if [ -n "$stray" ]; then
    echo "  FAIL  $name: the build left files outside lib/ and generated_code/:" >&2
    printf '%s\n' "$stray" | sed 's/^/        /' >&2
    dirty=1
    continue
  fi
  added="$(git -C "$dir" status --porcelain --untracked-files=all | wc -l | tr -d ' ')"
  if [ "$added" -lt 1 ]; then
    echo "  FAIL  $name: the build added nothing to $dir — no external library under" >&2
    echo "        lib/, no generated glue under generated_code/. A clean tree here" >&2
    echo "        means the build did not run, not that it ran without touching" >&2
    echo "        anything." >&2
    dirty=1
    continue
  fi
  echo "  ok    $name @ ${head:0:7} (the pinned revision): no tracked file changed;" \
       "$added added path(s), all under lib/ or generated_code/"
done
[ "$dirty" -eq 0 ] || die "a companion module was modified — that is the criterion, not a detail"

say "packaged"
ls -1 "$PKG"/*.lgx | sed 's/^/  /'
cat <<TXT

Now run them together with the agent module in one Logos Core:

  ./scripts/logos-core-headless.sh storage --alongside
TXT
