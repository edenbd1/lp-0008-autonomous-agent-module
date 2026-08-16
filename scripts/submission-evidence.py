#!/usr/bin/env python3
"""Generate the evidence sections of SUBMISSION-DRAFT.md from the chain.

    ./scripts/submission-evidence.py                 # print the sections
    ./scripts/submission-evidence.py --write SUBMISSION-DRAFT.md
    ./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md   # CI guard

Why this exists
---------------

SUBMISSION-DRAFT.md is the first thing a reviewer reads, and it had a table
headed "(getAccount)" and "(from getTransaction)", captioned "Verified
independently for this document", in which every single value was a literal
somebody typed. The transactions it named as the headline evidence were the four
that docs/DEPLOYMENT.md disowns by name as belonging to policy accounts that no
longer exist. Its balances (0/25/50/75) had never been true. The block numbers it
quoted appear in no artifact in this repository. None of that was noticed,
because a hand-written number does not announce that it has gone stale.

Correcting those numbers by hand is how they got there. Every redeploy on this
chain is content-addressed, so it moves the deploy hash, the ImageID, the
ProgramId, every policy PDA and every anchor transaction at once. A document
whose facts are typed is stale from the next deploy onward; a document whose
facts are fetched is stale never.

So nothing here is transcribed from another document. The deploy transaction is
DERIVED from the committed binary — sha256(u32_le(len) || bytecode) — and then
looked up. Balances come out of the transaction that set them. Limits come off
the account the state machine keeps. The only inputs are the committed bytes,
the committed manifests, and https://testnet.lez.logos.co.

What makes each number safe to print
------------------------------------

  * A transaction is only cited once `getTransaction` returns it AND its bytes
    are found inside the block the RPC names AND absent from both neighbours.
    The sequencer silently drops failing transactions and `getTransaction`
    answers null identically for dropped, pending, rejected and never-submitted,
    so "not null" is the weakest possible claim and block membership is checked
    rather than accepted. (SUBMISSION-DRAFT.md claimed this check for months; no
    script in the repository performed it. This one does — see `confirm`.)

  * Balances are read from the transaction's committed POST-STATE, not from
    `getAccount`. `getAccount` is current-state only and this chain has no
    historical-state RPC, so "the payee holds 75 today" is not evidence about a
    settlement that landed 200 blocks ago — and in fact the payee's balance has
    since gone down as well as up. A transaction commits to the balance each
    account it touches holds once it has run, and the hash proves the bytes are
    that transaction, so the post-state is the balance *at* the settlement. The
    decoder is the one from scripts/use-cases/settlement-facts.py, kept here so
    this script runs from a clean clone with nothing but python3; the field
    names are deliberately identical so the two cannot drift apart unnoticed.

  * TSVs are read BY HEADER NAME and a missing column is fatal. Positional reads
    have produced three separate false results in this repository, and the
    columns really do move: `anchored.tsv` renamed `create_tx` to `tx` and grew
    a `what` column, `agents.tsv` grew `claim_account`, `claim_tx` and
    `record_prefix`, and `a2a-task.tsv` gained a `program` column AT POSITION 1
    — which shifted every other column by one, so a positional read of
    `settlement_tx` now returns the nonce. That happened mid-task, under this
    script, and changed nothing about its output.

  * Nothing that moves with ordinary use is printed into a checked document.
    The ledger's `window_start` and `spent` change on every settlement, so
    stating them from `getAccount` would make --check fail on agent activity
    with nothing in the repository changed. A gate that cries wolf is one people
    learn to skip. They drifted from 50 to 55 while this was being written,
    which is how the rule was found. The anchored LIMITS are still stated and
    still compared against the manifest, because those are recorded in
    `agents.tsv` and a disagreement is a real defect; the running total is shown
    per settlement instead, out of that transaction's immutable post-state.

  * THE OUTPUT IS A FUNCTION OF THE CHAIN AND THE REPOSITORY, AND OF NOTHING
    ELSE ABOUT THE MACHINE. This is the rule that was learned the hard way. An
    earlier version shelled out to `spel program-id` and swallowed OSError, and
    three rendering sites branched on the result -- so the document said
    different things depending on whether a tool was installed. The machine
    that wrote it had `spel`; CI did not; --check compared 36 lines that could
    never match, and main was red twice. The trap in the easy fix is what makes
    this worth writing down: regenerating in CI WOULD have gone green, by
    silently replacing "**This settlement was made under a superseded
    deployment**" with "not compared on this run". A gate can be satisfied by
    deleting the claim it was guarding.

    So identity is established from the chain -- committed bytes -> deploy
    transaction (derived, and checked to embed those bytes) -> anchor payload
    -> ImageID -> ProgramId -- which also proves MORE than the tool did: `spel`
    only recomputes an ImageID from a file, while this ties the committed file
    to the program that owns the accounts. `spel` remains as a cross-check that
    may only raise Fatal; it contributes no character of output.

    Everything else of that family is checked too, and the output is identical
    with and without `spel` on PATH, under LC_ALL=C/en_US/de_DE/ja_JP, under
    three timezones, and from a different working directory. No clock is read.
    The one environment variable, SEQUENCER_URL, selects which chain to ask,
    which is supposed to change the answer -- and the endpoint is never rendered,
    only explorer links are.

  * Nothing may be left blank. `render` refuses to emit "TBD", "TODO", a
    placeholder or an empty table cell — that pattern is quoted verbatim in the
    closing comments on rejected submissions to this programme. A fact that
    cannot be fetched is written out as a sentence saying it could not be
    fetched, and why.

  * Links point at the block explorer, because reviewers check the explorer and
    not the RPC. The explorer indexes behind the sequencer, which the generated
    text says out loud rather than leaving the reviewer to conclude the hash is
    fake.

Exit status
-----------

  0  every cited fact resolved; sections generated (or, with --check, unchanged)
  1  a cited fact did not resolve, or a manifest disagrees with the chain, or
     --check found the document drifting from what the chain now says

Non-zero means the document must not ship. That is the whole point: a stale
SUBMISSION-DRAFT.md can no longer be produced quietly.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import urllib.error
import urllib.request

RPC_DEFAULT = "https://testnet.lez.logos.co"
EXPLORER = "https://explorer.testnet.lez.logos.co/transaction/%s"

# A hash that cannot be the sha256 of anything anybody submitted. Every run asks
# for it. Without the control, "the sequencer returned a transaction" proves
# nothing -- an RPC that answered non-null to everything would pass just as
# happily, and this repository has shipped a check that would not have noticed.
CONTROL_TX = "dededededededededededededededededededededededededededededededede"

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

BEGIN = "<!-- BEGIN GENERATED %s -- scripts/submission-evidence.py; do not edit by hand -->"
END = "<!-- END GENERATED %s -->"

# Things a reviewer has closed a submission over. Checked against the rendered
# output, not against intentions.
FORBIDDEN = re.compile(
    r"(?i)\bTBD\b|\bTODO\b|\bXXX\b|<<<|\bplaceholder\b|\bN/?A\b|\bcoming soon\b")


class Fatal(Exception):
    """A fact that cannot be established. Never rendered around, never guessed."""


# --------------------------------------------------------------------------
# chain
# --------------------------------------------------------------------------

class Chain:
    def __init__(self, url):
        self.url = url
        self._tx = {}
        self._block = {}
        self._account = {}

    def rpc(self, method, params):
        body = json.dumps({"jsonrpc": "2.0", "id": 1,
                           "method": method, "params": params}).encode()
        req = urllib.request.Request(
            self.url, data=body, headers={"Content-Type": "application/json"})
        last = None
        for _ in range(3):
            try:
                with urllib.request.urlopen(req, timeout=45) as r:
                    payload = json.load(r)
                if "error" in payload:
                    raise Fatal("%s: %s" % (method, payload["error"]))
                return payload.get("result")
            except urllib.error.URLError as e:      # transient; the tip moves
                last = e
        raise Fatal("%s(%s) could not reach %s: %s"
                    % (method, params, self.url, last))

    def transaction(self, tx):
        """(raw_bytes, block) or None. None means: dropped, pending, rejected or
        never submitted -- the RPC cannot tell those apart and neither can we."""
        if tx not in self._tx:
            r = self.rpc("getTransaction", [tx])
            self._tx[tx] = (base64.b64decode(r[0]), r[1]) if r else None
        return self._tx[tx]

    def block(self, n):
        if n not in self._block:
            r = self.rpc("getBlock", [n])
            self._block[n] = base64.b64decode(r) if r else None
        return self._block[n]

    def account(self, addr):
        if addr not in self._account:
            self._account[addr] = self.rpc("getAccount", [addr]) or {}
        return self._account[addr]

    def program_ids(self):
        return self.rpc("getProgramIds", [])


def confirm(chain, tx):
    """Resolve a transaction to a block, and prove the block attribution.

    `getTransaction` naming a block is the RPC's word for it. This checks that
    the transaction's own bytes occur in that block and in neither neighbour, so
    a wrong block number is a failure rather than a plausible figure.
    """
    got = chain.transaction(tx)
    if got is None:
        raise Fatal("%s is not on chain (dropped, pending, rejected or never "
                    "submitted -- the RPC returns the same null for all four)" % tx)
    raw, block = got
    digest = hashlib.sha256(raw[1:]).hexdigest()
    if digest != tx:
        raise Fatal("%s: the sequencer returned bytes that hash to %s" % (tx, digest))
    here = chain.block(block)
    if here is None or raw not in here:
        raise Fatal("%s: block %d does not contain these bytes" % (tx, block))
    # The successor block must already exist. Not for strength -- absence proves
    # nothing either way -- but for determinism: the neighbour list is printed
    # into the document, so a transaction sitting in the newest block would be
    # written up with one neighbour and then disagree with itself sixty seconds
    # later, when the next block arrived. That is a gate that goes red on its
    # own, and a gate that cries wolf is one people learn to skip. Evidence for
    # a transaction is generated once the chain has moved past it.
    if chain.block(block + 1) is None:
        raise Fatal("%s is in block %d, which is still the tip: wait for the "
                    "next block before generating evidence for it" % (tx, block))
    neighbours = []
    for n in (block - 1, block + 1):
        b = chain.block(n)
        if b is None:
            continue                       # before genesis
        if raw in b:
            raise Fatal("%s: its bytes are also in block %d" % (tx, n))
        neighbours.append(n)
    return {"tx": tx, "raw": raw, "block": block, "bytes": len(raw),
            "neighbours": neighbours}


def parse_updates(b):
    """The post-state a transaction commits to.

    Layout, per update: account id (32), the ImageID of the program that owns it
    (32), balance (u128 LE), data length (u32 LE), data, and a trailing u128.
    The header is a kind byte and then the number of updates as a u32.

    Taken unchanged from scripts/use-cases/settlement-facts.py, including the
    offset cross-check below: every account the parse claims to have found must
    occur at the offset it was read from. A layout that has drifted by one byte
    then produces an error instead of a plausible wrong balance.
    """
    out = []
    o = 1                                     # byte 0 is the kind discriminant
    n = int.from_bytes(b[o:o + 4], "little")
    o += 4
    if n == 0 or n > 64:
        raise Fatal("implausible update count %d" % n)
    for _ in range(n):
        start = o
        acct, o = b[o:o + 32], o + 32
        owner, o = b[o:o + 32], o + 32
        bal, o = int.from_bytes(b[o:o + 16], "little"), o + 16
        dl, o = int.from_bytes(b[o:o + 4], "little"), o + 4
        if dl > len(b):
            raise Fatal("an update claims %d bytes of data" % dl)
        data, o = b[o:o + dl], o + dl
        o += 16                               # trailer
        if o > len(b):
            raise Fatal("the update list runs past the end of the transaction")
        out.append({"offset": start, "account": acct, "owner": owner,
                    "balance": bal, "data": data})
    for u in out:
        if b.find(u["account"]) != u["offset"]:
            raise Fatal("update at %d but its account id first occurs at %d"
                        % (u["offset"], b.find(u["account"])))
    return out


def ledger_record(data):
    """The 97 bytes an anchored ledger account holds, or None if it holds
    something else.

    Named `ledger_*` to match scripts/use-cases/settlement-facts.py, which the
    use-case scripts consume: its `ledger_account`, `ledger_spent`,
    `ledger_window_start`, `ledger_per_tx`, `ledger_per_period` and
    `ledger_record_owner` are this dict's keys under that prefix. Two decoders
    of the same bytes that disagree about what to call them is a smaller
    problem than two that disagree about the bytes, but it is still a problem.

    `record_owner`, not `owner`: the account already has an owner -- the
    program allowed to write it -- and conflating the two is how a previous
    reading of this record went wrong.
    """
    if len(data) != 97 or data[0] != 1:
        return None
    le = lambda a, b: int.from_bytes(data[a:b], "little")
    return {"record_owner": data[1:33].hex(), "per_tx": le(33, 49),
            "per_period": le(49, 65), "period_blocks": le(65, 73),
            "window_start": le(73, 81), "spent": le(81, 97)}


def b58decode(s):
    n = 0
    for c in s:
        if c not in B58:
            raise Fatal("%r is not base58" % s)
        n = n * 58 + B58.index(c)
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    b = b"\x00" * (len(s) - len(s.lstrip("1"))) + b
    if len(b) != 32:
        raise Fatal("%r is not a 32-byte account id" % s)
    return b


def b58encode(b):
    n = int.from_bytes(b, "big")
    s = ""
    while n:
        n, r = divmod(n, 58)
        s = B58[r] + s
    return "1" * (len(b) - len(b.lstrip(b"\x00"))) + s


def program_id_words(raw32):
    return [int.from_bytes(raw32[i:i + 4], "little") for i in range(0, 32, 4)]


# --------------------------------------------------------------------------
# manifests
# --------------------------------------------------------------------------

def read_tsv(path, required, optional=(), aliases=None):
    """Rows as dicts, keyed by HEADER NAME.

    A required column that is absent is fatal and named in the error. Reading by
    position is what produced `balance_after` being read out of `balance_before`
    and two other false results here; the columns of these files have been
    reordered and renamed more than once, and a positional read answers
    confidently with whatever now sits in the slot.

    `aliases` maps a canonical name to other names it has been shipped under, so
    a rename is survivable but never silent -- an alias that no longer resolves
    to anything still fails.
    """
    aliases = aliases or {}
    if not os.path.exists(path):
        raise Fatal("%s does not exist" % path)
    with open(path, encoding="utf-8") as fh:
        lines = [l.rstrip("\n") for l in fh if l.strip()]
    if not lines:
        raise Fatal("%s is empty" % path)
    header = lines[0].split("\t")
    idx = {}
    for want in list(required) + list(optional):
        names = [want] + list(aliases.get(want, ()))
        hit = next((n for n in names if n in header), None)
        if hit is None:
            if want in required:
                raise Fatal("%s has no %s column (columns are: %s)"
                            % (path, " / ".join(names), ", ".join(header)))
            continue
        idx[want] = header.index(hit)
    rows = []
    for lineno, line in enumerate(lines[1:], start=2):
        f = line.split("\t")
        row = {}
        for name, i in idx.items():
            if i >= len(f):
                raise Fatal("%s line %d has %d fields, needs at least %d for %s"
                            % (path, lineno, len(f), i + 1, name))
            row[name] = f[i]
        rows.append(row)
    if not rows:
        raise Fatal("%s has a header and no rows" % path)
    return rows


def deploy_tx_of(binary):
    """The deploy transaction hash, DERIVED from the committed bytes.

    Never copied from a document. This is the whole content-addressing story: if
    the binary in the repository is the binary on chain, this hash resolves; if
    somebody rebuilt the guest and forgot to redeploy, it does not, and the run
    fails instead of describing a program nobody can call.
    """
    if not os.path.exists(binary):
        raise Fatal("%s does not exist" % binary)
    b = open(binary, "rb").read()
    return hashlib.sha256(struct.pack("<I", len(b)) + b).hexdigest(), len(b)


def policy_seeds(idl_path):
    """How the shipped IDL says a policy account's address is derived.

    Generated rather than described, because SUBMISSION-DRAFT.md spent three
    deployments asserting a derivation the program no longer used — and not a
    harmless one: it advertised `PDA(SHA256(owner ‖ agent ‖ limits))`, which is
    the design an attacker holding the agent's key broke by anchoring a fresh
    account of its own, and which was replaced for exactly that reason. A design
    claim read out of the committed IDL cannot outlive the design.

    Returns {instruction: "seed, seed"} for every instruction naming a `policy`
    account, so a derivation that differs between instructions is visible rather
    than averaged into one sentence.
    """
    if not os.path.exists(idl_path):
        raise Fatal("%s does not exist, so the policy account's address "
                    "derivation cannot be read off the shipped interface"
                    % idl_path)
    idl = json.load(open(idl_path, encoding="utf-8"))
    out = {}
    for ins in idl.get("instructions", []):
        for acct in ins.get("accounts", []):
            if acct.get("name") != "policy" or not acct.get("pda"):
                continue
            parts = []
            for s in acct["pda"].get("seeds", []):
                if s.get("kind") == "const":
                    parts.append('"%s"' % s.get("value"))
                else:
                    parts.append("%s (%s)" % (s.get("path"), s.get("kind")))
            out.setdefault(ins.get("name", "?"), ", ".join(parts))
    if not out:
        raise Fatal("%s declares no `policy` account with a PDA; the address "
                    "derivation this document describes cannot be confirmed"
                    % idl_path)
    return out


def shipped_identity(chain, root, agents):
    """Who the shipped program is, established entirely from the chain.

    This function exists because the version it replaces shelled out to `spel
    program-id` and swallowed OSError. That made the rendered document a
    function of whether a tool happened to be installed: three sites branched
    on it, the machine that wrote the document had `spel`, CI did not, and
    --check compared 36 lines of text that could never match. Worse than the
    red build, the tool-less rendering was the WEAKER one -- the ImageID row
    vanished and the superseded-settlement labelling degraded to "not compared
    on this run". Turning CI green by regenerating there would have quietly
    deleted the strongest claim in the document.

    The identity is a chain fact, so it is read from the chain:

      1. `SHA256(u32_le(len) || bytecode)` of the committed binary is the deploy
         transaction, because deployment is content-addressed. It is confirmed
         on chain, and the transaction is checked to EMBED those exact bytes --
         so the file in this repository is the program that was deployed.
      2. A `create_policy` payload begins with a kind discriminant of 0 and then
         carries, in bytes 1..33, the ImageID of the program it calls. Any
         anchor in artifacts/agents.tsv yields it. That is the shipped ImageID.
      3. Its ProgramId words are what `getAccount` reports as `program_owner`
         for the accounts that anchor created, which is how every settlement
         below is attributed.

    Nothing here reads PATH, the environment, the clock, or anything outside
    the repository, so every machine renders the same bytes.
    """
    binary = os.path.join(root, "artifacts/programs/agent_verifier.bin")
    tx, size = deploy_tx_of(binary)
    c = confirm(chain, tx)
    if open(binary, "rb").read() not in c["raw"]:
        raise Fatal("%s resolves on chain but the transaction does not contain "
                    "the committed bytes" % tx)

    image = None
    for a in agents:
        got = chain.transaction(a["create_tx"])
        if got is None:
            continue
        payload = got[0]
        if len(payload) > 33 and payload[0] == 0:
            image = {"hex": payload[1:33].hex(),
                     "words": program_id_words(payload[1:33]),
                     "from_tx": a["create_tx"], "from": a["category"]}
            break
    if image is None:
        raise Fatal("no anchor in artifacts/agents.tsv carries a readable "
                    "ImageID, so the shipped program cannot be identified from "
                    "the chain and no settlement can be attributed")

    # `spel`, if it is here, may only DISAGREE. It never contributes a
    # character to the output, so its absence cannot change a single byte.
    recomputed = spel_recomputed_image(binary)
    if recomputed is not None and recomputed != image["hex"]:
        raise Fatal("`spel program-id` recomputes the committed binary's "
                    "ImageID as %s, but the anchors on chain were made against "
                    "%s. The repository ships a different program from the one "
                    "enforcing these envelopes." % (recomputed, image["hex"]))

    return {"deploy_tx": tx, "size": size, "confirm": c,
            "image": image, "spel_agrees": recomputed is not None}


def spel_recomputed_image(binary):
    """ImageID recomputed locally from the ELF, or None if `spel` is absent.

    A CROSS-CHECK ONLY. Its result may fail a run and may never be rendered:
    text that depends on a local tool is text that differs between machines,
    which is the bug this whole module now documents. Callers must treat None
    as "no second opinion available", never as a fact worth printing.
    """
    try:
        out = subprocess.run(["spel", "program-id", binary],
                             capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    m = re.search(r"ImageID \(hex bytes\):\s*([0-9a-f]{64})", out.stdout)
    return m.group(1) if m else None


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------

def link(tx):
    return "[`%s…%s`](%s)" % (tx[:8], tx[-8:], EXPLORER % tx)


def fmt(n):
    return "{:,}".format(n)


class Doc:
    def __init__(self):
        self.lines = []

    def __call__(self, s=""):
        self.lines.append(s)

    def text(self):
        return "\n".join(self.lines).rstrip() + "\n"


def section_program(chain, root, agents, ident):
    d = Doc()
    tx, size, c = ident["deploy_tx"], ident["size"], ident["confirm"]

    d("Every figure in this section was fetched by "
      "`./scripts/submission-evidence.py` at generation time. Nothing in it is "
      "transcribed from another document, and the transaction hash is not "
      "quoted from anywhere — it is derived from the committed bytes.")
    d()
    d("`artifacts/programs/agent_verifier.bin` is %s bytes. Deployment on LEZ is "
      "content-addressed, so the deploy transaction is "
      "`SHA256(u32_le(len) ‖ bytecode)` of exactly those bytes:" % fmt(size))
    d()
    d("| | |")
    d("|---|---|")
    d("| deploy transaction | %s |" % link(tx))
    d("| block | %d |" % c["block"])
    d("| on the wire | %s bytes |" % fmt(c["bytes"]))
    d("| bytes found in block | %d, and in neither %s |"
      % (c["block"], " nor ".join(str(n) for n in c["neighbours"])))

    owners = {}
    for a in agents:
        acct = chain.account(a["policy_account"])
        w = acct.get("program_owner") or []
        if any(w):
            owners.setdefault(tuple(w), []).append(a["category"])

    d("| the deployed bytes | the transaction embeds this repository's copy of "
      "`agent_verifier.bin` verbatim |")
    d("| shipped ImageID, read off the on-chain `%s` anchor | `%s` |"
      % (ident["image"]["from"], ident["image"]["hex"]))
    d("| its ProgramId | `%s` |"
      % ",".join(str(x) for x in ident["image"]["words"]))
    d()

    if len(owners) == 1 and list(owners)[0] == tuple(ident["image"]["words"]):
        d("Every step of that is a chain fact, and none of it depends on a tool "
          "being installed. The deploy transaction is *derived* from the "
          "committed bytes rather than quoted, it resolves, and it contains "
          "those bytes — so the file in this repository is the program that was "
          "deployed. The anchor transaction that created these policy accounts "
          "names the ImageID it called, and `getAccount` reports that same "
          "ProgramId as the `program_owner` of every one of them. **The binary "
          "in this repository is the program enforcing these envelopes on "
          "chain.**")
    elif len(owners) == 1:
        raise Fatal(
            "the anchors were made against ImageID %s but the policy accounts "
            "are owned by %s"
            % (ident["image"]["hex"],
               ",".join(str(x) for x in list(owners)[0])))
    else:
        d("The policy accounts named in `artifacts/agents.tsv` are **not all "
          "owned by the same program** (%s). That means the manifest describes "
          "more than one deployment at once."
          % "; ".join("%s: %s" % (",".join(c), ",".join(str(x) for x in w))
                      for w, c in owners.items()))
    d()

    # The callee. Not deployed by this repository, so the claim to check is
    # identity with the chain's own program, and the chain will state it.
    named = chain.program_ids() or {}
    chain_transfer = named.get("authenticated_transfer")
    # `program_owner` comes back as eight u32 words already, not as 32 bytes.
    payee_owners = {tuple(chain.account(a["pay_account"]).get("program_owner") or [])
                    for a in agents}
    payee_owners.discard(())
    payee_owners.discard(tuple([0] * 8))
    if chain_transfer and payee_owners == {tuple(chain_transfer)}:
        d("`spend` moves no balance itself — LEZ rule 5 refuses any post-state "
          "that decreases the balance of an account the executing program does "
          "not own — so it chains a call into the authenticated transfer "
          "program, which does own them. "
          "`artifacts/programs/authenticated_transfer.bin` is committed because "
          "the circuit resolves the callee by ImageID; it is not deployed by "
          "this repository. `getProgramIds` reports `authenticated_transfer` as "
          "`%s`, and that is exactly the `program_owner` the chain gives every "
          "agent's receiving account — which is why the policy program cannot "
          "move those balances itself and has to chain the call."
          % ",".join(str(x) for x in chain_transfer))
    elif chain_transfer:
        raise Fatal(
            "`getProgramIds` reports authenticated_transfer as %s, but the "
            "agents' receiving accounts are owned by %s — the callee this "
            "document describes is not the one holding the money"
            % (",".join(str(x) for x in chain_transfer),
               "; ".join(",".join(str(x) for x in w) for w in sorted(payee_owners))))
    else:
        raise Fatal("`getProgramIds` did not report an `authenticated_transfer` "
                    "entry on this sequencer, so the callee's identity cannot "
                    "be confirmed and the chained-call claim is unsupported")
    d()

    seeds = policy_seeds(os.path.join(root, "idl/agent_verifier.idl.json"))
    by_shape = {}
    for ins, shape in sorted(seeds.items()):
        by_shape.setdefault(shape, []).append(ins)
    d("Read out of the shipped `idl/agent_verifier.idl.json` rather than "
      "described — the address derivation is the security argument, so the "
      "seeds are quoted from the interface the repository actually ships. "
      "`program` is the deployment identified above: a PDA is scoped to the "
      "program that owns it, which is why every one of these addresses moves "
      "when the ImageID does, and why the superseded settlements further down "
      "charge accounts that no longer exist.")
    d()
    d("| instruction | policy account address |")
    d("|---|---|")
    for shape, inss in sorted(by_shape.items(), key=lambda kv: kv[1]):
        d("| %s | `PDA(program, %s)` |"
          % (", ".join("`%s`" % i for i in inss), shape))
    d()
    d("There is **one policy account per agent**: the seed is the agent, and "
      "every limit is the account's *data*, which LEZ rule 6 "
      "(`UnauthorizedDataModification`) lets only this program write. Where the "
      "seed is `(account)` it comes from the pre-state the state machine built "
      "and there is no argument to lie about; where it is `(arg)` it is "
      "caller-supplied, which is why `create_policy` is `#[account(init, …)]` "
      "and the first anchor for an agent is the only one.")
    d()
    d("The control: `getTransaction` on "
      "`dededededededededededededededededededededededededededededededede`, a "
      "hash nobody has submitted, returned `null` on this run. Without it "
      "\"the sequencer returned a transaction\" would prove nothing.")
    return d.text()


def section_agents(chain, root, agents, anchors):
    d = Doc()
    d("Read from `artifacts/agents.tsv` **by column name**, then checked against "
      "the chain. Limits below are the ones the state machine holds, not the "
      "ones the manifest claims; where they differed this section would say so "
      "and the generator would exit non-zero.")
    d()
    d("| category | agent | policy account | per-tx | per-period | period | `create_policy` |")
    d("|---|---|---|---|---|---|---|")

    disagreements = []
    for a in agents:
        acct = chain.account(a["policy_account"])
        data = bytes(acct.get("data") or [])
        owner = acct.get("program_owner") or []
        if not any(owner):
            raise Fatal("policy account %s (%s) has no owner on chain — it was "
                        "never anchored, or the manifest names the wrong address"
                        % (a["policy_account"], a["category"]))
        rec = ledger_record(data)
        if rec is None:
            raise Fatal("policy account %s (%s) holds %d bytes, not a 97-byte v1 "
                        "policy record" % (a["policy_account"], a["category"], len(data)))
        for field in ("per_tx", "per_period", "period_blocks"):
            if str(rec[field]) != a[field]:
                disagreements.append(
                    "%s: the chain holds %s = %d, the manifest says %s"
                    % (a["category"], field, rec[field], a[field]))
        # The owner is the record's FIRST field and no part of the address --
        # which is precisely the derivation this section spent three deployments
        # stating backwards. It is compared rather than described: `agents.tsv`
        # has carried the `owner` column all along and nothing here read it, so
        # a policy anchored under the wrong owner would have gone unremarked
        # while the paragraph below asserted otherwise.
        if rec["record_owner"] != b58decode(a["owner"]).hex():
            disagreements.append(
                "%s: the policy record on chain names owner %s, the manifest "
                "says %s"
                % (a["category"], b58encode(bytes.fromhex(rec["record_owner"])),
                   a["owner"]))
        c = confirm(chain, a["create_tx"])
        d("| %s | `%s…` | `%s…` | %s | %s | %s blocks | %s, block %d |"
          % (a["category"], a["agent_id"][:8], a["policy_account"][:8],
             fmt(rec["per_tx"]), fmt(rec["per_period"]), fmt(rec["period_blocks"]),
             link(a["create_tx"]), c["block"]))
    d()

    if disagreements:
        raise Fatal("artifacts/agents.tsv disagrees with the chain: "
                    + "; ".join(disagreements))

    d("Each `create_policy` above was confirmed present in the block named and "
      "absent from both neighbours. The limits are the chain's own copy, and "
      "they are the account's *data* rather than part of its name: the address "
      "of a policy account is `PDA(program, [\"agent-policy/v1\", agent_id])` — "
      "the program and the agent, and nothing else. No limit and no owner is in "
      "that preimage, so there is exactly **one** such account per agent, "
      "raising a ceiling rewrites this record in place rather than naming a "
      "second one, and `init` gives the address to whoever anchors first. The "
      "owner is the record's own first field, and this generator compares it "
      "against the `owner` column above rather than describing it. What keeps "
      "the numbers honest is therefore not the address but who may write it: "
      "LEZ rule 6 (`UnauthorizedDataModification`) lets only the owning program "
      "touch that data, `update_policy` is the one instruction that rewrites "
      "the limits and it refuses any signer that is not the owner the record "
      "names, and `spend` seeds this same address from the *signing* agent's "
      "account instead of from an argument — so an agent presenting a roomier "
      "policy account fails the PDA check the macro emits before the program "
      "body runs.")
    d()
    d("The ledger's *running total* — `window_start` and `spent`, the halves only "
      "the owning program may write — is deliberately **not** in that table, and "
      "the reason is the same one that keeps `getAccount` out of the settlement "
      "balances. Those two fields move every time an agent spends. Reading them "
      "with `getAccount` puts a number in this document that is current only "
      "until the next settlement, so `--check` would fail on ordinary agent "
      "activity with nothing in the repository changed — a gate that cries wolf "
      "is one people learn to skip, and it drifted under this document once "
      "already, from 50 to 55, while it was being written. The limits above are "
      "safe to state because they are anchored and the manifest records them, so "
      "a disagreement is a real defect rather than the clock. The running total "
      "appears in the settlement table below instead, taken from each "
      "transaction's own committed post-state, where it is immutable.")
    d()

    # anchored.tsv is the full history including superseded programs. Its value
    # is precisely that it keeps rows this deployment has moved past.
    current, _ = deploy_tx_of(os.path.join(root, "artifacts/programs/agent_verifier.bin"))
    by_program = {}
    for r in anchors:
        by_program.setdefault(r["program"], []).append(r)
    live = by_program.get(current, [])
    older = {p: rs for p, rs in by_program.items() if p != current}

    d("`artifacts/anchored.tsv` records every `(program, anchor)` pair this "
      "repository has ever written, keyed on the program, which is why a "
      "redeploy shows up in it rather than overwriting it. Under the program "
      "deployed above there %s %d:"
      % ("is" if len(live) == 1 else "are", len(live)))
    d()
    d("| what | agent | transaction | block |")
    d("|---|---|---|---|")
    for r in live:
        c = confirm(chain, r["tx"])
        d("| `%s` | `%s…` | %s | %d |"
          % (r.get("what", "anchor"), r["agent_id"][:8], link(r["tx"]), c["block"]))
    d()
    if older:
        counts = ", ".join("`%s…` (%d)" % (p[:8], len(rs)) for p, rs in older.items())
        d("A further %d row%s belong%s to superseded programs — %s. Those "
          "transactions are still on chain and still resolve, but the policy "
          "accounts they created were derived from a different ImageID and no "
          "longer exist under the current one. They are evidence of what was "
          "redeployed, not of what is enforceable today."
          % (sum(len(rs) for rs in older.values()),
             "" if sum(len(rs) for rs in older.values()) == 1 else "s",
             "s" if sum(len(rs) for rs in older.values()) == 1 else "",
             counts))
    else:
        d("No superseded rows remain in that manifest at this commit.")
    return d.text()


def section_settlements(chain, root, agents, tasks, ident):
    d = Doc()
    # The shipped program's ProgramId, read off an on-chain anchor rather than
    # recomputed locally: the superseded-versus-shipped labelling below is the
    # strongest claim in this document and it must not depend on which tools a
    # machine happens to have installed.
    shipped = ident["image"]["words"]
    by_agent = {a["agent_id"]: a for a in agents}
    named = chain.program_ids() or {}
    transfer = list(named.get("authenticated_transfer") or [])

    rows, notes, mismatches = [], [], []
    for t in tasks:
        c = confirm(chain, t["settlement_tx"])
        updates = parse_updates(c["raw"])

        payee_raw = b58decode(t["server_pay_account"])
        hit = next((u for u in updates if u["account"] == payee_raw), None)
        if hit is None:
            raise Fatal("%s does not touch %s, the account the manifest says it "
                        "paid" % (t["settlement_tx"], t["server_pay_account"]))
        if str(hit["balance"]) != t["balance_after"]:
            mismatches.append(
                "%s: the transaction commits the payee to %d, "
                "`artifacts/a2a-task.tsv` says balance_after = %s"
                % (t["settlement_tx"], hit["balance"], t["balance_after"]))
        if transfer and program_id_words(hit["owner"]) != transfer:
            mismatches.append(
                "%s: the payee account is owned by %s, not by the authenticated "
                "transfer program"
                % (t["settlement_tx"],
                   ",".join(str(x) for x in program_id_words(hit["owner"]))))

        # The ledger account this settlement charged, found STRUCTURALLY: the
        # update whose data decodes as a 97-byte v1 record. It is NOT looked up
        # by address, and that is load-bearing rather than stylistic.
        #
        # A ledger address is PDA(program, agent_id), so a redeploy moves it.
        # artifacts/a2a-task.tsv now spans two programs, and its four rows
        # therefore straddle two different ledger accounts for the same agents.
        # Naming the address out of agents.tsv would look up the CURRENT PDA in
        # a transaction that charged the OLD one: measured against the live
        # manifest, two of the four rows find nothing. A check that finds
        # nothing does not fail -- it quietly stops testing anything, and
        # reports the remaining rows as a clean pass. Finding the record by its
        # shape instead means a redeploy cannot silently shrink the check.
        #
        # The manifest is then used only to cross-check the address, and only
        # for rows under the current program, where a disagreement is real.
        ledgers = [u for u in updates if ledger_record(u["data"])]
        if len(ledgers) > 1:
            raise Fatal("%s writes %d ledger records; a settlement charges one "
                        "envelope" % (t["settlement_tx"], len(ledgers)))
        ledger = under = ledger_account = None
        if ledgers:
            ledger = ledger_record(ledgers[0]["data"])
            under = program_id_words(ledgers[0]["owner"])
            ledger_account = b58encode(ledgers[0]["account"])
            client = by_agent.get(t["client"])
            # Only a settlement under the CURRENT program has to match the
            # manifest. One under a superseded program charges a PDA the
            # redeploy moved, so its address differs for a reason the chain
            # states -- that gets written down below, not treated as a bug.
            if (client and under == shipped
                    and client["policy_account"] != ledger_account):
                mismatches.append(
                    "%s charges policy account %s under the current program, but "
                    "artifacts/agents.tsv gives client %s the policy account %s"
                    % (t["settlement_tx"], ledger_account, t["client"],
                       client["policy_account"]))

        others = [b58encode(u["account"]) for u in updates
                  if u["account"] != payee_raw
                  and b58encode(u["account"]) != ledger_account]

        rows.append({"t": t, "c": c, "payee": hit["balance"], "ledger": ledger,
                     "under": under, "ledger_account": ledger_account, "others": others,
                     "updates": len(updates)})

    if mismatches:
        raise Fatal("the chain and artifacts/a2a-task.tsv disagree: "
                    + "; ".join(mismatches))

    d("Every figure in this table is decoded out of the settlement transaction "
      "itself. `getAccount` is deliberately **not** used for the balances: it "
      "reports current state, this chain has no historical-state RPC, and the "
      "payee's balance has since moved both up and down — so what it holds today "
      "is not evidence about a settlement that landed hundreds of blocks ago. A "
      "LEZ transaction commits to its own post-state, and the hash proves the "
      "bytes are that transaction, so the balance below is the balance *at* the "
      "settlement rather than a number cached in a file.")
    d()
    d("| # | settlement | block | on the wire | skill | price | payee balance after | policy `window` / `spent` after |")
    d("|---|---|---|---|---|---|---|---|")
    for i, r in enumerate(rows, 1):
        t = r["t"]
        ledger = ("`%s…` at %s / %s"
                  % (r["ledger_account"][:8], fmt(r["ledger"]["window_start"]),
                     fmt(r["ledger"]["spent"]))
                  if r["ledger"] else "this transaction writes no ledger record")
        d("| %d | %s | %d | %s bytes | `%s` | %s LEZ | %s | %s |"
          % (i, link(t["settlement_tx"]), r["c"]["block"], fmt(r["c"]["bytes"]),
             t["skill"], t["price"], fmt(r["payee"]), ledger))
    d()

    for i, r in enumerate(rows, 1):
        c = r["c"]
        notes.append(
            "Settlement %d: the sequencer's bytes hash to "
            "`%s`, which is the hash cited, and those bytes were found inside "
            "block %d and in neither block %s. The transaction touches %d "
            "account%s."
            % (i, c["tx"], c["block"],
               " nor ".join(str(n) for n in c["neighbours"]),
               r["updates"], "" if r["updates"] == 1 else "s"))
        if r["under"] is not None:
            same = r["under"] == shipped
            notes.append(
                "  The envelope it charged, `%s`, is owned by ProgramId `%s`, "
                "which %s the program this repository ships. %s"
                % (r["ledger_account"], ",".join(str(x) for x in r["under"]),
                   "is" if same else "is **not**",
                   "The anchor and the settlement are under the same deployment."
                   if same else
                   "**This settlement was made under a superseded deployment.** "
                   "Its policy account was derived from a different ImageID and "
                   "no longer exists under the program deployed today. It is a "
                   "real transaction and it resolves on the explorer, but it is "
                   "not evidence about the program in this repository."))
        else:
            notes.append(
                "  Its post-state contains no 97-byte policy record, so the "
                "envelope it was charged against cannot be read out of the "
                "transaction and none is claimed here. Everything else in this "
                "row still comes from the transaction's own bytes.")
        if r["others"]:
            notes.append(
                "  It also writes %s, which is stated rather than omitted so a "
                "settlement that quietly moved a third account cannot pass "
                "unremarked."
                % ", ".join("`%s…`" % o[:8] for o in r["others"]))
    for n in notes:
        d(n)
    d()

    orphans = sum(1 for r in rows
                  if r["under"] is not None and r["under"] != shipped)
    if orphans:
        d("**%d of the %d settlements above predate the program this repository "
          "ships.** They are kept because they are on chain and a reviewer will "
          "find them, but the criterion they support is only supported by the "
          "%d made under the current deployment."
          % (orphans, len(rows), len(rows) - orphans))
        d()

    d("What the chain cannot show, stated rather than implied: the payer is a "
      "shielded account, so only the credit side of each settlement is publicly "
      "readable. `getAccount` answers with a fully-populated default account — "
      "zero balance, zero nonce, zero owner — for a shielded address exactly as "
      "it does for one that has never existed, so it is not an existence check "
      "and no debit is quoted here. The debit is constrained anyway: LEZ rule 8 "
      "requires total balance to be preserved across every program in a "
      "transaction, so a transaction that credited %s LEZ debited %s LEZ."
      % (tasks[0]["price"], tasks[0]["price"]))
    d()
    d("The explorer indexes roughly an hour and three quarters behind the "
      "sequencer, so a settlement that landed in the last hour or two reads "
      "\"not found\" at the links above while `getTransaction` already returns "
      "it. That is an indexing lag, not a missing transaction; the RPC is the "
      "immediate source of truth and this document was generated from it.")
    return d.text()


# --------------------------------------------------------------------------
# splice
# --------------------------------------------------------------------------

def render(chain, root):
    agents = read_tsv(
        os.path.join(root, "artifacts/agents.tsv"),
        required=("category", "agent_id", "pay_account", "policy_account",
                  "per_tx", "per_period", "period_blocks", "create_tx", "owner"),
        optional=("claim_account", "claim_tx", "record_prefix"))
    anchors = read_tsv(
        os.path.join(root, "artifacts/anchored.tsv"),
        required=("program", "agent_id", "tx"),
        optional=("what",),
        # `create_tx` was this column's name until the claim/anchor split; an
        # alias keeps an older manifest readable without ever reading blind.
        aliases={"tx": ("create_tx",)})
    tasks = read_tsv(
        os.path.join(root, "artifacts/a2a-task.tsv"),
        required=("task_id", "client", "server", "server_pay_account", "skill",
                  "price", "settlement_tx", "balance_after"),
        optional=("balance_before", "nonce"))

    if chain.transaction(CONTROL_TX) is not None:
        raise Fatal("the cannot-exist control hash returned a transaction; this "
                    "sequencer's answers carry no information")

    # Established once, from the chain, and handed to both sections that need
    # it. Two sections deriving it separately is two chances to derive it
    # differently.
    ident = shipped_identity(chain, root, agents)

    return {
        "program": section_program(chain, root, agents, ident),
        "agents": section_agents(chain, root, agents, anchors),
        "settlements": section_settlements(chain, root, agents, tasks, ident),
    }


def guard(sections):
    """Refuse to emit the thing that gets submissions closed.

    Not a style rule. Closing comments on rejected submissions to this programme
    quote the placeholder back verbatim. A fact that could not be fetched is
    written as a sentence saying so; it is never a blank cell.
    """
    for name, body in sections.items():
        m = FORBIDDEN.search(body)
        if m:
            raise Fatal("section %r contains %r — a fact that cannot be fetched "
                        "must be written as a sentence, never left as a "
                        "placeholder" % (name, m.group(0)))
        for line in body.splitlines():
            if line.startswith("|") and re.search(r"\|\s*\|", line[1:]):
                if set(line.replace("|", "").replace("-", "").strip()) - {" "}:
                    raise Fatal("section %r has an empty table cell: %s"
                                % (name, line))


def splice(text, sections):
    out = text
    for name, body in sections.items():
        b, e = BEGIN % name, END % name
        block = "%s\n\n%s\n%s" % (b, body.rstrip(), e)
        pat = re.compile(re.escape(b) + r".*?" + re.escape(e), re.S)
        if not pat.search(out):
            raise Fatal("the document has no %r marker pair. Add:\n%s\n%s"
                        % (name, b, e))
        out = pat.sub(lambda _m: block, out, count=1)
    return out


def main():
    ap = argparse.ArgumentParser(
        description="Generate SUBMISSION-DRAFT.md's evidence sections from the chain.")
    ap.add_argument("--rpc", default=os.environ.get("SEQUENCER_URL", RPC_DEFAULT))
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), help="repository root to read manifests from")
    ap.add_argument("--write", metavar="FILE",
                    help="splice the sections into FILE between their markers")
    ap.add_argument("--check", metavar="FILE",
                    help="exit non-zero if FILE differs from what the chain now says")
    a = ap.parse_args()

    try:
        chain = Chain(a.rpc)
        sections = render(chain, a.root)
        guard(sections)
    except Fatal as e:
        print("submission-evidence: %s" % e, file=sys.stderr)
        print("submission-evidence: refusing to generate — the document would "
              "state something the chain does not.", file=sys.stderr)
        return 1

    if a.check:
        text = open(a.check, encoding="utf-8").read()
        try:
            want = splice(text, sections)
        except Fatal as e:
            print("submission-evidence: %s" % e, file=sys.stderr)
            return 1
        if want != text:
            print("submission-evidence: %s no longer matches the chain. Run "
                  "./scripts/submission-evidence.py --write %s" % (a.check, a.check),
                  file=sys.stderr)
            return 1
        print("submission-evidence: %s agrees with %s" % (a.check, a.rpc),
              file=sys.stderr)
        return 0

    if a.write:
        text = open(a.write, encoding="utf-8").read()
        try:
            new = splice(text, sections)
        except Fatal as e:
            print("submission-evidence: %s" % e, file=sys.stderr)
            return 1
        with open(a.write, "w", encoding="utf-8") as fh:
            fh.write(new)
        print("submission-evidence: rewrote %d section(s) in %s from %s"
              % (len(sections), a.write, a.rpc), file=sys.stderr)
        return 0

    for name in ("program", "agents", "settlements"):
        print(BEGIN % name)
        print()
        print(sections[name].rstrip())
        print(END % name)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
