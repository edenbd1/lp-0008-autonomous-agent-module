#!/usr/bin/env python3
"""The notary key for a content address, derived so anyone else can derive it.

    notary-key.py <content-address>          -> secret= and pubkey= lines
    notary-key.py --verify <content-address> --pubkey <hex>

A notarisation has to be checkable by a third party, and the thing that makes it
checkable is that the key which signs it is a FUNCTION of the document. So:

    secret = sha256("lp0008-notary/v1" || content_address)   reduced mod n
    pubkey = x-only secp256k1 public key of that secret      (BIP-340)

and the public key is what ends up on chain, in the witness of the transaction
that records the notarisation. A verifier who holds the document recomputes the
content address, recomputes this key, and looks for it in the transaction the
notary points at. Nobody who lacks the document can produce a transaction the
sequencer will accept under that key, because accepting it requires a signature
under it.

The chain never sees the document or its content address — only a public key,
which is 32 bytes that reveal nothing without a preimage. That is the "private"
half of "verifiable, private proof of existence".

secp256k1 is implemented here rather than imported. The point of this file is
that the derivation is reproducible from the specification by anyone, and a
dependency on a library the verifier has to install is a worse answer than
thirty lines of modular arithmetic they can read.
"""
import argparse
import hashlib
import sys

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)
DOMAIN = b"lp0008-notary/v1"


def point_add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    if p[0] == q[0] and (p[1] + q[1]) % P == 0:
        return None
    if p == q:
        lam = 3 * p[0] * p[0] * pow(2 * p[1], P - 2, P) % P
    else:
        lam = (q[1] - p[1]) * pow(q[0] - p[0], P - 2, P) % P
    x = (lam * lam - p[0] - q[0]) % P
    return (x, (lam * (p[0] - x) - p[1]) % P)


def point_mul(k):
    r, acc = None, G
    while k:
        if k & 1:
            r = point_add(r, acc)
        acc = point_add(acc, acc)
        k >>= 1
    return r


def derive(cid):
    """(secret, x-only public key) for a content address, both 32 bytes."""
    seed = hashlib.sha256(DOMAIN + b"|" + cid.encode()).digest()
    k = int.from_bytes(seed, "big") % N
    if k == 0:
        raise ValueError("degenerate secret")
    return seed, point_mul(k)[0].to_bytes(32, "big")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cid")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--pubkey")
    a = ap.parse_args()

    secret, pub = derive(a.cid)
    if a.verify:
        if not a.pubkey:
            sys.exit("--verify needs --pubkey")
        # Compared as bytes, not as strings, so case or a stray 0x cannot make
        # two different keys look equal.
        got = bytes.fromhex(a.pubkey.removeprefix("0x"))
        print("match=%d" % (1 if got == pub else 0))
        print("expected=%s" % pub.hex())
        return 0 if got == pub else 1
    print("secret=%s" % secret.hex())
    print("pubkey=%s" % pub.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
