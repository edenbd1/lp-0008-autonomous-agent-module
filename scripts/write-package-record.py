#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Write a .lgx's `.sources` record — what the package that was just built came from.

Run by module/package-basecamp.sh and app/package-ui.sh at the moment the
package is made, which is the only moment the answer is known for certain.
scripts/check-package-fresh.py reads it back on every CI run; the writers and the
checker share source_inventory(), literals() and binary_shape() below rather than
each having their own idea of what a build input is, because a recorder and a
checker that disagree about the file set is a hole in the shape of whichever
files only one of them looks at.

  write-package-record.py [--package module|app] <package.lgx> <plugin binary>
  write-package-record.py --package app --all-variants <package.lgx>

with VARIANT, and optionally QT_VERSIONS, COMPILER and DELIVERY, in the
environment. It merges: the variant named by VARIANT is (re)recorded and every
other variant already in the record is carried forward only if the package still
earns it — see carried_forward() below.

TWO PACKAGES, ONE RECORDER. `module/agent.lgx` is the core module and
`app/agent-ui.lgx` is the owner console; they differ in which files go in and
which .lgx comes out, and in nothing else. The alternative was a second script
beside this one, which is how the two would come to disagree about what counts
as a build input, what counts as enough corroboration, and what a record even
looks like. The spec below is read out of scripts/check-package-fresh.py so that
the file doing the checking is the file that defines both.

--all-variants exists for one situation and says so in the record it writes.
Normally one machine builds one variant and the other variants are carried
forward from the record that machine already has. `app/agent-ui.lgx` was
committed with three variants and no record at all, so there was nothing to carry
forward from — and two of those three cannot be rebuilt on any one machine. So
they are recorded on exactly the evidence carried_forward() accepts for a
sibling, which is the strongest evidence available without the machine that built
them: the bytes in the package, the architecture the binary's own header
declares, and every string literal of the source being recorded present inside
them. What that evidence cannot supply — which compiler, on which host — is
written as null rather than guessed, and a null there is visible in the record.
"""

import argparse
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

APP_HEADER = [
    "Written by app/package-ui.sh; checked by scripts/check-package-fresh.py in",
    "CI. Do not hand-edit: the point of this file is that it is a by-product of",
    "a real build, and an edited one is a claim about a build that did not",
    "happen.",
    "",
    "If CI says app/agent-ui.lgx is stale, the fix is to rebuild and repackage",
    "(app/README.md), not to update the hashes here.",
    "",
    "One package carries three platform variants and one machine can build one",
    "of them, so recording a variant MERGES it into whatever is already here; a",
    "variant already recorded is kept only if the bytes still in the package",
    "are the bytes it was recorded for AND those bytes still contain every",
    "string literal of the source being recorded. A stale sibling cannot ride",
    "along on a fresh variant's repackaging.",
    "",
    "package_root_hash is the manifest's content-derived root, not the",
    "archive's sha256: gzip records a timestamp, so repackaging identical",
    "contents produces a different file. variants[].sha256 is the load-bearing",
    "one — the root hash covers the variants tree and not the manifest's name,",
    "type or author.",
    "",
    "variants[].format is read out of the binary's own header, not out of the",
    "directory it sits in. app/README.md tells an installer to flatten ONE",
    "variant into the plugins directory and warns that a directory holding",
    "another platform's binary looks complete and can never load, so the claim",
    "the variant name makes about the file inside it is checked rather than",
    "trusted.",
    "",
    "variants[].qt is the Qt each binary was linked against, read from",
    "LC_LOAD_DYLIB on Mach-O and from the Qt_x.y symbol version on ELF. Qt is a",
    "CEILING: Basecamp 0.2.2 bundles 6.9.2 and refuses a plugin built against a",
    "higher minor, which presents as a load that reports success and then times",
    "out on every call. app/package-ui.sh can only ask that of the platform it",
    "is running on; the checker asks it of all three.",
]


class Spec:
    """Which package is being recorded. Everything that differs, in one place."""

    def __init__(self, name, package, record, dirs, files, suffixes,
                 exclusions, floor, header, repackage):
        self.name = name
        self.package = package
        self.record = record
        self.dirs = dirs
        self.files = files
        self.suffixes = suffixes
        self.exclusions = exclusions
        self.floor = floor
        self.header = header
        self.repackage = repackage

    def inventory(self, repo):
        return _checker.source_inventory(repo, self.dirs, self.files,
                                         self.suffixes)


SPECS = {
    "module": Spec(
        "module", _checker.PACKAGE, RECORD,
        _checker.SOURCE_DIRS, _checker.SOURCE_FILES, _checker.SOURCE_SUFFIXES,
        _checker.LITERAL_EXCLUSIONS, _checker.SOURCE_LITERAL_FLOOR, HEADER,
        "cmake --build build-basecamp -j8 && module/package-basecamp.sh "
        "build-basecamp"),
    "app": Spec(
        "app", _checker.APP_PACKAGE, _checker.APP_RECORD,
        _checker.APP_SOURCE_DIRS, _checker.APP_SOURCE_FILES,
        _checker.APP_SOURCE_SUFFIXES, _checker.APP_LITERAL_EXCLUSIONS,
        _checker.APP_LITERAL_FLOOR, APP_HEADER,
        "cmake --build build-ui -j8 && app/package-ui.sh build-ui"),
}


def shape_of(blob, member):
    """`format` for the record: what the binary's own header says it is.

    Fatal when it cannot be read. A record that omits the field it was asked to
    write, on the grounds that the file was unreadable, is a record whose reader
    then has nothing to compare — and check-package-fresh.py requires the field
    on every app variant precisely so that "unreadable" cannot become "absent"
    can become "unchecked".
    """
    try:
        shape = _checker.binary_shape(blob)
    except Exception as exc:                  # noqa: BLE001 - reported as fatal
        sys.exit("  FAIL  %s could not be read as a shared object, so its "
                 "architecture cannot be recorded: %s" % (member, exc))
    return shape


def qt_of(shape):
    version, _where = _checker.qt_built_against(shape)
    return ".".join(str(n) for n in version) if version else None


def variant_entry(blob, main, member, **provenance):
    """One variant's row: what the bytes are, and what the machine says.

    `qt` is DERIVED from the binary rather than taken from the environment, and
    that is a change to what the module record used to hold. package-basecamp.sh
    exports QT_VERSIONS only on its Darwin branch, so the two Linux variants of
    module/agent.lgx are recorded `"qt": null` — the field exists, three variants
    have it, and two of them say nothing, which is the shape of a fact nobody
    checked. The binary knows; both readers can ask it; so it is asked. What the
    packaging script observed with otool or readelf is kept beside it as
    `qt_reported`, because a disagreement between the two is worth being able to
    see and neither of them is the other's substitute.
    """
    shape = shape_of(blob, member)
    entry = {
        "main": main,
        "sha256": _checker.sha256_bytes(blob),
        "bytes": len(blob),
        "format": "%s %s" % (shape["format"], shape["arch"]),
        "qt": qt_of(shape),
    }
    entry.update(provenance)
    return entry


def corroborate(repo, spec, lits, blob, variant, what):
    """Every literal of the source being recorded is inside these bytes.

    The same evidence check-package-fresh.py's layer 2 recomputes, run here
    before anything is written down. `lits` has already cleared spec.floor in
    literals_or_die(); without that floor `missing` is empty for a scanner that
    extracted nothing and this blesses every binary on the machine.
    """
    missing = [lit for lit in lits
               if lit not in spec.exclusions
               and not _checker.in_binary(lit, blob)]
    if missing:
        sys.exit(
            "  FAIL  %d of %d source literals are absent from the %s binary %s, "
            "so it was not built from the source being recorded now. Rebuild "
            "and repackage it before recording another variant."
            % (len(missing), len(lits), variant, what))


def literals_or_die(repo, spec):
    """The source's literals, or a refusal to record anything at all.

    THE FLOOR, shared with the checker rather than typed here. Both
    corroborations below — the binary just built, and every sibling carried
    forward — are "the list of literals missing from these bytes is empty", and
    that list is also empty when the scanner extracted nothing, so a blind
    scanner blesses every binary on the machine and this file writes the record
    CI then reads back. Measured on the module side by making literals() return
    {}: this printed two `ok` lines and exited 0 while check-package-fresh.py,
    sharing the very same helper, went red with "the scanner found essentially
    nothing and would agree with any binary".
    """
    lits = spec.inventory(repo)
    lits = _checker.literals(repo, sorted(lits))
    if len(lits) < spec.floor:
        sys.exit(
            "  FAIL  only %d source literal(s) of >= %d bytes were extracted "
            "from %s, so 'the binary still contains every one of them' would be "
            "true of any binary at all. Refusing to record a variant, or to "
            "carry one forward, on a corroboration that checked nothing."
            % (len(lits), _checker.MIN_LITERAL, ", ".join(spec.dirs)))
    return lits


def carried_forward(repo, spec, packaged, members, lits):
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
    rec_path = os.path.join(repo, spec.record)
    if not os.path.isfile(rec_path):
        return {}
    with open(rec_path, encoding="utf-8") as fh:
        previous = json.load(fh).get("variants") or {}

    kept = {}
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
        corroborate(repo, spec, lits, blob, variant, "already in the package")
        print("  ok    %s carries forward: same bytes, and all %d source "
              "literal(s) are still in them" % (variant, len(lits)))
        # A carried-forward entry keeps its provenance and gains the fields this
        # writer adds, computed from the bytes that are right here. An entry
        # written before `format` existed would otherwise stay without one for
        # as long as nobody rebuilds that platform, and the checker requires it.
        kept[variant] = dict(info)
        kept[variant].update(variant_entry(
            blob, info.get("main"), member,
            qt_reported=info.get("qt_reported"),
            compiler=info.get("compiler"),
            delivery=info.get("delivery"), built_on=info.get("built_on")))
    return kept


def all_variants(repo, spec, manifest, members, lits):
    """Every variant the package ships, recorded from the package's own bytes.

    Only reachable through --all-variants, and only sensible for a package that
    was committed before it had a record. Each variant is put through exactly
    what carried_forward() puts a sibling through, which is the whole of the
    evidence a machine that did not build it can have.
    """
    variants = {}
    for variant, main in sorted((manifest.get("main") or {}).items()):
        member = "variants/%s/%s" % (variant, main)
        blob = members.get(member)
        if blob is None:
            sys.exit("  FAIL  the manifest declares main[%s] = %r and the "
                     "package does not contain %s" % (variant, main, member))
        corroborate(repo, spec, lits, blob, variant, "in the package")
        variants[variant] = variant_entry(
            blob, main, member,
            # Null, and deliberately so: this machine did not build these and
            # writing what it would have used is the difference between a record
            # and a guess.
            compiler=None, built_on=None, qt_reported=None)
        print("  ok    %s recorded from the package: %s, sha256 %s, %d bytes, "
              "Qt %s, and all %d source literal(s) are in it"
              % (variant, variants[variant]["format"],
                 variants[variant]["sha256"][:16], len(blob),
                 variants[variant]["qt"], len(lits)))
    if not variants:
        sys.exit("  FAIL  the manifest declares no `main`, so there is no "
                 "variant to record")
    return variants


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", choices=sorted(SPECS), default="module",
                    help="which .lgx is being recorded (default: module, so "
                         "that module/package-basecamp.sh's two-argument call "
                         "means what it always meant)")
    ap.add_argument("--all-variants", action="store_true",
                    help="record every variant the package ships, from the "
                         "package's own bytes; for a package committed before "
                         "it had a record. Needs no plugin argument.")
    ap.add_argument("pkg")
    ap.add_argument("plugin", nargs="?")
    args = ap.parse_args()

    spec = SPECS[args.package]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    manifest, members = _checker.read_package(args.pkg)
    lits = literals_or_die(repo, spec)

    if args.all_variants:
        if args.plugin:
            sys.exit("--all-variants records what is in the package; it takes "
                     "no plugin argument")
        variants = all_variants(repo, spec, manifest, members, lits)
    else:
        if not args.plugin:
            sys.exit(__doc__)
        variant = os.environ.get("VARIANT") or sys.exit("VARIANT is not set")
        with open(args.plugin, "rb") as fh:
            plugin = fh.read()
        member = "variants/%s/%s" % (variant, os.path.basename(args.plugin))
        if members.get(member) != plugin:
            sys.exit("  FAIL  %s in the package is not the plugin at %s; "
                     "refusing to record a provenance that is already wrong"
                     % (member, args.plugin))
        # The binary that was just built is corroborated too, and it was not
        # before: a sibling had to contain every literal of the source to be
        # carried forward while the freshly packaged one was recorded on the
        # strength of having been handed over. A build directory that did not
        # relink produces exactly that — the plugin in the package IS the plugin
        # at the path, and neither of them is this source.
        corroborate(repo, spec, lits, plugin, variant, "just packaged")
        variants = {
            variant: variant_entry(
                plugin, os.path.basename(args.plugin), member,
                qt_reported=(os.environ.get("QT_VERSIONS") or "").strip() or None,
                compiler=(os.environ.get("COMPILER") or "").strip() or None,
                built_on=(os.environ.get("BUILT_ON") or "").strip() or None,
                delivery=(os.environ.get("DELIVERY") or "").strip() or None),
        }
        variants.update(carried_forward(repo, spec, variant, members, lits))

    record = {
        "_comment": spec.header,
        "package": spec.package,
        "package_root_hash": manifest.get("hashes", {}).get("root"),
        "variants": variants,
        "note": "Qt is a CEILING, not a floor: a plugin built against a newer "
                "Qt than the host's reports a successful load and then times "
                "out. See docs/basecamp.md.",
        "built_from": spec.inventory(repo),
    }
    if args.all_variants:
        record["recorded_from"] = (
            "the package's own bytes, not a build: this record was written for "
            "a package that was already committed without one. Every variant "
            "was put through what a carried-forward sibling is put through — "
            "the bytes in the package, the architecture its own header "
            "declares, and every string literal of built_from inside it — and "
            "the fields only the building machine could know are null. "
            "Repackaging any variant with " + spec.repackage + " replaces its "
            "entry with a real build's.")

    out = os.path.join(repo, spec.record)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print("  ok    recorded %d build input(s) and %d variant(s) in %s"
          % (len(record["built_from"]), len(variants), spec.record))


if __name__ == "__main__":
    main()
