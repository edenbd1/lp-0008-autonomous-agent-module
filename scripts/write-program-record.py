#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Record what `artifacts/programs/agent_verifier.bin` was built from.

WHY THIS EXISTS

Three binary artefacts in this repository carry a provenance record —
`module/agent.lgx.sources`, `app/agent-ui.lgx.sources` and
`module/harness/harness.sources` — and each is checked, so editing a source
without rebuilding the thing it goes into turns a gate red.

The guest program had none, and it is the one that matters most: it is deployed
on the public testnet, its ImageID is anchored in every policy account, and
`crates/agent-policy-core` — the crate whose 24 tests are cited as the evidence
that the spending limit holds — is compiled INTO it.

What was verified before this existed:

    SHA256(u32_le(len) || bytecode) of artifacts/programs/agent_verifier.bin
      == the deploy transaction the chain reports

which proves the committed BINARY is the program on chain. It says nothing about
whether that binary was built from the source sitting next to it. Editing
`agent-policy-core/src/lib.rs` and pushing would leave every gate green while
the deployed program no longer matched the code a reviewer reads — and the
reviewer would be reading the code to decide whether the limit holds.

That is the defect this repository keeps finding in other people's work and had
one of, at the point of highest consequence.

WHAT IT CANNOT DO

It cannot rebuild the guest to compare. That needs the risc0 docker toolchain
and is a local operation, which is exactly why the binary is committed. So this
records content hashes and the checker compares them: it detects a source that
moved after the build, which is the failure that can happen silently. It does
not detect a binary built from something other than what was recorded, which
would take a reproducible rebuild.

Stated rather than implied, because a provenance record that is read as proof of
reproduction is worse than none.

    ./scripts/write-program-record.py
"""
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = "artifacts/programs/agent_verifier.bin"
RECORD = BINARY + ".sources"

# Everything the guest ELF is compiled from: its own crate, and the path
# dependency that carries the policy decision. Enumerated from git rather than
# globbed, so a file that is present but untracked cannot silently take part.
SOURCE_DIRS = ["crates/agent-verifier-spel/methods/guest",
               "crates/agent-policy-core"]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def tracked_sources():
    out = subprocess.run(["git", "ls-files", *SOURCE_DIRS],
                         cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        print("git ls-files failed: %s" % out.stderr.strip(), file=sys.stderr)
        sys.exit(1)
    files = [f for f in out.stdout.split("\n")
             if f.endswith((".rs", ".toml", ".lock"))]
    # examples/ are not compiled into the guest; excluding them keeps the record
    # about the deployed program rather than about the crate.
    return sorted(f for f in files if "/examples/" not in f)


def build_record():
    files = tracked_sources()
    if not files:
        print("no tracked sources found under %s — the record would claim the "
              "program was built from nothing" % ", ".join(SOURCE_DIRS),
              file=sys.stderr)
        sys.exit(1)
    bin_path = os.path.join(ROOT, BINARY)
    if not os.path.isfile(bin_path):
        print("%s is missing; there is nothing to record" % BINARY, file=sys.stderr)
        sys.exit(1)
    raw = open(bin_path, "rb").read()
    deploy = hashlib.sha256(len(raw).to_bytes(4, "little") + raw).hexdigest()
    return {
        "_comment": [
            "Written by scripts/write-program-record.py; checked by the same",
            "script with --check. It records the sources the deployed guest",
            "program was built from, so that editing one without rebuilding",
            "the binary is caught rather than silent.",
            "",
            "It does NOT prove the binary was produced by these sources: that",
            "needs the risc0 docker toolchain and a reproducible rebuild, which",
            "is a local operation. It proves the sources have not moved since",
            "the record was written.",
        ],
        "binary": BINARY,
        "binary_sha256": hashlib.sha256(raw).hexdigest(),
        "binary_bytes": len(raw),
        "deploy_transaction": deploy,
        "deploy_transaction_note":
            "SHA256(u32_le(len) || bytecode) — a LEZ deploy hash is content "
            "addressed, so this is derivable from the bytes and is checked "
            "against the chain by scripts/verify-deployment.sh",
        "built_from": {f: sha256_file(os.path.join(ROOT, f)) for f in files},
    }


def main(argv):
    check = "--check" in argv[1:]
    fresh = build_record()
    path = os.path.join(ROOT, RECORD)

    if not check:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(fresh, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("recorded %d source file(s) for %s"
              % (len(fresh["built_from"]), BINARY))
        print("  binary   %s (%d bytes)"
              % (fresh["binary_sha256"][:16] + "…", fresh["binary_bytes"]))
        print("  deploy   %s" % fresh["deploy_transaction"])
        return 0

    if not os.path.exists(path):
        print("%s does not exist. Run ./scripts/write-program-record.py" % RECORD)
        return 1
    with open(path, encoding="utf-8") as fh:
        have = json.load(fh)

    failures = []
    if have.get("binary_sha256") != fresh["binary_sha256"]:
        failures.append(
            "%s has changed since the record was written (recorded %s, on disk "
            "%s). Rebuild the record if the rebuild was deliberate."
            % (BINARY, str(have.get("binary_sha256"))[:16] + "…",
               fresh["binary_sha256"][:16] + "…"))
    if have.get("deploy_transaction") != fresh["deploy_transaction"]:
        failures.append(
            "the deploy transaction derived from the committed bytes is %s, "
            "and the record says %s"
            % (fresh["deploy_transaction"], have.get("deploy_transaction")))

    old, new = have.get("built_from", {}), fresh["built_from"]
    for f in sorted(set(old) | set(new)):
        if f not in old:
            failures.append("%s is a source of the guest program and is not in "
                            "the record — it was added after the build" % f)
        elif f not in new:
            failures.append("%s is in the record and no longer tracked — it was "
                            "removed after the build" % f)
        elif old[f] != new[f]:
            failures.append(
                "%s has been edited since the deployed binary was built. The "
                "program on chain no longer matches this file." % f)

    print("checked %d recorded source(s) against the tree" % len(old))
    if failures:
        print()
        for f in failures:
            print("  " + f)
        print()
        print("The deployed program's ImageID is anchored in every policy "
              "account, so a source that has moved past it is a document\n"
              "describing something the chain is not running.")
        return 1
    print("every source the deployed program was built from is unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
