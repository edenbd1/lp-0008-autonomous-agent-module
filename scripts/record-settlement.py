#!/usr/bin/env python3
"""Check a settlement against its own bytes, then record it — in that order.

    scripts/record-settlement.py --tx <hash> --client <b58> --server <b58> \
        --recipient <b58> --policy <b58> --price N --before N [--skill ID]

WHY THIS IS NOT THREE LINES IN THE RUNNER

`scripts/delivery-in-plugin.sh settle` finishes with a harness that asserted its
signer returned a transaction hash, and a signer that asserted a block holds it.
Neither of those is *the money moved*. A confirmed, on-chain proof that a policy
PERMITTED a payment and moved nothing is a thing this repository has produced
before and written up as a settlement, which is why the last word belongs to the
transaction's own committed post-state and not to any line either process
printed.

So this decodes the payee's balance out of the settlement — via
`scripts/use-cases/settlement-facts.py`, which verifies that the bytes hash to
the hash it was given before it decodes anything — and appends a row to
`artifacts/a2a-task.tsv` only if that balance rose by exactly the price. A run
that cannot show the movement writes nothing and exits non-zero, leaving any
previous manifest intact.

It is a command rather than a here-document inside the runner for one reason
worth stating: the runner's settle branch takes about half an hour and costs a
real settlement, so a bug in its last fifteen lines would be found once, in the
one run that could not be repeated. This can be run against a hash that already
landed.

TWO THINGS IT REFUSES

- A transaction already in the manifest. Re-running the recorder must not turn
  one settlement into two rows; `submission-evidence.py` would then render the
  same payment twice and the table would overcount.
- A header it does not write. An unrecognised header is a manifest from an older
  column layout, and appending to it puts values under the wrong names — which
  is the class of fault that has produced three separate false results here.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FACTS = os.path.join(ROOT, "scripts/use-cases/settlement-facts.py")

HEADER = ("program", "task_id", "client", "server", "server_pay_account",
          "skill", "price", "nonce", "settlement_tx", "balance_before",
          "balance_after")


def die(why):
    print("record-settlement: " + why, file=sys.stderr)
    sys.exit(1)


def program_id(path):
    """The deploy transaction hash of a program binary, as the chain forms it."""
    import hashlib
    import struct
    with open(path, "rb") as fh:
        body = fh.read()
    return hashlib.sha256(struct.pack("<I", len(body)) + body).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tx", required=True)
    ap.add_argument("--client", required=True, help="the paying agent, base58")
    ap.add_argument("--server", required=True, help="the paid agent, base58")
    ap.add_argument("--recipient", required=True,
                    help="the public account actually credited, base58")
    ap.add_argument("--policy", required=True,
                    help="the payer's anchored policy account, base58")
    ap.add_argument("--price", type=int, required=True)
    ap.add_argument("--before", type=int, required=True,
                    help="what the recipient held before this settlement. It can "
                         "only be captured beforehand: this chain has no "
                         "historical-state RPC at all")
    ap.add_argument("--task-id", default="")
    ap.add_argument("--skill", default="storage.upload")
    ap.add_argument("--program",
                    default=os.path.join(ROOT, "artifacts/programs/agent_verifier.bin"))
    ap.add_argument("--manifest",
                    default=os.environ.get("A2A_MANIFEST",
                                           os.path.join(ROOT, "artifacts/a2a-task.tsv")))
    ap.add_argument("--dry-run", action="store_true",
                    help="check the transaction and print the row, write nothing")
    args = ap.parse_args()

    facts = subprocess.run(
        [sys.executable, FACTS, "--tx", args.tx,
         "--recipient", args.recipient, "--policy", args.policy],
        capture_output=True, text=True)
    sys.stderr.write(facts.stderr)
    for line in facts.stdout.splitlines():
        print("  " + line, file=sys.stderr)
    if facts.returncode != 0:
        die("the settlement's own bytes could not be read, so nothing is recorded")
    kv = dict(l.split("=", 1) for l in facts.stdout.splitlines() if "=" in l)

    if kv.get("hash_ok") != "1":
        die("the bytes the sequencer returned are not the transaction %s names"
            % args.tx)
    if kv.get("recipient_found") != "1":
        die("%s is not in the post-state of %s: this transaction did not touch "
            "the account it is supposed to have paid" % (args.recipient, args.tx))
    after = int(kv["recipient_balance"])
    if after - args.before != args.price:
        die("the payee held %d and this settlement's own post-state says %d — "
            "that is a movement of %d, and the price was %d. Recording it would "
            "record a payment the chain did not make."
            % (args.before, after, after - args.before, args.price))

    row = (program_id(args.program), args.task_id or args.tx[:16], args.client,
           args.server, args.recipient, args.skill, str(args.price), "none",
           args.tx, str(args.before), str(after))
    line = "\t".join(row)

    if os.path.exists(args.manifest) and os.path.getsize(args.manifest):
        with open(args.manifest, encoding="utf-8") as fh:
            existing = fh.read().splitlines()
        if existing[0].split("\t") != list(HEADER):
            die("%s has a header this script does not write:\n  %s\nMigrate it, "
                "or move it aside — appending under the wrong names is how three "
                "false results got into this repository." % (args.manifest, existing[0]))
        if any(args.tx in l for l in existing[1:]):
            die("%s is already in %s. One settlement is one row; recording it "
                "twice would make the generated table overcount."
                % (args.tx, args.manifest))
        body = None
    else:
        body = "\t".join(HEADER) + "\n"

    if args.dry_run:
        print("would append: " + line, file=sys.stderr)
        print(after)
        return

    with open(args.manifest, "a", encoding="utf-8") as fh:
        if body:
            fh.write(body)
        fh.write(line + "\n")
    print("record-settlement: %s -> %s, %d -> %d, appended to %s"
          % (args.tx[:16], args.recipient, args.before, after, args.manifest),
          file=sys.stderr)
    print(after)


if __name__ == "__main__":
    main()
