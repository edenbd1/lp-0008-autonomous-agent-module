#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Put a Logos Core runtime on a Linux machine that has no Logos app installed.
#
#   ./scripts/fetch-logos-core.sh [destination]
#
# WHY THIS EXISTS
#
# `docs/limitations.md` said for months that the runtime half of "on any
# machine" could not be closed because "`liblogos_core` ships inside the app and
# there is no headless distribution of it to fetch". The first half of that is
# true on every platform. The second half was true of the macOS `.dmg` and was
# never checked against the Linux build, and the Linux build is an **AppImage**
# — a self-contained SquashFS image, published on the same release page, that
# unpacks with one command and needs no installer, no root, no FUSE and no
# desktop session. `liblogos_core.so`, `logos_host`, the embedded modules and
# Basecamp's own Qt 6.9.2 are all inside it.
#
# So this fetches it the way `.github/workflows/ci.yml`'s `storage-node` job
# fetches `libstorage`: a pinned upstream artefact, checked against a recorded
# sha256 before anything is unpacked. A release asset is mutable and "we
# downloaded something" is not the same claim as "we downloaded this".
#
# WHAT IT DOES NOT GIVE YOU
#
# Qt headers and `moc`. The AppImage carries Qt's *libraries* and not its SDK,
# and `scripts/logos-core-headless.sh` compiles a harness. Qt 6.9.2 from
# aqtinstall is still a prerequisite — see docs/limitations.md, which lists all
# of them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Every other script here guards this line, and this one did not, because the
# guard was written for `cd "$ROOT"` and this script never cds. The hazard is the
# same and the consequence is worse: `$ROOT` is interpolated into a path, so a
# failed subshell leaves it empty and DEST becomes `/_external/logos-core` — the
# filesystem root — which is where 278 MB would then be unpacked.
[ -n "$ROOT" ] || { echo "cannot resolve the repository root from $0" >&2; exit 1; }

DEST="${1:-$ROOT/_external/logos-core}"
VERSION="${BASECAMP_VERSION:-0.2.2}"
BUILD="${BASECAMP_BUILD:-d41a72}"

# Pinned. Both were downloaded and checked here, and both routes below were
# exercised: `--appimage-extract` natively on aarch64, `unsquashfs` on x86_64
# under container emulation. The sha256 GitHub's own asset `digest` field
# reports for each is the same value.
case "$(uname -m)" in
    x86_64|amd64)   ARCH=x86_64  ; SHA256=b5dd636f966411d0c2de72a739bdf2c832ec95f20315388630b294907f7dd5ae ;;
    aarch64|arm64)  ARCH=aarch64 ; SHA256=9fed46b9e06f21d0ca6f9bfc14747494ad27fb66377cef4e61a94f612f287ef2 ;;
    *) echo "no Logos Basecamp AppImage is published for $(uname -m)" >&2; exit 1 ;;
esac
ASSET="LogosBasecamp-Desktop-v${VERSION}-${BUILD}-${ARCH}.AppImage"
URL="https://github.com/logos-co/logos-basecamp/releases/download/${VERSION}/${ASSET}"

if [ "$(uname -s)" != "Linux" ]; then
    echo "This fetches the LINUX runtime. On macOS, install LogosBasecamp.app" >&2
    echo "from the .dmg on the same release page — there is no AppImage to" >&2
    echo "unpack there, and scripts/logos-core-headless.sh reads the app." >&2
    exit 1
fi

mkdir -p "$DEST"
IMG="$DEST/$ASSET"

if [ -f "$IMG" ]; then
    echo "have      $IMG"
else
    echo "fetch     $URL"
    curl -fL --progress-bar -o "$IMG.part" "$URL"
    mv "$IMG.part" "$IMG"
fi

# Before anything is unpacked. A checksum after the extraction would be a
# checksum of a decision already taken.
echo "${SHA256}  ${IMG}" | sha256sum -c - >/dev/null \
  || { echo "  FAIL  $ASSET does not match the pinned sha256 ${SHA256}." >&2
       echo "        Refusing to unpack it. Delete it and re-run to retry the" >&2
       echo "        download; if it keeps failing, upstream replaced the asset" >&2
       echo "        and the pin in this script has to be re-established" >&2
       echo "        deliberately rather than followed." >&2
       exit 1; }
echo "  ok       sha256 ${SHA256}"

TREE="$DEST/squashfs-root"
if [ -d "$TREE" ] && [ -f "$TREE/usr/lib/liblogos_core.so" ]; then
    echo "have      $TREE (already unpacked)"
else
    rm -rf "$TREE"
    chmod +x "$IMG"
    # Route 1: the AppImage's own extractor. No tools, no root, no FUSE — the
    # runtime unpacks itself. It is a static x86_64/aarch64 ELF, so it only
    # works when the host really is that architecture, which is the normal case
    # and is not the case under some container emulators.
    if ( cd "$DEST" && "$IMG" --appimage-extract >/dev/null 2>&1 ) \
         && [ -f "$TREE/usr/lib/liblogos_core.so" ]; then
        echo "unpack    --appimage-extract"
    else
        # Route 2: read the SquashFS out of it directly. An AppImage is an ELF
        # runtime with the filesystem appended, and the offset is where the ELF
        # ends: e_shoff + e_shnum * e_shentsize. Do NOT search for the `hsqs`
        # magic — the runtime binary contains those four bytes too, 750 KB
        # before the real superblock, and unsquashfs then reports "Can't find a
        # valid SQUASHFS superblock", which reads like a corrupt download.
        command -v unsquashfs >/dev/null 2>&1 || {
            echo "  FAIL  $ASSET would not run itself (that is normal when the" >&2
            echo "        host architecture is emulated) and unsquashfs is not" >&2
            echo "        installed to read it instead: apt install squashfs-tools" >&2
            exit 1; }
        OFFSET="$(python3 -c '
import struct, sys
with open(sys.argv[1], "rb") as fh:
    head = fh.read(64)
    if head[:4] != b"\x7fELF":
        sys.exit("not an ELF: this is not an AppImage")
    shoff = struct.unpack_from("<Q", head, 0x28)[0]
    entsize = struct.unpack_from("<H", head, 0x3A)[0]
    num = struct.unpack_from("<H", head, 0x3C)[0]
    end = shoff + entsize * num
    fh.seek(end)
    if fh.read(4) != b"hsqs":
        sys.exit("no SquashFS superblock where the ELF ends (offset %d)" % end)
    print(end)' "$IMG")"
        echo "unpack    unsquashfs -o $OFFSET"
        unsquashfs -o "$OFFSET" -d "$TREE" "$IMG" >/dev/null
    fi
    [ -f "$TREE/usr/lib/liblogos_core.so" ] \
      || { echo "  FAIL  unpacked, and $TREE/usr/lib/liblogos_core.so is not there" >&2; exit 1; }
fi

# Every path scripts/logos-core-headless.sh will look for, asserted here rather
# than discovered later inside a compile.
missing=0
for p in usr/lib/liblogos_core.so usr/bin/logos_host usr/modules usr/lib/qt/plugins; do
    if [ -e "$TREE/$p" ]; then
        echo "  ok       $p"
    else
        echo "  FAIL     $p is not in the unpacked tree" >&2
        missing=$((missing + 1))
    fi
done
[ "$missing" -eq 0 ] || exit 1

cat <<EOF

Logos Core ${VERSION} (${ARCH}) is unpacked at
  $TREE

scripts/logos-core-headless.sh finds it there with no environment set. To point
it somewhere else:

  export LOGOS_APP=$TREE
EOF
