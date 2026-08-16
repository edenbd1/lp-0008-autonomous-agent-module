#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Write module/harness/harness.sources — what the harness that was just built came from.

  write-harness-record.py <variant> <binary>

Run by `scripts/logos-core-headless.sh --build-harness` at the moment the binary
is produced, which is the only moment the answer is known for certain. It is the
same record `module/agent.lgx.sources` is, for the same reason: a binary in a
repository is a claim about a build, and this repository has already shipped two
plugins that did not come from the source committed beside them.

It merges. One machine builds one variant — a darwin-arm64 harness cannot be
produced on Linux and neither Linux one can be produced on macOS — so recording
`linux-arm64` must not delete the `darwin-arm64` entry, and must not silently
bless it either. A sibling is carried forward only when the bytes still on disk
are the bytes it was recorded for AND those bytes still contain every string
literal of the source being recorded now. Anything else is fatal: a variant
quietly dropping out of the record is how a stale binary stops being anyone's
problem, and a variant quietly kept is how a stale binary acquires a provenance
it never had.

`scripts/check-package-fresh.py` reads it back on every CI run, and
`scripts/logos-core-headless.sh` re-checks the one it is about to run against it
before running it.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader

_checker = SourceFileLoader(
    "check_package_fresh",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "check-package-fresh.py")).load_module()

RECORD = _checker.HARNESS_RECORD
SOURCE = _checker.HARNESS_SOURCE
NAME = _checker.HARNESS_NAME

# The SDK translation units scripts/logos-core-headless.sh compiles into the
# harness, in the order it compiles them. Recorded by content hash, not just by
# revision: a checkout can be at c87f343 and have a modified working tree, and
# "which SDK was this built against" is exactly the question a shipped binary
# cannot answer for itself. They are NOT re-checked by check-package-fresh.py,
# because the SDK is not in this repository and a machine checking the package
# has no checkout of it — this is provenance, and it is labelled as provenance.
SDK_UNITS = (
    "cpp/logos_api.cpp", "cpp/logos_api_client.cpp", "cpp/logos_api_consumer.cpp",
    "cpp/logos_api_provider.cpp", "cpp/module_proxy.cpp",
    "cpp/logos_provider_object.cpp", "cpp/qt_provider_object.cpp",
    "cpp/logos_types.cpp",
)

HEADER = [
    "Written by scripts/logos-core-headless.sh --build-harness; checked by",
    "scripts/check-package-fresh.py in CI, and again by",
    "scripts/logos-core-headless.sh before it runs the binary. Do not",
    "hand-edit: the point of this file is that it is a by-product of a real",
    "build, and an edited one is a claim about a build that did not happen.",
    "",
    "WHY THE HARNESS IS COMMITTED AT ALL. The deployment criterion asks for a",
    "single CLI command 'on any machine using Logos Core headless'. The",
    "command used to COMPILE this harness, which made a C++17 compiler, Qt",
    "6.9.2 with qtremoteobjects, a logos-cpp-sdk checkout and nlohmann/json",
    "prerequisites of running it -- so a machine that could run Logos Core",
    "still could not run the command. It is built once per variant instead,",
    "here, and rebuilt by whoever would rather not take a binary on trust:",
    "",
    "    ./scripts/logos-core-headless.sh --build-harness",
    "    HARNESS_FROM_SOURCE=1 ./scripts/logos-core-headless.sh storage",
    "",
    "One machine builds one variant, so recording one MERGES it into whatever",
    "is already here; a sibling is kept only if the bytes on disk are the bytes",
    "it was recorded for and they still contain every string literal of the",
    "source being recorded.",
    "",
    "sdk_units is provenance and not a check: the logos-cpp-sdk is not in this",
    "repository, so a machine verifying the package cannot recompute it.",
    "What IS checked, from the binary's own bytes and with no toolchain: the",
    "sha256 below, the architecture the variant name claims, that there is no",
    "rpath naming the machine that compiled it, that it links the runtime and",
    "Qt, that it needs no newer glibc/libstdc++ than liblogos_core.so itself,",
    "and that every string literal of the source is inside it.",
]


def carried_forward(repo, packaged, lits):
    """The other variants already recorded, if their binaries still earn it."""
    rec_path = os.path.join(repo, RECORD)
    if not os.path.isfile(rec_path):
        return {}
    with open(rec_path, encoding="utf-8") as fh:
        previous = json.load(fh).get("variants") or {}

    kept = {}
    for variant, info in sorted(previous.items()):
        if variant == packaged:
            continue
        path = os.path.join(repo, os.path.dirname(RECORD), variant, NAME)
        if not os.path.isfile(path):
            sys.exit("  FAIL  %s was recorded and is no longer on disk. Rebuild "
                     "it on that platform, or delete its entry deliberately; a "
                     "record that forgets a variant is how a stale one stops "
                     "being noticed." % os.path.relpath(path, repo))
        with open(path, "rb") as fh:
            blob = fh.read()
        if _checker.sha256_bytes(blob) != info.get("sha256"):
            sys.exit("  FAIL  %s is not the binary the record was written for. "
                     "Rebuild that variant too: this run would otherwise record "
                     "it against source it may never have seen."
                     % os.path.relpath(path, repo))
        missing = [lit for lit in lits if not _checker.in_binary(lit, blob)]
        if missing:
            sys.exit("  FAIL  %d of %d source literals are absent from the %s "
                     "harness already on disk, so it was not built from the "
                     "source being recorded now. Rebuild it before recording "
                     "another variant." % (len(missing), len(lits), variant))
        print("  ok    %s carries forward: same bytes, and they still contain "
              "every source literal" % variant)
        kept[variant] = info
    return kept


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    variant, binary = sys.argv[1], sys.argv[2]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    want = os.path.join(repo, os.path.dirname(RECORD), variant, NAME)
    if os.path.abspath(binary) != want:
        sys.exit("  FAIL  a %s harness belongs at %s, and this one is at %s"
                 % (variant, os.path.relpath(want, repo), binary))
    with open(binary, "rb") as fh:
        blob = fh.read()

    shape = _checker.binary_shape(blob)
    if shape["rpaths"]:
        sys.exit("  FAIL  the harness just built carries rpath(s) %s. Every one "
                 "of them names a directory on this machine, so the binary "
                 "would work here and nowhere else. Refusing to record it."
                 % ", ".join(shape["rpaths"]))

    sdk_root = os.environ.get("LOGOS_CPP_SDK_ROOT") or ""
    sdk_units = {}
    for rel in SDK_UNITS:
        p = os.path.join(sdk_root, rel)
        if os.path.isfile(p):
            sdk_units[rel] = _checker.sha256_file(p)
    if len(sdk_units) != len(SDK_UNITS):
        sys.exit("  FAIL  only %d of the %d logos-cpp-sdk translation units the "
                 "harness is built from could be hashed under "
                 "LOGOS_CPP_SDK_ROOT=%r. The binary was just built from them, "
                 "so a record that cannot name them is a record of nothing."
                 % (len(sdk_units), len(SDK_UNITS), sdk_root))

    lits = _checker.literals(repo, [SOURCE])
    missing = [lit for lit in lits if not _checker.in_binary(lit, blob)]
    if missing:
        sys.exit("  FAIL  %d of %d literals in %s are not in the binary that was "
                 "just built from it; something other than this source was "
                 "compiled." % (len(missing), len(lits), SOURCE))

    variants = {
        variant: {
            "sha256": _checker.sha256_bytes(blob),
            "bytes": len(blob),
            "format": "%s %s" % (shape["format"], shape["arch"]),
            "needed": shape["needed"],
            "version_needs": shape["version_needs"],
            "built_on": (os.environ.get("BUILT_ON") or "").strip() or None,
            "compiler": (os.environ.get("COMPILER") or "").strip() or None,
            "qt": (os.environ.get("QT_VERSIONS") or "").strip() or None,
            "sdk_revision": (os.environ.get("SDK_REVISION") or "").strip() or None,
            "sdk_units": sdk_units,
        }
    }
    variants.update(carried_forward(repo, variant, lits))

    record = {
        "_comment": HEADER,
        "harness": "%s/<variant>/%s" % (os.path.dirname(RECORD), NAME),
        "built_from": {SOURCE: _checker.sha256_file(os.path.join(repo, SOURCE))},
        "variants": variants,
    }
    out = os.path.join(repo, RECORD)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("  ok    recorded the %s harness in %s: sha256 %s, %d bytes, %s"
          % (variant, RECORD, variants[variant]["sha256"][:16], len(blob),
             variants[variant]["format"]))
    print("  ok    %d literal(s) of %s are in it" % (len(lits), SOURCE))


if __name__ == "__main__":
    main()
