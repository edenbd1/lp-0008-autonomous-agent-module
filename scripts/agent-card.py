#!/usr/bin/env python3
"""Build and sign one agent's A2A Agent Card, and publish its verifying key.

    scripts/agent-card.py --category storage --price 1
    scripts/agent-card.py --category blockchain --price 1
    scripts/agent-card.py --category messaging                # free, price 0

Writes `artifacts/agent-cards/<category>.json` — the signed card — and
`artifacts/agent-cards/<category>.pub`, the 32-byte x-only public key that
checks it. `scripts/use-cases/verify-agent-card.py --public-key` needs nothing
else, which is what lets CI verify a card on a runner that holds no wallet.

WHY THIS FILE EXISTS, WHICH IS AN AUDIT FINDING RATHER THAN A TIDY-UP

There were two card producers in this repository and they did not agree.

`CardSkill::invoke` (module/src/agent_skills.cpp) builds a card out of the
module's own skill registry and puts each skill's parameter schema on it as
`x-logos-parameters` — the extension member `docs/a2a-binding.md` §1.3 and §3.1
name as this binding's answer to the prize's "skills declare input/output
schemas". `module/tests/agent_skills_test.cpp` asserts it is there, under a
comment reading "the shape scripts/a2a-task.sh publishes, field for field".

The other producer was a heredoc inside `scripts/a2a-task.sh`, and it dropped
the field. It is the one that wrote the only committed card, so the artefact a
reviewer inspects was the one card in the repository that did not carry the
schema its own document, its own module and its own unit test said it carried.
A literal hand-copied beside a generator drifts from it; that is what happened,
and a second hand-copy for each of the other two agents would have drifted
three ways instead of one.

So the schema is not written out here either. It is READ OUT OF THE MODULE —
the class whose `name()` returns the skill, then that class's own
`parameterSchema()` — so a card carries the schema the module would answer with,
and renaming or re-writing that schema moves the card rather than silently
disagreeing with it. `scripts/check-docs.py` derives skill names from the
registry the same way and for the same reason.

WHAT THIS IS NOT

It is not the module. `agent.card` publishes EVERY registered skill (28 of
them); the cards here advertise the one priced service the agent is deployed to
sell, which is what `scripts/a2a-task.sh` has always published and what
`scripts/use-cases/02-services-marketplace.sh` reads back as `skills[0]`.
`docs/a2a-binding.md` §3.1 says which is which, so the difference is documented
rather than discovered.
"""

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The one service each deployed agent advertises, and the price it advertises
# it at. A price is not a decoration: `scripts/use-cases/02-services-marketplace.sh`
# pays whatever the card says, so every non-zero figure here is one this
# repository has actually settled and can point at a transaction for.
#
#   storage      1 LEZ  — the last five rows of artifacts/a2a-task.tsv
#   blockchain   1 LEZ  — the four `ts0…` rows of the same file, and the default
#                         price of `scripts/delivery-in-plugin.sh settle`, where
#                         this agent is the seller
#   messaging    0 LEZ  — this agent has never been paid for a task. It is the
#                         PAYER in scripts/a2a-task.sh, and its card is here so
#                         that a refund (docs/a2a-binding.md §6.6) has a
#                         destination to be paid back into. Advertising a price
#                         nothing has ever paid would be a claim with no
#                         transaction behind it, so it advertises none.
PROFILES = {
    "storage": {
        "name": "logos-storage-agent",
        "description": "Encrypts and stores a file on Logos Storage, "
                       "returns its content address",
        "skill": "storage.upload",
        "skill_description": "Encrypt and upload a file, returning a content address",
        "price": 1,
    },
    "messaging": {
        "name": "logos-messaging-agent",
        "description": "Carries messages and Agent Cards over Logos Messaging",
        "skill": "messaging.send",
        "skill_description": "Send a message to another account over Logos Messaging",
        "price": 0,
    },
    "blockchain": {
        "name": "logos-blockchain-agent",
        "description": "Settles and queries on LEZ, and serves storage tasks it is paid for",
        # Not `program.call`, which would read better and would not be true.
        # This agent has been paid four times, and every one of those rows in
        # artifacts/a2a-task.tsv names `storage.upload` — the skill
        # `scripts/delivery-in-plugin.sh settle` asks it for. All three agents
        # load the same module and register all 28 skills, so serving it is not
        # a stretch; advertising something it has never been paid for would be.
        "skill": "storage.upload",
        "skill_description": "Encrypt and upload a file, returning a content address",
        "price": 1,
    },
}

PROVIDER = {
    "organization": "LP-0008 reference agent",
    "url": "https://github.com/logos-co/lambda-prize",
}


def field(agents, category, column):
    """A column of the agent manifest, read BY HEADER NAME rather than position."""
    with open(agents, encoding="utf-8") as f:
        rows = [line.rstrip("\n").split("\t") for line in f if line.strip()]
    if not rows:
        raise SystemExit(f"{agents} is empty")
    header = rows[0]
    if column not in header:
        raise SystemExit(f"{agents} has no column '{column}'")
    idx = header.index(column)
    for row in rows[1:]:
        if row and row[0] == category and len(row) > idx and row[idx]:
            return row[idx]
    raise SystemExit(f"{agents} has no '{column}' for category '{category}'")


def parameter_schema(skill):
    """The module's own `parameterSchema()` for a skill, as a parsed object.

    Derived, never copied. The class is the one whose `name()` returns this
    skill — the same derivation `scripts/check-docs.py` uses to check the
    harness tables — and the schema is the raw-string literals that class's
    `parameterSchema()` returns, concatenated the way the compiler concatenates
    them. Anything that does not resolve is a hard failure: a card that quietly
    lost its schema is the exact defect this file was written to close.
    """
    src = ROOT / "module/src"
    headers = "".join(
        (src / h).read_text(encoding="utf-8") for h in sorted(os.listdir(src))
        if h.endswith(".h")
    )
    cls = None
    for decl in re.finditer(r"\bclass\s+([A-Za-z_][A-Za-z0-9_]*)\b", headers):
        # Bounded to this class's body, at its closing `};` in column one — an
        # unbounded search runs on into the next class and returns a wrong
        # answer rather than a miss.
        close = headers.find("\n};", decl.start())
        body = headers[decl.start():close if close != -1 else len(headers)]
        got = re.search(r'std::string\s+name\(\)\s*const\s+override\s*'
                        r'\{\s*return\s+"([^"]+)"\s*;\s*\}', body)
        if got and got.group(1) == skill:
            cls = decl.group(1)
            break
    if cls is None:
        raise SystemExit(f"no class in module/src declares a skill named '{skill}'")

    sources = "".join(
        (src / c).read_text(encoding="utf-8") for c in sorted(os.listdir(src))
        if c.endswith(".cpp")
    )
    defn = re.search(re.escape(cls) + r"::parameterSchema\(\)\s*const\s*\{(.*?)\n\}",
                     sources, re.S)
    if defn is None:
        raise SystemExit(f"{cls} declares '{skill}' but defines no parameterSchema()")
    text = "".join(re.findall(r'R"\((.*?)\)"', defn.group(1), re.S))
    if not text:
        raise SystemExit(f"{cls}::parameterSchema() returns no raw-string literal")
    try:
        schema = json.loads(text)
    except json.JSONDecodeError as e:
        raise SystemExit(f"{cls}::parameterSchema() is not JSON: {e}")
    if not isinstance(schema, dict):
        raise SystemExit(f"{cls}::parameterSchema() is not a JSON object")
    return schema


def shielded_keys(wallet_bin, wallet_home, agent_id):
    """The agent's `npk` and `vpk`, straight out of its own wallet.

    Both are PUBLIC keys — `npk` a nullifier public key, `vpk` an ML-KEM-768
    encapsulation key — and together they are the whole address a payer needs to
    credit this agent privately (docs/a2a-binding.md §6.2). Empty is not fatal:
    an operator whose wallet does not hold this agent gets a card with a public
    payment account and no shielded one, which is the pre-existing behaviour,
    rather than a run that stops.
    """
    if not wallet_bin or not os.access(wallet_bin, os.X_OK):
        return None
    env = dict(os.environ,
               LEE_WALLET_HOME_DIR=wallet_home,
               NSSA_WALLET_HOME_DIR=wallet_home)
    try:
        out = subprocess.run([wallet_bin, "account", "show-keys",
                              "--account-id", f"Private/{agent_id}"],
                             env=env, stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    keys = [l.strip() for l in out.splitlines()
            if re.fullmatch(r"[0-9a-f]{64,}", l.strip())]
    if len(keys) != 2:
        return None
    return {"npk": keys[0], "vpk": keys[1]}


def public_key(wallet_home, account):
    """The 32-byte x-only public key the wallet stores for a public account."""
    with open(f"{wallet_home}/storage.json", encoding="utf-8") as f:
        storage = json.load(f)
    for entry in storage["key_chain"]["accounts"]:
        pub = entry.get("Public")
        if pub and pub["account_id"] == account:
            return pub["data"]["pk"]
    raise SystemExit(f"the wallet at {wallet_home} holds no key for {account}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--category", required=True, choices=sorted(PROFILES))
    ap.add_argument("--price", type=int,
                    help="LEZ per task; defaults to this agent's advertised price")
    ap.add_argument("--agents", default=str(ROOT / "artifacts/agents.tsv"))
    ap.add_argument("--out", default=str(ROOT / "artifacts/agent-cards"))
    ap.add_argument("--agent-homes",
                    default=os.environ.get("AGENT_HOMES",
                                           os.path.expanduser("~/.lp0008-agents")))
    ap.add_argument("--wallet-bin", default=os.environ.get("WALLET_BIN", ""))
    args = ap.parse_args()

    profile = PROFILES[args.category]
    price = profile["price"] if args.price is None else args.price
    if price < 0:
        raise SystemExit("a price is a non-negative integer")

    agent_id = field(args.agents, args.category, "agent_id")
    pay = field(args.agents, args.category, "pay_account")
    home = os.path.join(args.agent_homes, args.category)

    skill = {
        "id": profile["skill"],
        "name": profile["skill"],
        "description": profile["skill_description"],
        # A2A requires `tags` present and non-empty. The substring before the
        # first `.`, which is what CardSkill derives.
        "tags": [profile["skill"].split(".")[0]],
        "inputModes": ["application/json"],
        "outputModes": ["application/json"],
        # A2A v0.3.0 has no field for an input schema and the prize asks the
        # card to declare one, so it travels as an extension member rather than
        # being dropped. `AgentSkill` does not set `additionalProperties: false`,
        # so this is a legal extension member and not tolerated junk.
        "x-logos-parameters": parameter_schema(profile["skill"]),
    }

    x = {
        "lezAccount": agent_id,
        # `Public/…` is the form `spel` resolves and the form the settlement in
        # scripts/a2a-task.sh is addressed to.
        "paymentAccount": pay if pay.startswith("Public/") else "Public/" + pay,
        "pricePerTask": price,
        "settlement": "lez-chained-authenticated-transfer",
    }
    keys = shielded_keys(args.wallet_bin, home, agent_id)
    if keys:
        x["shieldedPaymentKeys"] = keys
    else:
        print(f"  no shielded receiving keys for {args.category} (set WALLET_BIN); "
              f"the card will", file=sys.stderr)
        print("  advertise only the public payment account", file=sys.stderr)

    card = {
        "protocolVersion": "0.3.0",
        "name": profile["name"],
        "description": profile["description"],
        "url": "logos-messaging://" + agent_id,
        "preferredTransport": "logos-messaging",
        "version": "0.1.0",
        "provider": PROVIDER,
        "capabilities": {
            "streaming": True,             # agent.subscribe
            "stateTransitionHistory": True,  # the store keeps it
            "pushNotifications": False,    # no webhooks here
        },
        "defaultInputModes": ["application/json"],
        "defaultOutputModes": ["application/json"],
        "skills": [skill],
        "x-logos": x,
    }

    # One signer, not a second copy of the curve arithmetic. RFC 7515 JWS with a
    # detached payload, signed by the key that owns the account the card asks
    # you to pay — which is the thing the signature exists to vouch for.
    account = x["paymentAccount"].removeprefix("Public/")
    signed = subprocess.run(
        [sys.executable, str(ROOT / "scripts/sign-agent-card.py"),
         "--wallet-home", home, "--account", account],
        input=json.dumps(card), capture_output=True, text=True)
    if signed.returncode != 0:
        sys.stderr.write(signed.stderr)
        raise SystemExit("  the card could not be signed, so it is not published")

    outdir = pathlib.Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / f"{args.category}.json").write_text(signed.stdout, encoding="utf-8")
    # The verifying key, beside the card. `kid` is the base58 account id, not
    # the public key and not derived from it, so a reader who has only the card
    # cannot check it (docs/a2a-binding.md §3.6). This is what closes that, out
    # of band, and it is what CI verifies against.
    (outdir / f"{args.category}.pub").write_text(
        public_key(home, account) + "\n", encoding="utf-8")
    print(f"  published {outdir / (args.category + '.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
