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
//! spend is rejected before the program body runs.
//!
//! THAT ALONE IS NOT ENOUGH, AND SAYING IT WAS IS HOW THIS BROKE
//!
//! An address that encodes limits stops an agent *editing* its policy. It does
//! not stop it *anchoring a different one*, and until the deployment that this
//! comment was written for, nothing did: `create_policy` took the owner as a
//! caller-supplied 32 bytes and never compared it to the account that signed,
//! and `spend` never compared the paying account to the agent the policy names.
//! Both holes were demonstrated against the deployed binary, not argued from the
//! source: a `create_policy` signed by an agent's own key, naming an invented
//! owner and `per_tx = u128::MAX`, was accepted, and a `spend` of that agent's
//! whole balance under it was accepted too. Halt 0 both times.
//!
//! So three bindings, not one, are what makes the ceiling a ceiling, and all
//! three live in the guest because only the guest sees who signed:
//!
//! 1. `create_policy` — the signer must BE the owner the policy commits to.
//! 2. `spend` / `spend_approved` — the paying account must BE the agent it names.
//! 3. `approve_spend` — the signer must be that same owner.
//!
//! Without (1) an attacker anchors a fresh unlimited policy under an owner id it
//! made up. With (1) but without (2) it anchors one under an owner id it really
//! controls and points the agent at it. With both but without (3) it signs its
//! own approvals and walks past the threshold. Each is three lines; any one of
//! them missing costs the whole property.
//!
//! WHAT THE OWNER SIGNS
//!
//! An above-threshold spend needs the owner. Rather than a signature the agent
//! could replay, the approval is a marker account seeded by the *exact* spend it
//! authorises — recipient, amount, and a nonce. Approving one payment cannot
//! approve another. `init` stops the same marker being created twice, and
//! `spend_approved` stamps the marker's data on the way through, so a marker
//! that exists is not the same thing as a marker that is still unspent.
//!
//! WHERE THE RUNNING TOTAL LIVES
//!
//! The per-period limit needs a total, and a total supplied by the caller is not
//! a limit — this crate previously said the on-chain program "re-derives it
//! rather than trusting the agent", and the program did no such thing: both
//! callers passed zero. The total now lives in the policy account's own data,
//! written by the program that owns that account, and [`SpendPolicy::authorize`]
//! below is the whole of the decision. The guest is an adapter around it, which
//! is what makes the tests at the bottom of this file worth reading.

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

/// The running total, and the period it belongs to.
///
/// This is the entire contents of the policy account's data. It is written by
/// the policy program, which owns that account — LEZ rule 6
/// (`UnauthorizedDataModification`) is what stops anyone else touching it — and
/// read back on the next spend.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct SpendLedger {
    /// First block of the period this total covers. Always a multiple of
    /// `period_blocks`.
    pub window_start: u64,
    /// Total moved unattended since `window_start`.
    pub spent: u128,
}

/// Why a spend was refused. Each maps to one on-chain error code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SpendRefusal {
    /// `period_blocks` is zero, so there is no period to align to.
    PeriodZero,
    /// The declared window does not start on a period boundary. Unaligned
    /// windows would let the agent slide the window forward one block at a time
    /// and never accumulate anything.
    WindowMisaligned,
    /// The declared window is older than the one the ledger already records —
    /// an attempt to spend against a period that has already closed.
    WindowRegressed,
    /// One transfer over the per-transaction limit.
    OverPerTx,
    /// The period total would pass the per-period limit.
    OverPerPeriod,
}

impl SpendPolicy {
    /// The period containing `block`: the largest multiple of `period_blocks`
    /// that is not greater than it.
    ///
    /// The chain has no clock a program can read — `ProgramInput` carries the
    /// program id, the caller, the pre-states and the instruction, and nothing
    /// else — so the *caller* names the period and the guest makes the naming
    /// binding by refusing anything unaligned and pinning the transaction's
    /// block validity window to exactly that period. A caller that names a later
    /// period gets a transaction that no block in this period will accept; one
    /// that names an earlier period is refused by [`SpendPolicy::authorize`].
    /// Neither can reset the total early, which is the only thing the window has
    /// to prevent.
    #[must_use]
    pub fn window_start_for(&self, block: u64) -> u64 {
        if self.period_blocks == 0 {
            return 0;
        }
        (block / self.period_blocks) * self.period_blocks
    }

    /// The block range a spend declared in `window_start` may be included in:
    /// `[window_start, window_start + period_blocks)`.
    #[must_use]
    pub fn window_bounds(&self, window_start: u64) -> Option<(u64, u64)> {
        let end = window_start.checked_add(self.period_blocks)?;
        Some((window_start, end))
    }

    /// May the agent move `amount` unattended, given what it has already moved?
    ///
    /// Returns the ledger to write back on success. This is the whole decision:
    /// the guest re-derives the policy hash, checks who signed, and then calls
    /// this — so what the tests below cover is what the chain enforces, not a
    /// comparison performed on a number the agent chose.
    pub fn authorize(
        &self,
        ledger: &SpendLedger,
        window_start: u64,
        amount: u128,
    ) -> Result<SpendLedger, SpendRefusal> {
        if self.period_blocks == 0 {
            return Err(SpendRefusal::PeriodZero);
        }
        if window_start % self.period_blocks != 0 {
            return Err(SpendRefusal::WindowMisaligned);
        }
        if window_start < ledger.window_start {
            return Err(SpendRefusal::WindowRegressed);
        }
        // A later window is a fresh budget; the same window continues the one
        // already recorded.
        let spent = if window_start == ledger.window_start {
            ledger.spent
        } else {
            0
        };
        if amount > self.per_tx {
            return Err(SpendRefusal::OverPerTx);
        }
        // Saturating, so a total near the top of the range cannot wrap into
        // "plenty left".
        let total = spent.saturating_add(amount);
        if total > self.per_period {
            return Err(SpendRefusal::OverPerPeriod);
        }
        Ok(SpendLedger {
            window_start,
            spent: total,
        })
    }
}

/// The policy account's data was not written by this program.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MalformedLedger;

impl SpendLedger {
    /// `window_start` little-endian, then `spent` little-endian.
    pub const ENCODED_LEN: usize = 24;

    /// Decode the policy account's data. Empty data is a policy that has been
    /// anchored and never spent against — `create_policy` writes nothing.
    pub fn decode(data: &[u8]) -> Result<Self, MalformedLedger> {
        if data.is_empty() {
            return Ok(Self::default());
        }
        if data.len() != Self::ENCODED_LEN {
            return Err(MalformedLedger);
        }
        let mut w = [0u8; 8];
        w.copy_from_slice(&data[..8]);
        let mut s = [0u8; 16];
        s.copy_from_slice(&data[8..Self::ENCODED_LEN]);
        Ok(Self {
            window_start: u64::from_le_bytes(w),
            spent: u128::from_le_bytes(s),
        })
    }

    #[must_use]
    pub fn encode(&self) -> [u8; Self::ENCODED_LEN] {
        let mut out = [0u8; Self::ENCODED_LEN];
        out[..8].copy_from_slice(&self.window_start.to_le_bytes());
        out[8..].copy_from_slice(&self.spent.to_le_bytes());
        out
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
    // The accumulation these tests drive is the accumulation the chain performs:
    // `authorize` returns the ledger the guest writes into the policy account,
    // and the guest does nothing to it but encode it. What is still out of reach
    // from here is everything about *who is asking* — that the signer is the
    // owner the policy names, that the payer is the agent it names, that the
    // policy account was created by this program and not merely funded at the
    // right address. Those are checks on account metadata, they are in the
    // guest, and only an on-chain run reaches them.
    //
    // Two more, likewise on-chain:
    //
    // 1. "A declared period cannot be a future one." That is enforced by the
    //    block validity window the guest pins to the transaction, which the
    //    state machine checks (`OutOfValidityWindow`). `window_start_for` and
    //    `window_bounds` are tested here as arithmetic; the enforcement is not
    //    arithmetic.
    //
    // 2. "An approval cannot be spent twice." The marker names one spend, which
    //    is what `compute_approval_marker` is tested for. Single use comes from
    //    `init` refusing to create the marker twice and from `spend_approved`
    //    stamping it on the way through — both on-chain properties.

    const OWNER: [u8; 32] = [1; 32];
    const AGENT: [u8; 32] = [2; 32];
    const BOB: [u8; 32] = [3; 32];

    fn policy() -> SpendPolicy {
        SpendPolicy { per_tx: 100, per_period: 250, period_blocks: 1000 }
    }

    /// The ledger as it is when a policy has just been anchored.
    fn fresh() -> SpendLedger {
        SpendLedger::decode(&[]).expect("empty data is a fresh ledger")
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
    fn a_small_spend_with_room_left_is_allowed() {
        let after = policy().authorize(&fresh(), 1000, 100).expect("inside the envelope");
        assert_eq!(after, SpendLedger { window_start: 1000, spent: 100 });
    }

    #[test]
    fn a_spend_over_the_per_transaction_limit_needs_the_owner() {
        assert_eq!(
            policy().authorize(&fresh(), 1000, 101),
            Err(SpendRefusal::OverPerTx)
        );
    }

    #[test]
    fn repetition_under_the_per_transaction_limit_hits_the_period_cap() {
        // The reason a per-transaction limit alone is not a limit: three spends
        // of 100 are each individually allowed, and together exceed 250. The
        // total is not handed in — it is carried from one call to the next, the
        // way the policy account carries it from one transaction to the next.
        let p = policy();
        let l = p.authorize(&fresh(), 1000, 100).expect("first");
        let l = p.authorize(&l, 1000, 100).expect("second");
        assert_eq!(l.spent, 200);
        assert_eq!(p.authorize(&l, 1000, 100), Err(SpendRefusal::OverPerPeriod));
        // And the one that fits still does.
        assert_eq!(p.authorize(&l, 1000, 50).expect("third").spent, 250);
    }

    #[test]
    fn the_next_period_starts_the_total_again() {
        let p = policy();
        let spent_out = SpendLedger { window_start: 1000, spent: 250 };
        assert_eq!(p.authorize(&spent_out, 1000, 1), Err(SpendRefusal::OverPerPeriod));
        assert_eq!(p.authorize(&spent_out, 2000, 100).expect("new period").spent, 100);
    }

    #[test]
    fn an_exhausted_period_cannot_be_reset_by_naming_an_older_one() {
        // The window the caller names is not free: a *later* one is pinned into
        // the transaction's block validity window and cannot be reached early,
        // and an *earlier* one is refused here. Without this an agent that
        // exhausted period 2000 would replay period 1000 for a fresh 250.
        let p = policy();
        let spent_out = SpendLedger { window_start: 2000, spent: 250 };
        assert_eq!(p.authorize(&spent_out, 1000, 10), Err(SpendRefusal::WindowRegressed));
    }

    #[test]
    fn a_window_that_does_not_start_on_a_period_boundary_is_refused() {
        // Sliding the window forward by one block each time would reset the
        // budget every block and make the period cap meaningless. Only multiples
        // of `period_blocks` are windows.
        let p = policy();
        assert_eq!(p.authorize(&fresh(), 1001, 10), Err(SpendRefusal::WindowMisaligned));
        assert_eq!(p.authorize(&fresh(), 1999, 10), Err(SpendRefusal::WindowMisaligned));
        assert!(p.authorize(&fresh(), 2000, 10).is_ok());
    }

    #[test]
    fn the_period_total_cannot_be_overflowed_past_the_cap() {
        // A ledger holding a hostile total must not wrap around into "plenty
        // left". It cannot be reached through `authorize`, which caps every
        // total at `per_period` — but the guest decodes this value from account
        // data, so the arithmetic has to hold for anything 24 bytes can say.
        let p = policy();
        let maxed = SpendLedger { window_start: 1000, spent: u128::MAX };
        assert_eq!(p.authorize(&maxed, 1000, 1), Err(SpendRefusal::OverPerPeriod));
    }

    #[test]
    fn a_policy_with_no_period_can_authorise_nothing() {
        // `create_policy` refuses to anchor one, so this is unreachable on
        // chain; it is here because the alternative to returning an error is a
        // division by zero inside the guest.
        let p = SpendPolicy { per_tx: 100, per_period: 250, period_blocks: 0 };
        assert_eq!(p.authorize(&fresh(), 0, 1), Err(SpendRefusal::PeriodZero));
        assert_eq!(p.window_start_for(12345), 0);
    }

    #[test]
    fn the_ledger_survives_the_round_trip_through_account_data() {
        let l = SpendLedger { window_start: 8000, spent: 1234567890123456789 };
        assert_eq!(SpendLedger::decode(&l.encode()).expect("round trip"), l);
        // A never-spent policy has empty data, and that is not an error.
        assert_eq!(SpendLedger::decode(&[]).expect("fresh"), SpendLedger::default());
        // Anything else was not written by this program.
        assert!(SpendLedger::decode(&[0u8; 23]).is_err());
        assert!(SpendLedger::decode(&[0u8; 25]).is_err());
    }

    #[test]
    fn the_window_a_block_falls_in_is_the_one_the_chain_will_accept() {
        let p = policy();
        assert_eq!(p.window_start_for(8629), 8000);
        assert_eq!(p.window_start_for(8000), 8000);
        assert_eq!(p.window_start_for(7999), 7000);
        // `is_valid_for` on chain is [from, to): the last block of the period is
        // start + period_blocks - 1, and the exclusive bound is the next period.
        assert_eq!(p.window_bounds(8000), Some((8000, 9000)));
        assert_eq!(p.window_bounds(u64::MAX), None);
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
