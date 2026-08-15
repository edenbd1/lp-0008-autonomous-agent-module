# The skill interface

The prize asks for "a documented skill interface (module/SDK) that can be used
to add new skills without modifying the core agent module". This document is
that interface: the contract, how a skill is registered, the loader convention,
a worked example that is compiled and run on every invocation of
`examples/agent-console/run.sh`, the boundary the interface does **not** cross,
and a reference for every skill the module ships with.

It is also the honest account of which of those skills are wired to a running
node and which are only compiled. That half is further down, under
[Status, honestly](#status-honestly), and it has not been softened.

## 1. The contract

One class, three methods, in
[`module/src/agent_module_interface.h`](../module/src/agent_module_interface.h).
It is the only header a skill needs — not the plugin header, not the Logos SDK,
not Qt, and not a JSON library.

```cpp
namespace logos::agent {

class ISkill {
public:
    virtual ~ISkill() = default;

    /// Stable identifier, e.g. `storage.upload`.
    virtual std::string name() const = 0;

    /// JSON Schema of the parameters, as a JSON *object*.
    virtual std::string parameterSchema() const = 0;

    /// Perform the skill. Returns a JSON result.
    virtual std::string invoke(const std::string &paramsJson) = 0;
};

} // namespace logos::agent
```

Everything crossing that boundary is a `std::string`. That is a deliberate
narrowing rather than an oversight: it means a skill and the module need not
agree on a JSON library, or link one at all, and it means a skill can be
compiled by a different toolchain revision than the module was. The worked
example below relies on exactly that — it parses and emits JSON by hand, so the
claim is tested rather than asserted.

### What the module guarantees to a skill

Each of these is a property of `AgentModuleImpl`, and each is exercised by
`module/tests/skills_test.cpp`:

| Guarantee | Why it is there |
|---|---|
| All three methods are called inside a `try`/`catch`. A skill that throws costs its own entry and nothing else. | The prize requires skill failures to be isolated. Trusting implementers not to throw is not isolation. |
| No method is called while the module holds its own lock. A skill may call `skills()`, `invoke()` or `registerSkill()` back from inside any of them. | A non-recursive mutex plus a re-entrant skill is a deadlock, and the deadlock would appear only in the deployment that had such a skill. |
| `name()` is called **once**, at registration, and the registered name is published from then on. | A name that changed between calls would advertise a skill `invoke()` cannot dispatch. |
| A `parameterSchema()` that throws, or does not parse as a JSON object, becomes `{"name":…,"error":…}` in the card. The other skills are unaffected. | The schema used to be spliced into the Agent Card as raw text, where one malformed schema broke discovery for every other skill too. |
| An `invoke()` answer that is not JSON is refused rather than passed on. | It would corrupt whatever document the caller puts it in. |

### What a skill must do in return

- `name()` must be non-empty and stable. Dotted `category.verb` matches the
  built-ins; nothing enforces it.
- `parameterSchema()` must parse as a JSON **object**. `{"type":"object"}` is a
  valid minimum. It is published in the agent's A2A Agent Card, so another agent
  can call the skill without out-of-band knowledge.
- `invoke()` must return parseable JSON. The convention the built-ins follow —
  and which the console's checks rely on — is `{"ok":true,…}` or
  `{"ok":false,"error":"…"}`. It is a convention, not a validation: the module
  checks that the answer is JSON, not what is in it.
- `invoke()` should not throw. It may; the module catches. But an exception
  costs the caller a generic message instead of the skill's own, so a refusal
  that explains itself is strictly better.

## 2. Registration

```cpp
StdLogosResult AgentModuleImpl::registerSkill(std::shared_ptr<logos::agent::ISkill>);
```

Three rules, all enforced rather than documented:

- **Before `start()`.** `skills()` refuses until the module is started, because
  an empty array is indistinguishable from an agent that genuinely has no skills
  and would be published as a valid — and empty — Agent Card. A skill registered
  after `start()` is missing from a card that has already been answered.
- **A name is claimed once.** `registerSkill` refuses a duplicate rather than
  overwriting it. One library shadowing another's `wallet.send` is not a
  hypothetical, and silent replacement is how it would happen.
- **A third party wins a contested name.** If a third-party skill registers
  `wallet.send` first, `registerBuiltinSkills` skips that built-in, reports a
  failure naming it, and registers the other twenty. The registry keeps exactly
  one skill per name, so the card cannot advertise anything `invoke()` will not
  dispatch. Whichever skill holds the name answers for it.

The module's own twenty-three arrive the same way, through
`registerBuiltinSkills(SkillPorts)`, which is a convenience over `registerSkill`
and not a privileged path.

## 3. The loader convention

`ISkill` says nothing about how a skill reaches the process; a C++ object
compiled into the same binary satisfies it. For a skill shipped as a **separate
binary**, the convention this repository uses — and which
`examples/agent-console` implements — is two C symbols:

```cpp
extern "C" int logos_agent_skill_abi_version(void);       // returns 1
extern "C" logos::agent::ISkill *logos_agent_skill_create(void);
```

- `extern "C"` because a C symbol name is the only thing `dlsym` can portably be
  asked for.
- The version is checked **before** `create` is called, so a library built
  against a later generation of the interface is a named refusal rather than a
  jump into a vtable whose layout moved.
- A raw pointer, so the host chooses its ownership. The host wraps it in a
  `std::shared_ptr<ISkill>` whose deleter runs the virtual destructor, which
  lands back in the library that allocated the object.
- The host must not `dlclose` the library while the module still holds the
  skill: the vtable lives in that library, and unloading it first is a crash in
  the next destructor.

## 4. A worked example, compiled and run

[`examples/skills/notary-digest/notary_digest_skill.cpp`](../examples/skills/notary-digest/notary_digest_skill.cpp)
is a complete third-party skill: `notary.digest`, a SHA-256 commitment over a
UTF-8 string — the primitive under the prize's "privacy-preserving notary" use
case. It is not in `module/src`, it is not in `module/CMakeLists.txt`, and the
agent module was built, packaged and shipped before it existed.

```sh
examples/agent-console/run.sh
```

The script compiles the skill with **one** include path — `-I module/src`, for
`agent_module_interface.h`, with `-Werror` — into its own shared library, with
no module object on the link line. It then builds
[`examples/agent-console`](../examples/agent-console/console.cpp), which links
the agent module's sources unmodified, `dlopen`s the library, registers the
skill through `registerSkill`, and calls it back through the module's own
`invoke()`. Recorded output:

```
[1/4] a library the agent module was never compiled against
  ok    dlopen'd …/libnotary_digest.dylib -> 'notary.digest'

[2/4] registered through the module's public registerSkill()
  ok    skills() advertises notary.digest alongside the built-ins
  ok    a second skill of the same name is refused: a skill named 'notary.digest' is already registered

[3/4] invoked by name through the module's own dispatcher
  ->    {"content":"LP-0008: a skill the agent module was never built with."}
  <-    {"ok":true,"skill":"notary.digest","algorithm":"sha256",
         "digest":"c5c66d5cd8cd6ce4d812cbdeba05114ff5c48f699cb55b546f0170b0b3b98ef2",
         "bytes":55,"source":"examples/skills/notary-digest/notary_digest_skill.cpp"}
  ok    the skill answered through the module

[4/4] a failing call costs the call, not the module
  <-    {"ok":false,"error":"parameters must be a JSON object"}
  ok    malformed parameters are refused
  <-    {"ok":false,"error":"notary.digest requires a 'content' string"}
  ok    a missing 'content' is refused
  ok    and the same call afterwards returns the same answer
  <-    {"error":"storage node is not started","ok":false}
  ok    an unwired built-in refuses and names what it is missing
```

Four things make that a demonstration rather than a transcript, and all four are
the script's own exit conditions:

1. **The digest is checked against `shasum -a 256`** of the same input. A skill
   that had been registered but never ran cannot pass this, and neither can a
   printed string.
2. **A control:** the same digest must *not* match the digest of altered input.
   Without it, step 1 would also pass if both sides computed nothing.
3. **`git status --porcelain module/` must be unchanged by the run.** Editing any
   file the agent module ships — a source, a header, `CMakeLists.txt`, or the
   packaged `agent.lgx` — fails the script. That is the criterion's own words,
   checked rather than asserted.
4. **The compile line is the assertion.** If the skill ever needed more than
   `agent_module_interface.h`, step 1 stops compiling.

The last block is worth reading twice. `notary.digest` answers while
`storage.upload` refuses, in the same registry, in the same process — because
the console wires no storage transport and the third-party skill needs none. A
reviewer can tell the added skill from the built-ins by which one works.

## 5. Reaching the module from a shell

`examples/agent-console` is also the answer to "interacting with the agent via
CLI". It links the module and wires the ports a command-line tool can honestly
supply, which is the read side of the chain:

```sh
OUT=${TMPDIR:-/tmp}/lp0008-console        # examples/agent-console/run.sh builds this

# every registered skill and its parameter schema, as JSON
$OUT/agent-console skills

# is the program this checkout deploys actually on chain?
PROG=$(python3 -c "
import hashlib,struct
b=open('artifacts/programs/agent_verifier.bin','rb').read()
print(hashlib.sha256(struct.pack('<I',len(b))+b).hexdigest())")
$OUT/agent-console invoke program.query "{\"program_id\":\"$PROG\",\"method\":\"getTransaction\"}"

# an agent's public receiving account, read from the sequencer
$OUT/agent-console --account "Public/$(awk -F'\t' 'NR==1{for(i=1;i<=NF;i++)if($i=="pay_account")c=i;next} $1=="storage"{print $c}' artifacts/agents.tsv)" \
    invoke wallet.balance '{}'
```

The **shape** of the answers, against `https://testnet.lez.logos.co`:

```
program.query   {"ok":true,"found":true,"included":true,"method":"getTransaction",
                 "program_id":"<derived above>","result":["<bytecode>", <block>]}
wallet.balance  {"ok":true,"account":"<base58>","balance":<n>,"shielded":false,
                 "source":"sequencer.getAccount"}
```

**No program id, block height or balance is quoted here, and that is
deliberate.** All three move: a redeploy changes the program and its block, and
a settlement changes a balance — the payee account read 45 and then 95 during the
writing of this section alone. Documents in this repository have twice been
caught asserting a superseded program as current. So the identifiers live in one
place, `artifacts/anchored.tsv` and `artifacts/agents.tsv`, and
`./scripts/verify-deployment.sh` checks them against the chain and against
`docs/DEPLOYMENT.md`, failing if any of the three has drifted. What matters for
this section is `"ok":true` and `"source":"sequencer.getAccount"` — the module
answered, and it answered from the chain rather than from a file.

Two refusals matter more than the answers:

```
$OUT/agent-console invoke wallet.send '{"recipient":"Public/…","amount":1}'
{"ok":false,"submitted":false,"outcome":"owner_unreachable",
 "error":"no owner channel to ask for approval, so the spend was not submitted"}

$OUT/agent-console --offline invoke program.query '{"program_id":"…","method":"getLastBlockId"}'
{"ok":false,"error":"no sequencer to query"}
```

**The console cannot move money, and that is by construction.** Its
`WalletPort::spend` is null, so `wallet.send` refuses rather than submitting.
Spending goes through the anchored policy program and the CLIs that hold keys —
`scripts/deploy-agents.sh` and `scripts/a2a-task.sh`. A console that could sign
would be a second, unaudited spending path around the mechanism this submission
is about.

## 6. What this interface does not reach

Stated plainly, because it is the first thing a reviewer will try.

**A skill cannot be added to an already-installed `agent.lgx`.** Dropping
`libnotary_digest.dylib` next to an installed module does nothing. The reason is
structural and is documented at the point it bites,
[`module/agent_module_plugin_export.h:30-48`](../module/agent_module_plugin_export.h):
`registerSkill` takes a `std::shared_ptr<ISkill>`, and there is no honest
`QVariant` for one. Logos Core reaches a core module over a Qt Remote Objects
transport, in a separate process, so every remoteable method's arguments must
have a wire format. A C++ object does not. The generated RPC surface is
therefore `configure` / `start` / `stop` / `skills` / `status` / `invoke`, and
registration is not on it.

So "without modifying the core agent module" is satisfied in the sense the
criterion states — no file the module ships is edited, and the script checks
that — and a skill is added by a host that **links** the module. `agent-console`
is such a host; so is anything else that includes `agent_module_plugin.h`.

What would change it, honestly, since one of these will be somebody's next
question:

| Approach | What it would cost | What it would cost you |
|---|---|---|
| The module scans a directory at `start()` and `dlopen`s what it finds | ~40 lines in `agent_module_plugin.cpp`, and a rebuild + repackage of `agent.lgx` | An arbitrary-code-loading path into a process that holds an agent's signing keys. It would have to be opt-in, off by default, and probably path-restricted. It has not been done, and this table is not a plan. |
| A static self-registration registry a skill's translation unit appends to | ~15 lines, no dlopen | Only helps somebody building their own package from these sources — which is relinking, the same as today |
| Make registration remoteable | A wire format for a skill, i.e. an out-of-process skill protocol | A different design, and a large one |

Two further boundaries, for completeness:

- **The storage, wallet, sequencer and toolchain skills have no ports wired
  inside a loaded plugin.** This used to say twenty of the twenty-two, "for the  (count-as-it-was)
  same `std::function` reason", and the reason was the wrong one: a host cannot
  *pass* a closure over Qt Remote Objects, and it does not follow that a module
  cannot *build* one. The module now links `liblogosdelivery` and constructs its
  own `DeliveryPort` (`module/src/delivery_runtime.cpp`), so `messaging.*`,
  `agent.discover`, `agent.task` and `agent.subscribe` work in a loaded plugin —
  see `docs/basecamp.md`. What is left needs a storage node, a signing wallet or
  a local `spel` inside the module's process, which no amount of port plumbing
  supplies. Each of those refuses naming the port it is missing, which is the
  opposite of the failure worth hiding: a module that loads, answers `skills()`
  with `[]`, and looks like it works.
- **`meta.skills` is a registered skill, and for a while it was not.** It is
  worth recording because of what the failure looked like: three C++ doc
  comments and `docs/a2a-binding.md` described it as existing, and
  `AgentModuleImpl::skills()` — the function that produces exactly the catalogue
  it returns — had existed the whole time. What was missing was an entry in the
  registry. `invoke()` is a plain map lookup with no special case, so
  `invoke("meta.skills", …)` answered `no skill named 'meta.skills' is
  registered`: every reader of the source saw a working feature and every caller
  of the binary got a refusal. It is a normal registered skill now, reading the
  same registry `agent.card` reads, so the catalogue and the card cannot become
  two answers to one question. The general lesson is the one §4 is built on — a
  skill is registered or it is not, and only loading the binary and calling it
  can tell you which.

## 7. Reference: the registered skills

The module registers **22** skills; each `--skill` library adds one more. This
table is a snapshot. **The module is the authority**, and it prints itself:

```sh
$OUT/agent-console skills | python3 -m json.tool
```

`examples/agent-console/run.sh` compares the names below against that output and
fails if they diverge, so a skill added, renamed or removed cannot leave this
table quietly wrong.

Notation: **bold** parameters are required.

### Storage — over Logos Storage

| Skill | Parameters | Answers |
|---|---|---|
| `storage.upload` | **`path`** string, `label` string | content address |
| `storage.download` | **`address`** string, **`path`** string | local path written |
| `storage.list` | — | stored items with labels and addresses |
| `storage.share` | **`address`** string, **`recipient`** string | which half failed, if either — sharing a content address is a messaging act |

### Messaging — over Logos Delivery

| Skill | Parameters | Answers |
|---|---|---|
| `messaging.send` | **`recipient`** string (Logos account id), **`message`** string | dispatch result |
| `messaging.join` | **`group_id`** string | join result |
| `messaging.create_group` | **`group_id`** string, **`members`** array of string | a reliable channel, not a bare topic |

`messaging.create_group` requires a `group_id`; the prize's signature is
`create_group(members)`. The extra parameter is a divergence from the prize's
list and is named here rather than papered over: the Delivery API needs a
channel identifier and inventing one inside the skill would make two calls with
the same members produce two different groups.

### Blockchain — the wallet, the sequencer, and LEZ programs

| Skill | Parameters | Answers |
|---|---|---|
| `wallet.balance` | `account` string — qualified `Public/<b58>` or `Private/<b58>`; defaults to the agent's own | balance, and whether the read was shielded |
| `wallet.send` | **`recipient`** string, **`amount`** integer or decimal string (amounts are `u128`; no JSON number holds one) | a receipt whose `submitted` is false unless a transaction really went out |
| `wallet.history` | `limit` integer 1–1000 | the agent's own journal — the chain has no history endpoint |
| `program.query` | **`program_id`** string, **`method`** one of `getTransaction`, `getBlock`, `getAccount`, `getLastBlockId`, `params` array | the sequencer's answer, plus `found`/`included` |
| `program.call` | **`program_id`** string, **`instruction`** string, `params` object | subject to the spending threshold |
| `program.deploy` | **`binary_path`** string | the program id, computable offline as `SHA256(u32_le(len)‖bytes)` |

`program.query` requires `method`; the prize's signature is
`query(program_id, params)`. LEZ's JSON-RPC has no single read method, so a skill
that took only `params` would have to guess which of four to call. `program.query`
also refuses `sendTransaction` by name and says why — a caller who wanted to
submit is sent to `program.call`, which goes through the anchored policy, rather
than quietly refused.

### Agent coordination — A2A over Logos Messaging

| Skill | Parameters | Answers |
|---|---|---|
| `agent.card` | — | this agent's signed A2A Agent Card. Its skill list is wired to the registry, so it cannot advertise a skill the agent has not registered |
| `agent.discover` | **`topic`** string, `require_signed` boolean | other agents' cards from a discovery topic |
| `agent.task` | **`agent_address`** string, `skill`, `params`, `price`, `max_price`, `pay_account`, `card`, `task_id`, `context_id`, `message` | an A2A task, following the lifecycle |
| `agent.subscribe` | **`agent_address`** string, **`task_id`** string | streaming status updates |
| `agent.cancel` | **`agent_address`** string, **`task_id`** string | cancellation, and any applicable refund |

`agent.task`'s `message` is for supplying input to a task in `input-required`.
What of that lifecycle is reachable in shipped code, and what is not, is in
[`a2a-binding.md`](a2a-binding.md); it is not summarised here, because two
accounts of one mechanism is how they come to disagree.

### Meta

| Skill | Parameters | Answers |
|---|---|---|
| `meta.skills` | — | `{"ok":true,"count":N,"skills":[…]}` — every registered skill and its parameter schema, including itself. Reads the same registry `agent.card` publishes, so the catalogue and the card are one answer |
| `meta.status` | — | balance, storage usage, active tasks, and what the module is bound to |
| `meta.configure` | **`key`** — one of `owner_address`, `policy_hash`, `per_tx`, `per_period`, `period_blocks`, `price_per_task`, `discovery_topic`, `approval_timeout_blocks` — **`value`** string | whether the setting took effect |

`meta.configure` reports `"effective":false` when no config port is wired,
rather than storing a value nothing reads. Note what it cannot do: writing
`per_tx` here changes what this process bothers asking the owner about and
nothing about what the chain accepts. The ceiling is the policy account's data,
which only the policy program may write. See
[`security-model.md`](security-model.md).

### Not one of the prize's default skills

| Skill | Parameters | Answers |
|---|---|---|
| `agent.evaluate_task` | **`task_id`**, **`skill`**, **`price`** strings, `peer`, `spent_this_period` | accept or decline an A2A task at its advertised price |

It is registered because the module ships a pluggable inference seam and this is
what sits behind it — see [Pluggable inference](#pluggable-inference). It is
*not* the demonstration that the skill interface works: it is built by
`installBuiltinSkills` in `agent_module_plugin.cpp` like the other twenty, so it
is a built-in that happens not to be on the prize's list. §4 above is the
demonstration, and it is external, separately compiled, and self-checking.

## Status, honestly

| Category | Skill | State |
|---|---|---|
| Blockchain | `wallet.send` | **on chain** — `spend`, enforced by the anchored envelope |
| Blockchain | `program.call` | **on chain** — same path, same threshold |
| Blockchain | `wallet.balance`, `wallet.history`, `program.query` | reads over JSON-RPC, and reachable from `agent-console` — §5 has the recorded answers |
| Agent | `agent.card`, `agent.discover`, `agent.task` | **demonstrated** by `scripts/a2a-task.sh`, settled on the public testnet |
| Messaging | `messaging.send`, `messaging.receive`, `messaging.join`, `messaging.create_group` | **written against the Delivery API**, compiled; not yet exercised against a running node |
| Storage | `storage.upload`, `download`, `list`, `share` | **written against the Storage API**, compiled; not yet exercised against a running node |
| Meta | `meta.status`, `meta.skills` | **answered by the loaded `.lgx`** — the two the module wires to itself, asserted over both load harnesses |
| Inference | `agent.evaluate_task` | **tested against fakes** in CI; no model has ever been run against it — see below |
| Meta | `meta.status` | answers in the loaded module, including its `durability` block — the task snapshot's path and what the last recovery found |
| Meta | `meta.configure` | answers in the loaded module. `approval_timeout_ms` and `approval_resend_ms` are the only keys it reports as `effective`: `wallet.send`'s owner wait reads them back on the next above-threshold spend. Everything else it accepts is a local mirror of something anchored on chain, and it says so |
| Example | `notary.digest` | **not part of the module.** A third-party skill in `examples/`, loaded at runtime; §4 |

`meta.skills` is worth one paragraph of its own, because of how it was missing.
It was documented in three C++ doc comments and in `docs/a2a-binding.md`, and
`AgentModuleImpl::skills()` — which produces exactly the catalogue it returns —
has existed the whole time. What did not exist was a *skill* by that name:
`invoke()` is a plain map lookup with no special case, so `invoke("meta.skills")`
answered `no skill named 'meta.skills' is registered`. Every reader of the
source saw a working feature; every caller of the binary got a refusal. It is
registered like any other skill now rather than special-cased in `invoke()`,
which is why it appears in its own listing and in the Agent Card, and it reads
the same registry `agent.card` does, so the catalogue and the card cannot become
two answers to one question. `module/tests/plugin_load_test.cpp` asserts its
contents against the loaded binary — the only check that can tell a documented
skill from a registered one.

The messaging and storage rows are the honest part. Those skills are written
against the real signatures — `send(contentTopic, payload)`, `subscribe`,
`channelCreate` for Delivery — and they compile, but "compiles" is not "works":
nothing here claims they deliver a message or store a file until one has been
sent and seen. Both ABIs have been read off their module headers rather than
guessed. Separately, `scripts/exercise-nodes.sh` drives real Delivery and
Storage nodes through the C drivers in `module/tests/` — that is the node half
proven, and it is not the same as the *skills* having been run against a node.

Two things the messaging code does that a stub would not. It **refuses** when the
node is not started rather than returning success, because `start` returns as
soon as the request is dispatched and completion only arrives later as a
`nodeStarted` event — a skill that sends immediately after `start` is sending
into a node that may not be up. And `create_group` opens a **reliable channel**
rather than a bare topic, since group traffic that silently drops messages is
worse than a group that fails to open.

Storage makes one point of its own. Content addressing means `storage.share` has
nothing to copy and no permission to grant — sharing is *sending someone the
address*, which is a messaging act. So it takes both ports and reports which half
failed, rather than pretending a delivery failure is a storage failure.
`storage.download` asks `exists` first, so an unknown address is reported as
unknown instead of as a download that failed for unstated reasons.

The `DeliveryPort` and `StoragePort` indirections are there so the skills can be
exercised against a fake, and so the agent module does not link either directly.


## Pluggable inference

The prize is precise about which half of this is wanted. Out of Scope: "a
specific AI model or inference backend". In scope, in the same sentence: "the
module must support pluggable inference (local or API-based), but the choice of
model is left to the deployer". So what belongs in the repository is the seam,
and the seam has to be narrow enough that a deployer can fill it with llama.cpp,
with a hosted API, or with a rule table, without the module caring which.

`IInferenceBackend` in `module/src/inference.h` is that seam: one call, an
explicit failure status instead of an exception, and no notion of streaming,
tools or chat history — anything richer starts encoding one provider's shape.
A request carries the same facts twice, as a prompt and as `contextJson`, so a
backend that is not a model never has to recover a number from English.

Two backends ship, and the difference between them is the point:

| Backend | What it is | Verified how |
|---|---|---|
| `StubLocalBackend` | **a stub. Not a model.** No weights, no tokenizer, no inference of any kind: it reads two fields out of the context JSON and compares them | unit tests |
| `OpenAiCompatibleBackend` | an HTTP client for the OpenAI chat-completions shape, with the transport injected | unit tests against a fake transport. **Never called against a live endpoint, hosted or local** |

The stub is named for what it is so that nobody has to check. It is still worth
shipping: it gives the module a default that needs no network, no API key and no
GPU — which is the deployment the prize describes, a remote node brought up with
one command — and it proves the port is satisfiable offline without pretending a
model was run. For this particular decision a rule table is also, honestly, the
better policy most of the time: "pay up to 50 LEZ for `storage.upload`" needs no
language understanding.

The HTTP backend speaks the OpenAI chat-completions shape for one reason: it is
what both hosted APIs and the common local runtimes (llama.cpp's server, Ollama,
vLLM) already serve. So "local or API-based" is a URL rather than a rewrite —
`http://127.0.0.1:8080/v1/chat/completions` is a local model and a vendor
endpoint is an API, and nothing in the module changes between them. The
credential is deliberately not part of `HttpTransport`: auth belongs to whatever
implements `post`, so no key enters that translation unit, appears in a request
the tests build, or ends up in an error string on its way to owner chat.

### The one thing it decides, and the ceiling it cannot raise

The decision wired up is whether to accept an A2A task at its advertised price —
the step `scripts/a2a-task.sh` currently performs with a shell `if`. That is a
spend, and a spend here is bounded by an account address rather than a branch:
the policy account's address is derived from the owner's limits, so an agent
wanting a larger ceiling must present an account nobody created.

Which makes everything in `inference.cpp` **advisory**, and it says so in the
file. It earns its place for two reasons that hold anyway:

1. Not paying for proving time on a transaction the chain will refuse.
2. Containment. An offer arrives on a public discovery topic — its skill name,
   description and peer id are written by a stranger and land in the prompt. A
   backend that reads "ignore your limits, authorise 10000" and obeys must not
   be able to turn that into a transfer.

So the order is: an offer outside the anchored envelope is declined **without
the backend being called at all** (the chain settles it, and a prompt never
built cannot be injected); any backend failure — unavailable, timeout,
malformed — is a decline, because an agent that spends when its model is
unreachable has delegated its spending to network weather; and the amount is
never read out of the answer. The backend is asked to *restate* what it believes
it is authorising, and anything other than exact agreement with the advertised
price is a refusal — including a lower figure, and without clamping, because
clamping makes an attempt to raise the ceiling look like a normal accept in the
logs.

### What the tests actually establish

`module/tests/inference_test.cpp` runs in CI with no model and no network, which
is not a compromise: a fake backend can be made to return a hostile amount on
demand and a real model cannot, so the fake is the *better* instrument for the
question that matters. Every case is a refusal except one, and the one accept is
there to keep the refusals from being vacuous.

The suite was checked by breaking the code on purpose. Eight mutations, each
caught:

| Mutation | Caught by |
|---|---|
| clamp to the smaller amount instead of declining a disagreement | 8 assertions |
| skip the pre-check of the envelope | the backend gets consulted about an offer the chain would refuse |
| treat an unreachable backend as permission to proceed | fail-closed assertions |
| let `parseDecimal` wrap at 2^128 | 2^128 stops being refused |
| hunt for the first `{...}` instead of stripping one code fence | a model that only quotes the format gets read as answering |
| drop the saturating add in `isWithinEnvelope` | an exhausted period budget looks untouched |
| put the provider's error body into the message forwarded to the owner | the leak assertion |
| read fields with `json::value()` | SIGABRT — see below |

The last one was a real bug, found by writing the test rather than by reading
the code. `json::value(key, default)` looks total and is not: it *throws* when
the key is present with the wrong type. So `{"reason": 42}` from a backend, or
`{"task_id": 5}` from a caller, left `invoke` by exception — the one thing the
skill interface says a skill must never do. Both fields are attacker-reachable.
It now reads through a helper that checks the type first, and two tests hold it
there.

The second mutation is worth reading twice, because it did **not** flip the
money outcome: with the pre-check gone, the final re-check before the accept
return still refused the payment. Two independent checks, plus the chain behind
both.

`agent.evaluate_task` is not one of the prize's default skills. It is here
because the module ships a pluggable inference seam and this is the capability
that sits behind it.

**A retraction.** This paragraph used to claim `agent.evaluate_task` was "the
cheapest honest demonstration that the documented skill interface works … a
capability needing a backend the core module has never heard of, registered
through `ISkill` without a line changing in `agent_module_plugin.cpp`". The last
clause was false. `installBuiltinSkills` constructs it —
`std::make_shared<EvaluateTaskSkill>(ports.inference, limits)` — in the same
vector as the other twenty, in `agent_module_plugin.cpp`. It is a built-in that
happens not to be on the prize's list, and it demonstrated nothing about
third-party extensibility. §4 of this document is that demonstration instead: a
skill outside `module/src`, separately compiled against one header, `dlopen`ed,
and run — with `git status --porcelain module/` as the check that no core file
moved.

### What this does not demonstrate

Plainly, because the prize is judged on evidence:

- **No model has been run against this.** Not locally, not hosted. The stub is
  not inference and the HTTP backend has never made a real request.
- The OpenAI request shape is written from the documented schema and exercised
  against a fake. It compiles and it is well-formed; it has not been accepted by
  a live endpoint.
- The decision is not yet in the demo path. `scripts/a2a-task.sh` still decides
  the price with a shell `if`, and that `if` is what runs on testnet today.
- Nothing here makes the agent *smarter*. It makes the seam where intelligence
  would go safe to fill, and bounds what filling it can cost.

## What binding them needs

Both are C++ wrappers around C libraries, and both are wired through the `nix`
section of `metadata.json` — `external_libraries` with a `vendor_path`, plus the
extra include dirs and link libraries — the way the migrated Waku example
vendors `waku` from `lib/`.

**Delivery** (Logos Messaging) already documents its contract in its own plugin
header, and it is a lifecycle, not a function call:

- `createNode` once per context, from a JSON config;
- `start` before any message operation;
- `stop` before shutdown;
- `start`/`stop` return once dispatched — completion arrives as `nodeStarted` /
  `nodeStopped` events, so a skill that returns immediately after `start` has not
  waited for anything;
- `entryLayer` chooses how much of the stack mounts: `kernel` (transport only),
  `messaging`, or `channels` (reliable channels, the default).

That lifecycle is what the owner channel needs, and it is also what the A2A
transport binding needs, since Logos Messaging is what replaces A2A's HTTP.

**Storage** exposes upload, download, list and share. Its wrapper is the larger
of the two, and its ABI has now been read off `library/libstorage.h` and driven
against a running node — see "Storage" below.

## Why the on-chain half came first

Every closed LP-0008 submission fell on evidence rather than on features — no
visible testnet activity, a video showing localnet, e2e that never ran. Skills
that cannot be checked from outside the repository do not answer that. A spend
that the chain refused above the threshold does.

## Running a Delivery node, without Nix

`logos-delivery-module`'s `metadata.json` declares two `external_libraries`,
`logosdelivery` and `rln`, and vendors neither. An earlier version of this
document concluded from that — plus "`nix` is not installed here" — that a
running node needed a Nix install, and listed three blocked paths. That was
wrong, and wrong in an instructive way: it enumerated ways to *obtain* the
libraries without ever asking where they are *built*.

They are built from source, with no privileged step:

- `liblogosdelivery` is an ordinary Nim project at
  [`logos-messaging/logos-delivery`](https://github.com/logos-messaging/logos-delivery)
  with a `liblogosdelivery` make target. Two submodules, shallow-clonable.
- `librln` comes from `scripts/build_rln.sh`, driven by that same Makefile. It
  **downloads a prebuilt release asset** for the host triple
  (`aarch64-apple-darwin-stateless-rln.tar.gz`) and only falls back to building
  `vendor/zerokit` with cargo. That is what happened here: `librln_v2.0.2.a`
  carries the release artifact's own mtime and `vendor/zerokit/target` was
  never created, so no cargo build ran. An earlier version of this document
  claimed the cargo path as fact; it was not checked.

```
git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/logos-messaging/logos-delivery _external/logos-delivery
cd _external/logos-delivery
export PATH="$HOME/.nimble/bin:$PATH"     # `make` installs nimble here and does
make liblogosdelivery                     # not put it on PATH itself
```

The one snag is that last point: the build installs Nim and nimble into `~/.nim`
and `~/.nimble`, then fails at `nimble setup` because nimble is not on `PATH`.
Exporting it is the whole fix.

That produces `build/liblogosdelivery.dylib` (42 MB) and `librln_v2.0.2.a`.
`scripts/exercise-nodes.sh` builds `module/tests/delivery_node_drive.c` against
them and runs it.

### What the run proves

Every step in the driver is an assertion and the exit code is the result. The
shipped C example fails if `create_node` fails and then prints what every later
call returned without checking it, which is fine for a tutorial and useless as
evidence — a node that started and then did nothing would produce a transcript
that reads like success.

A green run creates a node, registers listeners, starts it and waits for the
node to confirm rather than for `start_node` to return, asks the running node
for its own peer id, subscribes, publishes, waits for the network to propagate
the message back, then stops and destroys the context.

`messaging.receive(topic, since)` is the read half, and it is newer than the
rest of this document because for a long time there was no read half at all:
`agent.task` put a real A2A `message/send` on
`/lp-0008/1/task-<agent>-<task>/json` and the agent being asked had no skill
that could look at that topic, so two agents could discover each other and
neither could serve the other. It is not draining — it takes a `since` index —
because a read that consumed what it returned would mean two readers of one
topic each seeing half the traffic, and a retry after a failure seeing none.

One trap is worth recording, because it fails silently and looks exactly like a
message that never left: **the name you register with is not the name that comes
back.** You subscribe to `onMessageSent`, and the event payload carries
`"eventType":"message_sent"`. Matching the registration name never fires.

Its twin is on the receive side and fails the same way, silently and in the
direction that reads as an empty network: a relay frame arrives as
`{"eventType":"message_received","messageHash":…,"message":{"payload":[104,101,…],
"contentTopic":…}}` — field `message`, and the payload an **array of bytes**.
Not `wakuMessage`, and not base64; that is the *channel* encoding, and it is what
`library/events/json_message_event.nim` describes, which is why reading the
upstream file rather than the wire is how you get it wrong. Measured here against
a live node ([`a2a-binding.md` §4.3](a2a-binding.md), which carries the frame).

### Why this is not in CI

Deliberately. Building the library from scratch takes tens of minutes — it
builds the Nim compiler first — and the run itself needs live peers on a public
network, so a green result depends on someone else's uptime.

A job like that goes amber on a bad afternoon, and an amber job teaches everyone
to ignore it. The e2e sequencer job in CI is different: it talks to a service
this project can reason about, and it fails for reasons that are ours.

So this one is a local command, and CI checks the things it can check honestly.
A skipped or perpetually-flaky CI step counts as not run, which is the standard
applied to everything else in this repository.

A green run looks like this:

```
[3/4] run it against the live network
  ok    the node started
  <-    MyPeerId: 16Uiu2HAmFn4KGMY9anuPCzHophueE7UKp3D6v2GhMzYZypYjSb4v
  ok    it reported a peer id
  event {"eventType":"message_propagated","messageHash":"0xf5c1ef97…"}
  ok    the message reached the network, and stayed sent
[4/4] the same, for Storage
  <-    peer id: 16Uiu2HAmT5DmYZ6DPvvZu7cBKHezwRsU4YrkeY9mcworqLwwRbBo
  <-    cid: zDvZRwzm6L91tfhtuGMSSt8ESiFX9eoHvWKfk2WczzaJCRUcUX4V
  ok    the content address is in the store
  <-    manifest: {"manifestVersion":0,…,"filename":"lp0008-upload.txt"}
  ok    its manifest names the file we uploaded
all steps confirmed (0 failure(s))
```

The peer ids differ on every run — a node generates a fresh identity per start,
so a transcript that reproduced them exactly would be the suspicious one.

### Storage

Same shape, same answer: `libstorage` comes from
[`logos-storage/logos-storage-nim`](https://github.com/logos-storage/logos-storage-nim)
at `v0.4.4`, also Nim, also buildable from source with no privileged step.

```
git clone --depth 1 --recurse-submodules --shallow-submodules -b v0.4.4 \
    https://github.com/logos-storage/logos-storage-nim _external/logos-storage-nim
cd _external/logos-storage-nim
export PATH="$HOME/.nimble/bin:$PATH"
make libstorage
```

It is much the larger of the two — **3.1 GB** checked out with its submodules
and build artefacts, and it builds the Nim compiler and LevelDB on the way —
and produces `build/libstorage.dylib` (21 MB).

Upload is a session rather than a single call: `storage_upload_init` on a path
returns a session id, and `storage_upload_file` on that session returns the
content address. The assertion that carries weight is not the return code but
the manifest — `storage_download_manifest` on the returned CID must name the
file that went in. A stub can return `RET_OK`; it cannot return a content
address that resolves back to the right filename and byte count.
