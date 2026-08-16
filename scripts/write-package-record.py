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

with VARIANT, and optionally QT_VERSIONS, COMPILER and DELIVERY, in the
environment. It merges: the variant named by VARIANT is (re)recorded and every
other variant already in the record is carried forward only if the package still
earns it — see carried_forward() below.
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
    "One package can carry several platform variants, so this record does too.",
    "Packaging one variant MERGES it into whatever is already here; a variant",
    "already recorded is kept only if the bytes still in the package are the",
    "bytes it was recorded for AND those bytes still contain every string",
    "literal of the source being recorded. A stale sibling therefore cannot",
    "ride along on a fresh variant's repackaging.",
    "",
    "package_root_hash is the manifest's content-derived root, not the",
    "archive's sha256: gzip records a timestamp, so repackaging identical",
    "contents produces a different file. variants[].sha256 is the load-bearing",
    "one — the root hash covers the variants tree and not the manifest's",
    "name, type or author.",
]


def carried_forward(repo, packaged, members):
    """The other variants already recorded, if this package still earns them.

    A package holds one variant per platform and only one of them is built at a
    time, so packaging linux-amd64 must not delete the darwin-arm64 record — and
    must not silently bless it either. `built_from` is rewritten from the source
    tree as it stands *now*, so a sibling binary that predates that source would
    end up recorded against a provenance it does not have: exactly the defect
    this whole file exists to make impossible, arriving through a side door.

    So a recorded sibling is carried forward only when both hold:

      * the package still contains the bytes the record was written for, and
      * those bytes still contain every string literal of the source being
        recorded — the same corroboration check-package-fresh.py's layer 2 runs.

    Anything else is fatal rather than dropped. A variant quietly disappearing
    from the record is how a stale artefact stops being anyone's problem.
    """
    rec_path = os.path.join(repo, RECORD)
    if not os.path.isfile(rec_path):
        return {}
    with open(rec_path, encoding="utf-8") as fh:
        previous = json.load(fh).get("variants") or {}

    kept = {}
    lits = None
    for variant, info in sorted(previous.items()):
        if variant == packaged:
            continue
        member = "variants/%s/%s" % (variant, info.get("main"))
        blob = members.get(member)
        if blob is None:
            sys.exit(
                "  FAIL  %s was recorded and is no longer in the package. "
                "Repackage it, or delete its entry deliberately; a record that "
                "forgets a variant is how a stale one stops being noticed."
                % member)
        if _checker.sha256_bytes(blob) != info.get("sha256"):
            sys.exit(
                "  FAIL  %s in the package is not the binary the record was "
                "written for. Repackage that variant too: this run would "
                "otherwise record it against source it may never have seen."
                % member)
        if lits is None:
            lits = _checker.literals(repo, sorted(_checker.source_inventory(repo)))
            # The floor check-package-fresh.py's layer 2 has had all along, and
            # this side did not. `missing` is empty when the scanner found
            # nothing, so a blind scanner made every sibling "carry forward:
            # same bytes, and they still contain every source literal" having
            # corroborated zero literals — measured, by making literals() return
            # {}: this file printed two `ok` lines and exited 0 while
            # check-package-fresh.py, sharing the very same helper, went red
            # with "the scanner found essentially nothing and would agree with
            # any binary". A recorder that blesses on a check that examined
            # nothing writes the record CI then reads back.
            if len(lits) < 300:
                sys.exit(
                    "  FAIL  only %d source literal(s) of >= %d bytes were "
                    "extracted, so 'the sibling binary still contains every "
                    "one of them' would be true of any binary at all. Refusing "
                    "to carry a variant forward on a corroboration that checked "
                    "nothing." % (len(lits), _checker.MIN_LITERAL))
        missing = [lit for lit in lits
                   if lit not in _checker.LITERAL_EXCLUSIONS
                   and not _checker.in_binary(lit, blob)]
        if missing:
            sys.exit(
                "  FAIL  %d of %d source literals are absent from the %s binary "
                "already in the package, so it was not built from the source "
                "being recorded now. Rebuild and repackage it before adding "
                "another variant." % (len(missing), len(lits), variant))
        print("  ok    %s carries forward: same bytes, and all %d source "
              "literal(s) are still in them" % (variant, len(lits)))
        kept[variant] = info
    return kept


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

    variants = {
        variant: {
            "main": os.path.basename(plugin_path),
            "sha256": _checker.sha256_bytes(plugin),
            "bytes": len(plugin),
            "qt": (os.environ.get("QT_VERSIONS") or "").strip() or None,
            "compiler": (os.environ.get("COMPILER") or "").strip() or None,
            "delivery": (os.environ.get("DELIVERY") or "").strip() or None,
        }
    }
    variants.update(carried_forward(repo, variant, members))

    record = {
        "_comment": HEADER,
        "package": _checker.PACKAGE,
        "package_root_hash": manifest.get("hashes", {}).get("root"),
        "variants": variants,
        "note": "Qt is a CEILING, not a floor: a plugin built against a newer "
                "Qt than the host's reports a successful load and then times "
                "out. See docs/basecamp.md.",
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
