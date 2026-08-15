#!/usr/bin/env python3
"""What the chain itself says a settlement did, decoded from the transaction.

    settlement-facts.py --tx <hash> --recipient <b58> [--policy <b58>]

This exists because the interesting claim about a settlement — that money moved
— was being read out of a local TSV. `getAccount` answers with CURRENT state and
this chain has no historical-state RPC at all (`getAccountAtBlock`, `getBalance`
and `getTransactionReceipt` are all "Method not found"), so a balance as it stood
at block 8740 cannot be fetched. That is why the numbers were cached, and caching
them is what made the strongest sentence in use case 2 a comparison between two
fields of a file.

The way out is that the transaction carries the answer itself. A LEZ transaction
is a proof plus the POST-STATE it commits to, and the post-state is a list of
accounts with the balance and data each one holds once the transaction has run.
So the balance the recipient held immediately after a settlement is inside that
settlement, and the chain will hand it over: `getTransaction` returns
`[base64(transaction_bytes), block]`.

Three things make decoding those bytes as good as asking the chain:

  1. The hash. A LEZ transaction hash is sha256 over the bytes with the leading
     kind discriminant dropped, so the bytes are content-addressed by the very
     hash the manifest names. Verified below before anything is decoded: if the
     bytes do not hash to the hash we asked for, nothing here is about that
     settlement and this exits non-zero.

  2. The block. It comes back beside the bytes, so a settlement can name the
     block it is in rather than merely being "not null".

  3. A structural parse that cannot silently drift. The update list is walked
     field by field, and then every account the parse claims to have found is
     checked against where its 32 raw bytes actually occur in the transaction. A
     layout that is off by even one byte produces a disagreement and an error,
     not a plausible wrong number.

Output is `key=value` lines, one per line, to be read by name.
"""
import argparse
import base64
import hashlib
import json
import sys
import urllib.request

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    b = b"\x00" * (len(s) - len(s.lstrip("1"))) + b
    if len(b) != 32:
        raise ValueError("not a 32-byte account id: %r" % s)
    return b


def b58encode(b):
    n = int.from_bytes(b, "big")
    s = ""
    while n:
        n, r = divmod(n, 58)
        s = B58[r] + s
    return "1" * (len(b) - len(b.lstrip(b"\x00"))) + s


def rpc(url, method, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1,
                       "method": method, "params": params}).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r).get("result")


def parse_updates(b):
    """The post-state a transaction commits to.

    Layout, per update: account id (32), the ImageID of the program that owns it
    (32), balance (u128 LE), data length (u32 LE), data, and a trailing u128.
    The header is a kind byte and then the number of updates as a u32.
    """
    out = []
    o = 1                                     # byte 0 is the kind discriminant
    n = int.from_bytes(b[o:o + 4], "little")
    o += 4
    if n == 0 or n > 64:
        raise ValueError("implausible update count %d" % n)
    for _ in range(n):
        acct, o = b[o:o + 32], o + 32
        start = o - 32
        owner, o = b[o:o + 32], o + 32
        bal, o = int.from_bytes(b[o:o + 16], "little"), o + 16
        dl, o = int.from_bytes(b[o:o + 4], "little"), o + 4
        if dl > len(b):
            raise ValueError("update claims %d bytes of data" % dl)
        data, o = b[o:o + dl], o + dl
        o += 16                               # trailer
        if o > len(b):
            raise ValueError("the update list runs past the end of the transaction")
        out.append({"offset": start, "account": acct, "owner": owner,
                    "balance": bal, "data": data})
    return out


def policy_fields(data):
    """The 97 bytes an anchored policy account holds, or None."""
    if len(data) != 97 or data[0] != 1:
        return None
    le = lambda a, b: int.from_bytes(data[a:b], "little")
    # `record_owner` and not `owner`: the account has an owner already — the
    # program that may write it — and one of those two would have silently
    # overwritten the other on the way out.
    return {"record_owner": data[1:33].hex(), "per_tx": le(33, 49),
            "per_period": le(49, 65), "period_blocks": le(65, 73),
            "window_start": le(73, 81), "spent": le(81, 97)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default="https://testnet.lez.logos.co")
    ap.add_argument("--tx", required=True)
    # Both optional: a caller that only wants the block, or that does not know
    # in advance which accounts a transaction touched, gets every update back as
    # an `other=` line instead.
    ap.add_argument("--recipient")
    ap.add_argument("--policy")
    a = ap.parse_args()

    emit = lambda k, v: print("%s=%s" % (k, v))

    res = rpc(a.rpc, "getTransaction", [a.tx])
    # null is the same answer for dropped, pending, rejected and never sent, so
    # it is reported as "not on chain" and nothing further is claimed.
    if not res:
        emit("found", 0)
        return 1
    raw, block = base64.b64decode(res[0]), res[1]
    emit("found", 1)
    emit("block", block)
    emit("bytes", len(raw))

    # Before anything is decoded: are these the bytes this hash names?
    digest = hashlib.sha256(raw[1:]).hexdigest()
    emit("hash_ok", 1 if digest == a.tx else 0)
    if digest != a.tx:
        emit("error", "the_bytes_hash_to_%s" % digest)
        return 1

    try:
        updates = parse_updates(raw)
    except ValueError as e:
        emit("error", str(e).replace(" ", "_"))
        return 1
    emit("updates", len(updates))

    # The parse is only trustworthy if it agrees with the bytes. Every account
    # it claims to have found must occur, exactly once, at the offset it was
    # read from — otherwise the layout has drifted and every number below it is
    # arithmetic on the wrong bytes.
    for u in updates:
        first = raw.find(u["account"])
        if first != u["offset"]:
            emit("error", "update_at_%d_but_its_id_occurs_at_%d" % (u["offset"], first))
            return 1

    # The policy ledger, FOUND rather than named. A settlement's post-state
    # carries exactly one 97-byte record with version byte 1, and that is the
    # ledger the program wrote. Finding it matters because the ledger's ADDRESS
    # moves whenever the program is redeployed — it is a PDA of the program —
    # so `artifacts/a2a-task.tsv` now spans two of them, and a caller that had
    # to name the account in advance would have to know which program each
    # settlement was made under before it could read the settlement.
    #
    # `ledger_account` is emitted so a caller can partition settlements by the
    # ledger they charged: a running total is only comparable against another
    # total from the same account.
    ledger = next((u for u in updates if policy_fields(u["data"])), None)
    if ledger is not None:
        emit("ledger_account", b58encode(ledger["account"]))
        for k, v in policy_fields(ledger["data"]).items():
            emit("ledger_%s" % k, v)

    wanted = {"recipient": a.recipient, "policy": a.policy}
    for name, addr in wanted.items():
        if not addr:
            continue
        target = b58decode(addr)
        hit = next((u for u in updates if u["account"] == target), None)
        if hit is None:
            emit("%s_found" % name, 0)
            continue
        emit("%s_found" % name, 1)
        emit("%s_balance" % name, hit["balance"])
        emit("%s_owner" % name, ",".join(
            str(int.from_bytes(hit["owner"][i:i + 4], "little"))
            for i in range(0, 32, 4)))
        rec = policy_fields(hit["data"])
        if rec:
            for k, v in rec.items():
                emit("%s_%s" % (name, k), v)
    # Anything else the transaction touched, so a settlement that quietly moved a
    # third account cannot pass unremarked.
    named = {b58decode(v) for v in wanted.values() if v}
    for u in updates:
        if u["account"] not in named:
            emit("other", "%s:%d" % (b58encode(u["account"]), u["balance"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
