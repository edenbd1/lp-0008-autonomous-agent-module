#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Check that the documentation still points at things that exist.

    ./scripts/check-docs.py            # report and exit non-zero on any failure
    ./scripts/check-docs.py --list     # also print what passed

Three classes of rot, all of which this repository has shipped:

  1. A dangling path. `docs/use-cases.md` credited `scripts/use-cases/policy-hash.py`
     with self-checking every row of a manifest, for several commits after the
     file was deleted along with the design that needed it. A reader who went
     looking found nothing, and the paragraph read as a stronger control than
     the repository had.

  2. An out-of-range line citation. `docs/architecture.md` cited
     `agent_verifier.rs:663-691` in a file 645 lines long. That one is decidable
     without knowing what the line was supposed to say, so it is checked here.

  3. A symbol citation naming something that does not exist. Line numbers into
     `module/src/agent_skills.cpp` drifted by between +87 and +324 lines over one
     refactor — a fixed correction would have fixed the middle third and broken
     the rest — so citations into module sources name a *symbol* instead.
     A symbol is stable under a refactor in a way a line is not, but only if it
     is real, which is what this checks.

WHAT THIS CANNOT DO, stated so nobody reads a green run as more than it is:
it cannot tell whether a line that exists is the line the sentence meant. Only
class 3 is immune to that, which is the argument for preferring it.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERBOSE = "--list" in sys.argv

DOCS = ["README.md", "examples/README.md"] + [
    os.path.join("docs", f)
    for f in sorted(os.listdir(os.path.join(ROOT, "docs")))
    if f.endswith(".md")
] + [
    os.path.join("docs/benchmarks", f)
    for f in sorted(os.listdir(os.path.join(ROOT, "docs/benchmarks")))
    if f.endswith(".md")
]

# Trees that are not in the repository and are documented as not being: upstream
# LEZ sources, the checkouts a reader is told to clone, system headers, and
# gitignored build output.
EXTERNAL = ("lee/", "lez/", "_external/", "logos-package/", "spel-framework-macros/",
            "vendor/spel/", "/path/to/", "library/", "build/", "lib/", "target/",
            "nlohmann/", "logos-execution-zone/", "build-basecamp/",
            # inside the pinned upstream checkouts: Basecamp, logos-cpp-sdk,
            # logos-module, logos-delivery. docs/basecamp.md pins each revision.
            "app/", "cpp/", "cmake/", "src/interface.h", "scripts/build_rln.sh")

# Paths a document names *because they are gone*. A retraction has to be able to
# say what it retracts. These are asserted ABSENT: if one comes back, the
# retraction is now the wrong sentence, and that fails too.
EXPECTED_ABSENT = {
    "scripts/use-cases/policy-hash.py":
        "deleted with the policy-hash design; docs/use-cases.md retracts it by name",
}

# A repo-relative path in backticks: `scripts/use-cases/lib.sh`, `module/src/x.h`.
PATH_RE = re.compile(r"`([A-Za-z0-9_.\-]+(?:/[A-Za-z0-9_.\-]+)+\.[A-Za-z0-9]+)`")
# `file.ext:12` or `file.ext:12-34`, possibly a bare `:56` continuation.
CITE_RE = re.compile(r"`([A-Za-z0-9_.\-/]+\.(?:cpp|h|rs|py|sh|json|toml|yml)):(\d+)(?:-(\d+))?`")
# The stable form this repository now prefers, and only that form:
#     `Symbol` in `module/src/file.cpp`      `Symbol`, `file.h`
# Anything looser matches prose that happens to put a backticked word near a
# backticked filename — "the module `dlopen`s what it finds … in
# `agent_module_plugin.cpp`" was matched by an earlier version of this regex.
SYMBOL_RE = re.compile(
    r"`([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)?)`"
    r"(?:,| in) `((?:module/src/)?[A-Za-z0-9_]+\.(?:cpp|h))`")

failures = []
checked = {"path": 0, "line": 0, "symbol": 0}


def note_ok(kind, what):
    checked[kind] += 1
    if VERBOSE:
        print("  ok    %s" % what)


def resolve(doc, path):
    """A path in a doc may be repo-relative, relative to the doc's directory, or
    — for the cross-links between documents — relative to `docs/`."""
    for cand in (os.path.join(ROOT, path),
                 os.path.join(ROOT, os.path.dirname(doc), path),
                 os.path.join(ROOT, "docs", path)):
        if os.path.exists(cand):
            return cand
    return None


for doc in DOCS:
    full = os.path.join(ROOT, doc)
    if not os.path.exists(full):
        continue
    text = open(full, encoding="utf-8").read()

    for m in PATH_RE.finditer(text):
        path = m.group(1)
        if path.startswith(EXTERNAL) or any(x in path for x in EXTERNAL):
            continue
        if "/" not in path:
            continue
        line0 = text[: m.start()].count("\n") + 1
        if path in EXPECTED_ABSENT:
            if resolve(doc, path) is not None:
                failures.append(
                    "%s:%d  %s is named as deleted but exists again — the retraction is now wrong"
                    % (doc, line0, path))
            else:
                note_ok("path", "%s -> %s (absent, as it says: %s)"
                        % (doc, path, EXPECTED_ABSENT[path]))
            continue
        if resolve(doc, path) is None:
            line = text[: m.start()].count("\n") + 1
            failures.append("%s:%d  path does not exist: %s" % (doc, line, path))
        else:
            note_ok("path", "%s -> %s" % (doc, path))

    for m in CITE_RE.finditer(text):
        path, start, end = m.group(1), int(m.group(2)), m.group(3)
        if path.startswith(EXTERNAL) or any(x in path for x in EXTERNAL):
            continue
        target = resolve(doc, path)
        if target is None:
            # Bare file names such as `agent_skills.cpp` are resolved against the
            # module and crate trees, which is how the docs write them.
            hits = []
            for base, _dirs, files in os.walk(ROOT):
                if any(s in base for s in (".git", "target", "build", "_external", "vendor")):
                    continue
                if os.path.basename(path) in files:
                    hits.append(os.path.join(base, os.path.basename(path)))
            if len(hits) != 1:
                continue          # ambiguous or absent: not this check's business
            target = hits[0]
        n = sum(1 for _ in open(target, "rb"))
        last = int(end) if end else start
        line = text[: m.start()].count("\n") + 1
        if last > n:
            failures.append(
                "%s:%d  cites %s:%s but that file has %d lines"
                % (doc, line, path, m.group(2) + ("-" + end if end else ""), n))
        else:
            note_ok("line", "%s -> %s:%d" % (doc, path, last))

    for m in SYMBOL_RE.finditer(text):
        symbol, path = m.group(1), m.group(2)
        if "/" not in path:
            path = "module/src/" + path
        target = resolve(doc, path)
        if target is None:
            continue
        body = open(target, encoding="utf-8", errors="replace").read()
        needle = symbol.split("::")[-1]
        if needle not in body:
            line = text[: m.start()].count("\n") + 1
            failures.append("%s:%d  %s is not in %s" % (doc, line, symbol, path))
        else:
            note_ok("symbol", "%s -> %s in %s" % (doc, symbol, path))

print("checked %d paths, %d line citations, %d symbol citations across %d documents"
      % (checked["path"], checked["line"], checked["symbol"], len(DOCS)))
if failures:
    print("\n%d documentation reference(s) point at something that is not there:\n"
          % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("every path, line citation and symbol citation resolves")
