#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Check that the documentation still points at things that exist.

    ./scripts/check-docs.py            # report and exit non-zero on any failure
    ./scripts/check-docs.py --list     # also print what passed

Five classes of rot, all of which this repository has shipped:

  1. A dangling path. `docs/use-cases.md` credited `scripts/use-cases/policy-hash.py`
     with self-checking every row of a manifest, for several commits after the
     file was deleted along with the design that needed it. A reader who went
     looking found nothing, and the paragraph read as a stronger control than
     the repository had.

  2. An out-of-range line citation. `docs/architecture.md` cited
     `agent_verifier.rs:663-691` in a file 645 lines long. That one is decidable
     without knowing what the line was supposed to say, so it is checked here.

  3. A document that promises to carry no identifiers, and then carries one.
     README.md's second paragraph says "No transaction hash appears on this
     page", and docs/skills.md 5 says "No program id, block height or balance
     is quoted here". Those are the right policy — four programs have been
     deployed and every redeploy moves the hash, the ImageID, every policy
     address and, when a shielded transfer mints a new note, the agent ids
     themselves; the storage agent's id changed from `7o9PT8uE...` to
     `9Xpkkvos...` on one such migration. But a promise nothing checks is how
     the four documents that asserted a superseded program as current came to
     do so. So the promise is checked.

  4. A markdown link whose target does not exist. This one was found by a
     falsification attempt against this script sailing through: class 1 only
     matches paths in BACKTICKS, so `[`docs/limitations.md`](docs/gone.md)`
     passed, because the half that resolves is the half nobody clicks. Every
     cross-reference between these documents is written that way. Link targets
     are resolved relative to the document's own directory, the way a reader's
     browser resolves them — not through the repo-root and docs/ fallbacks used
     for a path quoted in prose, which would make the check answer yes too
     often. `EXPECTED_OUTSIDE_REPO` holds the one target that is correct
     BECAUSE it does not resolve, and that entry fails if it ever starts to.

  5. A symbol citation naming something that does not exist. Line numbers into
     `module/src/agent_skills.cpp` drifted by between +87 and +324 lines over one
     refactor — a fixed correction would have fixed the middle third and broken
     the rest — so citations into module sources name a *symbol* instead.
     A symbol is stable under a refactor in a way a line is not, but only if it
     is real, which is what this checks.

  6. A skill the module registers that a documented catalogue does not list.
     `docs/architecture.md`'s table listed 24 of 28 for as long as the three
     `owner.*` skills and `meta.skills` had existed, and the skill-count gate
     below went green over it on every run — because a table short by four rows
     contains no wrong NUMBER. A count gate reads numbers; this reads names.

  7. A variant claim that disagrees with the packages. Both `.lgx` files went
     one variant, then two, then three, and "two variants", "one darwin-arm64
     variant" and "the packages are macOS arm64 only" all survived the moves —
     the last of them three lines above a transcript listing three. The tarball
     is the authority, not another sentence.

  8. A settlement on chain that `docs/DEPLOYMENT.md`'s ledger does not account
     for. That ledger opens "every transaction that has ever touched any of the
     three is listed below in block order" and was missing ten, four of which
     had taken an account the same document called one that "has never received
     a payment" from 0 to 4. `verify-deployment.sh` checks each settlement is on
     chain and never that the ledger mentions it.

  9. A truncated hash citation whose tail is not that hash's tail.

 10. A file under `module/tests/` that no workflow runs or names, against
     README §10's claim that CI "accounts for every file under module/tests/".
     Three of them — the transport harnesses — were in neither workflow at all.

WHAT THIS CANNOT DO, stated so nobody reads a green run as more than it is:
it cannot tell whether a line that exists is the line the sentence meant. Only
class 3 is immune to that, which is the argument for preferring it. Classes 6-10
are immune for the same reason class 3 is — each compares a document against an
artefact rather than against prose — which is why they were worth adding as
checks rather than as corrected sentences.
"""

import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERBOSE = "--list" in sys.argv

# SUBMISSION-DRAFT.md is the first document a reviewer opens and was the last
# one anything checked. Its evidence sections are generated by
# scripts/submission-evidence.py and guarded by that script's --check; its prose
# cites source paths and symbols like any other document here, and those are
# what this catches.
DOCS = ["README.md", "examples/README.md", "SUBMISSION-DRAFT.md"] + [
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
            # gitignored, and written by scripts/build-companion-modules.sh:
            # the upstream wallet/storage/messaging checkouts, their build trees
            # and the .lgx packages Logos Core installs them from.
            "build-companions/",
            # inside the pinned upstream checkouts: Basecamp, logos-cpp-sdk,
            # logos-module, logos-delivery. docs/basecamp.md pins each revision.
            "app/", "cpp/", "cmake/", "src/interface.h", "scripts/build_rln.sh")

# Paths a document names *because they are gone*. A retraction has to be able to
# say what it retracts. These are asserted ABSENT: if one comes back, the
# retraction is now the wrong sentence, and that fails too.
# Documents that promise, in their own words, to name no identifier. The reason
# string is the sentence each one is being held to — so a reader who deletes the
# promise knows to delete the entry, and one who deletes the entry has to face
# the promise.
HASH_FREE = {
    "README.md":
        'its own second paragraph: "No transaction hash appears on this page"',
    "docs/skills.md":
        'its own \u00a75: "No program id, block height or balance is quoted here"',
}

# The one value a hash-free document may carry that LOOKS like a live
# identifier: the worked example in docs/skills.md §7 prints the sha256 of a
# fixed string, and the digest of a constant is a constant.
#
# THE EXEMPTION USED TO BE A LINE EXEMPTION, and it was the widest thing in this
# file:
#
#     if "shasum" in raw or "digest" in raw or "sha256" in raw:
#         continue
#
# — the whole line skipped because one of three words appeared anywhere on it,
# in the one check whose subject is a document promising to carry no live
# identifier. Exactly one line in the tree needs it. Every line in the tree
# could claim it, and "digest" is a word these documents use constantly.
# Measured: appending
#
#     The digest is recorded beside settlement `54f851825f17…e2f47115` above.
#
# to README.md — a live settlement hash, on a page whose second paragraph
# promises "No transaction hash appears on this page" — passed, exit 0, with the
# promise counted among the 2,173 lines it had just checked.
#
# So the exemption is the VALUE now, and it is recomputed rather than described:
# every `{"content":"…"}` the document quotes is hashed, and only those digests
# are let through. A derived exemption cannot be borrowed by another line, and
# it fails if the transcript's digest ever stops being the digest of the
# transcript's own input — which is one more thing checked rather than one less.
CONTENT_RE = re.compile(r'\{"content":"((?:[^"\\]|\\.)*)"\}')


def worked_example_digests(text):
    """sha256 of every input a document's own transcripts quote."""
    out = set()
    for m in CONTENT_RE.finditer(text):
        try:
            content = json.loads('"%s"' % m.group(1))
        except ValueError:
            continue
        out.add(hashlib.sha256(content.encode("utf-8")).hexdigest())
    return out


# A deploy hash, an ImageID or a policy PDA seed: 16+ hex characters in a row.
HEX_RUN = re.compile(r"\b[0-9a-f]{16,}\b")
# A LEZ account or agent id: base58, which has no 0, O, I or l. 32 characters is
# already far past anything that occurs in English prose.
BASE58 = re.compile(r"\b[1-9A-HJ-NP-Za-km-z]{32,44}\b")

EXPECTED_ABSENT = {
    "scripts/use-cases/policy-hash.py":
        "deleted with the policy-hash design; docs/use-cases.md retracts it by name",
}

# A repo-relative path in backticks: `scripts/use-cases/lib.sh`, `module/src/x.h`.
PATH_RE = re.compile(r"`([A-Za-z0-9_.\-]+(?:/[A-Za-z0-9_.\-]+)+\.[A-Za-z0-9]+)`")
# The target of a markdown link: [text](docs/limitations.md), anchor stripped.
#
# This was a hole, and it was found the way holes should be: a falsification
# attempt against this very script sailed through. PATH_RE only matches paths
# inside BACKTICKS, so `[`docs/limitations.md`](docs/does-not-exist.md)` passed
# — the backticked half resolves and the half a reader actually clicks was
# never looked at. Every cross-reference between these documents is written in
# that form, and two of them now cite this script as their guarantee.
LINK_RE = re.compile(r"\]\(([^)#\s]+?)(?:#[^)\s]*)?\)")
LINK_SCHEMES = ("http://", "https://", "mailto:")

# Link targets that are correct precisely because they do not resolve here.
EXPECTED_OUTSIDE_REPO = {
    "../TERMS.md":
        "SUBMISSION-DRAFT.md is submitted into the prize repository, where it "
        "sits one level below TERMS.md. It does not resolve in this repository "
        "and must not: the link is right for where the document is read.",
}

# Line citations into a tree that is not in this repository, and the tree each
# one points into. This table is the price of making a citation that resolves
# nowhere a FAILURE rather than a silent skip — see the `hits` search in the
# CITE_RE loop below for what the silence cost — and it is deliberately a table
# with reasons rather than another entry in EXTERNAL: EXTERNAL is matched as a
# substring against a path, so widening it to cover `src/api_call_handler.h`
# would have exempted every `src/…` citation in the tree, this repository's
# included, which is a hole a hundred times the size of the one being closed.
#
# Each entry is asserted ABSENT, the way EXPECTED_OUTSIDE_REPO is: if one of
# these files turns up inside this repository, the exemption stops being true
# and that fails too. An allow-list nobody revisits is where the next defect
# hides.
EXPECTED_UPSTREAM = {
    "account_manager.rs":
        "lee's wallet, where the `KeyNotFoundError` docs/a2a-binding.md §7 "
        "quotes is raised. Not vendored here; docs/basecamp.md pins the "
        "checkout.",
    "validated_state_diff/mod.rs":
        "lee's state machine, which is what applies rule 7 and the chained-call "
        "cap. The line above it in the same table cites the same tree as "
        "`lee/state_machine/core/src/program/mod.rs:14`, which EXTERNAL already "
        "covers; this shorter form does not reach it.",
    "src/api_call_handler.h":
        "logos-delivery-module's own sources, one of the four upstream "
        "checkouts docs/basecamp.md pins by revision.",
    "src/delivery_module_plugin.cpp":
        "logos-delivery-module's own sources, as above.",
}
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
checked = {"path": 0, "link": 0, "line": 0, "symbol": 0, "promise": 0}


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

    # Markdown link targets, resolved the way a reader's browser resolves them:
    # relative to the directory the document is in, and nothing else. resolve()
    # is deliberately not used here — its repo-root and docs/ fallbacks are
    # right for a path quoted in prose and wrong for a link, which either works
    # when clicked or does not.
    for m in LINK_RE.finditer(text):
        target = m.group(1)
        if target.startswith(LINK_SCHEMES):
            continue
        line = text[: m.start()].count("\n") + 1
        if target in EXPECTED_OUTSIDE_REPO:
            if os.path.exists(os.path.normpath(
                    os.path.join(ROOT, os.path.dirname(doc), target))):
                failures.append(
                    "%s:%d  %s is allowlisted as resolving outside this "
                    "repository but now exists inside it — the exemption is "
                    "stale" % (doc, line, target))
            else:
                note_ok("link", "%s -> %s (outside the repo, as intended: %s)"
                        % (doc, target, EXPECTED_OUTSIDE_REPO[target]))
            continue
        if not os.path.exists(os.path.normpath(
                os.path.join(ROOT, os.path.dirname(doc), target))):
            failures.append(
                "%s:%d  link target does not exist: %s" % (doc, line, target))
        else:
            note_ok("link", "%s -> %s" % (doc, target))

    for m in CITE_RE.finditer(text):
        path, start, end = m.group(1), int(m.group(2)), m.group(3)
        if path.startswith(EXTERNAL) or any(x in path for x in EXTERNAL):
            continue
        line = text[: m.start()].count("\n") + 1
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
        else:
            hits = [target]
        # THE SKIP THAT COULD NOT FAIL, and it disarmed class 2 — the class this
        # script's own docstring calls "decidable without knowing what the line
        # was supposed to say". This search used to end
        #
        #     if len(hits) != 1:
        #         continue      # ambiguous or absent: not this check's business
        #
        # and the comment conflated two different answers. Ambiguous is indeed
        # undecidable. ABSENT is the finding: a citation naming a file that is
        # nowhere in this repository is class 1's defect wearing a line number,
        # and class 1 cannot see it, because PATH_RE only matches a path whose
        # closing backtick follows the extension — `module/src/gone.cpp:12` has
        # `:12` in the way. Measured by appending
        # "`module/src/nowhere.cpp:999`" to docs/limitations.md: this script
        # printed the same "7 line citations" it prints for a clean tree and
        # exited 0. A citation into nothing was counted as nothing rather than
        # as a failure, so the number a reader takes for coverage was the number
        # for a tree with one fewer citation in it.
        #
        # So absent is now a failure, ambiguous is now a failure that says which
        # files it could have meant, and the citations that are RIGHT to skip —
        # the four into upstream trees — are named in EXPECTED_UPSTREAM with the
        # tree each one points into, and fail if they ever start resolving here.
        if path in EXPECTED_UPSTREAM:
            if hits:
                failures.append(
                    "%s:%d  %s is allow-listed as a citation into an upstream "
                    "tree, but it resolves inside this repository now (%s) — the "
                    "exemption is stale and is hiding a line number this script "
                    "could check" % (doc, line, path,
                                     os.path.relpath(hits[0], ROOT)))
            else:
                note_ok("line", "%s -> %s (upstream, as intended: %s)"
                        % (doc, path, EXPECTED_UPSTREAM[path]))
            continue
        if not hits:
            failures.append(
                "%s:%d  cites %s:%s and there is no %s anywhere in this "
                "repository. Either the file moved and the citation did not, or "
                "it names an upstream tree and belongs in EXPECTED_UPSTREAM with "
                "the tree it points into."
                % (doc, line, path, m.group(2) + ("-" + end if end else ""),
                   path))
            continue
        if len(hits) > 1:
            failures.append(
                "%s:%d  cites %s:%s, and %d files in this repository are called "
                "that (%s). A line number is only checkable against one file, so "
                "write the path this citation means."
                % (doc, line, path, m.group(2) + ("-" + end if end else ""),
                   len(hits), ", ".join(sorted(os.path.relpath(h, ROOT)
                                               for h in hits))))
            continue
        target = hits[0]
        n = sum(1 for _ in open(target, "rb"))
        last = int(end) if end else start
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
            # The same silence class 2 had, in the class this file argues is the
            # STRONGER form of citation. `if target is None: continue` meant a
            # symbol cited in a file that does not exist was the one citation
            # shape nothing here looked at: PATH_RE never sees it either, because
            # SYMBOL_RE's own preferred spelling is a bare `agent_skills.cpp`
            # and PATH_RE requires a slash. Measured by writing "`NoSuchSymbol`
            # in `nowhere.cpp`" into docs/limitations.md — 71 symbol citations
            # checked, exit 0, and the closing line still read "every path, link
            # target, line citation and symbol citation resolves".
            #
            # Unlike a line citation there is no upstream case to carve out:
            # SYMBOL_RE only matches `Name`, `file.cpp` / `file.h`, which is
            # resolved under module/src, and module/src is this repository. So a
            # miss is a wrong sentence, and it says so.
            line = text[: m.start()].count("\n") + 1
            failures.append(
                "%s:%d  cites `%s` in %s, and there is no such file. A symbol "
                "citation survives a refactor that a line number does not, but "
                "only if the file it names is real." % (doc, line, symbol, path))
            continue
        body = open(target, encoding="utf-8", errors="replace").read()
        needle = symbol.split("::")[-1]
        if needle not in body:
            line = text[: m.start()].count("\n") + 1
            failures.append("%s:%d  %s is not in %s" % (doc, line, symbol, path))
        else:
            note_ok("symbol", "%s -> %s in %s" % (doc, symbol, path))

for doc, promise in HASH_FREE.items():
    full = os.path.join(ROOT, doc)
    if not os.path.exists(full):
        continue
    body = open(full, encoding="utf-8").read()
    # Derived from this document's own transcripts, before a line of it is read.
    exempt = worked_example_digests(body)
    for lineno, raw in enumerate(body.splitlines(), 1):
        # The promise itself, and the retraction paragraphs that explain why it
        # exists, may quote a truncated id as an example. Anything under 16 hex
        # or 32 base58 characters is too short to be mistaken for a live value.
        for m in list(HEX_RUN.finditer(raw)) + list(BASE58.finditer(raw)):
            token = m.group(0)
            # sha256 of a fixed string is a constant, not a chain identifier \u2014
            # but only if it IS that sha256, which is now recomputed from the
            # input the same document quotes rather than inferred from the word
            # "digest" being somewhere on the line. See CONTENT_RE above.
            if token in exempt:
                continue
            failures.append(
                "%s:%d  names an identifier (%s...) but promises not to \u2014 %s"
                % (doc, lineno, token[:12], promise))
        checked["promise"] += 1

# ---------------------------------------------------------------------------
# The skill count, which four documents and three harnesses all state
# ---------------------------------------------------------------------------
#
# Adding the twenty-third built-in skill meant editing forty-six lines across
# eleven files that carried the number twenty-two, and every one of them was
# found by hand. A document saying 22 next to a module registering 23 is the
# same class of drift as a package built from source nobody read: nothing fails,
# and a reviewer counting the entries in `skills()` finds the discrepancy before
# we do.
#
# So the number has one source — the registry in `installBuiltinSkills` — and
# every count-shaped mention of it anywhere in the tree has to agree. The next
# skill is then one edit plus whatever this reports.
SKILL_FILES = DOCS + [
    "docs/basecamp.md", "docs/skills.md", "docs/a2a-binding.md",
    "module/src/agent_module_plugin.cpp", "module/src/agent_module_plugin.h",
    "module/tests/skills_test.cpp", "module/tests/plugin_load_test.cpp",
    "module/tests/logos_core_load_test.cpp",
    ".github/workflows/ci.yml", "scripts/check-package-fresh.py",
    "app/README.md",
]

# Count-shaped: a number about a quantity of skills or card entries, ON A LINE
# THAT IS ABOUT SKILLS. Both halves are needed and the second is what keeps this
# from becoming noise: "all 121 assertions", "429 of them" and "1 entries" are
# all count-shaped and none of them is this count, so a checker that flagged
# every number would be turned off within a day. Two-and-up only, because a
# registry of one skill is not a thing this repository has ever had and `0
# entries` / `1 entries` appear in transcripts of failures.
#
# THE FOUR WAYS A COUNT HAS HIDDEN FROM THIS GATE, EACH FOUND THE SAME WAY:
# somebody read a number, believed it, and it was wrong.
#
# 1. THE HYPHENATED FORM. This gate went green over `reads its 22-skill card` in
#    README.md §8a while the module registered 23, counting 21 other mentions on
#    the same pass. A gate that reports a number and misses the one wrong line is
#    worse than no gate, because the number reads as coverage.
# 2. MARKDOWN EMPHASIS, which is not a word boundary to a reader and was one to
#    this regex. `docs/skills.md` §7 opened "The module registers **22** skills"
#    while the module registered 23, and `\b22\s+skills\b` never matched because
#    two asterisks sat between the digits and the word. The number a reader sees
#    in bold is the one they are most likely to quote, so emphasis is stripped
#    before matching rather than enumerated as more alternatives.
# 3. A LINE BREAK between the number and the noun. These documents are wrapped at
#    about 80 columns and this gate reads one line at a time, so "the module
#    registers 25\nskills" was invisible for the same reason the bold form was.
#    Adjacent lines are therefore joined and scanned as a window, and a match is
#    only counted there if it SPANS the break — otherwise every mention would be
#    counted twice.
# 4. THE NUMBER SPELLED OUT. `agent_module_plugin.h` said "Register the
#    twenty-two skills this module ships with" against a registry of 25, and
#    `skills_test.cpp` said "The other twenty have no port" when it was 23.
#
# The fourth is the one to be careful with, and the care is the point. "All
# twenty-one are implemented" and "the prize's twenty-one" are TRUE sentences
# about a different quantity — the prize's list of default skills — and a gate
# that flagged them would be wrong about a line nobody should change. So spelled
# numbers are read only inside two frames that can mean nothing else:
#
#   "the module's own <n>"  — a claim about this registry, so it must equal it;
#   "the <n> skills"        — likewise, and this is the one that caught
#                             `agent_module_plugin.h` still promising to
#                             "Register the twenty-two skills this module ships
#                             with" against a registry of 25;
#   "the other <n>"         — everything but the one under discussion, which is
#                             the registry MINUS ONE. Not decoration either:
#                             `docs/skills.md` said "registers the other twenty"
#                             while the module had 23, and "like the other
#                             twenty" in another paragraph of the same file, so
#                             two sentences were wrong by four between them and
#                             no arithmetic anywhere would have caught either.
#
# A frame that was tried and removed belongs here too, because the reason is the
# whole discipline: `"the same <n>"` would have caught a second stale number in
# that same header — and it also matched "the same 32 bytes the wallet would have
# supplied" in `ci.yml`, a true sentence about a public key on a line that
# happens to say "card". One false positive is enough to make a gate advisory,
# and an advisory gate is a gate that is off. The sentence in the header was
# rewritten to carry no second number instead.
#
# The last frame is the one with a limit, and it is stated rather than papered
# over: "the other <n>" means "all but the k I am talking about", and this gate
# can only read k = 1. `skills_test.cpp` had a comment meaning all but TWO, and
# the fix there was to write the sentence without a number at all — a count only
# some readers can compute is the thing this gate exists to stop, and removing it
# is better than teaching the gate to guess which k a sentence meant.
EMPHASIS = re.compile(r"[*_`]+")

WORD_NUMBERS = {
    "twenty": 20, "twenty-one": 21, "twenty-two": 22, "twenty-three": 23,
    "twenty-four": 24, "twenty-five": 25, "twenty-six": 26, "twenty-seven": 27,
    "twenty-eight": 28, "twenty-nine": 29, "thirty": 30, "thirty-one": 31,
    "thirty-two": 32, "thirty-three": 33, "thirty-four": 34, "thirty-five": 35,
}
# Longest first, or "twenty" matches the head of "twenty-five" and the gate
# reads 20 out of a line that says 25.
_WORDS = "|".join(sorted(WORD_NUMBERS, key=len, reverse=True))

# Case-insensitive throughout: "The other twenty" opens a sentence, and a gate
# that read the lower-case half of the tree would be the same defect one
# capital letter along.
COUNT_SHAPES = re.compile(
    r"\b(\d{2,3})\s+(?:skills|built-in skills|entries|of them)\b"
    r"|\b(\d{2,3})-skill\b"
    r"|\ball\s+(\d{2,3})\b"
    r"|\bexactly\s+(\d{2,3})\b"
    r"|\bevery one of the\s+(\d{2,3})\b"
    r"|\bmodule's own\s+(%s|\d{2,3})\b"
    r"|\bthe\s+(%s|\d{2,3})\s+skills\b" % (_WORDS, _WORDS), re.I)

# The same reading, for the frame whose true value is the registry less the one
# skill the sentence is about.
COUNT_SHAPES_LESS_ONE = re.compile(
    r"\bthe (?:other|remaining)\s+(%s|\d{2,3})\b" % _WORDS, re.I)


def count_mentions(text):
    """Every count-shaped claim in `text`, as (span, stated value, expected offset)."""
    for m in COUNT_SHAPES.finditer(text):
        yield m, next(g for g in m.groups() if g), 0
    for m in COUNT_SHAPES_LESS_ONE.finditer(text):
        yield m, m.group(1), 1


def stated_value(token):
    """`twenty-five` and `25` are the same claim."""
    return WORD_NUMBERS.get(token.lower(), None) if not token.isdigit() else int(token)


# …and the line has to be talking about skills at all.
ABOUT_SKILLS = re.compile(r"skill|card|registry|registered|dispatch", re.I)

# One marker, for a line that is deliberately about a count that is no longer
# true — the record of a defect, where changing the number would destroy the
# point being made. It has to be written down on the line itself, so an
# exemption is visible to whoever reads the sentence.
HISTORICAL = "(count-as-it-was)"


def registry_source():
    """The body of the `builtins{...}` initialiser in `installBuiltinSkills`."""
    src = open(os.path.join(ROOT, "module/src/agent_module_plugin.cpp"),
               encoding="utf-8").read()
    start = src.index("const std::vector<std::shared_ptr<logos::agent::ISkill>> builtins{")
    end = src.index("};", start)
    return src[start:end]


def registered_skill_count():
    """How many built-ins `installBuiltinSkills` actually registers."""
    return registry_source().count("std::make_shared<")


# ---------------------------------------------------------------------------
# The skill NAMES, and the three harness tables that spell them out
# ---------------------------------------------------------------------------
#
# The count above is not the whole drift, and the half it misses is the half
# that cost a red main. `skills_test.cpp`, `plugin_load_test.cpp` and
# `logos_core_load_test.cpp` each carry the registry written out by name —
# deliberately, and the comment above each says why: a count that goes red does
# not say WHAT went missing, and thirteen implemented skills were once
# unreachable with nothing to name them. A derived table would not do; an
# expectation read out of the thing it checks agrees with it by construction.
#
# But a hand-written table is a table that can go stale, and one did. When
# `owner.watch`, `owner.pending` and `owner.answer` joined the registry the two
# Qt harnesses got the three names and `skills_test.cpp` did not — it is the one
# harness CI can build, so eight of its assertions went red against a module
# that was entirely correct, and the log said only "the card parses, with one
# entry per registered skill". The count gate could not see it: nothing in that
# file states a number, `kBuiltinCount` is `sizeof` over the table itself, and a
# stale table is internally consistent.
#
# So the tables stay hand-written and their AGREEMENT is derived. The names come
# out of the registry the same way the count does — the classes it constructs,
# then each class's own `name()` — and every table has to be that set exactly.
# Adding a skill is then the registry edit plus whatever this reports, in
# seconds, at the first step of CI, instead of a compile-and-run four jobs later
# that names one assertion and no skill.
HARNESS_TABLES = [
    ("module/tests/skills_test.cpp", "kBuiltinSkills"),
    ("module/tests/plugin_load_test.cpp", "kSkills"),
    ("module/tests/logos_core_load_test.cpp", "kSkills"),
]


def registered_skill_names():
    """The name every built-in `installBuiltinSkills` registers answers to.

    Derived twice over: the classes the registry constructs, and the string each
    of those classes returns from `name()`. Anything that does not resolve is
    reported rather than dropped — a name-derivation that quietly resolved
    nothing would make every table below pass, which is the shape of gate this
    file exists to refuse.
    """
    classes = re.findall(r"std::make_shared<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>",
                         registry_source())
    headers = ""
    for hdr in sorted(os.listdir(os.path.join(ROOT, "module/src"))):
        if hdr.endswith(".h"):
            headers += open(os.path.join(ROOT, "module/src", hdr),
                            encoding="utf-8").read()
    names, unresolved = [], []
    for cls in classes:
        decl = re.search(r"\bclass\s+%s\b" % re.escape(cls), headers)
        got = None
        if decl:
            # Bounded to THIS class's body, at its closing `};` in column one.
            # Unbounded, the search ran on into the next class and returned its
            # name — which is not a miss, it is a wrong answer, and it was
            # measured: rewriting `OwnerWatchSkill::name()` to return a constant
            # made this script report `owner.pending` twice and blame the
            # registry for a duplicate registration it does not have. A
            # derivation that cannot read a class has to say so.
            close = headers.find("\n};", decl.start())
            body = headers[decl.start():close if close != -1 else len(headers)]
            got = re.search(r'std::string\s+name\(\)\s*const\s+override\s*'
                            r'\{\s*return\s+"([^"]+)"\s*;\s*\}', body)
        if got is None:
            unresolved.append(cls)
        else:
            names.append(got.group(1))
    return names, unresolved


def skill_table(rel, symbol):
    """The names a harness spells out, or None if the file is not here.

    Comments inside the table are stripped first: every one of these tables
    carries a `//` paragraph explaining a group of skills, and a name quoted
    inside one is prose, not an entry.
    """
    path = os.path.join(ROOT, rel)
    if not os.path.isfile(path):
        return None
    src = open(path, encoding="utf-8").read()
    m = re.search(r"\b%s\s*\[\s*\]\s*=\s*\{" % re.escape(symbol), src)
    if not m:
        return None
    body = src[m.end():src.index("};", m.end())]
    body = re.sub(r"//[^\n]*", "", body)
    return re.findall(r'"([^"]*)"', body)


expected_count = registered_skill_count()
expected_names, unresolved_classes = registered_skill_names()

# The derivation has to have worked before anything is compared against it.
if unresolved_classes:
    failures.append(
        "module/src/agent_module_plugin.cpp  the registry constructs %s, whose "
        "name() this script could not read out of module/src/*.h. Until it can, "
        "the harness tables below are compared against an incomplete registry."
        % ", ".join(unresolved_classes))
elif len(expected_names) != expected_count:
    failures.append(
        "module/src/agent_module_plugin.cpp  the registry constructs %d skills "
        "but %d names were derived from it: the two readings of installBuiltinSkills "
        "disagree, so neither can be trusted." % (expected_count, len(expected_names)))

tables_checked = 0
if expected_names and not unresolved_classes:
    want = set(expected_names)
    if len(want) != len(expected_names):
        dupes = sorted({n for n in expected_names if expected_names.count(n) > 1})
        failures.append(
            "module/src/agent_module_plugin.cpp  registers %s more than once: "
            "`registerSkill` refuses the second, so the card would be short one "
            "entry with nothing named." % ", ".join(dupes))
    for rel, symbol in HARNESS_TABLES:
        table = skill_table(rel, symbol)
        if table is None:
            failures.append(
                "%s  has no `%s[] = {...}` table for this check to read. Either "
                "it was renamed, or the harness stopped spelling the registry "
                "out — and a check that reads nothing passes everything."
                % (rel, symbol))
            continue
        tables_checked += 1
        got = set(table)
        missing = sorted(want - got)
        extra = sorted(got - want)
        if missing or extra:
            failures.append(
                "%s  `%s` disagrees with the registry in installBuiltinSkills: "
                "%s%s%s. The registry is the source; add or remove the name here."
                % (rel, symbol,
                   "missing " + ", ".join(missing) if missing else "",
                   " and " if missing and extra else "",
                   "names no built-in of that name: " + ", ".join(extra) if extra else ""))
        elif len(table) != len(got):
            dupes = sorted({n for n in table if table.count(n) > 1})
            failures.append(
                "%s  `%s` lists %s twice, so its `sizeof` count is one more than "
                "the registry has." % (rel, symbol, ", ".join(dupes)))

print("checked %d harness skill table(s) against the %d name(s) the registry "
      "derives" % (tables_checked, len(expected_names)))

counted = 0
for rel in sorted(set(SKILL_FILES)):
    path = os.path.join(ROOT, rel)
    if not os.path.isfile(path):
        continue
    lines = open(path, encoding="utf-8").read().splitlines()
    for i, raw in enumerate(lines):
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        # Emphasis is stripped before anything is matched: `**25**` and `25` are
        # the same claim to a reader, so they have to be the same claim here.
        here = EMPHASIS.sub("", raw)
        # The window, for a claim that wrapped. Comment and list markers on the
        # continuation are dropped so `25\n// skills` reads as `25 skills`.
        cont = re.sub(r"^\s*(?://+|#|\*|-|>)?\s*", " ", EMPHASIS.sub("", nxt))
        window = here.rstrip() + cont
        boundary = len(here.rstrip())

        for text, spans_break in ((here, False), (window, True)):
            if not ABOUT_SKILLS.search(text):
                continue
            if HISTORICAL in raw or (spans_break and HISTORICAL in nxt):
                continue
            for m, token, offset in count_mentions(text):
                # THE FIFTH SHAPE, and it was hiding a stale count in this
                # repository's own workflow. `.github/workflows/ci.yml` said
                #
                #     ... and both answer skills() with
                #     all 22 entries. Size, load and the whole ...
                #
                # against a registry of 28. The `here` pass never looked at the
                # second line because that line alone says nothing about skills;
                # the window pass looked, but discarded the match for lying
                # wholly inside the continuation — on the reasoning below, that
                # anything wholly inside the next line "will be counted when
                # that line is `here`". It never was, because it never could be.
                # So the discard is now conditional on that promise being true:
                # if the continuation on its own would not be scanned, the window
                # pass is the only pass that will ever see the claim, and it
                # keeps it.
                if spans_break and not (m.start() < boundary <= m.end()) \
                        and (m.end() <= boundary or ABOUT_SKILLS.search(cont)):
                    continue
                # (In the window pass, a claim wholly inside THIS line was
                # already counted by the pass above; a claim wholly inside the
                # next line is left to that line's own `here` pass, but only
                # when that pass will actually run — which is the condition
                # above.)
                value = stated_value(token)
                if value is None:
                    continue
                counted += 1
                expected = expected_count - offset
                if value != expected:
                    failures.append(
                        "%s:%d  says %s where the module registers %d built-in skills "
                        "(%s%s). Either the count moved and this did not, or the line is "
                        "about a count that is no longer true and needs %s on it."
                        % (rel, i + 1, token, expected_count, m.group(0).strip(),
                           ", which should be %d here" % expected if offset else "",
                           HISTORICAL))

print("checked %d skill-count mention(s) against the %d the module registers"
      % (counted, expected_count))

# ---------------------------------------------------------------------------
# The five checks below were added after a coherence pass found things every
# gate above went green over. Each one exists because a WRONG SENTENCE SHIPPED,
# and each is written so that the same wrong sentence fails it. A check that
# cannot fail is this repository's most-repeated defect, so the falsification
# for each is named beside it.
# ---------------------------------------------------------------------------

import tarfile

extra = {"catalogue": 0, "variant": 0, "ledger": 0, "trunc": 0, "suite": 0}


def package_variants(rel):
    """The variant directory names actually inside a committed .lgx."""
    with tarfile.open(os.path.join(ROOT, rel), "r:gz") as t:
        return sorted({n.split("/")[1] for n in t.getnames()
                       if n.startswith("variants/") and len(n.split("/")) > 2})


# 6. A SKILL THE MODULE REGISTERS AND NO CATALOGUE LISTS.
#
# docs/architecture.md's skill table listed 24 of 28 for as long as the three
# `owner.*` skills and `meta.skills` had existed. Every count-shaped mention in
# the tree agreed with the module the whole time, because the count gate reads
# NUMBERS and a table that is short by four rows contains no wrong number. The
# reference in docs/skills.md §7 is checked by examples/agent-console/run.sh
# against the running module; this checks the other catalogue, which nothing did.
#
# Falsification: delete a row from either table and this goes red.
SKILL_NAME_RE = re.compile(r'return "([a-z_]+\.[a-z_]+)"')
CATALOGUES = ["docs/architecture.md", "docs/skills.md"]

registered = set()
for _f in sorted(os.listdir(os.path.join(ROOT, "module/src"))):
    if _f.endswith((".cpp", ".h")):
        registered |= set(SKILL_NAME_RE.findall(
            open(os.path.join(ROOT, "module/src", _f), encoding="utf-8").read()))

for rel in CATALOGUES:
    body = open(os.path.join(ROOT, rel), encoding="utf-8").read()
    for name in sorted(registered):
        if "`%s`" % name in body:
            extra["catalogue"] += 1
        else:
            failures.append(
                "%s  does not name the registered skill `%s`. A catalogue with a "
                "row missing carries no wrong number, so the skill-count gate "
                "above cannot see it." % (rel, name))

# 7. A DOCUMENT THAT MISCOUNTS OR MISNAMES THE PACKAGE VARIANTS.
#
# Both .lgx files went from one variant to two to three. Sentences saying "two
# variants", "one darwin-arm64 variant" and "the packages are macOS arm64 only"
# survived in five documents — one of them three lines above a transcript
# listing three. The packages are the authority: a variant claim is checked
# against the tarball rather than against another sentence.
#
# Falsification: add a fourth variant, or write "two variants" anywhere.
VARIANTS = {rel: package_variants(rel)
            for rel in ("module/agent.lgx", "app/agent-ui.lgx")}
ALL_VARIANTS = sorted(set().union(*VARIANTS.values()))
N_VARIANTS = len(ALL_VARIANTS)
WORD = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five"}

# A claim about how many variants a package has, in either spelling.
VARIANT_COUNT = re.compile(
    r"\b(one|two|three|four|five|\d)\s+(?:platform\s+)?variants?\b", re.I)
# The claim that shipped hardest and is not count-shaped at all.
PLATFORM_ONLY = re.compile(
    r"packages are\s+\*{0,2}(?:macOS arm64|darwin-arm64)\s*only", re.I)

for doc in DOCS + ["app/README.md"]:
    full = os.path.join(ROOT, doc)
    if not os.path.exists(full):
        continue
    lines = open(full, encoding="utf-8").read().splitlines()
    for i, raw in enumerate(lines):
        lineno = i + 1
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if HISTORICAL in raw:
            continue
        # THE LINE BREAK, which this gate got wrong first time round and which
        # the skill-count gate above already documents as its failure mode 3.
        # These documents wrap at about 80 columns, and the sentence this check
        # was written for wrapped exactly through the middle of the claim:
        #     the packages are **macOS arm64
        #     only**, and the storage … skills have no ports wired
        # A one-line read sails past it, which is what the first version of this
        # check did — verified by putting the original sentence back and
        # watching it go green. Adjacent lines are joined before matching.
        window = EMPHASIS.sub("", raw.rstrip()) + " " + \
            EMPHASIS.sub("", nxt.lstrip()) if HISTORICAL not in nxt else \
            EMPHASIS.sub("", raw)
        if PLATFORM_ONLY.search(window):
            failures.append(
                "%s:%d  says the packages are macOS-only, but module/agent.lgx "
                "carries %s" % (doc, lineno, ", ".join(ALL_VARIANTS)))
        for m in VARIANT_COUNT.finditer(EMPHASIS.sub("", raw)):
            tok = m.group(1).lower()
            said = int(tok) if tok.isdigit() else \
                {v: k for k, v in WORD.items()}.get(tok)
            if said is None:
                continue
            extra["variant"] += 1
            if said != N_VARIANTS:
                failures.append(
                    "%s:%d  says %r where both packages carry %d variants (%s). "
                    "Either a variant was added and this did not move, or the "
                    "line needs %s on it."
                    % (doc, lineno, m.group(0).strip(), N_VARIANTS,
                       ", ".join(ALL_VARIANTS), HISTORICAL))

# Every variant a package holds must also be NAMED somewhere, or a reviewer on
# that platform is never told the variant exists. This is what caught
# README's layout block still listing agent-ui as two variants.
for rel, vs in VARIANTS.items():
    named = " ".join(open(os.path.join(ROOT, d), encoding="utf-8").read()
                     for d in ("README.md", "docs/basecamp.md", "app/README.md",
                               "docs/limitations.md"))
    for v in vs:
        if v not in named:
            failures.append(
                "%s carries a %s variant that no document names" % (rel, v))

# 8. A SETTLEMENT ON CHAIN THAT DEPLOYMENT.md's LEDGER DOES NOT ACCOUNT FOR.
#
# docs/DEPLOYMENT.md's ledger opens by promising "every transaction that has
# ever touched any of the three is listed below in block order". It was missing
# ten, including four that took an account the same document called one that
# "has never received a payment" from 0 to 4. Nothing noticed, because
# verify-deployment.sh checks that each settlement is ON CHAIN and never that
# the ledger MENTIONS it.
#
# This needs no network: the manifests already name every settlement, so the
# ledger is checked against them. A settlement appended by a2a-task.sh and not
# written up goes red on the next run.
#
# Falsification: delete a row from the ledger, or add one to a2a-task.tsv.
LEDGER_DOC = "docs/DEPLOYMENT.md"
_ledger = open(os.path.join(ROOT, LEDGER_DOC), encoding="utf-8").read()


def tsv_column(rel, header):
    """The values under a named column, and a sentence saying why there are none.

    Returns `(values, complaint)`. The complaint is the whole point of the
    signature: every one of the three ways this used to answer `[]` — no file,
    no such column, no rows — makes the ledger comparison below run over an
    empty list, and a comparison over an empty list passes. It is the shape this
    repository has met most often and it hid here in the plainest possible form.
    Measured, on a copy of the tree: renaming `settlement_tx` to `tx_hash` in
    artifacts/a2a-task.tsv — a rename this file has already had twice, and the
    exact reason submission-evidence.py reads TSVs by header name — took the
    ledger check from fifteen settlements to two and left the run green, saying
    "checked ... 2 settlement(s) against the ledger" where a reader sees a
    number and reads coverage. Thirteen settlements stopped being checked and
    nothing anywhere said so.

    So an absence is now reported as an absence, and the caller fails on it.
    """
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        return [], "%s is not in this repository" % rel
    rows = [l.rstrip("\n").split("\t") for l in open(path, encoding="utf-8")]
    if not rows:
        return [], "%s is empty" % rel
    if header not in rows[0]:
        return [], ("%s has no `%s` column; its columns are %s"
                    % (rel, header, ", ".join(rows[0])))
    i = rows[0].index(header)
    values = [r[i] for r in rows[1:] if len(r) > i]
    if not values:
        return [], "%s has a header and no rows under `%s`" % (rel, header)
    return values, None


for rel, header in (("artifacts/a2a-task.tsv", "settlement_tx"),
                    ("artifacts/shielded-settlement.tsv", "tx")):
    txs, complaint = tsv_column(rel, header)
    if complaint:
        failures.append(
            "%s, so %s's ledger was compared against nothing. That comparison "
            "cannot fail on an empty list, which makes a missing manifest and a "
            "complete ledger the same green run." % (complaint, LEDGER_DOC))
        continue
    here = 0
    for tx in txs:
        if len(tx) != 64:
            continue
        here += 1
        # The ledger cites hashes truncated first8…last8, the way every table in
        # these documents does; the full form counts too.
        if tx in _ledger or ("%s…%s" % (tx[:8], tx[-8:])) in _ledger:
            extra["ledger"] += 1
        else:
            failures.append(
                "%s: %s…%s is in %s but %s's ledger does not account for it, "
                "and that ledger's own words are \"every transaction that has ever "
                "touched any of the three\"." % (rel, tx[:8], tx[-8:], rel,
                                                 LEDGER_DOC))
    # And the column has to hold hashes. A `settlement_tx` column of `none` or
    # of truncated ids reads as a manifest with rows and compares nothing, which
    # is the same defect one column along.
    if not here:
        failures.append(
            "no row of %s carries a 64-character hash under `%s`, so %s's "
            "ledger was compared against nothing at all"
            % (rel, header, LEDGER_DOC))

# 9. A TRUNCATED HASH WHOSE HALVES BELONG TO DIFFERENT TRANSACTIONS.
#
# These documents cite hashes as `first8…last8`, dozens of times, and a reader
# checking one against the chain is the only thing that would notice a wrong
# tail. That is decidable by machine wherever the repository records the full
# hash, so it is decided here.
#
# WHAT THIS CANNOT SEE, stated because the gap is most of the ledger: it only
# checks a citation whose prefix matches a hash in artifacts/. The funding
# transfers and shielded spends in docs/DEPLOYMENT.md's ledger are named nowhere
# else, so nothing offline can confirm their tails — verify-deployment.sh is
# where that would have to go, and it would cost a getBlock per row.
#
# Falsification: change one character of the tail of any citation whose
# transaction is in a manifest — e.g. a settlement hash in a2a-task.tsv.
KNOWN_HASHES = set()
for _rel in ("artifacts/a2a-task.tsv", "artifacts/shielded-settlement.tsv",
             "artifacts/agents.tsv", "artifacts/anchored.tsv",
             "artifacts/adversarial.tsv", "artifacts/notary.tsv",
             "artifacts/alerts.tsv"):
    _p = os.path.join(ROOT, _rel)
    if os.path.exists(_p):
        KNOWN_HASHES |= set(re.findall(
            r"\b[0-9a-f]{64}\b", open(_p, encoding="utf-8").read()))

TRUNC = re.compile(r"`([0-9a-f]{8})…([0-9a-f]{6,9})`")
BY_PREFIX = {}
for h in KNOWN_HASHES:
    BY_PREFIX.setdefault(h[:8], set()).add(h)

for doc in DOCS:
    full = os.path.join(ROOT, doc)
    if not os.path.exists(full):
        continue
    for lineno, raw in enumerate(open(full, encoding="utf-8"), 1):
        for m in TRUNC.finditer(raw):
            head, tail = m.group(1), m.group(2)
            cands = BY_PREFIX.get(head)
            if not cands:
                continue          # not a hash this repository records: not ours
            extra["trunc"] += 1
            if not any(h.endswith(tail) for h in cands):
                failures.append(
                    "%s:%d  cites `%s…%s`, but the hash starting %s ends %s"
                    % (doc, lineno, head, tail, head,
                       "/".join(sorted(h[-8:] for h in cands))))

# 10. A SUITE UNDER module/tests/ THAT CI NEITHER RUNS NOR NAMES.
#
# README §10 says ci.yml "accounts for every file under module/tests/ so that a
# suite absent from CI cannot be mistaken for one that passed". Three files —
# plugin_delivery_test.cpp, logos_core_delivery_test.cpp and owner_responder.cpp
# — appeared in neither workflow at all, so that sentence was false about the
# three harnesses that cost the most to run.
#
# Falsification: add a file to module/tests/ and this goes red until CI runs it
# or the accounting block names it and says what it needs.
WORKFLOWS = " ".join(
    open(os.path.join(ROOT, ".github/workflows", w), encoding="utf-8").read()
    for w in sorted(os.listdir(os.path.join(ROOT, ".github/workflows")))
    if w.endswith((".yml", ".yaml")))

for _f in sorted(os.listdir(os.path.join(ROOT, "module/tests"))):
    if not _f.endswith((".cpp", ".c")):
        continue
    if _f in WORKFLOWS:
        extra["suite"] += 1
    else:
        failures.append(
            "module/tests/%s is neither run by nor named in any workflow. "
            "README §10 claims every file here is accounted for, so either "
            "run it or name it in ci.yml's accounting block with what it needs."
            % _f)

print("checked %d catalogue entr(ies), %d variant claim(s) against %d packaged "
      "variant(s), %d settlement(s) against the ledger, %d truncated hash(es), "
      "%d test file(s) against the workflows"
      % (extra["catalogue"], extra["variant"], N_VARIANTS, extra["ledger"],
         extra["trunc"], extra["suite"]))

print("checked %d paths, %d link targets, %d line citations, %d symbol "
      "citations, %d lines of two hash-free documents, across %d documents"
      % (checked["path"], checked["link"], checked["line"], checked["symbol"],
         checked["promise"], len(DOCS)))
if failures:
    print("\n%d documentation reference(s) point at something that is not there:\n"
          % len(failures))
    for f in failures:
        print("  " + f)
    sys.exit(1)


print("every path, link target, line citation and symbol citation resolves")
