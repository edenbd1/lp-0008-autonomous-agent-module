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
// is the default and the spend is rejected before the program body runs. The
// agent cannot lift its own ceiling because the ceiling is the name of the
// account it must present, not data it writes.
//
// An above-threshold spend needs an approval marker seeded by the exact payment
// — policy, recipient, amount, nonce. The marker is created with `init` by
// `approve_spend`, which only the owner can sign, and consumed here. Approving
// one transfer cannot authorise another, and none can be replayed.
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

#[lez_program]
mod agent_verifier {
    #[allow(unused_imports)]
    use super::*;

    /// Publish an agent's spending policy.
    ///
    /// The owner signs this once, when they deploy the agent. What matters is
    /// that a given (owner, agent, limits) maps to exactly one account, which
    /// `init` guarantees by refusing to overwrite.
    ///
    /// Accounts:
    /// - `policy` (init, PDA seeded by `policy_hash`): the on-chain commitment.
    ///   Its address encodes both limits and the period, so nothing needs to be
    ///   written into its data and none of them can be altered later.
    /// - `owner` (signer): the human deploying the agent. The instruction body
    ///   never reads it — signers reach the state machine through the witness
    ///   set, not through `message.account_ids`
    ///   (`lee/state_machine/src/validated_state_diff/mod.rs:498`) — but
    ///   declaring it is what makes `spel` sign at all, and anchoring has to be
    ///   authenticated: any policy account that exists is spendable against, so
    ///   permissionless anchoring would let an agent publish its own ceiling.
    ///
    ///   Returning a signer's post-state unclaimed would trip rule 7
    ///   (`program/mod.rs:730-738`) once its nonce left zero. It does not,
    ///   because `#[lez_program]` drops post-states for accounts this program
    ///   neither owns nor claims before writing the output, so one signer can
    ///   anchor any number of policies. Measured, not assumed:
    ///   `DumJ4LCB…` anchored `28930c0a…` at nonce 29 and `1075e47d…` at
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
        Ok(SpelOutput::execute(vec![policy, owner], vec![]))
    }

    /// Record that the owner approved one specific above-threshold spend.
    ///
    /// Accounts:
    /// - `approval` (init, PDA seeded by `marker_seed`): exists only because the
    ///   owner signed. `init` makes it single-use.
    /// - `policy` (PDA seeded by `policy_hash`): required to be owned by this
    ///   program, so an approval cannot be attached to an invented policy.
    /// - `owner` (signer).
    ///
    /// `recipient` is the account id of the account that will be paid, the same
    /// bytes `spend_approved` reads off the recipient account it is handed. An
    /// approval therefore names an account, not a label.
    #[instruction]
    pub fn approve_spend(
        #[account(init, pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        marker_seed: [u8; 32],
        policy_hash: [u8; 32],
        recipient: [u8; 32],
        amount: u128,
        nonce: u64,
    ) -> SpelResult {
        if policy.account.program_owner == nssa_core::program::DEFAULT_PROGRAM_ID {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed at this address",
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
    /// Accounts:
    /// - `policy` (PDA seeded by `policy_hash`): the anchored limits.
    /// - `agent` (signer): the agent's own account, and the payer.
    /// - `recipient` (mut): the account that is actually paid. An earlier
    ///   version did not declare it, which meant the program could check that a
    ///   policy permitted an amount and then move nothing — an authorisation
    ///   proof presented as a payment.
    #[instruction]
    pub fn spend(
        #[account(pda = arg("policy_hash"))]
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
        spent_this_period: u128,
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
        if policy.account.program_owner == nssa_core::program::DEFAULT_PROGRAM_ID {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for these limits",
            ));
        }
        // Refuse rather than silently fall through: an agent that is over its
        // limit must be told to go and get an approval, not handed a success.
        if !pol.is_autonomous(amount, spent_this_period) {
            return Err(SpelError::custom(
                E_OVER_PER_TX_LIMIT,
                "the spend needs an owner approval: use spend_approved",
            ));
        }

        let transfer = delegated_transfer(&agent, &recipient, amount)?;

        // The accounts come back untouched. Every balance in this transaction
        // is moved by the chained call, by the program that owns them; a
        // post-state from here that debited the agent would be rejected by
        // rule 5 before the transfer ever ran.
        //
        // Return the accounts themselves, not (account, claim) pairs: the
        // `#[lez_program]` macro rewrites this call into `execute_with_claims`
        // and derives each claim from the `#[account(...)]` attribute above, so
        // supplying claims here is both redundant and a type error.
        Ok(SpelOutput::execute(
            vec![policy, agent, recipient],
            vec![transfer],
        ))
    }

    /// The agent pays above its envelope, on an approval the owner signed.
    ///
    /// Accounts:
    /// - `policy` (PDA seeded by `policy_hash`): the anchored limits.
    /// - `approval` (PDA seeded by `marker_seed`): the owner's authorisation,
    ///   checked to be owned by this program — an account merely *existing* at
    ///   the right address proves nothing, since anyone can fund an address.
    /// - `agent` (signer): the agent's own account, and the payer.
    /// - `recipient` (mut): the account paid, checked against the account id
    ///   the approval commits to.
    #[instruction]
    pub fn spend_approved(
        // Injected by the dispatcher from the trusted ProgramInput; never part
        // of the instruction ABI, so the published IDL does not carry it.
        ctx: ProgramContext,
        #[account(pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(pda = arg("marker_seed"))]
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
        spent_this_period: u128,
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
        //    never initialised, whose owner is the default.
        if policy.account.program_owner == nssa_core::program::DEFAULT_PROGRAM_ID {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for these limits",
            ));
        }

        // 3. The account being paid must be the account the approval names.
        //    Without this, `recipient_id` is a label in a hash and the money
        //    could go somewhere else entirely.
        if *recipient.account_id.value() != recipient_id {
            return Err(SpelError::custom(
                E_RECIPIENT_MISMATCH,
                "the recipient account is not the one this spend names",
            ));
        }

        // 4. Below the threshold there is nothing to approve. The payment still
        //    happens — an approval that was not required is not a reason to
        //    refuse — but `spend` is the cheaper call, needing no approval PDA.
        if !pol.is_autonomous(amount, spent_this_period) {
            // 5. Outside the envelope, the owner must have approved this exact
            //    payment. The seed binds the approval to (policy, recipient,
            //    amount, nonce), so an approval for one transfer cannot be
            //    spent on another.
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

            // 7. Even approved, the per-period ceiling still holds. An owner
            //    approving one large payment has not agreed to drain the period.
            if spent_this_period.saturating_add(amount) > per_period {
                return Err(SpelError::custom(
                    E_OVER_PERIOD_LIMIT,
                    "the spend would carry the period total past the anchored limit",
                ));
            }
        }

        let transfer = delegated_transfer(&agent, &recipient, amount)?;

        let _ = E_OVER_PER_TX_LIMIT;
        Ok(SpelOutput::execute(
            vec![policy, approval, agent, recipient],
            vec![transfer],
        ))
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
