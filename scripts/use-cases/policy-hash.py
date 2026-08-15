#!/usr/bin/env python3
"""The commitment a policy account's address is derived from.

    scripts/use-cases/policy-hash.py <owner-hex> <agent-hex> \\
        <per_tx> <per_period> <period_blocks>

    scripts/use-cases/policy-hash.py --self-check artifacts/agents.tsv

`sha256("/lp-0008/v0.1/AgentPolicy/" || owner || agent || per_tx_le16 ||
per_period_le16 || period_blocks_le8)` — every field folded in, so no two
envelopes share an address and a policy cannot be edited in place. Raising a
limit does not change an account, it names a different one.

WHY THIS EXISTS RATHER THAN A CALL INTO THE CRATE

A second implementation of a hash is normally how two halves of a system drift
apart and a policy account gets created that nothing can spend against. It is
here anyway, for one reason and with one safeguard.

The reason: `scripts/use-cases/03-spending-threshold.sh` has to derive the
address of envelopes that were never anchored — the ceiling raised tenfold, the
period shortened — and it must keep being able to do that when the crate's
example binaries are reorganised. A use-case script that stops running because a
`--example` target was renamed is not evidence of anything.

The safeguard: `--self-check` recomputes the hash of EVERY policy in
`artifacts/agents.tsv` from that row's own owner, agent and limits, and fails if
any of them disagrees. Those three hashes are the addresses of accounts that are
on the public testnet and owned by the policy program, so agreeing with all
three is agreeing with the code that actually anchored them. The script runs the
self-check before it trusts a single derived address, and the crate's own
example is cross-checked against this file too whenever the crate ships one.

So drift is not silently absorbed here; it is a failed run.
"""

import hashlib
import sys

PREFIX = b"/lp-0008/v0.1/AgentPolicy/"


def policy_hash(owner: bytes, agent: bytes, per_tx: int, per_period: int, period_blocks: int) -> str:
    if len(owner) != 32 or len(agent) != 32:
        raise SystemExit("owner and agent must each be 32 bytes")
    h = hashlib.sha256()
    h.update(PREFIX)
    h.update(owner)
    h.update(agent)
    h.update(per_tx.to_bytes(16, "little"))         # u128
    h.update(per_period.to_bytes(16, "little"))     # u128
    h.update(period_blocks.to_bytes(8, "little"))   # u64
    return h.hexdigest()


B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58_decode32(s: str) -> bytes:
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    b = b"\x00" * (len(s) - len(s.lstrip("1"))) + b
    if len(b) != 32:
        raise SystemExit(f"not a 32-byte account id: {s!r}")
    return b


def self_check(path: str) -> int:
    with open(path) as f:
        rows = [line.rstrip("\n").split("\t") for line in f if line.strip()]
    header, body = rows[0], rows[1:]
    col = {name: i for i, name in enumerate(header)}
    for need in ("category", "agent_id", "policy_hash", "per_tx", "per_period",
                 "period_blocks", "owner"):
        if need not in col:
            raise SystemExit(f"{path} has no column {need!r}")
    if not body:
        raise SystemExit(f"{path} records no policy to check against")
    failed = 0
    for r in body:
        got = policy_hash(
            b58_decode32(r[col["owner"]]),
            b58_decode32(r[col["agent_id"]]),
            int(r[col["per_tx"]]),
            int(r[col["per_period"]]),
            int(r[col["period_blocks"]]),
        )
        want = r[col["policy_hash"]]
        mark = "ok  " if got == want else "DRIFT"
        print(f"  {mark} {r[col['category']]:<11} {want}")
        if got != want:
            print(f"       this file computes  {got}", file=sys.stderr)
            failed += 1
    if failed:
        print(
            f"{failed} of {len(body)} anchored policies do not match this derivation",
            file=sys.stderr,
        )
        return 1
    return 0


def main() -> int:
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "--self-check":
        return self_check(a[1])
    if len(a) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    print(policy_hash(bytes.fromhex(a[0]), bytes.fromhex(a[1]),
                      int(a[2]), int(a[3]), int(a[4])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
