#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Package the built agent module as an .lgx that Logos Basecamp can install.
#
#   module/package-basecamp.sh <build-dir> [variant] [out.lgx]
#
# `build-dir` is the CMake build directory produced by docs/basecamp.md's build
# step; the plugin is at <build-dir>/modules/agent_plugin.<dylib|so>.
#
# Packaging is done by `lgx` from logos-co/logos-package — the tool Basecamp's
# own packages are built with — found via $LGX_BIN, then
# ~/logos/src/logos-package/build/lgx, then $PATH. This script does not
# reimplement the package format: getting the manifest hashes subtly wrong
# produces a package that installs and then fails to load, which is exactly the
# failure this repository refuses to ship.
#
# `lgx add` never reads metadata.json, so author/description/type/category are
# empty in the manifest it writes. They are patched in afterwards — the same
# thing nix-bundle-lgx's bundle.sh does. `type` matters: Basecamp installs a
# `core` module into its modules directory and a `ui` one into its plugins
# directory, and an unset type lands in neither.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:?usage: package-basecamp.sh <build-dir> [variant] [out.lgx]}"
variant="${2:-}"
out="${3:-$here/agent.lgx}"

if [ -z "$variant" ]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)  variant=darwin-arm64 ;;
        Darwin-x86_64) variant=darwin-amd64 ;;
        Linux-x86_64)  variant=linux-amd64 ;;
        Linux-aarch64) variant=linux-arm64 ;;
        *) echo "unknown host; pass the variant explicitly" >&2; exit 1 ;;
    esac
fi

lgx="${LGX_BIN:-}"
for candidate in "$lgx" "$HOME/logos/src/logos-package/build/lgx" "$(command -v lgx || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then lgx="$candidate"; break; fi
done
if [ -z "$lgx" ] || [ ! -x "$lgx" ]; then
    echo "lgx not found. Build it from https://github.com/logos-co/logos-package" >&2
    echo "and set LGX_BIN, or put it on PATH." >&2
    exit 1
fi

plugin=""
for ext in dylib so dll; do
    if [ -f "$build/modules/agent_plugin.$ext" ]; then
        plugin="$build/modules/agent_plugin.$ext"
        break
    fi
done
if [ -z "$plugin" ]; then
    echo "no agent_plugin.{dylib,so,dll} under $build/modules — build it first," >&2
    echo "see docs/basecamp.md" >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$variant"
cp "$plugin" "$stage/$variant/"
# The module reads nothing from metadata.json at runtime — the copy is what the
# host reads to learn the module's name, type and dependencies before it opens
# the binary at all.
cp "$here/metadata.json" "$stage/$variant/"

rm -f "$out"
( cd "$(dirname "$out")" && "$lgx" create "$(basename "${out%.lgx}")" >/dev/null )
"$lgx" add "$out" --variant "$variant" --files "$stage/$variant" \
    --main "$(basename "$plugin")" -y >/dev/null

python3 - "$out" "$here/metadata.json" <<'PY'
import io, json, sys, tarfile

pkg, metadata_path = sys.argv[1], sys.argv[2]
meta = json.load(open(metadata_path))

with tarfile.open(pkg, "r:gz") as tar:
    members = [(m, tar.extractfile(m).read() if m.isfile() else None)
               for m in tar.getmembers()]

patched = []
for member, data in members:
    if member.name == "manifest.json":
        manifest = json.loads(data)
        for key in ("author", "description", "type", "category",
                    "version", "dependencies"):
            if key in meta:
                manifest[key] = meta[key]
        data = json.dumps(manifest, indent=2, sort_keys=True).encode()
        member.size = len(data)
    patched.append((member, data))

with tarfile.open(pkg, "w:gz", format=tarfile.GNU_FORMAT) as tar:
    for member, data in patched:
        if data is None:
            tar.addfile(member)
        else:
            tar.addfile(member, io.BytesIO(data))
PY

"$lgx" verify "$out"
"$lgx" manifest "$out"
echo
echo "sha256  $(shasum -a 256 "$out" | cut -d' ' -f1)"
