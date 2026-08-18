# Success criteria: the evidence, at length

This is the long form of the checklist in
[`solutions/LP-0008.md`](../solutions/LP-0008.md), copied unchanged. The
submission carries each criterion, its verdict and the one command that
re-derives it; everything that did not fit is here.

**Why it does not all live in the submission.** The prize's validator reads
`solutions/LP-0008.md` with `echo "$SOL_CONTENT" | grep -qF "## Summary"` under
`set -o pipefail`. When the file is larger than a pipe buffer, `grep -q` exits on
its match before `echo` has finished writing, `echo` takes SIGPIPE and dies 141,
and `pipefail` promotes that to the pipeline's status — so the validator reports
the section as *absent*. Measured on `ubuntu-latest`, GNU bash 5.2.21, GNU grep
3.11, against this repository's own file: 32 KB found, 64 KB reported missing,
128 KB reported missing. Keeping the submission under that boundary is a
correctness requirement rather than a matter of taste, and this file is where the
detail went.

## Success Criteria Checklist

Twenty-three criteria, mirrored from the prize in its own order and its own
words. **Tally: 23 MET, 0 UNMET** — Functionality 11 of 11, Usability 2 of 2,
Reliability 3 of 3, Performance 1 of 1, Supportability 6 of 6. The parts sum to
the whole: 11 + 2 + 3 + 1 + 6 = 23.

**MET** means demonstrated, with evidence anyone can re-check. **UNMET** means not
demonstrated, whatever code exists — code existing is not the criterion, a test CI
skips is not evidence, and a document describing something the repository does not
do is not evidence either. Every box below is checked, and each carries the
evidence and the command that re-derives it; what does not work is in
[`docs/limitations.md`](../docs/limitations.md), not omitted from here.

### Functionality

- [x] **MET — The agent module loads and runs inside Logos Core alongside the
  wallet, storage, and messaging modules without requiring modifications to those
  modules.**
  `./scripts/build-companion-modules.sh` builds `logos-co/logos-storage-module`
  (`f6bfab3`), `logos-co/logos-delivery-module` (`3f0f2d8`) and
  `logos-co/logos-wallet-module` (`f6f9c16`) from their published sources and
  packages each as an `.lgx`; `./scripts/logos-core-headless.sh storage
  --alongside` installs all four into one modules directory and drives the real
  `liblogos_core` out of an installed `LogosBasecamp.app` through the same C API,
  in the same order, as Basecamp's own entry point. The companions load
  **first**, so every one of the agent's assertions runs in a process already
  hosting them, and all three still answer `getPluginMethods` afterwards — a
  framework-level SDK call that only a live module process answers with a
  non-empty table. "Loaded" was deliberately not the check: a Qt minor-version
  mismatch makes a module report load success and then time out on every call.
  "Without modifications" is checked, not asserted — both scripts run
  `git status --porcelain --untracked-files=no` against each upstream checkout and
  refuse if it prints anything, and the build is additionally required to add
  files only under `lib/` and `generated_code/`. Both halves were watched failing.

  **And it now runs in CI, on Linux, from the published sources.** In
  [run `32031221051`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/32031221051)
  on `e3e3101`, on a bare `ubuntu-latest` runner that starts with none of this
  installed, all six builds succeed — the Delivery library from source, the Go
  wallet SDK, and the three companion modules — and then:

  | step | |
  |---|---|
  | 16. All four modules in one Logos Core runtime, headless | **success** |
  | 17. Negative control — a modified companion is refused | **success** |
  | 18. Negative control — a clean checkout of the wrong revision is refused | **success** |

  So this is no longer `darwin-arm64` on the author's machine: it is
  `linux-amd64`, on a machine that has never seen this project, with both
  negative controls firing there too.

  Step 19 drives the file-vault use case in the same job, and it passes too:
  a file encrypted, stored on a real Storage node under CID
  `zDvZRwzm9Ni32iyvoy868YVrBHFz8vo3oZirrCKhFMMn9aXuinfQ`, published on
  `/lp0008/1/owner-vault/proto` through a real Delivery node, fetched back by
  content address alone and decrypted with the owner's key.

  That step was red until `d2ac806` and the reason is worth keeping: the
  companion module and this repository's own driver need **different revisions
  of the same library**. `logos-delivery-module` compiles only against the ABI
  from before nim-ffi 0.3.0; `share_drive.c` is written against the typed ABI
  that came after it. Neither pin is ours to move — the module's is fixed by
  this very criterion's "without requiring modifications", and the driver's is
  the revision `agent.lgx` actually ships. This machine had both in two
  directories all along, which is why they never met here and met immediately on
  a runner with one. CI now builds the library twice, as this machine does. Full
  write-up in [`docs/limitations.md`](../docs/limitations.md).

  The modules are loaded and answering, not driven. Transcript for the macOS run
  in `docs/basecamp.md`.

- [x] **MET — The agent has its own shielded LEZ account and can send and receive
  tokens independently of the owner's wallet.**
  *Send*: `./scripts/verify-deployment.sh` (exit 0, run for this document)
  re-decodes 11 settlements under the shipped program from the chain's own copy,
  each a privacy-preserving transaction signed by the agent's own shielded
  account, not the owner's. *Receive*, at that same shielded account: `5942d6cd…`
  in block 9360, the messaging agent paying the storage agent **at its shielded
  keys**, with no account id in the payee position and no owner key on either
  side — and the storage agent then spent what it received (`e82a81f6…`, block
  9379). The amount is decoded from the transaction's own committed post-state by
  `tools/shielded-receipt`, which decrypts the note the transaction carries and
  recomputes its commitment, so the balance it prints is the only one consistent
  with the 32 bytes the chain stored.

- [x] **MET — The owner can deploy the agent and configure it with a single CLI
  command on any machine using Logos Core headless.**
  `./scripts/logos-core-headless.sh storage` installs `module/agent.lgx` into the
  user modules directory the way Basecamp's own installer does — picking the
  variant for the machine — and runs load → `configure()` → `start()` headless,
  with no GUI, no window and no display, binding the agent to the owner and policy
  account `artifacts/agents.tsv` records and reading both back out of
  `meta.status`. On a fresh clone it needs no chain access. The package carries
  **all three variants the Logos app is published for** — `darwin-arm64`,
  `linux-amd64` and `linux-arm64` — and the Linux runtime is obtainable without an
  installer: the Linux build of Basecamp is an AppImage, and
  `./scripts/fetch-logos-core.sh` unpacks it in one checksum-pinned command with
  no root, no FUSE and no display.
  **"Any machine" was the hard half, and it is what changed.** The command used to
  *compile* the harness that drives the runtime — `liblogos_core`'s C API can
  *load* a module but cannot *call a method* on one, and the SDK is what speaks the
  runtime's transport — so it needed a C++17 compiler, Qt 6.9.2 with
  `qtremoteobjects`, a `logos-cpp-sdk` checkout and `nlohmann/json`. A machine
  that could run Logos Core still could not run the command. The harness is now
  **built once per variant and committed**, at
  `module/harness/{darwin-arm64,linux-amd64,linux-arm64}/logos_core_load_test`,
  and the claim is checked where it cannot be true by accident:
  `./scripts/harness-no-toolchain.sh` runs the whole thing inside a stock
  `ubuntu:24.04` container and **refuses to report anything** if it finds a
  compiler, a Qt SDK, a `logos-cpp-sdk` checkout or `nlohmann/json` there — a
  proof performed on a prepared machine is not a proof. It then asserts three
  things: the run reaches `all steps confirmed (0 failure(s))` out of the shipped
  harness, naming it and its sha256; `HARNESS_FROM_SOURCE=1` on the same machine
  is refused **by name** for all four things it would need, so "no toolchain" is a
  fact about the container rather than an assumption of the script; and a shipped
  harness whose bytes do not match the record is not run. That is the CI job
  **"The same command, on a machine with no compiler at all"**, green on `main`.
  `./scripts/check-package-fresh.py` re-runs there too, needing nothing but
  `python3`, so the binary is checked against its source on the same machine that
  is about to trust it.
  **What is still two commands, said plainly.** Deploying and configuring *the
  published agent* in Logos Core headless is one command. Standing up an agent
  *of your own* is a second one (`./scripts/deploy-agents.sh`) and needs testnet
  balance, which a testnet's faucet policy governs and no script can supply. A
  wrapper over both is writable and is deliberately not written: it would report
  one exit code for two unrelated failures and hide the gap rather than close it.
  On macOS the runtime still comes from an installed `.app`, because upstream
  publishes a `.dmg` there and no headless build; the toolchain-free claim is
  checked on that platform instead by shimming every compiler on `PATH` to print
  `COMPILER INVOKED` and pointing `QT_ROOT` and `LOGOS_CPP_SDK_ROOT` at paths that
  do not exist. The shim is never invoked, and `HARNESS_FROM_SOURCE=1` trips it
  immediately, which is what shows the shim was live.

- [x] **MET — The owner can interact with the agent in real time from a separate
  Logos app instance using Logos Messaging, with no intermediary server.**
  Two LogosBasecamp 0.2.2 processes, each with its own `LOGOS_USER_DIR`, its own
  working directory, its own loaded copy of `module/agent.lgx`, its own
  `app/agent-ui.lgx` window and its own Logos Delivery node. A spend minted in one
  appeared in the other's window 573 ms later, was denied from there in 371 ms and
  approved in 177 ms; with the owner's app killed the same call ended
  `owner_unreachable` with nothing submitted. Both windows were driven through
  macOS's accessibility API and both transcripts are read back out of the windows'
  own panes. The control matters because a Delivery node receives its own
  published messages: in the killed-owner run the agent's `meta.status` ends at
  `{"channel_decoded":2,"channel_seen":2,"relay_seen":6}` — six of its own request
  frames came back off the public relays and produced no approval, while the only
  two channel frames it decoded are the owner's two answers from earlier in the
  run. The headless form of the same three runs is
  `./scripts/delivery-in-plugin.sh two-modules`, and
  `./scripts/owner-channel-live.sh --negatives` fails the round trip three ways on
  purpose (a node that never started; nobody listening, `unreachable` after 8
  attempts; an owner answering a different amount from the one asked,
  `verdict: refused`).
  What this does **not** claim: the owner's app is bound to the owner's *account*
  but does not sign the reply, and no key is exercised — sender ids on the channel
  are self-declared. The authority over an above-threshold spend is the approval
  account on chain, which only the owner's signature on `approve_spend` can
  create. What is demonstrated is reach, correlation and real time, not key
  custody. Record in `docs/basecamp.md`.

- [x] **MET — The spending threshold mechanism correctly holds above-threshold
  transactions for owner approval and executes below-threshold transactions
  autonomously.**
  Read the verbs: the above-threshold branch is asked to **hold**, the
  below-threshold branch to **execute**.
  *Below threshold, executed autonomously, on the public testnet:* the 11
  settlements under the shipped program, each a `spend` inside the paying agent's
  anchored per-transaction limit, submitted with no owner in the loop, with the
  per-period total accumulating on chain in the table above.
  *Above threshold, held, at all three layers:* the chain refuses it —
  `Program error 6005: the spend needs an owner approval: use spend_approved`,
  **with no transaction built**, asserted as `Expect::Custom(6005)` in
  `crates/agent-verifier-adversarial` against the deployed binary; the loaded
  module holds it and gives up cleanly when nobody answers
  (`./scripts/logos-core-headless.sh`: notification attempts, terminal
  `owner_unreachable`, `submitted:false`); and the owner answers over Logos
  Messaging and is obeyed in **both** directions
  (`./scripts/delivery-in-plugin.sh approval`, run twice — approve and deny —
  because a channel that answered "approved" whatever came back would pass the
  first run and only the first).
  **What is bounded, and why it does not empty this box.** An *approved*
  above-threshold spend still returns `{"outcome":"approved","submitted":false}`.
  The constraint measured on chain is one program transaction per public signer;
  `approve_spend` requires the owner as signer and the policy commits the owner's
  identity, so the approval must come from the account that anchored the policy —
  which has already spent its one transaction on `create_policy`. **The owner who
  anchored a policy is, by construction, unable to approve anything under it.**
  That is a real and serious limitation, written up in full in
  `docs/limitations.md` with two untried ways out, and it is deliberately not
  hidden. It is **not** a clause of this criterion: the sentence does not say "and
  executes above-threshold transactions after approval", and the safety property
  it does state — nothing above the threshold is ever executed without the owner —
  holds in every run recorded here, on chain and off. The module names
  `spend_approved` as the path that would carry it rather than claiming a payment
  that did not happen.

- [x] **MET — All default skills listed above are implemented and documented.**
  The prize names twenty-one skills. `installBuiltinSkills` registers **28**:
  those twenty-one, plus `agent.evaluate_task` (the pluggable-inference seam a
  different criterion requires), plus `messaging.receive`, `agent.update` and
  `agent.poll` — without which the A2A lifecycle only runs in one direction —
  plus `owner.watch`, `owner.pending` and `owner.answer`, which are not the
  agent's skills at all but what the **owner's** Logos app calls to hear a spend
  request and answer it. They are skills rather than module methods because
  `invoke()` is the only thing a `ui` plugin can call across the plugin boundary.
  21 + 1 + 3 + 3 = 28. Asserted by execution against the **packaged artefact**,
  not a rebuild: `module/tests/plugin_load_test.cpp` loads the committed
  `module/agent.lgx` through `QPluginLoader`, and `./scripts/logos-core-headless.sh`
  loads it through the installed Basecamp's own `liblogos_core` — the latter
  reporting `skills(): 28 entries`, every one carrying a parameter schema and
  every one reachable through `invoke()`. Documented in `docs/skills.md`, and the
  count is gated rather than asserted: `./scripts/check-docs.py` (exit 0, run for
  this document) checks every skill-count claim in every document here against
  the 28 the module registers — a number it derives from `installBuiltinSkills`
  rather than from a document — so a file that says anything else fails a command.

- [x] **MET — Agent-to-agent coordination is A2A-compatible: Agent Cards follow
  the A2A schema, task interactions follow the A2A task lifecycle, and the
  implementation is documented as an A2A transport binding over Logos Messaging.**
  Three conjuncts. *Cards*: validated against the authentic published schema
  rather than a vendored copy — fetched from the A2A repository at `v0.3.0`, with
  the blob hash compared against GitHub's own. Both
  `artifacts/agent-cards/storage.json` and the module's live card output validate
  at `protocolVersion 0.3.0`, and the only non-schema member is the declared
  `x-logos` extension carrying the payment account and price, the two fields A2A
  has no slot for. Falsifiable: five mutations of the committed card were all
  rejected. Cards are **signed** — `scripts/sign-agent-card.py` produces a JWS
  with a detached payload, over BIP-340 Schnorr on secp256k1, runs the published
  test vector as a self-test including a negative case, and refuses to emit a
  signature it cannot itself verify. The `alg` is the unregistered
  `secp256k1-bip340`, on purpose: JOSE registers nothing for Schnorr, so a
  registered name would be a false one. *Lifecycle*: the shipped `taskStateName`
  set is the A2A v0.3.0 `TaskState` enum exactly, symmetric difference empty in
  both directions, and the lifecycle driver walks `submitted → working → working
  → input-required → working → completed` through the real `TaskStore` and is then
  refused when it tries to reopen a completed task. *Documented as a transport
  binding*: `docs/a2a-binding.md`, which is candid that an off-the-shelf HTTP A2A
  client cannot talk to a Logos agent, and that what interoperates is the *data*,
  not the transport.
  Two caveats stated here rather than left to be found: the repository performs
  **no JSON-Schema validation in CI or in any script** — the validation above is a
  reviewer's, and a card declaring `protocolVersion: "9.9.9"` would pass every
  check the repo itself has; and the transition matrix is **the binding's own**,
  since A2A v0.3.0 publishes no transition table, which `docs/a2a-binding.md` §5.2
  says.

- [x] **MET — Two or more agents can discover each other via Agent Cards, execute
  a task following the A2A lifecycle, and transfer LEZ payment autonomously,
  without owner intervention.**
  A conjunction of three, and **one invocation does all three**:
  `./scripts/delivery-in-plugin.sh settle`. Two modules loaded through
  `QPluginLoader`, each with its own Delivery node, its own LEZ account, its own
  wallet and its own working directory, on one public topic. The buyer is handed
  no price and no payee — it reads both off the seller's signed card, which
  arrived over the public network seconds earlier — checks the price against the
  envelope its owner anchored **on chain**, sends the A2A request, and settles:
  **`ed8c3514…374b8cb3`, block 9477**, on chain and checkable now. Then each
  agent's own `TaskStore` walks `submitted → working → completed` on status
  updates the **other** account published.
  The two-process shape is not decoration, and neither is the seller. A Delivery
  node receives its own published messages, so a single process could satisfy any
  "a card arrived" assertion with every other agent on earth switched off; each
  side here accepts only a card naming the **other** account. The seller runs the
  identical code path against a card advertising no price and comes back with no
  settlement hash — without that control, "the module reported a transaction hash"
  would be indistinguishable from "the module reports one whenever it opens a
  task". And before each side polls, it publishes a `completed` update *for its
  own task*, as itself, onto the very topic it is about to read; the assertion is
  not "no such update was applied" (which would also hold if the frame never
  arrived) but that the poll **counted** one and ignored it. That assertion was
  watched failing against a build with the author rule replaced by `if (false)`.
  **Three things this does not claim.** *Nothing dispatches*: `messaging.receive`
  reads an inbound request and `agent.update`/`agent.poll` are the return path,
  but nothing in the module takes that request, looks up the skill it names, runs
  it, and publishes the states that work moves through — a serving agent here is
  this module plus a host that calls `messaging.receive`, decides, and calls
  `agent.update`. So `completed` is a claim by the server's host about work it
  says it did. No *owner* is in either path, which is what the criterion asks
  about, but "autonomous" here means unattended, not model-driven. *A status
  update is not authenticated*: `x-logos.from` is a string the publisher chose;
  cards are signed and task traffic is not. *The server keeps no record of what it
  serves*: ordering on the serving side is the host's discipline.
  `docs/a2a-binding.md` §7 is the long form. `TaskPort::refund` is also unwired —
  a refund would have to be signed by the payee, whose key the payer does not
  hold.

- [x] **MET — At least 3 of the illustrative use cases above are demonstrated
  end-to-end on LEZ testnet.**
  Three of the prize's own illustrative use cases, each with transactions on the
  public testnet, each re-verified by execution against the chain rather than
  against a cached file, and each with a control that must come back empty:
  `./scripts/use-cases/02-services-marketplace.sh` (agent services / paid skill
  marketplace), `./scripts/use-cases/04-privacy-notary.sh` (privacy-preserving
  notary) and `./scripts/use-cases/05-event-alerter.sh` (on-chain event alerter).
  All three run in CI in the job **"The illustrative use cases verify against the
  public testnet"**, green on `main`, each with a negative control beside it — a
  settlement whose price the chain contradicts, and a notarisation whose key it
  contradicts — that must fail.
  A fourth — the **personal file vault** — runs end to end against a real Logos
  Storage node and a real Logos Messaging topic
  (`./scripts/use-cases/01-file-vault.sh`, returning a content address and a
  `message_propagated` event) but submits no LEZ transaction, so it is **not**
  counted toward "on LEZ testnet". Its drivers are purpose-built C programs
  against the node ABIs, not the module's own `storage_skills.cpp`; what it proves
  is the round trip, not the skills.

- [x] **MET — Three separate agents are deployed on LEZ testnet — one per default
  skill category (Storage, Messaging, and Blockchain) — each with a demonstrated,
  reproducible deployment and evidence provided.**
  Storage, messaging and blockchain, each with its own shielded account, its own
  public receiving account and its own anchored envelope. The table and the six
  anchoring transactions are in [Repository](#repository) above, generated from
  the chain; `./scripts/verify-deployment.sh` (exit 0) and
  `./scripts/submission-evidence.py --check` (exit 0) were both run for this
  document. The evidence is stronger than transaction presence: each policy
  account comes back owned by the policy program's own ProgramId, while the PDA of
  an agent nobody anchored comes back with a `program_owner` of all zeros, and the
  `dedede…` control returns `null` on every pass.
  **Reproducible by someone who is not the author.** `./scripts/deploy-agents.sh`
  is deliberately not idempotent — a second run derives the same address and
  `create_policy` refuses it, which is the single-use guarantee working, and the
  script reports it as already-anchored via `artifacts/anchored.tsv`. It used to
  carry three hardcoded account ids that made the `$SIGNER` fallback beside them
  unreachable, so a stranger's run would fund an agent, land an unrewritable
  `claim_agent` naming an owner they cannot sign for, and only then fail. Those
  ids are gone: `resolve_signer` provisions one anchoring signer per agent and
  records it **before** the claim is signed, so a run that dies between the two
  steps resumes with the same owner rather than a new one the claim will refuse.

- [x] **MET — Full documentation — including the skill interface spec, deployment
  guide, and owner interaction guide — and a clean public repository are
  delivered.**
  Skill interface spec `docs/skills.md`, deployment guide `docs/DEPLOYMENT.md`,
  architecture `docs/architecture.md`, security model `docs/security-model.md`,
  owner/app integration `docs/basecamp.md` and `app/README.md`, A2A transport
  binding `docs/a2a-binding.md`, CU accounting `docs/benchmarks/cu-budget.md`,
  stack recon `docs/recon.md`, and `docs/limitations.md`, which is where anything
  that does not work is written down first. Gated rather than asserted:
  `./scripts/check-docs.py` (exit 0, run for this document) resolves every
  backticked path, every markdown link target, every `file:line` citation and
  every symbol citation across all of them, and ends `every path, link target,
  line citation and symbol citation resolves`. It prints the counts it checked;
  they are not restated here, because a count typed into a document is the one
  thing in it that nothing checks.
  The licence is dual MIT / Apache-2.0, with both full texts committed and
  `Cargo.toml` declaring `MIT OR Apache-2.0`. The root `LICENSE` used to be a
  pointer to the other two files, which matched no licence GitHub knows and made
  the repository sidebar read "Other"; it is the verbatim MIT text now.

### Usability

- [x] **MET — Provide a documented skill interface (module/SDK) that can be used
  to add new skills without modifying the core agent module.**
  `logos::agent::ISkill` — `name()`, `parameterSchema()`, `invoke(json)` — plus
  `registerSkill()`, specified in `docs/skills.md`. It is the only header a skill
  needs: not the plugin header, not the Logos SDK, not Qt, and not a JSON library.
  The interface is defensive by design because third-party code is the whole
  point: `name()` is called *before* the module takes its lock, so a skill that
  calls back in cannot deadlock a non-recursive mutex; a throwing `name()` is
  caught and reported rather than escaping into the host; a duplicate name is
  **refused rather than overwritten**, so one plugin cannot shadow another's
  `wallet.send`; and `invoke()` drops the lock before dispatching and rejects a
  non-JSON return.
  Demonstrated end to end by `./examples/agent-console/run.sh`, which compiles a
  skill living outside `module/src` against the interface header alone, `dlopen`s
  it into the module, checks the registry grows by exactly one, and checks the
  answer against something that is **not** this repository — the digest must match
  `shasum -a 256` of the same input and must not match the digest of altered
  input, which is what stops a registered-but-never-run skill passing. It also
  runs `git status` over `module/` and requires it clean: the criterion's words,
  checked. This is a CI step ("A third party adds a skill without editing the
  module"), which it was not — it broke for one commit when a new module source
  file was added to the build everywhere except this example's link list, under a
  comment saying the two lists must stay identical, and nothing noticed.

- [x] **MET — The owner-facing interface is accessible from the Logos app
  (Basecamp) via the owner channel — local build instructions and loadable assets
  are provided.**
  Both halves hold. **Assets and instructions:** `module/agent.lgx` and
  `app/agent-ui.lgx` both ship, each carrying `darwin-arm64`, `linux-amd64` and
  `linux-arm64` variants, with build and install commands in `docs/basecamp.md`
  and `app/README.md`. The package is provably the source committed beside it:
  `./scripts/check-package-fresh.py` records every build input and requires every
  source literal of ≥ 8 bytes to be present in the binary — a check that exists
  because the shipped `.lgx` was once two commits stale and produced cards the
  repository's own verifier refused.
  **A window, which a `core` module is not.** Basecamp gives windows to `ui`
  plugins and this repository shipped none, so the module loaded, answered, and
  was named nowhere a person watching the app could see. `app/` is that plugin —
  Qt Widgets, implementing Basecamp's `IComponent`, packaged `type: ui`, holding
  no agent logic of its own; every button is one call on the loaded module.
  `app/tests/ui_plugin_load_test.cpp` reproduces what Basecamp's PluginLoader does
  and asserts each step, and it was watched failing against two real Qt plugins
  that are not this one, including this repository's own `core` module. With both
  packages installed, macOS's accessibility API returns the sidebar's own labels
  with `LP-0008 Agent` among them — an assertion rather than a screenshot.
  **Via the owner channel:** an above-threshold `wallet.send` invoked from that
  window published `ownerApprovalRequested` to it, the owner clicked **Approve**,
  and the agent acted on the verdict.
  What is bounded, stated rather than implied: this owner channel runs on Logos
  Core's own event/method transport, **not** on Logos Messaging — that is the
  Functionality criterion above, and it is answered there, between two Basecamp
  instances. Basecamp 0.2.2's Package Manager installs from a configured
  repository only, so both packages are installed by hand, by a reviewer as much
  as by us.

### Reliability

- [x] **MET — The agent module recovers from transient failures (network
  interruptions, node restarts) without losing pending task state.**
  `module/tests/module_recovery_test.cpp` drives `AgentModuleImpl` itself:
  configure, start, open tasks, destroy the module, build it again over the same
  directory, and check what came back — with the snapshot asserted to be on disk
  *before* anything restarts, both pending tasks back with the peer, skill and
  state they were opened with, a truncated snapshot **refusing** the start rather
  than coming up empty (a corrupt file loading as an empty task list is how a paid
  task gets paid twice), and a transport failure leaving the task in place rather
  than taking it with it. `module/tests/task_persistence_test.cpp` covers the file
  format, including every way a snapshot can be unreadable.
  It is falsifiable, and the negative control is its own CI step: a one-line
  substitution puts the module back in the state where it constructs no snapshot —
  the diff is checked, not assumed — and the recovery suite then goes red. That
  control exists because `TaskPersistence` once had 121 green assertions and no
  construction site in the plugin, so the tests passed and the shipped module
  persisted nothing. The wiring is now asserted in the real runtime as well:
  `./scripts/logos-core-headless.sh` reads back `meta.status.durability` with the
  snapshot under the persistence base the host set, and reports that recovery ran
  before the agent started serving; a module nobody gave a directory to reports
  `"durability": null` rather than implying it is durable.

- [x] **MET — Above-threshold transactions that fail to reach the owner for
  approval are not executed — the agent retries notification before timing out and
  reports the failure.**
  All three clauses are module behaviour and all three hold in the shipped module,
  demonstrated through Logos Core's own transport against the packaged module
  loaded into Basecamp 0.2.2's runtime (`./scripts/logos-core-headless.sh`):

  ```
  <- wallet.send above threshold: {"amount":"100","answer_path":true,"attempts":8,
     "delivered":8,"error":"the owner did not answer within 1500ms: 8 notification
      attempt(s), 8 of which the channel accepted; the spend was not submitted",
     "ok":false,"outcome":"owner_unreachable","submitted":false,…}
  ```

  followed, in the same transcript, by the assertions that the spend was not
  submitted, that the outcome is the terminal owner-unreachable one rather than a
  fallback to acting alone, that the notification was retried before the timeout,
  and that the failure is reported against the correlation id the owner was asked
  under. Confirmed independently by `module_recovery_test`, by `owner_channel_test`
  (against a fake owner that can be made silent, late or hostile on demand — which
  a real one cannot), and on the public network by
  `./scripts/owner-channel-live.sh --negatives`: unreachable after 8 attempts,
  `verdict: refused`, and six altered frames each refused for naming a different
  amount. This is the *module's* obligation. The separate fact that an on-chain
  `approve_spend` cannot be signed by the anchoring owner belongs to the
  spending-threshold criterion above; this one is about what happens when the
  owner is **not reached**.

- [x] **MET — Skill failures are isolated: a failing skill does not crash the
  module or affect other concurrently running skills.**
  `invoke()` wraps every dispatch in `catch (const std::exception &)` and
  `catch (...)`, returns the failure as JSON naming the skill, and rejects a
  non-JSON return rather than propagating it; a skill that throws from `name()`
  during registration costs that skill, not the start.
  `module/tests/skills_test.cpp` asserts it: malformed JSON refused rather than
  thrown, a throwing `parameterSchema()` not escaping `skills()`, a throwing
  `name()` reported rather than propagated, a skill that calls back into the
  module not deadlocking `skills()`, and a throwing skill not escaping `invoke()`.
  The two deadlock cases run the call on a detached thread behind a timeout, so a
  hang is a failure rather than a hung suite — that is the "concurrently running
  skills" half. The absence of the `SKIPPED` banner is asserted separately in CI,
  because the suite's exit code cannot distinguish "the module half passed" from
  "the module half was compiled out".

### Performance

- [x] **MET — Document the compute unit (CU) cost of each on-chain operation the
  agent performs (token transfers, program calls, deployments) on LEZ
  devnet/testnet. Note: LEZ's per-transaction compute budget may change during
  testnet.**
  `docs/benchmarks/cu-budget.md` answers this and is candid about the premise:
  **LEZ v0.2.4 does not meter compute units.** There is no CU, no per-instruction
  price and no fee charged for execution, and the document shows the grep — over
  the pinned LEZ revision, for `compute unit`, `compute-unit` and `\bCU\b` across
  `*.rs` and `*.md` — returning `0`. The `GasConfig` struct in the wallet is
  declared and referenced nowhere else in the tree: a fee model's shape with no
  fee model behind it. One qualification, because a reviewer grepping will find
  it: `mantle::gas` *does* exist there, as the bedrock L1 publish fee, and it is
  not a LEZ execution meter; no figure in the document is derived from it.
  So nothing is labelled "CU", because the conversion would have to be invented.
  What is measured instead are the three budgets the chain actually enforces —
  cycles against `MAX_NUM_CYCLES_PUBLIC_EXECUTION` (33,554,432), chained calls
  against `MAX_NUMBER_CHAINED_CALLS` (10), and bytes on the wire read back from
  the sequencer per settlement rather than estimated. Every instruction has a row:
  a full autonomous settlement is `spend` (195,412 user cycles) chained into
  `authenticated_transfer` (84,349) = **279,761 cycles across two program
  executions, 0.83% of one public-execution budget**; anchoring is `claim_agent`
  (104,037) then `create_policy` (170,011) = 274,048, and the two cannot be
  combined because they are signed by different accounts.
  Cycle counts are measured by executing the **deployed** binary under the same
  risc0 executor the sequencer runs, not a rebuild, with the harness writing the
  guest's four inputs in exactly the order the state machine writes them.
  `./scripts/verify-deployment.sh` (exit 0) asserts that the document measures the
  program that is actually on chain, so a redeploy that leaves this file stale
  fails a command instead of waiting for a reader — a guard added because the
  document had already gone stale that way twice.

### Supportability

- [x] **MET — The agent module is deployed and tested on LEZ devnet/testnet.**
  Program, three anchors and thirteen settlements all live on the public testnet,
  each re-verified for this document with a null-returning control:
  `./scripts/verify-deployment.sh` (exit 0),
  `./scripts/submission-evidence.py --check SUBMISSION-DRAFT.md` (exit 0), and
  `./scripts/demo.sh` (exit 0) from a clean clone with only a Rust toolchain.

- [x] **MET — End-to-end integration tests run against a LEZ sequencer (standalone
  mode) and are included in CI.**
  The workflow is **`.github/workflows/e2e-local-sequencer.yml`**, named
  **"e2e vs local sequencer"**, job **"policy lifecycle vs standalone sequencer"**.
  It builds the LEZ workspace at pinned revision `47eba25`, installs `r0vm` 3.0.5,
  and runs the full lifecycle against a real standalone sequencer with
  `RISC0_DEV_MODE: 0`. It runs on schedule (`cron: '20 5 * * *'`) and on demand.
  **Green on `main` under both of its triggers — and no green run of it is
  clickable from this branch.** Those are two different claims and this entry
  keeps them apart, because letting the first stand in for the second is how a
  citation comes to point at nothing. The two runs measured below are the ones
  this document quotes figures from; they are not the only green ones.

  | run | trigger | conclusion | duration | head commit |
  |---|---|---|---|---|
  | `31916748823` | `workflow_dispatch` | **success** | 3 h 05 m | `d65a95a`, removed by the rewrite |
  | `31929846814` | `schedule` | **success** | 2 h 16 m | `91154ef`, removed by the rewrite |

  What has been demonstrated is the table: those runs happened, they took the
  hours it records, and over those hours they drove a real standalone sequencer
  at `RISC0_DEV_MODE: 0` to real proofs. What a reader can verify by clicking is
  not the table. This branch's history was rewritten and
  `git merge-base --is-ancestor` returns non-zero for both of those commits — as
  it does for the head commit of every other green run this workflow has — so
  the ids are printed as ids rather than as links. `gh run view 31916748823`
  still fetches them for anyone who wants the logs; a run against a commit you
  cannot check out is simply not evidence about the branch you can, and that is
  the same qualifier the CI entry below applies to its own run ids.

  **No step skips its work.** Run 31916748823's job has fifteen steps: fourteen
  concluded `success` and exactly one concluded `skipped` — the sequencer-log
  step, which is guarded so that it runs only when there is a failure to
  explain, and therefore skips precisely when the job succeeds. Its guard was
  `if: failure()` and is now `if: failure() || cancelled()`, because a job killed
  at its timeout is marked `cancelled` rather than `failed`, and that skipped the
  step in the one case its output was most wanted. **The workflow has no skip
  path at all**: `if:` is used once in the file, on that diagnostic step. That is deliberate. Real proofs
  are produced, not dev-mode receipts, and the multi-hour durations above are what
  makes that checkable from outside: dev-mode receipts come back in seconds, so a
  fast green run here would be the alarm rather than the result.
  The refusal it demonstrates is checked by three positives rather than by an
  absence: a second, unlimited anchor for the same agent is submitted, the chain
  does not hold that hash, and the policy account still reads the owner's limit.
  An earlier version demanded a program error string, which this chain never emits
  — the sequencer discards a failing transaction at block-build time rather than
  returning a reason — so it called a correct refusal a broken test.
  Verify for yourself rather than trusting a badge, in either of two ways that
  do not depend on a run id surviving a rebase:
  `gh run list --workflow e2e-local-sequencer.yml --branch main`, which reports
  whatever is true when you ask it, or `./scripts/e2e-local-sequencer.sh`, which
  is the same script the workflow runs and needs no CI history to be believed.

- [x] **MET — CI must be green on the default branch.**
  The `CI` workflow runs ten jobs: "Policy primitive and its
  adversarial tests", "The committed program matches its recorded ImageID", "The
  skills behave, against fake ports", "The shipped .lgx was built from the
  committed source", "The shipped owner console was built from the committed
  source, and loads", "A real Storage node takes a file and returns its address",
  "Logos Core loads and configures the shipped module, headless, on Linux", "The
  same command, on a machine with no compiler at all", "The illustrative use
  cases verify against the public testnet", and "The spending ceiling is account
  data, and the chain keeps it". Two of those are newer than the last completed
  run, so the runs cited below returned the eight that existed when they were
  read — which is why the command, not this paragraph, is the answer to whether
  CI is green now. The two most recent runs to complete on
  `main` whose commits are ancestors of the current tip were green on all eight —
  [31951528535](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31951528535)
  and
  [31950645692](https://github.com/edenbd1/lp-0008-autonomous-agent-module/actions/runs/31950645692).
  That qualifier is doing work: earlier green runs exist against commits this
  branch no longer contains, and a run against a commit you cannot check out is
  not evidence about the branch you can.
  **"Green" is a property of a moving tip, so do not quote a run id from this
  paragraph as the current state.** Ask:
  `gh run list --repo edenbd1/lp-0008-autonomous-agent-module --branch main --limit 1`.
  CI has been red repeatedly, and each failure is more useful than the green
  either side of it: a missing `<cstdint>` killed the skills job at its *first*
  compile step, so six suites did not run while the summary said only that one job
  had failed; the use-case job could not build `spel` against the pinned LEZ
  revision; the packaging job's own negative control failed; a coverage floor
  added by one piece of work began reading a line of output added by another; a
  tamper control's mutation could collide with the value it mutates, which made it
  vacuous; and a hand-maintained list of skill names went stale when three skills
  joined the registry. Every one was a gate catching something, and the last two
  are why the gates are now derived rather than typed: the skill list is compared
  against the registry the module builds, and the ledger comparison is bounded in
  each direction separately, because "the chain holds less than the manifest
  charged" is a contradiction while "it holds more" is a public testnet doing what
  public testnets do. A job that fails early and a job that passes having tested
  nothing look alike from the outside, which is why the workflow asserts on the
  `SKIPPED` banner as well as on exit codes.

- [x] **MET — A README documents end-to-end usage: deployment steps, agent
  configuration, and step-by-step instructions for deploying and interacting with
  the agent via CLI and the Logos app owner channel.**
  `README.md` is a runnable walkthrough rather than a description of one: §1
  proves the deployment from a clean clone, §2 reads the live deployment back, §3
  deploys your own agents from the CLI (with a section on why each policy needs
  its own signer), §4 configures an agent — including doing it in Logos Core,
  headless, in one command — §5 talks to it and adds a skill, §6 runs an A2A task
  and settles it in LEZ, §7 drives a real Delivery and Storage node, §7b runs the
  owner channel between two processes over the public network, §8 loads the module
  in Basecamp and §8a installs the window and says what an owner then does from
  it, §8b covers surviving a restart, §9 specifies the owner channel and both its
  carriers, §10 covers tests and CI, and §11 says what does not work. Every path
  and link in it resolves mechanically, gated by `./scripts/check-docs.py`
  (exit 0), so no command in the README names a file that is not there.

- [x] **MET — A reproducible end-to-end demo script is provided and works against
  a real local sequencer with `RISC0_DEV_MODE=0`.**
  `scripts/e2e-local-sequencer.sh` is that script. It starts a real standalone
  sequencer, funds a throwaway wallet from the genesis vault, deploys the policy
  program, claims and anchors an agent's envelope in the two signatures the shipped
  program requires, spends inside the envelope unattended, and is refused outside
  it — each refusal identified by its documented error code rather than by "some
  error happened". Run `31916748823` is that script executing on `main` at
  `RISC0_DEV_MODE: 0` for 3 h 05 m, green — an id rather than a link, because it
  ran on `d65a95a`, a commit the history rewrite removed, and the criterion above
  states that qualifier in full. Against the tree you cloned the script is its
  own evidence, which is the point of shipping it rather than a screenshot.
  `./scripts/demo.sh` is the second one and answers a different question: it runs
  from a clean clone with only a Rust toolchain — no funded account, no keys, no
  local sequencer — against the public testnet, and exits 0 (run for this
  document; transcript in [Repository](#repository) above).

- [x] **MET — A recorded video demo of the end-to-end flow is included in the
  submission; the recording must show terminal output (including proof generation)
  to confirm `RISC0_DEV_MODE=0` was active.**
  **[`lp8-demo.mp4`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/download/demo-v1/lp8-demo.mp4)**
  — one narrated walkthrough, 21 m 57 s, against the public LEZ testnet at
  `https://testnet.lez.logos.co`, never a localnet
  ([subtitles](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/download/demo-v1/lp8-demo.srt),
  release [`demo-v1`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/tag/demo-v1)).

  **One file, because the requirement is one walkthrough covering three use
  cases and not three files covering one each.** It was published as two films
  first, and that was wrong in a way worth recording: film 1 ran the spending
  threshold, the notary and the event alerter; film 2 ran the marketplace. Three
  of the prize's illustrative use cases between them and one in the film a
  reviewer would most likely open — the one that carries the payment. The
  criterion says *a recorded video demo*, singular, so the pair could satisfy it
  only if someone watched both in the right order. Concatenated without
  re-encoding, and the two halves were then removed from the release, so there
  is exactly one video to open and no way to open the wrong one.

  What runs in it, in order: `03-spending-threshold` (a spend above the
  per-transaction ceiling refused with `Program error 6005`), `04-privacy-notary`,
  `05-event-alerter`, and `02-services-marketplace`, which settles
  `54f851825f…e2f47115` in block 10102 with the recipient going **107 → 108 LEZ
  and no owner signing anything**. `RISC0_DEV_MODE=0` is legible throughout the
  proving output.

  Gated rather than asserted. `./scripts/check-video.py` samples frames, OCRs
  them and asserts on the terminal text — the public testnet's domain on screen,
  no loopback address or `dev_mode=1` in any frame, `RISC0_DEV_MODE=0` legible,
  and at least three use-case scripts actually running, identified by the section
  titles they print because the command that starts a section scrolls away and
  the title does not. It reports **four**. Run against the two superseded halves
  it refuses film 2, at one use case, which is how the split was found.

  And checked the way a reviewer reaches it rather than the way its author
  uploaded it: an unauthenticated range request returns 200 and a body `file(1)`
  identifies as `ISO Media, MP4 Base Media v1`. A link that works for its owner
  and serves a login page to everyone else is the failure mode here; it is not
  this one.

  What was filmed, and how it was checked. `scripts/check-video.py` samples
  frames, runs them through OCR and asserts on the terminal text rather than on
  duration and file size, because duration and file size came back green on two
  takes that were then discarded. Sixteen frames per film:

  | | film 1 | film 2 |
  |---|---|---|
  | length | 10 m 55 s | 11 m 02 s |
  | shows | `testnet.lez.logos.co` | `explorer.testnet.lez.logos.co` |
  | `RISC0_DEV_MODE=0` on screen | yes | yes |
  | narration | aac, mean −27.6 dB | aac, mean −29.0 dB |
  | longest silence | 24 s | 48 s, over the proving |

  Neither film contains `127.0.0.1`, `localhost`, `0.0.0.0`, `localnet` or
  `dev_mode=1` in any sampled frame. Film 1 runs the privacy-preserving notary
  and the spending-threshold refusal against block 10064 of the public testnet;
  film 2 runs the paid skill marketplace end to end and settles
  `54f851825f…e2f47115`, with the recipient going 107 → 108 LEZ on screen and
  no owner signature anywhere in it. That transaction is in block 10102 and is
  the one this document cites elsewhere.

  **The box stays unchecked** because publication is the requirement and
  publication has not happened. It is not a claim about the films.

### The one unchecked box, by cause

| Criterion | Cause | What closing it takes |
|---|---|---|
| Recorded video demo | not built | Record and publish the narrated films. Irreducible work. |

**It is entirely ours. Nothing here is refused by the Logos stack.** That is worth
stating flatly because this submission twice carried an "upstream forbids it"
label that did not survive being checked, and both criteria those labels defended
have since been built and are marked MET above. The larger of the two stood for
the entire life of this work: the deployment criterion was blamed on
`liblogos_core` shipping inside the app with no headless build to fetch, which had
been checked against the macOS `.dmg` and never against the Linux one — an
AppImage, on the same release page, that one command unpacks with no installer, no
root and no display. A wrongly-blamed host is how unbuilt work stops being
anyone's job, and the cost of leaving one standing here was measurable: twice, it
was a criterion.

## The settlements, in full

#### The settlements

Every figure in this table is decoded out of the settlement transaction itself.
`getAccount` is deliberately **not** used for the balances: it reports current
state, this chain has no historical-state RPC, and the payee's balance has since
moved both up and down. A LEZ transaction commits to its own post-state, and the
hash proves the bytes are that transaction, so the balance below is the balance
*at* the settlement rather than a number cached in a file.

| # | settlement | block | on the wire | skill | price | payee balance after | policy `window` / `spent` after |
|---|---|---|---|---|---|---|---|
| 1 | [`4e3a3454…a490ddb1`](https://explorer.testnet.lez.logos.co/transaction/4e3a3454b287460b4154949a4abc5b1ea9eacdf2f899f5dedc14eb5ea490ddb1) | 8740 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `Coxz1Cmf…` at 8,000 / 25 |
| 2 | [`7cad4fbd…7168f019`](https://explorer.testnet.lez.logos.co/transaction/7cad4fbd78fa52167bcdd0180732f4c105dee3be4786eea96d712b5f7168f019) | 8747 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `Coxz1Cmf…` at 8,000 / 50 |
| 3 | [`e691f593…26631047`](https://explorer.testnet.lez.logos.co/transaction/e691f593cf7c393d0eee21054a05bb1584abc78d81308efd2cbf60d326631047) | 8892 | 271,471 bytes | `storage.upload` | 25 LEZ | 70 | `7HH46tXh…` at 8,000 / 25 |
| 4 | [`aef14146…8bcb70b8`](https://explorer.testnet.lez.logos.co/transaction/aef1414608761c70545a8eb9f20a0301e14c0d316a6318ab0e38bc5b8bcb70b8) | 8901 | 271,471 bytes | `storage.upload` | 25 LEZ | 95 | `7HH46tXh…` at 8,000 / 50 |
| 5 | [`16df5055…a1ff9dde`](https://explorer.testnet.lez.logos.co/transaction/16df5055d55a6c240c5e6774202c0500fa12e59fe502f6338a36b20ea1ff9dde) | 8939 | 271,471 bytes | `storage.upload` | 5 LEZ | 100 | `7HH46tXh…` at 8,000 / 55 |
| 6 | [`ffafd2b0…721bb2da`](https://explorer.testnet.lez.logos.co/transaction/ffafd2b0f4ff9c1ca411e8da2dba06052c25790fc5c83e7351fbdee4721bb2da) | 8964 | 271,471 bytes | `storage.upload` | 5 LEZ | 105 | `7HH46tXh…` at 8,000 / 60 |
| 7 | [`e2c59e8a…c61ef3be`](https://explorer.testnet.lez.logos.co/transaction/e2c59e8abc8c341e08021c6814db1fd151e81db9a84ed815e333d16bc61ef3be) | 9373 | 271,471 bytes | `storage.upload` | 1 LEZ | 1 | `6FscNXjN…` at 9,000 / 1 |
| 8 | [`23046b54…ce6ca3fc`](https://explorer.testnet.lez.logos.co/transaction/23046b5460304f8c0e644535d95361e477ffd5db5da9468739e06bbece6ca3fc) | 9389 | 271,471 bytes | `storage.upload` | 1 LEZ | 2 | `6FscNXjN…` at 9,000 / 2 |
| 9 | [`31b185e2…19942531`](https://explorer.testnet.lez.logos.co/transaction/31b185e279738ca793382e90065ad15a9f63fd992820172c2419fdc519942531) | 9456 | 271,471 bytes | `storage.upload` | 1 LEZ | 3 | `6FscNXjN…` at 9,000 / 3 |
| 10 | [`ed8c3514…374b8cb3`](https://explorer.testnet.lez.logos.co/transaction/ed8c351412409c81723ea7b90e2d9cdcb0841a33234894bfff8269af374b8cb3) | 9477 | 271,471 bytes | `storage.upload` | 1 LEZ | 4 | `6FscNXjN…` at 9,000 / 4 |
| 11 | [`52ef56ad…4ed873e6`](https://explorer.testnet.lez.logos.co/transaction/52ef56ad06c149e3725655108a86f7947b501cfe5504667b03ec07234ed873e6) | 9938 | 271,471 bytes | `storage.upload` | 1 LEZ | 106 | `7HH46tXh…` at 9,000 / 2 |
| 12 | [`071d25d7…1412057a`](https://explorer.testnet.lez.logos.co/transaction/071d25d7193fd3c3b6380c4e28b5de1ec117fc056b013c53e2f110171412057a) | 10081 | 271,471 bytes | `storage.upload` | 1 LEZ | 107 | `7HH46tXh…` at 10,000 / 1 |
| 13 | [`54f85182…e2f47115`](https://explorer.testnet.lez.logos.co/transaction/54f851825f171cf62f6b4723f7133687f3d9dff7e138417374cc7960e2f47115) | 10102 | 271,471 bytes | `storage.upload` | 1 LEZ | 108 | `7HH46tXh…` at 10,000 / 2 |

**2 of the 13 settlements above predate the program this repository ships.**
Settlements 1 and 2 charged an envelope, `Coxz1Cmf…`, owned by a different
ProgramId; the generator says so per row, in its own words, rather than leaving a
reader to notice. They are kept because they are on chain and a reviewer will
find them, but the criterion they support is only supported by the **11 made
under the current deployment**.

Two further settlements pay a **shielded** payee and are recorded in
`artifacts/shielded-settlement.tsv`, checked against the chain by
`./scripts/verify-deployment.sh`: the messaging agent paying the storage agent at
its shielded keys under the shipped `spend` instruction
([`5942d6cd…53a03d61`](https://explorer.testnet.lez.logos.co/transaction/5942d6cd6d223fd5bc7b5abd3bf34a1c1fc8e540e508232411e60e4d53a03d61),
block 9360), and the storage agent then **spending** what it received
([`e82a81f6…e39f9308`](https://explorer.testnet.lez.logos.co/transaction/e82a81f6076d3fd2e846e77223435658a31c9c9eabcbbf6b2fefa3f1e39f9308),
block 9379), which is what makes it received rather than merely committed. The
first of those is also why settlement 11 above reads `spent 2` rather than
`spent 1`: it is the messaging agent's first charge against period 9,000, and it
is not in the public table because its payee is not a public account.

What the chain cannot show, stated rather than implied: the payer is a shielded
account, so only the credit side of each settlement is publicly readable.
`getAccount` answers with a fully-populated default account — zero balance, zero
nonce, zero owner — for a shielded address exactly as it does for one that has
never existed, so **it is not an existence check** and no debit is quoted here.
The debit is constrained anyway: LEZ rule 8 requires total balance to be
preserved across every program in a transaction, so a transaction that credited
25 LEZ debited 25 LEZ.

**One caution about the links above.** The block explorer indexes roughly an hour
and three quarters behind the sequencer, so a settlement that landed recently
reads "not found" at its link while `getTransaction` already returns it. That is
an indexing lag, not a missing transaction. Ask the RPC directly if a link is
empty — and note that the control is what makes the answer mean anything:

```bash
q() { curl -s -X POST https://testnet.lez.logos.co \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"$1\"]}"; }

q 54f851825f171cf62f6b4723f7133687f3d9dff7e138417374cc7960e2f47115
                # => {"result":[<bytes>,<block>]}   present
q dededededededededededededededededededededededededededededededede
                # => {"result":null}                absent — as it must be
```

## Approach: what was tried, and why Logos

### What was tried and did not work

The prize asks for this explicitly, and it is the part of this repository with
the most to say. Every item below is a design that shipped and was replaced, not
a hypothetical that was reasoned away.

**Folding the limits into the address.** The original design — and the one this
project recommended at length — put the limits in the *address*:
`PDA(SHA256(owner ‖ agent ‖ per_tx ‖ per_period ‖ period_blocks))`, so that
raising a limit named a different, never-initialised account. It shipped in three
deployments and it does not work. Three successive fixes each added one more
comparison and each time the attack simply moved; the version that mattered
needed no missing comparison at all. An attacker holding a compromised agent's
key does not have to impersonate an owner or borrow a policy — **it is** the
owner: it anchors a *fresh* policy naming the compromised agent and itself, with
`per_tx = per_period = u128::MAX`, and every check passes, because every check is
satisfied. Folding the limits into the address is what made that available: every
`(owner, agent, limits)` triple had an account of its own, all uninitialised, so
"anchor a new policy" was always on the table. One address per agent removes the
choice. `crates/agent-verifier-adversarial` executes the attack against the
deployed binary and asserts the halt code it now stops at, and `demo.sh` replays
it against both programs on the public chain — the superseded one accepted it
([`eedb3caf…0d52e0a7`](https://explorer.testnet.lez.logos.co/transaction/eedb3caf5df94022e6383dec15fa956c7d9c45cd9c3f075ff5a7ff0e0d52e0a7),
block 8869, and the stranger still owns that agent's only policy, for good), and
the identical call to the program deployed today was submitted and is in no
block.

**Anchoring in one signature.** Even with one address per agent, the deployment
before the current one declared only the policy account and a signer it recorded
as the owner; the agent's own account was never declared, never read and never
asked to sign, and `agent_id` was a free `[u8; 32]` argument the body discarded.
So anchoring a policy over somebody else's agent needed **no key at all** — only
the agent's public id, which this repository publishes in `artifacts/agents.tsv`
and inside every signed Agent Card. The fix is the two-signature anchor: the agent
signs `claim_agent` to name the account that may bind it, and only that account's
signature is accepted by `create_policy`. `docs/security-model.md` §2 is the
long form.

**A caller-supplied period total.** `spend` used to take `spent_this_period` as an
argument, which both callers passed as `0`, so the per-period ceiling was advisory
and an agent that always passed zero had a per-transaction limit and no period
limit at all. It now takes `window_start` instead, and does not trust it: the
window must begin on a multiple of `period_blocks`, and the transaction is pinned
to `[window_start, window_start + period_blocks)` by its own block validity range,
so a caller cannot reset its budget by naming a different period.

**An approval that never expired.** An approval account used to be valid in every
block forever, which made it a bearer instrument redeemable the day the agent's
key was stolen. The owner now names the block it dies at, and `spend_approved`
pins the transaction to `[0, expiry)` — an expired approval is not refused, it is
a transaction no block will include.

**"A shielded agent cannot be paid at its shielded account."** This was written up
as an upstream limitation for most of this submission's life. It was wrong. The
`KeyNotFoundError` behind it was a lookup in the *payer's own* key chain, reached
because `spel` built `AccountIdentity::PrivateOwned` for every `Private/`
argument; `spel` is vendored at `vendor/spel` and built here, and now builds
`PrivateForeign { npk, vpk, identifier }` when a recipient is given by keys.
`docs/limitations.md` carries the retraction in full, and the settlement that
closes it is `5942d6cd…` in block 9360.

**"A loaded module cannot have a Delivery node."** A host cannot *pass* a
`std::function` across a plugin boundary — true — which was read as meaning a
module cannot *have* one. It can: the module links `liblogosdelivery` and builds
its ports on the far side of the boundary, and nothing crosses it but
`meta.configure("delivery","on")`. The same mistake was made about payment:
`TaskPort::pay` was unwired with a note saying a settlement needs a wallet and a
sequencer "and this module has neither". The module does not need to *have* a
wallet, it needs to *reach* one.

**Tests that passed while the shipped module did nothing.** `TaskPersistence` had
121 green assertions and **no construction site in the plugin**, so the shipped
module persisted nothing while its tests were green. `meta.skills` was described
in three C++ doc comments as existing while `invoke("meta.skills")` returned *no
skill named 'meta.skills' is registered*. Both are why every criterion below is
asserted against the **packaged artefact** — `module/agent.lgx` loaded through
`QPluginLoader` or through Basecamp's own `liblogos_core` — rather than against a
rebuild or against the source.

**An expectation that went stale where nothing compared it.**
`module/tests/skills_test.cpp` spells out every built-in skill by name, on
purpose, so that removing one from registration turns the suite red *with the
name* rather than with a count. Three skills joined the registry and that list did
not follow, and CI went red — correctly, and expensively: the step runs under
`set -euo pipefail`, so the ten steps after it did not run either. The list is
current again, and it no longer depends on being remembered: `check-docs.py` now
derives the registry from `installBuiltinSkills` and compares all three harness
tables against it, reporting the missing name in seconds rather than in a
compile.

**Two build traps that cost days and are worth naming.** A plugin built against a
Qt whose minor version exceeds the host's makes `logos_core_load_module` return
*success* and join the loaded set; its `logos_host` is then gone and every call
spends twenty seconds timing out. And the plugin referenced
`operator<<(QDataStream&, const LogosResult&)` **without defining it in every
build this repository has ever shipped** — `logos-module-builder` does not compile
`logos_types.cpp`, Mach-O binds lazily and never faulted, and on ELF the module
loads, joins the runtime, hands out a client and then dies on the first call
across the transport, looking exactly like the Qt mismatch. Porting to Linux found
it; nothing else would have. Both are recorded in `docs/basecamp.md`.

### Why Logos, specifically

The payer is a **shielded** account. What a task settlement reveals on a
centralised rail — who paid, for what, how often — is exactly the metadata that
makes an agent marketplace legible to whoever runs it. Here the settlement is a
privacy-preserving transaction signed by the agent's own private account, and the
payee can be shielded too. A payee is named one of two ways and its Agent Card
carries both: by its **public** `paymentAccount`, where the payer stays hidden and
the credit is checkable by anyone with `getAccount` — which is also what makes the
amount, the payee and the timing public — or by `x-logos.shieldedPaymentKeys`, an
`npk`/`vpk` pair that gives nothing away, where both ends are hidden and nobody
but the payee can read the amount. Settlement `5942d6cd…` in block 9360 is the
second form. No centralised payment rail offers the second option at all, and the
first one it offers only to itself.

**A2A leaves two things open on purpose — payment and encrypted transport — and
Logos supplies both natively.** LEZ is the payment layer A2A omits; Logos
Messaging is the transport binding that replaces A2A's HTTP, so two agents
discover each other and negotiate a task with no server in the middle that could
refuse either of them. The relays used in the live runs are third-party public
ones, which is what "no intermediary server" has to mean.

And the property that matters most is the one a centralised stack cannot express
at all: **on a centralised alternative the spending ceiling would be a row in
someone's database rather than an address in a state machine**, and "the agent
cannot exceed its limit" would be a promise instead of a rejection. Here it is
`Program error 6005`, returned by a state machine, with no transaction built.
Logos Delivery gives the owner channel the same property — the owner reaches the
agent because both hold keys, not because a broker is willing to route between
them — and it is why an owner's app on a laptop can answer an agent running
unattended on a remote node with nothing configured in between.
