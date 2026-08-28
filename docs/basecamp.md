# Reaching the agent from the Logos app

The prize asks for an owner-facing interface "accessible from the Logos app
(Basecamp) via the owner channel — local build instructions and loadable assets
are provided". This is the build instructions and the loadable asset, and the
record of what was actually checked rather than assumed.

## What was verified, and how

Shipping a package is not the same as it loading. Basecamp gives no visible
error when a module fails: nothing appears, and the reason only reaches stderr.
So there are two harnesses in `module/tests/`, both of which are assertions with
the exit code as the result, and both of which are run against the artefact this
repository ships — not against a rebuild made for the occasion.

**1. The plugin satisfies the Qt side of the contract**
(`module/tests/plugin_load_test.cpp`). `QPluginLoader` accepts the binary, the
interface IID is `org.logos.LogosProviderPlugin`, the embedded manifest is this
module's own `metadata.json`, `main` names the file that was actually built, and
`qobject_cast` to both `PluginInterface` and `LogosProviderPlugin` succeeds
across the plugin boundary. Then it calls the module through the published
method table and asserts on the module's real behaviour:

```
  ok    the interface IID is the one Logos Core casts against
  <-    name: agent, main: agent_plugin, type: core
  ok    `main` (agent_plugin) names the file that was built (agent_plugin)
  ok    it casts to PluginInterface across the boundary
  ok    it casts to LogosProviderPlugin across the boundary
  <-    getMethods(): status, stop, configure, approveSpend, start, invoke, skills
  ok    before configure it reports itself unconfigured
  ok    configure refuses a malformed policy hash
  ok    a second configure is refused — the binding is the agent's identity
  ok    before start, skills() is an error rather than an empty card
  ok    status reflects the running agent
  <-    skills(): 28 entries: agent.card, messaging.create_group, owner.watch, …
  ok    every skill the module ships with is listed: missing none
  ok    the card has exactly 28 entries, and 28 distinct names
  ok    each carries a parameter schema: all present
  ok    invoke() dispatches to every one of them: undispatched none
  <-    invoke(meta.skills): {"count":28,"ok":true,"skills":[{"name":"agent.cancel", …
  ok    meta.skills lists all 28 skills over the boundary, and counts them
  ok    and every one of them carries the parameter schema skills() published for it
  ok    including itself: it is a registered skill, not a special case in invoke()
  ok    an unwired skill refuses as itself, not as a name nobody registered
  ok    and a name that is not registered is refused as that, without taking the module down
all steps confirmed (0 failure(s))
```

Those three `meta.skills` lines are the ones this harness did not have, and not
having them is how the skill came to be documented in three C++ headers and in
`docs/a2a-binding.md` while `invoke("meta.skills")` answered *no skill named
'meta.skills' is registered*. `AgentModuleImpl::skills()` had always produced
the catalogue, so every reader of the source saw a working feature; `invoke()`
is a plain map lookup, and nothing had ever put that name in the map. A harness
that asks the loaded binary is the only thing that tells those two apart.

That run is against the `agent_plugin.dylib` unpacked from the committed
`module/agent.lgx`, not against `build-basecamp/`.

**2. Logos Core itself loads it, and the module it loaded offers its skills**
(`module/tests/logos_core_load_test.cpp`). This one is not a reproduction of
the host: it `dlopen`s the real `liblogos_core.dylib` out of the installed
`LogosBasecamp.app` and drives it through the same C API, in the same order, as
Basecamp's own `app/main.cpp` — `logos_core_init`, both module directories
added, persistence path, access policy, `logos_core_start`, then
`logos_core_load_module("agent", true)`. That last call is exactly what runs
when a user enables a module; Basecamp itself auto-loads only `package_manager`
and `package_downloader`.

It does not stop at "loaded", because loaded was never the claim worth making.
A module that loads and answers `skills()` with `[]` is *worse* than one that
fails to load: an empty Agent Card is a valid Agent Card, so what a reviewer
sees is a module that installed, enabled, and does nothing, with no error
anywhere saying why. So everything after the load goes back into the module
over the runtime's own transport — `LogosAPI("core")` / `LogosAPIClient`, the
same SDK facade `app/main.cpp` constructs, and the only way to reach a core
module, which the runtime runs in its own `logos_host` process:

```
  <-    known modules: package_manager package_downloader capability_module agent
  ok    the runtime discovers the module in the user modules directory
  ok    the installed module names the variant it is
  <-    installed manifest: main[darwin-arm64] = agent_plugin.dylib
  ok    the manifest declares a `main` for darwin-arm64
  ok    `main` (agent_plugin.dylib) names a file that is really in the module directory
  ok    metadata.json's `main` (agent_plugin) agrees with the manifest's (agent_plugin.dylib)
  ok    logos_core_load_module() reports success
  <-    loaded modules: capability_module agent
  ok    the module is in the runtime's loaded set
  ok    the SDK hands out a client for the loaded module
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  ok    skills() answers with a JSON array, not an error object: [{"name":"agent.cancel", …
  <-    skills(): 28 entries
  ok    the loaded module lists all 28 documented skills
  ok    it lists exactly 28 — no more, no fewer (got 28)
  ok    every listed skill carries a parameter schema (28 checked)
  ok    and it answered as a running agent, not as a stopped one
  ok    invoke() dispatches to every one of the 28
  <-    meta.status durability: {"path":".../agent-persistence/agent/a45bddb77136/tasks.json","recovered_active":0,"recovered_tasks":0,"recovery":"absent","recovery_ran":true,"settled_payments":0,"uncertain_payments":0}
  ok    the loaded module reports a durability record, not null: it was given a persistence directory and opened a task snapshot in it
  ok    and the snapshot lives under the persistence base the host set
  ok    recovery ran before the agent started serving, and reported 'absent'
  ok    approval_timeout_ms is settable on the running module, and effective: {"effective":true,"key":"approval_timeout_ms","ok":true,"stored":"1500","value":"1500"}
  ok    approval_resend_ms is settable on the running module, and effective
  <-    wallet.send above threshold: {"amount":"100","answer_path":true,"attempts":8,"delivered":8,"error":"the owner did not answer within 1500ms: 8 notification attempt(s), 8 of which the channel accepted; the spend was not submitted","ok":false,"outcome":"owner_unreachable","submitted":false, …}
  ok    an above-threshold spend nobody approved is not submitted by the loaded module
  ok    and the outcome is the terminal owner-unreachable one, not a fallback to acting alone
  ok    the notification was retried before the timeout: 8 attempts
  ok    and the failure is reported against the correlation id the owner was asked under
  ok    approveSpend is reachable, and refuses a request nobody is waiting on: no spend is waiting on 'spend-nobody-asked': there is nothing here for this answer to release
  ok    a name nobody registered is refused as unregistered: {"error":"no skill named 'wallet.definitely_not' is registered","ok":false}
all steps confirmed (0 failure(s))
```

Two of those lines are the Reliability criteria, and both were previously
demonstrable only against classes the shipped plugin never constructed:

- `meta.status` reports a **durability** record, which means the host really did
  hand this module a per-instance directory and the module really did open a
  task snapshot under it. The negative control is worth running: put a
  half-written file at that path and the same harness reports
  `FAIL start() is accepted across the transport: pending task state could not
  be recovered from …: the snapshot … is truncated or corrupt. Refusing to
  start with an empty task list on top of a snapshot that could not be read`,
  followed by `skills(): 0 entries` — the agent serves nothing rather than
  coming up believing it owes nobody anything. Executed; exit 1, 13 failures.
- `wallet.send` above the envelope was published to the owner **eight times over
  1500 ms** and then reported as unreachable with nothing submitted. The
  runtime's own log carries the eight `emitEvent: "ownerApprovalRequested"`
  lines between the call and the answer.

The runtime's own log during that run prints `Module loaded: agent` — the same
line it prints for `capability_module` — then spawns `logos_host` for it,
publishes `local:logos_agent_<id>`, and the capability module issues a token for
`agent`. So the module was published over the transport and registered with the
capability system, not merely dlopened, and the `skills()` above came back
across that transport from the separate process the runtime started.

Two details make those last checks mean what they say rather than pass for
free:

- **The dispatch check has a control.** Called with `{}`, most skills refuse,
  because a module loaded as a *plugin* has no way to receive a `std::function`
  port across the boundary. That refusal is the skill's own —
  `{"error":"no account to read: …"}` — and is the proof the call arrived. Only
  the registry's `no skill named '…' is registered` means it did not. The
  harness asserts both directions: no listed skill may produce the registry's
  refusal, and a name nobody registered must.
- **It fails on the artefact this repository shipped in `726e142`.** Run against
  that `agent.lgx` — packaged before the skills were registered, and otherwise
  identical in manifest, type and load behaviour — the same harness reports
  `skills(): 0 entries`, `[]`, and three failures, while still passing every
  check up to and including "the module is in the runtime's loaded set". The
  checks are not describing the code; they discriminate between two builds of
  it, and they discriminate exactly where the difference is.

Environment for run 2: **LogosBasecamp 0.2.2**, official macOS arm64 `.dmg`,
`/Applications/LogosBasecamp.app`, portable build, bundling **Qt 6.9.2**. The
package under test was the committed `module/agent.lgx`, unpacked into the user
modules directory by the procedure below — not the build tree.

## The ports a loaded module builds for itself

For most of this repository's life the sentence above — "a port is a
`std::function` and there is no wire format for one" — stood as the reason a
loaded module could do nothing on the network. It is a true sentence about what
a HOST can PASS. It was read as a statement about what a MODULE can HAVE, and
those are different claims: a module that links `liblogosdelivery` can open a
node from its own configuration and construct its own ports on the far side of
the boundary, where nothing has to be serialised because nothing crosses.

What crosses is `invoke("meta.configure", {"key":"delivery","value":"on"})` —
two strings, which Qt Remote Objects has carried since the beginning. Nothing
starts on its own: loading the module joins no network and opens no socket, and
every skill on the wire keeps the refusal it already had (`"delivery node is not
started"`) until an operator asks. `meta.status` reports which of the two
situations a refusal means:

```
  {"linked":true, "state":"off"}       the library is in the binary, no node yet
  {"linked":true, "state":"ready"}     a node is up
  {"linked":false,"state":"absent"}    this build has no Delivery library at all
```

**3. The runtime loads it, and the module opens a Delivery node inside
`logos_host`** (`module/tests/logos_core_delivery_test.cpp`). Harness 2 above
proves the module is reachable; this one proves what it can do once reached. A
core module does not run in the caller's process — Logos Core spawns
`logos_host` for it — so the node below is opened inside a process the harness
never enters, by a plugin it only caused to be `dlopen`ed. Run against the
plugin unpacked from the committed `module/agent.lgx`:

```
  ok    logos_core_load_module() reports success
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  <-    meta.status delivery: {"linked":true,"state":"off"}
  ok    the module the RUNTIME loaded links Logos Delivery into itself
  ok    and has started no node, because nobody has asked it to
  <-    messaging.send: {"error":"delivery node is not started","ok":false}
  ok    the wire skills refuse over the transport, exactly as documented
  <-    meta.configure: {"effective":true,"key":"delivery","ok":true,"stored":"on","value":"on"}
  ok    meta.configure('delivery','on') crosses Qt Remote Objects
  ok    the module opened and started its own Delivery node inside logos_host
  <-    messaging.join: {"ok":true,"topic":"/lp-0008/1/discovery-lp0008corelive/json"}
  <-    messaging.send: {"bytes":7,"ok":true,"topic":"/lp-0008/1/owner-lp0008corelive/json"}
  ok    messaging.send put a message on the public network
  <-    agent.discover: {"agents":[],"ok":true,"rejected":[{"index":0,"reason":"the card is not valid JSON"}],"seen":1,…}
  ok    agent.discover answers with a result, not 'no discovery transport is configured'
  <-    agent.task: {"ok":true,"state":"submitted","task_id":"corelivetask","topic":"/lp-0008/1/task-lp0008corelive-corelivetask/json",…}
  ok    agent.task opened a task and put the A2A request on the wire
  ok    agent.subscribe subscribed to that task's topic
  ok    meta.configure('delivery','off')
  ok    the node is down and the module says so
the module Logos Core loaded obtained a working Delivery port (0 failure(s))
```

`agent.discover`'s `"seen":1` is not decoration: the document it rejected is the
seven bytes `messaging.send` published two lines earlier, which went out to the
public relays and came back into the module's own inbox on that exact content
topic. A card it cannot parse is reported as rejected; a topic nothing arrived
on reports `"seen":0`.

**4. Two loaded modules discover each other's signed Agent Cards**
(`module/tests/plugin_delivery_test.cpp`, `peer` mode; run it with
`./scripts/delivery-in-plugin.sh peers`). Harness 3 cannot be evidence for the
discovery criterion on its own, because a Waku node receives its own published
messages — a single process can satisfy any assertion about "a card arrived"
with every other agent on earth switched off. So this is two processes, each
loading the same plugin, each with its own node, its own working directory and
its own LEZ account, sharing nothing but a content topic derived from one run
id. Each accepts only a card whose `url` names the OTHER account, so neither can
satisfy itself:

```
agent A                                    agent B
  ok  this agent's own Delivery node came up  ok  this agent's own Delivery node came up
  ok  the card names this agent's account     ok  the card names this agent's account
  ok  and carries a signature                 ok  and carries a signature
  ok  published its own signed card           ok  published its own signed card
  ok  discovered the OTHER agent's signed Agent Card over the public network
                                              ok  (the same, in the other direction)
  ok  which is signed — `require_signed` was on
  ok  this agent opened an A2A task addressed to the other one
  ok  and READ the other agent's A2A request off its own task topic
  ok  carrying the context id the other agent minted
  ok  this agent publishes `working` for the task it was asked to do
  ok  signed with its own account, which agent.update reads off this module's
      configuration and not off the call
  ok  then `completed`, which agent.update marks final because the state is
  ok  and it puts a forged `completed` for its OWN task on the topic it reads
  ok  THIS agent's own TaskStore reached `completed`, and every transition into
      it came off the wire
  ok  applying 2 status update(s) the peer published
  ok  all of them published by the OTHER account — none by this one
  ok  while the forged update this agent published about its own task was read
      back off the same topic and refused (1 of them)
  ok  and the walk it records is submitted -> working -> completed
two loaded modules discovered each other and ran a task lifecycle across the
network (0 failure(s))
```

The second half of that is the A2A lifecycle running **between** the two
processes rather than inside either. `agent.update` publishes a
`TaskStatusUpdateEvent` on the task's topic; `agent.poll` reads that topic and
applies what the peer published to this agent's own `TaskStore`, refusing any
frame that does not name the peer as its author. The forged line is why the rest
of it means anything: a node receives what it publishes, so each agent puts the
exact frame a self-satisfying harness would use — `completed`, for its own task,
as itself — onto the topic it is about to read, and asserts the poll counted it
and refused it. "No self-authored update was applied" would also be true if none
had arrived; "one arrived and was refused" is not.

**The negative control for it is a rebuilt module, not an argument.** Delete the
author rule from `PollSkill` — `if (author != task.agent)` becomes `if (false)`
— rebuild the plugin, and run the same two agents: both processes exit 1 on
exactly the forged-update line, `refused (0 of them)`, and on nothing else. The
run is worth reading for what still passed on that build. The peer's updates
arrived before the forged one, so the state machine refused it as a second
`completed` and it never reached `applied[]` — meaning the weaker assertion, "no
self-authored update was applied", was **true of a module with no author check
in it**. The count is the assertion that discriminates.

The cards are real: `agent.card` is assembled by the loaded module out of its
own registry — so it cannot advertise a skill the agent has not registered — and
signed BIP-340 over secp256k1 by the LEZ account key, through the `card_signer`
command `meta.configure` names. `CardPort::sign` has always declared exactly
that contract ("given `<protected>.<payload>`, return the base64url signature"),
and the curve arithmetic stays outside the module deliberately: this plugin
links no crypto library, and a hand-rolled 256-bit field inside the binary that
signs payment instructions is the last place to put one.

**5. A loaded module pays for the task it was served**
(`module/tests/plugin_delivery_test.cpp`, `peer` mode with a payment configured;
run it with `./scripts/delivery-in-plugin.sh settle`). Harness 4 ends with two
agents that have found each other, served each other and driven each other's
tasks to `completed`, and no money moving.
That was not a limitation of the plugin boundary either — it was
`TaskPort::pay` left unwired, with a note in `agent_module_plugin.cpp` saying a
settlement needs a wallet and a sequencer "and this module has neither". It is
the same sentence as the one about ports, and it has the same answer: the module
does not need to HAVE a wallet, it needs to be able to REACH one, and
`card_signer` had already shown how.

So `pay_signer` is `card_signer`'s mechanism with a different command behind it —
literally the same function, `AgentModuleImpl::runConfiguredCommand`, of which
the card signer is now one of three callers. `policy_source` is the third, and it
is what makes the payment *unattended*: it reads the agent's anchored policy
account off the chain, so `agent.task` can see that the price is inside the
envelope its owner anchored and pay it without asking anyone. Both are named in
`meta.configure`, both take their input on stdin, and both have their answer
checked character by character before it is believed — 64 lower-case hex for a
settlement, decimal digits for a limit.

The buyer is handed no price and no payee. It reads both off the seller's signed
card, which arrived over Waku a few seconds earlier:

```
buyer                                       seller
  ok  discovered the OTHER agent's signed Agent Card over the public network
  ok  the discovered card advertises a price to pay: 1 LEZ
  ok  and a public account to pay it into: Public/BzYks91a…
  ok  this agent opened an A2A task addressed to the other one
  <-  agent-spend: resyncing ~/.lp0008-agents/storage
  <-  agent-spend: Synced to block 9470
  <-  agent-spend: 1 LEZ -> Public/BzYks91a…, window 9000, policy 6FscNXjN…
  <-  agent-spend: spel exited 0 after 432 s
  <-  agent-spend: submitted ed8c3514…
  <-  agent-spend: ed8c3514… is in block 9477
  ok  it paid the price the peer's card advertised, 1 LEZ
  ok  and settled it on chain, from inside the loaded module, with no owner in
      the path: ed8c351412409c81723ea7b90e2d9cdcb0841a33234894bfff8269af374b8cb3
                                              ok  the card this agent was handed
                                                  advertises no price, so there
                                                  is nothing to pay
                                              ok  and no settlement hash came
                                                  back for it
  ok  and READ the other agent's A2A request off its own task topic
  ok  carrying the context id the other agent minted
  ok  this agent publishes `working` for the task it was asked to do
  ok  then `completed`, which agent.update marks final because the state is
  ok  and it puts a forged `completed` for its OWN task on the topic it reads
  ok  THIS agent's own TaskStore reached `completed`, and every transition into
      it came off the wire
  ok  applying 2 status update(s) the peer published
  ok  all 2 of them published by A7UBoMbSoQXNaDTiSjbr28KjedNrvBvroiamrc39JtMu,
      the OTHER account — none by this one
  ok  while the forged update this agent published about its own task was read
      back off the same topic and refused (1 of them)
  ok  and the walk it records is submitted -> working -> completed
                                              (and the same on the seller's side)
```

**All three conjuncts, one invocation.** Discovery, the payment, and the
lifecycle running BETWEEN the two processes are the same `SCRIPT_EXIT=0` — 37
checks on the buyer, 34 on the seller, no failures on either. The lifecycle half
is harness 4's, unchanged; what this adds is that it happens on the far side of
a settlement rather than in a run with no money in it.

**The two sides wait for very different things here, and that had to be built
in.** A proof blocks the paying module for as long as it takes — 432 s in this
run — and a module that is blocked publishes nothing. So the buyer unblocks to
find its peer's updates already waiting, while the seller has to outwait a proof
it cannot see: it polls for 480 rounds where the buyer polls for 40, and it knows
to because its own card carries a price. Nothing in it times the peer.

The seller's two lines are the control, and they are the same code path: one
`agent.task` call, one card, and the answer differs only because the card does.
Without them "the module reported a transaction hash" would be indistinguishable
from "the module reports a transaction hash whenever it opens a task".

**And the harness is not the last word either.** `scripts/record-settlement.py`
decodes the payee's balance out of that transaction's own committed post-state —
`getAccount` cannot answer it, because this chain has no historical-state RPC —
and refuses to write anything unless it rose by exactly the price. For this run:
`hash_ok=1`, block 9389, `recipient_balance=2` where it held 1, and the anchored
ledger `6FscNXjN…` at `window_start=9000, spent=2`. The per-period total on chain
moved by the price, which is the part that says the policy program ran rather
than that a transfer happened beside it.

The proof took **418 seconds** and the module blocked for all of them, which is
worth having as a number: it is why `TaskPort::pay` blocks, why the signer
confirms inclusion itself rather than handing back a hash to be checked later,
and why this harness is not in CI. A run the day before took 767 s for the same
instruction with another proof on the machine — always check the wall clock
before reading a duration as a property of the work.

**Settlement 9 in the submission's table is this exact flow, run once before
this one, and it is on the page for a reason that is not redundancy.** That run
paid, and its buyer's task really did walk `submitted → working → completed` on
the seller's frames — and the harness reported failure, because the assertion
counted the updates its poll LOOP applied and the poll that opens the topic had
already applied them both on the far side of the proof. The line beneath it then
printed `ok` for "all of them published by the other account" about an empty
list. `docs/limitations.md` has the full account; the fix was verified against a
signer stub that sleeps rather than proves, which costs nothing and would have
caught it first.

**Settlement 7 is an older instance of the same lesson, one layer down.** That run was
made by a version of `delivery-in-plugin.sh` two lines short of the committed
one — the file was edited while bash was executing it, so the branch that shipped
had never been run start to finish. Both lines were exercised separately and both
were right, and it did not matter: a script whose committed form is not the form
that was verified is exactly the defect this repository keeps finding, and the
only way to close it is to run the committed form. Settlement 8 is that run,
whole, `SCRIPT_EXIT=0` with both processes at 0.

**The refusals are a separate harness, because this one costs money.**
`./scripts/delivery-in-plugin.sh signers` needs no second agent, no key and no
chain, and pins nine decisions: an envelope the module cannot read is *unknown*
and unknown is outside, a price over the anchored limits never reaches the
signer at all, and neither an empty answer nor a diagnostic from the signer
becomes a settlement. Its assertions were watched failing against three mutated
builds — with the hash check removed the module writes `error: this signer holds
no key` into the task record as a settlement, and a module that reads "I do not
know" as "no limit" pays a task nobody configured it for.

**What the module still cannot do here.** `TaskPort::refund` stays unwired: a
refund would have to be signed by the payee, whose key this agent does not hold.
And an above-envelope *task* price is refused immediately with
`owner_unreachable` rather than put to the owner — `wallet.send` has that path
and `agent.task` does not. The reason is specific to the agents in this
repository rather than to the chain: their owners anchored while still
*unclaimed*, so they can never sign `approve_spend` again and for these agents
the wait could not succeed. An owner claimed before it anchors signs
indefinitely, and the approved path has run end to end on the public testnet —
`approve_spend` in block 10776, `spend_approved` in block 10786. Wiring
`agent.task` to it is open work, not an impossibility.

**6. And the OWNER is a loaded module too**
(`module/tests/plugin_delivery_test.cpp`, `owner` mode; run it with
`./scripts/delivery-in-plugin.sh two-modules`). Harness 5's owner is
`module/tests/owner_responder.cpp`, which links the Delivery library directly
and is not a Logos app. This one is the same package loaded twice, driven only
through `invoke()` — the agent through `wallet.send`, the owner through
`owner.watch`, `owner.pending` and `owner.answer` — which is the only thing a
`ui` plugin in a second Basecamp can do, and is therefore the headless form of
§"Two Basecamps" below.

Three runs, and the first is the one that makes the other two mean anything:

```
run 1 — nobody watching
  <-  wallet.send: {"outcome":"owner_unreachable","attempts":6,"submitted":false,
      "waited_ms":45000, …}
  <-  meta.status delivery: {"frames":{"channel_decoded":0,"channel_seen":0,"relay_seen":6}}
  ok  with nobody on the other end, the agent's own node handed its own request back
      and no approval came of it
  ok  its own 6 request(s) came back to it off the network (6 relay frame(s) seen)
      and none of them was an answer

run 2 — the owner's module approves        run 3 — and denies
  ok  owner.watch opened the reliable channel      (the same, then)
  ok  and its id is the one the agent derives from the same two accounts
  ok  an agent asked this owner to approve a spend, within the time allowed
  ok  this app derived the approval marker from the terms and got the seed the
      agent named — two independent derivations
  ok  the owner's approval went out on the channel  ok  … the owner's denial …
  ok  one payment, one answer                      ok  and an id nobody asked
                                                       about is refused
  <-  859 ms                                       <-  670 ms
  ok  and approved these exact terms: approved     ok  the owner's DENIAL came
                                                       back as a denial: denied
```

`SCRIPT_EXIT=0`, three processes' worth of assertions, no failures. The two
apps in §"Two Basecamps" do the same thing with a person clicking.

**The negative control, which is what makes the four transcripts above mean
anything.** Build without `-DLOGOS_DELIVERY_ROOT` — the default — and run the
same harness against the same package layout. It reports:

```
  <-    meta.status delivery: {"error":"this build of the agent module does not link Logos
        Delivery, so no node can be started in it","linked":false,"state":"absent"}
  FAIL  this build links Logos Delivery into the plugin
  FAIL  and has not started a node, because nobody has asked it to
  ok    meta.configure('delivery','on') is accepted
  FAIL  the module's own Delivery node came up and reported it
```

Note which line stays green: `meta.configure('delivery','on')` is *accepted* by
a build that cannot act on it, because the setting is stored either way. A
harness that stopped at "the call succeeded" would have called that a pass —
which is the same shape as the Qt-version failure two sections down, where
`logos_core_load_module` returns success for a plugin the host then cannot use.

### What this needs at build time

The library is not vendored. Point the build at a `logos-delivery` checkout that
has been through `make liblogosdelivery` (see `scripts/exercise-nodes.sh`, which
already builds it):

```sh
cmake -S module -B build-basecamp \
      -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
      -DCMAKE_CXX_FLAGS=-I/opt/homebrew/include \
      -DLOGOS_DELIVERY_ROOT=$PWD/_external/logos-delivery
```

Without that flag the module still builds and still ships `delivery_runtime.cpp`
— every entry point answers "no" and `meta.status` says `absent`. That is a
deliberate difference from leaving the file out: a build that cannot start a
node has to be able to say so, and `"absent"` and `"off"` produce byte-identical
refusals from every skill that touches the wire.

## What was NOT verified

Stated plainly, because a reviewer will check.

- **No click in the Basecamp GUI.** Verification went through Logos Core's C API
  with Basecamp's own shipped library, not through the App Manager. Basecamp
  0.2.2 has no "install from file" button — its Package Manager installs from a
  configured package repository only — so a GUI install of a local `.lgx` is not
  something a reviewer can do either, which is why the module directory is
  populated by hand below.
- ~~**macOS arm64 only.**~~ Closed, and it is worth saying what closed it,
  because the sentence that stood here — "no `linux-amd64` variant is built or
  packaged, and nothing here was run on Linux" — was true and the reason given
  for it elsewhere was not. `docs/limitations.md` said the runtime half could
  not be had because `liblogos_core` ships inside the app and there is no
  headless distribution to fetch. That is true of the macOS `.dmg`. The Linux
  build of the same app is an **AppImage** — a SquashFS image with an ELF
  runtime in front of it — published on the same release page, and it unpacks
  with no installer, no root, no FUSE and no display.
  `scripts/fetch-logos-core.sh` does that, checksum-pinned, and
  `scripts/logos-core-headless.sh` then runs against it with nothing set.
  Both packages now carry `linux-amd64` **and** `linux-arm64`, and the runs
  below are on x86-64 and aarch64 Linux, both green, from the committed
  package. See "Linux" below.
- **The GUI click is now verified; two rows below it are not.** This bullet used
  to read "No click in the Basecamp GUI" and then "No owner-facing UI plugin",
  and both are closed — `app/` is the `ui` plugin, the tile is in Basecamp's
  left rail, and the section "Inside the running app" below is the record of it
  being clicked. What is still true is the sentence those bullets opened with:
  Basecamp 0.2.2 has no "install from file" button — its Package Manager
  installs from a configured package repository only — so **both** packages are
  installed by hand, by the procedures below, and a reviewer cannot do it
  through the GUI either.
- **The sequencer and the local toolchain have no ports wired**, and neither
  does the *reading* half of the wallet. The storage skills are no longer on
  that list: the module opens its own Logos Storage node when
  `meta.configure("storage","on")` asks it to, the same way it opens its own
  Delivery node, and `scripts/skills-live.sh` drives all four through
  `invoke()` from a completely empty `SkillPorts`.
  `invoke("wallet.balance", "{}")` returns `{"ok":false,"error":"no account to
  read: the agent has none configured and none was given"}`, and `meta.status`
  reports `balance: null` with `balance_error` rather than `0`. Those need a
  Logos Storage node and a sequencer client in the module's process, which is a
  different problem from the transport one below and is not solved.

  The *spending* half is a third case and it is now closed, by delegation rather
  than by linking: `agent.task` pays through a `pay_signer` command and reads
  its anchored envelope through a `policy_source` command, the same shape
  `card_signer` has always had. `wallet.send` deliberately does not — its
  envelope is a struct of strings fixed at `start()`, so wiring a spend there
  would let one out under an envelope that was empty when the module came up.
  See §5 above and [`docs/skills.md`](skills.md).
- **The module reaches Logos Delivery and not Logos Storage.** This bullet used
  to say it linked neither, and to give the reason: "a port is a `std::function`
  and there is no wire format for one, so a host that loads this as a plugin
  cannot wire them". The premise is right and the conclusion was wrong — see
  §"The ports a loaded module builds for itself" above.
  `module/src/delivery_runtime.cpp` opens `liblogosdelivery` with `dlopen` at
  the moment a node is asked for, `module/agent.lgx`'s **`darwin-arm64`** variant
  carries the library and its licence beside the plugin — the two Linux variants
  carry the code path and no library, because upstream publishes no
  `liblogosdelivery.so`, so `meta.configure("delivery","on")` there names the
  file it could not open — and the messaging, discovery and task transports
  work in a module Logos Core loaded. `libstorage.h` is still included by
  nothing, so `storage.*` still refuses.
- **`logos_protocol_version`.** The runtime logs
  `Module agent carries no usable logos_protocol_version (pre-protocol build) —
  loading permissively` and loads it. That is a property of the pinned
  `logos-cpp-sdk` (below), which predates the stamp. It loads today; a runtime
  that stops being permissive would reject it.

## Building it, without Nix

The blessed path is `logos-module-builder`, a Nix flake library. Nix is not
needed: the generated `CMakeLists.txt` looks for the helper in
`$LOGOS_MODULE_BUILDER_ROOT` first, so a plain checkout is enough.

### The four checkouts, pinned

Pinned to what has been proven, not to what is newest — following default
branches is what broke earlier attempts, when `logos-qt-sdk` and
`logos-protocol` moved and the builder could no longer find `cpp/logos_api.h`.

| Repository | Revision | Used for |
|---|---|---|
| `logos-co/logos-module-builder` | `5396513` | `cmake/LogosModule.cmake` |
| `logos-co/logos-cpp-sdk` | `c87f343` | SDK headers/sources and `logos-cpp-generator` |
| `logos-co/logos-module` | `1947784` | `src/interface.h` |
| `logos-co/logos-package` | `18b0075` | the `lgx` packager (only for packaging) |

```sh
mkdir -p ~/logos/src && cd ~/logos/src
git clone https://github.com/logos-co/logos-module-builder && \
  git -C logos-module-builder checkout 5396513
git clone https://github.com/logos-co/logos-cpp-sdk && \
  git -C logos-cpp-sdk checkout c87f343
git clone https://github.com/logos-co/logos-module && \
  git -C logos-module checkout 1947784
```

Newer `logos-module-builder` revisions additionally require `logos-qt-sdk` and
`logos-protocol` checkouts and will `FATAL_ERROR` without them. The pinned
revision above needs only the three.

### Qt — the version is a ceiling, not a floor

Qt refuses any plugin whose minor version exceeds the host's. Basecamp 0.2.2
bundles **Qt 6.9.2**, so Homebrew's current Qt (6.11.x) is rejected outright —
and a Homebrew build also hardcodes `/opt/homebrew/opt/qtbase/lib/...` as its
library paths, which resolve on the build machine and nowhere else. An official
Qt references its frameworks as `@rpath/...`, which is what resolves against the
host's bundled copy. `QtRemoteObjects` is required (the module transport) and is
not in the default archive set, so ask for it:

```sh
python3 -m venv ~/logos/aqt && ~/logos/aqt/bin/pip install aqtinstall
~/logos/aqt/bin/python -m aqt install-qt mac desktop 6.9.2 clang_64 \
    -m qtremoteobjects --archives qtbase --outputdir ~/logos/Qt
```

That lands 209 MB in `~/logos/Qt/6.9.2/macos` and takes about six seconds.
`--archives qtbase` is what keeps it that small, and it also avoids an
extraction bug in aqt 3.3.0 that trips over `QtSvg`'s symlinks. On Linux,
`install-qt linux desktop 6.9.2 linux_gcc_64 -m qtremoteobjects --archives
qtbase`; a distribution Qt at or below 6.9 works too (Debian bookworm's 6.4.2
is fine).

**Not `/tmp`.** Earlier revisions of this document said `--outputdir /tmp/Qt`,
and that is how an afternoon was lost. What was found there later was
`/tmp/Qt/6.9.2/macos/lib` holding 63 frameworks and the directory holding
nothing else: no `lib/cmake/`, no `include/`, no `bin/`, no `libexec/`, no
`mkspecs/`, and no `QtRemoteObjects.framework`. The `aqt` virtualenv beside it
had been reduced to a `bin/` with no `lib/`. That is macOS's periodic `/tmp`
cleaner, which deletes files by age and takes out precisely the ones a finished
build stops touching. The result reads as a partially extracted download and is
not one, which is why the symptom is so hard to place.

CMake's behaviour on that directory is the expensive part: it does not fail. It
cannot find `Qt6Config.cmake`, falls through to the system Qt without a word,
and produces a Homebrew-linked 6.11 plugin. Install Qt somewhere durable. The
check is two lines:

```sh
ls ~/logos/Qt/6.9.2/macos/lib/cmake/Qt6/Qt6Config.cmake   # must exist
ls -d ~/logos/Qt/6.9.2/macos/lib/QtRemoteObjects.framework # and this
```

A complete install has `bin include lib libexec mkspecs plugins` and more; one
that has only `lib` is the trap above, and re-running the `aqt install-qt`
command repairs it.

What a 6.11 plugin then does in Basecamp is worth stating exactly, because it
is not "fails to load" and it is the reason `package-basecamp.sh` refuses to
build one. Harness 2, run against a module directory holding the Homebrew
build, reports:

```
  ok    logos_core_load_module() reports success
  ok    the module is in the runtime's loaded set
  FAIL  configure() is accepted across the transport: no LogosResult came back (got nothing)
```

`logos_core_load_module` returns success and the module joins the loaded set.
The truth is 250 ms earlier, in the runtime's log, and nothing surfaces it:

```
[error] [logos] [agent] LogosModule: Failed to load plugin: ".../agent_plugin.dylib"
  Error: "The plugin '.../agent_plugin.dylib' uses incompatible Qt library. (6.11.0) [release]"
```

After that the module's `logos_host` is gone, nothing is published, and every
call spends 20 seconds on `Timeout waiting for replica: "agent"` before giving
back an empty QVariant. A harness that stopped at "loaded" would have called
that a pass.

`nlohmann/json.hpp` is also needed — `brew install nlohmann-json`, or
`apt install nlohmann-json3-dev`.

### Build

```sh
export LOGOS_MODULE_BUILDER_ROOT=$HOME/logos/src/logos-module-builder
export LOGOS_MODULE_ROOT=$HOME/logos/src/logos-module
export LOGOS_CPP_SDK_ROOT=$HOME/logos/src/logos-cpp-sdk

cmake -S module -B build-basecamp \
      -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
      -DCMAKE_CXX_FLAGS=-I/opt/homebrew/include
cmake --build build-basecamp -j8
```

That produces `build-basecamp/modules/agent_plugin.dylib`. Confirm which Qt it
picked up before going further — this is the check that costs nothing and saves
an afternoon:

```sh
otool -L build-basecamp/modules/agent_plugin.dylib | head -4
#   @rpath/QtRemoteObjects.framework/... (current version 6.9.2)
#   @rpath/QtNetwork.framework/...       (current version 6.9.2)
#   @rpath/QtCore.framework/...          (current version 6.9.2)
```

`@rpath` and `6.9.2`. An absolute `/opt/homebrew/...` path or a `6.11.x` means
CMake found the wrong Qt: check `Qt6Core_DIR` in `build-basecamp/CMakeCache.txt`
— it must read `…/logos/Qt/6.9.2/macos/lib/cmake/Qt6Core`, not
`/opt/homebrew/opt/qt/lib/cmake/Qt6Core` — and that `Qt6Config.cmake` exists at
all. An incomplete Qt has the frameworks but not the CMake config, and CMake
then silently falls through to the system Qt. `module/package-basecamp.sh`
refuses to package a plugin that got this wrong, so the mistake cannot reach the
committed artefact, but it is cheaper to catch here.

### Linux, and the two Linux variants

Two things had to be established before any of this could be written down, and
both were assumptions this document previously recorded as facts.

**1. The runtime is obtainable without installing anything.** `liblogos_core`
does ship inside the app on every platform. On Linux the app is an
**AppImage**, and an AppImage is an ELF runtime with a SquashFS filesystem
appended — so "inside the app" and "fetchable" are not opposites there. One
command, checksum-pinned, no installer, no root, no FUSE, no display:

```sh
./scripts/fetch-logos-core.sh
```

It takes `LogosBasecamp-Desktop-v0.2.2-d41a72-x86_64.AppImage` (sha256
`b5dd636f…dd5ae`; `aarch64` is pinned too, at `9fed46b9…87ef2`), refuses to
unpack anything that does not match, and unpacks it two ways: the AppImage's own
`--appimage-extract`, which needs nothing at all, and `unsquashfs -o <offset>`
when the runtime cannot execute — which is the case in a cross-architecture
container. The offset is **where the ELF ends**, `e_shoff + e_shnum *
e_shentsize`; do not search for the `hsqs` magic, because the runtime binary
contains those four bytes 750 KB before the real superblock and `unsquashfs`
then reports `Can't find a valid SQUASHFS superblock`, which reads exactly like
a truncated download.

What comes out is the same tree the macOS app has, under `usr/`:
`usr/lib/liblogos_core.so`, `usr/bin/logos_host`, `usr/modules`,
`usr/lib/qt/plugins`, and Basecamp's own Qt 6.9.2. What does **not** come out is
Qt's SDK — no headers, no `moc` — so `aqtinstall` is still a prerequisite for
building anything.

**2. The Qt to build against, and the two things that bite on Linux only.**

```sh
python3 -m aqt install-qt linux desktop 6.9.2 linux_gcc_64 \
    -m qtremoteobjects --archives qtbase icu --outputdir ~/logos/Qt
# aarch64: the host and the arch both change, and so does the directory
python3 -m aqt install-qt linux_arm64 desktop 6.9.2 linux_gcc_arm64 \
    -m qtremoteobjects --archives qtbase icu --outputdir ~/logos/Qt
```

`icu` is not optional there. Official Qt for Linux links ICU **73** by soname
and no current distribution ships that, so without the archive every link ends
in `undefined reference to ucnv_open_73` — inside the `cpp-generator` sub-build,
which makes it look like a generator problem.

And the distribution floor is set by upstream, not by us: the libraries inside
Basecamp's own AppImage want `GLIBC_2.38`, `CXXABI_1.3.15` and
`GLIBCXX_3.4.32`. On Debian bookworm (glibc 2.36, gcc 12) the harness builds and
then dies with `version GLIBC_2.38 not found (required by liblogos_core.so)`.
Ubuntu 24.04 — which is what `ubuntu-latest` is — is above that line, and any
machine that can run Logos Core on Linux at all already is.

```sh
export LOGOS_MODULE_BUILDER_ROOT=~/logos/src/logos-module-builder
export LOGOS_MODULE_ROOT=~/logos/src/logos-module
export LOGOS_CPP_SDK_ROOT=~/logos/src/logos-cpp-sdk
export CMAKE_PREFIX_PATH=~/logos/Qt/6.9.2/gcc_64   # the sub-build reads the env

cmake -S module -B build-linux \
      -DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH \
      -DLOGOS_DELIVERY_ROOT=$PWD/_external/logos-delivery \
      -DLOGOS_DELIVERY_LIBRARY_OPTIONAL=ON
cmake --build build-linux -j"$(nproc)"

module/package-basecamp.sh build-linux linux-amd64
```

`-DLOGOS_DELIVERY_LIBRARY_OPTIONAL=ON` is the one flag that has no macOS
counterpart and it is not a shortcut: upstream publishes **no**
`liblogosdelivery.so`, and never has (the audit is in
`module/third-party/liblogosdelivery/README.md`, re-checked against the release
API when this was written). The header is the build input; the library is opened
by name at run time. So the `linux-amd64` plugin carries the delivery code path
— every one of its string literals is in the binary, which is what
`scripts/check-package-fresh.py` asserts against **all three** variants — and
there is no library beside it for `dlopen` to find. The same is true of
`linux-arm64`, built the same way. Without the flag CMake refuses,
and says which of the three states to ask for.

`package-basecamp.sh` **adds** the variant rather than replacing the package:
one machine cannot build both binaries, so recreating it would silently drop the
other. `scripts/write-package-record.py` carries the other variant's record
forward only if the package still holds the exact bytes it was recorded for and
those bytes still contain every string literal of the source being recorded —
otherwise a stale sibling would inherit a fresh variant's provenance.

**The symbol that only ELF notices.** `logos-module-builder` at `5396513`
compiles eight of the SDK's translation units into every plugin and not
`logos_types.cpp`, so the plugin references `operator<<(QDataStream&, const
LogosResult&)` and never defines it. On Mach-O that is invisible — dyld binds
lazily and `logos_host` never faulted. On ELF the module loads, joins the
runtime's loaded set, hands out a client, and then the first call across the
transport ends in

```
.logos_host.elf: symbol lookup error: agent_plugin.so:
  undefined symbol: _ZlsR11QDataStreamRK11LogosResult
```

and `configure()` times out exactly the way a Qt mismatch does. `logos_host`
links spdlog and Qt and not `liblogos_core`, on either platform, so nothing in
that process can supply it; Basecamp's own `capability_module_plugin.so` has
zero undefined Logos symbols. `module/CMakeLists.txt` now compiles
`logos_types.cpp` into the plugin on both platforms, which is why the
`darwin-arm64` variant was rebuilt in the same commit.

**Running it.** With the runtime unpacked, nothing else has to be set:

```sh
QT_ROOT=~/logos/Qt/6.9.2/gcc_64 LOGOS_CPP_SDK_ROOT=~/logos/src/logos-cpp-sdk \
  ./scripts/logos-core-headless.sh storage
```

```
install   module/agent.lgx  variant linux-amd64
          -> /root/.local/share/Logos/LogosBasecamp/modules/agent
          installed, main=agent_plugin.so
...
  ok    logos_core_load_module() reports success
  ok    the module is in the runtime's loaded set
  ok    configure() is accepted across the transport
  ok    start() is accepted across the transport
  ok    the loaded module lists all 28 documented skills
  ok    and bound to the owner it was configured with
  ok    and to the policy account it was configured with

all steps confirmed (0 failure(s))
```

That run is x86-64 Linux, from the committed `module/agent.lgx`, against
`liblogos_core.so` out of the published AppImage — the same 40 assertions the
macOS run makes, in the same order, with the same result. **aarch64 Linux is the
same**, against the `aarch64` AppImage and Qt's `linux_gcc_arm64` build:
`install module/agent.lgx variant linux-arm64`, 28 skills, 40 assertions, 0
failures. On an Apple Silicon host a `linux/arm64` container is native, so that
leg is the cheapest of the three to reproduce and the last one that was open.
The environments both were performed in are recorded in `docs/limitations.md`.

**The variant is chosen, not stumbled into.** `logos-core-headless.sh` used to
install `tar tzf … | head -n1`, which was correct for exactly as long as the
package held one variant (count-as-it-was); the first Linux run after `linux-amd64` was added
installed the **dylib**, because tar lists `darwin-arm64` first. It now requires
the variant this machine needs and names what the package has when that is
missing. There is no fallback: a module directory holding another platform's
binary is one that looks complete and can never load.

### The harness, built once per variant

The run above needs a program that drives `liblogos_core`'s C API and then calls
the loaded module across the runtime's transport — the C API can *load* a module
and cannot *call a method* on one. `scripts/logos-core-headless.sh` used to
compile it on every machine it ran on, which made the whole Qt/SDK/compiler list
above a prerequisite of **running** the deployment command rather than of
building anything. It is built once per variant instead, in the same three
places the three plugin variants are built:

```sh
# macOS, on the machine that has Qt and the SDK checkout
./scripts/logos-core-headless.sh --build-harness

# each Linux architecture, in its container (the images docs above describe)
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work lp8-linux:amd64 \
  ./scripts/logos-core-headless.sh --build-harness
docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work lp8-linux:arm64 \
  ./scripts/logos-core-headless.sh --build-harness
```

Each run writes `module/harness/<variant>/logos_core_load_test` and **merges**
its entry into `module/harness/harness.sources`, carrying the other variants
forward only if their bytes are still the recorded ones and still contain every
string literal of the harness source. `scripts/check-package-fresh.py` checks
all of it afterwards, from the binaries' own bytes and with nothing but
`python3`.

Three things about that build differ from the one a developer does for
themselves, and each is asserted rather than remembered:

- **No rpath.** The link drops `-Wl,-rpath,$QT_ROOT/lib` and the runtime's
  directory, because both name paths on the machine that ran the compiler. The
  shipped harness finds Qt through `LD_LIBRARY_PATH=$APP/usr/lib`
  (`DYLD_FRAMEWORK_PATH=$APP/Contents/Frameworks` on macOS), which the run step
  sets — the app's own Qt 6.9.2, the same one `logos_host` loads the plugin
  under. `write-harness-record.py` refuses to record a binary that has an rpath
  and `check-package-fresh.py` refuses to pass one.
- **One recipe, two destinations.** The same `compile_harness()` produces the
  shipped binary and the one `HARNESS_FROM_SOURCE=1` builds; the flags differ
  only by those rpaths. A second recipe for the shipped binary would be a second
  recipe to drift, and "you can rebuild it yourself" would stop meaning
  anything.
- **The build directory is per variant** (`build-headless/<variant>`). These
  three builds share one checkout through a bind mount, and with a single
  directory the Mach-O `.o` files from the macOS build are newer than the SDK
  sources the Linux build reads — so the Linux build skips them and hands Mach-O
  objects to `ld`.

**And the run that the whole change exists for**, on a machine that has none of
the toolchain:

```sh
./scripts/harness-no-toolchain.sh linux-arm64     # or linux-amd64
```

It copies the repository into a stock `ubuntu:24.04` container, refuses to
continue if it finds a compiler, a Qt SDK, a `logos-cpp-sdk` checkout or
`nlohmann/json` there, installs `python3` and nothing else, unpacks the
published AppImage, and runs `./scripts/logos-core-headless.sh storage`:

```
harness   /work/module/harness/linux-arm64/logos_core_load_test
          shipped, sha256 aa9c9ca53f732f39, as recorded in module/harness/harness.sources
          no compiler, no Qt SDK and no logos-cpp-sdk checkout were needed
  <-    ... 40 assertions
all steps confirmed (0 failure(s))
```

Both Linux variants pass it. Two controls run in the same container: the same
command with `HARNESS_FROM_SOURCE=1` must be refused there, naming the compiler,
Qt and the SDK; and a harness whose bytes are not the recorded ones must not be
run. On an arm64 host the `linux/amd64` leg additionally installs
`squashfs-tools`, because an AppImage extracts itself by *running* and an
emulated container cannot run an x86-64 ELF — the script says so when it does,
and a decompressor is not a compiler.

### The generated glue is committed

`module/generated_code/` holds `logos-cpp-generator`'s output for this module —
the `AgentProviderObject` that dispatches `callMethod`/`getMethods` onto the
impl, the `AgentPlugin` that carries `Q_PLUGIN_METADATA`, and the bodies for the
`logos_events:` declarations. It is committed so the build needs no code
generator, and `LogosModule.cmake` picks the directory up on its own.

Regenerate it after changing `module/agent_module_plugin_export.h`:

```sh
bash $LOGOS_CPP_SDK_ROOT/cpp-generator/compile.sh
~/logos/src/build/cpp-generator/bin/logos-cpp-generator \
    --from-header  $PWD/module/agent_module_plugin_export.h \
    --impl-class   AgentModuleExport \
    --impl-header  agent_module_plugin_export.h \
    --metadata     $PWD/module/metadata.json \
    --backend qt --output-dir $PWD/module/generated_code
```

Two things the generator's parser will do silently, both of which cost a method:
it reads **one-line prototypes only** (a signature wrapped across two lines is
skipped without a warning), and it maps a parameter type it does not know to
`QVariant` and passes it through unchanged, which then does not compile.
`module/agent_module_plugin_export.h` exists because of both — see the note at
the top of that file.

## Packaging

```sh
module/package-basecamp.sh build-basecamp
```

Packaging uses `lgx` from `logos-co/logos-package` — the tool Basecamp's own
packages are built with — found via `$LGX_BIN`, `~/logos/src/logos-package/build/lgx`,
then `PATH`. The script does not reimplement the package format; it only patches
the manifest's `author`/`description`/`type`/`category` afterwards, because
`lgx add` never reads `metadata.json` and leaves them empty. `type` is the one
that matters: Basecamp installs a `core` module into its modules directory and a
`ui` one into its plugins directory, and an unset type lands in neither.

It then refuses to hand back a package with either of the two defects that
produce a module which installs and loads nowhere, both of which are silent:

```
  ok    main[darwin-arm64] = agent_plugin.dylib is in the package
  ok    Qt is referenced through @rpath, version(s): 6.9.2
```

The first is not what `lgx verify` checks. `verify` compares the contents
against the manifest's hashes, which a manifest naming a file the package does
not contain passes perfectly well — the host resolves `main` inside the module
directory, finds nothing, and logs nothing. This repository shipped that defect
once, with `main` naming `agent_module_plugin` while the builder emits
`agent_plugin`. The second catches a plugin built against Homebrew's Qt: it
fails on the absolute `/opt/homebrew/...` paths before the version even matters,
because those resolve on the build machine and on no other.

### The package records what it was built from

Packaging also writes `module/agent.lgx.sources`: the SHA-256 of every build
input, the SHA-256 of the plugin binary that came out, and the package's
manifest root hash. `scripts/check-package-fresh.py` reads it back, and the
`package` job in `.github/workflows/ci.yml` runs that on every push.

It exists because the two checks above cannot see the defect that has now
shipped twice from this directory — a package that was correct when it was made
and was left behind by later commits to `module/src`. `3322142`'s `agent.lgx`
stayed committed across five commits to the sources; and `524866c` made the
module sign Agent Cards `secp256k1-bip340` instead of the `EdDSA` that
`scripts/use-cases/verify-agent-card.py` rejects, without repackaging, so the
published `.lgx` signed cards this repository's own verifier refuses. Both
binaries load, cast, and answer `skills()` with all 23 entries  (count-as-it-was),
and the stale one is the same 3699040 bytes as the fresh one, so nothing in
`module/tests/` could tell them apart.

Do not hand-edit that file. If CI says the package is stale, the fix is to
rebuild and repackage; editing the hashes is a claim about a build that did not
happen, and the checker's second layer — every string literal in `module/src` of
8 bytes or more must be present in the shipped binary — is there to catch
exactly that.

Locally, and only locally, the same script will also rebuild and compare:

```sh
./scripts/check-package-fresh.py --rebuild build-basecamp
#   ok    a rebuild of the committed source is byte-identical to the
#         darwin-arm64 binary in the package (sha256 595c225721ab998a)
```

That comparison is whole-file, and it holds: rebuilt into a different build
directory, from a source tree at a different absolute path, under a different
`TZ` and a different locale, the plugin comes out at the identical SHA-256. What
it does not survive is a different toolchain — the compiler, the macOS SDK, the
Qt patch level and `nlohmann/json` all reach the bytes and none of them is
pinned by this repository — which is why `--rebuild` is a local command and not
a CI step. It fails loudly when the toolchain is absent rather than passing.

### The 16 MB in it, and why it is committed rather than downloaded

The committed package is `module/agent.lgx`. It was 674 KB, then 589 KB, then
16 MB with one `darwin-arm64` variant, and it is larger again now that it
carries three — and nobody noticed the first two changes, which is the whole
argument for the checked record above. No byte count is written here any more
for the same reason: it moved three times without this sentence moving, and
`ls -l module/agent.lgx` answers it. This one is not an accident and it is not free, so the reasoning is here
rather than in a commit message nobody re-reads.

**What is in it.** `liblogosdelivery.dylib`, 42 MB uncompressed, because the
module opens its own Delivery node (§"The ports a loaded module builds for
itself") and the library has to be somewhere the module can find. Compressed it
is 16 MB; stripped and compressed it is 14.3 MB, and it is deliberately *not*
stripped — a 1.7 MB saving is not worth shipping a modified copy of somebody
else's binary, because "this is what `make liblogosdelivery` produced" is a
property worth keeping.

**Why not fetch it at install time with a pinned checksum**, which is what the
`storage-node` CI job does for `libstorage` and is plainly the better shape:
because there is nothing to fetch. Upstream's releases since `v0.37.0-beta`
carry **zero assets**; its `release-assets.yml` does build a darwin-arm64
`liblogosdelivery` and then uploads it to the GitHub *run artefact store* —
authenticated, expiring, no stable URL — with no release-upload step anywhere in
the file, so even a green run would publish nothing installable; and no run of
it has been green since 2025-10-16. Nimble ships source, there is no Homebrew
formula, the container images are linux executables with a 30-day expiry, and
the one Logos nix cache that really does hold an `aarch64-darwin` build holds it
at version `dev` from a pull-request commit, in a cache that garbage-collects,
signed with a key `flake.nix` does not publish. The full check, with what was
run, is in
[`module/third-party/liblogosdelivery/README.md`](../module/third-party/liblogosdelivery/README.md).
`libstorage` is fetched because `logos-storage/logos-storage-nim` — a different
organisation — publishes a full asset matrix including darwin-arm64. The pattern
is right; the delivery repository does not offer it.

**Licence.** `liblogosdelivery` is **MIT OR Apache-2.0**, © 2025-2026 Logos, so
redistributing the binary inside this package is permitted. It is not free of
obligations: MIT requires the copyright and permission notice to travel with the
copy. `package-basecamp.sh` therefore stages
`module/third-party/liblogosdelivery/` into every package that carries the
library — and **fails** if a library is staged with no licence directory beside
it, because shipping someone else's binary without one is a licensing defect and
not a packaging one. The statically-linked `librln` v2.0.2 is Apache-2.0 OR MIT
(© 2022 Vac Research). The only GPL-family files anywhere upstream are four
Solidity test harnesses under `vendor/waku-rlnv2-contract/lib/`, which are never
compiled and are not linked in.

**What would delete all of this**: one release-upload step upstream, and one
green run of it. Nothing in the module would have to change, because the library
is opened by name at run time rather than linked — where it comes from is the
installer's business.

Check the package against itself rather than trusting this document:

`lgx` is not on `PATH` after building `logos-package` — it stays in that
checkout's `build/`, which is where `package-basecamp.sh` looks for it. Written
out here, because the two lines below used to say `lgx …` and a reader following
them got `command not found: lgx` and no hint that the tool was sitting in a
directory this document had already named:

```sh
LGX="${LGX_BIN:-$HOME/logos/src/logos-package/build/lgx}"
$LGX verify   module/agent.lgx    # contents match the manifest hashes
$LGX manifest module/agent.lgx    # type: core, three variants
```

`manifest` prints three variants now — one for every platform the Logos app is
published for — and which one you get is the point of them:

```
Variants:       darwin-arm64, linux-amd64, linux-arm64
                darwin-arm64 -> agent_plugin.dylib
                linux-amd64 -> agent_plugin.so
                linux-arm64 -> agent_plugin.so
```

That prints root hash
`05028f1024605d1562455e9c39b888551c454a73952192a61132aaa43cfdcc02`. Rebuilding
the module changes it; none of the checks below depend on the value, and this
line no longer has to be remembered — the same hash is in
`module/agent.lgx.sources`, written by the packaging script and checked by CI,
so a stale copy of it here is now a CI failure rather than a paragraph nobody
re-reads. (This line was stale: it said `cf07408e…`, which is the package as it
stood at `d7f68c2` — the one whose Agent Cards this repository's own verifier
rejects. The archive's own sha256 changes on every repackage even when the root
hash does not, because gzip records a timestamp. The root hash is the one that
describes the contents.)

## Installing it into Basecamp

There is no "install from file" button in Basecamp 0.2.2's Package Manager — it
installs from a configured package repository only. Install by hand into the
user modules directory, which Basecamp adds to Logos Core at startup
(`logos_core_add_modules_dir(LogosBasecampPaths::modulesDirectory())`):

| Platform | User modules directory |
|---|---|
| macOS | `~/Library/Application Support/Logos/LogosBasecamp/modules` |
| Linux | `~/.local/share/Logos/LogosBasecamp/modules` |

`LOGOS_USER_DIR` overrides the base directory outright, which is the clean way
to try this without touching an existing install.

The package is a gzipped tar of `manifest.json` + `variants/<variant>/…`; an
installed module is that variant **flattened**, plus a `variant` file naming it.
Dropping the archive in does nothing.

```sh
DEST=~/Library/Application\ Support/Logos/LogosBasecamp/modules/agent
mkdir -p "$DEST" && cd "$DEST"
tar xzf /path/to/lp-0008/module/agent.lgx
mv variants/darwin-arm64/* . && rm -rf variants
printf 'darwin-arm64' > variant
ls   # agent_plugin.dylib  liblogosdelivery.dylib  manifest.json  metadata.json
     # third-party/  variant
```

`liblogosdelivery.dylib` belongs in that directory and not somewhere on a
library path: the plugin asks for it by name and looks beside itself first, so
the module directory is where it is found. `third-party/liblogosdelivery/` is
upstream's licence, which travels with the binary because this is
redistribution — see below.

**Leaving it out is survivable, and that is the point.** The plugin does not
link the library; it opens it with `dlopen` when a node is first asked for. A
module directory missing it still loads, still registers all 28 skills, and
answers `meta.status` with the file it wanted and every path it tried:

```json
{"linked":true,"state":"failed",
 "error":"liblogosdelivery.dylib could not be opened. Tried: …/modules/agent/liblogosdelivery.dylib,
          liblogosdelivery.dylib. It belongs in the module directory, beside the plugin"}
```

Linked instead, the same mistake makes the *plugin* fail to load —
`Library not loaded: @rpath/liblogosdelivery.dylib` — which Basecamp reports to
nobody: the tile is inert and the reason goes to stderr. That was measured, both
ways, before the choice was made.

## Running the checks

Harness 2 — install, then Logos Core headless — is also
`./scripts/logos-core-headless.sh <category>`, which does everything in this
section in one command: unpacks `module/agent.lgx` into the user modules
directory, flattens the variant, compiles the SDK translation units and the
harness if they are not already built, and runs it. It additionally passes the
owner and policy account from `artifacts/agents.tsv`, so the module is
configured with the envelope anchored on chain for that agent rather than with
the placeholder below; the harness defaults to the placeholder when those
arguments are absent, so the invocation recorded here still works unchanged.

Harnesses 3 and 4 — the transport ones — have a runner of their own, for the
same reason and with the same shape:

```sh
./scripts/delivery-in-plugin.sh          # 3, and 1 again through QPluginLoader
./scripts/delivery-in-plugin.sh peers    # 4: two loaded modules, two nodes
./scripts/delivery-in-plugin.sh signers  # what the two delegates say, free
./scripts/delivery-in-plugin.sh settle   # 5: discover, serve and pay, one flow
./scripts/delivery-in-plugin.sh approval # an owner on another node answers
./scripts/delivery-in-plugin.sh two-modules  # …and that owner is a module too
```

`two-modules` is the headless form of §"Two Basecamps" above: both ends are the
same package loaded twice and driven only through `invoke`, the owner's end
through `owner.watch` / `owner.pending` / `owner.answer`. It runs three times —
nobody watching, then approve, then deny — and the first run is the one that
makes the other two mean anything.

`signers` needs nothing but the package and a network for the node. `settle`
needs a funded agent wallet and moves real testnet LEZ, so it is the one to
think before running.

The by-hand versions are kept because they are what the wrappers do, and because
when a wrapper fails they are the way to find out where.

All four harnesses are plain compiles — no CMake target, so they cannot silently
stop being built.

```sh
SDK=$HOME/logos/src/logos-cpp-sdk
QT=$HOME/logos/Qt/6.9.2/macos
APP=/Applications/LogosBasecamp.app
# An array, not a string: zsh does not word-split an unquoted variable, so a
# string here reaches clang as one enormous argument and every Qt header goes
# missing at once. "${QTINC[@]}" is right in both shells.
QTINC=(-F$QT/lib -I$QT/lib/QtCore.framework/Headers
       -I$QT/lib/QtRemoteObjects.framework/Headers
       -I$QT/lib/QtNetwork.framework/Headers)

# 1. the Qt plugin contract
clang++ -std=c++17 -o /tmp/plugin_load_test \
    module/tests/plugin_load_test.cpp $SDK/cpp/logos_types.cpp \
    -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-rpath,$QT/lib
# the artefact, not the build tree — install it first, per the section above
/tmp/plugin_load_test \
    "$HOME/Library/Application Support/Logos/LogosBasecamp/modules/agent/agent_plugin.dylib"
```

Harness 2 needs the SDK, because calling the loaded module is the point of it
and `liblogos_core`'s C API has no "call a method" entry point — a core module
runs in its own process and is reached over the transport. So build the SDK
translation units `LogosModule.cmake` compiles into the module itself, minus
`token_manager.cpp` (which is the trap described below) and plus
`logos_types.cpp` for the `LogosResult` metatype that crosses the wire. Run
`moc` over the headers that declare `Q_OBJECT`, and link the lot:

```sh
mkdir -p /tmp/sdkobj && cd /tmp/sdkobj
for h in logos_api logos_api_client logos_api_consumer logos_api_provider \
         module_proxy qt_provider_object; do
    $QT/libexec/moc $SDK/cpp/$h.h -o moc_$h.cpp -I$SDK/cpp -I$SDK/core
done
# module_proxy.cpp and qt_provider_object.cpp #include their own moc output, so
# `-I.` is what finds it and their moc_*.cpp is not a translation unit of its
# own — compiling it as one is a duplicate-symbol link error. logos_provider_object.h
# declares Q_OBJECT but moc emits nothing for it ("No relevant classes found").
for f in $SDK/cpp/logos_api.cpp $SDK/cpp/logos_api_client.cpp \
         $SDK/cpp/logos_api_consumer.cpp $SDK/cpp/logos_api_provider.cpp \
         $SDK/cpp/module_proxy.cpp $SDK/cpp/logos_provider_object.cpp \
         $SDK/cpp/qt_provider_object.cpp $SDK/cpp/logos_types.cpp \
         moc_logos_api.cpp moc_logos_api_client.cpp \
         moc_logos_api_consumer.cpp moc_logos_api_provider.cpp; do
    clang++ -std=c++17 -c -I. -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include \
        "${QTINC[@]}" "$f" -o "$(basename ${f%.cpp}).o"
done
cd -

# 2. Logos Core, from the installed app
clang++ -std=c++17 -o /tmp/logos_core_load_test \
    module/tests/logos_core_load_test.cpp /tmp/sdkobj/*.o \
    -I$SDK/cpp -I$SDK/core -I/opt/homebrew/include "${QTINC[@]}" \
    -framework QtCore -framework QtRemoteObjects -framework QtNetwork \
    -Wl,-undefined,dynamic_lookup \
    -Wl,-rpath,"$APP/Contents/Frameworks"

LOGOS_HOST_PATH="$APP/Contents/MacOS/logos_host" \
QT_PLUGIN_PATH="$APP/Contents/Resources/qt/plugins" \
/tmp/logos_core_load_test \
    "$APP/Contents/Frameworks/liblogos_core.dylib" \
    "$APP/Contents/modules" \
    "$HOME/Library/Application Support/Logos/LogosBasecamp/modules" \
    /tmp/agent-persistence agent
```

Harness 2 links Qt for headers and stubs but adds an rpath pointing at the
**app's** frameworks, so exactly one QtCore is in the process and it is the one
the runtime resolves. Four traps, and the first three look like a hang or a
permission error rather than a mistake in the build:

- Without a `QCoreApplication` constructed **before** `logos_core_init`, the
  module transport reports `QEventLoop: Cannot be used without QCoreApplication`
  and the load never returns. Basecamp constructs its `QApplication` first; the
  harness does the same.
- Without `LOGOS_HOST_PATH`, the runtime logs `logos_host_qt (or logos_host) not
  found` and `Failed to load module: agent`. Core modules are process-isolated
  and the host binary is what runs them.
- **`token_manager.cpp` is deliberately absent from that list**, and building it
  in is the mistake that costs the most time. `TokenManager::instance()` is a
  singleton, and the capability tokens the harness needs to call the module are
  the ones `liblogos_core` minted into *its* copy. Compile a second one into
  the executable and it wins the symbol, starts empty, and every call comes back
  `ModuleProxy: rejecting unauthorized call to "requestModule" — auth token not
  recognized`, which reads like a permissions problem and is not. Leaving it out
  lets `-Wl,-undefined,dynamic_lookup` resolve the singleton to the runtime's,
  which is what Basecamp gets for free by linking `liblogos_core` rather than
  the SDK.
- That same `dynamic_lookup` is what resolves `LogosTransportFactory` and the
  rest of the transport layer, which the module does not carry either — the
  host provides them. It is the configuration the plugin itself runs in, not a
  shortcut around one.

The persistence directory can be left in place between runs. `configure()` may
be called only once, but only per module *instance*: the flag lives in memory,
and the runtime starts a fresh `logos_host` for the module on every run, so the
second `configure()` of the day is the first one that process has seen. Checked
by running the harness twice against the same `/tmp/agent-persistence` — both
green.

## Watching Basecamp itself

Basecamp's file log
(`~/Library/Application Support/Logos/LogosBasecamp/logs/`) truncates early and
never records the loader messages. Launch it from a terminal and read stderr:

```sh
/Applications/LogosBasecamp.app/Contents/MacOS/LogosBasecamp > /tmp/out.log 2>&1 &
grep -E 'Module loaded|Total modules' /tmp/out.log
```

An installed-but-not-enabled core module does **not** appear there — Basecamp
prints `Total modules: 3` and auto-loads only its own three. That is the
expected state, not a failure: `logos_core_load_module` is what promotes a known
module to a loaded one, and harness 2 is that call.

Measured again, with the module sitting at the exact path above and Basecamp
0.2.2 launched twice — once with `LOGOS_USER_DIR` pointing at a scratch base and
once against the real one:

```
[info] [logos] Module loaded: capability_module
[info] [logos] Module loaded: package_manager
[info] [logos] Module loaded: package_downloader
Total modules: 3
```

and `grep -ci agent` over the whole of that output returns **0**. That is still
true, and it is still the right thing to expect: an installed core module that
nothing has loaded is not named anywhere, because Basecamp auto-loads only its
own three. It is not evidence that the module is unreachable — it is evidence
that nothing has asked for it yet.

What used to follow this paragraph was the conclusion that the criterion could
not be met, on two grounds. One of them was wrong and the other was
incomplete:

- **"This repository ships no Basecamp `ui` app, so a `core` module has no
  window."** True when written, and it was the whole blocker. `app/` is that
  plugin now; the section below is the record of it loading.
- **"Its Package Manager installs from a configured repository only, so a local
  `.lgx` cannot be installed through the GUI at all."** Still true, and still
  worth stating — but it was never a blocker on *accessibility*, only on the
  install being a click. Both packages are installed by hand here, and a
  reviewer does the same.

## Inside the running app

The same `grep -ci agent`, on a Basecamp launched with **both** packages
installed — `module/agent.lgx` in the modules directory, `app/agent-ui.lgx` in
the plugins directory:

```
$ grep -ci agent /tmp/out.log     # at startup, before anything is clicked
0
$ grep -ci agent /tmp/out.log     # after clicking the sidebar tile once
30
```

Still 0 at startup, and that is correct rather than a shortfall: nothing has
asked for the module yet. The click is what asks. Do not read the number
itself as the result — it counts every line the console's calls produce and
was 62 after an approval round trip in the same run. **0 → nonzero** is the
result, and the lines under it are what it is made of, in order:

```
App launcher clicked: "agent-ui"
Loading UI module: "agent-ui"
Loading core dependency for "agent-ui" : "agent"
[info] [logos] Module loaded: agent
[info] [logos] [agent] [LogosProviderObject] LogosAPIProvider: successfully published "agent"
MainContainer: Added plugin dock to WorkspaceArea: "LP-0008 Agent" (module: "agent-ui" )
Successfully loaded UI module: "agent-ui"
[info] [logos] [agent] ModuleProxy: callRemoteMethod "status" args: 0
```

`Loading core dependency` is the whole mechanism, and it is not a side effect:
Basecamp's PluginLoader reads a `ui` plugin's `dependencies`, calls
`logos_core_load_module` for each, has `capability_module` mint the plugin a
token for it, and only then calls `createWidget(LogosAPI*)`. So declaring
`"dependencies": ["agent"]` in `app/metadata.json` is what turns a click on a
tile into a loaded module. The last line is the console's first call arriving.

The app presents a window in this environment — the previous revision of this
document said it did not, and that was a property of how it had been launched,
not of the app. Both the window and the pane that could not be read before are
now readable without a screenshot, through macOS's accessibility API, which
makes the tile's own label an assertion rather than a description:

```sh
osascript -e 'tell application "System Events" to tell process "LogosBasecamp.bin" \
  to tell window "Logos Basecamp" to get name of every button'
#   LP-0008 Agent, …, Applications, Package Manager, Settings
```

The elision is two further tiles, for unrelated packages that happen to be
installed in the same Basecamp on the machine this was read from. They are
nothing to do with this module, so they are cut rather than reproduced — but
they are why this reads as six buttons and not four.

Before `app/` existed the same command returned that list without its first
entry. That is the criterion's "accessible from the Logos app", read out of the
app itself.

What the window then does — bind the agent, start it, list its 28 skills,
invoke any of them, and answer the spends it asks the owner to approve — is in
`app/README.md`, with the transcript of a completed approval round trip. The
two module-side facts that round trip depends on are in
`docs/limitations.md` §"The owner channel inside Basecamp"; both were found by
measurement and one of them changed `module/src`.

## Two Basecamps, and the owner in the second one

The criterion, verbatim: *"The owner can interact with the agent in real time
from a separate Logos app instance using Logos Messaging, with no intermediary
server."* Three of its four clauses were closed before this section existed —
`scripts/delivery-in-plugin.sh approval` completes a correlated approval round
trip between two processes on two Delivery nodes over the public relays. The
clause that was open is **"a separate Logos app instance"**: the owner in that
exercise is `module/tests/owner_responder.cpp`, a program this repository wrote,
and the prize glosses the phrase itself — its Usability criterion says
"accessible from the Logos app (Basecamp)" and its Architecture section says
"any Logos app instance that holds the owner's keys".

So: two LogosBasecamp 0.2.2 processes, the same installed bundle, each with its
own `LOGOS_USER_DIR`, its own working directory, its own module and plugin
directories, and its own Logos Delivery node. Both were driven through macOS's
accessibility API — the buttons are clicked, the text fields are typed into, and
the transcripts below are read back out of each window's own text pane, so
every line here is the app's, not this document's.

**Three things had to be true before any of it, and each could have ended it.**

1. **Two instances run at all.** They do. Each prints its own base data
   directory and each mints its own transport registry — `local:logos_
   capability_module_0b89831138ba` in one and `local:logos_capability_module_
   85c62059ec7c` in the other, per instance rather than per module name, so
   the two runtimes do not collide on a socket.
2. **Each opens its own window with the module's tile in it.** `System Events`
   reports two processes named `LogosBasecamp.bin`, each with a window "Logos
   Basecamp", each listing `LP-0008 Agent` among its buttons.
3. **Each loads its own copy of the module.** Two `logos_host` processes, one
   per app, each `--path`ed at that app's own copy of the plugin, under that
   app's own modules directory,
   and `--instance-persistence-path`ed under that app's own base — and each
   inheriting its app's working directory, which is not tidiness: **a Delivery
   node keeps its reliable-channel state in the current working directory**, and
   two nodes started from one directory share it silently. Launch each app from
   a directory of its own.

### What the two windows printed

One machine, one clock. The agent's app on the left, the owner's on the right;
both transcripts are quoted from the windows.

```
agent app                                     owner app
12:53:22.792 <- delivery node: starting       12:53:26.243 <- meta.configure(delivery): "on"
12:53:26.699 <- delivery node: ready          12:53:30.201 <- delivery node: ready
                                              12:53:44.068 → invoke(owner.watch, {})
                                              12:53:44.101 <- owner.watch: {"channel":
                                                 "/lp-0008/1/owner-channel/BzYks91a…/5Sa13NyN…",
                                                 "topic":"/lp-0008/1/owner-BzYks91a…/json","ok":true}
12:53:57.627 → invoke(wallet.send,
   {"recipient":"Public/Dxh7…","amount":"250"})
                                              12:53:58.200 <= over Logos Messaging:
                                                 {"arrived":1,"frames":1,"pending":[{"id":
                                                 "spend-1786877637631","seed_verified":true,…}]}
                                              12:54:08.304 → invoke(owner.answer,
                                                 {"decision":"deny","id":"spend-1786877637631"})
12:54:08.675 <- invoke(wallet.send): {"outcome":"denied","attempts":1,
   "error":"the owner denied this spend, so it was not submitted",
   "submitted":false,"waited_ms":11043}
12:54:21.683 → invoke(wallet.send, …)
                                              12:54:24.803 → invoke(owner.answer,
                                                 {"decision":"approve","id":"spend-1786877661687"})
12:54:24.980 <- invoke(wallet.send): {"outcome":"approved","approved":true,
   "attempts":1,"submitted":false,"waited_ms":3292}
```

Three numbers out of that, and they are what "in real time" means here: the
request was visible in the *other app's window* **573 ms** after the agent was
asked to spend; the owner's denial reached the agent **371 ms** after the button
was pressed, and the approval **177 ms** after. Nothing was submitted either
time — an approval unlocks the `spend_approved` path, it does not move money,
and this module wires no wallet.

Both windows are the same `ui` plugin, `app/agent-ui.lgx`, and both talk to the
same `core` module, `module/agent.lgx`. The owner's half is three skills the
module registers in every build — `owner.watch`, `owner.pending`,
`owner.answer` (`module/src/owner_skills.cpp`) — reached the only way a `ui`
plugin can reach a core module, which is `invoke()`. The window polls
`owner.pending` once a second; that poll is why a request minted in the other
app appears without anybody pressing anything.

### The control, which is the reason to believe the rest

**A node receives its own published messages.** So "the owner answered" is an
assertion one process can satisfy alone, and this whole exercise would prove
nothing without watching it fail. The owner's app was killed — the process, not
the window — and the same call made again:

```
12:54:43.333 → invoke(wallet.send, {"recipient":"Public/Dxh7…","amount":"250"})
12:54:58.347 <- invoke(wallet.send): {"outcome":"owner_unreachable","attempts":2,
   "error":"the owner did not answer within 15000ms: 2 notification attempt(s),
    2 of which the channel accepted; the spend was not submitted",
   "submitted":false,"waited_ms":15009}
```

and the agent's own `meta.status` at the end of the run says why that is not a
network failure being read as a control:

```
"delivery":{"frames":{"channel_decoded":2,"channel_seen":2,"relay_seen":6},"state":"ready"}
```

**Six** relay frames came back to the agent's own node — its own six requests,
off the public relays, exactly the self-satisfying case — and produced no
approval. **Two** channel frames were decoded, and they are the owner's two
answers, one deny and one approve. The same control runs headless in
`./scripts/delivery-in-plugin.sh two-modules`, whose first of three runs has
nobody watching and asserts the terminal outcome.

The authorship rule that refuses a self-authored *request* cannot be exercised
this way, and the reason is worth recording because it is the opposite of what
was expected: this transport hands a node its own **relay** messages and does
**not** hand it its own **reliable-channel** frames. Measured on both sides —
the owner's `owner.pending` reports `self_refused: 0` with `frames: 1` in every
live run. So the rule is exercised where a self-authored frame can be put in
front of it, in `module/tests/owner_skills_test.cpp`, with the falsification
beside it: the same bytes with the other account as the author, which must be
accepted.

### The one ordering that does not work, measured

**The owner's app must open the channel before the agent's first request on
it.** The first attempt at this exercise did it the other way round: the agent
published two requests at 12:49:46 and 12:49:54, and the owner watched at
12:50:14. The agent went on asking, the owner's node *received the frames* —
its log carries `received relay message … contentTopic=/lp-0008/1/owner-
BzYks91a…/json` at 12:50:23 and 12:50:38 — and the owner's module reported

```
"delivery":{"frames":{"channel_decoded":0,"channel_seen":0,"relay_seen":2},"state":"ready"}
```

The bytes were in the process and the reliable channel never delivered them: a
channel opened mid-stream does not receive the backlog, and this reads exactly
like a network that is not carrying anything. With the owner watching first,
every frame arrives. That is also the order the runner script uses — it starts
the owner two seconds ahead — and it is the order the real deployment has
anyway, since an owner's app is running before an agent needs it.

### What this does not claim

The owner's app is bound to the owner's **account**; it does not sign the reply,
and no key is exercised anywhere in this run. That is not an accident of the
demonstration, it is what the channel is: sender ids on it are self-declared,
`owner_channel.h` says so at length, and the authority that decides whether an
above-threshold spend can happen is the approval account on chain, which only
the owner's own signature on `approve_spend` can create. What the second app
demonstrates is reach and correlation — the owner is somewhere else, the terms
that came back are the terms that went out, and there is nothing between the two
apps but the public relays.

## What still has to be built for the criterion to be met

Honest list, in the order that matters:

1. The skills need their ports wired from inside the loaded module. This item
   has been rewritten twice and is now mostly done. It first read "the plugin
   has to construct and register the skill objects"; it does, and all 28
   dispatch. It then read "`registerBuiltinSkills` takes `std::function` ports
   that cannot cross a plugin boundary", which was the wrong conclusion from a
   right premise — a host cannot pass a closure, and a module can build one. The
   transport ports are now built by the module itself
   (`module/src/delivery_runtime.cpp`), so `messaging.*`, `agent.discover`,
   `agent.task` and `agent.subscribe` work in a loaded plugin. What remains
   needs something in the module's process that no port can supply: a Logos
   Storage node for `storage.*`, a sequencer endpoint for `program.query` and
   `wallet.balance`, and a local `spel` for `program.call` / `program.deploy`.
   Those are three separate pieces of work, not one boundary.

   `agent.task`'s settlement was on that list and is not any more, and the way
   it came off it is worth reading as a pattern rather than as a fix: it did not
   need a wallet **in** the module's process, it needed the module to be able to
   **reach** one. `card_signer` had shown that a loaded plugin can run a command
   and check its answer; `pay_signer` and `policy_source` are the same function
   with a wallet and a sequencer on the other end of them. Two of the three
   remaining items are the same shape and could go the same way.
2. The owner channel over **Logos Messaging** has to be driven from inside the
   loaded module, so that "the owner can interact with the agent in real time
   from a separate Logos app instance using Logos Messaging" is demonstrable.
   This item has moved but is not closed. What the loaded module has now is an
   owner channel built out of the *runtime's* surface rather than Delivery's:
   the module emits `ownerApprovalRequested(requestJson, attempt, timestamp)`
   once per notification attempt, and the owner answers with the module method
   `approveSpend(requestId, verdict)`. Harness 2 exercises both across the
   transport, and the runtime's own log carries the emissions:

   ```
   [logos] [agent] [LogosProviderObject] emitEvent: "ownerApprovalRequested"
   [logos] [agent] [LogosProviderObject] ModuleProxy: forwarding event as Qt signal
   ... eight times, 200 ms apart ...
     ok    an above-threshold spend nobody approved is not submitted by the loaded module
     ok    the notification was retried before the timeout: 8 attempts
     ok    approveSpend is reachable, and refuses a request nobody is waiting on
   ```

   That closes the Reliability half — a spend that does not reach its owner is
   retried, timed out, reported and not executed.

   The last sentence of this item used to read: "It does not close the Usability
   half, which names Logos Messaging and a second app instance specifically, and
   `OwnerChannel` (which does speak Delivery) still needs a `DeliveryPort` the
   plugin cannot be handed." **The transport half is now closed and watched.**
   The plugin builds its own `OwnerChannelPort` from its own node and constructs
   `OwnerChannel` on it (`AgentModuleImpl::publishApprovalOverDelivery`),
   preferring it over the runtime event when `owner_channel_account` and
   `agent_account` are configured and the node is up.
   `./scripts/delivery-in-plugin.sh approval` runs it twice, against
   `module/tests/owner_responder.cpp` on a second node:

   ```
   the owner will approve
     ok  and it is the seed the agent named — two independent derivations
     ok  the owner's approval went out on the channel
     <-  outcome: approved, approved: true, attempts: 1, waited: 472 ms
   the owner will deny
     <-  outcome: denied, approved: false, attempts: 1, waited: 463 ms
     ok  the owner's DENIAL came back as a denial
   ```

   The deny run is the control: a channel that reported "approved" for whatever
   came back would pass the first run and only the first.

   **Two things this cost, both worth recording.** The path could never have
   worked as first written: `OwnerChannel::requestApproval` refuses a request
   with no marker seed *before it sends anything*, and the module was sending an
   empty one, with a comment explaining that the derivation lived in a crate it
   does not link. Every piece was separately tested and the assembly was not.
   `module/src/spend_marker.cpp` derives it now, and
   `module/tests/spend_marker_test.cpp` pins that derivation to
   `crates/agent-policy-core` by *running the crate* rather than by comparing
   against a table. And the first live run still failed, looking exactly like an
   owner who never answered — 23 attempts, no verdict — until the frame counters
   in `meta.status` showed `channel_seen: 1, channel_decoded: 1`: the answer had
   arrived and been read, and it was the *responder* that was wrong, sending
   `{"approve": true}` where `checkReply` reads `decision`.

   ~~Still open: the **second app instance**. The owner end is a program written
   for the purpose, not Basecamp with a person in front of it.~~ **Closed**, and
   the record is §"Two Basecamps, and the owner in the second one" above: two
   LogosBasecamp 0.2.2 processes, each with its own user directory, its own
   working directory, its own loaded module and its own Delivery node; a spend
   minted in one, visible in the other's window 573 ms later, denied from there
   in 371 ms and approved in 177 ms, with the owner's app killed for the control
   and the outcome then `owner_unreachable` with nothing submitted.

   What that took was not a new transport. `./scripts/owner-channel-live.sh` had
   already put two processes on two Delivery nodes and completed a correlated
   round trip in **312 ms on the first attempt**; what was missing is that
   nothing on the OWNER's side could be reached through a module method table,
   so a second Basecamp had a window and nothing to say with it. Three skills
   fixed that — `owner.watch`, `owner.pending`, `owner.answer`, in
   `module/src/owner_skills.cpp` — because `invoke()` is the only thing a `ui`
   plugin can call, and it is now enough.

   The line that closed *before* this one is still worth separating from it: the
   owner can also answer from a window in the *same* instance, over Logos Core's
   transport rather than Delivery (`app/README.md` has that transcript). That
   one is the Usability criterion's "accessible from the Logos app"; this one is
   the Architecture criterion's "any Logos app instance that holds the owner's
   keys", minus the keys — see the last subsection above for exactly what the
   second app is bound to and what it does not sign.
3. ~~A `linux-amd64` variant of **both** packages, since a reviewer may be on
   Linux and a package with only `darwin-arm64` is unopenable for them.~~
   Built, and so is `linux-arm64`. Both packages carry all three variants —
   every platform the Logos app is published for — and Logos Core loads,
   configures and starts the module out of the committed package on x86-64 and
   on aarch64 Linux. See "Linux, and the two Linux variants" above, which
   records the three defects that had to be fixed first and the one capability
   the Linux variants do not have (no `liblogosdelivery.so` exists to ship).
4. ~~A Basecamp `ui` app for the owner console.~~ Built: `app/`. It is a Qt
   Widgets plugin implementing `IComponent`, packaged `type: ui`, and it drives
   the loaded module through its published method table — no second
   implementation of anything. `app/README.md` is its build, package and
   install procedure, and `app/tests/ui_plugin_load_test.cpp` is the harness
   that reproduces Basecamp's own PluginLoader and was watched failing against
   two other real Qt plugins before it was believed.
5. ~~The wallet, storage and messaging modules loaded beside it.~~ Built and
   run: see the section below. This item spent two revisions of the submission
   draft — the working document, which is no longer carried in this repository —
   being described as impossible against Basecamp 0.2.2, which was a statement
   about the app *bundle* and not about Logos Core.

## Alongside the wallet, storage and messaging modules

The criterion, verbatim: "The agent module loads and runs inside Logos Core
alongside the wallet, storage, and messaging modules without requiring
modifications to those modules."

This document, and the submission draft with it, said for a long time that no
submission could close that against this host, because
`ls /Applications/LogosBasecamp.app/Contents/modules/` returns exactly three
modules and none of them is a wallet, a storage or a messaging module. The
listing is right. The conclusion was wrong, and it was wrong in a way worth
naming, because the evidence for it was real and pointed at the wrong thing:
**Logos Core loads from the user modules directory as well as the bundle's**,
which is the whole subject of the "Installing it into Basecamp" section above
and is how this repository's own module gets in. What the bundle ships was never
the constraint.

### The two commands

```sh
./scripts/build-companion-modules.sh                  # storage, delivery, wallet
./scripts/logos-core-headless.sh storage --alongside
```

Both exit 0. The first fetches the three module repositories at the revisions
below, stages the external library each one's own `metadata.json` declares, runs
`logos-cpp-generator` over its impl header, builds each against the pinned Qt,
and packages each as an `.lgx` with the same `lgx` tool `module/package-basecamp.sh`
uses. The second installs all four packages — the agent's and the three
companions' — into one modules directory and runs
`module/tests/logos_core_load_test.cpp` against them.

By default the companions go into `build-companions/modules`, not into the
Logos Basecamp install. Not because the real directory would not work — it is
the same code path, and `logos_core_add_modules_dir` is handed whichever it is —
but because a verification command should not drop three third-party modules
into somebody's app behind their back. `LOGOS_MODULES_DIR=…` points it at the
real one.

### The pinned revisions

| Repository | Revision | Why this one |
|---|---|---|
| `logos-co/logos-storage-module` | `f6bfab3` | default-branch tip |
| `logos-co/logos-delivery-module` | `3f0f2d8` | default-branch tip; this is the messaging module |
| `logos-co/logos-wallet-module` | `f6f9c16` | default-branch tip |
| `logos-messaging/logos-delivery` | `f8b0365` | **the delivery module's own `flake.lock` pin**, not the tip — see below |
| `status-im/go-wallet-sdk` | `f6c0924` | the wallet module's own `flake.nix` pin |
| `status-im/nim-taskpools` | `9e8ccc7` | `logos-delivery`'s own `nimble.lock` pin at `f8b0365` |

`logos-co/logos-modules` aggregates the three modules as submodules, but its
submodule pointers are four months behind (April 2026) and predate the
`universal` interface these revisions use; building those instead would need a
different `logos-cpp-sdk`. The tips are what the module repositories publish.

### The one upstream break this has to route around

`logos-delivery-module`'s tip does **not** build against `logos-delivery`'s tip.
The library's C ABI moved on 2026-08-14: the reply callbacks went from a scalar
convention, `void (*)(int, const char*, size_t, void*)`, to a three-argument one,
`void (*)(int err_code, const char* reply, const char* err_msg, void*)`, and the
module's own `api_call_handler.h` still binds the old shape. Against the newer
library that is seventeen compile errors, the first of them

```
error: no matching function for call to 'bindApiCall'
note: cannot initialize a parameter of type
      'void (*)(int, const char *, const char *, void *)' with an lvalue of
      type 'DeliveryCallback' (aka 'void (*)(int, const char *, unsigned long, void *)')
```

So the library is built at `f8b0365`, which is the revision
`logos-delivery-module`'s own `flake.lock` names. That is its published build
input; taking the tip instead is the deviation, not the other way round.

One more thing bites on the way there, and it is recorded because it costs an
hour to find. `make liblogosdelivery` at that revision fails with

```
ffi/ffi_context.nim(6, 58) Error: cannot open file: taskpools/channels_spsc_single
```

because `nimble setup --localdeps` resolves `taskpools` to `7d96007`, which has
no `channels_spsc_single`, rather than to the `9e8ccc7` its own `nimble.lock`
pins, which does. `scripts/build-companion-modules.sh` detects that exact
condition — the file's absence, not the revision — and puts the locked revision
in place before building. This is a dependency of the *library*; nothing in the
three module checkouts is touched by it, and the check below would catch it if
it were.

**And one break in this script's own assumptions, found by running it on a
second machine.** It staged `library/generated/logosdelivery.h` out of the
delivery checkout and *died* without one — which meant it could not complete
against the very revision it pins. That header is a build artefact and not a
tracked file, and `make liblogosdelivery` at `f8b0365` does not write one:
`git cat-file -e f8b0365:library/generated/logosdelivery.h` reports the path
"exists on disk, but not in" that commit in a checkout that has it, and a clean
clone at `f8b0365` built here has no `library/generated` at all. It is also not
needed: `logos-delivery-module` includes `<liblogosdelivery.h>` and nothing else
(`src/api_call_handler.h:14`, `src/delivery_module_plugin.cpp:18`), and its
`metadata.json` puts `lib` on the include path, not `lib/generated`. So the
header is staged when the library's build produced one and its absence is
reported and carried on from — and the claim that it was not needed is the
downstream one, which is the strong form: the module compiled, packaged, loaded,
and answered `getPluginMethods` across the transport without it.

### What the runtime prints

The companions are loaded **first**, deliberately: the claim is that the agent
works with them present, not that four modules each load into an empty runtime
one at a time.

```
<- known modules: wallet_module storage_module package_downloader
                  package_manager delivery_module capability_module agent
ok  logos_core_load_module('storage_module') reports success
ok  logos_core_load_module('delivery_module') reports success
ok  logos_core_load_module('wallet_module') reports success
ok  logos_core_load_module() reports success
<- loaded modules: wallet_module storage_module delivery_module
                   capability_module agent
ok  and so is 'storage_module' — one runtime, both modules
...
<- storage_module getPluginMethods(): 29 method(s) — init, start, stop, destroy,
                                                    version, moduleVersion, …
ok  'storage_module' answers across the runtime's transport with its own method
    table: it is running, not just loaded
<- delivery_module getPluginMethods(): 13 method(s) — createNode, start, stop,
                                                     send, subscribe, …
ok  'delivery_module' answers across the runtime's transport …
<- wallet_module getPluginMethods(): 19 method(s) — ethClientInit, ethClientClose,
                                                   ethClientGetClients, …
ok  'wallet_module' answers across the runtime's transport …
<- loaded modules, after the agent's skills were exercised:
   wallet_module storage_module delivery_module capability_module agent
ok  the agent module is still loaded after every skill was invoked
ok  and the agent still reports itself started and bound to … with 3 other
    module(s) loaded in the same runtime
all steps confirmed (0 failure(s))
```

### Why "loaded" is not the check

Because this document already contains the run where "loaded" was true and
nothing worked. The Qt section above records it: a plugin built against 6.11
makes `logos_core_load_module` return success and join the loaded set, and then
its `logos_host` is gone and every call spends twenty seconds on `Timeout
waiting for replica`. Four of the six companion checks would have passed for
that.

So each companion has to *answer*. `getPluginMethods` is framework-level in the
SDK — `qt_provider_object.cpp` and `module_proxy.cpp` both special-case it before
the call reaches the module's own dispatch — so it can be asked of a module this
repository knows nothing about, and only a live module process produces a
non-empty method table.

That check was watched failing before it was believed. `storage_module` rebuilt
against Homebrew's Qt 6.11.1 and dropped into the module directory in place of
the good one:

```
ok    logos_core_load_module('storage_module') reports success
ok    and so is 'storage_module' — one runtime, both modules
FAIL  'storage_module' answers across the runtime's transport with its own
      method table: it is running, not just loaded
FAIL  and 'storage_module' is still loaded beside it
```

with, in the runtime's log 250 ms earlier and surfaced to nobody:

```
[error] [logos] [storage_module] LogosModule: Failed to load plugin: …
  Error: "The plugin '…/storage_module_plugin.dylib' uses incompatible Qt
  library. (6.11.0) [release]"
```

`build-companion-modules.sh` refuses to package a plugin that got this wrong,
reading `Qt6Core_DIR` out of the CMake cache and the framework references out of
the binary, because CMake falls through to the system Qt without a word when it
cannot find the pinned one.

### "Without requiring modifications to those modules", checked

Both scripts run

```sh
git -C build-companions/src/<repo> status --porcelain --untracked-files=no
```

against each of the three checkouts and refuse if it prints anything — the same
mechanism `examples/agent-console/run.sh` uses on `module/` for the third-party
skill criterion. `build-companion-modules.sh` additionally requires every path
the build **added** to be under `lib/` or `generated_code/`, which are the two
directories the modules' own published build writes: `logos-module-builder`'s
`lib/modulePreConfigure.nix` stages external libraries into the first
(`copyExternalLibsToLib`) and writes `logos-cpp-generator`'s glue into the
second (`universalCodegen`). Nothing under `src/`, no `CMakeLists.txt` and no
`metadata.json` is touched.

Both halves were watched failing. A comment appended to
`storage_module_plugin.cpp`:

```
FAIL  storage_module: tracked files changed in …/logos-storage-module
       M src/storage_module_plugin.cpp
error: a companion module was modified — that is the criterion, not a detail
```

and an empty `PATCH_APPLIED.txt` dropped in the wallet checkout:

```
FAIL  wallet_module: the build left files outside lib/ and generated_code/:
      PATCH_APPLIED.txt
```

The green run reads:

```
ok    storage_module @ f6bfab3: no tracked file changed; 6 added path(s), all under lib/ or generated_code/
ok    delivery_module @ 3f0f2d8: no tracked file changed; 4 added path(s), all under lib/ or generated_code/
ok    wallet_module @ f6f9c16: no tracked file changed; 2 added path(s), all under lib/ or generated_code/
```

### What this does not claim

- **`darwin-arm64` only** — and, unlike the agent's own two packages, which now
  carry all three variants, no `linux-amd64` or `linux-arm64` variant of the
  companions is built. This bullet used to read "like everything else packaged
  here", which stopped being true when `module/agent.lgx` and
  `app/agent-ui.lgx` gained their Linux variants.
- **They are loaded and answering, not driven.** The criterion is about
  coexistence, and nothing here has the agent *call* `storage_module` or
  `wallet_module`. The agent's own `storage.*` skills reach a Logos Storage node
  the module opens for itself, which is a different node from the one
  `storage_module` runs — so this section still proves coexistence and not
  interoperation, and loading somebody else's storage module into the same
  runtime does not change that.
- **`logos-chat-module` is not built.** It is a Rust `cdylib` whose
  `metadata.json` declares `delivery_module` as its dependency, and
  `delivery_module` is the messaging module the criterion names, so it would add
  a second messaging module rather than a missing one.
- **The prerequisites are not small.** Qt 6.9.2, a `logos-cpp-sdk` checkout,
  `lgx`, Go, Nim, and a built Logos Storage library. Every one is checked by
  name before anything is built, the way `logos-core-headless.sh` checks its
  own; `docs/limitations.md` carries the list as prose.
