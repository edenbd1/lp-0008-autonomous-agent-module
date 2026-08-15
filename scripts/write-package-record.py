#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Write module/agent.lgx.sources — what the package that was just built came from.

Run by module/package-basecamp.sh at the moment the package is made, which is
the only moment the answer is known for certain. scripts/check-package-fresh.py
reads it back on every CI run; the two share source_inventory() below rather
than each having their own idea of what a build input is, because a recorder and
a checker that disagree about the file set is a hole in the shape of whichever
files only one of them looks at.

  write-package-record.py <package.lgx> <plugin binary>

with VARIANT, and optionally QT_VERSIONS and COMPILER, in the environment.
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

RECORD = "module/agent.lgx.sources"

HEADER = [
    "Written by module/package-basecamp.sh; checked by",
    "scripts/check-package-fresh.py in CI. Do not hand-edit: the point of this",
    "file is that it is a by-product of a real build, and an edited one is a",
    "claim about a build that did not happen.",
    "",
    "If CI says module/agent.lgx is stale, the fix is to rebuild and repackage",
    "(docs/basecamp.md), not to update the hashes here.",
    "",
    "package_root_hash is the manifest's content-derived root, not the",
    "archive's sha256: gzip records a timestamp, so repackaging identical",
    "contents produces a different file. variants[].sha256 is the load-bearing",
    "one — the root hash covers the variants tree and not the manifest's",
    "name, type or author.",
]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pkg_path, plugin_path = sys.argv[1], sys.argv[2]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    variant = os.environ.get("VARIANT") or sys.exit("VARIANT is not set")

    manifest, members = _checker.read_package(pkg_path)
    with open(plugin_path, "rb") as fh:
        plugin = fh.read()

    member = "variants/%s/%s" % (variant, os.path.basename(plugin_path))
    if members.get(member) != plugin:
        sys.exit("  FAIL  %s in the package is not the plugin at %s; refusing "
                 "to record a provenance that is already wrong"
                 % (member, plugin_path))

    record = {
        "_comment": HEADER,
        "package": _checker.PACKAGE,
        "package_root_hash": manifest.get("hashes", {}).get("root"),
        "variants": {
            variant: {
                "main": os.path.basename(plugin_path),
                "sha256": _checker.sha256_bytes(plugin),
                "bytes": len(plugin),
            }
        },
        "toolchain": {
            "host": variant,
            "qt": (os.environ.get("QT_VERSIONS") or "").strip() or None,
            "compiler": (os.environ.get("COMPILER") or "").strip() or None,
            "note": "Qt is a CEILING, not a floor: a plugin built against a "
                    "newer Qt than the host's reports a successful load and "
                    "then times out. See docs/basecamp.md.",
        },
        "built_from": _checker.source_inventory(repo),
    }

    out = os.path.join(repo, RECORD)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("  ok    recorded %d build input(s) in %s"
          % (len(record["built_from"]), RECORD))


if __name__ == "__main__":
    main()
