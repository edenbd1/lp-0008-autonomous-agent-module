#!/usr/bin/env python3
"""The agent module's `policy_source` and `pay_signer`, in one file.

    scripts/agent-spend.py --policy <b58> --envelope
    scripts/agent-spend.py --policy <b58> --wallet-home DIR --agent <b58> \
        --max-amount N --settle   < '{"recipient":"Public/<b58>","amount":"1"}'

WHY A COMMAND AND NOT A LIBRARY

The module is a 4 MB Qt plugin that Basecamp drops into a directory. It links no
crypto library, no wallet and no HTTP client, and `SkillPorts` is a struct of
`std::function`s, so a host that LOADS it over Qt Remote Objects cannot hand it
any of those either. `card_signer` settled the shape of the answer: name a
command in `meta.configure`, put the input on its stdin, read one line back, and
check that line character by character before believing it. `agent.card` produces
a real BIP-340 signature that way from inside a loaded plugin.

These two modes are the same shape with the chain on the other end. `--envelope`
is a read: it tells the module what its owner anchored, so `agent.task` can see
that a price is INSIDE the envelope and pay it unattended instead of holding it
for an owner. `--settle` is the payment, and it is the policy program's `spend`
instruction — the same call `scripts/a2a-task.sh` makes, the same one whose
limits the chain applies. There is no path here to `authenticated_transfer` that
does not go through the anchored policy account first.

WHAT `--settle` REFUSES TO DO

- Spend more than `--max-amount`. The envelope is enforced on chain and checked
  again below against the chain's own numbers, and this is still here: it is the
  one bound that holds even if the policy account is misread, and the wallet it
  guards holds a handful of testnet LEZ that cannot be replaced.
- Spend without resyncing first. A private account's state moves every time it
  signs, and a proof built against a stale local view still produces a
  transaction hash — one the sequencer silently never lands. That failure looks
  exactly like an intermittent network fault and is not one.
- Print a hash that is not in a block. `TaskPort::pay` is documented as
  returning "the settlement transaction hash, or empty if nothing settled", and
  `agent.task` writes what comes back into the task record as proof of payment.
  `getTransaction` answers the same null for a dropped, a pending, a rejected and
  a never-submitted hash, so a hash printed before inclusion is a claim nobody
  can check. Inclusion is confirmed here, and only then is the hash printed.

Everything that is not the answer goes to stderr. Stdout carries one line: the
policy JSON, or the transaction hash.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def note(*a):
    print(*a, file=sys.stderr)
    sys.stderr.flush()


def die(why):
    note("agent-spend: " + why)
    sys.exit(1)


def rpc(url, method, params):
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    ).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        answer = json.load(r)
    if "error" in answer:
        raise RuntimeError("%s: %s" % (method, answer["error"]))
    return answer.get("result")


# ── the anchored policy account, decoded exactly as the program wrote it ──────
#
# Layout, v1: version(1), owner(32), per_tx(u128 LE), per_period(u128 LE),
# period_blocks(u64 LE), window_start(u64 LE), spent(u128 LE) = 97 bytes. Found
# by SHAPE, never by trusting the address to hold what we expect: an account that
# holds something else is a policy this build cannot read, and reporting a
# guessed number from it is how a limit becomes decorative.
POLICY_RECORD_BYTES = 97


def window_start_for(block, period_blocks):
    """The period a block falls in — `SpendPolicy::window_start_for`.

    Eight characters of arithmetic, and it is consensus: the guest refuses a
    `window_start` that is not a multiple of `period_blocks`, and pins the
    transaction's block validity window to `[start, start + period_blocks)`. The
    vectors below are the ones in `crates/agent-policy-core/src/lib.rs`'s own
    tests, so this implementation and the one the chain links are checked against
    the same numbers rather than against each other's confidence.
    """
    if period_blocks == 0:
        return 0
    return (block // period_blocks) * period_blocks


def selftest():
    for block, period, expected in ((8629, 1000, 8000), (8000, 1000, 8000),
                                    (7999, 1000, 7000), (12345, 0, 0)):
        got = window_start_for(block, period)
        assert got == expected, (
            "window_start_for(%d, %d) = %d, and the crate the guest links says %d"
            % (block, period, got, expected))


def read_policy(url, policy_account):
    """`per_tx`, `per_period`, `period_blocks`, `window_start`, `spent`, `block`,
    plus `policy`, `owner` and `record_window_start` — nine keys, not six.

    `record_window_start` is the one that must not be dropped from this list,
    because the paragraph below is entirely about the difference between it and
    `window_start`: the first is what the account literally holds, the second is
    the period this read is answering for.

    `spent` is what has been spent IN THE CURRENT PERIOD, which is not always
    what the account holds: `SpendPolicy::authorize` treats a later window as a
    fresh budget and starts its total at zero, so a record whose `window_start`
    is behind the current period has already been superseded. Reporting its stale
    total as this period's would make the agent hold payments for its owner that
    the chain would have let it make — the safe direction, but the wrong number,
    and "conservative" is not a licence to report something untrue.
    """
    account = rpc(url, "getAccount", [policy_account])
    if not account:
        die("the sequencer has no account %s — is this the policy account?"
            % policy_account)
    data = bytes(account.get("data") or [])
    if len(data) != POLICY_RECORD_BYTES or data[0] != 1:
        die("account %s holds %d bytes starting %s, not a v1 policy record this "
            "build can read" % (policy_account, len(data), data[:1].hex() or "''"))

    def le(a, b):
        return int.from_bytes(data[a:b], "little")

    period_blocks = le(65, 73)
    if period_blocks == 0:
        die("the anchored policy has a period of zero blocks, which the chain "
            "refuses every spend under")
    block = rpc(url, "getLastBlockId", [])
    if not isinstance(block, int):
        die("the sequencer did not answer getLastBlockId with a height")
    record_window = le(73, 81)
    current_window = window_start_for(block, period_blocks)
    if current_window < record_window:
        # The chain refuses `window_start < ledger.window_start` outright
        # (`WindowRegressed`), so this is not a spend that would merely be
        # accounted oddly — it is one no block will take.
        die("the chain is at block %d, in period %d, and the policy account "
            "already records period %d: this node is behind the policy"
            % (block, current_window, record_window))
    return {
        "policy": policy_account,
        "owner": data[1:33].hex(),
        "per_tx": str(le(33, 49)),
        "per_period": str(le(49, 65)),
        "period_blocks": str(period_blocks),
        "window_start": str(current_window),
        "record_window_start": str(record_window),
        "spent": str(le(81, 97) if current_window == record_window else 0),
        "block": str(block),
    }


# ── the settlement ───────────────────────────────────────────────────────────


def settle(args, policy):
    request = sys.stdin.read().strip()
    if not request:
        die("no payment request on stdin")
    try:
        parsed = json.loads(request)
    except ValueError as e:
        die("the payment request is not JSON: %s" % e)
    if not isinstance(parsed, dict):
        die("the payment request is not a JSON object")
    recipient = parsed.get("recipient")
    amount = parsed.get("amount")
    if not isinstance(recipient, str) or not isinstance(amount, str):
        die("a payment request needs 'recipient' and 'amount', both strings")

    # `Public/<id>` is the form `spel` resolves and the form the module's Agent
    # Card advertises. Refused rather than coerced: a private recipient cannot be
    # paid by a wallet that does not hold its spending key, and `spel` answers
    # that with a resolution failure minutes into a proof.
    if not recipient.startswith("Public/"):
        die("the recipient must be a Public/<id> account, not %r — a shielded "
            "recipient is not resolvable by the payer's wallet" % recipient)
    account = recipient[len("Public/"):]
    if not account or any(c not in B58 for c in account):
        die("the recipient account %r is not base58" % account)
    if not re.fullmatch(r"[0-9]+", amount):
        die("the amount %r is not a non-negative decimal integer" % amount)
    amount = int(amount)
    if amount == 0:
        die("refusing to prove a transfer of zero")
    if amount > args.max_amount:
        die("refusing to settle %d LEZ: this signer is capped at %d by "
            "--max-amount" % (amount, args.max_amount))

    per_tx = int(policy["per_tx"])
    per_period = int(policy["per_period"])
    spent = int(policy["spent"])
    if amount > per_tx:
        die("%d LEZ is over the anchored per-transaction limit of %d; the chain "
            "would refuse this and the proof is not free"
            % (amount, per_tx))
    if amount > per_period - spent:
        die("%d LEZ is over what is left of the anchored per-period limit of %d "
            "(%d already spent this period)" % (amount, per_period, spent))

    window_start = int(policy["window_start"])
    block = int(policy["block"])
    period_blocks = int(policy["period_blocks"])
    left = window_start + period_blocks - block
    if left < 3:
        die("only %d blocks left in period [%d, %d): a settlement that slips "
            "past the end is refused as out of its window, and this one takes "
            "minutes to prove" % (left, window_start, window_start + period_blocks))

    env = dict(os.environ)
    env["LEE_WALLET_HOME_DIR"] = args.wallet_home
    env["NSSA_WALLET_HOME_DIR"] = args.wallet_home

    # THE RESYNC, WHICH IS NOT OPTIONAL.
    #
    # `scripts/a2a-task.sh` only warns when it cannot do this. Here it is fatal,
    # because the module records what this prints as a payment: a proof built
    # against a stale private view produces a hash that is never included, and
    # nothing anywhere reports an error.
    note("agent-spend: resyncing %s" % args.wallet_home)
    synced = subprocess.run([args.wallet, "account", "sync-private"],
                            env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True)
    if synced.returncode != 0:
        die("the wallet could not resync, so any proof built now would be "
            "against a stale private state:\n" + (synced.stderr or synced.stdout))
    for line in (synced.stdout or "").splitlines():
        if "synced to block" in line.lower():
            note("agent-spend: " + line.strip())

    cmd = [args.spel, "--idl", args.idl, "--program", args.program,
           "--bin-auth-transfer", args.auth_transfer,
           "--", "spend",
           "--agent", "Private/" + args.agent,
           "--recipient", recipient,
           "--amount", str(amount),
           "--window-start", str(window_start)]
    note("agent-spend: %d LEZ -> %s, window %d, policy %s"
         % (amount, recipient, window_start, policy["policy"]))
    note("agent-spend: " + " ".join(cmd))
    started = time.time()
    proof = subprocess.run(cmd, env=env, cwd=ROOT, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True)
    elapsed = time.time() - started
    output = (proof.stdout or "") + (proof.stderr or "")
    note("agent-spend: spel exited %d after %.0f s" % (proof.returncode, elapsed))
    for line in output.splitlines()[-25:]:
        note("  | " + line)
    found = re.search(r"tx_hash: ([0-9a-f]{64})", output)
    if not found:
        die("spel submitted no transaction, so nothing was paid")
    tx = found.group(1)
    note("agent-spend: submitted %s" % tx)

    # A submitted hash is not a settlement. Blocks are 60 seconds apart, and a
    # hash that never lands is indistinguishable from one that is still waiting,
    # so this waits long enough that absence means something.
    for _ in range(args.confirm_polls):
        time.sleep(args.confirm_interval)
        try:
            result = rpc(args.sequencer, "getTransaction", [tx])
        except Exception as e:            # a poll that fails is not a verdict
            note("agent-spend: getTransaction: %s" % e)
            continue
        if isinstance(result, list) and result:
            note("agent-spend: %s is in block %s" % (tx, result[1]))
            print(tx)
            return
        note("agent-spend: %s not in a block yet" % tx)
    die("%s never landed. It is NOT reported as a settlement: the sequencer "
        "drops a failing transaction silently and answers null for a dropped, a "
        "pending and a rejected hash alike." % tx)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--policy", required=True,
                    help="the agent's anchored policy account, base58")
    ap.add_argument("--sequencer", default=os.environ.get(
        "SEQUENCER_URL", "https://testnet.lez.logos.co"))
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--envelope", action="store_true",
                      help="print the anchored envelope as JSON (policy_source)")
    mode.add_argument("--settle", action="store_true",
                      help="read a payment request on stdin, settle it, print "
                           "the confirmed transaction hash (pay_signer)")
    ap.add_argument("--wallet-home", help="the PAYING agent's wallet home")
    ap.add_argument("--agent", help="the paying agent's own account, base58")
    ap.add_argument("--max-amount", type=int,
                    help="hard ceiling on one settlement, in LEZ")
    ap.add_argument("--spel", default=os.environ.get("SPEL_BIN", "spel"))
    ap.add_argument("--wallet", default=os.environ.get("WALLET_BIN", "wallet"))
    ap.add_argument("--idl", default=os.path.join(ROOT, "idl/agent_verifier.idl.json"))
    ap.add_argument("--program",
                    default=os.path.join(ROOT, "artifacts/programs/agent_verifier.bin"))
    ap.add_argument("--auth-transfer",
                    default=os.path.join(ROOT,
                                         "artifacts/programs/authenticated_transfer.bin"))
    ap.add_argument("--confirm-polls", type=int, default=24)
    ap.add_argument("--confirm-interval", type=int, default=30)
    args = ap.parse_args()

    selftest()

    policy = read_policy(args.sequencer, args.policy)
    if args.envelope:
        print(json.dumps(policy, sort_keys=True, separators=(",", ":")))
        return

    for name, value in (("--wallet-home", args.wallet_home),
                        ("--agent", args.agent),
                        ("--max-amount", args.max_amount)):
        if value is None:
            die("--settle needs %s" % name)
    if not os.path.isdir(args.wallet_home):
        die("no wallet home at %s" % args.wallet_home)
    for path in (args.idl, args.program, args.auth_transfer):
        if not os.path.isfile(path):
            die("missing %s" % path)
    settle(args, policy)


if __name__ == "__main__":
    main()
