#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Deploy the agent in Logos Core headless ON A MACHINE WITH NO BUILD TOOLCHAIN.
#
#   ./scripts/harness-no-toolchain.sh [linux-amd64|linux-arm64]
#
# WHAT THIS IS FOR
#
# The deployment criterion says "on any machine using Logos Core headless".
# `scripts/logos-core-headless.sh` used to COMPILE the harness that drives the
# runtime, so running it needed a C++17 compiler, Qt 6.9.2 with qtremoteobjects,
# a logos-cpp-sdk checkout and nlohmann/json — on every platform. A machine that
# could run Logos Core still could not run the command, and that is not "any
# machine". The harness is now built once per variant and committed under
# `module/harness/`.
#
# A claim like that cannot be checked on the machine that made the binaries.
# This runs the whole thing inside a stock `ubuntu:24.04` container that has no
# compiler, no Qt SDK, no logos-cpp-sdk checkout and no nlohmann/json — and
# REFUSES TO REPORT ANYTHING if it finds any of them, because a proof performed
# on a prepared machine is not a proof. What it installs is `python3` (the
# script decodes an account id and reads a manifest with it) and, only when no
# AppImage has been cached for it, `curl` and `ca-certificates` to fetch the
# published runtime. None of those is a build toolchain and each is named in the
# transcript.
#
# Then it asserts the three things that make the green mean something:
#
#   1. the run gets to `all steps confirmed (0 failure(s))` out of the SHIPPED
#      harness — it says which one it used and its sha256;
#   2. `HARNESS_FROM_SOURCE=1` on the same machine is refused, by name, for the
#      four things it would need — so "no toolchain" is a fact about the
#      container and not an assumption of this script;
#   3. a shipped harness whose bytes do not match the record is not run.
#
# and it re-runs `scripts/check-package-fresh.py` there, which needs nothing but
# python3, so the binary is checked against its source on the same machine that
# is about to trust it.
#
# WHERE THE RUNTIME COMES FROM
#
# `scripts/fetch-logos-core.sh`, inside the container: the published AppImage,
# checksum-pinned. Set LOGOS_APPIMAGE_CACHE to a directory holding an already
# downloaded `LogosBasecamp-Desktop-*.AppImage` to skip the ~278 MB download;
# the checksum is verified either way, in the container, before anything is
# unpacked.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VARIANT="${1:-}"
if [ -z "$VARIANT" ]; then
  case "$(uname -m)" in
    x86_64|amd64)  VARIANT=linux-amd64 ;;
    aarch64|arm64) VARIANT=linux-arm64 ;;
    *) echo "no Logos Basecamp AppImage is published for $(uname -m)" >&2; exit 1 ;;
  esac
fi
case "$VARIANT" in
  linux-amd64) PLATFORM=linux/amd64; ASSET_ARCH=x86_64 ;;
  linux-arm64) PLATFORM=linux/arm64; ASSET_ARCH=aarch64 ;;
  *) echo "this checks the Linux variants; '$VARIANT' is not one of them" >&2
     echo "(macOS has no toolchain-free container to run in: the runtime is" >&2
     echo " inside an installed .app. The same claim is checked there by" >&2
     echo " running scripts/logos-core-headless.sh on a machine whose Qt SDK" >&2
     echo " and logos-cpp-sdk checkout have been moved aside.)" >&2
     exit 1 ;;
esac

IMAGE="${TOOLCHAIN_FREE_IMAGE:-ubuntu:24.04}"
CACHE="${LOGOS_APPIMAGE_CACHE:-}"
command -v docker >/dev/null 2>&1 || { echo "this needs docker" >&2; exit 1; }

echo "image     $IMAGE ($PLATFORM), stock, nothing added but python3"
echo "variant   $VARIANT"
echo "repo      $ROOT (mounted read-only; the container works on a copy)"
[ -n "$CACHE" ] && echo "appimage  $CACHE (cached; the checksum is still checked in the container)"
echo

mounts=(-v "$ROOT:/src:ro")
[ -n "$CACHE" ] && mounts+=(-v "$CACHE:/appimages:ro")

# `-i`, and the marker at the bottom, because of the way this script first went
# green: without `-i` docker does not attach stdin, `bash -s` reads an empty
# program, and the container exits 0 having done nothing at all. The exit code
# alone reported a toolchain-free deployment that never happened. So the exit
# code is not the assertion — the last line the container prints is.
out=$(docker run --rm -i --platform "$PLATFORM" "${mounts[@]}" \
  -e ASSET_ARCH="$ASSET_ARCH" -e VARIANT="$VARIANT" "$IMAGE" bash -s <<'INSIDE'
set -uo pipefail
fail() { echo "  FAIL  $*" >&2; exit 1; }

echo "=== 1. what this machine does NOT have ============================="
# The whole run is worthless if any of these is present, so this is a refusal
# and not a note.
for c in cc c++ gcc g++ clang clang++ make cmake ninja moc qmake ld as; do
  if command -v "$c" >/dev/null 2>&1; then
    fail "$c is on PATH: this container is not toolchain-free, so nothing it
        does would prove anything. Use a stock image."
  fi
  printf '  ok    no %s\n' "$c"
done
for f in $(find / -xdev -name 'Qt6Config.cmake' -o -xdev -name 'json.hpp' \
                 -o -xdev -name 'logos_api.cpp' 2>/dev/null); do
  fail "$f exists: a Qt SDK, nlohmann/json or a logos-cpp-sdk checkout is here."
done
echo "  ok    no Qt SDK, no nlohmann/json, no logos-cpp-sdk checkout"
echo "  <-    $(cat /etc/os-release | sed -n 's/^PRETTY_NAME=//p' | tr -d '\"'), $(uname -m)"
echo

echo "=== 2. what it installs, and it is not a compiler =================="
export DEBIAN_FRONTEND=noninteractive
need_curl=0
[ -f /appimages/LogosBasecamp-Desktop-v0.2.2-d41a72-${ASSET_ARCH}.AppImage ] || need_curl=1
apt-get update -qq >/dev/null 2>&1 \
  || fail "apt-get update failed; this container has no network"
pkgs="python3"
[ "$need_curl" -eq 1 ] && pkgs="$pkgs curl ca-certificates"
apt-get install -y -qq --no-install-recommends $pkgs >/dev/null 2>&1 \
  || fail "could not install $pkgs"
echo "  ok    installed: $pkgs"
python3 -c 'import sys; print("  <-    python3", sys.version.split()[0])'
# And they did not drag a compiler in with them.
for c in cc c++ gcc make; do
  command -v "$c" >/dev/null 2>&1 && fail "$c appeared during apt-get install"
done
echo "  ok    still no compiler after the install"
echo

echo "=== 3. the repository, as a fresh clone would have it =============="
cp -a /src /work || fail "could not copy the repository"
cd /work || fail "no /work"
rm -rf build-headless _external/logos-core
echo "  ok    /work is a copy of the repository with no build tree in it"
ls -l "module/harness/$VARIANT/logos_core_load_test" \
  || fail "there is no committed harness for $VARIANT"
echo

echo "=== 4. the Logos Core runtime, as upstream publishes it ============"
mkdir -p _external/logos-core
if [ -f "/appimages/LogosBasecamp-Desktop-v0.2.2-d41a72-${ASSET_ARCH}.AppImage" ]; then
  cp "/appimages/LogosBasecamp-Desktop-v0.2.2-d41a72-${ASSET_ARCH}.AppImage" \
     _external/logos-core/ || fail "could not stage the cached AppImage"
  echo "  <-    using a cached download; the pinned sha256 is still checked below"
fi
if ! ./scripts/fetch-logos-core.sh > /tmp/fetch.out 2>&1; then
  cat /tmp/fetch.out
  # One retry, for one specific reason, and it is a property of THIS host and
  # not of the claim: an AppImage extracts itself by RUNNING, and a linux/amd64
  # container on an arm64 machine cannot run an x86-64 ELF. On a real x86-64
  # machine this branch is never taken. `squashfs-tools` reads the image
  # instead; it is a decompressor, not a compiler, and the check above still
  # holds afterwards.
  grep -q "would not run itself" /tmp/fetch.out \
    || fail "the runtime did not unpack, and not because the AppImage could not
        run itself. That is the failure to read, not this one."
  echo "  <-    this host is emulating $(uname -m); the AppImage cannot execute"
  echo "        itself here, so squashfs-tools is installed to read the image"
  echo "        instead. On a real $(uname -m) machine this step does not happen."
  apt-get install -y -qq --no-install-recommends squashfs-tools >/dev/null 2>&1 \
    || fail "could not install squashfs-tools"
  for c in cc c++ gcc make; do
    command -v "$c" >/dev/null 2>&1 && fail "$c appeared with squashfs-tools"
  done
  echo "  ok    squashfs-tools installed, and there is still no compiler"
  ./scripts/fetch-logos-core.sh || fail "the runtime did not unpack"
else
  cat /tmp/fetch.out
fi
echo

echo "=== 5. THE COMMAND ================================================="
./scripts/logos-core-headless.sh storage > /tmp/run.out 2>&1; rc=$?
sed -n '1,20p' /tmp/run.out
echo "  <-    ... $(grep -c '^  ok  ' /tmp/run.out) assertions"
tail -8 /tmp/run.out
echo "  <-    exit=$rc"
[ "$rc" -eq 0 ] || { sed -n '1,120p' /tmp/run.out; fail "the headless run exited $rc"; }
grep -q "all steps confirmed (0 failure(s))" /tmp/run.out \
  || fail "no confirmation banner"
grep -q "shipped, sha256" /tmp/run.out \
  || fail "it did not use the shipped harness"
grep -q "no compiler, no Qt SDK and no logos-cpp-sdk checkout were needed" /tmp/run.out \
  || fail "it did not say which harness it ran"
grep -q "variant $VARIANT" /tmp/run.out \
  || fail "it did not install the $VARIANT variant"
grep -q "the loaded module lists all 28 documented skills" /tmp/run.out \
  || fail "the module loaded and never listed its skills"
grep -q "and to the policy account it was configured with" /tmp/run.out \
  || fail "it never read the envelope back out of meta.status"
echo "  ok    Logos Core loaded, configured and started the module, out of the"
echo "        committed package and the committed harness, on a machine that"
echo "        cannot compile anything"
echo

echo "=== 6. control: the source path is genuinely unavailable here ======"
# Without this, "no toolchain was needed" is a sentence rather than a fact:
# the same command asked to BUILD the harness must fail on this machine, and
# must name every piece it lacks rather than dying inside a compile.
HARNESS_FROM_SOURCE=1 ./scripts/logos-core-headless.sh storage \
  > /tmp/src.out 2>&1; rc=$?
cat /tmp/src.out
[ "$rc" -ne 0 ] || fail "building the harness SUCCEEDED here; this container has
        a toolchain after all and step 5 proves nothing"
grep -q "a C++17 compiler" /tmp/src.out || fail "it did not name the compiler"
grep -q "Qt 6.9.2" /tmp/src.out         || fail "it did not name Qt"
grep -q "logos-cpp-sdk checkout" /tmp/src.out || fail "it did not name the SDK"
echo "  ok    refused, naming all three, before touching anything"
echo

echo "=== 7. control: a harness that is not the recorded one is not run =="
printf 'x' | dd of="module/harness/$VARIANT/logos_core_load_test" bs=1 seek=262144 \
     conv=notrunc status=none || fail "could not corrupt the harness"
./scripts/logos-core-headless.sh storage > /tmp/bad.out 2>&1; rc=$?
tail -12 /tmp/bad.out
[ "$rc" -ne 0 ] || fail "a modified harness was run anyway"
grep -q "not the binary the record was written for" /tmp/bad.out \
  || fail "it went red for some other reason"
grep -q "^  ok    logos_core_load_module" /tmp/bad.out \
  && fail "it ran the harness before checking it"
echo "  ok    refused before the runtime was started"
echo

echo "=== 8. and the binary is checkable against its source, here ========"
cp -f "/src/module/harness/$VARIANT/logos_core_load_test" \
      "module/harness/$VARIANT/logos_core_load_test" || fail "could not restore it"
./scripts/check-package-fresh.py > /tmp/fresh.out 2>&1; rc=$?
grep -E "^OK|harness" /tmp/fresh.out | tail -20
echo "  <-    exit=$rc"
[ "$rc" -eq 0 ] || { cat /tmp/fresh.out; fail "check-package-fresh.py exited $rc"; }
grep -q "^OK the shipped harness was built from the source" /tmp/fresh.out \
  || fail "it did not report the harness as checked"
echo
echo "ALL OF IT ON A MACHINE WITH NO COMPILER, NO QT SDK, NO logos-cpp-sdk"
echo "CHECKOUT AND NO nlohmann/json."
echo "TOOLCHAIN-FREE-RUN-COMPLETE"
INSIDE
) 2>&1
rc=$?
printf '%s\n' "$out"
echo
if [ $rc -ne 0 ]; then
  echo "the toolchain-free run failed (exit $rc)" >&2
  exit $rc
fi
printf '%s\n' "$out" | grep -qx "TOOLCHAIN-FREE-RUN-COMPLETE" || {
  echo "the container exited 0 without running the checks to the end — that is
how this script first passed: no -i, so bash -s read an empty program." >&2
  exit 1
}
echo "toolchain-free deployment confirmed for $VARIANT in a stock $IMAGE"
