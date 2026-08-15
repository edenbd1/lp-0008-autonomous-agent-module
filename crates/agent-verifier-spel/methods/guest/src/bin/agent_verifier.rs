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
// ONE POLICY ACCOUNT PER AGENT
//
// The policy account's address is `PDA(this program, ["agent-policy/v1",
// agent_id])`. The agent, and nothing else. Everything the policy says — who
// owns it, both limits, the period, and the running total — lives in that
// account's DATA, which only this program may write (LEZ rule 6,
// `UnauthorizedDataModification`, `program/mod.rs:718-728`).
//
// `create_policy` declares that account `#[account(init)]`, and `init` refuses
// an account that is not in its default state. So the first anchor for an agent
// is the only anchor for that agent. A second one is not detected — it is
// impossible.
//
// WHY THAT, AND NOT MORE CHECKS
//
// The three previous deployments derived the address from the policy CONTENTS:
// owner, agent, and both limits. Each fix added a comparison, and each time the
// attack moved:
//
//   - `create_policy` did not compare `owner_id` to the signer, so an agent
//     anchored a policy naming an owner that did not exist;
//   - `spend` did not compare the payer to `agent_id`, so any agent spent under
//     anybody's anchored policy;
//   - `approve_spend` did not compare its signer to the owner, so the agent
//     signed its own approvals.
//
// All three were closed. The attack still worked, and this is the version that
// matters: an attacker holding a compromised agent's key does not have to
// invent an owner or borrow a policy. **It is the owner.** It anchors a fresh
// policy naming the compromised agent as `agent_id` and itself as `owner_id`,
// with `per_tx = per_period = u128::MAX`, and spends the balance under that.
// Every comparison above passes, because every one of them is satisfied. That
// was executed against the deployed binary — `create_policy` accepted at halt 0,
// `spend` of the agent's whole balance accepted at halt 0 — in three variants,
// including one where the anchoring signer was the agent's own public pay
// account and the follow-up was a self-signed `approve_spend` for 999,999.
//
// The defect was never a missing comparison. It was that the caller chose the
// address: folding the limits into it meant every (owner, agent, limits) triple
// had an account of its own, all uninitialised, so "anchor a new policy" was
// always available. Removing that choice is the fix. There is one address per
// agent, `init` gives it to whoever writes first, and the owner writes it when
// it creates the agent — before the agent has run, let alone been taken.
//
// WHAT REMAINS, AND WHERE IT IS CHECKED
//
//   create_policy   `init` — one policy account per agent, first writer wins.
//                   The owner recorded is the signer's own id, not an argument.
//   spend           the policy account's address is derived from the PAYING
//                   account's id, so there is no `agent_id` argument to lie
//                   about. The limits come out of the account, not the call.
//   approve_spend   the signer must be the owner the RECORD names            (6012)
//   spend_approved  same address derivation, and the marker was created by
//                   this program and is unspent.
//
// `account_id` and `program_owner` reach the program through the pre-states the
// state machine built, not through the instruction the agent serialised, which
// is why these are here and not in the agent.
//
// THE PER-PERIOD LIMIT, AND THE CLOCK THIS CHAIN DOES NOT HAVE
//
// `per_period` needs a running total. It used to be an instruction argument —
// `spent_this_period` — which both callers passed as 0, while the comments here
// and in agent-policy-core claimed the program "re-derives it rather than
// trusting the agent". It did not. The enforced ceiling was min(per_tx,
// per_period) per transaction and unbounded in aggregate.
//
// The total lives in the policy account's data, next to the limits it is
// measured against.
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
// — agent, recipient, amount, nonce. The marker is created with `init` by
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
/// No policy is committed at this address: the agent has never been anchored.
const E_POLICY_NOT_ANCHORED: u32 = 6002;
/// `marker_seed` is not the marker for this (agent, recipient, amount, nonce).
const E_SPEND_REF_MISMATCH: u32 = 6003;
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
/// The account that signed is not the owner this agent's policy records.
/// Without this the agent approves its own above-threshold spends.
const E_OWNER_MISMATCH: u32 = 6012;
/// The period named does not start on a multiple of `period_blocks`.
const E_WINDOW_MISALIGNED: u32 = 6014;
/// The period named is older than the one the policy record holds.
const E_WINDOW_REGRESSED: u32 = 6015;
/// The policy account's data is not a record this program wrote.
const E_RECORD_MALFORMED: u32 = 6016;
/// `period_blocks` is zero, so the policy has no period to account against.
const E_PERIOD_ZERO: u32 = 6017;
/// The owner approval presented has already been spent.
const E_APPROVAL_ALREADY_SPENT: u32 = 6018;

// 6001 (policy hash mismatch), 6004 (marker seed mismatch at anchoring) and
// 6013 (payer is not the agent) are retired rather than reused. All three
// existed because the caller chose the policy account's address and the program
// had to check the choice; the address is now a function of the agent, so the
// disagreements they reported cannot be expressed. Leaving the numbers unused
// keeps an integration that branches on them from silently matching a different
// refusal.

#[lez_program]
mod agent_verifier {
    #[allow(unused_imports)]
    use super::*;

    /// Publish an agent's spending policy. Once, per agent, for good.
    ///
    /// Accounts:
    /// - `policy` (init, PDA seeded by `["agent-policy/v1", agent_id]`): the
    ///   on-chain commitment. Its address is a function of the agent and nothing
    ///   else, so this is *the* policy account for that agent rather than one of
    ///   many, and `init` refuses to overwrite it. That is the whole of the fix
    ///   for the anchoring hole: an attacker holding the agent's key cannot
    ///   anchor a second, unlimited policy, because there is nowhere to put it.
    ///
    ///   Its data is the record — owner, limits, period, running total — written
    ///   here and afterwards only by `spend`.
    ///
    /// - `owner` (signer): the human deploying the agent. There is no `owner_id`
    ///   argument: the signer's own account id is what gets recorded, so the
    ///   claim and the fact cannot differ. Signers reach the state machine
    ///   through the witness set rather than through `message.account_ids`
    ///   (`lee/state_machine/src/validated_state_diff/mod.rs:498`), and
    ///   declaring the account is what makes `spel` sign at all.
    ///
    ///   **The signer must be an account some program already owns** — in
    ///   practice, one that has received a transfer — if it is to anchor more
    ///   than once. A signer still holding the default program owner works
    ///   exactly once: on its second anchor its nonce is no longer zero,
    ///   `#[lez_program]` drops its post-state to dodge rule 7
    ///   (`program/mod.rs:730-738`), and the state machine then rejects the
    ///   transaction for the account being declared and missing
    ///   (`validated_state_diff/mod.rs:311-319`,
    ///   `DeclaredAccountMissingFromOutput`). A program-owned signer is exempt
    ///   from both. Measured, not assumed — `DumJ4LCB…`, owned by the transfer
    ///   program, anchored two policies in consecutive blocks.
    ///
    /// `agent_id` is still a caller-supplied value, and it has to be: the owner
    /// must be able to name the agent it is deploying. What changed is that
    /// naming an agent no longer buys anything, because the account that name
    /// resolves to is either free (and then this is the owner anchoring) or
    /// taken (and then this call cannot land).
    #[instruction]
    pub fn create_policy(
        #[account(init, pda = [literal("agent-policy/v1"), arg("agent_id")])]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    ) -> SpelResult {
        // `agent_id` is read by the macro to derive and check the PDA above; the
        // body has nothing left to compare it against, which is the point.
        let _ = agent_id;
        // A policy with no period cannot be accounted against — `authorize`
        // would have nothing to align a window to. Refuse it at anchoring time
        // so no such account can ever exist.
        if period_blocks == 0 {
            return Err(SpelError::custom(
                E_PERIOD_ZERO,
                "period_blocks must not be zero",
            ));
        }
        let record = agent_policy_core::PolicyRecord {
            // The signer, off the pre-state. Not a claim in the instruction.
            owner: *owner.account_id.value(),
            policy: agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks },
            ledger: agent_policy_core::SpendLedger::default(),
        };
        let mut policy = policy;
        write_record(&mut policy, &record)?;
        Ok(SpelOutput::execute(vec![policy, owner], vec![]))
    }

    /// Record that the owner approved one specific above-threshold spend.
    ///
    /// Accounts:
    /// - `approval` (init, PDA seeded by `marker_seed`): exists only because the
    ///   owner signed. `init` makes it single-issue; `spend_approved` stamping
    ///   its data is what makes it single-*use*.
    /// - `policy` (PDA seeded by `["agent-policy/v1", agent_id]`): required to be
    ///   owned by this program, so an approval cannot be attached to an invented
    ///   policy. It is also where the owner comes from.
    /// - `owner` (signer): must be the owner the RECORD names — read off the
    ///   chain, not supplied here. Without this a compromised agent signs its own
    ///   approvals and walks straight past the threshold `spend` refuses to
    ///   cross, which would make the whole above-threshold path decorative.
    ///
    /// `recipient` is the account id of the account that will be paid, the same
    /// bytes `spend_approved` reads off the recipient account it is handed. An
    /// approval therefore names an account, not a label.
    #[instruction]
    pub fn approve_spend(
        ctx: ProgramContext,
        #[account(init, pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(pda = [literal("agent-policy/v1"), arg("agent_id")])]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        marker_seed: [u8; 32],
        agent_id: [u8; 32],
        recipient: [u8; 32],
        amount: u128,
        nonce: u64,
    ) -> SpelResult {
        // Owned by *this* program, not merely non-default: anyone can fund an
        // address, and an approval attached to an account this program never
        // created is not attached to a policy at all.
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for this agent",
            ));
        }
        let record = read_record(&policy)?;
        if *owner.account_id.value() != record.owner {
            return Err(SpelError::custom(
                E_OWNER_MISMATCH,
                "the signer is not the owner this agent's policy records",
            ));
        }
        let spend_ref = agent_policy_core::compute_spend_ref(&agent_id, &recipient, amount, nonce);
        if agent_policy_core::compute_approval_marker(&spend_ref) != marker_seed {
            return Err(SpelError::custom(
                E_SPEND_REF_MISMATCH,
                "marker_seed does not commit to this spend",
            ));
        }
        Ok(SpelOutput::execute(vec![approval, policy, owner], vec![]))
    }

    /// The agent pays another account, inside its envelope, alone.
    ///
    /// There is no `agent_id` argument and no limits in the call. The policy
    /// account's address is derived from the PAYING account's own id — `pda =
    /// [const("agent-policy/v1"), account("agent")]` — so an agent presenting
    /// another agent's policy account fails the PDA check the macro emits before
    /// this body runs, and the ceiling it is measured against is whatever the
    /// account says, not what the caller typed.
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
    /// older than the period the record holds, and the transaction this
    /// instruction produces is only valid inside it.
    ///
    /// Accounts:
    /// - `policy` (mut, PDA seeded by `["agent-policy/v1", agent]`): the anchored
    ///   record. Mutable because the running total is written back here.
    /// - `agent` (signer): the agent's own account, the payer, and the seed of
    ///   the policy address.
    /// - `recipient` (mut): the account that is actually paid. An earlier
    ///   version did not declare it, which meant the program could check that a
    ///   policy permitted an amount and then move nothing — an authorisation
    ///   proof presented as a payment.
    #[instruction]
    pub fn spend(
        ctx: ProgramContext,
        #[account(mut, pda = [literal("agent-policy/v1"), account("agent")])]
        policy: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        #[account(mut)]
        recipient: AccountWithMetadata,
        amount: u128,
        window_start: u64,
    ) -> SpelResult {
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for this agent",
            ));
        }

        let mut policy = policy;
        let mut record = read_record(&policy)?;
        // The one decision, made in the crate the tests exercise. Refuse rather
        // than silently fall through: an agent that is over its limit must be
        // told to go and get an approval, not handed a success.
        record.ledger = record
            .policy
            .authorize(&record.ledger, window_start, amount)
            .map_err(refusal_error)?;
        let pol = record.policy;
        write_record(&mut policy, &record)?;

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
    /// payment carries an authorisation from the owner naming this recipient,
    /// this amount and this nonce. Making it draw down the same budget would mean
    /// an owner's own approval could be refused because the agent had been busy,
    /// and — since the marker is single-use — would buy nothing.
    ///
    /// Accounts:
    /// - `policy` (PDA seeded by `["agent-policy/v1", agent]`): the anchored
    ///   record, addressed from the payer exactly as in `spend`.
    /// - `approval` (mut, PDA seeded by `marker_seed`): the owner's
    ///   authorisation, checked to be owned by this program — an account merely
    ///   *existing* at the right address proves nothing, since anyone can fund
    ///   an address — and stamped here so it cannot be presented twice.
    /// - `agent` (signer): the agent's own account, and the payer.
    /// - `recipient` (mut): the account paid, checked against the account id
    ///   the approval commits to.
    #[instruction]
    pub fn spend_approved(
        // Injected by the dispatcher from the trusted ProgramInput; never part
        // of the instruction ABI, so the published IDL does not carry it.
        ctx: ProgramContext,
        #[account(pda = [literal("agent-policy/v1"), account("agent")])]
        policy: AccountWithMetadata,
        #[account(mut, pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        #[account(mut)]
        recipient: AccountWithMetadata,
        recipient_id: [u8; 32],
        amount: u128,
        nonce: u64,
        marker_seed: [u8; 32],
    ) -> SpelResult {
        // 1. Anchor it. The address came from the payer, so this is that agent's
        //    policy or it is nothing — and one anyone funded is not one this
        //    program created.
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for this agent",
            ));
        }
        // Read it so a policy account holding data this program did not write
        // cannot be presented as an anchor. Nothing else here needs the limits:
        // an approved payment is bounded by the approval, not by the envelope.
        let _ = read_record(&policy)?;

        // 2. The account being paid must be the account the approval names.
        //    Without this, `recipient_id` is a label in a hash and the money
        //    could go somewhere else entirely.
        if *recipient.account_id.value() != recipient_id {
            return Err(SpelError::custom(
                E_RECIPIENT_MISMATCH,
                "the recipient account is not the one this spend names",
            ));
        }

        // 3. The owner must have approved this exact payment. The seed binds the
        //    approval to (agent, recipient, amount, nonce) — and the agent is
        //    read off the paying account, not taken from the call, so an
        //    approval granted for one agent cannot be presented by another.
        let spend_ref = agent_policy_core::compute_spend_ref(
            agent.account_id.value(),
            &recipient_id,
            amount,
            nonce,
        );
        if agent_policy_core::compute_approval_marker(&spend_ref) != marker_seed {
            return Err(SpelError::custom(
                E_SPEND_REF_MISMATCH,
                "marker_seed does not commit to this spend",
            ));
        }

        // 4. And the approval must have been created by *this* program.
        //    Checking it is merely non-default would let anyone fund the
        //    address and manufacture consent.
        if approval.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_APPROVAL_NOT_ANCHORED,
                "no owner approval is anchored for this spend",
            ));
        }

        // 5. Once. `init` in `approve_spend` stops the marker being created
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
            .map_err(|_| SpelError::custom(E_RECORD_MALFORMED, "the spent marker does not fit"))?;

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

/// Read the owner, the limits and the running total out of the policy account.
///
/// Fails closed: data this program did not write is not a policy, and empty data
/// is not "a fresh ledger" any more — `create_policy` writes a full record, so an
/// account this program owns always has one.
fn read_record(policy: &AccountWithMetadata) -> Result<agent_policy_core::PolicyRecord, SpelError> {
    agent_policy_core::PolicyRecord::decode(&policy.account.data).map_err(|_| {
        SpelError::custom(
            E_RECORD_MALFORMED,
            "the policy account does not hold a record this program wrote",
        )
    })
}

/// Write it back. Rule 6 allows this because the policy account is a PDA of
/// this program — or, at `create_policy`, because its pre-state is default —
/// and forbids it to everyone else.
fn write_record(
    policy: &mut AccountWithMetadata,
    record: &agent_policy_core::PolicyRecord,
) -> Result<(), SpelError> {
    policy.account.data = nssa_core::account::Data::try_from(record.encode().to_vec())
        .map_err(|_| SpelError::custom(E_RECORD_MALFORMED, "the record does not fit in account data"))?;
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
