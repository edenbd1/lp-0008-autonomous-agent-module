// LP-0008 on-chain agent spending policy.
//
// WHAT THIS ENFORCES, AND WHY IT IS ON CHAIN
//
// The prize asks for a threshold below which the agent spends unattended and
// above which it waits for its owner. Implemented inside the agent that is not
// a threshold: the agent holds its own keys on a remote node, so whoever takes
// the process takes the spending. The limit has to be enforced somewhere the
// agent cannot rewrite, which is here.
//
// The policy account's address derives from the policy itself — both limits and
// the period length. Raising a limit therefore does not edit an account, it
// names a different one, which `create_policy` never initialised, so its owner
// is the default and the spend is rejected before the program body runs.
//
// THE THREE BINDINGS THAT MAKE THAT A CEILING
//
// Deriving the address from the limits stops the agent EDITING its policy. On
// its own it does not stop the agent ANCHORING ANOTHER ONE, and for four
// deployments it did not: `create_policy` accepted a caller-supplied `owner_id`
// without ever comparing it to the account that signed, and `spend` accepted any
// anchored policy account without comparing the payer to the agent that policy
// names. Both were demonstrated against the deployed binary — a `create_policy`
// signed by an agent's own key, naming an invented owner and `per_tx =
// u128::MAX`, was accepted at halt 0, and a `spend` of that agent's entire
// balance under it was accepted at halt 0 as well.
//
// Fixing only the first would have changed nothing: an attacker who cannot
// invent an owner id can still anchor a policy under an account it really
// controls and point the compromised agent at that. Fixing only the first two
// leaves `approve_spend`, which took a signer and never asked whether that
// signer was the policy's owner — so the agent could sign its own approvals and
// step over the threshold that way. The property needs all three:
//
//   create_policy   the signer IS the owner the policy commits to   (6012)
//   spend           the payer IS the agent the policy commits to    (6013)
//   spend_approved  same, and the marker was created by this program
//   approve_spend   the signer IS that same owner                   (6012)
//
// Each is one comparison against account metadata the caller does not control,
// which is why they are here and not in the agent: `account_id` and
// `program_owner` reach the program through the pre-states the state machine
// built, not through the instruction the agent serialised.
//
// THE PER-PERIOD LIMIT, AND THE CLOCK THIS CHAIN DOES NOT HAVE
//
// `per_period` needs a running total. It used to be an instruction argument —
// `spent_this_period` — which both callers passed as 0, while the comments here
// and in agent-policy-core claimed the program "re-derives it rather than
// trusting the agent". It did not. The enforced ceiling was min(per_tx,
// per_period) per transaction and unbounded in aggregate.
//
// The total now lives in the policy account's data. This program owns that
// account, so LEZ rule 6 lets it write there and stops anyone else
// (`UnauthorizedDataModification`, `program/mod.rs:718-728`).
//
// The period is harder, because a program cannot read the block height:
// `ProgramInput` is (self_program_id, caller_program_id, pre_states,
// instruction) and there is nothing else in it. What a program CAN do is
// constrain where its transaction lands — `SpelOutput::with_block_validity_window`
// becomes `ProgramOutput.block_validity_window`, which the state machine checks
// against the block it is building (`validated_state_diff/mod.rs:202-208` for
// the public path, `:393-398` for the privacy-preserving one) and rejects with
// `OutOfValidityWindow`. So the caller names its period, and the guest makes the
// name binding:
//
//   - the period must start on a multiple of `period_blocks`, so windows cannot
//     be slid forward a block at a time to reset the budget;
//   - the transaction is pinned to `[window_start, window_start + period_blocks)`,
//     so naming a future period yields a transaction no current block accepts;
//   - a period older than the one the ledger records is refused outright.
//
// Together those make "the sum of unattended spends inside one period" a
// quantity the chain computes rather than one the agent asserts.
//
// An above-threshold spend needs an approval marker seeded by the exact payment
// — policy, recipient, amount, nonce. The marker is created with `init` by
// `approve_spend`, which only the owner can sign, and `spend_approved` stamps it
// as it consumes it: a marker that exists is not the same as a marker that is
// still unspent, and without the stamp the same approval paid out again on every
// later transaction that presented it.
//
// HOW THE MONEY MOVES, AND WHY NOT DIRECTLY
//
// This program never touches a balance. It cannot: `validate_execution` rule 5
// (`lee/state_machine/core/src/program/mod.rs:707-716`) refuses any post-state
// that decreases the balance of an account the executing program does not own,
// and an agent's account is owned by LEZ's own authenticated transfer program —
// measured on chain, not assumed:
//
//     Private/9KdQSJ2t…  {"program_owner":"J8otq1J8Zpjhhpp6FPfhFtWKTCkLjthdk12cwHiMZCTB"}
//
// So a version of `spend` that debited the agent itself could never be accepted,
// however the accounts were declared. The mechanism that exists for this is the
// second argument of `SpelOutput::execute` — the `Vec<ChainedCall>` this program
// passed empty throughout its first four deployments. The policy check gates the
// payment and then **chains a call to the transfer program that does own the
// accounts**, exactly as LEZ's own `vault` moves funds
// (`lez/programs/vault/src/main.rs:47-58`). The callee is not a constant: it is
// `agent.account.program_owner`, so the program can only ever delegate to
// whoever already holds the agent's money.
//
// A LEZ public transaction re-executes rather than proves
// (`lee/state_machine/src/program/mod.rs:73-77`); the privacy-preserving path
// composes the same chain of calls under a real `env::verify`
// (`lee/privacy_preserving_circuit/src/execution_state.rs:149-155`) and the
// sequencer checks the receipt against the pinned
// `PRIVACY_PRESERVING_CIRCUIT_ID`. Both enforce this program; only one of them
// hides who paid.

#![no_main]

use spel_framework::prelude::*;

risc0_zkvm::guest::entry!(main);

/// The instruction ABI of LEZ's authenticated transfer program, mirrored from
/// `lez/programs/authenticated_transfer/core/src/lib.rs` at rev `47eba25`.
///
/// Mirrored rather than imported: that crate is `edition = "2024"`, which the
/// pinned risc0 guest toolchain does not build. The wire format is a risc0
/// `serde` enum — variant index first — so the variant ORDER here is the ABI and
/// must not be reordered. `Initialize` is never constructed by this program; it
/// is present so `Transfer` keeps index 0.
#[derive(serde::Serialize)]
enum AuthTransfer {
    /// Move `amount` of native balance. Accounts: `[sender, recipient]`.
    Transfer { amount: u128 },
    #[allow(dead_code)]
    Initialize,
}

// Deterministic and stable: an integration may branch on these.
/// The policy hash does not commit to the (owner, agent, limits) presented.
const E_POLICY_MISMATCH: u32 = 6001;
/// No policy is committed at this address: the limits are not anchored.
const E_POLICY_NOT_ANCHORED: u32 = 6002;
/// `spend_ref` is not the reference for this (policy, recipient, amount, nonce).
const E_SPEND_REF_MISMATCH: u32 = 6003;
/// The marker seed does not commit to the spend reference it claims to.
const E_MARKER_SEED_MISMATCH: u32 = 6004;
/// The spend exceeds the per-transaction limit and carries no owner approval.
const E_OVER_PER_TX_LIMIT: u32 = 6005;
/// The spend would carry the period total past the per-period limit.
const E_OVER_PERIOD_LIMIT: u32 = 6006;
/// An approval account exists at the right address but this program never
/// created it, so no owner ever approved this spend.
const E_APPROVAL_NOT_ANCHORED: u32 = 6007;
/// The agent's balance does not cover the amount it is trying to send.
const E_INSUFFICIENT_BALANCE: u32 = 6008;
/// Crediting the recipient would overflow its balance.
const E_RECIPIENT_OVERFLOW: u32 = 6009;
/// The agent's account is not held by any program, so there is nothing to
/// delegate the transfer to. An unfunded, never-claimed account looks like this.
const E_AGENT_UNOWNED: u32 = 6010;
/// The recipient account presented is not the recipient the approval names.
const E_RECIPIENT_MISMATCH: u32 = 6011;
/// The account that signed is not the owner this policy commits to. Without
/// this an agent anchors its own policy, or approves its own spend.
const E_OWNER_MISMATCH: u32 = 6012;
/// The account paying is not the agent this policy commits to. Without this any
/// agent spends under anybody's anchored policy — including one the attacker
/// anchored for itself.
const E_AGENT_MISMATCH: u32 = 6013;
/// The period named does not start on a multiple of `period_blocks`.
const E_WINDOW_MISALIGNED: u32 = 6014;
/// The period named is older than the one the policy ledger records.
const E_WINDOW_REGRESSED: u32 = 6015;
/// The policy account's data is not a ledger this program wrote.
const E_LEDGER_MALFORMED: u32 = 6016;
/// `period_blocks` is zero, so the policy has no period to account against.
const E_PERIOD_ZERO: u32 = 6017;
/// The owner approval presented has already been spent.
const E_APPROVAL_ALREADY_SPENT: u32 = 6018;

#[lez_program]
mod agent_verifier {
    #[allow(unused_imports)]
    use super::*;

    /// Publish an agent's spending policy.
    ///
    /// The owner signs this once, when they deploy the agent. What matters is
    /// that a given (owner, agent, limits) maps to exactly one account, which
    /// `init` guarantees by refusing to overwrite — and that the owner it
    /// commits to is the account that actually signed.
    ///
    /// Accounts:
    /// - `policy` (init, PDA seeded by `policy_hash`): the on-chain commitment.
    ///   Its address encodes both limits and the period, so none of them can be
    ///   altered later. Its data is the running total, and starts empty.
    /// - `owner` (signer): the human deploying the agent. `owner_id` must be
    ///   this account's own id.
    ///
    ///   Signers reach the state machine through the witness set rather than
    ///   through `message.account_ids`
    ///   (`lee/state_machine/src/validated_state_diff/mod.rs:498`), which is why
    ///   an earlier version of this instruction declared `owner` and never read
    ///   it. Declaring it is what makes `spel` sign at all; comparing it is what
    ///   makes anchoring authenticated. Without the comparison `owner_id` is 32
    ///   bytes the caller invents, and an agent that can call this instruction —
    ///   any agent, it needs no permission — anchors itself a policy with
    ///   `per_tx = u128::MAX` and spends its whole balance under it. That was
    ///   not a theory about this program; it was done to the deployed binary.
    ///
    ///   **The signer must be an account some program already owns** — in
    ///   practice, one that has received a transfer. A signer still holding the
    ///   default program owner works exactly once: on its second anchor its
    ///   nonce is no longer zero, `#[lez_program]` drops its post-state to dodge
    ///   rule 7 (`program/mod.rs:730-738`), and the state machine then rejects
    ///   the transaction for the account being declared and missing
    ///   (`validated_state_diff/mod.rs:311-319`,
    ///   `DeclaredAccountMissingFromOutput`). A program-owned signer is exempt
    ///   from both: it is never filtered, and rule 7 only fires on a *default*
    ///   post-state owner. Measured, not assumed — `DumJ4LCB…`, owned by the
    ///   transfer program, anchored `28930c0a…` at nonce 29 and `1075e47d…` at
    ///   nonce 30, in consecutive blocks.
    #[instruction]
    pub fn create_policy(
        #[account(init, pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    ) -> SpelResult {
        // Re-derive the commitment the address encodes. The macro already
        // constrains `policy` to be the PDA for this hash; this check is what
        // gives the hash its meaning, tying the address to specific limits
        // rather than to opaque bytes.
        let expected = agent_policy_core::compute_policy_hash(
            &owner_id,
            &agent_id,
            &agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks },
        );
        if expected != policy_hash {
            return Err(SpelError::custom(
                E_POLICY_MISMATCH,
                "policy_hash does not commit to this (owner, agent, limits)",
            ));
        }
        // The whole of the fix for the anchoring hole. Same shape as the
        // recipient check in `spend_approved`: an id in the instruction is a
        // claim, an id on an account in the pre-states is a fact.
        if *owner.account_id.value() != owner_id {
            return Err(SpelError::custom(
                E_OWNER_MISMATCH,
                "the signer is not the owner this policy commits to",
            ));
        }
        // A policy with no period cannot be accounted against — `authorize`
        // would have nothing to align a window to. Refuse it at anchoring time
        // so no such account can ever exist.
        if period_blocks == 0 {
            return Err(SpelError::custom(
                E_PERIOD_ZERO,
                "period_blocks must not be zero",
            ));
        }
        Ok(SpelOutput::execute(vec![policy, owner], vec![]))
    }

    /// Record that the owner approved one specific above-threshold spend.
    ///
    /// Accounts:
    /// - `approval` (init, PDA seeded by `marker_seed`): exists only because the
    ///   owner signed. `init` makes it single-issue; `spend_approved` stamping
    ///   its data is what makes it single-*use*.
    /// - `policy` (PDA seeded by `policy_hash`): required to be owned by this
    ///   program, so an approval cannot be attached to an invented policy.
    /// - `owner` (signer): must be the owner the policy commits to.
    ///
    /// The limits are arguments here for one reason: without them the policy
    /// hash is opaque bytes and this instruction cannot tell whose policy it is.
    /// Re-deriving the hash from (owner, agent, limits) and then checking the
    /// signer against `owner_id` is what stops a compromised agent signing its
    /// own approvals — which would walk straight past the threshold that
    /// `spend` refuses to cross, and make the whole above-threshold path
    /// decorative.
    ///
    /// `recipient` is the account id of the account that will be paid, the same
    /// bytes `spend_approved` reads off the recipient account it is handed. An
    /// approval therefore names an account, not a label.
    #[instruction]
    pub fn approve_spend(
        ctx: ProgramContext,
        #[account(init, pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        marker_seed: [u8; 32],
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        recipient: [u8; 32],
        amount: u128,
        nonce: u64,
    ) -> SpelResult {
        let expected = agent_policy_core::compute_policy_hash(
            &owner_id,
            &agent_id,
            &agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks },
        );
        if expected != policy_hash {
            return Err(SpelError::custom(
                E_POLICY_MISMATCH,
                "policy_hash does not commit to this (owner, agent, limits)",
            ));
        }
        // Owned by *this* program, not merely non-default: anyone can fund an
        // address, and an approval attached to an account this program never
        // created is not attached to a policy at all.
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed at this address",
            ));
        }
        if *owner.account_id.value() != owner_id {
            return Err(SpelError::custom(
                E_OWNER_MISMATCH,
                "the signer is not the owner this policy commits to",
            ));
        }
        let spend_ref =
            agent_policy_core::compute_spend_ref(&policy_hash, &recipient, amount, nonce);
        let expected = agent_policy_core::compute_approval_marker(&spend_ref);
        if expected != marker_seed {
            return Err(SpelError::custom(
                E_MARKER_SEED_MISMATCH,
                "marker_seed does not commit to this spend",
            ));
        }
        Ok(SpelOutput::execute(vec![approval, policy, owner], vec![]))
    }

    /// The agent pays another account, inside its envelope, alone.
    ///
    /// This instruction does not take an approval account, and that is the
    /// point. An earlier version declared one for both paths and passed
    /// whatever the caller handed over on the autonomous path. It could not
    /// work: SPEL requires one returned account per declared account, while the
    /// privacy circuit counts *distinct* identities and accepts two. Declaring
    /// three and naming a never-initialised PDA fails with `Invalid
    /// account_identities length, left: 3, right: 2`; passing the policy
    /// account twice to collapse them fails with `Pre-state account IDs are not
    /// unique`. Both happen while building the transaction, so nothing reaches
    /// the chain and no explorer shows a reason.
    ///
    /// An above-threshold payment goes to `spend_approved`, which declares the
    /// approval account because it actually reads it.
    ///
    /// `window_start` is the period the agent declares it is spending in. It is
    /// not trusted: it must be a multiple of `period_blocks`, it may not be
    /// older than the period the ledger records, and the transaction this
    /// instruction produces is only valid inside it.
    ///
    /// Accounts:
    /// - `policy` (mut, PDA seeded by `policy_hash`): the anchored limits, and
    ///   the running total. Mutable because the total is written back here —
    ///   the address carries the limits, the data carries what has been spent
    ///   against them.
    /// - `agent` (signer): the agent's own account, and the payer. Checked to
    ///   be the agent the policy commits to, otherwise every anchored policy on
    ///   the chain is a ceiling any agent may borrow.
    /// - `recipient` (mut): the account that is actually paid. An earlier
    ///   version did not declare it, which meant the program could check that a
    ///   policy permitted an amount and then move nothing — an authorisation
    ///   proof presented as a payment.
    #[instruction]
    pub fn spend(
        ctx: ProgramContext,
        #[account(mut, pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        #[account(mut)]
        recipient: AccountWithMetadata,
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        amount: u128,
        window_start: u64,
    ) -> SpelResult {
        let pol =
            agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks };
        let expected = agent_policy_core::compute_policy_hash(&owner_id, &agent_id, &pol);
        if expected != policy_hash {
            return Err(SpelError::custom(
                E_POLICY_MISMATCH,
                "policy_hash does not commit to these limits",
            ));
        }
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for these limits",
            ));
        }
        // The policy names an agent. This is the account paying. They have to be
        // the same account, or an agent under a 25-per-transaction envelope
        // simply presents the 200-per-transaction policy anchored next to it —
        // or one an attacker anchored for itself — and the ceiling is whatever
        // the loosest policy on the chain happens to be.
        if *agent.account_id.value() != agent_id {
            return Err(SpelError::custom(
                E_AGENT_MISMATCH,
                "the paying account is not the agent this policy commits to",
            ));
        }

        let mut policy = policy;
        let ledger = read_ledger(&policy)?;
        // The one decision, made in the crate the tests exercise. Refuse rather
        // than silently fall through: an agent that is over its limit must be
        // told to go and get an approval, not handed a success.
        let next = pol
            .authorize(&ledger, window_start, amount)
            .map_err(refusal_error)?;
        write_ledger(&mut policy, &next)?;

        let transfer = delegated_transfer(&agent, &recipient, amount)?;

        // Only the policy account's data changes here. Every balance in this
        // transaction is moved by the chained call, by the program that owns
        // them; a post-state from here that debited the agent would be rejected
        // by rule 5 before the transfer ever ran.
        //
        // Return the accounts themselves, not (account, claim) pairs: the
        // `#[lez_program]` macro rewrites this call into `execute_with_claims`
        // and derives each claim from the `#[account(...)]` attribute above, so
        // supplying claims here is both redundant and a type error.
        let out = SpelOutput::execute(
            vec![policy, agent, recipient],
            vec![transfer],
        );
        // And this is what makes `window_start` more than a number the agent
        // chose: the transaction is only includable inside the period it names.
        let (from, to) = pol
            .window_bounds(window_start)
            .ok_or_else(|| SpelError::custom(E_WINDOW_MISALIGNED, "the period does not fit in u64"))?;
        out.try_with_block_validity_window(from..to)
            .map_err(|_| SpelError::custom(E_WINDOW_MISALIGNED, "the period is not a valid block range"))
    }

    /// The agent pays above its envelope, on an approval the owner signed.
    ///
    /// The approval is always required here, whatever the amount. An earlier
    /// version skipped the marker checks when the payment happened to be inside
    /// the envelope, which meant this instruction had a path that read the
    /// approval account and never looked at it. Anything inside the envelope
    /// belongs in `spend`, where it is accounted against the period.
    ///
    /// An approved payment does not consume the unattended budget and is not
    /// blocked by it. That budget bounds what the agent moves *by itself*; this
    /// payment carries a signature from the owner naming this recipient, this
    /// amount and this nonce. Making it draw down the same budget would mean an
    /// owner's own approval could be refused because the agent had been busy,
    /// and — since the marker is single-use — would buy nothing.
    ///
    /// Accounts:
    /// - `policy` (PDA seeded by `policy_hash`): the anchored limits.
    /// - `approval` (mut, PDA seeded by `marker_seed`): the owner's
    ///   authorisation, checked to be owned by this program — an account merely
    ///   *existing* at the right address proves nothing, since anyone can fund
    ///   an address — and stamped here so it cannot be presented twice.
    /// - `agent` (signer): the agent's own account, and the payer. Checked
    ///   against the agent the policy commits to, exactly as in `spend`.
    /// - `recipient` (mut): the account paid, checked against the account id
    ///   the approval commits to.
    #[instruction]
    pub fn spend_approved(
        // Injected by the dispatcher from the trusted ProgramInput; never part
        // of the instruction ABI, so the published IDL does not carry it.
        ctx: ProgramContext,
        #[account(pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(mut, pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        #[account(mut)]
        recipient: AccountWithMetadata,
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        recipient_id: [u8; 32],
        amount: u128,
        nonce: u64,
        marker_seed: [u8; 32],
    ) -> SpelResult {
        // 1. The limits must be the anchored ones. Without re-deriving the hash,
        //    the agent could present generous limits alongside a real policy
        //    account and the address constraint alone would not notice.
        let pol =
            agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks };
        let expected = agent_policy_core::compute_policy_hash(&owner_id, &agent_id, &pol);
        if expected != policy_hash {
            return Err(SpelError::custom(
                E_POLICY_MISMATCH,
                "policy_hash does not commit to these limits",
            ));
        }

        // 2. Anchor it. An invented policy resolves to an address this program
        //    never initialised, and one anyone funded is not one this program
        //    created.
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for these limits",
            ));
        }

        // 3. The payer must be the agent the policy names — same reason as in
        //    `spend`. An approval is granted to one agent under one policy.
        if *agent.account_id.value() != agent_id {
            return Err(SpelError::custom(
                E_AGENT_MISMATCH,
                "the paying account is not the agent this policy commits to",
            ));
        }

        // 4. The account being paid must be the account the approval names.
        //    Without this, `recipient_id` is a label in a hash and the money
        //    could go somewhere else entirely.
        if *recipient.account_id.value() != recipient_id {
            return Err(SpelError::custom(
                E_RECIPIENT_MISMATCH,
                "the recipient account is not the one this spend names",
            ));
        }

        // 5. The owner must have approved this exact payment. The seed binds the
        //    approval to (policy, recipient, amount, nonce), so an approval for
        //    one transfer cannot be spent on another.
        let spend_ref =
            agent_policy_core::compute_spend_ref(&policy_hash, &recipient_id, amount, nonce);
        if agent_policy_core::compute_approval_marker(&spend_ref) != marker_seed {
            return Err(SpelError::custom(
                E_SPEND_REF_MISMATCH,
                "marker_seed does not commit to this spend",
            ));
        }

        // 6. And the approval must have been created by *this* program.
        //    Checking it is merely non-default would let anyone fund the
        //    address and manufacture consent.
        if approval.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_APPROVAL_NOT_ANCHORED,
                "no owner approval is anchored for this spend",
            ));
        }

        // 7. Once. `init` in `approve_spend` stops the marker being created
        //    twice; nothing stopped it being *presented* twice, and a marker
        //    that stays untouched authorises the same payment on every later
        //    transaction that names it. Stamping the account is the consumption
        //    — this program owns it, so rule 6 permits the write and forbids
        //    anyone else undoing it.
        if !approval.account.data.is_empty() {
            return Err(SpelError::custom(
                E_APPROVAL_ALREADY_SPENT,
                "this owner approval has already been spent",
            ));
        }
        let mut approval = approval;
        approval.account.data = nssa_core::account::Data::try_from(SPENT_MARKER.to_vec())
            .map_err(|_| SpelError::custom(E_LEDGER_MALFORMED, "the spent marker does not fit"))?;

        let transfer = delegated_transfer(&agent, &recipient, amount)?;

        Ok(SpelOutput::execute(
            vec![policy, approval, agent, recipient],
            vec![transfer],
        ))
    }
}

/// What `spend_approved` writes into a marker it has consumed. Any non-empty
/// data would do; a version byte leaves room to say more later.
const SPENT_MARKER: [u8; 1] = [1];

/// Read the running total out of the policy account.
fn read_ledger(policy: &AccountWithMetadata) -> Result<agent_policy_core::SpendLedger, SpelError> {
    agent_policy_core::SpendLedger::decode(&policy.account.data).map_err(|_| {
        SpelError::custom(
            E_LEDGER_MALFORMED,
            "the policy account does not hold a ledger this program wrote",
        )
    })
}

/// Write it back. Rule 6 allows this because the policy account is a PDA of
/// this program, and forbids it to everyone else.
fn write_ledger(
    policy: &mut AccountWithMetadata,
    ledger: &agent_policy_core::SpendLedger,
) -> Result<(), SpelError> {
    policy.account.data = nssa_core::account::Data::try_from(ledger.encode().to_vec())
        .map_err(|_| SpelError::custom(E_LEDGER_MALFORMED, "the ledger does not fit in account data"))?;
    Ok(())
}

/// One refusal, one stable error code. An integration may branch on these.
fn refusal_error(refusal: agent_policy_core::SpendRefusal) -> SpelError {
    use agent_policy_core::SpendRefusal as R;
    match refusal {
        R::PeriodZero => SpelError::custom(E_PERIOD_ZERO, "this policy has no period"),
        R::WindowMisaligned => SpelError::custom(
            E_WINDOW_MISALIGNED,
            "the period named does not start on a multiple of period_blocks",
        ),
        R::WindowRegressed => SpelError::custom(
            E_WINDOW_REGRESSED,
            "the period named is older than the one this policy has already spent in",
        ),
        R::OverPerTx => SpelError::custom(
            E_OVER_PER_TX_LIMIT,
            "the spend needs an owner approval: use spend_approved",
        ),
        R::OverPerPeriod => SpelError::custom(
            E_OVER_PERIOD_LIMIT,
            "the spend would carry the period total past the anchored limit",
        ),
    }
}

/// Build the chained call that actually moves the money.
///
/// The callee is `agent.account.program_owner` rather than a program id this
/// program carries or is handed. That is deliberate: it can only ever delegate
/// to the program that already owns the agent's balance, so no argument and no
/// deployment constant can redirect the payment to an attacker's program.
///
/// The pre-states go through unmodified. `is_authorized` in a chained call is
/// not a hint — the circuit recomputes it and asserts equality
/// (`execution_state.rs`, `Inconsistent authorization for account`), and the
/// public path rejects a forged one the same way
/// (`validated_state_diff/mod.rs:161-171`). The agent is authorized because it
/// signed this transaction; the recipient is not, and does not need to be.
///
/// The two balance checks below are not the enforcement — the transfer program
/// does its own `checked_sub`/`checked_add`. They are here so that an agent that
/// cannot afford a task gets error code 6008 instead of a guest panic inside a
/// program it did not write.
fn delegated_transfer(
    agent: &AccountWithMetadata,
    recipient: &AccountWithMetadata,
    amount: u128,
) -> Result<ChainedCall, SpelError> {
    if agent.account.program_owner == nssa_core::program::DEFAULT_PROGRAM_ID {
        return Err(SpelError::custom(
            E_AGENT_UNOWNED,
            "the agent account is held by no program and cannot pay",
        ));
    }
    if agent.account.balance < amount {
        return Err(SpelError::custom(
            E_INSUFFICIENT_BALANCE,
            "the agent cannot cover this spend",
        ));
    }
    if recipient.account.balance.checked_add(amount).is_none() {
        return Err(SpelError::custom(
            E_RECIPIENT_OVERFLOW,
            "the recipient balance would overflow",
        ));
    }
    Ok(ChainedCall::new(
        agent.account.program_owner,
        vec![agent.clone(), recipient.clone()],
        &AuthTransfer::Transfer { amount },
    ))
}
