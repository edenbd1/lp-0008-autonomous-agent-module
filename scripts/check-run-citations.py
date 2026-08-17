#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Every CI run this repository cites must have run on a commit this branch has.

WHY THIS EXISTS

A green run is the evidence for two criteria here -- "end-to-end integration
tests run against a LEZ sequencer (standalone mode) and are included in CI", and
"a reproducible end-to-end demo script works against a real local sequencer with
RISC0_DEV_MODE=0". Nobody re-runs those: the first needs a whole
logos-execution-zone build and hours of proving. What a reader does instead is
follow the link. So the link IS the deliverable, and a link that lands on a
commit the repository does not contain is worse than no link at all -- it looks
like a history rewritten to hide something.

This repository rewrote its history once, for a good reason, and orphaned every
green run it had cited. That was noticed, and fixed, in the documents somebody
happened to be reading -- and four more citations in two files went on pointing
at commits that no longer exist, because the fix was applied to the instances
instead of to the class. This is the class.

WHAT IT CHECKS

For every `actions/runs/<id>` in the documents: resolve the run's head commit
through the GitHub API, and require it to be an ancestor of the current branch.
A run whose commit is gone is reported by id, together with what it was.

NO SKIP PATH. If `gh` is missing or unauthenticated this exits non-zero and says
so. A citation checker that quietly passes when it cannot check is the failure it
was written to prevent, one level up.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = ["README.md", "SUBMISSION-DRAFT.md", "solutions/LP-0008.md"] + [
    os.path.join("docs", f)
    for f in sorted(os.listdir(os.path.join(ROOT, "docs")))
    if f.endswith(".md")
]
RUN_RE = re.compile(r"actions/runs/(\d+)")
# GitHub run ids are 11 digits today. Anchored on word boundaries so block
# heights, byte counts and hashes cannot be mistaken for one.
BARE_RE = re.compile(r"\b(\d{11})\b")


def sh(*args):
    """Run a command, and treat "it is not installed" as a failed run.

    subprocess.run RAISES FileNotFoundError when the binary is absent rather
    than returning a non-zero code, so every `if out.returncode != 0` below was
    unreachable in the one case the docstring above promises to handle. The
    negative control — running this with gh off PATH — produced a traceback
    instead of the sentence explaining what was wrong. Exiting non-zero by
    crashing is not the same as saying so.
    """
    try:
        return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    except (FileNotFoundError, PermissionError) as exc:
        return subprocess.CompletedProcess(args, 127, "", "%s: %s" % (args[0], exc))


def main():
    # PROBE WITH THE THING THIS SCRIPT ACTUALLY DOES, not with `gh auth status`.
    #
    # `gh auth status` validates the token against /user, which a GitHub Actions
    # job token legitimately cannot read — it is a repository-scoped token, not
    # a user one. It answered "The token in GH_TOKEN is invalid" on 2026-08-17
    # and turned this gate red on a commit that changed a sentence in a markdown
    # file, while `gh run view` — the only gh call below — worked fine in the
    # same job. It had also passed in the four runs before that, which is worse
    # than failing consistently: a probe that is a coin flip teaches everyone to
    # re-run rather than to read.
    #
    # So the probe is one real query of the same kind the checks below make. If
    # that works, they will; if it does not, nothing here can be checked and
    # this exits non-zero and says so.
    probe = sh("gh", "run", "list", "--limit", "1", "--json", "databaseId")
    if probe.returncode != 0:
        print("gh cannot list this repository's runs, so no citation could be\n"
              "checked. This gate has no skip path on purpose: passing here without\n"
              "having looked is the exact failure it exists to prevent.\n"
              + (probe.stderr or probe.stdout).strip())
        return 1

    # A SHALLOW CLONE CANNOT ANSWER THIS QUESTION, and must not pretend to.
    # `actions/checkout@v4` fetches depth 1 by default, so on a runner HEAD has
    # no ancestors and `merge-base --is-ancestor` is false for every commit
    # ever made -- which this gate reported as "the reader is sent to a commit
    # this branch does not contain", about citations that are perfectly good.
    # Three of them, on the first run where it had permission to look.
    #
    # This is the conflation the previous commit message flagged and left
    # standing: "I could not resolve it" and "it is orphaned" are a credentials
    # or checkout problem and a repository problem, and they had the same
    # message. They do not now.
    shallow = sh("git", "rev-parse", "--is-shallow-repository")
    if shallow.stdout.strip() == "true":
        print("this is a SHALLOW clone: HEAD has no ancestors here, so every\n"
              "citation would read as orphaned and none of that would be true.\n"
              "Fetch the full history before running this — in a workflow that\n"
              "is `fetch-depth: 0` on the checkout step.")
        return 1

    cited = {}
    bare = {}
    for doc in DOCS:
        path = os.path.join(ROOT, doc)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                linked = RUN_RE.findall(line)
                for run_id in linked:
                    cited.setdefault(run_id, []).append("%s:%d" % (doc, lineno))
                # A run id written as a bare number is still evidence. The two
                # green e2e runs this submission rests on are in a markdown
                # table as `31916748823`, not as URLs, and so escaped this gate
                # entirely — the check looked for links, and what was
                # load-bearing was a number. Anything that long and that
                # numeric is a run id or a mistake; both are worth resolving.
                for run_id in BARE_RE.findall(line):
                    if run_id not in linked:
                        bare.setdefault(run_id, []).append("%s:%d" % (doc, lineno))

    if not cited:
        print("no CI run is cited in any document, so there is nothing to check.\n"
              "That is suspicious rather than clean: the two criteria that rest on a\n"
              "run being green have no link for a reader to follow.")
        return 1

    failures = []
    for run_id, sites in sorted(cited.items()):
        out = sh("gh", "run", "view", run_id, "--json", "headSha,workflowName,conclusion")
        if out.returncode != 0:
            failures.append("run %s (cited at %s) could not be resolved: %s"
                            % (run_id, ", ".join(sites), out.stderr.strip()[:120]))
            continue
        info = json.loads(out.stdout)
        sha = info.get("headSha", "")
        # Distinguish "the commit is not an ancestor" from "this repository has
        # never heard of it" -- the second is a fetch problem wearing the
        # first's face.
        known = sh("git", "cat-file", "-e", sha + "^{commit}")
        if known.returncode != 0:
            failures.append(
                "run %s ran on %s, which this checkout does not have AT ALL — that is a "
                "fetch problem, not necessarily an orphaned commit; cited at %s"
                % (run_id, sha[:7], ", ".join(sites)))
            continue
        anc = sh("git", "merge-base", "--is-ancestor", sha, "HEAD")
        if anc.returncode != 0:
            failures.append(
                "run %s ran on %s, which is not an ancestor of HEAD — the reader is "
                "sent to a commit this branch does not contain (%s %s), cited at %s"
                % (run_id, sha[:7], info.get("workflowName", "?"),
                   info.get("conclusion", "?"), ", ".join(sites)))

    print("checked %d cited CI run(s) across %d document(s)" % (len(cited), len(DOCS)))

    # Bare run ids are held to a different standard on purpose. A URL is
    # something a reader clicks, so it must land inside this branch's history.
    # A number in a table is a claim about a run that happened, and the honest
    # requirement is that the run exists and says what the document says it
    # says. Ancestry is REPORTED for them, never enforced: this repository
    # rewrote its history once for a good reason, and the runs from before that
    # are still real runs. What would be dishonest is not disclosing it, and
    # the disclosure is what this prints.
    if bare:
        print("\n%d run id(s) mentioned without a link:" % len(bare))
        for run_id, sites in sorted(bare.items()):
            out = sh("gh", "run", "view", run_id, "--json",
                     "headSha,workflowName,conclusion")
            if out.returncode != 0:
                failures.append(
                    "run %s is named at %s and DOES NOT RESOLVE — a number that "
                    "looks like evidence and is not"
                    % (run_id, ", ".join(sites)))
                continue
            info = json.loads(out.stdout)
            sha = info.get("headSha", "")
            known = sh("git", "cat-file", "-e", sha + "^{commit}").returncode == 0
            anc = known and sh("git", "merge-base", "--is-ancestor",
                               sha, "HEAD").returncode == 0
            print("  %s  %s  %s  on %s — %s"
                  % (run_id, info.get("workflowName", "?"),
                     info.get("conclusion", "?"), sha[:7],
                     "in this branch" if anc
                     else "NOT in this branch; the document must say so"))

    if failures:
        print("\n%d citation(s) a reader could not follow:\n" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("\nevery cited run ran on a commit this branch contains")
    return 0


if __name__ == "__main__":
    sys.exit(main())
