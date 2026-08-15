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
    ///
    /// That split is also the limit of what this crate can be tested for. Nothing
    /// in here accumulates: the running total, and the block window it resets on,
    /// live in the guest. So the unit tests below can only show that the
    /// comparison is right for a total handed to it — they cannot show that the
    /// total handed to it is right.
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

    // WHAT THESE TESTS DO NOT COVER
    //
    // Two of the properties the design leans on are out of reach from here, and
    // the tests below should not be read as covering them.
    //
    // 1. "A per-transaction cap is drained by repetition." `is_autonomous` takes
    //    `spent_this_period` as a parameter and this crate never accumulates
    //    anything, so that case is covered here as arithmetic only: given a total,
    //    the comparison is right. Whether the total is the real sum over the real
    //    window is the guest's job, and only a guest-level test reaches it.
    //
    // 2. "An approval cannot be spent twice." The marker names one spend, which is
    //    what `compute_approval_marker` is tested for — but single use comes from
    //    creating that account with `init`, so the second attempt fails at account
    //    creation. That is an on-chain property; no unit test in this crate can
    //    reach it.

    const OWNER: [u8; 32] = [1; 32];
    const AGENT: [u8; 32] = [2; 32];
    const BOB: [u8; 32] = [3; 32];

    fn policy() -> SpendPolicy {
        SpendPolicy { per_tx: 100, per_period: 250, period_blocks: 1000 }
    }

    /// SHA-256 over `separator || parts…`, spelled out so a test can ask what a
    /// digest *would* have been under a different separator, under none at all, or
    /// with two fields transposed. The functions under test must not agree with
    /// any of those.
    fn digest(separator: &[u8], parts: &[&[u8]]) -> [u8; 32] {
        let mut h = Sha256::new();
        h.update(separator);
        for part in parts {
            h.update(part);
        }
        h.finalize().into()
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
    fn the_same_policy_under_a_different_owner_is_a_different_policy() {
        // Two owners deploying the identical (agent, limits) must not derive the
        // same policy account. If they did, one owner's agent could present the
        // other's policy — and the address is the whole of the policy's authority,
        // so nothing downstream would notice.
        assert_ne!(
            compute_policy_hash(&OWNER, &AGENT, &policy()),
            compute_policy_hash(&[9; 32], &AGENT, &policy())
        );
    }

    #[test]
    fn the_policy_hash_folds_its_fields_in_one_fixed_order() {
        // `owner` and `agent` are both 32 bytes, and `per_tx` and `per_period` are
        // both u128. Transposing either pair is invisible to every test that varies
        // one field at a time, yet it collides two distinct policies onto one
        // address: (owner=A, agent=B) with (owner=B, agent=A), and a 1-per-tx /
        // 1000-per-period policy with a 1000-per-tx / 1-per-period one — the agent
        // then picks whichever reading of the account it prefers. Only pinning the
        // layout catches that, so the layout is pinned.
        let p = policy();
        let per_tx = p.per_tx.to_le_bytes();
        let per_period = p.per_period.to_le_bytes();
        let period_blocks = p.period_blocks.to_le_bytes();
        let parts: [&[u8]; 5] = [&OWNER, &AGENT, &per_tx, &per_period, &period_blocks];
        assert_eq!(compute_policy_hash(&OWNER, &AGENT, &p), digest(POLICY_HASH_PREFIX, &parts));
    }

    #[test]
    fn an_approval_is_bound_to_the_policy_it_was_granted_under() {
        // The owner approving "500 to Bob, nonce 7" under a tight policy is not
        // approving the identical payment under a different policy account. Drop
        // the policy from the spend reference and one marker satisfies the check
        // under every policy the agent can name: the owner's approval of a payment
        // under a 100-per-tx ceiling would also authorise it under a 10_000 one.
        // That is a replay across policies, which this derivation exists to stop.
        let raised = SpendPolicy { per_tx: 10_000, ..policy() };
        let tight = compute_policy_hash(&OWNER, &AGENT, &policy());
        let loose = compute_policy_hash(&OWNER, &AGENT, &raised);
        assert_ne!(tight, loose);

        let under_tight = compute_spend_ref(&tight, &BOB, 500, 7);
        let under_loose = compute_spend_ref(&loose, &BOB, 500, 7);
        assert_ne!(under_tight, under_loose);
        assert_ne!(compute_approval_marker(&under_tight), compute_approval_marker(&under_loose));
    }

    #[test]
    fn the_spend_ref_folds_its_fields_in_one_fixed_order() {
        // Same argument as for the policy hash: `policy_hash` and `recipient` are
        // both 32 bytes, so a transposition would let a spend to recipient R under
        // policy P share an approval with a spend to recipient P under policy R.
        let p = compute_policy_hash(&OWNER, &AGENT, &policy());
        let amount = 500u128.to_le_bytes();
        let nonce = 7u64.to_le_bytes();
        let parts: [&[u8]; 4] = [&p, &BOB, &amount, &nonce];
        assert_eq!(compute_spend_ref(&p, &BOB, 500, 7), digest(SPEND_REF_PREFIX, &parts));
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
    fn a_digest_made_for_one_role_is_not_a_digest_for_another() {
        // A hash produced for one role must never be valid in another. That the
        // three constants differ is necessary and nowhere near sufficient: it says
        // nothing about whether the functions use them, or use the right one.
        assert_ne!(POLICY_HASH_PREFIX, SPEND_REF_PREFIX);
        assert_ne!(SPEND_REF_PREFIX, APPROVAL_MARKER_PREFIX);
        assert_ne!(POLICY_HASH_PREFIX, APPROVAL_MARKER_PREFIX);

        // Feed all three functions the same material and require three digests.
        const X: [u8; 32] = [0xab; 32];
        let zero_policy = SpendPolicy { per_tx: 0, per_period: 0, period_blocks: 0 };
        let as_policy = compute_policy_hash(&X, &X, &zero_policy);
        let as_spend = compute_spend_ref(&X, &X, 0, 0);
        let as_marker = compute_approval_marker(&X);
        assert_ne!(as_policy, as_spend);
        assert_ne!(as_spend, as_marker);
        assert_ne!(as_policy, as_marker);

        // Those three would differ even with no separators at all, because the
        // three bodies are different lengths — which is an accident of the current
        // field list, not the guarantee. The guarantee is that each digest commits
        // to its own role, so assert that directly: no function may agree with the
        // same material hashed bare, or hashed under another role's separator.
        let zero_u128 = 0u128.to_le_bytes();
        let zero_u64 = 0u64.to_le_bytes();
        let policy_body: [&[u8]; 5] = [&X, &X, &zero_u128, &zero_u128, &zero_u64];
        let spend_body: [&[u8]; 4] = [&X, &X, &zero_u128, &zero_u64];
        let marker_body: [&[u8]; 1] = [&X];

        for (own, got, body) in [
            (POLICY_HASH_PREFIX, as_policy, &policy_body[..]),
            (SPEND_REF_PREFIX, as_spend, &spend_body[..]),
            (APPROVAL_MARKER_PREFIX, as_marker, &marker_body[..]),
        ] {
            assert_ne!(got, digest(b"", body), "the separator is not in the digest");
            for other in [POLICY_HASH_PREFIX, SPEND_REF_PREFIX, APPROVAL_MARKER_PREFIX] {
                if other != own {
                    assert_ne!(got, digest(other, body), "another role's separator");
                }
            }
        }
    }
}
