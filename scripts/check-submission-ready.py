#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""The things that are only wrong at the moment you submit.

WHY THIS EXISTS, AND WHY IT IS NOT PART OF CI

Every other gate here answers a question about the tree: does this path resolve,
does this hash match, does this binary contain that literal. They run on every
push and they are right to. This one answers questions that are *supposed* to
have the wrong answer during development and must have the right one exactly
once -- when the pull request opens.

The one that prompted it: two criteria say the repository must be **public**
("full documentation and a clean public repository are delivered", and the first
submission requirement opens "Public repository with the Logos Core module").
This repository is deliberately private while it is being built. The
self-assessment answered the documentation half and the licence half of that
criterion and never mentioned visibility at all -- so two boxes read MET for
months while the thing they name was false, and no gate could have caught it,
because during development the answer is *meant* to be private.

That is the shape of everything in here: true late, false early, and invisible
to a checker that only ever looks at files.

    ./scripts/check-submission-ready.py

Run it immediately before opening the pull request. A non-zero exit is a list of
things a reviewer would find.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBMISSION = "solutions/LP-0008.md"
VIDEO_RE = re.compile(r"(youtu\.be/|youtube\.com/watch|vimeo\.com/)[\w?=&/-]+", re.I)


def sh(*args):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as fh:
        return fh.read()


def main():
    failures, notes = [], []

    # 1. public
    out = sh("gh", "repo", "view", "--json", "visibility,name,licenseInfo")
    if out.returncode != 0:
        failures.append("could not ask GitHub whether this repository is public: "
                        + out.stderr.strip()[:120])
    else:
        info = json.loads(out.stdout)
        vis = info.get("visibility", "?")
        if vis != "PUBLIC":
            failures.append(
                "the repository is %s. Two criteria name a PUBLIC repository — "
                "'full documentation and a clean public repository are delivered', "
                "and the first submission requirement. Nothing in the tree can show "
                "this; it is a property of the remote." % vis)
        else:
            notes.append("the repository is public")

    # 2. a video the reader can watch
    if not os.path.exists(os.path.join(ROOT, SUBMISSION)):
        failures.append("%s does not exist" % SUBMISSION)
    else:
        body = read(SUBMISSION)
        if not VIDEO_RE.search(body):
            failures.append(
                "no video URL in %s. The criterion asks for 'a recorded video demo "
                "of the end-to-end flow ... showing terminal output', and the "
                "submission requirement asks the builder to narrate it. A local "
                ".mp4 nobody can open is not a submission." % SUBMISSION)
        else:
            notes.append("a video URL is present in " + SUBMISSION)

    # 3. the boxes and the tally agree
    if os.path.exists(os.path.join(ROOT, SUBMISSION)):
        body = read(SUBMISSION)
        met = len(re.findall(r"(?m)^- \[x\]", body))
        unmet = len(re.findall(r"(?m)^- \[ \]", body))
        if unmet:
            failures.append(
                "%d criterion box(es) in %s are still unchecked, out of %d. A "
                "submission that grades itself short is being honest, but it is not "
                "ready — decide deliberately before opening it."
                % (unmet, SUBMISSION, met + unmet))
        else:
            notes.append("all %d criterion boxes are checked" % met)

    # 4. every cited run resolves to a commit this branch has
    cites = sh(os.path.join(ROOT, "scripts/check-run-citations.py"))
    if cites.returncode != 0:
        failures.append("scripts/check-run-citations.py fails:\n      "
                        + "\n      ".join(cites.stdout.strip().splitlines()[-4:]))
    else:
        notes.append("every cited CI run is on a commit this branch contains")

    # 5. nothing that points outside this repository
    leak = sh("git", "grep", "-nIE",
              r"competing submission|another submission|was closed with|"
              r"lp-000[2357]|LP-000[2357]", "--", ".")
    hits = [l for l in leak.stdout.splitlines()
            if "check-submission-ready" not in l]
    if hits:
        failures.append(
            "%d line(s) refer to something outside this repository:\n      %s"
            % (len(hits), "\n      ".join(h[:150] for h in hits[:5])))
    else:
        notes.append("nothing in the tree points at another submission")

    for n in notes:
        print("  ok    " + n)
    if failures:
        print("\nNOT READY — %d thing(s) a reviewer would find:\n" % len(failures))
        for f in failures:
            print("  --    " + f)
        return 1
    print("\nready to submit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
