# Three illustrative use cases, end to end

The prize lists nine illustrative use cases and asks that at least three be
"demonstrated end-to-end on LEZ testnet". Three of them are demonstrated here,
each by a script that computes or fetches every claim it makes in front of the
reader, and each of which exits non-zero when its use case does not hold.

| | use case, as the prize words it | script | on chain? |
|---|---|---|---|
| 1 | **Personal file vault** — "owner sends files to the agent via chat; agent encrypts, stores on Logos Storage, and responds with a content address. Owner can retrieve from any device." | [`scripts/use-cases/01-file-vault.sh`](../scripts/use-cases/01-file-vault.sh) | no — real Storage and Messaging nodes |
| 2 | **Agent services marketplace** — "agents advertise skills on a shared discovery topic with a LEZ price; other agents discover, request, and pay for services autonomously." | [`scripts/use-cases/02-services-marketplace.sh`](../scripts/use-cases/02-services-marketplace.sh) | yes — three settlements on the public testnet |
| 3 | The spending threshold underneath both: **accepted below the anchored ceiling, refused above it.** | [`scripts/use-cases/03-spending-threshold.sh`](../scripts/use-cases/03-spending-threshold.sh) | yes — the ceiling is an account the chain holds |

Use case 3 is not one of the prize's nine. It is the mechanism the other two
stand on, and it is the claim this project is really making, so it gets a script
of its own rather than a paragraph inside one of the others.

**Which parts are on chain and which are not, stated once and plainly.** Use
cases 2 and 3 run against the public LEZ testnet at `https://testnet.lez.logos.co`
and every transaction and account they mention can be re-read with
`getTransaction` and `getAccount` by anyone. Use case 1 touches no chain at all:
it drives a real Logos Storage node and a real Logos Delivery node on the live
dev network, and there is no LEZ transaction in it because storing a file and
sending a message are not chain operations in this stack. Saying "demonstrated on
testnet" of use case 1 would be false, so it is not said.

## What any of this costs

A settlement costs real testnet balance and the funder holds 10 LEZ. So:

- **Use case 1 spends nothing.** No chain, no transaction.
- **Use case 3 spends nothing.** Its three refusals fail while the proof is being
  built, so no transaction is ever produced to submit — and each asks for 201 LEZ
  from an agent holding 25, so even a ceiling that failed completely could not
  move money. Its accepted side reads settlements that already landed.
- **Use case 2 spends nothing by default.** It verifies the settlements the
  marketplace has already produced against the chain. Run it as
  `SETTLE=1 ./scripts/use-cases/02-services-marketplace.sh` to pay for a fresh
  task; it prints the price, the payer, the payee and the payee's current balance
  before it signs anything, and hands off to `scripts/a2a-task.sh`.

## Running them

```bash
export SPEL_BIN=$PWD/vendor/spel/target/release/spel   # or any spel on PATH
./scripts/use-cases/02-services-marketplace.sh
./scripts/use-cases/03-spending-threshold.sh

# needs the Storage and Delivery libraries built from source; the script
# prints the two clone-and-make lines if they are missing
./scripts/use-cases/01-file-vault.sh
```

Each one reads `artifacts/agents.tsv` **by column name**, never by position.
That file has gained columns twice, and a script that says `$4` keeps running
after a column moves — it just starts reading the per-transaction limit out of
the policy hash.

---

## 1. Personal file vault

The owner hands the agent a document. The agent encrypts it, gives the
ciphertext to a real Logos Storage node, gets a content address back, and sends
that address — and only that address — to the owner over a real Logos Messaging
topic. The owner asks for the address back and decrypts.

Two C drivers do the node work, on the same contract as
`module/tests/*_node_drive.c`: every step is an assertion, and the exit code is
the result, so a node that silently failed to start cannot produce a passing
transcript.

- [`scripts/use-cases/vault_drive.c`](../scripts/use-cases/vault_drive.c) —
  Logos Storage: start, upload, manifest, retrieve to a file. The existing
  driver in `module/tests/` stops at the manifest, and a manifest is not the
  file: nothing about "the owner can get it back" is shown until bytes come back
  out.
- [`scripts/use-cases/share_drive.c`](../scripts/use-cases/share_drive.c) —
  Logos Delivery: subscribe to the owner channel topic and publish the address
  on it.

Real output:

```
== 2. the agent encrypts it before the node ever sees it
  aes-256-cbc, pbkdf2 200000 iterations, a 32-byte key made for this run
  sha256  25b41150698ad0008522c42b56918e49dfc6b00888dd4d23b3667a53a3f7d67c
  OK   the ciphertext is not the plaintext
  OK   control: the marker is findable in the plaintext
  OK   the marker does not appear in the bytes the node is about to be handed

== 3. the agent stores it on a real Logos Storage node
  1. the agent's storage node
    ok    the node started
    <-    peer id: 16Uiu2HAmGaUXszCT3hhTGm4TbbM6izC62dtFqrRs9JdzCHzMiMtK
  2. the agent stores the encrypted file
    <-    cid: zDvZRwzm7xEAJ8SK3Zy9UbHDUDtAVCsgBF5o91qrGPyrrEfTUytw
    ok    the node returned a content address
    <-    manifest: {"manifestVersion":0,"treeCid":"zDzSvJTf49uAN2BZhoMhZmDVSv7KDVeUVXTXvDc6QNnU1TKJYD55",
                     "blockSize":65536,"datasetSize":240,"filename":"secret.enc","mimetype":null}
    ok    its manifest names the file that was stored
  3. the owner asks for the address back
    <-    exists: true
    ok    the real address is in the store
    ok    download_init opened a session
    ok    the node streamed the content back to a file
  4. control: an address that cannot exist
    <-    ghost: zDvZRwzm7xEAJ8SK3Zy9UbHDUDtAVCsgBF5o91qrGPyrrEfTUqtw
    ok    the control address differs from the real one
    <-    exists: false   (a key lookup, not an answer about the address)
    <-    manifest: Failed to fetch manifest zDvZRwzm7xEAJ8SK…EfTUqtw after 10 attempts
    ok    the node cannot produce the content behind the control address

== 4. the agent sends the address to its owner over Logos Messaging
    topic:   /lp0008/1/owner-vault/proto
    payload: zDvZRwzm7xEAJ8SK3Zy9UbHDUDtAVCsgBF5o91qrGPyrrEfTUytw
    base64:  ekR2WlJ3em03eEVBSjhTSzNaeTlVYkhEVUR0QVZDc2dCRjVvOTFxckdQeXJyRWZUVXl0dw==
    <-    MyPeerId: 16Uiu2HAm5doGZxWiU4JgYFDmmRYNKvfRyPe3dVabpJJrLqbys72F
    event {"eventType":"message_propagated","requestId":"b9dd396e3403d9720889",
           "messageHash":"0xdf7890cb057187ca3cf64b33c90b7fa679c400df02412360bde6f2e1fb76eaa7"}
    event {"eventType":"message_sent","requestId":"b9dd396e3403d9720889",...}
    ok    the address reached the network, and stayed sent
    note  delivered back to this subscriber: not observed
  OK   the published payload decodes to exactly the content address

== 5. the owner retrieves it, by content address alone
  retrieved 240 bytes, sha256 25b41150698ad0008522c42b56918e49dfc6b00888dd4d23b3667a53a3f7d67c
  OK   byte-for-byte the ciphertext that was stored
  OK   it decrypts with the owner's key
  OK   and it is the owner's original document: sha256 f27183477b1c723ead326b4ee2a69d38f0d105340a790f201922ebc30d4413ba
  OK   the marker is back
```

### The controls, and why they are there

- **A content address that cannot exist.** The real CID with one character
  changed. A Logos content address carries no checksum over that field, so the
  mutant is well formed and addresses nothing. What the control asks it for is
  the **data**, not `storage_exists` — see below.
- **The marker is findable in the plaintext.** Otherwise "the marker is not in
  the ciphertext" is equally consistent with a `grep` that finds nothing
  anywhere.
- The run was falsified on purpose: with a fake `openssl` earlier on `PATH` that
  copies instead of encrypting, the script reports
  `FAIL the 'ciphertext' hashes to the same thing as the plaintext` and
  `FAIL the marker survives into the ciphertext — this is not encryption`, and
  exits 1.

### `storage_exists` is not a control, and finding that out cost a run

The first version of this driver asked the node `storage_exists` about the
mutated address and asserted it answered `false`. It ran green twice and then
this happened:

```
    <-    cid:   zDvZRwzkwUMxMuCqnKiGDWb1BiSzHChEkK6B1pmC9hMd2iyAWQbx
    <-    ghost: zDvZRwzkwUMxMuCqnKiGDWb1BiSzHChEkK6B1pmC9hMd2iyAWqbx
    <-    exists: true
    FAIL  the node does not claim to hold it
```

Reproduced against the same store, one character at a time: `…AWqbx` answers
`true`, `…AWQbr` and `…AWQqx` answer `false`, and a longer string is rejected as
unparseable. `exists` is `hasLocalBlock`, which is `cid in localStore` — a
datastore key lookup — and a near miss can land on a key that is present. Asked
for the actual content, the same address fails and says why:

```
Failed to fetch manifest: Cid doesn't match the data
```

which is the store finding a block whose content is not what that address names.
So the driver prints `exists` for both addresses and believes neither, and the
assertion is on the manifest fetch. An `exists`-only control passes while proving
nothing, which is exactly the class of check this repository keeps trying not to
ship.

The control also runs **last**, after the real retrieval has been asserted: a
failing manifest fetch can time out rather than answer, and a late reply to a
timed-out call satisfies the next wait.

### What this use case does *not* show

- **The encryption is the script's, not the module's.** `UploadSkill` in
  `module/src/storage_skills.cpp` is a passthrough to the node; it does not
  encrypt, and the prize's wording for `storage.upload` says it does. An agent
  that wants an encrypted vault has to encrypt before it calls the skill, which
  is what the script does with `openssl`. That is a real gap and it is named here
  rather than hidden behind calling `openssl` "the agent".
- **"Any device" is one node here.** The store and the retrieval are driven
  against the same running Logos Storage node. What a second device needs is the
  address, and the address is what crosses the network in step 4. A second
  Storage node fetching the same address from the network is not demonstrated.
- The message is published and the network propagates it; a second Delivery node
  receiving it is not demonstrated either. The driver registers
  `onMessageReceived` and reports whether the publisher's own subscription saw it
  come back, but does not assert it — a relay is not obliged to hand a publisher
  its own message, so asserting it would be a claim about the network rather than
  about the agent.

---

## 2. Agent services marketplace

The storage agent publishes a signed A2A Agent Card advertising `storage.upload`
at 25 LEZ. The blockchain agent verifies the card, checks the price against the
envelope its owner anchored on chain, runs the A2A task lifecycle, and pays.
Nobody approves anything.

Real output:

```
== 1. the marketplace: a signed Agent Card on the discovery topic
  protocolVersion 0.3.0   transport logos-messaging
  name            logos-storage-agent  v0.1.0
  url             logos-messaging://7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6
  provider        LP-0008 reference agent <https://github.com/logos-co/lambda-prize>
  skill           storage.upload  in=['application/json'] out=['application/json']
  price           25 LEZ per task, to Public/5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ
  OK   every field A2A requires of an AgentCard is present

== 2. the card is signed by the key that owns the account it wants paying
  verified: 5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ signed this card, and it is the payment account
  OK   the signature verifies against the advertised payment account
  OK   control: rewriting the price breaks the signature

== 3. discovery, and the client's own limit
  client  9KdQSJ2tB9CGDWKZYFLEuZ28enPhzb2erPwTYVVXicNe  (blockchain)
  server  7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6  (storage)
  task    storage.upload at 25 LEZ, payable to 5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ
  OK   the card's payment account is the server agent's account in artifacts/agents.tsv
  the client's anchored envelope lives at BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS
  OK   and the chain says it is owned by this repository's policy program
  OK   25 <= the anchored per-transaction limit of 200: no owner in the loop

== 4. the A2A task lifecycle
  task 2f4d343166945c5612dc8ee57e726f2f
  state -> submitted
  state -> working
  state -> completed

== 6. every settlement, against the chain

  task 53d4db4323990a3466a872e08322d29d
    storage.upload, 25 LEZ, 9KdQSJ2t… -> Public/5Sa13NyN…
    c45d3f2441cf1d19d69ae4cc70cfd50308fc2f0ed89ec40310c5ea2a94cf7275
  OK     getTransaction returns it
  OK     the recipient went 0 -> 25, exactly the advertised price

  task b68c9e512d9a78fc1e4e8c6d34b6a0a2
    8d7aba60786d812d6e596624518a38813e7b9f4573d20b6efe802ac4bb7502fb
  OK     getTransaction returns it
  OK     the recipient went 25 -> 50, exactly the advertised price

  task 671bb087f48904f4e7f71d0eeceed0e8
    5a488f287857e7f77204547360c710b295bfd1a269ea26f89bb34021aa00c554
  OK     getTransaction returns it
  OK     the recipient went 50 -> 75, exactly the advertised price

  OK   3 settlement(s), each one live on the public testnet
  OK   control: a transaction hash that cannot exist returns null
  Public/5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ holds 75 LEZ, by getAccount
       the payer is a shielded account, so only the credit side is publicly readable
```

Anyone can re-read the last line:

```bash
curl -s -X POST https://testnet.lez.logos.co -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getAccount",
       "params":["5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ"]}'
```

### The controls

- **A transaction hash that cannot exist** —
  `dededededededededededededededededededededededededededededededede` — must
  return `"result":null`. This chain answers `null` for a dropped transaction, a
  pending one and one that was never submitted alike, so "the chain returns it"
  is worth nothing unless the same question about something that is not there
  comes back empty.
- **Rewriting the card breaks its signature.** The verifier
  ([`verify-agent-card.py`](../scripts/use-cases/verify-agent-card.py)) is run a
  second time on a card with the price changed to 1, and the run fails if that
  still verifies. The verifier also self-tests against the published BIP-340
  vectors before it is allowed to call anything valid.
- **The card must be signed by the account it asks you to pay.** A card signed by
  some other key of the same agent would verify and still be a licence to
  redirect the money, so the `kid` in the JWS header is compared against
  `x-logos.paymentAccount`.
- Falsified on purpose: with one settlement hash in the manifest replaced by the
  impossible hash, the script reports
  `FAIL getTransaction returns null — an unconfirmed hash is not a payment` and
  exits 1.

### What this use case does *not* show

- **The card cannot be verified by a stranger.** A LEZ account id is not its
  public key, and neither the card nor the RPC carries the mapping, so
  `verify-agent-card.py` reads `pk` out of a wallet that holds the account. That
  is a real limit on who can check a card today, and `--wallet-home` is the gap
  rather than a convenience.
- The card is written to `artifacts/agent-cards/` and read back from there;
  publishing it to a Logos Messaging discovery topic is what `agent.card()` does
  in the module, and this script does not drive that node.
- The three settlements were produced by `scripts/a2a-task.sh`. The first two are
  under the superseded program `b028eabf…`; the third, `5a488f28…`, is under the
  current `8c87cc9b…`. All three are still on chain, and all three moved balance;
  the program a settlement was made under does not change either fact. See
  [`docs/limitations.md`](limitations.md), "Superseded programs are on the
  testnet".

---

## 3. Accepted below the ceiling, refused above it

The prize asks for an agent that acts autonomously below a threshold its owner
configures, and waits for approval above it. Everything else rests on that being
true of the chain rather than of the agent's source code: the agent holds its own
keys on a remote node, so whoever takes the process takes the spending, and an
`if (amount > limit)` in the agent is worth nothing against them.

So the ceiling is not a number stored anywhere. It is an **address**. The policy
account is the program-derived address of `sha256(owner, agent, per-tx,
per-period, period)` — raising a limit does not edit an account, it names a
different account, one that `create_policy` never initialised.

Real output:

```
== 1. the envelope is a hash, and this script recomputes every one of them
  ok   storage     610135ad0af56840c3ca91093b13d5aa299737f41181905f39933e3e86047ac3
  ok   messaging   2a1e29408d3c866e8974a88e9616326c5e718544a86a6afa21469e95815d3a60
  ok   blockchain  1a317aae885143298b3b033539273a02ff9c0c4f55e586f979a22b15c6e7c356
  OK   the derivation reproduces every anchored policy in artifacts/agents.tsv
  owner  FF8HZ8d38chXGDoZ1VV1pKBkoFx4QqyLwDsRjzEbngy9
         = d3a1fc6686a01570add47b72a98659d1e2a88e832123c0f13521ab8bd387f4bc
  agent  9KdQSJ2tB9CGDWKZYFLEuZ28enPhzb2erPwTYVVXicNe
         = 7ba31cb85ba5a21fa5f6b3854f28076c75d4982a1b96332afcbb05f34d11a11b
  sha256(prefix, owner, agent, 200, 1000, 1000)
         = 1a317aae885143298b3b033539273a02ff9c0c4f55e586f979a22b15c6e7c356
  OK   matches the policy hash in artifacts/agents.tsv

== 2. the program that owns the ceiling is the program in this repository
  ProgramId(artifacts/programs/agent_verifier.bin)
         = 2148920614,3576134543,3415609557,259224239,1770396588,3252552076,3201387284,3192958219
  deploy tx = sha256(len || bytecode) = 8c87cc9b2f4ef75cb8061dc3bb1a5bf531b56ce5a75c7b0b781d799f2d20ebbe
  OK   the chain holds that deploy transaction
  OK   control: a hash that cannot exist returns null

== 3. the anchored envelope exists on chain, at its own address
  policy account for 200/1000 per 1000 blocks:
         BLHNchq8haEZ8w1UPk68Qr6sGLzYZB6haBrZLZ4GhpsS
  getAccount(...).program_owner = 2148920614,3576134543,3415609557,259224239,...
  OK   owned by exactly the program above — the owner anchored this envelope

== 4. a bigger ceiling is a different address, and nobody created it
  per-tx 2000                  F5tEquWHzoB1q7nSF3oh2hP3rK7bQ5dR2gZpQeoqcPBr
  OK     program_owner is all zeros: never initialised
  per-period 10000             9xyojyLDK9QRNtjkGHjas6QxbtFNx6LMYZ4RvEur2WuZ
  OK     program_owner is all zeros: never initialised
  period 100 blocks            FueG4Qd8Qz5A1EWFAszPDgnraadXCbrBvpUs4TvPgvZ6
  OK     program_owner is all zeros: never initialised
  control policy hash          BHDMUhjkd4o1oCnoq6B5bXezefpxdscVX1jx6TUhHj2G
  OK     program_owner is all zeros

== 5. below the ceiling: accepted, unattended, and already on chain
  25 LEZ  c45d3f2441cf1d19d69ae4cc70cfd50308fc2f0ed89ec40310c5ea2a94cf7275
       25 <= 200, so spend takes the autonomous branch
  OK     the chain holds it
  OK     the recipient went 0 -> 25, exactly the price
  25 LEZ  8d7aba60786d812d6e596624518a38813e7b9f4573d20b6efe802ac4bb7502fb
  OK     the chain holds it
  OK     the recipient went 25 -> 50, exactly the price
  25 LEZ  5a488f287857e7f77204547360c710b295bfd1a269ea26f89bb34021aa00c554
  OK     the chain holds it
  OK     the recipient went 50 -> 75, exactly the price
  5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ holds 75 LEZ now, by getAccount

== 6. above the ceiling: refused, three ways, before a transaction exists
  asking for 201 LEZ, which is 1 above the ceiling of 200
  (and above the agent's balance, so nothing here can move money either way)
  block 8685, so the current period starts at 8000

  the agent simply asks for more than its envelope allows
         Program error 6005: the spend needs an owner approval: use spend_approved
  OK     refused with 6005: no transaction was built, so there is nothing to submit

  the agent presents the anchored account but claims a bigger ceiling
         Program error 6001: policy_hash does not commit to these limits
  OK     refused with 6001: no transaction was built, so there is nothing to submit

  the agent names the bigger envelope's own account instead
         Program error 6002: no policy is committed for these limits
  OK     refused with 6002: no transaction was built, so there is nothing to submit
```

The refusals are the whole point, and they are what is left of the moves
available to an attacker who owns the agent's process:

| move | refused by | why |
|---|---|---|
| ask for more than the envelope allows | `6005` | over the per-transaction ceiling; the program names the approved path instead of merely refusing |
| slide the period forward a block to reset the running total | `6014` | a window must start on a multiple of `period_blocks`, and the transaction is pinned to the one it names |

There used to be a third and a fourth row here — presenting the anchored account
while claiming bigger limits (`6001`), and naming the account those bigger limits
hashed to (`6002`). Both are gone because the moves are gone: `spend` carries no
agent id and no limits at all, so the policy account's address comes from the
account that PAYS and the ceiling is read out of it. There is nothing left in the
call to disagree with. `6001` and `6013` are retired rather than reused, so an
integration branching on them cannot silently match a different refusal.

The move that is *not* in this list, because it is not a move the agent makes,
is anchoring a second policy — and under the program before this one, anybody at
all could anchor the first. That is [`docs/security-model.md`](security-model.md)
§2, and it is the reason this deployment exists.

Each refusal happens while the proof is being built. No transaction is produced,
so there is nothing to submit and nothing to cost anything.

### The controls

- The **expected error code** is checked, not merely "some program error". An
  earlier version of this script passed `--window-start 0` and reported all three
  attempts as refused — correctly, and for the wrong reason: the policy account
  now carries the period it last spent in, and naming an older one is refused by
  `6015` before the ceiling is ever looked at. Naming the code is what makes each
  attempt a demonstration of the mechanism it claims to be about.
- The **cannot-exist transaction hash** returns null, and a **cannot-exist agent
  id** derives a policy address whose `program_owner` is eight zeros.
- **The address is resolved, not re-derived.** There used to be a
  `scripts/use-cases/policy-hash.py` here, a second implementation of the digest
  the guest computed — normally the way two halves of a system drift apart, and
  it needed a `--self-check` mode to keep them together. It is gone with the
  thing it computed: the policy account's address is a PDA of the agent, so the
  script asks `spel pda policy --agent-id` to resolve it from the published IDL,
  and the answer is compared against the `policy_account` column of
  `artifacts/agents.tsv`. One derivation, in the program, with the IDL as the
  interface to it.
- The refusals run against a **copy** of the agent's wallet home. A refused
  `spend` panics inside the guest, and a panicking run leaves the wallet store in
  a state the next run cannot load — observed here, and it would take the live
  agent down with it.
- Falsified on purpose: with `per_tx` in the manifest changed from 200 to 999,
  the script reports `FAIL computed 4b7b33b0… but the manifest records 1a317aae…`
  and exits 1.

### What this use case does *not* show

The **approval** side. `spend_approved` takes a fourth account — an approval PDA
seeded by the exact payment (policy, recipient, amount, nonce) and owned by this
program, which only the owner can create with `approve_spend`. That path is
implemented and is not demonstrated on the public testnet, because an owner
account that has anchored a policy has already spent its one program transaction
and cannot sign a second. The reason is written up in
[`docs/limitations.md`](limitations.md), "The owner can never approve a spend
after anchoring a policy". What is demonstrated here is the half that works:
below the line the agent acts alone, above it the chain will not let it.

---

## Where each script's evidence comes from

| | reads | writes |
|---|---|---|
| 01 | `_external/logos-storage-nim`, `_external/logos-delivery` | a working directory under `$TMPDIR`; nothing in the repository |
| 02 | `artifacts/agents.tsv`, `artifacts/agent-cards/`, `artifacts/a2a-task.tsv`, the testnet RPC | nothing, unless `SETTLE=1`, in which case `scripts/a2a-task.sh` appends to `artifacts/a2a-task.tsv` |
| 03 | `artifacts/agents.tsv`, `artifacts/a2a-task.tsv`, `artifacts/programs/agent_verifier.bin`, the testnet RPC | nothing |

The helpers each script leans on, all under `scripts/use-cases/`:
`lib.sh` (manifest columns by name, RPC shapes, the control hash),
`verify-agent-card.py` (BIP-340 verification of an A2A card),
`vault_drive.c` and `share_drive.c` (the Storage and Delivery node drivers).

None of the three writes evidence of its own. Everything they check already
exists — in the repository, or on the chain — which is the property that lets a
reviewer run them from a clean clone and get the same answers.
