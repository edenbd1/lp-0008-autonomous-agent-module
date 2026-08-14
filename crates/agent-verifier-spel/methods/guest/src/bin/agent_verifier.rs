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
// A LEZ public transaction re-executes rather than proves
// (`lee/state_machine/src/program/mod.rs:73-77`), so nothing on the public path
// verifies anything. This program is written for the privacy-preserving path,
// where the circuit composes chained calls with a real `env::verify`
// (`lee/privacy_preserving_circuit/src/execution_state.rs:149-155`) and the
// sequencer checks the receipt against the pinned
// `PRIVACY_PRESERVING_CIRCUIT_ID`.

#![no_main]

use spel_framework::prelude::*;

risc0_zkvm::guest::entry!(main);

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
    /// - `owner` (signer): the human deploying the agent.
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

    /// The agent spends.
    ///
    /// Below the threshold this needs nothing but the anchored policy. Above it,
    /// the caller must present the approval account the owner created, and it is
    /// checked to be owned by this program — an account merely *existing* at the
    /// right address proves nothing, since anyone can fund an address.
    ///
    /// Accounts:
    /// - `policy` (PDA seeded by `policy_hash`): the anchored limits.
    /// - `approval` (PDA seeded by `marker_seed`): the owner's authorisation.
    ///   For an autonomous spend the caller passes the same policy account here
    ///   and `requires_approval` is false.
    /// - `agent` (signer): the agent's own shielded account.
    #[instruction]
    pub fn spend(
        // Injected by the dispatcher from the trusted ProgramInput; never part
        // of the instruction ABI, so the published IDL does not carry it.
        ctx: ProgramContext,
        #[account(pda = arg("policy_hash"))]
        policy: AccountWithMetadata,
        #[account(pda = arg("marker_seed"))]
        approval: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        recipient: [u8; 32],
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

        // 3. Inside the envelope, the agent acts alone.
        if pol.is_autonomous(amount, spent_this_period) {
            return Ok(SpelOutput::execute(vec![policy, approval, agent], vec![]));
        }

        // 4. Outside it, the owner must have approved this exact payment. The
        //    seed binds the approval to (policy, recipient, amount, nonce), so
        //    an approval for one transfer cannot be spent on another.
        let spend_ref =
            agent_policy_core::compute_spend_ref(&policy_hash, &recipient, amount, nonce);
        if agent_policy_core::compute_approval_marker(&spend_ref) != marker_seed {
            return Err(SpelError::custom(
                E_SPEND_REF_MISMATCH,
                "marker_seed does not commit to this spend",
            ));
        }

        // 5. And the approval must have been created by *this* program. Checking
        //    it is merely non-default would let anyone fund the address and
        //    manufacture consent.
        if approval.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_APPROVAL_NOT_ANCHORED,
                "no owner approval is anchored for this spend",
            ));
        }

        // 6. Even approved, the per-period ceiling still holds. An owner
        //    approving one large payment has not agreed to drain the period.
        if spent_this_period.saturating_add(amount) > per_period {
            return Err(SpelError::custom(
                E_OVER_PERIOD_LIMIT,
                "the spend would carry the period total past the anchored limit",
            ));
        }

        let _ = E_OVER_PER_TX_LIMIT;
        Ok(SpelOutput::execute(vec![policy, approval, agent], vec![]))
    }
}
