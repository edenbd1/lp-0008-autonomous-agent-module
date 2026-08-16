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
# `agent.lgx` packaged at 3322142 stayed committed across five commits to
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
elif [ "$(uname -s)" = "Linux" ] && command -v readelf >/dev/null 2>&1; then
    # The same two questions, asked the way ELF answers them, plus one the Mach-O
    # check gets for free and this one does not.
    #
    # There is no `@rpath` on ELF: a DT_NEEDED entry is a bare soname and the
    # search path is DT_RUNPATH. So the analogue of "referenced by absolute path"
    # is a RUNPATH naming the build machine's Qt — `/opt/Qt/6.9.2/gcc_64/lib`
    # resolves in the container that built it and nowhere else. `$ORIGIN` is what
    # `@loader_path` is, and it is what LogosModule.cmake sets.
    #
    # And the version. On macOS a Homebrew Qt gives itself away by hardcoding
    # `/opt/homebrew/...`, so the absolute-path rule catches it; a distribution
    # Qt on Linux hardcodes nothing, so a 6.11 plugin would pass every check
    # above and then do the thing docs/basecamp.md measures — load "successfully"
    # and time out on every call. Qt writes the version it was compiled against
    # into a `.qtversion` section as patch, minor, major; that is the number the
    # host compares with its own before it will hand the plugin a `logos_host`.
    qtneeded="$(readelf -d "$plugin" 2>/dev/null | sed -n 's/.*Shared library: \[\(libQt6[^]]*\)\].*/\1/p')"
    if [ -z "$qtneeded" ]; then
        echo "  FAIL  the plugin references no Qt libraries at all" >&2
        exit 1
    fi
    runpath="$(readelf -d "$plugin" 2>/dev/null | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\].*/\2/p')"
    case ":${runpath}:" in
        *:/*) echo "  FAIL  the plugin's RUNPATH names an absolute directory, so it" >&2
              echo "        resolves only on the machine that built it: $runpath" >&2
              exit 1 ;;
    esac
    # Out of the `.note.qt.metadata` note, whose first three bytes are the
    # metadata format version and then Qt's own major and minor — the two
    # numbers `qt_plugin_query_metadata` compares against the host's before it
    # will accept the plugin. Not out of `.qtversion`, which carries the patch
    # level as well and is padding-dependent; it is read below only to print.
    qtnote="$(readelf -n "$plugin" 2>/dev/null |
              sed -n 's/^ *description data: *\([0-9a-f]\{2\}\) \([0-9a-f]\{2\}\) \([0-9a-f]\{2\}\).*/\1 \2 \3/p' |
              head -n1)"
    qtfmt="$(printf '%s' "$qtnote" | cut -d' ' -f1)"
    qtmajhex="$(printf '%s' "$qtnote" | cut -d' ' -f2)"
    qtminhex="$(printf '%s' "$qtnote" | cut -d' ' -f3)"
    if [ "$qtfmt" != "01" ] || [ -z "$qtminhex" ]; then
        echo "  FAIL  the plugin carries no readable Qt plugin-metadata note, so" >&2
        echo "        the Qt it was built against cannot be checked against the" >&2
        echo "        6.9.2 Basecamp bundles — and a plugin built against a newer" >&2
        echo "        Qt reports a successful load and then times out." >&2
        exit 1
    fi
    qtmaj=$((16#$qtmajhex)); qtmin=$((16#$qtminhex))
    # `.qtversion` is patch, minor, major, little-endian. Used only to print the
    # patch level, and only when its own minor and major agree with the note —
    # otherwise the two disagree and the note is the one that decides loads.
    qtpat="$(readelf -x .qtversion "$plugin" 2>/dev/null |
             awk '/^ *0x/ {print $4; exit}')"
    if [ "${#qtpat}" -eq 8 ] && [ "${qtpat:2:2}" = "$qtminhex" ] \
       && [ "${qtpat:4:2}" = "$qtmajhex" ]; then
        qtver="$qtmaj.$qtmin.$((16#${qtpat:0:2}))"
    else
        qtver="$qtmaj.$qtmin"
    fi
    if [ "$qtmaj" -ne 6 ] || [ "$qtmin" -gt 9 ]; then
        echo "  FAIL  built against Qt $qtver. Basecamp 0.2.2 bundles 6.9.2 and Qt" >&2
        echo "        refuses any plugin whose minor version exceeds the host's —" >&2
        echo "        see docs/basecamp.md, which measures what that then looks" >&2
        echo "        like (a load that reports success and times out)." >&2
        exit 1
    fi
    echo "  ok    Qt is referenced by soname ($(printf '%s ' $qtneeded)) with" \
         "RUNPATH ${runpath:-(none)}, built against $qtver"
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$variant"
cp "$plugin" "$stage/$variant/"
# The module reads nothing from metadata.json at runtime — the copy is what the
# host reads to learn the module's name, type and dependencies before it opens
# the binary at all.
cp "$here/metadata.json" "$stage/$variant/"

# Anything else the build staged beside the plugin travels with it.
#
# This is `liblogosdelivery`, and the package is unloadable without it: the
# plugin's rpath is `@loader_path`/`$ORIGIN`, so the host resolves the library in
# the module directory it unpacked, finds nothing, and the load fails with a
# missing-symbol message nobody sees. A package that carries the plugin and not
# its library is exactly the shape of defect this script exists to refuse, so
# every sibling library is copied and then *checked for* below.
for sibling in "$(dirname "$plugin")"/*.dylib "$(dirname "$plugin")"/*.so; do
    [ -f "$sibling" ] || continue
    [ "$sibling" = "$plugin" ] && continue
    cp "$sibling" "$stage/$variant/"
    echo "  ok    $(basename "$sibling") travels with the plugin"

    # And so does its licence, because this is redistribution of somebody
    # else's binary. MIT wants the copyright and permission notice carried "in
    # all copies or substantial portions"; Apache-2.0 wants the licence and the
    # notices retained. A package that ships the library and not the licence is
    # not a packaging bug, it is a licensing one, so it fails here rather than
    # shipping.
    base="$(basename "$sibling")"; base="${base%%.*}"
    third="$here/third-party/$base"
    if [ -d "$third" ]; then
        mkdir -p "$stage/$variant/third-party/$base"
        cp "$third"/* "$stage/$variant/third-party/$base/"
        echo "  ok    so does its licence ($(ls "$third" | tr '\n' ' '))"
    else
        echo "  FAIL  $(basename "$sibling") is redistributed inside this package" >&2
        echo "        and module/third-party/$base does not exist, so the package" >&2
        echo "        would ship somebody else's binary with no licence in it" >&2
        exit 1
    fi
done

# And the check that makes the copy above mean something.
#
# It cannot be an `otool -L` check, and the reason is the point: the plugin
# does not LINK the delivery library, it `dlopen`s it by name at run time, so
# there is no load command to read. That is deliberate — a link-time dependency
# turns a missing file into a plugin that does not load, which Basecamp reports
# to nobody (see the note in module/CMakeLists.txt). But it also means the
# linker can no longer tell us what the binary needs, so the *name it will ask
# for* is read out of the binary instead, and every name that is in there has to
# be in the package.
missing=""
for wanted in liblogosdelivery.dylib liblogosdelivery.so libstorage.dylib libstorage.so; do
    strings -a "$plugin" 2>/dev/null | grep -qxF "$wanted" || continue
    [ -f "$stage/$variant/$wanted" ] || missing="$missing $wanted"
done
if [ -n "$missing" ]; then
    echo "  FAIL  the plugin opens these by name at run time and the package" >&2
    echo "        carries none of them, so every skill on the wire would" >&2
    echo "        refuse in an installation that looks complete:$missing" >&2
    exit 1
fi
echo "  ok    every library the plugin opens by name at run time is in the package"

# ONE PACKAGE, ONE VARIANT PER PLATFORM — and only one of them is ever built on
# the machine doing the packaging. So an existing package is ADDED TO, not
# replaced: `lgx add` replaces the variant named and leaves the others alone,
# which is how `module/agent.lgx` comes to carry both `darwin-arm64` and
# `linux-amd64` when neither machine can build the other's binary.
#
# The obvious hazard is the one this repository has already shipped twice under
# a different name: a sibling variant left behind by later commits, riding along
# on a fresh variant's repackaging and inheriting its provenance.
# scripts/write-package-record.py refuses that below — a variant is carried
# forward in the record only if the bytes still in the package are the bytes it
# was recorded for AND they still contain every string literal of the source
# being recorded.
#
# LGX_FRESH=1 starts a new package instead, for the case where the old one is
# genuinely to be discarded.
if [ -f "$out" ] && [ "${LGX_FRESH:-0}" != "1" ]; then
    had="$("$lgx" manifest "$out" 2>/dev/null | awk -F': *' '/^Variants:/ {print $2}')"
    echo "  <-    adding $variant to the existing package (has: ${had:-none});"
    echo "        LGX_FRESH=1 to start a new one instead"
else
    rm -f "$out"
    ( cd "$(dirname "$out")" && "$lgx" create "$(basename "${out%.lgx}")" >/dev/null )
fi
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
# `shasum` is the macOS name and `sha256sum` the GNU one; a packaging script
# that only knows one of them cannot be run on the platform it is packaging for.
if command -v sha256sum >/dev/null 2>&1; then
    echo "sha256  $(sha256sum "$out" | cut -d' ' -f1)"
else
    echo "sha256  $(shasum -a 256 "$out" | cut -d' ' -f1)"
fi
