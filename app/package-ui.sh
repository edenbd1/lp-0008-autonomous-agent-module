#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Package the built owner console as an .lgx Basecamp can install.
#
#   app/package-ui.sh <build-dir> [variant] [out.lgx]
#
# Same tool and same shape as module/package-basecamp.sh — `lgx` from
# logos-co/logos-package, found via $LGX_BIN, then
# ~/logos/src/logos-package/build/lgx, then $PATH — because the format is not
# something to reimplement: a manifest whose hashes are subtly wrong produces a
# package that installs and then fails to load.
#
# The one difference that matters is `type`. Basecamp installs a `core` package
# into its modules directory and a `ui` one into its **plugins** directory, and
# an unset type lands in neither. `lgx add` never reads metadata.json, so the
# type — along with author, description and category — is patched into the
# manifest afterwards, exactly as nix-bundle-lgx's bundle.sh does.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:?usage: package-ui.sh <build-dir> [variant] [out.lgx]}"
variant="${2:-}"
out="${3:-$here/agent-ui.lgx}"

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
    if [ -f "$build/agent_ui.$ext" ]; then plugin="$build/agent_ui.$ext"; break; fi
done
if [ -z "$plugin" ]; then
    echo "no agent_ui.{dylib,so,dll} in $build — build it first, see app/README.md" >&2
    exit 1
fi

# A build tree older than the source is how a package comes to ship code nobody
# wrote. CMake relinks on any source it is newer than, so a source newer than
# the plugin means the plugin does not contain it.
newer="$(find "$here/src" "$here/metadata.json" -newer "$plugin" -type f 2>/dev/null || true)"
if [ -n "$newer" ]; then
    echo "  FAIL  the plugin in $build is older than the source it is built from:" >&2
    printf '%s\n' "$newer" | sed "s|^$here/|          app/|" >&2
    echo "        rebuild first:  cmake --build $build" >&2
    exit 1
fi
echo "  ok    the plugin is newer than every source it is built from"

# Qt is a ceiling, not a floor. Basecamp 0.2.2 bundles 6.9.2 and refuses a
# plugin built against a higher minor — and a Homebrew build additionally
# hardcodes /opt/homebrew/... paths that resolve on this machine and no other.
if [ "$(uname -s)" = "Darwin" ] && command -v otool >/dev/null 2>&1; then
    qtrefs="$(otool -L "$plugin" | awk '/Qt[A-Za-z]*\.framework/ {print $1, $NF}')"
    if [ -z "$qtrefs" ]; then
        echo "  FAIL  the plugin references no Qt frameworks at all" >&2
        exit 1
    fi
    if printf '%s\n' "$qtrefs" | grep -qv '^@rpath/'; then
        echo "  FAIL  a Qt framework is referenced by absolute path, so it resolves" >&2
        echo "        only on this machine — see docs/basecamp.md:" >&2
        printf '%s\n' "$qtrefs" | grep -v '^@rpath/' >&2
        exit 1
    fi
    echo "  ok    Qt is referenced through @rpath, version(s):" \
         "$(printf '%s\n' "$qtrefs" | tr -d ')' | awk '{print $NF}' | sort -u | tr '\n' ' ')"

    # The plugin deliberately leaves the SDK symbols undefined so they bind to
    # the liblogos_core already in Basecamp's process. That is only correct if
    # the host really exports them; an undefined symbol nothing defines is a
    # load that fails with "symbol not found" and no other explanation.
    core="${LOGOS_CORE_LIB:-/Applications/LogosBasecamp.app/Contents/Frameworks/liblogos_core.dylib}"
    if [ -f "$core" ]; then
        # Read the export table into a variable first, and match with a
        # here-string. `nm … | grep -q` is wrong under `set -o pipefail`: grep
        # exits on the first match, nm dies of SIGPIPE, and the pipeline's
        # status is 141 — so *every* symbol found early reads as missing while
        # one found late reads as present. That is exactly how this check first
        # behaved, and it is the kind of failure that gets "fixed" by deleting
        # the check.
        core_syms="$(nm -gU "$core")"
        # Every undefined symbol that names a Logos type, not just the ones
        # mangled as `LogosAPI::…`. The first version of this matched
        # `^__ZN?K?[0-9]+Logos` and checked five symbols while the plugin had
        # seven: the two QDataStream operators for `LogosResult` are mangled
        # under QDataStream, so a host that stopped exporting them would have
        # passed this check and failed to load.
        undefined="$(nm -u "$plugin" | grep -i logos || true)"
        missing=0
        checked=0
        for sym in $undefined; do
            checked=$((checked + 1))
            if ! grep -q -- " $sym\$" <<<"$core_syms"; then
                echo "  FAIL  $sym is undefined here and not exported by liblogos_core" >&2
                missing=1
            fi
        done
        [ "$missing" -eq 0 ] || exit 1
        echo "  ok    all $checked undefined Logos symbol(s) are exported by the host's liblogos_core"
    fi
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$variant"
cp "$plugin" "$stage/$variant/"
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
        for key in ("author", "description", "type", "category", "version",
                    "dependencies", "display_name"):
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

# `lgx verify` compares the contents against the manifest's hashes. It does not
# check that `main` names one of them — a manifest can be internally consistent
# and still point at a file the package does not contain, which is an invisible
# load failure. Asserted here, at the one moment it is cheap to fix.
python3 - "$out" "$here/metadata.json" <<'PY'
import json, posixpath, sys, tarfile

pkg, metadata_path = sys.argv[1], sys.argv[2]
meta = json.load(open(metadata_path))

with tarfile.open(pkg, "r:gz") as tar:
    names = set(tar.getnames())
    manifest = json.loads(tar.extractfile("manifest.json").read())

if manifest.get("type") != "ui":
    sys.exit("  FAIL  manifest type is %r, not 'ui' — Basecamp would install this "
             "into its modules directory, where nothing loads it as a window"
             % manifest.get("type"))
print("  ok    type: ui — Basecamp installs this into its plugins directory")

mains = manifest.get("main") or {}
if not mains:
    sys.exit("  FAIL  manifest declares no `main` — the host would have nothing to load")

failures = []
for variant, main in sorted(mains.items()):
    path = posixpath.join("variants", variant, main)
    if path not in names:
        failures.append("manifest `main` for %s is %r, which is not in the package"
                        % (variant, main))
        continue
    declared = meta.get("main", "")
    if declared and main != declared and not main.startswith(declared + "."):
        failures.append("metadata.json's `main` (%r) does not agree with the "
                        "manifest's (%r) for %s" % (declared, main, variant))
    print("  ok    main[%s] = %s is in the package" % (variant, main))

if failures:
    sys.exit("\n".join("  FAIL  " + f for f in failures))
PY

"$lgx" verify "$out"
"$lgx" manifest "$out"
echo
echo "sha256  $(shasum -a 256 "$out" | cut -d' ' -f1)"
