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

# The third way a package is wrong, and the only one of the three that is
# invisible even to a reviewer who unpacks it: the binary loads perfectly and is
# built from source nobody has read. This repository shipped that too — an
# `agent.lgx` packaged at f53f822 stayed committed across five commits to
# module/src, so the downloadable artefact was missing content-topic identifier
# validation, the owner-channel hardening, `program.call`'s flag-value checking
# and task persistence, while README §7 offered it as the loadable asset. Every
# check in this script passed on it, because every one of them was true.
#
# Nothing here can tell what source a binary was built from. What it can tell is
# whether the build directory it is being handed is older than the source, which
# is how the stale package got made: package a build tree nobody rebuilt. mtime
# is a weak signal in general and an exact one for this — CMake relinks on any
# source it is newer than, so a source newer than the plugin means the plugin
# does not contain it.
newer="$(find "$here/src" "$here/generated_code" "$here/metadata.json" \
              "$here/agent_module_plugin_export.h" \
              -newer "$plugin" -type f 2>/dev/null || true)"
if [ -n "$newer" ]; then
    echo "  FAIL  the plugin in $build is older than the source it is built" >&2
    echo "        from, so this package would ship code nobody wrote:" >&2
    printf '%s\n' "$newer" | sed "s|^$here/|          module/|" >&2
    echo "        rebuild first:  cmake --build $build" >&2
    exit 1
fi
echo "  ok    the plugin is newer than every source it is built from"

# The other way a package loads nowhere: built against a Qt the host does not
# have. Basecamp's bundled Qt is a ceiling, not a floor, and a Homebrew build
# additionally hardcodes /opt/homebrew/... as its library paths, which resolve
# on this machine and on no other. An official Qt references @rpath.
if [ "$(uname -s)" = "Darwin" ] && command -v otool >/dev/null 2>&1; then
    qtrefs="$(otool -L "$plugin" | awk '/Qt[A-Za-z]*\.framework/ {print $1, $NF}')"
    if [ -z "$qtrefs" ]; then
        echo "  FAIL  the plugin references no Qt frameworks at all" >&2
        exit 1
    fi
    if printf '%s\n' "$qtrefs" | grep -qv '^@rpath/'; then
        echo "  FAIL  a Qt framework is referenced by absolute path, so it" >&2
        echo "        resolves only on this machine — see docs/basecamp.md:" >&2
        printf '%s\n' "$qtrefs" | grep -v '^@rpath/' >&2
        exit 1
    fi
    echo "  ok    Qt is referenced through @rpath, version(s):" \
         "$(printf '%s\n' "$qtrefs" | tr -d ')' | awk '{print $NF}' | sort -u | tr '\n' ' ')"
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

# `lgx verify` checks the contents against the manifest's hashes. It does not
# check that `main` names one of them — a manifest can be internally consistent
# and still point at a file the package does not contain, which is an invisible
# load failure: the host resolves `main` inside the module directory, finds
# nothing, and logs nothing. That defect shipped here once already, with `main`
# naming `agent_module_plugin` while the builder emits `agent_plugin`. So it is
# asserted at the one moment it can still be fixed cheaply.
python3 - "$out" "$here/metadata.json" <<'PY'
import json, posixpath, sys, tarfile

pkg, metadata_path = sys.argv[1], sys.argv[2]
meta = json.load(open(metadata_path))

with tarfile.open(pkg, "r:gz") as tar:
    names = set(tar.getnames())
    manifest = json.loads(tar.extractfile("manifest.json").read())

mains = manifest.get("main") or {}
if not mains:
    sys.exit("manifest declares no `main` — the host would have nothing to load")

failures = []
for variant, main in sorted(mains.items()):
    if not main:
        failures.append("variant %s declares an empty `main`" % variant)
        continue
    path = posixpath.join("variants", variant, main)
    if path not in names:
        failures.append(
            "manifest `main` for %s is %r, which is not in the package "
            "(it holds: %s)" % (
                variant, main,
                ", ".join(sorted(
                    posixpath.basename(n) for n in names
                    if n.startswith("variants/%s/" % variant) and
                    posixpath.basename(n))) or "nothing"))
        continue
    # metadata.json carries the name without an extension; the manifest carries
    # it with one. Anything else means the two disagree about what to load.
    declared = meta.get("main", "")
    if declared and main != declared and not main.startswith(declared + "."):
        failures.append(
            "metadata.json's `main` (%r) does not agree with the manifest's "
            "(%r) for %s" % (declared, main, variant))
    print("  ok    main[%s] = %s is in the package" % (variant, main))

if failures:
    sys.exit("\n".join("  FAIL  " + f for f in failures))
PY

# The record of what this package was built from.
#
# The mtime check at the top of this script catches a build tree older than the
# source. It cannot catch the other half of the same defect, which is the one
# that has now shipped twice: a package that was correct when it was made and
# was left behind by later commits to module/src. Nothing in the package says
# what source it came from, so the answer is written down beside it, at the one
# moment it is known for certain — here — and checked on every CI run by
# scripts/check-package-fresh.py.
#
# Only for the canonical artefact. Packaging to some other path is a throwaway
# and must not rewrite the record for the committed one.
canonical="$here/agent.lgx"
if [ "$(cd "$(dirname "$out")" && pwd)/$(basename "$out")" = "$canonical" ]; then
    QT_VERSIONS="${qtrefs:+$(printf '%s\n' "$qtrefs" | tr -d ')' | awk '{print $NF}' | sort -u | tr '\n' ' ')}" \
    COMPILER="$( (c++ --version 2>/dev/null || echo unknown) | head -1)" \
    VARIANT="$variant" \
    python3 "$here/../scripts/write-package-record.py" "$out" "$plugin"
else
    echo "  <-    not $canonical, so module/agent.lgx.sources is left alone"
fi

"$lgx" verify "$out"
"$lgx" manifest "$out"
echo
echo "sha256  $(shasum -a 256 "$out" | cut -d' ' -f1)"
