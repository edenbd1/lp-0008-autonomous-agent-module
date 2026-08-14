//! The agent's spending policy, and the derivations that put it on chain.
//!
//! WHY THIS IS NOT A CHECK IN THE AGENT PROCESS
//!
//! The prize asks for a threshold below which the agent acts alone and above
//! which it waits for its owner. Implemented as an `if` inside the agent, that
//! is worth nothing: the agent holds its own keys, so whoever controls the
//! process controls the spending, and the "limit" is a comment. An owner
//! deploying an autonomous agent on a remote node is trusting exactly the
//! machine most likely to be compromised.
//!
//! So the policy is anchored the way LP-0002 anchors a member set: the policy
//! account's *address* is derived from the policy itself. Changing a limit
//! changes the address, which resolves to an account nobody created, and the
//! spend is rejected before the program body runs. The agent cannot raise its
//! own ceiling, because the ceiling is not data it can write — it is the name of
//! the account it must present.
//!
//! WHAT THE OWNER SIGNS
//!
//! An above-threshold spend needs the owner. Rather than a signature the agent
//! could replay, the approval is a marker account seeded by the *exact* spend it
//! authorises — recipient, amount, and a nonce. Approving one payment cannot
//! approve another, and the marker is created with `init`, so it cannot be spent
//! twice.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use sha2::{Digest, Sha256};

/// Domain separators. Distinct prefixes keep a hash computed for one purpose
/// from ever being valid for another.
pub const POLICY_HASH_PREFIX: &[u8] = b"/lp-0008/v0.1/AgentPolicy/";
pub const SPEND_REF_PREFIX: &[u8] = b"/lp-0008/v0.1/SpendRef/";
pub const APPROVAL_MARKER_PREFIX: &[u8] = b"/lp-0008/v0.1/ApprovalMark/";

/// What the owner fixes when they deploy the agent.
///
/// `per_tx` bounds a single spend; `per_period` bounds the sum over a window of
/// `period_blocks`. Both are needed: a per-transaction limit alone is drained by
/// repetition, and a per-period limit alone permits one catastrophic transfer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpendPolicy {
    /// Largest single transfer the agent may make unattended.
    pub per_tx: u128,
    /// Largest total the agent may move within one period, unattended.
    pub per_period: u128,
    /// Length of that period, in blocks.
    pub period_blocks: u64,
}

impl SpendPolicy {
    /// Is this spend inside the autonomous envelope?
    ///
    /// `spent_this_period` is what the agent has already moved in the current
    /// window. The caller supplies it; the on-chain program re-derives it rather
    /// than trusting the agent, which is why this is a pure function here.
    #[must_use]
    pub fn is_autonomous(&self, amount: u128, spent_this_period: u128) -> bool {
        amount <= self.per_tx && spent_this_period.saturating_add(amount) <= self.per_period
    }
}

/// The commitment the policy account's address is derived from.
///
/// Every field is folded in, so no two policies share an address and a policy
/// cannot be edited in place — an edited policy is simply a different account.
#[must_use]
pub fn compute_policy_hash(owner: &[u8; 32], agent: &[u8; 32], policy: &SpendPolicy) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(POLICY_HASH_PREFIX);
    h.update(owner);
    h.update(agent);
    h.update(policy.per_tx.to_le_bytes());
    h.update(policy.per_period.to_le_bytes());
    h.update(policy.period_blocks.to_le_bytes());
    h.finalize().into()
}

/// Names one specific spend: this policy, this recipient, this amount, once.
///
/// The nonce is what stops an approval being replayed for an identical later
/// payment. Without it, "send 100 to Bob" approved today authorises the same
/// transfer forever.
#[must_use]
pub fn compute_spend_ref(
    policy_hash: &[u8; 32],
    recipient: &[u8; 32],
    amount: u128,
    nonce: u64,
) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(SPEND_REF_PREFIX);
    h.update(policy_hash);
    h.update(recipient);
    h.update(amount.to_le_bytes());
    h.update(nonce.to_le_bytes());
    h.finalize().into()
}

/// Seed of the account that records "the owner approved this exact spend".
#[must_use]
pub fn compute_approval_marker(spend_ref: &[u8; 32]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(APPROVAL_MARKER_PREFIX);
    h.update(spend_ref);
    h.finalize().into()
}

#[cfg(test)]
mod tests {
    use super::*;

    const OWNER: [u8; 32] = [1; 32];
    const AGENT: [u8; 32] = [2; 32];
    const BOB: [u8; 32] = [3; 32];

    fn policy() -> SpendPolicy {
        SpendPolicy { per_tx: 100, per_period: 250, period_blocks: 1000 }
    }

    #[test]
    fn a_small_spend_with_room_left_is_autonomous() {
        assert!(policy().is_autonomous(100, 0));
        assert!(policy().is_autonomous(50, 200));
    }

    #[test]
    fn a_spend_over_the_per_transaction_limit_needs_the_owner() {
        assert!(!policy().is_autonomous(101, 0));
    }

    #[test]
    fn repetition_under_the_per_transaction_limit_still_hits_the_period_cap() {
        // The reason a per-transaction limit alone is not a limit: three spends
        // of 100 are each individually allowed, and together exceed 250.
        assert!(policy().is_autonomous(100, 0));
        assert!(policy().is_autonomous(100, 100));
        assert!(!policy().is_autonomous(100, 200));
    }

    #[test]
    fn the_period_total_cannot_be_overflowed_past_the_cap() {
        // A hostile `spent_this_period` must not wrap around into "plenty left".
        assert!(!policy().is_autonomous(1, u128::MAX));
    }

    #[test]
    fn changing_any_limit_changes_the_policy_address() {
        let base = compute_policy_hash(&OWNER, &AGENT, &policy());
        for altered in [
            SpendPolicy { per_tx: 101, ..policy() },
            SpendPolicy { per_period: 251, ..policy() },
            SpendPolicy { period_blocks: 1001, ..policy() },
        ] {
            assert_ne!(base, compute_policy_hash(&OWNER, &AGENT, &altered));
        }
    }

    #[test]
    fn the_same_policy_under_a_different_agent_is_a_different_policy() {
        assert_ne!(
            compute_policy_hash(&OWNER, &AGENT, &policy()),
            compute_policy_hash(&OWNER, &[9; 32], &policy())
        );
    }

    #[test]
    fn an_approval_names_one_spend_and_not_another() {
        let p = compute_policy_hash(&OWNER, &AGENT, &policy());
        let a = compute_spend_ref(&p, &BOB, 500, 0);
        // Same recipient and amount, later nonce: a distinct authorisation.
        assert_ne!(a, compute_spend_ref(&p, &BOB, 500, 1));
        // Same nonce, larger amount: cannot ride on the first approval.
        assert_ne!(a, compute_spend_ref(&p, &BOB, 501, 0));
        // Same spend, different recipient.
        assert_ne!(a, compute_spend_ref(&p, &[4; 32], 500, 0));
    }

    #[test]
    fn the_marker_is_a_function_of_the_spend_alone() {
        let p = compute_policy_hash(&OWNER, &AGENT, &policy());
        let r = compute_spend_ref(&p, &BOB, 500, 7);
        assert_eq!(compute_approval_marker(&r), compute_approval_marker(&r));
        assert_ne!(
            compute_approval_marker(&r),
            compute_approval_marker(&compute_spend_ref(&p, &BOB, 500, 8))
        );
    }

    #[test]
    fn the_domain_separators_are_distinct() {
        // A hash produced for one role must never be valid in another.
        assert_ne!(POLICY_HASH_PREFIX, SPEND_REF_PREFIX);
        assert_ne!(SPEND_REF_PREFIX, APPROVAL_MARKER_PREFIX);
        assert_ne!(POLICY_HASH_PREFIX, APPROVAL_MARKER_PREFIX);
    }
}
