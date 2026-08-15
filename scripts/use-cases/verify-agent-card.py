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
So this reads `pk` out of a wallet that holds the account. That is a real limit
on who can check a card today and it is stated here rather than left for a
reader to discover: `--wallet-home` is not a convenience, it is the gap.
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
    ap.add_argument("--wallet-home", required=True)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

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
        pk = public_key_for(args.wallet_home, kid)
        if not schnorr_verify(digest, pk, bytes.fromhex(sig["signature"])):
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
