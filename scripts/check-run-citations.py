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


def sh(*args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main():
    probe = sh("gh", "auth", "status")
    if probe.returncode != 0:
        print("gh is not available or not authenticated, so no citation could be\n"
              "checked. This gate has no skip path on purpose: passing here without\n"
              "having looked is the exact failure it exists to prevent.\n"
              + (probe.stderr or probe.stdout).strip())
        return 1

    cited = {}
    for doc in DOCS:
        path = os.path.join(ROOT, doc)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                for run_id in RUN_RE.findall(line):
                    cited.setdefault(run_id, []).append("%s:%d" % (doc, lineno))

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
        anc = sh("git", "merge-base", "--is-ancestor", sha, "HEAD")
        if anc.returncode != 0:
            failures.append(
                "run %s ran on %s, which is not an ancestor of HEAD — the reader is "
                "sent to a commit this branch does not contain (%s %s), cited at %s"
                % (run_id, sha[:7], info.get("workflowName", "?"),
                   info.get("conclusion", "?"), ", ".join(sites)))

    print("checked %d cited CI run(s) across %d document(s)" % (len(cited), len(DOCS)))
    if failures:
        print("\n%d citation(s) point at a commit this branch does not have:\n"
              % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("every cited run ran on a commit this branch contains")
    return 0


if __name__ == "__main__":
    sys.exit(main())
