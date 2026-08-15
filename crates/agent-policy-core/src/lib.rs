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
//! ANCHORING IS A TWO-PARTY ACT, AND THAT IS THE WHOLE PROPERTY
//!
//! The address of an agent's policy account is `PDA(program,
//! ["agent-policy/v1", agent])` — the agent, and nothing else. That much is
//! right and it is kept. What it does not decide, and what the previous
//! deployment got badly wrong, is **who may write there**.
//!
//! That deployment declared exactly two accounts on `create_policy`: the policy
//! account and a signer recorded as its owner. `agent_id` was a free `[u8; 32]`
//! argument, discarded by the body, and the agent's account was never declared,
//! never read and never asked to sign. So anchoring a policy over somebody
//! else's agent needed **no key at all** — only the agent's public id, which
//! this repository publishes in `artifacts/agents.tsv` and inside every signed
//! Agent Card. Executed against the deployed binary: an unrelated account
//! anchored `per_tx = per_period = u128::MAX` over an agent it did not control
//! and was accepted; the honest owner's later anchor was then refused
//! `AccountAlreadyInitialized`, as was every variant of it, for the life of that
//! agent identity. `per_tx = 0` instead of `u128::MAX` makes the same call a
//! permanent denial of service. The predecessor design at least required the
//! agent's key; this one required nothing.
//!
//! One address per agent is therefore necessary and nowhere near sufficient.
//! The missing half is consent, and it is now two signatures, taken in two
//! separate transactions from two separate wallets:
//!
//!   1. `claim_agent` — the **agent** signs, and designates the account id that
//!      is allowed to anchor its policy. It writes an [`OwnerClaim`] into
//!      `PDA(program, ["agent-owner/v1", agent])`, an address derived from the
//!      *signing* account, so there is no `agent_id` argument to lie about.
//!   2. `create_policy` — the **designated owner** signs. It reads that claim
//!      account and refuses unless the signer is the id the agent named.
//!
//! A third party holds neither key, so it can do neither step: it cannot claim
//! an agent it cannot sign for, and it cannot anchor over an agent that has
//! designated somebody else. Both refusals are error codes rather than
//! `AccountAlreadyInitialized`, so the reason is legible.
//!
//! Splitting it in two is what makes it deployable. `spel` signs only for
//! accounts an instruction declares as signers, and only with keys the *one*
//! wallet home it is pointed at actually holds. One instruction requiring both
//! signatures would require the owner's key and the agent's key in the same
//! wallet — which is the assumption the whole design exists to avoid. Two
//! single-signer instructions keep the agent's key on the agent's node and the
//! owner's key with the owner.
//!
//! LOSING THE RACE IS NOT PERMANENT
//!
//! `init` cannot be undone: LEZ rule 4 forbids changing an account's program
//! owner (`ModifiedProgramOwner`, `program/mod.rs:697-703`), so an account this
//! program has claimed is this program's forever and no `close` can exist. The
//! recovery therefore has to live in the record rather than in the address, and
//! it does: `update_policy` re-fixes both limits and the period in place, on the
//! signature of the owner the record names. An envelope that was anchored wrong
//! is corrected rather than abandoned, and an owner who believes the agent is
//! compromised can set `per_tx = 0` and stop it spending unattended at all,
//! which nothing in the previous design could do.
//!
//! What stays irreversible is the agent's own designation of its owner. It has
//! to: an agent that could re-designate could name itself and then anchor its
//! own unlimited policy, which is the attack. The cost of getting it wrong is
//! bounded — a private account costs nothing to create and the agent's balance
//! is never trapped, since the balance is held by the transfer program and moves
//! on the agent's own key — so the remedy is a new agent identity, and no third
//! party can force it.
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
//! An approval also **expires**. It used to be a bearer instrument: a marker,
//! once created, authorised its payment in every block forever, redeemable the
//! day the agent's key was stolen and against a period ledger it does not draw
//! on. The owner now names the last block it is good for, that block is written
//! into the marker by [`ApprovalRecord`], and `spend_approved` pins the
//! transaction's own block validity window to it — so an expired approval is not
//! refused by this program, it is a transaction no block will include.
//!
//! Who the owner *is* is not an argument either. `approve_spend` reads it out of
//! the policy account and compares it to the signer, so a compromised agent
//! signing its own approvals is refused by the same account it would have had to
//! overwrite.
//!
//! WHERE THE RUNNING TOTAL LIVES
//!
//! The per-period limit needs a total, and a total supplied by the caller is not
//! a limit — this crate previously said the on-chain program "re-derives it
//! rather than trusting the agent", and the program did no such thing: both
//! callers passed zero. The total lives in the policy account's own data,
//! alongside the limits it is measured against, and [`SpendPolicy::authorize`]
//! below is the whole of the decision. The guest is an adapter around it, which
//! is what makes the tests at the bottom of this file worth reading.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use sha2::{Digest, Sha256};

/// The constant first seed of every policy account's address. The second is the
/// agent's own account id, and there are no others.
///
/// It is here rather than spelled out at each call site because it is
/// consensus: the deployment script resolves the address through `spel pda`
/// against the published IDL, and the adversarial harness recomputes it to build
/// pre-states. The guest is the one place that cannot use this constant —
/// `#[account(pda = [literal("agent-policy/v1"), …])]` needs a string literal at
/// macro-expansion time — so the harness recomputing the PDA from *this* value
/// and then running the committed ELF is what pins the two together: if they
/// ever drift, the macro's own PDA check fails every case in the suite.
///
/// Domain separation, not decoration: approval markers are PDAs of this same
/// program seeded by a single 32-byte value. Without a distinct prefix a caller
/// could pass an unspent approval's seed as `agent_id` and squat that address
/// with a policy account, blocking one specific owner approval before it was
/// ever granted. `seed_from_str` zero-pads to 32 bytes and panics past that, so
/// this must stay short.
pub const POLICY_PDA_PREFIX: &str = "agent-policy/v1";

/// The constant first seed of every owner-claim account's address. The second is
/// the agent's own account id, exactly as for the policy account.
///
/// It has to differ from [`POLICY_PDA_PREFIX`], and the reason is not tidiness:
/// with a shared prefix an agent's claim account and its policy account would be
/// the same address, `create_policy` would find a claim where it expects a
/// policy record, and `claim_agent` would be refused by `init` for an agent that
/// had merely been anchored. Two prefixes, two accounts, one agent.
///
/// Both are two-seed PDAs of 32 bytes each, while an approval marker is a
/// one-seed PDA, so no marker address can collide with either — which matters,
/// because a marker seed is derivable by anyone who knows the payment.
pub const OWNER_CLAIM_PDA_PREFIX: &str = "agent-owner/v1";

/// Domain separators. Distinct prefixes keep a hash computed for one purpose
/// from ever being valid for another.
pub const SPEND_REF_PREFIX: &[u8] = b"/lp-0008/v0.2/SpendRef/";
pub const APPROVAL_MARKER_PREFIX: &[u8] = b"/lp-0008/v0.2/ApprovalMark/";

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
    /// the guest checks who signed, reads the anchored limits out of the policy
    /// account, and then calls this — so what the tests below cover is what the
    /// chain enforces, not a comparison performed on numbers the agent chose.
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

/// The account's data was not written by this program, or was written under a
/// layout this build does not know.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MalformedRecord;

/// The agent's own statement of which account may anchor its policy.
///
/// This is the whole contents of `PDA(program, ["agent-owner/v1", agent])`, and
/// it is the account that makes anchoring a two-party act. It is written by
/// `claim_agent`, whose policy-account-shaped guarantee is that the *agent*
/// signs and the address is derived from the signing account — so nobody can
/// write here for an agent whose key they do not hold, and there is no
/// `agent_id` argument to disagree with.
///
/// `create_policy` reads it and refuses any signer that is not `owner`. That is
/// the difference between the previous deployment, where an anchor needed no key
/// whatsoever, and this one, where it needs two.
///
/// One field, no limits, no period: the claim says *who decides*, never *what
/// was decided*. Keeping the terms out of it is what lets `update_policy` change
/// them later without the agent having to sign again — and what stops a
/// compromised agent from mattering here at all, since by the time it is
/// compromised this account already exists and `init` will not have it twice.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OwnerClaim {
    /// The account id `create_policy` will require as its signer.
    pub owner: [u8; 32],
}

impl OwnerClaim {
    /// Layout version. Distinct from [`PolicyRecord::VERSION`] so that even a
    /// hypothetical address collision could not have one read as the other.
    pub const VERSION: u8 = 2;
    /// `version || owner`.
    pub const ENCODED_LEN: usize = 1 + 32;

    #[must_use]
    pub fn encode(&self) -> [u8; Self::ENCODED_LEN] {
        let mut out = [0u8; Self::ENCODED_LEN];
        out[0] = Self::VERSION;
        out[1..33].copy_from_slice(&self.owner);
        out
    }

    /// Decode a claim account's data. Fails closed, and in particular refuses a
    /// claim naming the all-zero account: no key produces that id, so a policy
    /// keyed to it could never be anchored by anybody and the agent identity
    /// would be spent for nothing.
    pub fn decode(data: &[u8]) -> Result<Self, MalformedRecord> {
        if data.len() != Self::ENCODED_LEN || data[0] != Self::VERSION {
            return Err(MalformedRecord);
        }
        let mut owner = [0u8; 32];
        owner.copy_from_slice(&data[1..33]);
        if owner == [0u8; 32] {
            return Err(MalformedRecord);
        }
        Ok(Self { owner })
    }
}

/// Everything the chain knows about one agent's envelope: who owns it, what it
/// permits, and what has been spent against it.
///
/// This is the entire contents of the policy account's data, and the account's
/// address is a function of the agent alone — so these fields are what an
/// attacker would have had to overwrite, and rule 6
/// (`UnauthorizedDataModification`) is what stops it. The previous design put
/// `owner` and the limits in the *address* instead, which sounds stronger and is
/// weaker: an address the caller picks is an address the caller can pick a
/// different one of.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PolicyRecord {
    /// The account that signed `create_policy`. Not an argument — the guest
    /// writes the signer's own id here, so there is nothing to disagree with.
    pub owner: [u8; 32],
    /// The envelope, fixed at anchoring.
    pub policy: SpendPolicy,
    /// What has been spent inside the current period.
    pub ledger: SpendLedger,
}

impl PolicyRecord {
    /// Layout version. A record this program did not write, or wrote under a
    /// different layout, decodes to an error rather than to a policy — which is
    /// the difference between "no ceiling" and "refuse".
    pub const VERSION: u8 = 1;
    /// `version || owner || per_tx || per_period || period_blocks ||
    /// window_start || spent`, every integer little-endian.
    pub const ENCODED_LEN: usize = 1 + 32 + 16 + 16 + 8 + 8 + 16;

    #[must_use]
    pub fn encode(&self) -> [u8; Self::ENCODED_LEN] {
        let mut out = [0u8; Self::ENCODED_LEN];
        out[0] = Self::VERSION;
        out[1..33].copy_from_slice(&self.owner);
        out[33..49].copy_from_slice(&self.policy.per_tx.to_le_bytes());
        out[49..65].copy_from_slice(&self.policy.per_period.to_le_bytes());
        out[65..73].copy_from_slice(&self.policy.period_blocks.to_le_bytes());
        out[73..81].copy_from_slice(&self.ledger.window_start.to_le_bytes());
        out[81..97].copy_from_slice(&self.ledger.spent.to_le_bytes());
        out
    }

    /// Decode the policy account's data.
    ///
    /// Empty data is an error here, and that is the point: `create_policy`
    /// writes a full record, so an account this program owns always has one. An
    /// empty-data policy account would be an account created some other way, and
    /// treating it as a default policy would mean a zero envelope at best and a
    /// missing one at worst. Refuse instead.
    pub fn decode(data: &[u8]) -> Result<Self, MalformedRecord> {
        if data.len() != Self::ENCODED_LEN || data[0] != Self::VERSION {
            return Err(MalformedRecord);
        }
        let mut owner = [0u8; 32];
        owner.copy_from_slice(&data[1..33]);
        Ok(Self {
            owner,
            policy: SpendPolicy {
                per_tx: u128_le(&data[33..49]),
                per_period: u128_le(&data[49..65]),
                period_blocks: u64_le(&data[65..73]),
            },
            ledger: SpendLedger {
                window_start: u64_le(&data[73..81]),
                spent: u128_le(&data[81..97]),
            },
        })
    }
}

/// What an owner approval says beyond the fact that it exists.
///
/// The approval account's *address* already commits to the payment — agent,
/// recipient, amount, nonce, through [`compute_spend_ref`] and
/// [`compute_approval_marker`] — so the only things left to record are the two
/// that the address cannot carry because they change: when it stops being good,
/// and whether it has been used.
///
/// Both were missing. The account used to hold empty data until it was spent and
/// a single byte afterwards, which gave single use and nothing else: an approval
/// was valid in **every block, forever**, so an owner who approved one payment
/// had written a bearer instrument that an attacker could redeem the day it
/// stole the agent's key, months later, against a period ledger the approved
/// path does not draw on. There was no way back either — `approve_spend` is
/// `init`, so the marker cannot be re-issued to cancel it.
///
/// `expiry_block` is the fix and it is enforced by the chain rather than by this
/// program: `spend_approved` pins the transaction's block validity window to
/// `[0, expiry_block)`, and a transaction outside its window is rejected by the
/// state machine with `OutOfValidityWindow`. So an expired approval is not an
/// approval this program refuses — it is one that no block will accept.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ApprovalRecord {
    /// First block at which this approval is no longer redeemable. The
    /// transaction that spends it must land strictly before this.
    pub expiry_block: u64,
    /// Set by `spend_approved` as it consumes the approval. `init` gives single
    /// *issue*; this is what gives single *use*.
    pub spent: bool,
}

impl ApprovalRecord {
    /// Layout version, distinct from the other two records in this crate.
    pub const VERSION: u8 = 3;
    /// `version || expiry_block || spent`, the integer little-endian.
    pub const ENCODED_LEN: usize = 1 + 8 + 1;

    #[must_use]
    pub fn encode(&self) -> [u8; Self::ENCODED_LEN] {
        let mut out = [0u8; Self::ENCODED_LEN];
        out[0] = Self::VERSION;
        out[1..9].copy_from_slice(&self.expiry_block.to_le_bytes());
        out[9] = u8::from(self.spent);
        out
    }

    /// Decode an approval account's data.
    ///
    /// An expiry of zero is refused here as well as at issuing time: `[0, 0)` is
    /// an empty block range, which `SpelOutput::try_with_block_validity_window`
    /// rejects, and an approval that cannot be pinned to a window is exactly the
    /// unbounded bearer instrument this record exists to prevent. Anything but
    /// `0` or `1` in the `spent` byte is refused too — a marker this program
    /// wrote only ever holds those, so a third value is data it did not write.
    pub fn decode(data: &[u8]) -> Result<Self, MalformedRecord> {
        if data.len() != Self::ENCODED_LEN || data[0] != Self::VERSION {
            return Err(MalformedRecord);
        }
        let expiry_block = u64_le(&data[1..9]);
        if expiry_block == 0 {
            return Err(MalformedRecord);
        }
        let spent = match data[9] {
            0 => false,
            1 => true,
            _ => return Err(MalformedRecord),
        };
        Ok(Self {
            expiry_block,
            spent,
        })
    }
}

fn u64_le(b: &[u8]) -> u64 {
    let mut x = [0u8; 8];
    x.copy_from_slice(b);
    u64::from_le_bytes(x)
}

fn u128_le(b: &[u8]) -> u128 {
    let mut x = [0u8; 16];
    x.copy_from_slice(b);
    u128::from_le_bytes(x)
}

/// Names one specific spend: this agent, this recipient, this amount, once.
///
/// The agent identifies the policy, because there is exactly one policy account
/// per agent and its address is derived from this same value. The nonce is what
/// stops an approval being replayed for an identical later payment: without it,
/// "send 100 to Bob" approved today authorises the same transfer forever.
#[must_use]
pub fn compute_spend_ref(
    agent: &[u8; 32],
    recipient: &[u8; 32],
    amount: u128,
    nonce: u64,
) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(SPEND_REF_PREFIX);
    h.update(agent);
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
    // and the guest does nothing to it but put it back in the record. What is
    // still out of reach from here is everything about *who is asking* — that
    // the payer is the agent whose address the policy account derives from, that
    // the signer of an approval is the owner the record names, that the policy
    // account was created by this program and not merely funded at the right
    // address. Those are checks on account metadata, they are in the guest, and
    // only an on-chain run — or the adversarial harness in
    // `crates/agent-verifier-adversarial`, which executes the committed ELF —
    // reaches them.
    //
    // Two more, likewise on-chain:
    //
    // 1. "A declared period cannot be a future one." That is enforced by the
    //    block validity window the guest pins to the transaction, which the
    //    state machine checks (`OutOfValidityWindow`). `window_start_for` and
    //    `window_bounds` are tested here as arithmetic; the enforcement is not
    //    arithmetic.
    //
    // 2. "No third party can bind an agent to a policy." That is two account
    //    checks in the guest — `claim_agent` deriving the claim account's
    //    address from the account that signs, and `create_policy` refusing a
    //    signer that is not the id that claim names. Neither is arithmetic and
    //    neither can be reached from here. The harness in
    //    `crates/agent-verifier-adversarial` proves both against the binary, and
    //    the two-step attack it runs end to end is the reason to read it.
    //
    // 3. "An approval stops being redeemable." The expiry lives in
    //    [`ApprovalRecord`] and is tested below as a round trip; the enforcement
    //    is the block validity window the guest pins to the transaction, which
    //    the state machine checks. Encoding is arithmetic, expiry is not.

    const OWNER: [u8; 32] = [1; 32];
    const AGENT: [u8; 32] = [2; 32];
    const BOB: [u8; 32] = [3; 32];

    fn policy() -> SpendPolicy {
        SpendPolicy { per_tx: 100, per_period: 250, period_blocks: 1000 }
    }

    /// The record as `create_policy` writes it, before anything is spent.
    fn anchored() -> PolicyRecord {
        PolicyRecord { owner: OWNER, policy: policy(), ledger: SpendLedger::default() }
    }

    fn fresh() -> SpendLedger {
        SpendLedger::default()
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
        // data, so the arithmetic has to hold for anything the record can say.
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

    // ── the policy record, which is now where the ceiling lives ──────────

    #[test]
    fn the_record_survives_the_round_trip_through_account_data() {
        let mut r = anchored();
        r.ledger = SpendLedger { window_start: 8000, spent: 1234567890123456789 };
        assert_eq!(PolicyRecord::decode(&r.encode()).expect("round trip"), r);
        assert_eq!(PolicyRecord::ENCODED_LEN, 97);
    }

    #[test]
    fn every_field_of_the_record_is_carried_and_none_is_transposed() {
        // owner is 32 bytes and so is nothing else here, but `per_tx` and
        // `per_period` are both u128 and `period_blocks` and `window_start` are
        // both u64. Transposing either pair round-trips perfectly and silently
        // swaps a 1-per-transaction envelope for a 1000-per-transaction one. So
        // vary each field alone and require the encoding to move.
        let base = anchored();
        let variants = [
            PolicyRecord { owner: [9; 32], ..base },
            PolicyRecord { policy: SpendPolicy { per_tx: 101, ..policy() }, ..base },
            PolicyRecord { policy: SpendPolicy { per_period: 251, ..policy() }, ..base },
            PolicyRecord { policy: SpendPolicy { period_blocks: 1001, ..policy() }, ..base },
            PolicyRecord { ledger: SpendLedger { window_start: 1000, spent: 0 }, ..base },
            PolicyRecord { ledger: SpendLedger { window_start: 0, spent: 1 }, ..base },
        ];
        for v in variants {
            assert_ne!(base.encode(), v.encode());
            assert_eq!(PolicyRecord::decode(&v.encode()).expect("round trip"), v);
        }
        // And the fields land where the layout says, not merely somewhere
        // reversible: a decoder written against this comment must agree.
        let e = base.encode();
        assert_eq!(e[0], PolicyRecord::VERSION);
        assert_eq!(&e[1..33], &OWNER);
        assert_eq!(u128_le(&e[33..49]), policy().per_tx);
        assert_eq!(u128_le(&e[49..65]), policy().per_period);
        assert_eq!(u64_le(&e[65..73]), policy().period_blocks);
    }

    #[test]
    fn data_this_program_did_not_write_is_not_a_policy() {
        // The guest reads this off an account it owns, so in practice only it
        // writes here — but "the ceiling could not be read" must fail closed,
        // not decode to something permissive. Empty data included: an anchored
        // policy always carries a record, so empty means the account was made
        // some other way.
        assert!(PolicyRecord::decode(&[]).is_err());
        assert!(PolicyRecord::decode(&[0u8; PolicyRecord::ENCODED_LEN - 1]).is_err());
        assert!(PolicyRecord::decode(&[0u8; PolicyRecord::ENCODED_LEN + 1]).is_err());
        // Right length, wrong version.
        let mut wrong = anchored().encode();
        wrong[0] = PolicyRecord::VERSION + 1;
        assert!(PolicyRecord::decode(&wrong).is_err());
        // The old 24-byte ledger from the previous deployment is not a record.
        assert!(PolicyRecord::decode(&[0u8; 24]).is_err());
    }

    // ── the owner claim, which is what makes anchoring two-party ─────────

    #[test]
    fn the_claim_carries_the_owner_and_nothing_else() {
        let c = OwnerClaim { owner: OWNER };
        assert_eq!(OwnerClaim::decode(&c.encode()).expect("round trip"), c);
        assert_eq!(OwnerClaim::ENCODED_LEN, 33);
        // The bytes land where the layout says, because `create_policy` compares
        // its signer against exactly these 32.
        assert_eq!(c.encode()[0], OwnerClaim::VERSION);
        assert_eq!(&c.encode()[1..33], &OWNER);
    }

    #[test]
    fn a_claim_naming_nobody_is_not_a_claim() {
        // An agent that designated the all-zero account would have designated an
        // id no key produces: `create_policy` could never be signed for it, so
        // the agent identity would be spent on an envelope nobody can anchor.
        // `claim_agent` refuses it and so does this decoder, which is the half
        // that matters — the guest reads the account rather than the call.
        let zero = OwnerClaim { owner: [0; 32] };
        assert!(OwnerClaim::decode(&zero.encode()).is_err());
        assert!(OwnerClaim::decode(&[]).is_err());
        assert!(OwnerClaim::decode(&[0u8; OwnerClaim::ENCODED_LEN - 1]).is_err());
        let mut wrong = OwnerClaim { owner: OWNER }.encode();
        wrong[0] = OwnerClaim::VERSION + 1;
        assert!(OwnerClaim::decode(&wrong).is_err());
    }

    #[test]
    fn the_three_records_cannot_be_read_as_one_another() {
        // Their addresses are separated by prefix and seed count, so this is
        // belt and braces — but a decoder that accepted another record's bytes
        // would turn an address mistake into a silent policy, and each of these
        // decides who may spend what.
        assert_ne!(POLICY_PDA_PREFIX, OWNER_CLAIM_PDA_PREFIX);
        let claim = OwnerClaim { owner: OWNER }.encode();
        let policy = anchored().encode();
        let approval = ApprovalRecord { expiry_block: 9000, spent: false }.encode();
        assert!(PolicyRecord::decode(&claim).is_err());
        assert!(PolicyRecord::decode(&approval).is_err());
        assert!(OwnerClaim::decode(&policy).is_err());
        assert!(OwnerClaim::decode(&approval).is_err());
        assert!(ApprovalRecord::decode(&policy).is_err());
        assert!(ApprovalRecord::decode(&claim).is_err());
        // Three layouts, three version bytes.
        assert_eq!(
            [PolicyRecord::VERSION, OwnerClaim::VERSION, ApprovalRecord::VERSION],
            [1, 2, 3]
        );
    }

    #[test]
    fn the_claim_prefix_is_a_usable_pda_seed() {
        // Same trap as `POLICY_PDA_PREFIX`: `seed_from_str` zero-pads to 32
        // bytes and panics past that, inside the guest, where a panic is a halt
        // with no error code.
        assert!(!OWNER_CLAIM_PDA_PREFIX.is_empty());
        assert!(OWNER_CLAIM_PDA_PREFIX.len() <= 32);
    }

    // ── approvals ────────────────────────────────────────────────────────

    #[test]
    fn an_approval_carries_an_expiry_and_a_use_flag() {
        let a = ApprovalRecord { expiry_block: 9000, spent: false };
        assert_eq!(ApprovalRecord::decode(&a.encode()).expect("round trip"), a);
        let used = ApprovalRecord { spent: true, ..a };
        assert_eq!(ApprovalRecord::decode(&used.encode()).expect("round trip"), used);
        // Issued and spent differ in the byte `spend_approved` writes, so an
        // unspent approval cannot be produced by truncating a spent one.
        assert_ne!(a.encode(), used.encode());
        assert_eq!(ApprovalRecord::ENCODED_LEN, 10);
    }

    #[test]
    fn an_approval_with_no_expiry_is_not_an_approval() {
        // `[0, 0)` is an empty block range and cannot be pinned to a
        // transaction, so an approval that says zero is the unbounded bearer
        // instrument this record exists to prevent. Refuse rather than treat it
        // as "never expires".
        assert!(ApprovalRecord::decode(&ApprovalRecord { expiry_block: 0, spent: false }.encode()).is_err());
        // And a `spent` byte this program never wrote is data it never wrote.
        let mut odd = ApprovalRecord { expiry_block: 9000, spent: false }.encode();
        odd[9] = 2;
        assert!(ApprovalRecord::decode(&odd).is_err());
        // The empty account the previous deployment used as "issued, unspent"
        // is not a record, so a marker made by that program cannot be redeemed
        // by this one.
        assert!(ApprovalRecord::decode(&[]).is_err());
        assert!(ApprovalRecord::decode(&[1]).is_err());
    }


    #[test]
    fn an_approval_is_bound_to_the_agent_it_was_granted_for() {
        // The owner approving "500 to Bob, nonce 7" for one agent is not
        // approving the identical payment for another. The agent is what selects
        // the policy account now, so binding the spend reference to it is what
        // stops one owner's approval being presented by a different agent.
        let mine = compute_spend_ref(&AGENT, &BOB, 500, 7);
        let theirs = compute_spend_ref(&[9; 32], &BOB, 500, 7);
        assert_ne!(mine, theirs);
        assert_ne!(compute_approval_marker(&mine), compute_approval_marker(&theirs));
    }

    #[test]
    fn the_spend_ref_folds_its_fields_in_one_fixed_order() {
        // `agent` and `recipient` are both 32 bytes, so a transposition would let
        // a spend to recipient R by agent A share an approval with a spend to
        // recipient A by agent R.
        let amount = 500u128.to_le_bytes();
        let nonce = 7u64.to_le_bytes();
        let parts: [&[u8]; 4] = [&AGENT, &BOB, &amount, &nonce];
        assert_eq!(
            compute_spend_ref(&AGENT, &BOB, 500, 7),
            digest(SPEND_REF_PREFIX, &parts)
        );
        assert_ne!(
            compute_spend_ref(&AGENT, &BOB, 500, 7),
            compute_spend_ref(&BOB, &AGENT, 500, 7)
        );
    }

    #[test]
    fn an_approval_names_one_spend_and_not_another() {
        let a = compute_spend_ref(&AGENT, &BOB, 500, 0);
        // Same recipient and amount, later nonce: a distinct authorisation.
        assert_ne!(a, compute_spend_ref(&AGENT, &BOB, 500, 1));
        // Same nonce, larger amount: cannot ride on the first approval.
        assert_ne!(a, compute_spend_ref(&AGENT, &BOB, 501, 0));
        // Same spend, different recipient.
        assert_ne!(a, compute_spend_ref(&AGENT, &[4; 32], 500, 0));
    }

    #[test]
    fn the_marker_is_a_function_of_the_spend_alone() {
        let r = compute_spend_ref(&AGENT, &BOB, 500, 7);
        assert_eq!(compute_approval_marker(&r), compute_approval_marker(&r));
        assert_ne!(
            compute_approval_marker(&r),
            compute_approval_marker(&compute_spend_ref(&AGENT, &BOB, 500, 8))
        );
    }

    #[test]
    fn a_digest_made_for_one_role_is_not_a_digest_for_another() {
        // A hash produced for one role must never be valid in another. That the
        // constants differ is necessary and nowhere near sufficient: it says
        // nothing about whether the functions use them, or use the right one.
        assert_ne!(SPEND_REF_PREFIX, APPROVAL_MARKER_PREFIX);

        const X: [u8; 32] = [0xab; 32];
        let as_spend = compute_spend_ref(&X, &X, 0, 0);
        let as_marker = compute_approval_marker(&X);
        assert_ne!(as_spend, as_marker);

        // Those two would differ even with no separators at all, because the
        // bodies are different lengths — which is an accident of the current
        // field list, not the guarantee. The guarantee is that each digest
        // commits to its own role, so assert that directly: neither function may
        // agree with the same material hashed bare, or under the other's
        // separator.
        let zero_u128 = 0u128.to_le_bytes();
        let zero_u64 = 0u64.to_le_bytes();
        let spend_body: [&[u8]; 4] = [&X, &X, &zero_u128, &zero_u64];
        let marker_body: [&[u8]; 1] = [&X];

        for (own, got, body) in [
            (SPEND_REF_PREFIX, as_spend, &spend_body[..]),
            (APPROVAL_MARKER_PREFIX, as_marker, &marker_body[..]),
        ] {
            assert_ne!(got, digest(b"", body), "the separator is not in the digest");
            for other in [SPEND_REF_PREFIX, APPROVAL_MARKER_PREFIX] {
                if other != own {
                    assert_ne!(got, digest(other, body), "another role's separator");
                }
            }
        }
    }

    #[test]
    fn the_policy_address_prefix_is_a_usable_pda_seed() {
        // `spel_framework::pda::seed_from_str` zero-pads to 32 bytes and panics
        // past that, inside the guest, where a panic is a halt with no error
        // code. Catch it here instead. It also has to be non-empty, or every
        // policy address collapses onto the marker namespace it exists to
        // separate.
        assert!(!POLICY_PDA_PREFIX.is_empty());
        assert!(POLICY_PDA_PREFIX.len() <= 32);
    }
}
