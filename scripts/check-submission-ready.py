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
import urllib.request
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBMISSION = "solutions/LP-0008.md"
# A video the reader can watch -- which is a question about what a URL SERVES,
# not about which company hosts it. This used to match youtube/vimeo only, and
# refused a film published as a release asset of this very repository while
# happily passing a youtube link that 404s. Both halves of that were wrong: it
# rejected a working video for its hostname and accepted a dead one for its
# hostname.
VIDEO_RE = re.compile(
    # `*?` and not `+?`: there are ZERO characters between "https://" and
    # "youtu.be/", so requiring at least one made this pattern blind to exactly
    # the host it names first. It found the release asset and not the primary
    # link, which is the failure mode of a check that reports green.
    r"https?://[^\s)>\"]*?"
    r"(?:youtu\.be/[\w?=&/-]+"
    r"|youtube\.com/watch[\w?=&/-]+"
    r"|vimeo\.com/[\w?=&/-]+"
    r"|\.(?:mp4|webm|mov|mkv))",
    re.I)
# Hosts whose players cannot be checked by fetching bytes; reachability is all
# this can honestly assert for them.
PLAYER_RE = re.compile(r"youtu\.be/|youtube\.com/|vimeo\.com/", re.I)


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
        urls = []
        for m in VIDEO_RE.finditer(body):
            u = m.group(0)
            if u not in urls:
                urls.append(u)
        if not urls:
            failures.append(
                "no video URL in %s. The criterion asks for 'a recorded video demo "
                "of the end-to-end flow ... showing terminal output', and the "
                "submission requirement asks the builder to narrate it. A local "
                ".mp4 nobody can open is not a submission." % SUBMISSION)
        else:
            # Present is not the same as reachable, and reachable is not the
            # same as a video. Fetch them AS AN ANONYMOUS READER -- no token,
            # no cookie -- because the failure this is guarding against is a
            # link that works for its owner and serves a login page to the
            # reviewer.
            for u in urls:
                try:
                    req = urllib.request.Request(u, method="GET",
                                                 headers={"Range": "bytes=0-2047"})
                    with urllib.request.urlopen(req, timeout=30) as resp:
                        head = resp.read(2048)
                        ctype = resp.headers.get("Content-Type", "")
                        code = resp.status
                except Exception as exc:
                    failures.append("the video at %s does not load for an "
                                    "anonymous reader: %s" % (u, exc))
                    continue
                if code not in (200, 206):
                    failures.append("the video at %s answers HTTP %s to an "
                                    "anonymous reader" % (u, code))
                elif PLAYER_RE.search(u):
                    notes.append("%s loads (a player page; its bytes cannot be "
                                 "checked from here)" % u)
                elif b"ftyp" in head[:64] or b"\x1a\x45\xdf\xa3" in head[:8] \
                        or ctype.lower().startswith("video/"):
                    notes.append("%s serves a video to an anonymous reader "
                                 "(%s)" % (u, ctype or "container signature"))
                else:
                    failures.append(
                        "%s answers 200 but the first bytes are not a media "
                        "container and the type is %r — that is a page, not a "
                        "film" % (u, ctype))

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

    # 4b. the e2e is green ON THIS BRANCH, not merely green once
    #
    # This gate said "ready to submit" while no green run of the e2e existed on
    # any commit in this branch. Check 4 above passes as long as the links in
    # the document resolve to ancestors — and it did, because the three linked
    # runs are fine. The most expensive criterion in the whole submission,
    # "end-to-end integration tests run against a LEZ sequencer (standalone
    # mode) and are included in CI", had no in-branch evidence at all and
    # nothing here noticed, because nothing here asked.
    #
    # The runs that were green ran on commits a history rewrite orphaned. They
    # are real runs and the document says so, but a reviewer opening this PR
    # cannot get from the submitted tree to any of them. So: at least one
    # success whose head commit is an ancestor of HEAD.
    e2e = sh("gh", "run", "list", "--workflow", "e2e vs local sequencer",
             "--status", "success", "--limit", "30", "--json", "databaseId,headSha")
    if e2e.returncode != 0:
        failures.append("could not ask GitHub about the e2e workflow: "
                        + e2e.stderr.strip()[:120])
    else:
        runs = json.loads(e2e.stdout or "[]")
        good = [r for r in runs
                if sh("git", "merge-base", "--is-ancestor",
                      r["headSha"], "HEAD").returncode == 0]
        if not good:
            failures.append(
                "no GREEN run of 'e2e vs local sequencer' sits on a commit this "
                "branch contains (%d green run(s) exist, all on commits outside "
                "it). That criterion is the one nobody re-runs — the link is the "
                "deliverable, and there is no link to give." % len(runs))
        else:
            notes.append("the e2e is green on this branch: run %s on %s"
                         % (good[0]["databaseId"], good[0]["headSha"][:7]))

    # 4b. and no workflow is currently RED on HEAD.
    #
    # The check above asks whether a GREEN run exists on a commit this branch
    # contains. That is necessary and it is not enough: a workflow can be green on
    # an ancestor and red on HEAD, and the citation still passes while a reviewer
    # opening the Actions tab sees failures. That is precisely what happened —
    # 'alongside the companion modules' went red for three consecutive days on the
    # submission's own head commit, with the criterion it backs citing a green run
    # from five days earlier. The citation was true. The picture was not.
    latest = sh("gh", "run", "list", "--limit", "60", "--json",
                "name,conclusion,headSha,databaseId")
    if latest.returncode != 0:
        failures.append("could not ask GitHub for the latest workflow runs: "
                        + latest.stderr.strip()[:120])
    else:
        head = sh("git", "rev-parse", "HEAD").stdout.strip()
        seen, red = set(), []
        for r in json.loads(latest.stdout or "[]"):
            if r.get("headSha") != head or r["name"] in seen:
                continue
            seen.add(r["name"])          # newest first, so this is the latest run
            if r.get("conclusion") == "failure":
                red.append((r["name"], r["databaseId"]))
        for name, rid in red:
            failures.append(
                "'%s' is RED on HEAD (run %s). A reviewer opening the Actions tab "
                "sees that before reading any citation. Fix it, or say in the "
                "document that it is red and why." % (name, rid))
        if seen and not red:
            notes.append("every workflow that ran on HEAD is green (%d)" % len(seen))
        elif not seen:
            notes.append("no workflow has run on HEAD yet — nothing to contradict, "
                         "and nothing to show either")

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
