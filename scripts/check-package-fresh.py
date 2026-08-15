#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""The committed package was built from the committed source.

`module/agent.lgx` is a binary artefact built from `module/src`. Nothing about a
binary says what source it came from, so twice now source changed, nothing
repackaged, and the published artefact did not contain the fix its own commit
claimed to make:

  1. A rebase left the artefact built from source no longer in the tree: the
     `agent.lgx` packaged at f53f822 stayed committed across five commits to
     `module/src`, so the downloadable module was missing content-topic
     validation, the owner-channel hardening, `program.call`'s flag checking and
     task persistence, while README §7 offered it as the loadable asset.

  2. d995d85 stopped the module signing Agent Cards `alg: EdDSA` — an algorithm
     `scripts/use-cases/verify-agent-card.py`, this repository's own verifier,
     rejects with `unexpected algorithm 'EdDSA'` — and started signing them
     `secp256k1-bip340`. The source was fixed. The package was not rebuilt. So
     anyone installing the published `.lgx` got exactly the card this
     repository's own tooling refuses. Nothing caught it; it was found by
     counting strings in the two binaries:

         committed package : secp256k1-bip340 -> 0    EdDSA -> 1
         build of HEAD     : secp256k1-bip340 -> 1    EdDSA -> 0

     Note what could not have caught it. Both binaries are 3699040 bytes and
     both load, cast, and answer `skills()` with all 22 entries. A size check, a
     load test and the whole plugin-contract harness pass on the wrong one.

Every check `module/package-basecamp.sh` runs passed on both of those packages,
because every one of them was true of the file it was handed. What was never
checked is the only thing that matters here: whether the file it was handed came
from the source sitting beside it in the same commit.

WHAT THIS ASSERTS, in two layers.

  (1) The record. `module/agent.lgx.sources` is written by package-basecamp.sh
      at the moment the package is made, and lists the SHA-256 of every build
      input together with the SHA-256 of the plugin binary that came out and the
      package's manifest root hash. This layer recomputes all of it. Any edit to
      any build input — a string, a constant, a comparison, a whitespace change
      — moves a hash and this goes red until someone repackages. Adding or
      deleting a source file goes red too: the file *set* is compared, not just
      the files the record happens to name.

      This layer is complete over source changes and it is a record, which means
      a determined person could rewrite it by hand without rebuilding anything.
      Which is why there is a second layer that reads the binary itself.

  (2) The corroboration. Every string literal of >= 8 bytes in `module/src` and
      `module/generated_code` must appear as bytes inside the plugin the package
      actually ships. This is layer 1's forgery check and it is the check that
      would have caught defect 2 above on its own: the source at d995d85 says
      "secp256k1-bip340", and those 16 bytes are simply not in the stale binary.
      It is deliberately the same evidence the human investigation used, made
      into an assertion.

      Eight bytes is the floor because shorter literals prove nothing: "ok" or
      "{}" occurs in a 3.7 MB binary by accident. An 8-character literal does
      not — there are ~10^14 of them and 3.7*10^6 places to sit.

WHAT THIS DOES NOT ASSERT, stated plainly because the gap is real.

  - It does not rebuild on CI, and so it does not prove the shipped bytes are
    what today's source compiles to. See --rebuild below and the note on the
    `package` job in .github/workflows/ci.yml for exactly what is missing.

  - Layer 2 is one-directional: the source's literals must be in the binary, not
    the reverse. A binary carries Qt's literals, the SDK's and the generated
    glue's, so "the binary contains a string the source does not" is not a
    signal. That means a change which only *deletes* a literal, or which touches
    no literal at all (a constant, an operator, a bounds check), is caught by
    layer 1 alone. Forging layer 1 by hand *and* choosing a change that moves no
    string literal is the one path through both, and it is deliberate rather
    than the forgetfulness this file exists to make impossible.

  - It says nothing about whether the module works. That is what
    module/tests/ and the rest of the workflow are for.

--rebuild BUILDDIR additionally rebuilds the module from the committed source
and compares. It is not run in CI and the reason is in ci.yml. It fails loudly
when the toolchain is absent rather than degrading to a pass: a step that went
green because a toolchain was missing is the exact failure being closed here.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile

# The package is a gzipped tar — `lgx create` writes tar.gz with a .lgx
# extension. It is not a zip, and reading it as one gets BadZipFile, which reads
# like a corrupt package and is not.
PACKAGE = "module/agent.lgx"
RECORD = "module/agent.lgx.sources"

# Everything the build reads. module/CMakeLists.txt names each file in
# module/src explicitly, but the set is taken from the directory rather than
# from that list on purpose: a file added to module/src and forgotten in
# CMakeLists is itself a defect, and hashing the directory means the record
# notices it instead of silently agreeing with a build that never saw it.
SOURCE_DIRS = ("module/src", "module/generated_code")
SOURCE_FILES = (
    "module/CMakeLists.txt",
    "module/metadata.json",
    "module/agent_module_plugin_export.h",
)
SOURCE_SUFFIXES = (".h", ".cpp", ".hpp", ".cc", ".json", ".txt")

MIN_LITERAL = 8

# Literals that are in the source and legitimately never reach the binary. Each
# one needs a reason, and a stale entry here is an error rather than a silent
# hole: an allow-list nobody revisits is where the next defect hides. If an
# excluded literal turns up in the binary, or leaves the source, this goes red
# and the entry has to go.
LITERAL_EXCLUSIONS = {
    b"metadata.json":
        "the argument of Q_PLUGIN_METADATA(IID ... FILE \"metadata.json\") in "
        "module/generated_code/agent_qt_glue.h. moc reads that path at build "
        "time and inlines the file's *contents*; the path itself is never "
        "emitted as data.",
}
# `onChannelMessageReceived` used to be excluded here, with the reason "nothing
# in the plugin ODR-uses it — OwnerChannel is host-owned and is linked but never
# constructed inside the plugin". That reason stopped being true: the plugin now
# opens its own Logos Delivery node and registers that listener on it
# (module/src/delivery_runtime.cpp), so the literal is in the binary and the
# check finds it like any other. The entry is gone rather than kept, because a
# stale exclusion is a hole in exactly the check that closed the last two
# stale-package defects.


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path):
    with open(path, "rb") as fh:
        return sha256_bytes(fh.read())


def source_inventory(repo):
    """Every build input, as {repo-relative path: sha256}."""
    found = {}
    for rel in SOURCE_FILES:
        p = os.path.join(repo, rel)
        if os.path.isfile(p):
            found[rel] = sha256_file(p)
    for d in SOURCE_DIRS:
        base = os.path.join(repo, d)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.endswith(SOURCE_SUFFIXES):
                    continue
                p = os.path.join(dirpath, fn)
                found[os.path.relpath(p, repo)] = sha256_file(p)
    return found


# --------------------------------------------------------------------------
# String literals out of C++ source.
#
# Written as a scanner rather than a regex because three constructs make a naive
# `"([^"]*)"` wrong in ways that produce false failures, and a gate that cries
# wolf is one people learn to skip:
#
#   R"(raw ... " ... )"   a raw literal may contain a bare quote
#   '"'                   a character literal may BE a bare quote, and a regex
#                         that misses it treats the rest of the file as a string
#   "a" "b"               adjacent literals are one object; neither half is in
#                         the binary on its own, only the concatenation
#
# Measured against the shipped package: with those three handled, 656 literals
# of >= 8 bytes are found in module/src + module/generated_code and 654 of them
# are present in the binary. The two that are not are LITERAL_EXCLUSIONS above.
# Without them the scanner reports 21 phantom absences, every one a parsing
# artefact.
# --------------------------------------------------------------------------

_INCLUDE = re.compile(r"(?m)^\s*#\s*include[^\n]*")
_PIECE = re.compile(r"\x00(RAW|STR)([^\x00]*)\x00")
_RUN = re.compile(r"\x00(?:RAW|STR)[^\x00]*\x00(?:\s*\x00(?:RAW|STR)[^\x00]*\x00)*")


def _tokenise(src):
    """Replace every literal with a \\0-delimited marker; drop comments, char
    literals and #include lines (a header name is not data)."""
    src = _INCLUDE.sub("", src)
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    break
                j += 1
            i = j + 1
            out.append(" ")
            continue
        if c == "R" and i + 1 < n and src[i + 1] == '"':
            k = src.find("(", i + 2)
            if k > 0:
                closing = ")" + src[i + 2:k] + '"'
                j = src.find(closing, k)
                if j > 0:
                    out.append("\x00RAW" + src[k + 1:j].replace("\x00", "") + "\x00")
                    i = j + len(closing)
                    continue
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            out.append("\x00STR" + src[i + 1:j] + "\x00")
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _decode(kind, body):
    if kind == "RAW":
        return body.encode("utf-8")
    try:
        return body.encode("utf-8").decode("unicode_escape").encode("latin-1")
    except (UnicodeDecodeError, UnicodeEncodeError):
        return None


def literals(repo, paths):
    """{literal bytes: sorted [source files it came from]} for >= MIN_LITERAL."""
    found = {}
    for rel in paths:
        if not rel.endswith((".cpp", ".h", ".hpp", ".cc")):
            continue
        with open(os.path.join(repo, rel), encoding="utf-8") as fh:
            marked = _tokenise(fh.read())
        for run in _RUN.finditer(marked):
            blob = b""
            for piece in _PIECE.finditer(run.group(0)):
                part = _decode(piece.group(1), piece.group(2))
                if part is None:
                    blob = None
                    break
                blob += part
            if blob and len(blob) >= MIN_LITERAL:
                found.setdefault(blob, set()).add(rel)
    return {k: sorted(v) for k, v in found.items()}


def in_binary(needle, blob):
    """Qt stores QStringLiteral / tr() text as UTF-16, everything else is bytes
    as written, so both encodings count as present."""
    if needle in blob:
        return True
    try:
        return needle.decode("utf-8").encode("utf-16-le") in blob
    except UnicodeDecodeError:
        return False


def read_package(path):
    """(manifest, {member name: bytes}). gzip, then tar. Not a zip."""
    with tarfile.open(path, "r:gz") as tar:
        members = {}
        for m in tar.getmembers():
            if m.isfile():
                members[m.name] = tar.extractfile(m).read()
    if "manifest.json" not in members:
        raise SystemExit("  FAIL  %s holds no manifest.json" % path)
    return json.loads(members["manifest.json"]), members


class Report:
    def __init__(self):
        self.failures = []
        self.blocked = []

    def blocked_on(self, msg):
        """The check could not be run, which is not the same finding as the
        package being stale — and is still a failure. A step that goes green
        because a toolchain was missing is the defect being closed here."""
        print("  FAIL  " + msg)
        self.blocked.append(msg)
        self.failures.append(msg)

    def ok(self, msg):
        print("  ok    " + msg)

    def note(self, msg):
        print("  <-    " + msg)

    def fail(self, msg):
        print("  FAIL  " + msg)
        self.failures.append(msg)


def check(repo, rebuild_dir=None):
    r = Report()
    pkg_path = os.path.join(repo, PACKAGE)
    rec_path = os.path.join(repo, RECORD)

    for p in (pkg_path, rec_path):
        if not os.path.isfile(p):
            raise SystemExit("  FAIL  %s is missing" % os.path.relpath(p, repo))

    manifest, members = read_package(pkg_path)
    with open(rec_path, encoding="utf-8") as fh:
        record = json.load(fh)

    # ---- layer 1: the record describes THIS package -----------------------
    # Not the archive's own sha256: gzip stores a timestamp, so repackaging
    # identical contents produces a different file. The manifest root hash is
    # derived from the contents and does not move (docs/basecamp.md says the
    # same). The root hash alone is not enough either — it covers the variants
    # tree and not the manifest's name/type/author — so the plugin's own sha256
    # is recorded beside it and is the load-bearing one.
    want_root = record.get("package_root_hash")
    got_root = manifest.get("hashes", {}).get("root")
    if want_root and want_root == got_root:
        r.ok("the record describes this package: root hash %s" % got_root[:16])
    else:
        r.fail("the record's package_root_hash (%s) is not this package's (%s): "
               "the record and the .lgx came from different builds"
               % (want_root, got_root))

    recorded_variants = record.get("variants") or {}
    if not recorded_variants:
        r.fail("the record names no variants")
    binaries = {}
    for variant, info in sorted(recorded_variants.items()):
        main = info.get("main")
        member = "variants/%s/%s" % (variant, main)
        if member not in members:
            r.fail("the record names %s, which is not in the package" % member)
            continue
        blob = members[member]
        got = sha256_bytes(blob)
        # Kept for the literal check either way. A binary that fails the record
        # is exactly the one worth reading, and "which strings is it missing"
        # is the sentence that tells a stale package apart from a swapped one.
        binaries[variant] = blob
        if got == info.get("sha256"):
            r.ok("%s is the binary the record was written for (sha256 %s, %d bytes)"
                 % (member, got[:16], len(blob)))
        else:
            r.fail("%s hashes to %s, the record says %s: the package holds a "
                   "different build from the one that was recorded"
                   % (member, got[:16], str(info.get("sha256"))[:16]))
        declared = manifest.get("main", {}).get(variant)
        if declared != main:
            r.fail("the manifest's main[%s] is %r, the record's is %r"
                   % (variant, declared, main))
        meta_member = "variants/%s/metadata.json" % variant
        if meta_member in members:
            if members[meta_member] == open(
                    os.path.join(repo, "module/metadata.json"), "rb").read():
                r.ok("the metadata.json inside %s is module/metadata.json" % variant)
            else:
                r.fail("the metadata.json inside %s differs from "
                       "module/metadata.json — the host reads the packaged copy"
                       % variant)

    # docs/basecamp.md quotes the root hash as evidence, and a document that
    # quotes a superseded artefact is the same defect one level up — this line
    # was found reading `cf07408e…`, the package whose Agent Cards this
    # repository's own verifier rejects, long after that package was replaced.
    # A citation nobody re-reads is a citation that rots, so it is asserted
    # rather than remembered.
    doc = os.path.join(repo, "docs/basecamp.md")
    if got_root and os.path.isfile(doc):
        with open(doc, encoding="utf-8") as fh:
            text = fh.read()
        if got_root in text:
            r.ok("docs/basecamp.md quotes this package's root hash, not an "
                 "earlier one")
        else:
            stale = sorted(set(re.findall(r"\b[0-9a-f]{64}\b", text)))
            r.fail("docs/basecamp.md does not quote this package's root hash "
                   "(%s); it still cites %s. The document is describing a "
                   "package that is no longer the one committed."
                   % (got_root[:16], ", ".join(h[:16] for h in stale) or "none"))

    # ---- layer 1: the record describes THIS source ------------------------
    on_disk = source_inventory(repo)
    recorded = record.get("built_from") or {}
    if len(recorded) < 15:
        r.fail("the record lists only %d build inputs; module/src alone has 20, "
               "so the record is truncated and would agree with almost anything"
               % len(recorded))

    added = sorted(set(on_disk) - set(recorded))
    removed = sorted(set(recorded) - set(on_disk))
    changed = sorted(p for p in set(on_disk) & set(recorded)
                     if on_disk[p] != recorded[p])
    if not (added or removed or changed):
        r.ok("all %d build inputs hash exactly as they did when the package was "
             "made" % len(on_disk))
    else:
        for p in changed:
            r.fail("%s has changed since the package was made (%s -> %s)"
                   % (p, recorded[p][:12], on_disk[p][:12]))
        for p in added:
            r.fail("%s is a build input the package was never made from" % p)
        for p in removed:
            r.fail("%s was a build input and is gone; the package still "
                   "contains it" % p)
        print()
        print("        module/agent.lgx is stale. Rebuild and repackage:")
        print("          cmake --build build-basecamp -j8 && \\")
        print("            module/package-basecamp.sh build-basecamp")
        print("        docs/basecamp.md carries the full build environment.")
        print()

    # ---- layer 2: the shipped binary carries this source's own strings ----
    lits = literals(repo, sorted(on_disk))
    if len(lits) < 300:
        r.fail("only %d literals of >= %d bytes were extracted; the scanner "
               "found essentially nothing and would agree with any binary"
               % (len(lits), MIN_LITERAL))

    for variant, blob in sorted(binaries.items()):
        missing = []
        for lit, sites in sorted(lits.items()):
            if lit in LITERAL_EXCLUSIONS:
                continue
            if not in_binary(lit, blob):
                missing.append((lit, sites))
        checked = len(lits) - len(LITERAL_EXCLUSIONS)
        if not missing:
            r.ok("every one of the %d source literals of >= %d bytes is in the "
                 "%s binary" % (checked, MIN_LITERAL, variant))
        else:
            r.fail("%d of %d source literals are absent from the %s binary, so "
                   "that binary was not built from this source:"
                   % (len(missing), checked, variant))
            for lit, sites in missing[:12]:
                print("          %-46r  %s" % (lit[:44], ", ".join(sites)))
            if len(missing) > 12:
                print("          ... and %d more" % (len(missing) - 12))

        # The allow-list must stay exactly as large as its reasons.
        for lit, why in sorted(LITERAL_EXCLUSIONS.items()):
            if lit not in lits:
                r.fail("%r is excluded from the literal check but is no longer "
                       "in the source; delete the exclusion (%s)" % (lit, why))
            elif in_binary(lit, blob):
                r.fail("%r is excluded from the literal check but IS in the %s "
                       "binary; the exclusion is stale and is now hiding that "
                       "literal from the check" % (lit, variant))
        if not r.failures:
            r.note("%d exclusion(s), each still necessary and each still "
                   "explained in scripts/check-package-fresh.py"
                   % len(LITERAL_EXCLUSIONS))

    # ---- optional: rebuild and compare ------------------------------------
    if rebuild_dir is not None:
        rebuild_and_compare(repo, rebuild_dir, binaries, r)

    print()
    if r.blocked and len(r.blocked) == len(r.failures):
        print("%d failure(s): the check could not be completed, which is not a "
              "pass" % len(r.failures))
        return 1
    if r.failures:
        print("%d failure(s): the committed package does not agree with the "
              "committed source" % len(r.failures))
        return 1
    print("OK the shipped package was built from the source in this commit: "
          "%d build input(s) recorded and unchanged, %d literal(s) found in the "
          "shipped binary" % (len(on_disk), len(lits) - len(LITERAL_EXCLUSIONS)))
    return 0


def rebuild_and_compare(repo, build_dir, binaries, r):
    """Build module/ from the committed source and compare with what shipped.

    Whole-file comparison, and the honesty about it is this: the build IS
    byte-reproducible, but only for a fixed toolchain. Measured on the machine
    that made the committed package — rebuilt into a different build directory,
    from a source tree at a different absolute path, under a different TZ and a
    different locale — the plugin came out at the identical sha256
    595c2257...c518. So nothing about the path, the clock or the environment
    reaches the bytes.

    What does reach the bytes is the compiler, the macOS SDK, the Qt patch level
    and nlohmann/json's version. None of those is pinned by this repository, so
    this comparison is only meaningful when it is run on the toolchain
    docs/basecamp.md pins — which is why it is opt-in here and not a CI step. On
    a mismatch it says so rather than claiming the package is stale.
    """
    env = dict(os.environ)
    for var in ("LOGOS_MODULE_BUILDER_ROOT", "LOGOS_MODULE_ROOT",
                "LOGOS_CPP_SDK_ROOT"):
        if not env.get(var):
            r.blocked_on("--rebuild needs %s; see docs/basecamp.md for the four "
                   "pinned checkouts. Refusing to report a pass without having "
                   "rebuilt anything." % var)
            return

    build = os.path.join(repo, build_dir)
    if not os.path.isfile(os.path.join(build, "CMakeCache.txt")):
        r.blocked_on("--rebuild wants an already-configured build directory at %s "
               "(cmake -S module -B %s ...; docs/basecamp.md carries the flags, "
               "including the Qt prefix — Qt 6.9.2 is a CEILING, and a plugin "
               "built against a newer Qt reports a successful load and then "
               "times out)" % (build_dir, build_dir))
        return

    print("  <-    rebuilding into %s" % build_dir)
    proc = subprocess.run(["cmake", "--build", build, "-j8"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        r.blocked_on("the rebuild failed; the last lines were:\n%s"
               % "\n".join(proc.stdout.splitlines()[-15:]))
        return

    built = None
    for ext in ("dylib", "so", "dll"):
        cand = os.path.join(build, "modules", "agent_plugin." + ext)
        if os.path.isfile(cand):
            built = cand
            break
    if built is None:
        r.blocked_on("the rebuild produced no agent_plugin.{dylib,so,dll} under %s"
               % os.path.join(build_dir, "modules"))
        return

    with open(built, "rb") as fh:
        fresh = fh.read()
    fresh_hash = sha256_bytes(fresh)
    for variant, blob in sorted(binaries.items()):
        if blob == fresh:
            r.ok("a rebuild of the committed source is byte-identical to the "
                 "%s binary in the package (sha256 %s)"
                 % (variant, fresh_hash[:16]))
        else:
            r.fail("a rebuild of the committed source is NOT the %s binary in "
                   "the package: %d bytes hashing %s, against %d bytes hashing "
                   "%s. Either the package is stale, or this toolchain is not "
                   "the one docs/basecamp.md pins — check `otool -L %s` for Qt "
                   "6.9.2 through @rpath before believing the first."
                   % (variant, len(fresh), fresh_hash[:16], len(blob),
                      sha256_bytes(blob)[:16], os.path.relpath(built, repo)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", default=None,
                    help="repository root (default: the parent of scripts/)")
    ap.add_argument("--rebuild", metavar="BUILDDIR", default=None,
                    help="also rebuild from source and compare; needs the "
                         "toolchain from docs/basecamp.md and fails loudly "
                         "without it")
    args = ap.parse_args()
    repo = args.repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.exit(check(repo, args.rebuild))


if __name__ == "__main__":
    main()
