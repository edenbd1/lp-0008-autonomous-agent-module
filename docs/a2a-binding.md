# The A2A transport binding over Logos Messaging

The prize asks that "agent-to-agent coordination is A2A-compatible: Agent Cards
follow the A2A schema, task interactions follow the A2A task lifecycle, and the
implementation is documented as an A2A transport binding over Logos Messaging".
This is that document: the binding itself, written so that a third party who has
never read this repository can implement the other end and interoperate.

It is a specification and an audit at once. Every rule below is stated as a
requirement, and where the shipped code does not yet meet its own requirement
that is said in the same place rather than in a footnote — with the file and the
line, so the claim can be checked in a minute. Section 8 is the honest
conformance table against A2A's own compliance section, and section 7 is what
this binding does not do at all.

The keywords MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

---

## 1. Where this binding sits in A2A

### 1.1 The version this is written against

A2A **v0.3.0**. That is the version this repository's cards declare
(`protocolVersion: "0.3.0"`, `module/src/agent_skills.cpp:640`), the version its
card validator was written against (`agent_skills.cpp:469-471`), and the version
whose `TaskState` enum the lifecycle copies verbatim.

A2A v1.0 exists and this binding does not target it. Three of its changes are
breaking for anything written here, and are listed in §7.7 so nobody discovers
them by shipping against the wrong one.

### 1.2 A2A's own extension clause, and the one rule this cannot satisfy

A2A §3.2.4 ("Transport Extensions") is the clause this binding claims:

> Additional transport protocols **MAY** be defined as extensions to the core A2A
> specification. Such extensions:
> - **MUST** maintain functional equivalence with the core transports
> - **MUST** use clear namespace identifiers to avoid conflicts
> - **MUST** be clearly documented and specified
> - **SHOULD** provide migration paths from core transports

And the rule it cannot satisfy, A2A §3.1:

> A2A communication **MUST** occur over **HTTP(S)**.

Those two are in tension in v0.3.0 itself. §3.1 says communication MUST be over
HTTP(S) and that agents MUST implement one of the three core transports; §3.2
softens the same sentence to SHOULD and adds "**They MAY be compliant
implementing a transport extension as defined in 3.2.4**". A binding that
replaces HTTP with a gossip network is squarely inside §3.2.4 and squarely
outside §3.1, and no reading of v0.3.0 makes both true.

This document takes the §3.2.4 reading, and states the consequence plainly: an
off-the-shelf A2A client that speaks only HTTP cannot talk to a Logos agent, and
this binding does not pretend otherwise. What interoperates is the *data* — the
Agent Card, the JSON-RPC request objects, the task states — and any client that
implements this document's twelve pages of transport rules. The prize's
"cross-framework agent interoperability" use case therefore needs a bridge, and
§7.6 says what that bridge would be.

### 1.3 Namespace identifiers

| Namespace | Value | Where |
|---|---|---|
| Transport name (`preferredTransport`) | `logos-messaging` | `agent_skills.cpp:647` |
| URL scheme | `logos-messaging://` | `agent_skills.cpp:646` |
| Card extension block | `x-logos` | `agent_skills.cpp:711-721` |
| Skill parameter-schema extension | `x-logos-parameters` | `agent_skills.cpp:695` |
| Content-topic application | `lp-0008` | `messaging_skills.cpp:12-20` |

`preferredTransport` is typed as a free `string` in the A2A v0.3.0 JSON schema —
the `JSONRPC | GRPC | HTTP+JSON` list is `examples`, not an `enum` — so
`logos-messaging` is schema-valid, not a violation smuggled through. Likewise,
`AgentCard` and `AgentSkill` do not set `additionalProperties: false`, so
`x-logos` and `x-logos-parameters` are legal extension members rather than
tolerated junk.

No method name is invented. This binding carries A2A's own
`message/send` and `tasks/cancel` and nothing else, so A2A §3.5.5 (extension
method naming) never applies.

---

## 2. Addressing

### 2.1 The content-topic grammar

Logos Messaging addresses traffic by *content topic*, not by peer. Every topic in
this binding follows the grammar Logos documents in
[LIP-23](https://lip.logos.co/messaging/informational/23/topics.html):

```
/<application>/<version>/<name>/<encoding>
```

with `application = lp-0008`, `version = 1`, `encoding = json`.

### 2.2 The three topics

| Purpose | Topic | Function |
|---|---|---|
| Owner channel, and one-to-one traffic with any account | `/lp-0008/1/owner-<account>/json` | `ownerTopic()`, `messaging_skills.cpp:12` |
| Agent Card discovery, per namespace | `/lp-0008/1/discovery-<namespace>/json` | `discoveryTopic()`, `messaging_skills.cpp:17` |
| One A2A task | `/lp-0008/1/task-<agentAccount>-<taskId>/json` | `taskTopic()`, `agent_skills.cpp:199` |

`<account>` and `<agentAccount>` are base58 LEZ account ids. `<taskId>` is the
A2A task id.

A task topic is derived from **the peer's account and the task id**, and both
ends compute it the same way, so no rendezvous or session setup is needed: the
client publishes its request there and the server publishes its status updates
there. `agent.subscribe` subscribes to exactly the same string
(`agent_skills.cpp:1052`).

Two consequences a third party must understand before deploying this:

- **A task topic is a public, cleartext string containing the peer's account id
  and the task id.** Anyone on the network sees that agent `9Xpkkvos…` is running
  task `a1b2…` — and, from the card, what that agent charges. The *contents* are
  protected by whatever Delivery's entry layer provides; the *existence, timing
  and counterparty* of every task are not. This binding does not fix that, and
  §7.4 says what would.
- **A task topic is a rendezvous, not a permission.** Nothing stops a third party
  publishing a forged status update on it. The lifecycle guard
  (`TaskStore::applyUpdate`) refuses illegal transitions and unknown task ids,
  which limits the damage to "a stranger can drive a task you opened into
  `failed`". Authenticating status updates is not implemented; see §7.3.

### 2.3 The `logos-messaging://` URL

A2A §5.6.1 requires that `url` and `preferredTransport` agree, and that the
transport named MUST be available at that URL. For this binding:

```
url                = "logos-messaging://" + <agent's LEZ account id>
preferredTransport = "logos-messaging"
```

The authority component is the agent's **shielded** LEZ account (`x-logos.lezAccount`),
which is the agent's identity. It is *not* the account that gets paid; see §6.2.

A card that names `logos-messaging` as its preferred transport and whose `url`
does not start with `logos-messaging://` MUST be rejected. This is one of the two
rules the validator adds on top of A2A's own schema
(`agent_skills.cpp:543-553`).

`additionalInterfaces` is not emitted, because there is no second transport to
declare. A2A §5.6.4 requires at least one transport declared through
`url`/`preferredTransport` **or** `additionalInterfaces`; the former is present,
so the requirement is met.

---

## 3. The Agent Card

### 3.1 What a conforming card looks like

The card `agent.card` builds (`agent_skills.cpp:639-721`), and the one
`scripts/a2a-task.sh` publishes to testnet, field for field:

```json
{
  "protocolVersion": "0.3.0",
  "name": "logos-storage-agent",
  "description": "Encrypts and stores a file on Logos Storage, returns its content address",
  "url": "logos-messaging://9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE",
  "preferredTransport": "logos-messaging",
  "version": "0.1.0",
  "provider": {
    "organization": "LP-0008 reference agent",
    "url": "https://github.com/logos-co/lambda-prize"
  },
  "capabilities": {
    "streaming": true,
    "stateTransitionHistory": true,
    "pushNotifications": false
  },
  "defaultInputModes": ["application/json"],
  "defaultOutputModes": ["application/json"],
  "skills": [
    {
      "id": "storage.upload",
      "name": "storage.upload",
      "description": "Encrypt and upload a file, returning a content address",
      "tags": ["storage"],
      "inputModes": ["application/json"],
      "outputModes": ["application/json"],
      "x-logos-parameters": { "type": "object", "properties": { "…": {} } }
    }
  ],
  "x-logos": {
    "lezAccount": "9XpkkvosC14TKTNZAoUdKXJwCheJ3dF8u3Xoojfv1FaE",
    "paymentAccount": "Public/5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ",
    "pricePerTask": 25,
    "settlement": "lez-chained-authenticated-transfer"
  },
  "signatures": [ { "protected": "eyJhbGci…", "signature": "…" } ]
}
```

The live one is in [`artifacts/agent-cards/storage.json`](../artifacts/agent-cards/storage.json).

Rules a producer MUST follow:

- **`skills` comes from the skill registry, not from a literal.** `agent.card`
  reads the module's own `meta.skills()` output (`agent_skills.cpp:667-709`,
  wired at `agent_module_plugin.cpp:216-218`), so a card cannot advertise a skill
  the agent has not registered, and cannot omit one it has.
- **`skills[].id` is the skill name**, e.g. `storage.upload`. `tags` is the
  substring before the first `.` (`["storage"]`), or `["logos"]` when there is no
  dot. A2A requires `tags` to be present and non-empty.
- **`x-logos-parameters` carries the JSON Schema for the skill's parameters.**
  A2A v0.3.0 has no field for an input schema and the prize asks the card to
  declare input/output schemas, so it travels as an extension member rather than
  being dropped. If that schema has a string `description`, it becomes the
  skill's `description`.
- **`capabilities.streaming: true`** means this binding's streaming (§4.5), not
  SSE. **`pushNotifications: false`** is not a placeholder: there are no
  webhooks here and `tasks/pushNotificationConfig/*` is not implemented.
- **`provider` is optional, but if present both `organization` and `url` are
  required** — A2A's own rule, and one this repository has published a card
  violating before. `agent.card` refuses to emit a half-filled provider rather
  than emitting one (`agent_skills.cpp:657-663`).

### 3.2 Publication

`agent.card` **builds** a card. It does not publish one. Publication is a
messaging act and is done separately:

```
agent.card()                          -> the signed card document
messaging.send / a publish onto        -> discoveryTopic(<namespace>)
  /lp-0008/1/discovery-<ns>/json
```

This is the same split `storage.share` makes, and it is deliberate: a document
and a delivery fail for different reasons and should report separately.

A publisher SHOULD republish its card periodically. Nothing on this transport
retains a message forever, and §3.3 explains why a subscriber that joined after
your last publish sees nothing at all.

### 3.3 Discovery

```
agent.discover({"topic": "/lp-0008/1/discovery-<ns>/json",
                "require_signed": true})
```

`agent.discover` passes the topic through **exactly as given**
(`agent_skills.h:220-223`); turning a namespace into a content topic is
`discoveryTopic()`'s job, and `messaging.join(group_id)` is the call that
subscribes to it (`messaging_skills.cpp:108`).

For each document fetched it returns either a summary — `name`, `url`,
`version`, `signed`, `transport`, `skills` (ids only), `price`, `lez_account`,
`pay_account`, and the full `card` — or an entry in `rejected` carrying the
index and the reason. Nothing is dropped silently: `seen` is the count of
documents the transport handed over, and `agents.length + rejected.length`
equals it.

`require_signed: true` rejects a card carrying no `signatures` array. Read §3.6
before believing that means very much.

**How cards are actually retrieved is the honest gap in discovery.** A relay
`subscribe(contentTopic)` delivers messages published *while you are subscribed*.
A card published before you joined is retrievable only through the Delivery
module's `storeQuery(jsonQuery, peerAddr, timeoutMs)` against a store service
peer — which its own header marks "⚠️ USE AT YOUR OWN RISK: backed by the kernel
API, subject to change at any point without a deprecation cycle"
(`_external/logos-delivery-module/src/delivery_module_plugin.h:150-183`). A
conforming implementation therefore MUST do one of:

1. republish cards on a timer, and accept that discovery has a warm-up window; or
2. use `storeQuery` with `contentTopics: ["/lp-0008/1/discovery-<ns>/json"]` and
   accept the stability warning.

`DiscoveryPort.fetch` (`agent_skills.h:218-224`) is the seam where either goes.
**Nothing in this repository wires it.** The skill refuses with "no discovery
transport is configured" when it is empty (`agent_skills.cpp:780`), which is the
correct failure and is not the same as working.

### 3.4 Validation

`validateAgentCard(cardJson)` (`agent_skills.cpp:596`) returns an empty string
for a valid card and the reason otherwise. It is applied in three places — to
cards read off a discovery topic, to a peer's card passed to `agent.task`, and to
the agent's *own* output before signing (`agent_skills.cpp:725-728`), so an agent
cannot publish a card its own peers would reject.

It enforces the A2A v0.3.0 `AgentCard.required` set:

`protocolVersion`, `name`, `description`, `url`, `version` (non-empty strings);
`capabilities` (object); `defaultInputModes`, `defaultOutputModes` (non-empty
arrays of non-empty strings); `skills` (array, each with non-empty `id`, `name`,
`description` and a non-empty `tags` array).

Plus A2A's rule on `provider` (both fields or neither), the RFC 7515 shape of
each `signatures` entry (`protected` and `signature`, both non-empty), and the
two rules this binding adds:

1. a card whose `preferredTransport` is `logos-messaging` MUST have a
   `logos-messaging://` `url`;
2. a card whose `x-logos.pricePerTask` is greater than zero MUST name an
   `x-logos.paymentAccount`. A price nobody can pay is worse than no price: the
   client would settle into nothing and call the task accepted.

### 3.5 Signing

A2A carries a card signature as an RFC 7515 JWS with a **detached payload**
(`AgentCardSignature`: `protected`, `signature`, optional `header`). The signing
input is:

```
BASE64URL(UTF8(protected header)) || '.' || BASE64URL(UTF8(canonical card without "signatures"))
```

**Canonicalisation.** The payload is the card with its own `signatures` member
removed, serialised as compact, key-sorted JSON — no whitespace, members in
ascending key order. Both signers in this repository do this
(`nlohmann::json::dump()` sorts by key and emits no spaces;
`json.dumps(card, sort_keys=True, separators=(",", ":"))` in
`scripts/sign-agent-card.py:226`). See §7.5 for the case where those two do
*not* agree.

**Algorithm.** LEZ account keys are BIP-340 Schnorr over secp256k1
(`k256::schnorr`). JOSE registers `ES256K` for *ECDSA* over that curve and
nothing for Schnorr, so a registered name would be a false one. This binding
therefore specifies:

```json
{"alg": "secp256k1-bip340", "kid": "<base58 account id>"}
```

The 32-byte message BIP-340 signs is `SHA-256(signing input)`
(`sign-agent-card.py:228-229`) — BIP-340 takes a 32-byte message, and the signing
input is not 32 bytes, so a prehash is unavoidable and both ends MUST use the
same one. The signature is deterministic (`aux_rand = 0`), so re-signing an
unchanged card produces a byte-identical document rather than a new one that
merely says the same thing.

**Which key signs.** The key that owns the account the card asks you to send
money to — `x-logos.paymentAccount`. An unsigned or wrongly-signed card is a
document anyone on a public discovery topic can rewrite, payment account
included, so the signature has to be over the very thing the reader is being
asked to trust.

**No unsigned fallback.** `agent.card` refuses to emit a card when there is no
signing key, and refuses when the key returns nothing
(`agent_skills.cpp:744-749`). "Refuse" rather than "emit unsigned", because a
document that reads like a signed card and is not one is worse than no card.

**The two signers in this repository disagree with each other**, and a third
party must know which to follow. See §7.5.

### 3.6 Verification — and why nobody can do it yet

A verifier MUST:

1. take the card, remove `signatures`, canonicalise as §3.5;
2. base64url-decode `protected`, read `alg` and `kid`;
3. recompute the signing input and its SHA-256;
4. recover the signer's 32-byte x-only public key;
5. BIP-340-verify;
6. check that the recovered key is the key that owns the account named in
   `x-logos.paymentAccount`.

**Steps 4 and 6 cannot be performed from the card.** This is the most serious
defect in the binding as shipped, and it was found by trying it:

- `kid` is the base58 **account id**. It is not the public key, and it is not
  derived from it. For the published card, `kid = 5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ`
  decodes to `41fb94fa…`, while the key that signed is `87447003…`. They are
  unrelated, and no hash of the key (SHA-256, SHA3-256, BLAKE2b/2s) produces the
  account id either.
- The card carries no `jwk`, no `x5c`, and no key material anywhere in `x-logos`.
- `getAccount` returns `program_owner`, `balance`, `data`, `nonce` and no public
  key, so the chain does not close the gap through the documented RPC.

The docstring at `scripts/sign-agent-card.py:18-21` claims "a reader who has the
card has everything needed to check it". **That claim is false** and is recorded
here rather than repeated.

The fix this binding specifies, and which is not implemented:

> A conforming card MUST carry the signing key. Add
> `x-logos.signingKey` — the 32-byte x-only public key, lower-case hex — and
> require a verifier to check that key against the account named by `kid` using
> whatever binding the chain exposes. Without the second half, `signingKey` only
> proves the card is internally consistent, which a forger can also arrange.

And what the code does today, so nobody mistakes it for the above:
`agent.discover` checks that a `signatures` array **exists and is non-empty**
(`agent_skills.cpp:798-799`). It performs no cryptography whatsoever. `signed:
true` in a discovery summary means "carries a signatures member", not "verified".

---

## 4. The transport mapping

### 4.1 Which A2A message goes where

| A2A method | Direction | Topic | Emitted by |
|---|---|---|---|
| `message/send` (new task) | client → server | `taskTopic(server, taskId)` | `agent_skills.cpp:966-978` |
| `message/send` (continuation) | client → server | `taskTopic(server, taskId)` | `agent_skills.cpp:889-901` |
| `tasks/cancel` | client → server | `taskTopic(server, taskId)` | `agent_skills.cpp:1097-1100` |
| `TaskStatusUpdateEvent` | server → client | `taskTopic(server, taskId)` | consumed by `TaskStore::applyUpdate`, `agent_skills.cpp:288` |
| Agent Card | publisher → all | `discoveryTopic(ns)` | §3.2 |
| `tasks/get` | — | — | **not implemented**, §7.1 |
| `message/stream`, `tasks/resubscribe` | — | — | not sent; §4.5 |
| `tasks/pushNotificationConfig/*` | — | — | not implemented; the card declares `pushNotifications: false` |

A new-task request on the wire, exactly as built:

```json
{
  "jsonrpc": "2.0",
  "id": "9f0c…<32 hex>",
  "method": "message/send",
  "params": {
    "message": {
      "kind": "message",
      "role": "user",
      "messageId": "<32 hex>",
      "taskId": "<32 hex>",
      "contextId": "<32 hex>",
      "parts": [
        { "kind": "data", "data": { "skill": "storage.upload", "params": {} } }
      ]
    }
  }
}
```

A continuation — the answer to a task sitting in `input-required` or
`auth-required` — is the same envelope with the same `taskId` and `contextId`,
and a `DataPart` carrying `{"message": "<text>", "params": {…}}` instead.

A cancellation:

```json
{"jsonrpc": "2.0", "id": "<32 hex>", "method": "tasks/cancel", "params": {"id": "<taskId>"}}
```

`params` here is A2A's `TaskIdParams`, whose required member is `id` — not
`taskId`. That is A2A's own inconsistency between `Message.taskId` and
`TaskIdParams.id`, faithfully reproduced.

A status update, in the shape the receiving side accepts:

```json
{"taskId": "<taskId>", "status": {"state": "working", "message": "reading the file"}}
```

`{"id": …, "status": {…}}` — an A2A `Task` object — is accepted too
(`agent_skills.cpp:296-300`). See §7.2 for where this diverges from A2A's
`TaskStatusUpdateEvent` and what a conforming sender should emit.

### 4.2 Reliable channel or bare topic

This is the choice that decides whether a paid task can silently evaporate, so
it is stated as a requirement and then measured against the code.

Logos Delivery offers both, and the difference is not cosmetic
(`_external/logos-delivery-module/src/delivery_module_plugin.h`):

- `send(contentTopic, payload)` / `subscribe(contentTopic)` — relay publish and
  subscribe. Fire-and-forget. Delivery is asynchronous and reported, if at all,
  through `messagePropagated` / `messageSent` / `messageError` events. There is
  no retransmission, no gap detection, and no ordering guarantee.
- `channelCreate(channelId, contentTopic, senderId)` / `channelSend(channelId,
  payload)` — a reliable channel, with inbound frames arriving as
  `channelMessageReceived(channelId, senderId, payload, timestamp)`. This is
  Delivery's default entry layer (`entryLayer: "channels"`).

**Requirement.** Task traffic — `message/send`, `tasks/cancel`, and the status
updates flowing back — MUST run over a reliable channel, with the task content
topic as the channel's content topic and a channel id derived from the task.
Discovery-topic publication MAY use a bare topic, because a card is idempotent,
republished, and worth nothing if it arrives twice.

The reason is specific to *this* binding rather than general good taste: the
client **pays after the request is delivered and never hears an acknowledgement**
(§6.4). A dropped `message/send` on a bare topic is indistinguishable from a
peer that has not started work yet, and the client has already paid for it.
Group traffic that silently drops is worse than a group that fails to open, and
paid traffic that silently drops is worse again.

**What the code does.** The rule is honoured where it has been wired and is not
yet wired where it matters most:

| Traffic | Mechanism | Status |
|---|---|---|
| Owner approval channel | reliable channel — `channelCreate(channelId, contentTopic, agentAccount)`, `channelSend` | wired, `owner_channel.cpp:286-321` |
| `messaging.create_group` | reliable channel — `channelCreate(group)` | wired, `messaging_skills.cpp:137` |
| `messaging.send` | bare topic — `send(ownerTopic(recipient), payload)` | wired, `messaging_skills.cpp:84-88` |
| A2A task traffic | **`TaskPort.send(topic, json)` — an unbound seam** | **not wired by anything in this repository** |

`TaskPort` (`agent_skills.h:227-241`) is shaped as "publish this JSON to this
content topic". That shape admits either mechanism: a host may bind it to
`send()` and get a bare topic, or to `channelCreate` + `channelSend` and get a
reliable channel. **A host implementing this binding MUST bind it to the
reliable-channel path.** No shipped code binds it at all — `SkillPorts.task`
(`agent_module_plugin.h:69`) is default-constructed to nothing, the skills refuse
with "delivery node is not started", and §7.1 records what that means for the
end-to-end claim.

### 4.3 Delivery readiness is not optional

Delivery's lifecycle is `createNode` once per context, then `start`, then message
operations, then `stop` — and **`start` returns as soon as the request is
dispatched**. Completion arrives later as a `nodeStarted` event. A send issued
immediately after `start` is a send into a node that may not be up, and it
vanishes without an error.

So every skill on this transport checks `ready()` *before* it does anything
irreversible, and refuses rather than returning success:
`agent.task` at `agent_skills.cpp:888` and `:960`, `agent.subscribe` at `:1050`,
`agent.cancel` at `:1095`, `messaging.*` at `messaging_skills.cpp:80, 106, 133`.

There is a related trap worth carrying into any implementation, because it fails
silently and looks exactly like a peer that never answered: **the event name you
register with is not the name that comes back.** You register `onMessageSent` and
the payload carries `"eventType":"message_sent"`; you register
`onChannelMessageReceived` and the payload carries
`"eventType":"channel_message_received"`. Matching the registration name never
fires. Two constants exist for exactly this reason
(`owner_channel.h:71-81`), and `parseInboundEvent` matches the second
(`owner_channel.cpp:238`). Channel payloads also cross the FFI base64-encoded.

### 4.4 There is no response

A2A is request/response: `message/send` returns a `Task` or a `Message`,
`tasks/cancel` returns the updated `Task`, and errors come back as JSON-RPC error
objects with A2A's own codes (`-32001 TaskNotFoundError`, `-32002
TaskNotCancelableError`, and so on).

**This binding has none of that.** A request is published to a topic and the
skill returns. The JSON-RPC `id` is generated (`randomId()`, 32 hex characters)
and put on the wire, but nothing correlates a reply to it, because no reply is
expected. Consequences, all of which a third party must design around:

- **Nothing is ever acknowledged.** A successful `agent.task` means "the request
  was handed to the transport", not "the peer received it" and certainly not
  "the peer accepted it".
- **No A2A error code is ever produced or consumed.** Failures are local, and
  reported in this repository's own skill-result shape: `{"ok": false, "error":
  "…"}`. A2A §11.1.5 requires the A2A error codes; this binding does not meet it
  (§8).
- **The only feedback path is a status update on the task topic.** A peer that
  wants to say "I will not do this" says it as a `rejected` or `failed` status
  update, not as an error response.

A conforming implementation of this binding therefore MUST treat every request as
fire-and-forget and MUST derive all knowledge of the peer's state from status
updates. That is the meaning of the design rule stated at `agent_skills.h:32-35`:
*local state is never a claim about the remote agent.*

### 4.5 Streaming

A2A §3.3 makes streaming explicitly transport-specific. Here it is topic
subscription: the server publishes `TaskStatusUpdateEvent`s to the task topic,
and `agent.subscribe(agent_address, task_id)` subscribes the client to that same
topic (`agent_skills.cpp:1052-1055`) and records `subscribed: true` on the task.

No `message/stream` and no `tasks/resubscribe` request is ever sent. On this
transport that is not a gap in the same way it is over HTTP: there is no
connection to resume, the topic is shared, and the server publishes whether or
not anyone is listening. Resubscribing is subscribing again. Missed events are
missed — the transport does not backfill, which A2A §7.9 explicitly leaves
implementation-defined.

`capabilities.streaming: true` on the card means this mechanism. A client that
speaks a core A2A transport and reads that flag will expect SSE and be
disappointed; that is a restatement of §1.2, not a separate problem.

### 4.6 Payload encoding

Every payload is UTF-8 JSON, and every content topic ends in `/json`. The
Delivery module base64-encodes the payload itself on the way across the FFI
boundary; that is the transport's business and not part of this binding.

The agent module adds **no encryption of its own**. Whatever confidentiality
exists is whatever the configured Delivery entry layer provides for that topic
or channel. A bare relay publish is addressed to a topic, not to a recipient:
everyone subscribed to the topic receives it. Do not read "Logos Messaging is
end-to-end encrypted" as "this binding encrypts task payloads to the peer" —
that inference is not established here.

---

## 5. The task lifecycle

### 5.1 The states

A2A's `TaskState` enum, spelled exactly as A2A v0.3.0 spells it and with no
additions (`agent_skills.h:51-61`, `agent_skills.cpp:113-146`):

```
submitted  working  input-required  auth-required
completed  canceled  failed  rejected  unknown
```

`completed`, `canceled`, `failed` and `rejected` are terminal. `unknown` is
A2A's "we have lost track" and is never a state a task is driven into
deliberately.

An unrecognised state name is a **protocol error**, not a reason to invent a
state: `taskStateFromName` returns false and `applyUpdate` refuses the whole
update (`agent_skills.cpp:316-319`).

### 5.2 The legal transitions

A2A v0.3.0 defines the state *names* normatively and says only that a terminal
task cannot be restarted (§6.1). It publishes no transition matrix. The matrix
below is therefore **this binding's**, it is stricter than anything A2A requires,
and it is enforced in exactly one place — `canTransition`,
`agent_skills.cpp:161-197` — so that a failing skill cannot corrupt the lifecycle
and two callers cannot disagree.

| From ↓ To → | submitted | working | input-required | auth-required | completed | canceled | failed | rejected | unknown |
|---|---|---|---|---|---|---|---|---|---|
| **submitted** | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ |
| **working** | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ |
| **input-required** | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ |
| **auth-required** | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ |
| **completed / canceled / failed / rejected** | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| **unknown** | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

The reasoning behind each rule, because a third party will otherwise relax them:

- **Nothing enters `submitted`.** Only `TaskStore::create` opens a task. A status
  update claiming `submitted` is refused, so a duplicate request cannot reset a
  task that is already running.
- **`working → working` is the one legal self-transition.** A streaming agent
  repeats `working` as progress arrives. Every other state that stays put is a
  bug dressed as an update, and is refused.
- **`working → rejected` is illegal.** Rejection is an answer to the *request*;
  once work has started the honest terminal state is `failed`.
- **`input-required` and `auth-required` do not chain into each other.** An agent
  that needs both asks for one, receives it, returns to `working`, and asks for
  the other.
- **Nothing leaves a terminal state.** A `completed` task that went back to
  `working` would un-finish work that was already paid for and reported. This is
  also A2A §6.1.
- **`unknown` is a dead end in both directions.**

### 5.3 What each skill does, precisely

**`agent.task(agent_address, skill, params, …)`** — `agent_skills.cpp:846`.

Order of operations, and it is deliberate: *check, post, pay.*

1. If `task_id` names an existing task, this is a **continuation**, not a new
   request. It is accepted only if the task is `input-required` or
   `auth-required`, it is with the same peer, and it carries a `message` or
   `params`. A retried `agent.task` on a task still in `submitted` is a duplicate
   request and is refused as one (`:879-883`).
2. If a peer `card` is supplied, it is validated as an Agent Card, it MUST
   advertise the requested `skill`, and its `x-logos.lezAccount` MUST equal
   `agent_address`. The price and payment account are taken **from the card**,
   because that is the figure the peer published and the only one it is bound to
   (`:919-940`). Without a card, the caller may pass `price` and `pay_account`
   directly.
3. `max_price` is checked before anything is sent and long before anything is
   signed. A task above it is refused (`:950-958`): proving time on a transfer
   the owner's envelope would refuse is paid for whether or not the chain accepts
   it.
4. `ready()` is checked, and only then is the task created. A transport that is
   down means **no task is opened at all**.
5. The request is published. If the publish fails, the task is moved to `failed`
   with "the request could not be delivered", and **nothing is paid**
   (`:980-987`).
6. Only now is the price paid (§6.4).
7. The task is returned in **`submitted`** (`:1010-1018`). It has posted a
   request; it has not seen the other agent start work. `working` arrives as a
   status update or not at all.

**`agent.subscribe(agent_address, task_id)`** — `agent_skills.cpp:1031`. Refuses
an unknown task, a task with a different peer, a task already in a terminal state
("there are no further updates to stream"), and a transport that is not ready.
Otherwise subscribes to the task topic and sets `subscribed`.

**`agent.cancel(agent_address, task_id)`** — `agent_skills.cpp:1073`. Refuses an
unknown task, a mismatched peer, and a task already terminal. **Refuses while the
transport is down** rather than marking the task canceled locally, because a
cancel that never leaves the node is not a cancel: the peer keeps working and
keeps the money. On a successful publish the task moves to `canceled` and the
refund path (§6.6) runs.

Note the asymmetry with §4.4's rule, and it is a real one: `agent.cancel` marks
the task `canceled` on a *successful send*, not on a peer acknowledgement,
whereas `agent.task` deliberately leaves the task in `submitted`. A2A §7.4 says
cancellation "success is not guaranteed" and that `tasks/cancel` returns the
peer's updated `Task`. This binding has no return path, so `canceled` here means
"this client has stopped participating", not "the peer stopped working". A third
party MUST NOT read a local `canceled` as a statement about the peer.

### 5.4 What happens when the transport is down

| Call | Transport down | Publish fails | Effect on the task |
|---|---|---|---|
| `agent.task`, new | refused before the task exists | task → `failed` | never created / `failed`, nothing paid |
| `agent.task`, continuation | refused | refused | stays in `input-required` / `auth-required`, retryable |
| `agent.subscribe` | refused | subscription refused | unchanged |
| `agent.cancel` | refused | refused | unchanged — **not** locally canceled |
| payment (after a successful publish) | — | settlement returns no hash | task → `failed`, "the payment did not settle", no retry |

The last row is the one to read twice. The request is out and the money is not.
It is reported as a failure rather than retried silently, because a second
attempt would risk paying twice for one task (`agent_skills.cpp:998-1006`).

### 5.5 Restarts

The prize counts losing pending task state across a node restart as a reliability
failure. `TaskStore` provides `snapshot()` → a JSON array of every task with its
id, context, peer, skill, state, price paid, payment account, settlement tx,
subscription flag, full state history and last note; and `restore(json)`
(`agent_skills.cpp:386-461`).

`restore` builds into a scratch map and swaps only on success, so a snapshot with
malformed JSON, a duplicate id or a state name A2A does not define leaves the
store **untouched**. A half-restored store is worse than an empty one.

A host that must survive a restart owns the store and passes it in through
`SkillPorts.tasks` (`agent_module_plugin.h:106-110`); the module's internal store
runs fine but cannot be snapshotted from outside. Persisting the snapshot is the
host's job and is not implemented here.

---

## 6. The payment binding

This is the part vanilla A2A has no answer for. A2A deliberately omits payment;
this project settles in LEZ.

### 6.1 Advertising a price

The `x-logos` block on the Agent Card:

| Member | Type | Meaning |
|---|---|---|
| `lezAccount` | string | the agent's **shielded** LEZ account — its identity, and the authority of the card's `url` |
| `paymentAccount` | string | the **public** account a price is paid into, as `Public/<base58>` |
| `pricePerTask` | unsigned integer | LEZ per task. `0` advertises a free agent |
| `settlement` | string | `lez-chained-authenticated-transfer` — how the payment is built |

Validator rules (`agent_skills.cpp:571-590`): `x-logos` MUST be an object;
`lezAccount`, if present, MUST be a non-empty string; `pricePerTask`, if present,
MUST be a non-negative integer; and **a non-zero `pricePerTask` MUST be
accompanied by a non-empty `paymentAccount`**.

`agent.card` refuses to produce a card at all if the agent charges a price and
has no account to be paid into (`agent_skills.cpp:634-637`), and normalises the
payment account to the `Public/` form `spel` resolves
(`agent_skills.cpp:717-720`).

The price is a flat per-task figure. There is no per-skill pricing, no quoting
round-trip, and no negotiation. A client that will not pay more than *N* passes
`max_price: N` and gets a refusal before anything is signed.

### 6.2 How an account to be paid is named — and the constraint that forces it

**A payer cannot name a foreign shielded account.** `spel` resolves a
`Private/<id>` account only for accounts the *signing* wallet holds keys for — it
builds them as `AccountIdentity::PrivateOwned`, and a private account's state
cannot be constructed without its viewing key. One agent naming another's private
account as recipient fails before anything is built:

```
❌ Failed to submit privacy-preserving transaction: KeyNotFoundError
```

It is the same wall as funding: `auth-transfer` reaches a shielded account
through `--to-keys`, never through an account id.

So the asymmetry is forced, and this binding states it plainly rather than
burying it:

> **The payee advertises a public receiving account. The payer stays shielded.**

Each agent keeps two accounts: its shielded identity (`x-logos.lezAccount`, the
card's `url` authority, the account that signs) and a public receiving account
initialised once under the transfer program (`x-logos.paymentAccount`). The
settlement is a privacy-preserving transaction signed by the payer's own private
account, paying into the payee's public one.

**What it costs in privacy**, precisely:

- The **recipient and the amount of every task payment are public.** Anyone
  holding the card holds the payment account, and `getAccount` on it is
  unauthenticated. An observer can count a paid agent's tasks, size each one, and
  time them.
- Because the same `paymentAccount` appears in every copy of the card, payments
  are **linkable to each other and to the advertised identity** across tasks.
  There is no per-task address.
- The **payer stays shielded**: which account paid is not visible on chain.
- But only *half* of the payment is hidden even so. `getAccount` reads public
  state only; a private account is a commitment in the private state and the RPC
  answers with a default account for it. So the **credit** side of a settlement
  is checkable by anyone and the **debit** side is not. The debit is still
  constrained — `validate_execution` rule 8 requires total balance to be
  preserved across every program in the transaction, so an accepted transaction
  that credits 25 has debited 25 from an account in the same call — but "the
  payer's balance went down" is a statement only the payer's wallet can show
  directly.
- Combined with §2.2, an observer who sees a task topic
  (`/lp-0008/1/task-<payee>-<taskId>/json`) and a credit to that payee's
  advertised account for the advertised price has learned that a task happened,
  which agent performed it, what it cost, and when — without breaking anything.

**The fix needs work upstream, not here.** The card would carry the recipient's
`npk`/`vpk` under `x-logos`, and `spel` would build a `PrivateForeign` recipient
from them. The wallet already has that account kind
(`lez/wallet/src/account_manager.rs:30-34`); the CLI does not expose it. Until
then, a shielded payee is not reachable and this binding does not claim one.

### 6.3 How a settlement is built

`spend` is the autonomous instruction of the anchored policy program. It takes
three accounts — the policy account, the agent that pays, and the account paid —
and it deliberately does **not** take an approval account: an above-threshold
payment calls `spend_approved` instead.

`spend` moves nothing itself and cannot: LEZ rule 5 refuses a post-state that
debits an account the executing program does not own, and the agent's account
belongs to the transfer program. So the policy check gates a **chained call**
into that program, which is why the transfer program's ELF has to be supplied
(`--bin-auth-transfer`) for the circuit to compose the inner call. That is what
`x-logos.settlement: "lez-chained-authenticated-transfer"` names.

The invocation, from `scripts/a2a-task.sh:276-281`:

```
spel --idl idl/agent_verifier.idl.json \
     --program artifacts/programs/agent_verifier.bin \
     --bin-auth-transfer artifacts/programs/authenticated_transfer.bin \
  -- spend --agent "Private/<client agent>" \
           --recipient "Public/<payee's paymentAccount>" \
           --amount <price> --window-start <block>
```

There is no `--policy-hash`, no `--owner-id`, no `--agent-id` and no limits, and
their absence is the design: the policy account's address is derived from the
account that PAYS, and the ceiling is read out of that account. Nothing in the
call is left for a caller to disagree with.

Two preconditions a third party will otherwise hit:

- **Resync before proving.** A private account's state changes on chain every
  time it signs. The proof is built against the wallet's local view, so an agent
  that spent once and never resynced proves against a stale pre-state: the proof
  still builds, `spel` still returns a transaction hash, and the sequencer simply
  never lands it. Nothing reports an error. That is why the first settlement by a
  fresh agent works and the second does not, which reads like an intermittent
  fault and is not one (`a2a-task.sh:186-202`).
- **The period window.** The transaction is only includable inside
  `[window_start, window_start + period_blocks)`. A settlement submitted in the
  last blocks of a period can miss it, and that is a refusal, not a network fault
  (`a2a-task.sh:222-244`).

### 6.4 Ordering: request first, payment second

`agent.task` posts the request and *then* pays (`agent_skills.cpp:980-1008`).
A price paid for a request that never left the node buys nothing, so a transport
failure must not reach the wallet.

The cost of that ordering, stated so nobody has to discover it: **the client pays
on submission, not on acceptance, and there is no escrow.** The prize text says
an agent "pays the declared LEZ price on task acceptance"; this binding cannot,
because §4.4 gives it no acceptance signal to wait for. A peer that takes the
money and never publishes a status update leaves the client with a settled
payment and a task sitting in `submitted` forever. The only recourse is §6.6,
and §6.6 depends on the peer's goodwill.

Designing that out needs a settlement instruction that releases on a proof of
delivery rather than on submission. That is a real piece of work, it is not here,
and it is named in §7 rather than implied away.

### 6.5 How settlement is proved

A payment is recorded against a task only through
`TaskStore::recordPayment(id, amount, payAccount, settlementTx)`, and that call
**refuses an amount with no transaction hash** (`agent_skills.cpp:333-336`):

> a payment without a settlement transaction is not a payment

An amount with no hash would make an unsettled task look paid for the rest of its
life. The same standard governs the evidence path. `scripts/a2a-task.sh` refuses
to write its manifest unless **both** of these hold:

1. `getTransaction(<hash>)` returns a result — a submitted hash is not a
   settlement, and the RPC cannot distinguish a dropped, a pending and a
   never-submitted hash: all three return the same null. The script polls for 12
   minutes at 60-second blocks (`a2a-task.sh:289-308`).
2. `getAccount(<paymentAccount>).balance` moved by **exactly** the price
   (`a2a-task.sh:310-322`). An included transaction is still not a payment: an
   earlier version of this instruction produced a real, confirmed, on-chain proof
   that a policy *permitted* 25 LEZ and moved nothing at all, and it was written
   up as a settlement.

That second check is the whole reason the payee is public (§6.2). A third party
verifying someone else's claimed settlement should perform both, in that order.

### 6.6 Cancellation and refunds

`agent.cancel` reports a refund only when it has a transaction to point at
(`agent_skills.cpp:1109-1127`):

```json
"refund": {"amount": 25, "paid_into": "Public/…", "pending": false, "tx": "…"}
```

and otherwise:

```json
"refund": {"amount": 25, "paid_into": "Public/…", "pending": true,
           "reason": "the refund did not settle" | "no refund path is configured"}
```

A free task returns `{"amount": 0, "pending": false}`.

**The refund port as shipped cannot be implemented honestly, and a third party
should not try.** `TaskPort.refund(paidAccount, amount)`
(`agent_skills.h:238-240`) is called in the *client's* process and is documented
as "reverse a settlement of `amount` that was paid into `paidAccount`". No client
can do that: the money is in the payee's account and only the payee can move it.

What a refund actually is, and what this binding specifies:

> A refund is a **new** LEZ transfer, signed by the payee, from its receiving
> account to the client's public receiving account, referencing the task id. It
> is initiated by the payee in response to a `tasks/cancel`, and the client
> observes it the same way anyone observes a settlement (§6.5).

Which also means the client's card must advertise a receiving account for a
refund to have a destination — the same public-payee constraint as §6.2, in the
other direction. Nothing wires `TaskPort.refund`, so `agent.cancel` today always
reports `pending: true` with "no refund path is configured". That is the correct
answer for an unwired port, and it is not a refund mechanism.

### 6.7 The spending envelope bounds all of it

A task payment is a spend, and a spend is bounded by an account address rather
than by a branch: the policy account is a PDA whose seed is the hash of (owner,
agent, per-tx, per-period, period-blocks). An agent wanting a larger ceiling
would have to present an account nobody created.

So `max_price` in `agent.task` and any price check in a client are *advisory* —
they save proving time on a transaction the chain would refuse. The ceiling is
the chain's. `meta.configure` accepts `per_tx`, `per_period` and `period_blocks`
and reports them as **not effective** for exactly this reason
(`agent_skills.cpp:1241, 1266-1270`): writing a larger number changes what the
agent asks the owner about and changes nothing about what the chain will let it
spend.

A task priced above the envelope needs owner approval, and
[`docs/limitations.md`](limitations.md) records why that path does not currently
work: the owner who anchored a policy is, by construction, unable to approve
anything under it. Below-threshold autonomous settlement is unaffected, and is
what the testnet evidence covers.

---

## 7. What this binding does not do

Carried over honestly, including the parts that are uncomfortable.

### 7.1 The server half is not implemented

There is no code in this repository that **receives** an A2A request. Nothing
routes an inbound Delivery event into `TaskStore::applyUpdate`; nothing reads a
`message/send` off a task topic, dispatches the named skill, and publishes status
updates back. `applyUpdate` is exercised only by
`module/tests/agent_skills_test.cpp:429-450`.

Concretely, the following are all true at once:

- `agent.task`, `agent.subscribe` and `agent.cancel` are complete **client-side**
  implementations with a validated lifecycle, a transport seam and a payment
  path.
- `TaskPort` is not bound to Logos Delivery by any shipped host
  (`SkillPorts.task`, `agent_module_plugin.h:69`), so in the loaded module every
  one of them refuses with "delivery node is not started".
- `tasks/get` is not implemented in either direction. A2A §11.1.2 lists it among
  the three methods an agent MUST implement.
- The end-to-end testnet evidence in [`docs/DEPLOYMENT.md`](DEPLOYMENT.md) covers
  the **card and the settlement**, which are the parts the chain and the network
  can be asked about. `scripts/a2a-task.sh:179-184` prints the three lifecycle
  states in a `for` loop; it does not drive `TaskStore`. The state machine is
  covered by unit tests, not by the testnet run, and this document will not
  describe a printed line as a transition.

### 7.2 The status-update shape is looser than A2A's

A2A's `TaskStatusUpdateEvent` requires `kind: "status-update"`, `taskId`,
`contextId`, `final` and `status`, and its `status.message` is a **`Message`
object**, not a string. In A2A's streaming transports it arrives wrapped in a
JSON-RPC success response.

`TaskStore::applyUpdate` requires only a task id (`taskId` or `id`) and a
`status.state` naming a state A2A defines. Extra members are ignored. A
spec-shaped event is therefore **accepted** — but its `status.message`, being an
object rather than a string, fails the `is_string()` test at
`agent_skills.cpp:321` and the note is silently dropped.

A conforming sender SHOULD emit the full `TaskStatusUpdateEvent`. A conforming
receiver SHOULD read `status.message.parts[]` for the note. Neither is
implemented; the first is a one-line change on the sending side and the second is
a small one on the receiving side, and both are stated here rather than done.

### 7.3 Status updates are not authenticated

A task topic is a public rendezvous. Anyone can publish a status update on it,
and the only defence is the transition matrix: a stranger cannot resurrect a
completed task, but can drive a task you opened from `submitted` to `failed`.

Cards are signed; task traffic is not. Signing status updates with the peer's
account key — the same construction as §3.5 — is the obvious fix and is not
implemented.

### 7.4 Metadata leaks

Restating §2.2 and §6.2 in one place, because these are the privacy costs of the
design rather than bugs in it:

- task content topics carry the peer's account id and the task id in cleartext;
- the payee's receiving account, the price and the timing of every settlement are
  public and linkable across tasks;
- only the payer is shielded, and only the debit side of the settlement is
  private.

### 7.5 The two card signers disagree with each other

There are two implementations of §3.5 in this repository and they do not produce
the same document. The one that has actually published a card to testnet is the
Python script; the one in the module has never been wired to a key.

| | `scripts/sign-agent-card.py` (published) | `CardSkill::invoke` (module default) |
|---|---|---|
| `alg` | `secp256k1-bip340` (`:221`) | `EdDSA` (`agent_skills.cpp:734`) |
| `kid` | the **payment** account (`:221`) | the agent's **lezAccount** (`agent_skills.cpp:735`) |
| `signature` encoding | lower-case **hex** (`:233`) | base64url, per the port's contract (`agent_skills.h:207-210`) |
| non-ASCII in the payload | `\uXXXX`-escaped (`json.dumps` defaults to `ensure_ascii=True`) | raw UTF-8 (`nlohmann::json::dump` defaults to `ensure_ascii=false`) |

Every row is a genuine interop break:

- **`EdDSA` is simply wrong.** LEZ account keys are BIP-340 Schnorr over
  secp256k1. A verifier that trusted the module's default header would attempt an
  Ed25519 verification against a Schnorr signature. `module/tests/agent_skills_test.cpp:156-157`
  currently locks the wrong value in.
- **`kid` must name the account whose key signed.** The module's default names
  the shielded identity, whose key the wallet does not expose the way
  `account_secret` reads a public account's — so a card signed under the module's
  default `kid` is verifiable by nothing.
- **Hex is not base64url.** RFC 7515 §2 and the A2A `AgentCardSignature` schema
  both say the signature is base64url-encoded. `artifacts/agent-cards/storage.json`
  carries 128 hex characters. A strict RFC 7515 verifier rejects the published
  card.
- **The escaping difference only bites on non-ASCII**, which today's cards do not
  contain, so it is latent rather than active. A card whose `description` gains a
  single accented character would be signed over two different byte strings by
  the two signers.

**This binding specifies `alg: "secp256k1-bip340"`, `kid` = the payment account,
and base64url for `signature`.** The published card meets the first two and not
the third. Fixing it is a one-line change in `sign-agent-card.py` plus a
re-signature, and it is deliberately not made in this document.

### 7.6 An off-the-shelf A2A client cannot talk to a Logos agent

§1.2. What a bridge would be, so the gap is at least dimensioned: an HTTP server
holding the Logos agent's keys, serving the card at
`/.well-known/agent-card.json` (A2A §5.3), accepting `message/send`, `tasks/get`
and `tasks/cancel`, translating each onto the topics in §4.1, and holding the
HTTP response open until the corresponding status update arrives. That bridge is
a custodian for the duration of a task and reintroduces the server this project
exists to remove, which is why it is described and not built.

### 7.7 A2A v1.0 breaks three things here

If a third party implements against the current A2A rather than v0.3.0:

- **Task state names changed from `kebab-case` to `SCREAMING_SNAKE_CASE`.**
  `input-required` becomes `INPUT_REQUIRED`. `taskStateFromName` would reject
  every v1.0 state name.
- **`kind` discriminators were removed** in favour of JSON member-based
  polymorphism, and `final` was removed from `TaskStatusUpdateEvent`. The
  `"kind": "message"` and `"kind": "data"` members in §4.1 are v0.3.0 shapes.
- **Card signing was formalised on RFC 8785 (JSON Canonicalization Scheme)**
  rather than "compact and key-sorted". For ASCII keys and small integers the two
  coincide; in general they do not, which makes §7.5's escaping row a v1.0
  conformance issue as well as an internal one.

### 7.8 Other limitations that reach this binding

From [`docs/limitations.md`](limitations.md), the ones a payment path inherits:

- A shielded agent can pay but cannot be paid at its shielded account (§6.2).
- `getAccount` cannot see a private balance, so only half a payment is publicly
  checkable (§6.2).
- The owner can never approve a spend after anchoring a policy, so an
  above-threshold task price has no working approval path (§6.7).
- Deployment is content-addressed and superseded programs remain on testnet;
  a policy account is a PDA of the program, so rebuilding the guest orphans every
  anchor made under the old one.
- The Delivery and Storage node drivers are local commands, not CI. A skipped or
  perpetually-flaky CI step counts as not run, which is the standard applied to
  everything else here.

---

## 8. Conformance against A2A §11.1

Honest, item by item. "Agent compliance" in A2A v0.3.0 §11.1.

| Requirement | Status |
|---|---|
| §11.1.1 Support at least one transport | **Extension only.** `logos-messaging` per §3.2.4. No core transport, and §3.1's HTTP(S) requirement is not met (§1.2) |
| §11.1.1 Expose a valid `AgentCard` | **Yes.** Validated against the v0.3.0 required set plus two binding rules (§3.4); one published on testnet |
| §11.1.1 Declare transport capabilities | **Yes.** `url` + `preferredTransport`, consistent per §5.6.4 |
| §11.1.2 `message/send` | **Client side yes, server side no** (§7.1) |
| §11.1.2 `tasks/get` | **No** (§7.1) |
| §11.1.2 `tasks/cancel` | **Client side yes, server side no** (§7.1) |
| §11.1.3 `message/stream` / `tasks/resubscribe` | Optional. Not sent; streaming is topic subscription (§4.5) |
| §11.1.3 `tasks/pushNotificationConfig/*` | Optional. Not implemented; card declares `pushNotifications: false` |
| §11.1.4 Multi-transport equivalence | N/A — one transport |
| §11.1.5 Valid JSON-RPC 2.0 request/response objects | **Requests yes, responses no** (§4.4) |
| §11.1.5 A2A data objects | **Mostly.** Card, `Message`, `DataPart`, `TaskState`, `TaskIdParams` are A2A shapes; `TaskStatusUpdateEvent` is accepted in a looser form (§7.2) |
| §11.1.5 A2A error codes | **No.** Failures are local `{"ok": false, "error": "…"}` (§4.4) |
| §3.2.4 Functional equivalence with core transports | **No** — `tasks/get` and the response path are missing |
| §3.2.4 Clear namespace identifiers | **Yes** (§1.3) |
| §3.2.4 Clearly documented and specified | **This document** |
| §3.2.4 Migration path from core transports | **Described, not built** (§7.6) |

The two lines that would most change this table are `tasks/get` and a response
path, and both are the same missing piece: nothing here receives.

---

## 9. Implementing the other end

A checklist for someone writing a peer, in the order the work has to happen.

1. **Run a Delivery node.** `createNode` with `entryLayer: "channels"`, `start`,
   and **wait for the `nodeStarted` event** — not for `start` to return (§4.3).
   Register listeners under `onMessageReceived` / `onChannelMessageReceived` and
   match `eventType` against `message_received` / `channel_message_received`, not
   against the registration name.
2. **Get two LEZ accounts**: a shielded identity, and a public receiving account
   initialised under the transfer program.
3. **Build a card** per §3.1 and validate it against §3.4 before publishing.
4. **Sign it** per §3.5: `alg` `secp256k1-bip340`, `kid` the payment account,
   payload the compact key-sorted card without `signatures`, message
   `SHA-256(signing input)`, signature base64url. Carry the x-only public key in
   the card (§3.6) or your card cannot be verified by anyone.
5. **Publish it** on `/lp-0008/1/discovery-<ns>/json`, and republish on a timer
   (§3.3).
6. **Subscribe to your own task topics.** For each task you accept, that is
   `/lp-0008/1/task-<your account>-<taskId>/json` — derived from *your* account
   because you are the server.
7. **Accept requests.** A `message/send` whose `params.message.parts[0].data`
   carries `{"skill": …, "params": …}`. Reject an unknown skill with a `rejected`
   status update, not with silence.
8. **Publish status updates** on the same topic as full `TaskStatusUpdateEvent`s
   (§7.2), respecting the transition matrix in §5.2. Repeat `working` for
   progress; use `input-required` when you need more, and expect the answer as a
   continuation on the same `taskId`.
9. **Expect payment after submission, not before work** (§6.4), into the account
   your card advertises. Verify it as §6.5 does — the transaction landed *and*
   the balance moved by exactly the price — before treating the task as paid.
10. **Handle `tasks/cancel`** by stopping work and publishing a `canceled` status
    update, and by initiating a refund as a new transfer if you hold money for
    work you did not do (§6.6).
11. **Persist your task store** across restarts (§5.5).

---

## Where the code is

| Thing | File |
|---|---|
| Task states, transitions, topic derivation, base64url | `module/src/agent_skills.cpp:113-235` |
| `TaskStore` — create, advance, applyUpdate, recordPayment, snapshot, restore | `module/src/agent_skills.cpp:241-461` |
| `validateAgentCard` | `module/src/agent_skills.cpp:467-601` |
| `agent.card` | `module/src/agent_skills.cpp:607-754` |
| `agent.discover` | `module/src/agent_skills.cpp:760-829` |
| `agent.task` | `module/src/agent_skills.cpp:835-1019` |
| `agent.subscribe` | `module/src/agent_skills.cpp:1025-1061` |
| `agent.cancel` | `module/src/agent_skills.cpp:1067-1129` |
| Ports (`CardPort`, `DiscoveryPort`, `TaskPort`) | `module/src/agent_skills.h:186-241` |
| Content topics | `module/src/messaging_skills.cpp:12-20` |
| Reliable channels vs bare topics | `module/src/messaging_skills.cpp:122-166`, `module/src/owner_channel.h:43-56` |
| Where the ports are handed in | `module/src/agent_module_plugin.h:43-111` |
| Card signing, as published | `scripts/sign-agent-card.py` |
| The end-to-end evidence path | `scripts/a2a-task.sh` |
| Lifecycle and card unit tests | `module/tests/agent_skills_test.cpp` |
