#!/usr/bin/env python3
"""Check the signature on an A2A Agent Card, the way a discovering agent must.

    scripts/use-cases/verify-agent-card.py --wallet-home DIR < signed-card.json

Exit 0 if the card verifies, 1 if it does not. Nothing else — the point of a
separate verifier is that it is the half `scripts/sign-agent-card.py` cannot be
trusted to do for itself.

WHY THE CARD IS SIGNED AT ALL

An Agent Card advertises the account to send money to. An unsigned one is a
document anyone on the discovery topic can rewrite, payment account included, so
"discover an agent and pay it" would mean "pay whoever rewrote the card last".
The signature is by the key that owns the advertised account, so a reader who
has the card has everything needed to check that the account it names is the
account whose key vouched for it.

HOW

A2A carries the signature as an RFC 7515 JWS with a *detached* payload: the
payload is the card without its `signatures`, so a verifier strips them,
re-serialises the same way the signer did — compact, key-sorted, the
`nlohmann::json::dump()` shape the module produces — and recomputes the signing
input. The algorithm is BIP-340 Schnorr over secp256k1, because that is what a
LEZ account key is.

The BIP-340 primitives are imported from scripts/sign-agent-card.py rather than
copied. Two independent implementations of the same curve arithmetic is how a
verifier ends up agreeing with a signer about a signature that is wrong.

WHAT THIS STILL NEEDS THAT A STRANGER DOES NOT HAVE

The public key. A LEZ account id is not its public key — the wallet stores `pk`
separately, and the id is derived, so the mapping from the `kid` in the card to
the 32 bytes that verify it is not something the card carries or the RPC serves.
So this reads `pk` out of a wallet that holds the account. That was a real limit
on who could check a card, and `--public-key` is the way out of it: a public key
is public, so an agent that publishes the 32 bytes alongside its card makes the
card checkable by a stranger who holds no wallet at all. The storage agent's key
is published at `artifacts/agent-cards/<category>.pub` for exactly that reason,
and it is what CI verifies against — a runner has no agent wallet and must not
be given one.

Both routes end at the same comparison. `--public-key` is not a weaker check: it
supplies the same 32 bytes from a different place, and if those bytes are wrong
the signature fails, which is the whole point of checking a signature.
"""

import argparse
import base64
import hashlib
import importlib.util
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
SIGNER = HERE.parent / "sign-agent-card.py"

_spec = importlib.util.spec_from_file_location("sign_agent_card", SIGNER)
if _spec is None or _spec.loader is None:
    raise SystemExit(f"cannot load the signing primitives from {SIGNER}")
_signer = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_signer)

schnorr_verify = _signer.schnorr_verify
selftest = _signer.selftest


def b64url_decode(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


# A BIP-340 signature is 64 bytes, which is 86 unpadded base64url characters and
# 128 hex ones. The two are not ambiguous, and this repository published the
# wrong one: `sign-agent-card.py` emitted `signature.hex()`, so every card it
# ever wrote carried 128 hex characters where RFC 7515 §2 and the A2A
# `AgentCardSignature` schema both require BASE64URL(JWS Signature).
#
# Nothing caught it. The A2A JSON Schema types the member as `string`, and hex
# is a string; a mechanical validation of the card against the published schema
# passes with the signature in the wrong alphabet. So the check has to live
# here, in the verifier, because this is the only place that claims to know what
# the member means.
B64URL_ALPHABET = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
)


def decode_signature(s):
    """The 64 signature bytes, or SystemExit naming the encoding that was used.

    Deliberately strict about the *encoding* and not merely about the length: a
    128-character hex string decodes to 96 bytes as base64url without raising,
    so a lenient decoder turns a wrong-alphabet signature into a wrong-length
    one and reports a puzzle instead of the fault.
    """
    if len(s) == 128 and all(c in "0123456789abcdefABCDEF" for c in s):
        raise SystemExit(
            "the signature is 128 hex characters; RFC 7515 §2 and the A2A "
            "AgentCardSignature schema both require base64url "
            "(86 unpadded characters for a 64-byte BIP-340 signature). "
            "Re-sign the card with scripts/sign-agent-card.py."
        )
    if s.rstrip("=") != s:
        raise SystemExit("the signature carries base64 padding; RFC 7515 §2 forbids it")
    if not s or any(c not in B64URL_ALPHABET for c in s):
        raise SystemExit("the signature is not base64url")
    raw = b64url_decode(s)
    if len(raw) != 64:
        raise SystemExit(
            f"the signature decodes to {len(raw)} bytes, not the 64 a BIP-340 signature is"
        )
    return raw


def public_key_for(wallet_home, account):
    """The 32-byte x-only public key the wallet stores for a public account."""
    with open(f"{wallet_home}/storage.json") as f:
        storage = json.load(f)
    for entry in storage["key_chain"]["accounts"]:
        pub = entry.get("Public")
        if pub and pub["account_id"] == account:
            return bytes.fromhex(pub["data"]["pk"])
    raise SystemExit(f"the wallet at {wallet_home} holds no key for {account}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    # Exactly one. The key has to come from somewhere and silently defaulting
    # to "no key" would make an unverifiable card look verified.
    ap.add_argument("--wallet-home")
    ap.add_argument("--public-key",
                    help="the signer's 32-byte x-only public key, hex")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if bool(args.wallet_home) == bool(args.public_key):
        ap.error("give exactly one of --wallet-home or --public-key")

    # The verifier checks itself against the published BIP-340 vectors before it
    # is allowed to call anything valid. A verifier that returns True for
    # everything would otherwise report a forged card as genuine.
    selftest()

    card = json.load(sys.stdin)
    sigs = card.get("signatures")
    if not isinstance(sigs, list) or not sigs:
        print("the card carries no signature", file=sys.stderr)
        return 1
    body = {k: v for k, v in card.items() if k != "signatures"}

    for sig in sigs:
        protected_b64 = sig["protected"]
        header = json.loads(b64url_decode(protected_b64))
        kid = header.get("kid")
        alg = header.get("alg")
        if alg != "secp256k1-bip340":
            print(f"unexpected algorithm {alg!r}", file=sys.stderr)
            return 1
        payload_b64 = (
            base64.urlsafe_b64encode(
                json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
            )
            .decode()
            .rstrip("=")
        )
        digest = hashlib.sha256(f"{protected_b64}.{payload_b64}".encode()).digest()
        if args.public_key:
            pk = bytes.fromhex(args.public_key.strip().removeprefix("0x"))
            if len(pk) != 32:
                raise SystemExit("--public-key is not 32 bytes")
        else:
            pk = public_key_for(args.wallet_home, kid)
        # The key comes from either source; the SIGNATURE is decoded the one way
        # RFC 7515 allows. Note the asymmetry, because it is deliberate: a public
        # key is carried as hex here (it is not a JWS member and nothing
        # specifies its encoding), while `signature` IS a JWS member and is
        # base64url or it is not a signature this verifier will read.
        if not schnorr_verify(digest, pk, decode_signature(sig["signature"])):
            print(f"the signature by {kid} does not verify", file=sys.stderr)
            return 1
        # The card is only worth anything if the key that signed it is the key
        # that owns the account the card asks you to pay. A card signed by some
        # other key of the same agent would verify and still be a licence to
        # redirect the money.
        paid = card.get("x-logos", {}).get("paymentAccount", "")
        if paid.removeprefix("Public/") != kid:
            print(
                f"signed by {kid} but the card asks for payment to {paid}",
                file=sys.stderr,
            )
            return 1
        if not args.quiet:
            print(f"verified: {kid} signed this card, and it is the payment account")
    return 0


if __name__ == "__main__":
    sys.exit(main())
