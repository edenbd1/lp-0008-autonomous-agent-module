#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""The committed package was built from the committed source.

`module/agent.lgx` is a binary artefact built from `module/src`. Nothing about a
binary says what source it came from, so twice now source changed, nothing
repackaged, and the published artefact did not contain the fix its own commit
claimed to make:

  1. A rebase left the artefact built from source no longer in the tree: the
     `agent.lgx` packaged at 3322142 stayed committed across five commits to
     `module/src`, so the downloadable module was missing content-topic
     validation, the owner-channel hardening, `program.call`'s flag checking and
     task persistence, while README §7 offered it as the loadable asset.

  2. 524866c stopped the module signing Agent Cards `alg: EdDSA` — an algorithm
     `scripts/use-cases/verify-agent-card.py`, this repository's own verifier,
     rejects with `unexpected algorithm 'EdDSA'` — and started signing them
     `secp256k1-bip340`. The source was fixed. The package was not rebuilt. So
     anyone installing the published `.lgx` got exactly the card this
     repository's own tooling refuses. Nothing caught it; it was found by
     counting strings in the two binaries:

         committed package : secp256k1-bip340 -> 0    EdDSA -> 1
         build of HEAD     : secp256k1-bip340 -> 1    EdDSA -> 0

     Note what could not have caught it. Both binaries are 3699040 bytes and
     both load, cast, and answer `skills()` with all 22 entries. A size check, a  (count-as-it-was)
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
      would have caught defect 2 above on its own: the source at 524866c says
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

AND THE THIRD COMMITTED BINARY, WHICH HAD NONE OF THIS.

`app/agent-ui.lgx` is the owner-facing package — the window the usability
criterion is evidenced by — and it was committed as ~182 kB of binary with no
record beside it at all. Nothing recomputed a hash of it, nothing read a string
out of it, and `.github/workflows/ci.yml` did not contain the substring `app/`
even once. Every sentence in the two paragraphs at the top of this file is
therefore true of the owner console today, with nothing at all standing between
it and the next rebase: source moves, the package does not, and the artefact a
reviewer installs is the one that was correct some commits ago.

So the same two layers are run over it, from the same helpers rather than from a
second implementation, plus one property the module package never asserted and
this one has to. See check_app_package() for what is and is not claimed.

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
import struct
import subprocess
import sys
import tarfile

# The package is a gzipped tar — `lgx create` writes tar.gz with a .lgx
# extension. It is not a zip, and reading it as one gets BadZipFile, which reads
# like a corrupt package and is not.
PACKAGE = "module/agent.lgx"
RECORD = "module/agent.lgx.sources"

# The second committed binary, and it arrived for the same reason the first one
# did: `scripts/logos-core-headless.sh` used to COMPILE the harness that drives
# Logos Core, which made a C++17 compiler, Qt 6.9.2, a logos-cpp-sdk checkout and
# nlohmann/json prerequisites of *running* the deployment command. It is now
# built once per variant and committed — and a committed binary is a claim about
# a build, which is the thing this file exists to stop anyone having to take on
# trust. Same two layers, same source-literal corroboration, plus three
# properties that are specific to an executable somebody else will run:
# it must be for the architecture its variant names, it must carry no rpath
# (every rpath this build can offer names a directory on the machine that
# compiled it), and there must be one for every variant the package ships a
# plugin for — otherwise "on any machine" has a hole exactly where nobody looks.
HARNESS_DIR = "module/harness"
HARNESS_RECORD = "module/harness/harness.sources"
HARNESS_SOURCE = "module/tests/logos_core_load_test.cpp"
HARNESS_NAME = "logos_core_load_test"

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

# HOW MANY LITERALS THE SCANNER HAS TO FIND BEFORE "they are all in the binary"
# IS A SENTENCE ABOUT THE BINARY.
#
# Layer 2 answers by building a list of the literals that are missing and asking
# whether it is empty, so a scanner that extracts nothing produces an empty list
# and corroborates every binary in the world. The floor is what makes that
# distinguishable from a clean run, and it is a shared constant rather than a
# number typed at each site because the same corroboration is run in three
# places — here, and in the two record writers, which bless a sibling binary on
# exactly this evidence and then write the record CI reads back. One of those
# three had the floor, one had it and said in its own comment that it had been
# measured going wrong without it, and one had neither. A floor that only some
# of the callers apply is not a floor.
#
# The numbers are the counts a healthy tree produces with a wide margin: the
# module's sources yield ~750 literals of >= MIN_LITERAL bytes and the harness's
# single translation unit yields ~146.
SOURCE_LITERAL_FLOOR = 300
HARNESS_LITERAL_FLOOR = 60

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

# --------------------------------------------------------------------------
# THE OWNER CONSOLE'S PACKAGE, which is the one that had no record.
#
# Same names, same shapes, same helpers — a second set of constants and not a
# second machinery, because the only thing that differs between the two packages
# is which files go in and which .lgx comes out.
#
# `.json` and `.txt` are deliberately NOT in APP_SOURCE_SUFFIXES, and this is the
# one place the two packages' definitions could not be identical. app/
# CMakeLists.txt line 49 does
#
#     configure_file(app/metadata.json app/src/metadata.json COPYONLY)
#
# because moc resolves Q_PLUGIN_METADATA(FILE "metadata.json") relative to the
# header that declares the plugin. That copy is a build product: it is in
# .gitignore, it exists on a machine that has built and on no other, and walking
# app/src for `.json` would put it in the record written by the packaging machine
# (where it is present) and out of the inventory computed on a CI checkout (where
# it is not). The two would then disagree by exactly one file and this checker
# would report "app/src/metadata.json was a build input and is gone" on a tree
# where nothing is wrong — a gate that cries wolf, which is the one failure mode
# the scanner at the top of this file was written out longhand to avoid.
#
# Nothing goes unhashed by leaving it out: app/metadata.json, the file those
# bytes are copied FROM, is in APP_SOURCE_FILES, and the copy is asserted to be a
# copy in check_app_package() whenever the machine running this has one.
APP_PACKAGE = "app/agent-ui.lgx"
APP_RECORD = "app/agent-ui.lgx.sources"
APP_SOURCE_DIRS = ("app/src",)
APP_SOURCE_FILES = ("app/CMakeLists.txt", "app/metadata.json")
APP_SOURCE_SUFFIXES = (".h", ".cpp", ".hpp", ".cc")
APP_METADATA = "app/metadata.json"
APP_GENERATED_METADATA = "app/src/metadata.json"

# Measured on the committed package: app/src + app/CMakeLists.txt +
# app/metadata.json yield 111 literals of >= MIN_LITERAL bytes (109 from
# agent_console.cpp, 2 from plugin.h), and all 111 are present in all three
# shipped binaries. The floor is the same shape as HARNESS_LITERAL_FLOOR and set
# well under the measurement, because literals come and go with ordinary edits
# and only "essentially none" is a defect.
APP_LITERAL_FLOOR = 60

# The record must not be truncated to nothing. Six build inputs today; the
# comparison that actually catches a truncated record is the file-set difference
# below, and this is the floor under the case where the *inventory definition*
# stops matching the tree — in which case both sides go empty together, agree
# perfectly, and mean nothing.
APP_MIN_INPUTS = 5

# Empty, and that is a measurement rather than an omission. The module needs one
# exclusion because moc inlines metadata.json's contents and never emits the path
# as data; the owner console declares Q_PLUGIN_METADATA the same way in
# app/src/plugin.h and the 13 bytes `metadata.json` ARE in all three shipped
# binaries — checked, not assumed. An empty allow-list is the strongest state
# this can be in, so it is written down as one and the hygiene rule below still
# applies to it: an entry that is not needed is an error.
APP_LITERAL_EXCLUSIONS = {}

# Qt is a CEILING, not a floor: Basecamp 0.2.2 bundles 6.9.2 and Qt refuses any
# plugin whose minor exceeds the host's — and docs/basecamp.md measures what that
# then looks like, which is a load that reports success and then times out on
# every call. app/package-ui.sh asserts this at packaging time, on the platform
# doing the packaging, out of otool or readelf. Neither of those exists on the
# other platform, and two of the three shipped variants were built somewhere
# else, so on the machine that ships the package two of the three claims were
# nobody's. Read here out of the binaries' own bytes instead, with no toolchain:
# a Mach-O carries the Qt it linked in LC_LOAD_DYLIB's current_version, an ELF
# carries it as the `Qt_6.9` symbol version it needs from libQt6Core.
APP_QT_CEILING = (6, 9)


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path):
    with open(path, "rb") as fh:
        return sha256_bytes(fh.read())


def source_inventory(repo, dirs=SOURCE_DIRS, files=SOURCE_FILES,
                     suffixes=SOURCE_SUFFIXES):
    """Every build input, as {repo-relative path: sha256}.

    The arguments default to the module's inventory so that every existing
    caller — scripts/write-package-record.py, and the block in ci.yml's `package`
    job that execs this file's constants to count them — keeps meaning exactly
    what it meant. The owner console passes its own three, for the reason written
    beside APP_SOURCE_SUFFIXES: one directory of the two packages holds a
    generated file and the other does not, and that is the whole difference.
    """
    found = {}
    for rel in files:
        p = os.path.join(repo, rel)
        if os.path.isfile(p):
            found[rel] = sha256_file(p)
    for d in dirs:
        base = os.path.join(repo, d)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.endswith(suffixes):
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


# --------------------------------------------------------------------------
# What an executable says about itself, read out of its own bytes.
#
# Written here, in struct.unpack, rather than shelled out to readelf/otool, for
# the reason the whole harness change exists: this check has to run on a machine
# that has no toolchain, and `otool` does not exist on Linux while `readelf`
# does not exist on a stock macOS. A binary that can only be checked where
# binutils is installed is a binary nobody checks.
#
# Three questions, and each one is a defect this repository has already met in
# another form:
#
#   arch    — the variant directory says `linux-arm64`; is the file inside it
#             actually aarch64? The install path made exactly this mistake with
#             the plugin (`tar tzf | head -n1` installed the *dylib* on Linux),
#             and a wrong-architecture executable fails with `cannot execute
#             binary file`, which reads like a corrupt download.
#   rpath   — a shipped binary that carries `-rpath /Users/<someone>/logos/Qt`
#             works on one computer and is a lie everywhere else. It must have
#             none: the run step supplies the app's own Qt through the
#             environment.
#   needs   — which libraries and which symbol versions. This is what says
#             whether "any machine that can run Logos Core can run this" is a
#             measurement or a hope.
# --------------------------------------------------------------------------

_ELF_MACHINE = {0x3E: "x86-64", 0xB7: "aarch64", 0x28: "arm", 0xF3: "riscv"}
_MACHO_CPU = {0x0100000C: "arm64", 0x01000007: "x86-64"}


def _elf_shape(blob):
    if blob[4] != 2 or blob[5] != 1:
        raise ValueError("only 64-bit little-endian ELF is understood here")
    machine = struct.unpack_from("<H", blob, 0x12)[0]
    phoff, = struct.unpack_from("<Q", blob, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", blob, 0x36)
    loads, dyn = [], None
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, = struct.unpack_from("<I", blob, off)
        p_offset, p_vaddr = struct.unpack_from("<QQ", blob, off + 0x08)
        p_filesz, = struct.unpack_from("<Q", blob, off + 0x20)
        if p_type == 1:                      # PT_LOAD
            loads.append((p_vaddr, p_filesz, p_offset))
        elif p_type == 2:                    # PT_DYNAMIC
            dyn = (p_offset, p_filesz)

    def at(vaddr):
        for vstart, size, off in loads:
            if vstart <= vaddr < vstart + size:
                return off + (vaddr - vstart)
        raise ValueError("address 0x%x is in no PT_LOAD segment" % vaddr)

    shape = {"format": "ELF", "arch": _ELF_MACHINE.get(machine, hex(machine)),
             "rpaths": [], "needed": [], "version_needs": {},
             # ELF has no per-library version field; the analogue is the symbol
             # version an object needs from its dependency, which is
             # `version_needs` above. Present and empty so both readers answer
             # the same questions with the same keys.
             "dylib_versions": {}}
    if dyn is None:
        return shape
    entries, strtab, verneed, verneednum = [], None, None, 0
    off, end = dyn[0], dyn[0] + dyn[1]
    while off + 16 <= end:
        tag, val = struct.unpack_from("<qQ", blob, off)
        off += 16
        if tag == 0:                         # DT_NULL
            break
        entries.append((tag, val))
        if tag == 5:
            strtab = val
        elif tag == 0x6FFFFFFE:
            verneed = val
        elif tag == 0x6FFFFFFF:
            verneednum = val

    def s(offset):
        base = at(strtab) + offset
        return blob[base:blob.index(b"\0", base)].decode("utf-8", "replace")

    if strtab is not None:
        for tag, val in entries:
            if tag == 1:
                shape["needed"].append(s(val))
            elif tag in (15, 29):            # DT_RPATH, DT_RUNPATH
                shape["rpaths"].extend(s(val).split(":"))
    if verneed is not None and strtab is not None:
        vn = at(verneed)
        for _ in range(verneednum):
            _ver, cnt, file_off, aux, nxt = struct.unpack_from("<HHIII", blob, vn)
            lib = s(file_off)
            a = vn + aux
            for _i in range(cnt):
                _hash, _flags, _other, name, anext = struct.unpack_from(
                    "<IHHII", blob, a)
                shape["version_needs"].setdefault(lib, []).append(s(name))
                if not anext:
                    break
                a += anext
            if not nxt:
                break
            vn += nxt
    for lib in shape["version_needs"]:
        shape["version_needs"][lib] = sorted(set(shape["version_needs"][lib]))
    return shape


def _macho_shape(blob):
    magic, = struct.unpack_from("<I", blob, 0)
    if magic in (0xCAFEBABE, 0xBEBAFECA):
        raise ValueError("this is a fat Mach-O; the shipped harness is thin")
    if magic != 0xFEEDFACF:
        raise ValueError("not a 64-bit little-endian Mach-O")
    cputype, = struct.unpack_from("<I", blob, 4)
    ncmds, = struct.unpack_from("<I", blob, 16)
    shape = {"format": "Mach-O", "arch": _MACHO_CPU.get(cputype, hex(cputype)),
             "rpaths": [], "needed": [], "version_needs": {},
             "dylib_versions": {}}
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", blob, off)
        if cmd == 0x8000001C:                # LC_RPATH
            p, = struct.unpack_from("<I", blob, off + 8)
            raw = blob[off + p:off + cmdsize]
            shape["rpaths"].append(raw.split(b"\0")[0].decode("utf-8", "replace"))
        elif cmd in (0x0C, 0x8000001F):      # LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB
            p, = struct.unpack_from("<I", blob, off + 8)
            raw = blob[off + p:off + cmdsize]
            name = raw.split(b"\0")[0].decode("utf-8", "replace")
            shape["needed"].append(name)
            # struct dylib is (name offset, timestamp, current_version,
            # compatibility_version), each a u32, and current_version is the
            # library's own version packed X.Y.Z as xxxx.yy.zz. It is the only
            # place a Mach-O records WHICH Qt it linked against: the install name
            # is `@rpath/QtCore.framework/Versions/A/QtCore` on 6.9 and on 6.11
            # alike. Read for the ceiling check in check_app_package().
            cur, = struct.unpack_from("<I", blob, off + 16)
            shape["dylib_versions"][name] = (cur >> 16, (cur >> 8) & 0xFF,
                                             cur & 0xFF)
        off += cmdsize
    return shape


def binary_shape(blob):
    """{format, arch, rpaths, needed, version_needs} for an ELF or a Mach-O."""
    if blob[:4] == b"\x7fELF":
        return _elf_shape(blob)
    return _macho_shape(blob)


# What each variant's name asserts about the file inside it. A dict rather than
# a parse of the name, so a new variant has to be added here deliberately: an
# unknown variant is a failure below, not a check that quietly passes.
VARIANT_ARCH = {
    "darwin-arm64": ("Mach-O", "arm64"),
    "linux-amd64": ("ELF", "x86-64"),
    "linux-arm64": ("ELF", "aarch64"),
}


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

    # WHICH VARIANTS THE PACKAGE ACTUALLY SHIPS, read out of the package and not
    # out of the record.
    #
    # Everything below used to iterate the RECORD and nothing ever iterated the
    # package, so the one direction that matters for a stranger installing this
    # file — "the .lgx offers me a plugin for my platform; was that plugin ever
    # checked?" — was not asked. A variant present in the package and absent
    # from the record passed every assertion in this file by not being looked
    # at, which is the same defect as a check that finds nothing and calls it a
    # pass. check_harness() has had the mirror image of this check all along
    # ("an unrecorded binary is one nobody can check"); the package side did
    # not.
    #
    # Measured on a copy of the tree: a `variants/linux-riscv64/agent_plugin.so`
    # holding 53 bytes of prose, added to the tarball and advertised in
    # manifest.json's `main`, produced no failure, no mention, and the line
    # "every variant the package ships a plugin for has a harness beside it:
    # darwin-arm64, linux-amd64, linux-arm64" — a sentence about the package
    # that was measuring the record.
    #
    # Both readings are taken, because they answer different questions: the
    # member paths are what a host unpacking the tarball finds on disk, and
    # `manifest.json`'s `main` is what it is told to load. A variant in either
    # is a variant this package offers somebody.
    shipped_variants = {n.split("/")[1] for n in members
                        if n.startswith("variants/") and len(n.split("/")) > 2}
    shipped_variants |= set(manifest.get("main") or {})
    for variant in sorted(shipped_variants - set(recorded_variants)):
        r.fail("the package ships a %s variant that %s says nothing about, so "
               "none of the checks below ever read it: not its sha256, not its "
               "source literals, and not whether a harness exists for the "
               "platform it invites somebody to install on. Repackage that "
               "variant, or delete it from the .lgx." % (variant, RECORD))

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
        # The host reads the PACKAGED metadata.json — the name, the type and the
        # IID it loads the plugin under all come from that copy — so it has to be
        # this repository's copy. `if meta_member in members:` made the one case
        # where the host reads NOTHING the case this check said nothing about:
        # measured by rebuilding the tarball without
        # `variants/darwin-arm64/metadata.json`, which removed one `ok` line from
        # the output and left the run green. An absence is not a match, and it
        # is a worse fault than a mismatch, because a variant with no metadata is
        # one Basecamp cannot describe at all.
        meta_member = "variants/%s/metadata.json" % variant
        if meta_member not in members:
            r.fail("%s is not in the package. Every variant carries its own copy "
                   "of module/metadata.json and the host reads the packaged one, "
                   "so a variant without it is one that ships no name, type or "
                   "IID." % meta_member)
        elif members[meta_member] == open(
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
            # NAME ONLY WHAT COULD BE A PACKAGE HASH. This listed every
            # 64-hex string in the document, which on a repackage means the
            # message hands you a chain transaction beside the stale root and
            # invites you to "update" it -- one agent was told to change
            # `ed8c3514...`, the settlement in block 9477, and rightly refused.
            # A failure message that suggests falsifying a chain citation to
            # satisfy a docs check is a bad message.
            # A WINDOW, not a line. The citation reads "That prints root hash"
            # and then the hash on the NEXT line, because it is prose that
            # wraps -- a same-line match found nothing and would have reported
            # "none", which is worse than the over-broad list it replaced.
            lines = text.splitlines()
            near = []
            for i, line in enumerate(lines):
                window = " ".join(lines[max(0, i - 2):i + 1])
                if not re.search(r"(?i)root hash|package hash|agent\.lgx", window):
                    continue
                near += re.findall(r"\b[0-9a-f]{64}\b", line)
            stale = sorted(set(near))
            r.fail("docs/basecamp.md does not quote this package's root hash "
                   "(%s); the hash(es) it cites for the package are %s. The "
                   "document is describing a package that is no longer the one "
                   "committed. Other 64-hex strings in that file are NOT "
                   "candidates -- several are transactions on chain."
                   % (got_root[:16], ", ".join(h[:16] for h in stale) or "none"))

    # ---- layer 1: the record describes THIS source ------------------------
    on_disk = source_inventory(repo)
    recorded = record.get("built_from") or {}
    if len(recorded) < 15:
        # The comparison figure is COUNTED, not remembered. This message used to
        # say "module/src alone has 20"; module/src holds 25 today and the whole
        # inventory holds 31, so the one number a reader would have anchored on
        # was the stale one — in the text of a failure, where it is least likely
        # to be re-checked and most likely to send somebody to the wrong file.
        r.fail("the record lists only %d build inputs; this tree has %d, "
               "so the record is truncated and would agree with almost anything"
               % (len(recorded), len(on_disk)))

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
    if len(lits) < SOURCE_LITERAL_FLOOR:
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

    package_failures = len(r.failures)
    package_banner = (
        "OK the shipped package was built from the source in this commit: "
        "%d build input(s) recorded and unchanged, %d literal(s) found in the "
        "shipped binary" % (len(on_disk), len(lits) - len(LITERAL_EXCLUSIONS)))

    # ---- and the other committed binary -----------------------------------
    #
    # The variants handed on are the ones the PACKAGE ships, not the ones the
    # record names. check_harness()'s coverage clause is a sentence about the
    # package — "every variant the package ships a plugin for needs a harness" —
    # and it was being answered from the record, so a variant the record had
    # never heard of could not create a hole in it. Being unrecorded is already
    # a failure above; it must not also buy exemption from this one.
    print()
    check_harness(repo, r, shipped_variants | set(binaries))
    harness_failures = len(r.failures) - package_failures

    # ---- and the package the owner installs --------------------------------
    #
    # Third, and last to arrive: `app/agent-ui.lgx` is the package a reviewer
    # installs to get a window, and it had no record at all. Its own banner, for
    # the same reason the harness has one — a single "OK" covering all three
    # would go green on the first two the first time this section was skipped.
    print()
    app_counts = check_app_package(repo, r)
    app_failures = len(r.failures) - package_failures - harness_failures

    print()
    if r.blocked and len(r.blocked) == len(r.failures):
        print("%d failure(s): the check could not be completed, which is not a "
              "pass" % len(r.failures))
        return 1
    if package_failures:
        print("%d failure(s): the committed package does not agree with the "
              "committed source" % package_failures)
    else:
        print(package_banner)
    if harness_failures:
        print("%d failure(s): the committed harness does not agree with the "
              "committed source" % harness_failures)
    else:
        print("OK the shipped harness was built from the source in this commit: "
              "%d variant(s), each for the architecture it names, each carrying "
              "no rpath, and every literal of %s in each"
              % (len(record_variant_count(repo)), HARNESS_SOURCE))
    if app_failures or app_counts is None:
        print("%d failure(s): the committed owner console does not agree with "
              "the committed source" % app_failures)
    else:
        print("OK the shipped owner console was built from the source in this "
              "commit: %d build input(s) recorded and unchanged, %d variant(s), "
              "each for the architecture it names, and %d literal(s) found in "
              "each of them"
              % (app_counts["inputs"], app_counts["variants"],
                 app_counts["literals"]))
    return 1 if r.failures else 0


def record_variant_count(repo):
    """The variants the harness record names, for the banner. Empty if there is
    no record — in which case check_harness has already failed and the banner is
    not printed."""
    path = os.path.join(repo, HARNESS_RECORD)
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        return json.load(fh).get("variants") or {}


# The floor upstream's own libraries set, measured out of the published AppImage
# with the reader above rather than taken from a changelog:
#
#   liblogos_core.so        GLIBC_2.38   GLIBCXX_3.4.29   CXXABI_1.3.15
#   libQt6Core.so.6.9.2                                   CXXABI_1.3.15
#   libQt6Positioning.so                 GLIBCXX_3.4.32
#   libsystemd.so.0         GLIBC_2.39
#
# So a machine that can run Logos Core on Linux already has at least these. The
# shipped harness must not need MORE than the runtime it drives — if it did,
# "any machine that can run Logos Core can run this" would be false, and it
# would be false silently, on somebody else's distribution, months from now.
# Measured on the committed binaries, both architectures:
# GLIBC_2.38, GLIBCXX_3.4.29, CXXABI_1.3.9.
LINUX_SYMBOL_FLOOR = {
    "GLIBC_": (2, 38),
    "GLIBCXX_": (3, 4, 32),
    "CXXABI_": (1, 3, 15),
}


def _version_tuple(name, prefix):
    try:
        return tuple(int(p) for p in name[len(prefix):].split("."))
    except ValueError:
        return None


def check_harness(repo, r, package_variants):
    """The committed harness binaries were built from the committed source.

    Same two layers as the package — a record that is recomputed, and the
    binary's own bytes read back for the source's string literals — plus what
    is specific to an executable this repository asks a stranger to run: the
    architecture its variant claims, no rpath, a dependency floor no higher than
    the runtime's own, and one harness for every variant the package ships a
    plugin for.
    """
    src_path = os.path.join(repo, HARNESS_SOURCE)
    rec_path = os.path.join(repo, HARNESS_RECORD)
    if not os.path.isfile(src_path):
        r.fail("%s is missing; it is what the shipped harness is built from"
               % HARNESS_SOURCE)
        return
    if not os.path.isfile(rec_path):
        r.fail("%s is missing, so no committed harness can be checked against "
               "the source it came from. scripts/logos-core-headless.sh refuses "
               "to run an unrecorded one, which means the deployment command "
               "needs a compiler again." % HARNESS_RECORD)
        return
    with open(rec_path, encoding="utf-8") as fh:
        record = json.load(fh)

    # ---- layer 1: the record describes THIS source ------------------------
    on_disk = {HARNESS_SOURCE: sha256_file(src_path)}
    recorded = record.get("built_from") or {}
    if set(recorded) != set(on_disk):
        r.fail("the harness record's build inputs are %s; the source tree's are "
               "%s" % (sorted(recorded), sorted(on_disk)))
    for path, digest in sorted(on_disk.items()):
        if recorded.get(path) == digest:
            r.ok("the shipped harness was recorded against this %s (sha256 %s)"
                 % (path, digest[:16]))
        else:
            r.fail("%s has changed since the harness was built (%s -> %s). "
                   "Rebuild it on each platform: "
                   "./scripts/logos-core-headless.sh --build-harness"
                   % (path, str(recorded.get(path))[:12], digest[:12]))

    # ---- the file set: a binary nobody recorded is not shipped ------------
    hdir = os.path.join(repo, HARNESS_DIR)
    on_disk_variants = set()
    if os.path.isdir(hdir):
        for entry in sorted(os.listdir(hdir)):
            if os.path.isdir(os.path.join(hdir, entry)):
                on_disk_variants.add(entry)
    recorded_variants = set(record.get("variants") or {})
    for extra in sorted(on_disk_variants - recorded_variants):
        r.fail("%s/%s holds a harness the record says nothing about; an "
               "unrecorded binary is one nobody can check"
               % (HARNESS_DIR, extra))
    for gone in sorted(recorded_variants - on_disk_variants):
        r.fail("the record names a %s harness and there is no %s/%s directory"
               % (gone, HARNESS_DIR, gone))

    # ---- every variant the package ships a plugin for needs one -----------
    #
    # This is the criterion's own clause, as an assertion. A package that can be
    # installed on a platform whose harness was never built is a platform where
    # the deployment command silently needs a compiler again.
    hole = sorted(package_variants - recorded_variants)
    if hole:
        r.fail("module/agent.lgx ships a plugin for %s and there is no harness "
               "for it, so on that platform the deployment command falls back "
               "to compiling one" % ", ".join(hole))
    elif package_variants:
        r.ok("every variant the package ships a plugin for has a harness beside "
             "it: %s" % ", ".join(sorted(recorded_variants)))

    # ---- layer 1 + the executable's own bytes -----------------------------
    lits = literals(repo, [HARNESS_SOURCE])
    if len(lits) < HARNESS_LITERAL_FLOOR:
        r.fail("only %d literals of >= %d bytes were extracted from %s; the "
               "scanner found essentially nothing and would agree with any "
               "binary" % (len(lits), MIN_LITERAL, HARNESS_SOURCE))

    for variant, info in sorted((record.get("variants") or {}).items()):
        rel = "%s/%s/%s" % (HARNESS_DIR, variant, HARNESS_NAME)
        path = os.path.join(repo, rel)
        if not os.path.isfile(path):
            r.fail("the record names a %s harness and %s is not there"
                   % (variant, rel))
            continue
        with open(path, "rb") as fh:
            blob = fh.read()
        got = sha256_bytes(blob)
        if got == info.get("sha256") and len(blob) == info.get("bytes"):
            r.ok("%s is the binary the record was written for (sha256 %s, %d "
                 "bytes)" % (rel, got[:16], len(blob)))
        else:
            r.fail("%s hashes to %s (%d bytes), the record says %s (%s bytes): "
                   "the committed harness is not the one that was recorded"
                   % (rel, got[:16], len(blob), str(info.get("sha256"))[:16],
                      info.get("bytes")))

        # It has to be runnable. A mode bit is easy to lose — `git` keeps only
        # the execute bit, and a harness without it fails with "Permission
        # denied" in the middle of a deployment.
        if not os.access(path, os.X_OK):
            r.fail("%s is not executable; the deployment command cannot run it"
                   % rel)

        try:
            shape = binary_shape(blob)
        except Exception as exc:              # noqa: BLE001 - reported, not raised
            r.fail("%s could not be read as an executable: %s" % (rel, exc))
            continue

        want = VARIANT_ARCH.get(variant)
        if want is None:
            r.fail("%s is a variant this check does not know the architecture "
                   "of; add it to VARIANT_ARCH deliberately rather than letting "
                   "it through" % variant)
        elif (shape["format"], shape["arch"]) == want:
            r.ok("%s really is %s %s, which is what the variant name claims"
                 % (rel, shape["format"], shape["arch"]))
        else:
            r.fail("%s is %s %s and the variant name says %s %s — a binary for "
                   "another machine, which fails at exec time with a message "
                   "about the file format rather than about the platform"
                   % (rel, shape["format"], shape["arch"], want[0], want[1]))

        if shape["rpaths"]:
            r.fail("%s carries rpath(s) %s. Every one of them names a directory "
                   "on the machine that compiled it; a shipped binary must find "
                   "Qt and the runtime through the environment "
                   "scripts/logos-core-headless.sh sets, out of the Logos app "
                   "itself" % (rel, ", ".join(shape["rpaths"])))
        else:
            r.ok("%s carries no rpath: nothing in it names the machine it was "
                 "built on" % rel)

        # What it is linked to, and the two platforms differ here for a reason
        # the harness's own header explains.
        #
        # Qt on both: the runtime assumes its host application already created a
        # QCoreApplication before logos_core_init, and the harness is that host.
        #
        # `liblogos_core` on ELF only. On Mach-O it is dlopen'd from the app by
        # path — "what runs is the shipped binary, not a rebuild of it" — and
        # `-undefined dynamic_lookup` defers the rest. There is no ELF
        # equivalent of that for an executable, so the Linux link is real, which
        # is asserted here rather than remembered. A darwin harness that HAD
        # linked it would be one that needs the app at its link-time path.
        needed = " ".join(shape["needed"])
        qt_soname = "Qt6Core" if shape["format"] == "ELF" else "QtCore"
        if qt_soname in needed:
            r.ok("%s links Qt, which is what creates the QCoreApplication the "
                 "runtime assumes its host already made" % rel)
        else:
            r.fail("%s does not link Qt; its dependencies are %s"
                   % (rel, ", ".join(shape["needed"]) or "none"))
        if shape["format"] == "ELF":
            if "logos_core" in needed:
                r.ok("%s links liblogos_core, as an ELF harness must: "
                     "`-undefined dynamic_lookup` is Mach-O only" % rel)
            else:
                r.fail("%s does not link liblogos_core; its dependencies are %s"
                       % (rel, ", ".join(shape["needed"]) or "none"))
        elif "logos_core" in needed:
            r.fail("%s links liblogos_core at a fixed path. The darwin harness "
                   "dlopen's the runtime out of the app instead, so that what "
                   "runs is the binary the app shipped and not a rebuild of it: "
                   "%s" % (rel, ", ".join(shape["needed"])))
        else:
            r.ok("%s does not link liblogos_core: it dlopen's the runtime out "
                 "of the app, by path, at run time" % rel)

        if shape["format"] == "ELF":
            worst = {}
            unreadable = []
            for lib, versions in sorted(shape["version_needs"].items()):
                for v in versions:
                    for prefix, floor in LINUX_SYMBOL_FLOOR.items():
                        if not v.startswith(prefix):
                            continue
                        got_v = _version_tuple(v, prefix)
                        if got_v is None:
                            # A `GLIBC_2.38` this reader cannot turn into
                            # numbers is not a version below the floor, it is a
                            # version nobody compared. Silently skipping it made
                            # the strongest claim about this binary depend on
                            # the reader's own parser succeeding, so it is
                            # reported instead.
                            unreadable.append("%s (in %s)" % (v, lib))
                            continue
                        if got_v > worst.get(prefix, (0,)):
                            worst[prefix] = got_v
                        if got_v > floor:
                            r.fail("%s needs %s, and the Logos Core runtime "
                                   "itself needs only %s%s. A machine that can "
                                   "run Logos Core would not be able to run "
                                   "this harness, which is the whole claim."
                                   % (rel, v, prefix,
                                      ".".join(str(n) for n in floor)))
            floor_text = ", ".join(
                "%s%s" % (p, ".".join(str(n) for n in v))
                for p, v in sorted(LINUX_SYMBOL_FLOOR.items()))
            # THE FLOOR CHECK'S OWN FLOOR, and without it the loop above is a
            # loop over whatever the ELF reader managed to find — which for an
            # empty `version_needs` is nothing at all, and a loop over nothing
            # passes.
            #
            # This is not hypothetical about a binary this repository does not
            # have: it was measured on the committed linux-amd64 harness, by
            # retagging its DT_VERNEED and DT_VERNEEDNUM entries to a tag this
            # reader ignores and leaving every other byte alone. The run stayed
            # green, printed "needs at most nothing" as a NOTE, and went on to
            # state in its closing banner that the harness "needs no newer
            # glibc/libstdc++ than liblogos_core.so itself" — a claim it had
            # just failed to read a single symbol version for. A note is what
            # this file uses for a measurement; it is not what an unperformed
            # measurement gets.
            #
            # Every C++ binary this toolchain produces needs versioned symbols
            # from glibc and libstdc++ — the committed pair need GLIBC_2.38,
            # GLIBCXX_3.4.29 and CXXABI_1.3.9 between them — so finding none is
            # a reader that did not work, not a binary that needs nothing.
            if unreadable:
                r.fail("%s declares symbol version(s) %s that this reader could "
                       "not turn into numbers, so they were compared against "
                       "nothing" % (rel, ", ".join(sorted(set(unreadable)))))
            if not worst:
                r.fail("no symbol version requirement could be read out of %s at "
                       "all, so \"it needs nothing newer than the runtime does\" "
                       "was asserted over an empty list. Every ELF this "
                       "toolchain builds needs versioned glibc and libstdc++ "
                       "symbols; a binary that appears to need none is a "
                       "DT_VERNEED this reader failed to find. The runtime's own "
                       "floor is %s." % (rel, floor_text))
            else:
                r.note("%s needs at most %s — the runtime's own floor is %s"
                       % (rel, ", ".join("%s%s" % (p, ".".join(str(n) for n in v))
                                         for p, v in sorted(worst.items())),
                          floor_text))

        # ---- layer 2: the shipped binary carries this source's strings ----
        missing = [(lit, sites) for lit, sites in sorted(lits.items())
                   if not in_binary(lit, blob)]
        if not missing:
            r.ok("every one of the %d literals in %s is in the %s harness"
                 % (len(lits), HARNESS_SOURCE, variant))
        else:
            r.fail("%d of %d literals from %s are absent from the %s harness, "
                   "so that binary was not built from this source:"
                   % (len(missing), len(lits), HARNESS_SOURCE, variant))
            for lit, sites in missing[:12]:
                print("          %-46r  %s" % (lit[:44], ", ".join(sites)))
            if len(missing) > 12:
                print("          ... and %d more" % (len(missing) - 12))


def qt_built_against(shape):
    """(major, minor), and where it was read from — or (None, why not).

    Two formats, one question. A Mach-O records the version of each library it
    linked in that library's LC_LOAD_DYLIB command; an ELF records the symbol
    version it needs from it, and Qt tags its symbols `Qt_6.9`. Neither is a
    string anybody typed into this repository, which is the point: the packaging
    script asks the platform's own tool at packaging time and cannot ask it about
    a variant built on another machine, and this reads all three out of the bytes
    that shipped.
    """
    best, where = None, None
    for name, ver in sorted(shape.get("dylib_versions", {}).items()):
        if "Qt" not in name:
            continue
        if best is None or ver[:2] > best:
            best, where = ver[:2], "%s current_version %s" % (
                name.rsplit("/", 1)[-1], ".".join(str(n) for n in ver))
    for lib, versions in sorted(shape.get("version_needs", {}).items()):
        if "Qt" not in lib:
            continue
        for v in versions:
            if not v.startswith("Qt_"):
                continue
            got = _version_tuple(v, "Qt_")
            if got is None or len(got) < 2:
                # `Qt_6` on its own is the major-only tag every Qt 6 object
                # needs; it says nothing about the minor and is not an answer.
                continue
            if best is None or got[:2] > best:
                best, where = got[:2], "%s needs %s" % (lib, v)
    if best is None:
        return None, ("no Qt version could be read out of it — a Mach-O should "
                      "carry one in LC_LOAD_DYLIB and an ELF as the Qt_x.y "
                      "symbol version it needs from libQt6Core")
    return best, where


def check_app_package(repo, r):
    """The committed owner console was built from the committed source.

    THE SAME TWO LAYERS AS module/agent.lgx, over the package that had neither.

      (1) `app/agent-ui.lgx.sources` is written by app/package-ui.sh at the
          moment the package is made and recomputed here: every build input's
          sha256, every shipped binary's sha256 and length, the manifest's
          content-derived root hash, and — this record has one the module's does
          not — the format and architecture of each variant's binary.

      (2) Every string literal of >= MIN_LITERAL bytes in app/src must be inside
          each of the three binaries the package ships. Same scanner, same
          8-byte floor, same reason: the record is a record, and a determined
          hand could rewrite it without rebuilding anything.

    AND ONE THING THE MODULE PACKAGE NEVER ASSERTED, which this one has to.
    `check_harness` reads a variant directory's binary and asks whether the file
    is the architecture the directory name claims, because the install path had
    already made exactly that mistake once. The package side never asked it of a
    plugin — and app/agent-ui.lgx ships three, two of them built on machines this
    one cannot reproduce, in a package whose install instructions (app/README.md)
    say in as many words that the variants "are not interchangeable" and that a
    directory holding the other platform's binary "looks complete and can never
    load". So it is asserted, from the binary's own header.

    WHAT IS NOT ASSERTED HERE, and why, because a check quietly not made is worse
    than one that is absent loudly:

      - No rebuild. Same as the module: see --rebuild and the note in ci.yml.
        Two of the three variants cannot be rebuilt on any one machine at all.

      - No rpath rule. `check_harness` refuses any rpath because the harness is
        an executable this repository asks a stranger to RUN, and every rpath it
        could carry names a directory on the machine that compiled it. A package
        plugin is not run, it is dlopen'd by Basecamp, and Qt is resolved through
        the loading process's own rpath — Basecamp's. The committed darwin-arm64
        console does carry `/Users/eden/logos/Qt/6.9.2/macos/lib`, left in by
        CMake's build-tree rpath, and it loads inside Basecamp on a machine with
        no such directory because the host had already loaded Qt. It is printed
        as a note below rather than passed over in silence, and it is not made a
        failure, because a failure here would be a claim about loading that this
        repository has measured to be false.

      - No document-citation check. docs/basecamp.md quotes module/agent.lgx's
        root hash and is held to it above; no document quotes this package's, so
        there is no stale citation to catch. scripts/check-docs.py already holds
        the prose to this package in the two ways it can be held — every variant
        it ships must be named by some document, and no document may claim a
        different number of them.
    """
    pkg_path = os.path.join(repo, APP_PACKAGE)
    rec_path = os.path.join(repo, APP_RECORD)
    if not os.path.isfile(pkg_path):
        r.fail("%s is missing; it is the package a reviewer installs to get a "
               "window at all" % APP_PACKAGE)
        return None
    if not os.path.isfile(rec_path):
        r.fail("%s is missing, so the committed owner console cannot be tied to "
               "any source. It is written by app/package-ui.sh at the moment the "
               "package is made: rebuild and repackage (app/README.md), or run "
               "scripts/write-package-record.py --package app --all-variants "
               "against the committed package." % APP_RECORD)
        return None

    manifest, members = read_package(pkg_path)
    with open(rec_path, encoding="utf-8") as fh:
        record = json.load(fh)

    # ---- layer 1: the record describes THIS package -----------------------
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

    # Read out of the PACKAGE, not out of the record, for the reason written at
    # the same point on the module side: a variant the record has never heard of
    # is one every check below skips by not iterating it, and it is exactly the
    # variant somebody is about to install.
    shipped_variants = {n.split("/")[1] for n in members
                        if n.startswith("variants/") and len(n.split("/")) > 2}
    shipped_variants |= set(manifest.get("main") or {})
    for variant in sorted(shipped_variants - set(recorded_variants)):
        r.fail("the package ships a %s console that %s says nothing about, so "
               "none of the checks below ever read it: not its sha256, not its "
               "architecture, and not its source literals. Repackage that "
               "variant, or delete it from the .lgx." % (variant, APP_RECORD))

    binaries = {}
    for variant, info in sorted(recorded_variants.items()):
        main = info.get("main")
        member = "variants/%s/%s" % (variant, main)
        if member not in members:
            r.fail("the record names %s, which is not in the package" % member)
            continue
        blob = members[member]
        binaries[variant] = blob
        got = sha256_bytes(blob)
        if got == info.get("sha256") and len(blob) == info.get("bytes"):
            r.ok("%s is the binary the record was written for (sha256 %s, %d "
                 "bytes)" % (member, got[:16], len(blob)))
        else:
            r.fail("%s hashes to %s (%d bytes), the record says %s (%s bytes): "
                   "the package holds a different build from the one that was "
                   "recorded" % (member, got[:16], len(blob),
                                 str(info.get("sha256"))[:16], info.get("bytes")))
        declared = manifest.get("main", {}).get(variant)
        if declared != main:
            r.fail("the manifest's main[%s] is %r, the record's is %r"
                   % (variant, declared, main))

        # The host reads the PACKAGED metadata.json — Basecamp learns the name,
        # the `ui` type that decides which directory it is installed into, and
        # the `agent` dependency that makes it load the core module, all from
        # that copy. An absence is a worse fault than a mismatch: a variant with
        # no metadata is one the app cannot describe or install at all.
        meta_member = "variants/%s/metadata.json" % variant
        if meta_member not in members:
            r.fail("%s is not in the package. Every variant carries its own copy "
                   "of app/metadata.json and the host reads the packaged one, so "
                   "a variant without it ships no name, no `ui` type and no "
                   "`agent` dependency." % meta_member)
        elif members[meta_member] == open(
                os.path.join(repo, APP_METADATA), "rb").read():
            r.ok("the metadata.json inside %s is %s" % (variant, APP_METADATA))
        else:
            r.fail("the metadata.json inside %s differs from %s — the host reads "
                   "the packaged copy" % (variant, APP_METADATA))

        # ---- the architecture the variant name claims ---------------------
        try:
            shape = binary_shape(blob)
        except Exception as exc:              # noqa: BLE001 - reported, not raised
            r.fail("%s could not be read as a shared object: %s" % (member, exc))
            continue

        want = VARIANT_ARCH.get(variant)
        seen = "%s %s" % (shape["format"], shape["arch"])
        if want is None:
            r.fail("%s is a variant this check does not know the architecture "
                   "of; add it to VARIANT_ARCH deliberately rather than letting "
                   "it through" % variant)
        elif (shape["format"], shape["arch"]) == want:
            r.ok("%s really is %s, which is what the variant name claims"
                 % (member, seen))
        else:
            r.fail("%s is %s and the variant name says %s — app/README.md tells "
                   "an installer to flatten one variant into the plugins "
                   "directory, and a directory holding another platform's binary "
                   "looks complete and can never load"
                   % (member, seen, " ".join(want)))

        # The record has to say the same thing the bytes do. Without this the
        # architecture above is checked against a constant in this file and the
        # record could claim anything at all about a binary it names.
        if info.get("format") != seen:
            r.fail("the record says %s is %r and its own header says %r"
                   % (member, info.get("format"), seen))

        # ---- the Qt it was built against, as a ceiling --------------------
        qt, where = qt_built_against(shape)
        if qt is None:
            r.fail("%s: %s. app/package-ui.sh refuses a plugin whose Qt cannot "
                   "be read, because a plugin built against a newer Qt than the "
                   "host's reports a successful load and then times out on every "
                   "call; a version nobody could read is not a version below the "
                   "ceiling." % (member, where))
        elif qt > APP_QT_CEILING:
            r.fail("%s was built against Qt %s (%s) and Basecamp 0.2.2 bundles "
                   "%s. Qt refuses any plugin whose minor exceeds the host's — "
                   "see docs/basecamp.md, which measures what that then looks "
                   "like." % (member, ".".join(str(n) for n in qt), where,
                              ".".join(str(n) for n in APP_QT_CEILING)))
        else:
            r.ok("%s was built against Qt %s, at or under the %s Basecamp "
                 "bundles (%s)" % (member, ".".join(str(n) for n in qt),
                                   ".".join(str(n) for n in APP_QT_CEILING),
                                   where))

        if shape["rpaths"]:
            r.note("%s carries rpath(s) %s. Not a failure here and it is one for "
                   "the harness: a package plugin is dlopen'd by Basecamp and "
                   "resolves Qt through the host's own rpath, which is why this "
                   "one loads on machines that have no such directory."
                   % (member, ", ".join(shape["rpaths"])))

    # ---- layer 1: the record describes THIS source ------------------------
    on_disk = source_inventory(repo, APP_SOURCE_DIRS, APP_SOURCE_FILES,
                               APP_SOURCE_SUFFIXES)
    if len(on_disk) < APP_MIN_INPUTS:
        r.fail("the inventory definition matched %d file(s) under %s; it is not "
               "seeing this tree, and a record compared against nothing agrees "
               "with everything"
               % (len(on_disk), ", ".join(APP_SOURCE_DIRS + APP_SOURCE_FILES)))
    recorded = record.get("built_from") or {}
    if len(recorded) < APP_MIN_INPUTS:
        r.fail("the record lists only %d build input(s); this tree has %d, so "
               "the record is truncated and would agree with almost anything"
               % (len(recorded), len(on_disk)))

    added = sorted(set(on_disk) - set(recorded))
    removed = sorted(set(recorded) - set(on_disk))
    changed = sorted(p for p in set(on_disk) & set(recorded)
                     if on_disk[p] != recorded[p])
    if not (added or removed or changed):
        r.ok("all %d build inputs hash exactly as they did when the console was "
             "packaged" % len(on_disk))
    else:
        for p in changed:
            r.fail("%s has changed since the console was packaged (%s -> %s)"
                   % (p, recorded[p][:12], on_disk[p][:12]))
        for p in added:
            r.fail("%s is a build input the console was never packaged from" % p)
        for p in removed:
            r.fail("%s was a build input and is gone; the package still "
                   "contains it" % p)
        print()
        print("        app/agent-ui.lgx is stale. Rebuild and repackage:")
        print("          cmake --build build-ui -j8 && app/package-ui.sh build-ui")
        print("        app/README.md carries the full build environment.")
        print()

    # The one build input that is generated, checked as one. app/CMakeLists.txt
    # copies app/metadata.json to app/src/metadata.json so moc can inline it, and
    # only the source of that copy is in the inventory above. On a machine that
    # has built, the copy is here and it has to still be a copy — a hand-edited
    # app/src/metadata.json is a plugin whose embedded manifest disagrees with
    # the one the package ships, which is a `ui` type in one place and something
    # else in the other.
    gen = os.path.join(repo, APP_GENERATED_METADATA)
    if not os.path.isfile(gen):
        r.note("%s is a build product (.gitignore) and this tree has none, so "
               "the copy moc inlines is only checkable where a build happened; "
               "the file it is copied from is build input %s above"
               % (APP_GENERATED_METADATA, APP_METADATA))
    elif sha256_file(gen) == sha256_file(os.path.join(repo, APP_METADATA)):
        r.ok("%s is still a byte-for-byte copy of %s, which is what moc inlines "
             "into the plugin" % (APP_GENERATED_METADATA, APP_METADATA))
    else:
        r.fail("%s is not a copy of %s. moc inlines the copy, `lgx` packages the "
               "original, and a plugin whose embedded manifest disagrees with "
               "its packaged one is one Basecamp reads twice and gets two "
               "answers from." % (APP_GENERATED_METADATA, APP_METADATA))

    # ---- layer 2: the shipped binaries carry this source's own strings ----
    lits = literals(repo, sorted(on_disk))
    if len(lits) < APP_LITERAL_FLOOR:
        r.fail("only %d literals of >= %d bytes were extracted from %s; the "
               "scanner found essentially nothing and would agree with any "
               "binary" % (len(lits), MIN_LITERAL, ", ".join(APP_SOURCE_DIRS)))

    for variant, blob in sorted(binaries.items()):
        missing = []
        for lit, sites in sorted(lits.items()):
            if lit in APP_LITERAL_EXCLUSIONS:
                continue
            if not in_binary(lit, blob):
                missing.append((lit, sites))
        checked = len(lits) - len(APP_LITERAL_EXCLUSIONS)
        if not missing:
            r.ok("every one of the %d source literals of >= %d bytes is in the "
                 "%s console" % (checked, MIN_LITERAL, variant))
        else:
            r.fail("%d of %d source literals are absent from the %s console, so "
                   "that binary was not built from this source:"
                   % (len(missing), checked, variant))
            for lit, sites in missing[:12]:
                print("          %-46r  %s" % (lit[:44], ", ".join(sites)))
            if len(missing) > 12:
                print("          ... and %d more" % (len(missing) - 12))

        # The allow-list must stay exactly as large as its reasons — and it is
        # empty, so this loop runs zero times and says nothing. That is the
        # correct amount for it to say: the assertion that the console's literal
        # check examined anything at all is the floor above, not this.
        for lit, why in sorted(APP_LITERAL_EXCLUSIONS.items()):
            if lit not in lits:
                r.fail("%r is excluded from the literal check but is no longer "
                       "in the source; delete the exclusion (%s)" % (lit, why))
            elif in_binary(lit, blob):
                r.fail("%r is excluded from the literal check but IS in the %s "
                       "console; the exclusion is stale and is now hiding that "
                       "literal from the check" % (lit, variant))

    return {"inputs": len(on_disk),
            "literals": len(lits) - len(APP_LITERAL_EXCLUSIONS),
            "variants": len(binaries)}


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
