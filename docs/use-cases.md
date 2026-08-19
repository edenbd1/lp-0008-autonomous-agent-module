# The illustrative use cases, end to end

The prize lists nine illustrative use cases and asks that at least three be
"demonstrated end-to-end on LEZ testnet". Four of the nine are demonstrated
here — 1, 2, 4 and 5 — each by a script that computes or fetches every claim
it makes in front of the reader, and each of which exits non-zero when its use
case does not hold. Script 3 is a fifth script, for a different criterion.

| | use case, as the prize words it | script | on chain? |
|---|---|---|---|
| 1 | **Personal file vault** — "owner sends files to the agent via chat; agent encrypts, stores on Logos Storage, and responds with a content address. Owner can retrieve from any device." | [`scripts/use-cases/01-file-vault.sh`](../scripts/use-cases/01-file-vault.sh) | no — real Storage and Messaging nodes, and the use case as the prize words it has no chain step in it |
| 2 | **Agent services marketplace** — "agents advertise skills on a shared discovery topic with a LEZ price; other agents discover, request, and pay for services autonomously." | [`scripts/use-cases/02-services-marketplace.sh`](../scripts/use-cases/02-services-marketplace.sh) | yes — the settlements are on the public testnet, and the amount each one moved is decoded out of the transaction |
| 4 | **Privacy-preserving notary** — "agent timestamps a document, uploads it to Logos Storage, and records the content address on LEZ — providing a verifiable, private proof of existence." | [`scripts/use-cases/04-privacy-notary.sh`](../scripts/use-cases/04-privacy-notary.sh) | yes — a real Storage node AND a transaction in a block on the public testnet |
| 5 | **On-chain event alerter** — "agent monitors a LEZ program or account for state changes and notifies the owner via Logos Messaging." | [`scripts/use-cases/05-event-alerter.sh`](../scripts/use-cases/05-event-alerter.sh) | yes — a `claim_agent` transaction in a block, found by reading the blocks rather than by being told |
| 3 | The spending threshold underneath all of them: **accepted below the anchored ceiling, refused above it.** | [`scripts/use-cases/03-spending-threshold.sh`](../scripts/use-cases/03-spending-threshold.sh) | yes — the ceiling is an account the chain holds |

Script 3 is deliberately **not** counted among the three. It is not one of the
prize's nine use cases: it demonstrates the spending-threshold Functionality
criterion, which is a criterion in its own right. It is numbered 3 because it was
written third, and it stays numbered 3 because its transactions and accounts are
cited by hash elsewhere in this repository.

**Which parts are on chain and which are not, stated once and plainly.** Use
cases 2, 3, 4 and 5 run against the public LEZ testnet at
`https://testnet.lez.logos.co` and every transaction and account they mention can
be re-read with `getTransaction` and `getAccount` by anyone. Use case 1 touches no
chain at all: it drives a real Logos Storage node and a real Logos Delivery node
on the live dev network, and there is no LEZ transaction in it because storing a
file and sending a message are not chain operations in this stack — nor are they
in the prize's own wording of that use case, which ends at "content address".
Saying "demonstrated on testnet" of use case 1 would be false, so it is not said.
Use case 4 is the one that does both halves: the same real Storage node, and then
a transaction in a block. Use case 5 reads the chain and writes to it only in its
`claim` mode, which is the event the watcher is there to detect.

## What any of this costs

A settlement costs real testnet balance and the funder holds 10 LEZ. So:

- **Use case 1 spends nothing.** No chain, no transaction.
- **Use case 3 spends nothing.** Its **two** refusals fail while the proof is
  being built, so no transaction is ever produced to submit. The first asks for
  201 LEZ against a ceiling of 200 and the second asks for 1 in a window that
  does not start on a period boundary — so neither could move money even if the
  ceiling failed completely, the first because the agent does not hold that much
  and the second because no block will include it. Its accepted side reads
  settlements that already landed. (There were three attempts once; the third can
  no longer be expressed, because `spend` carries no limits to disagree with.)
- **Use case 2 spends nothing by default.** It verifies the settlements the
  marketplace has already produced against the chain. Run it as
  `SETTLE=1 ./scripts/use-cases/02-services-marketplace.sh` to pay for a fresh
  task; it prints the price, the payer, the payee and the payee's current balance
  before it signs anything, and hands off to `scripts/a2a-task.sh`.
- **Use case 5 spends nothing in the mode CI runs.** `ALERTER_VERIFY_ONLY=1`
  re-reads every alert in `artifacts/alerts.tsv` off the chain and writes
  nothing. Its `claim` mode lands a `claim_agent`, which moves no value.
- **Use case 4 spends nothing, and it does write to the chain.** Its
  notarisation is an account-creation transaction that moves no value, and LEZ
  v0.2.4 charges no execution fee — so it costs 0 LEZ. That is not an argument
  from the fee schedule: the account that signs it is derived from the document
  and has never held a balance, so a chain that charged anything would have
  refused it. It did not.

## Running them

```bash
# spel is vendored but not built: that path does not exist in a fresh clone.
# Build it once (about five minutes), then point at it.
cargo build --release --locked -p spel --manifest-path vendor/spel/Cargo.toml
export SPEL_BIN=$PWD/vendor/spel/target/release/spel   # or any spel on PATH
./scripts/use-cases/02-services-marketplace.sh
./scripts/use-cases/03-spending-threshold.sh

# needs the Storage and Delivery libraries built from source; the script
# prints the two clone-and-make lines if they are missing
./scripts/use-cases/01-file-vault.sh

# needs the Storage library and the LEZ wallet. Writes one transaction.
# It must be the wallet built from a logos-execution-zone checkout
# (cargo build --release -p wallet), not any binary called `wallet`: this use
# case turns a document into a signing key and imports it, and a stock build has
# no `account import`. The script probes for that subcommand and says so.
export WALLET_BIN=/path/to/wallet
./scripts/use-cases/04-privacy-notary.sh
# re-check every notarisation ever made, without writing anything:
NOTARY_VERIFY_ONLY=1 ./scripts/use-cases/04-privacy-notary.sh

# two terminals: the watcher does not make the event it detects
./scripts/use-cases/05-event-alerter.sh prepare
./scripts/use-cases/05-event-alerter.sh          # terminal A
./scripts/use-cases/05-event-alerter.sh claim    # terminal B
# re-check every alert ever raised, without a wallet or a messaging node:
ALERTER_VERIFY_ONLY=1 ./scripts/use-cases/05-event-alerter.sh
```

**CI runs these.** `.github/workflows/ci.yml` has a `use-cases` job that runs 2,
4 and 5 in their verification-only modes against the public testnet, asserts on
the banner each one prints — and compares the count in that banner against the
number of rows in the manifest, so a script that verifies *some* of them is as
red as one that verifies none. Then it alters one field of each manifest and
requires the script to go red. That job exists because
`02-services-marketplace.sh` once sat on the default branch verifying zero
settlements and nothing ran it.

Each one reads `artifacts/agents.tsv` and `artifacts/a2a-task.tsv` **by column
name**, never by position. Both files have gained columns more than once, and a
script that says `$4` keeps running after a column moves — it just starts reading
the per-transaction limit out of the wrong field. That is not hypothetical: when
`a2a-task.tsv` gained a leading `program` column, the one loop in these scripts
still destructuring a row positionally reported a price of "skill" as being over
the ceiling and a transaction hash of "70" as missing from the chain.

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
  client  A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu  (blockchain)
  server  7o9PT8uEzF5TJLdF8zgo8vGAUZrx2xDEC8EscPGPEUM6  (storage)
  task    storage.upload at 25 LEZ, payable to 5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ
  OK   the card's payment account is the server agent's account in artifacts/agents.tsv
  OK   the manifest's policy_account is the PDA of the client agent's own id
  the client's anchored envelope lives at Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM
  OK   and the chain says it is owned by this repository's policy program
  OK   its per-transaction limit reads back as 200, the manifest's figure
  OK   25 <= the anchored per-transaction limit of 200: no owner in the loop

== 4. the A2A task lifecycle
  task 10e787e0cde95b80bf782468388ee6a6
  state -> submitted
  state -> working
  state -> completed

== 6. every settlement, decoded out of the chain's own copy of it

  task 192c7dcec965cd7bd3f5424f55e2715a
    storage.upload, 25 LEZ advertised, A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu -> Public/5Sa13NyN…
    4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1
  OK     the chain holds it, in block 8740
  OK     its 271471 bytes hash to exactly this settlement's hash
    the transaction commits to: payee on 70, ledger Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM on 25 for period 8000
  OK     it moved 25: period 8000 opened at zero and its ledger reads 25 after this
  OK     local record 45 -> 70 agrees with the transaction

  task b9a7ca40117a12207dac1d00572fe20d
    storage.upload, 25 LEZ advertised, A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu -> Public/5Sa13NyN…
    7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019
  OK     the chain holds it, in block 8747
  OK     its 271471 bytes hash to exactly this settlement's hash
    the transaction commits to: payee on 95, ledger Coxz1Cmfrcg6oUTqRhFxXsuwCrYwDfmV1GLjJxZk5rgM on 50 for period 8000
  OK     it moved 25: payee and ledger both advanced by the advertised price
  OK     local record 70 -> 95 agrees with the transaction

  task d31ded5afaec7dc843ba82f3480cecd5
    storage.upload, 25 LEZ advertised, GpRdooEWJjX4JmRyT2n5KzMnDKtCM2HrvZ8iwMZpe5FS -> Public/5Sa13NyN…
    e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047
  OK     the chain holds it, in block 8892
  OK     its 271471 bytes hash to exactly this settlement's hash
    the transaction commits to: payee on 70, ledger 7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp on 25 for period 8000
  OK     it moved 25: period 8000 opened at zero and its ledger reads 25 after this
  OK     local record 45 -> 70 agrees with the transaction

  task 13191d7c5674794b57c75c13214dea97
    storage.upload, 25 LEZ advertised, GpRdooEWJjX4JmRyT2n5KzMnDKtCM2HrvZ8iwMZpe5FS -> Public/5Sa13NyN…
    aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8
  OK     the chain holds it, in block 8901
  OK     its 271471 bytes hash to exactly this settlement's hash
    the transaction commits to: payee on 95, ledger 7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp on 50 for period 8000
  OK     it moved 25: payee and ledger both advanced by the advertised price
  OK     local record 70 -> 95 agrees with the transaction

  OK   4 settlement(s), each one decoded from the chain's own copy
  OK   control: a transaction hash that cannot exist returns null
       the last settlements were paid by another agent, so this is its ledger
  ledger 7HH46tXhgfrMSSzWwpNrjkqujCB9EGA5cEvnYK1dA7bp
  OK   it still reads 50 for period 8000: the sum of every price charged to it
  Public/5Sa13NyN… holds 95 LEZ right now, by getAccount
       current state, not a settlement figure — this account keeps moving in both directions
       the payer is a shielded account, so only the credit side is publicly readable
```

**Read the block numbers and the last balance together, because they are the
point of this section.** The payee holds 95 LEZ today, and it has held 45, 70,
95, 45 and 95 again in the course of a single day. Every one of those readings
was true when it was taken and none of them tells you what any settlement moved.
This is exactly why the amount a settlement moved is
decoded out of the settlement — `getAccount` answers with *current* state, and
this chain has no `getAccountAtBlock`, no `getBalance` and no
`getTransactionReceipt`. The previous version of this script differenced two
columns of `artifacts/a2a-task.tsv` against a price in the same file, which
cannot fail and is a statement about the file rather than about the chain.

What the verdict rests on now is two numbers that are inside the settlement
itself: a LEZ transaction commits to its post-state, so the payee's balance
after it and the policy program's running total after it are both in the bytes
`getTransaction` returns — bytes that are content-addressed by the settlement's
own hash, which the script checks before it decodes anything.

Note the two ledger addresses. A policy account is a PDA of `(program,
agent_id)`, so these four settlements charged **two different ledgers** between
two programs and two paying agents. The script does not name the ledger it
expects: it finds the 97-byte policy record in each settlement's own post-state
and only differences totals that came from the same account. Naming one would
have made the check quietly stop testing anything the first time a settlement
was paid by a different agent — which is what the third row is.

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

- **The card is not self-contained, and the key has to travel beside it.** A LEZ
  account id is not its public key and neither the card nor the RPC carries the
  mapping, so a verifier needs the x-only key from somewhere. This bullet used to
  conclude from that "the card cannot be verified by a stranger", which stopped
  being true when the key was published: `artifacts/agent-cards/<category>.pub`
  sits beside the card, `verify-agent-card.py --public-key` checks the signature
  and the payment-account binding with no wallet at all, and that is the path CI
  runs. What is still missing is an *in-band* binding from the key to the account
  id — see [`a2a-binding.md`](a2a-binding.md) §3.6.
- The card is written to `artifacts/agent-cards/` and read back from there;
  publishing it to a Logos Messaging discovery topic is what `agent.card()` does
  in the module, and this script does not drive that node.
- **The settlements in the transcript above were made under more than one
  program, and this bullet used to get that wrong in the worst available way.**
  It read "the third, `5a488f28…`, is under the current `8c87cc9b…`" —
  `8c87cc9b…` is a *superseded* program, and calling it current is the exact
  defect that once had four documents describing a dead deployment as live. The
  shipped program is `697746f5…`; `./scripts/verify-deployment.sh` attributes
  every row of `artifacts/a2a-task.tsv` by reading the ImageID out of the
  transaction rather than out of the manifest, and prints which rows are under
  the shipped program and which are earlier history. No count is written here
  either, for the reason the ledger in
  [`docs/DEPLOYMENT.md`](DEPLOYMENT.md) gives: settlements are appended by
  `scripts/a2a-task.sh`, so any number on this line is wrong the next time one
  lands — and this line said "three" while the transcript above it counted four.
  All of them are still on chain and all of them moved balance; the program a
  settlement was made under does not change either fact. See
  [`docs/limitations.md`](limitations.md), "Superseded programs are on the
  testnet".

---

## 3. Accepted below the ceiling, refused above it

The prize asks for an agent that acts autonomously below a threshold its owner
configures, and waits for approval above it. Everything else rests on that being
true of the chain rather than of the agent's source code: the agent holds its own
keys on a remote node, so whoever takes the process takes the spending, and an
`if (amount > limit)` in the agent is worth nothing against them.

So the ceiling is not a number stored anywhere the agent can reach. It is the
**data of an account whose address the agent cannot choose**: `PDA(program,
["agent-policy/v1", agent_id])`, one per agent. And writing that account takes
two signatures — the agent's, on `claim_agent`, naming the one account allowed
to anchor over it, and then that account's, on `create_policy`. A stranger who
knows the agent's public id holds neither.

Real output:

```
== 1. the envelope is account data, and this script decodes it
  owner  G64pMjF9MR2vZjjwCyCFsC7DvG4uUPJC7quJiih9uvCc
         = e02b85f5940df6d695ca88e19468adeb57a07273e3e3f3d53d3b2ba1e6423c75
  agent  A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu
         = 8761681eb6bdf2cc7bb2341a58b9c3213f3a0112c2195aa634db12c780c0fa90
  policy 2RK4dPwzDTAdgjUGpGsCkok962StYpPV14QpW3Wusvc9
  owner          e02b85f5940df6d695ca88e19468adeb57a07273e3e3f3d53d3b2ba1e6423c75
  OK     matches artifacts/agents.tsv
  per_tx         200
  OK     matches artifacts/agents.tsv
  per_period     1000
  OK     matches artifacts/agents.tsv
  period_blocks  1000
  OK     matches artifacts/agents.tsv
  spent          0 in period 0
  OK   the anchored envelope is what this repository says it is

== 2. the program that owns the ceiling is the program in this repository
  ProgramId(artifacts/programs/agent_verifier.bin)
         = 1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647
  deploy tx = sha256(len || bytecode) = 697746f52ff24019dbde4861c3649f49426904617840139a5405aa24cb5370bf
  OK   the chain holds that deploy transaction
  OK   control: a hash that cannot exist returns null

== 3. the anchored envelope exists on chain, at the agent's own address
  PDA(program, ["agent-policy/v1", A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu])
         2RK4dPwzDTAdgjUGpGsCkok962StYpPV14QpW3Wusvc9
  OK   the same account the manifest records — derived, not copied
  getAccount(...).program_owner = 1100188279,1826885024,3328836940,838231610,3865620566,360697372,1581853530,1631980647
  OK   owned by exactly the program above — the owner anchored this envelope

== 4. there is only one such address, so a bigger ceiling has nowhere to go
  the IDL's seeds for the policy account:
         const    agent-policy/v1
         arg      agent_id
  OK     one agent, one policy account — no limit is a seed of the address
  an agent nobody anchored     2qttGYZ6dKzNJNejZqmZxDnynbH8AW7S8zk98QE9q1mt
  OK     program_owner is all zeros: never initialised, so init would accept it
       the superseded program accepted the agent's own public pay account anchors an unlimited policy over it
         e530e0ba9a49c4ebacbfeaeac8fff3376f8bece24b71cb8f985b70c5399d462d
       the superseded program accepted the agent then moves its entire 65 LEZ in one transaction, against an owner-anchored ceiling of 25
         7fc6c9af06e590c7553af9d3090384e88a2780e38995117ca4e091f49a022228
       the superseded program accepted the same anchor against the storage agent
         d7498d65a77e9e0d550bf89ae16127d5bb328d42643c6eacd3e74a611fbdd09b
       the superseded program accepted the storage agent then moves its entire balance under it
         0a9ac12ce1442cd6d33c7eac02df8a120f13e558273e6a91a4289f4f15b0e170
       the superseded program accepted a STRANGER anchors an unlimited policy over an agent whose key it does not hold
         eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7
  the identical call, same agent, same limits, to the program deployed today
  OK     60de3fc607f98d15474fd288d366fa578d01de57c3fd20ba4191779337309040: submitted, never included

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

== 6. above the ceiling: refused, before a transaction exists
  asking for 201 LEZ, which is 1 above the ceiling of 200
  (and above the agent's balance, so nothing here can move money either way)
  block 8907, so the current period starts at 8000

  the agent simply asks for more than its envelope allows
         Program error 6005: the spend needs an owner approval: use spend_approved
  OK     refused with 6005: no transaction was built, so there is nothing to submit

  the agent slides its period forward a block to reset the total
         Program error 6014: the period named does not start on a multiple of period_blocks
  OK     refused with 6014: no transaction was built, so there is nothing to submit
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
program, which only the owner can create with `approve_spend`. That path is implemented **and has now run on the public testnet** — a fourth
agent, provisioned for it, whose owner was claimed *before* it anchored:
`approve_spend` in block 10776 and `spend_approved` in block 10786, with the
payee going 4 to 6 and the marker stamped single-use. What this script does not
show it against is the three agents it reads from `artifacts/agents.tsv`: their
owners anchored while still unclaimed, which is irreversible for them. The
account of both halves is in [`docs/limitations.md`](limitations.md), under
"CLOSED: an owner can approve a spend, and the approved spend executes".

---

## 4. Privacy-preserving notary

> "agent timestamps a document, uploads it to Logos Storage, and records the
> content address on LEZ — providing a verifiable, private proof of existence."

`scripts/use-cases/04-privacy-notary.sh`. Spends nothing. Writes one transaction.

The difficulty is that "verifiable" and "private" pull against each other. A
content address is a hash of the document, so putting it on chain in the clear
lets anyone who guesses the document confirm the guess, and tells everyone that
this agent notarised *something*. Putting nothing on chain proves nothing.

So the content address is turned into a key rather than into a record:

```
secret = sha256("lp0008-notary/v1" || content_address)
pubkey = the x-only secp256k1 public key of that secret
```

and the notarisation is a transaction on the public testnet **signed by that
key**. The chain stores the public key, in the witness, in a block.

- **Verifiable.** A holder of the document recomputes the content address,
  recomputes the key, fetches the transaction and compares. Nobody without the
  document can put that key on chain: the sequencer will not accept a
  transaction that is not signed under it.
- **Private.** The chain never sees the document, its content address, or its
  hash — only 32 bytes of public key, which open to nothing by inspection.
- **Timestamped.** By the block, which is what a block is for. The script reads
  the chain's own clock account (`/LEZ/ClockProgramAccount/0000001`, whose 16
  bytes are a block number and a millisecond time) rather than trusting the
  machine it runs on.

The first record, made by a real run and re-checkable by anyone:

| | |
|---|---|
| content address | `zDvZRwzm4a6BS3VPE6RXqhEyMGpz5hAGhUb4opHpmfSCENhBDUsL` (real Logos Storage node, manifest `datasetSize: 186`) |
| derived public key | `6d12fb6f03c219e35775de58050e2ce5ddc53dd5a2707cd5974c66d9c437299d` |
| notarising transaction | `aa0c0f9f3a88e5e193bf3ec7ac951ea5791d884c20ee723c1bfb0291b403de38` |
| block | 8882 |
| account created | `8x1cCdojpgRNziSyNvdscmk1zMLKg4vDrXj7t45Hy3LX` |
| cost | 0 LEZ |

Those values are in `artifacts/notary.tsv` as **identifiers**, and the script
re-derives or re-fetches every one of them: the block and the transaction bytes
from `getTransaction`, the public key by decoding those bytes, the expected
public key by recomputing it from the content address, and the account's
continued existence from `getAccount`. Run
`NOTARY_VERIFY_ONLY=1 ./scripts/use-cases/04-privacy-notary.sh` to re-check
every notarisation ever made without writing anything.

**The cost is zero and that is checkable rather than claimed.** The signing
account is derived from the document and has never held a balance. A chain that
charged anything for this transaction would have refused it.

**What is not claimed.** The content address is *not* stored on chain as data.
The deployed `agent_verifier` program has no instruction that writes arbitrary
bytes, so the binding runs through the signing key instead of through a stored
record. A program with a `notarise(address)` instruction would put the address
on chain where it could be read — and would thereby make it public, which is the
property this use case is named for. The honest sentence is "a transaction
signed by the key this document derives is in block 8882", and that is the
sentence the script prints.

---

## 5. On-chain event alerter

> "agent monitors a LEZ program or account for state changes and notifies the
> owner via Logos Messaging."

`scripts/use-cases/05-event-alerter.sh`. Spends nothing.

**This section did not exist while the table at the top of this document
promised it**, which is the same class of defect as the rest of the file: the
script was written, wired into CI's `use-cases` job and given a manifest, and
the document went on listing four use cases in a table and describing three.

**Which event, which is the whole question for an alerter.** Pointing a watcher
at something that changes on its own — the chain rewrites
`/LEZ/ClockProgramAccount/0000001` every block — produces a green transcript that
demonstrates polling and nothing else. The event here is a `claim_agent`: on this
deployment an agent names the one account allowed to anchor a policy over it, and
that claim costs nothing, needs no permission from the account it names, and is
`#[account(init)]` so it can never be rewritten. An owner therefore has a real
interest in learning that an agent has named them, and nothing in the stack tells
them. That is a step of the shipped flow rather than an event invented for a
demo.

**Why the event comes from a second invocation.** A demonstration that causes the
event it then detects reads as a loop even when it is not, so by default the
watcher does not make the claim — it prints the command and a second process, run
by the reader, makes it. `ALERTER_SELF_TRIGGER=1` runs both in one process, which
is what CI does, where orchestrating two is not worth the machinery.

```sh
./scripts/use-cases/05-event-alerter.sh prepare   # a fresh agent to watch
./scripts/use-cases/05-event-alerter.sh           # terminal A: watch
./scripts/use-cases/05-event-alerter.sh claim     # terminal B: the event
ALERTER_VERIFY_ONLY=1 ./scripts/use-cases/05-event-alerter.sh   # re-check, no wallet
```

Each alert is recorded in [`artifacts/alerts.tsv`](../artifacts/alerts.tsv) with
the agent, the claim account it appeared at, the owner it named, the transaction
and the block. `ALERTER_VERIFY_ONLY=1` re-reads every one of them off the chain —
the transaction from `getTransaction`, the claim account and its 33 bytes from
`getAccount` — and needs no wallet and no messaging node, which is why it is the
mode CI can run.

**What it costs: nothing.** `auth-transfer init` and `claim_agent` both move zero
value and LEZ v0.2.4 charges no execution fee, so a full run is 0 LEZ. The agent
it creates is funded with nothing and never needs to be.

**What this use case does *not* show.** The detection is a poll of `getAccount`,
not a subscription — this chain publishes no event stream, so "monitors" is
implemented as "reads the account until its bytes change". And the notification
half shares the limit use case 1 has: the address goes out on a real Logos
Messaging topic, and a second node receiving it is not asserted.

---

## Where each script's evidence comes from

| | reads | writes |
|---|---|---|
| 01 | `_external/logos-storage-nim`, `_external/logos-delivery` | a working directory under `$TMPDIR`; nothing in the repository |
| 02 | `artifacts/agents.tsv`, `artifacts/agent-cards/`, `artifacts/a2a-task.tsv`, the testnet RPC | nothing, unless `SETTLE=1`, in which case `scripts/a2a-task.sh` appends to `artifacts/a2a-task.tsv` |
| 03 | `artifacts/agents.tsv`, `artifacts/a2a-task.tsv`, `artifacts/programs/agent_verifier.bin`, the testnet RPC | nothing |
| 04 | `_external/logos-storage-nim`, `artifacts/notary.tsv`, the testnet RPC | one row in `artifacts/notary.tsv`, and one transaction on the testnet — neither unless the transaction was produced |
| 05 | `artifacts/alerts.tsv`, the testnet RPC, and a Delivery node for the notification | one row in `artifacts/alerts.tsv`, and — in `claim` mode only — one `claim_agent` on the testnet, which moves no value. Nothing at all under `ALERTER_VERIFY_ONLY=1` |

The helpers each script leans on, all under `scripts/use-cases/`:
`lib.sh` (manifest columns by name, RPC shapes, the control hash),
`verify-agent-card.py` (BIP-340 verification of an A2A card),
`settlement-facts.py` (a settlement's own committed post-state, decoded from
the transaction the chain returns — the only route to a historical balance on a
chain with no `getAccountAtBlock`),
`notary-key.py` (the content-address-to-key derivation, secp256k1 implemented
in thirty lines so a verifier can read it rather than install it),
`vault_drive.c` and `share_drive.c` (the Storage and Delivery node drivers).

Only 04 writes evidence of its own, and only after the chain has given it a
transaction hash. Everything the others check already exists — in the
repository, or on the chain — which is the property that lets a reviewer run
them from a clean clone and get the same answers.
