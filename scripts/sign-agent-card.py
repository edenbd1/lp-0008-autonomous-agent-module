#!/usr/bin/env python3
"""Sign an A2A Agent Card with the agent's own LEZ account key.

    scripts/sign-agent-card.py --wallet-home DIR --account BASE58 < card.json > signed.json

A2A carries a card signature as an RFC 7515 JWS with a *detached* payload: the
payload is the card without its `signatures`, so a verifier strips them,
re-serialises, and recomputes the signing input. That only works if both sides
serialise the same way, so the payload here is the compact, key-sorted JSON that
`nlohmann::json::dump()` produces in module/src/agent_skills.cpp — a card signed
by this script and a card signed by the module verify by the same procedure.

WHY THE KEY IS THE ACCOUNT KEY

An unsigned card is a document anyone can rewrite, including the payment account
in it, and the whole point of the card is to tell another agent where to send
money. The key that signs it is therefore the same key that owns the account
being advertised: LEZ account keys are BIP-340 Schnorr over secp256k1
(`k256::schnorr`, `lee/state_machine/src/signature/mod.rs`), the public half is
the x-only 32 bytes the wallet stores as `pk`, and `kid` is the account id, so a
reader who has the card has everything needed to check it.

`alg` says `secp256k1-bip340` because that is what it is. JOSE registers
`ES256K` for *ECDSA* over the same curve and nothing for Schnorr, so a
registered name here would be a false one.

The signature is deterministic (aux_rand = 0), so re-running produces a
byte-identical card rather than a new one that merely says the same thing.

Before it signs anything this script checks itself against the published BIP-340
test vectors — the same ones LEZ's own signature module tests against
(`bip340_test_vectors.rs`). A signer that quietly produces garbage would
otherwise emit a card that looks signed and verifies nowhere.
"""

import argparse
import base64
import hashlib
import json
import sys

# ── BIP-340, from the reference implementation in the BIP ────────────────────

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (
    0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
    0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8,
)


def tagged_hash(tag, msg):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + msg).digest()


def point_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    if p1[0] == p2[0] and p1[1] != p2[1]:
        return None
    if p1 == p2:
        lam = (3 * p1[0] * p1[0] * pow(2 * p1[1], P - 2, P)) % P
    else:
        lam = ((p2[1] - p1[1]) * pow(p2[0] - p1[0], P - 2, P)) % P
    x3 = (lam * lam - p1[0] - p2[0]) % P
    return (x3, (lam * (p1[0] - x3) - p1[1]) % P)


def point_mul(p, n):
    r = None
    for i in range(256):
        if (n >> i) & 1:
            r = point_add(r, p)
        p = point_add(p, p)
    return r


def lift_x(b):
    x = int.from_bytes(b, "big")
    if x >= P:
        return None
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    return (x, y if y & 1 == 0 else P - y)


def b32(x):
    return x.to_bytes(32, "big")


def pubkey_of(seckey):
    d0 = int.from_bytes(seckey, "big")
    if not 1 <= d0 <= N - 1:
        raise ValueError("the secret key is not a valid secp256k1 scalar")
    return b32(point_mul(G, d0)[0])


def schnorr_sign(msg32, seckey, aux=bytes(32)):
    """Sign a 32-byte prehash, exactly as `Signature::new_with_aux_random` does."""
    d0 = int.from_bytes(seckey, "big")
    if not 1 <= d0 <= N - 1:
        raise ValueError("the secret key is not a valid secp256k1 scalar")
    point = point_mul(G, d0)
    d = d0 if point[1] % 2 == 0 else N - d0
    t = bytes(a ^ b for a, b in zip(b32(d), tagged_hash("BIP0340/aux", aux)))
    rand = tagged_hash("BIP0340/nonce", t + b32(point[0]) + msg32)
    k0 = int.from_bytes(rand, "big") % N
    if k0 == 0:
        raise ValueError("nonce is zero, which has probability ~2^-256")
    r = point_mul(G, k0)
    k = k0 if r[1] % 2 == 0 else N - k0
    e = (
        int.from_bytes(
            tagged_hash("BIP0340/challenge", b32(r[0]) + b32(point[0]) + msg32), "big"
        )
        % N
    )
    return b32(r[0]) + b32((k + e * d) % N)


def schnorr_verify(msg32, pubkey, sig):
    point = lift_x(pubkey)
    if point is None or len(sig) != 64:
        return False
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    if r >= P or s >= N:
        return False
    e = (
        int.from_bytes(
            tagged_hash("BIP0340/challenge", sig[:32] + b32(point[0]) + msg32), "big"
        )
        % N
    )
    rp = point_add(point_mul(G, s), point_mul(point, N - e))
    return rp is not None and rp[1] % 2 == 0 and rp[0] == r


# Published BIP-340 vectors, the same ones LEZ's signature module tests against.
VECTORS = [
    (
        "0000000000000000000000000000000000000000000000000000000000000003",
        "F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "E907831F80848D1069A5371B402410364BDF1C5F8307B0084C55F1CE2DCA821525F66A4A85EA8B71E482A74F382D2CE5EBEEE8FDB2172F477DF4900D310536C0",
    ),
    (
        "B7E151628AED2A6ABF7158809CF4F3C762E7160F38B4DA56A784D9045190CFEF",
        "DFF1D77F2A671C5F36183726DB2341BE58FEAE1DA2DECED843240F7B502BA659",
        "0000000000000000000000000000000000000000000000000000000000000001",
        "243F6A8885A308D313198A2E03707344A4093822299F31D0082EFA98EC4E6C89",
        "6896BD60EEAE296DB48A229FF71DFE071BDE413E6D43F917DC8DCF8C78DE33418906D11AC976ABCCB20B091292BFF4EA897EFCB639EA871CFA95F6DE339E4B0A",
    ),
]


def selftest():
    for sec, pub, aux, msg, sig in VECTORS:
        sec, pub, aux, msg, sig = (bytes.fromhex(x) for x in (sec, pub, aux, msg, sig))
        assert pubkey_of(sec) == pub, "BIP-340 self-test: public key mismatch"
        assert schnorr_sign(msg, sec, aux) == sig, "BIP-340 self-test: signature mismatch"
        assert schnorr_verify(msg, pub, sig), "BIP-340 self-test: verification failed"
        # One flipped bit in the message. Not a fixed wrong message: the first
        # published vector signs 32 zero bytes, so "verify against zeros" is a
        # check that passes for the wrong reason exactly once.
        tampered = bytes([msg[0] ^ 1]) + msg[1:]
        assert not schnorr_verify(tampered, pub, sig), "BIP-340 self-test: verified a lie"


# ── the card ─────────────────────────────────────────────────────────────────


def b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def account_secret(wallet_home, account):
    """The signing key the wallet holds for this public account.

    The key that signs is `ssk`, not `sk`: a public account's stored `sk` is the
    node in the wallet's key tree, and the signing key is its tweak —
    `ssk = PrivateKey::tweak(sk)`, `pk = PublicKey::new_from_private_key(&ssk)`
    in `lee/key_protocol/.../keys_public.rs`. Reading the wrong one of the two
    produces a perfectly well-formed signature that verifies against nothing,
    which is why this asserts the derivation rather than assuming it.
    """
    with open(f"{wallet_home}/storage.json") as f:
        storage = json.load(f)
    for entry in storage["key_chain"]["accounts"]:
        pub = entry.get("Public")
        if pub and pub["account_id"] == account:
            sec = bytes.fromhex(pub["data"]["ssk"])
            expected = bytes.fromhex(pub["data"]["pk"])
            if pubkey_of(sec) != expected:
                raise SystemExit(
                    f"the key stored for {account} does not derive its own public key"
                )
            return sec, expected
    raise SystemExit(f"the wallet at {wallet_home} holds no key for {account}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wallet-home", required=True)
    ap.add_argument("--account", required=True, help="the public account id, base58")
    ap.add_argument(
        "--sign-input",
        action="store_true",
        help="read a JWS signing input (`<protected>.<payload>`) on stdin and write "
        "the base64url signature — the operation the module's CardPort.sign "
        "expects a host to provide, so the module can assemble and sign a real "
        "card without a second copy of the curve arithmetic living anywhere",
    )
    args = ap.parse_args()

    selftest()

    if args.sign_input:
        # The module has already built the protected header and the payload; all
        # that is delegated here is the key operation, which is the whole point
        # of `CardPort.sign` being an injected function.
        signing_input = sys.stdin.read().strip().encode()
        secret, public = account_secret(args.wallet_home, args.account)
        digest = hashlib.sha256(signing_input).digest()
        signature = schnorr_sign(digest, secret)
        if not schnorr_verify(digest, public, signature):
            raise SystemExit("the signature this script just produced does not verify")
        sys.stdout.write(b64url(signature))
        return

    card = json.load(sys.stdin)
    card.pop("signatures", None)
    secret, public = account_secret(args.wallet_home, args.account)

    protected = b64url(
        json.dumps(
            {"alg": "secp256k1-bip340", "kid": args.account},
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    )
    payload = b64url(json.dumps(card, sort_keys=True, separators=(",", ":")).encode())
    signing_input = f"{protected}.{payload}".encode()
    digest = hashlib.sha256(signing_input).digest()
    signature = schnorr_sign(digest, secret)
    if not schnorr_verify(digest, public, signature):
        raise SystemExit("the signature this script just produced does not verify")

    # base64url, NOT hex. RFC 7515 §2 defines the JWS `signature` member as
    # BASE64URL(JWS Signature), and the A2A `AgentCardSignature` schema repeats
    # it in as many words: "The computed signature, Base64url-encoded." This
    # script emitted `signature.hex()` for its whole life and nothing caught it,
    # because the schema only types the member as `string` — a 128-character hex
    # run is a perfectly good string and a signature no RFC 7515 verifier can
    # read. The wire encoding is the only thing that changes here; the bytes
    # signed, the digest and the curve are untouched.
    card["signatures"] = [{"protected": protected, "signature": b64url(signature)}]
    json.dump(card, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
