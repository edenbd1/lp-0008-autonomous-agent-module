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
// ONE POLICY ACCOUNT PER AGENT, WRITTEN BY TWO CONSENTING PARTIES
//
// The policy account's address is `PDA(this program, ["agent-policy/v1",
// agent_id])`. The agent, and nothing else. Everything the policy says — who
// owns it, both limits, the period, and the running total — lives in that
// account's DATA, which only this program may write (LEZ rule 6,
// `UnauthorizedDataModification`, `program/mod.rs:718-728`).
//
// One address per agent decides WHERE a policy goes. It does not decide who may
// put one there, and the previous deployment answered that question with
// "anybody". It declared two accounts on `create_policy` — the policy account
// and a signer it recorded as the owner — while `agent_id` was a free `[u8; 32]`
// argument the body discarded. The agent's account was never declared, never
// read and never asked to sign, so anchoring a policy over somebody else's agent
// needed no key: only the agent's public id, which this repository publishes in
// `artifacts/agents.tsv` and inside every signed Agent Card. Executed against
// that binary, an unrelated account anchored `per_tx = per_period = u128::MAX`
// over an agent it did not control and was accepted at halt 0; the honest
// owner's anchor was then refused `AccountAlreadyInitialized` and stayed refused,
// because `init` has no second chance and there is no `close`. `per_tx = 0`
// instead of `u128::MAX` turns the same call into a permanent denial of service.
//
// So consent is the missing half, and it is now two signatures in two
// transactions:
//
//   claim_agent     the AGENT signs. The claim account's address is derived from
//                   the signing account, so there is no `agent_id` argument to
//                   lie about, and it records the one account id that may anchor.
//   create_policy   the DESIGNATED OWNER signs. It reads that claim account and
//                   refuses any other signer                              (6020),
//                   and refuses outright if the agent never claimed       (6019).
//
// A third party holds neither key. It cannot claim an agent it cannot sign for,
// and it cannot anchor over an agent that has designated somebody else.
//
// WHY TWO INSTRUCTIONS AND NOT TWO SIGNERS ON ONE
//
// `spel` signs only for accounts an instruction declares as signers, and only
// with keys the single wallet home it is pointed at actually holds
// (`spel-cli/src/tx.rs`, `get_account_public_signing_key`; the privacy path
// marks an account authorized exactly when the wallet has its key,
// `wallet/src/account_manager.rs:214-224`). One instruction demanding both
// signatures would demand the owner's key and the agent's key in the same
// wallet — the arrangement this whole design exists to avoid. Two single-signer
// instructions keep the agent's key on the agent's node and the owner's key with
// the owner, and each is a transaction shape this repository has already landed.
//
// Removing a signer is not an option either: an instruction with no declared
// signer produces a transaction with an empty witness set, which is what made
// `create_policy` permissionless in the first place.
//
// LOSING THE RACE IS NOT PERMANENT
//
// LEZ rule 4 forbids changing an account's program owner at all
// (`ModifiedProgramOwner`, `program/mod.rs:697-703`), so an account this program
// has claimed can never be released and no `close` instruction can exist. The
// way back therefore has to be in the record, and it is:
//
//   update_policy   the owner the RECORD names re-fixes both limits and the
//                   period, in place, on its own signature                (6012).
//
// An envelope anchored wrong is corrected instead of abandoned, and an owner who
// believes the agent is compromised sets `per_tx = 0` and stops it spending
// unattended at all. The running total is carried through unchanged: changing a
// ceiling is not forgiveness for what has already been spent under the old one.
//
// What stays irreversible is the agent's own designation of its owner. It has
// to — an agent that could re-designate could name itself and anchor its own
// unlimited policy — and its cost is bounded: a shielded account costs nothing
// to create, the agent's balance is held by the transfer program and moves on
// the agent's own key, so the remedy is a new agent identity. No third party can
// force that remedy on anyone.
//
// WHAT REMAINS, AND WHERE IT IS CHECKED
//
//   claim_agent     `init` — one claim account per agent, and the agent signs it.
//                   A claim naming the all-zero account is refused        (6022),
//                   because no key produces that id.
//   create_policy   `init` — one policy account per agent. The owner recorded is
//                   the signer's own id, not an argument, and the signer must be
//                   the id the agent claimed.
//   update_policy   the signer must be the owner the record names.
//   spend           the policy account's address is derived from the PAYING
//                   account's id, so there is no `agent_id` argument to lie
//                   about. The limits come out of the account, not the call.
//   approve_spend   the signer must be the owner the RECORD names         (6012),
//                   and the approval names the block it stops being good at.
//   spend_approved  same address derivation; the marker was created by this
//                   program, is unspent, and the transaction is pinned inside
//                   the approval's own validity window.
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
// The same mechanism is what gives an owner approval an expiry. A marker used to
// be valid in every block forever, which made it a bearer instrument redeemable
// the day the agent's key was stolen — and unrevocable, since `approve_spend` is
// `init`. The owner now names the block it stops being good at, the marker
// carries that block, and `spend_approved` pins its transaction to `[0, expiry)`.
// An expired approval is not refused by this program: it is a transaction no
// block will include.
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
// That same ownership is the ceiling on everything above: an account holder can
// always call the program that owns its balance directly. This program bounds
// what the agent may move THROUGH IT, and `docs/limitations.md` states plainly
// what that does and does not mean for a stolen key.
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
/// Without this the agent approves its own above-threshold spends, and the
/// envelope can be re-fixed by somebody who does not own it.
const E_OWNER_MISMATCH: u32 = 6012;
/// The period named does not start on a multiple of `period_blocks`.
const E_WINDOW_MISALIGNED: u32 = 6014;
/// The period named is older than the one the policy record holds.
const E_WINDOW_REGRESSED: u32 = 6015;
/// An account's data is not a record this program wrote.
const E_RECORD_MALFORMED: u32 = 6016;
/// `period_blocks` is zero, so the policy has no period to account against.
const E_PERIOD_ZERO: u32 = 6017;
/// The owner approval presented has already been spent.
const E_APPROVAL_ALREADY_SPENT: u32 = 6018;
/// This agent has never designated an owner, so nobody may anchor a policy over
/// it. The refusal that makes anchoring require the agent's own key.
const E_AGENT_UNCLAIMED: u32 = 6019;
/// The account signing is not the one this agent designated as its owner.
const E_NOT_DESIGNATED_OWNER: u32 = 6020;
/// An approval must name the block it stops being good at, and it must be a
/// block: zero is an empty validity window, which is the unbounded approval the
/// expiry exists to prevent.
const E_APPROVAL_EXPIRY_INVALID: u32 = 6021;
/// An agent may not designate the all-zero account as its owner: no key produces
/// that id, so no `create_policy` for it could ever be signed.
const E_OWNER_UNSET: u32 = 6022;

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

    /// The agent names the one account that may anchor its policy. Once.
    ///
    /// This is the half of anchoring that the previous deployment did not have,
    /// and without it `create_policy` is permissionless: an agent's id is
    /// public — it is in this repository's manifest and in its signed Agent Card
    /// — so anything keyed on the id alone is keyed on nothing.
    ///
    /// Accounts:
    /// - `claim` (init, PDA seeded by `["agent-owner/v1", agent]`): the agent's
    ///   statement of who decides its envelope. `init` makes it once and for
    ///   all, which is deliberate: an agent that could re-designate could name
    ///   itself and then anchor its own unlimited policy, which is the whole
    ///   attack. The address is derived from `agent` — the account that SIGNS —
    ///   so there is no `agent_id` argument here, and nothing to disagree with.
    ///
    /// - `agent` (signer): the agent's own account, and the only thing that
    ///   makes this call authenticated. `spel` signs only for declared signers,
    ///   so removing it would make this instruction permissionless and hand the
    ///   designation to whoever ran first.
    ///
    /// `owner_id` is a caller-supplied value and stays one: the agent has to be
    /// able to name an owner that does not yet hold anything on this chain. What
    /// it buys is nothing it did not already have — the agent signed this call,
    /// so it is choosing its own owner, and the only party harmed by a bad
    /// choice is the agent itself, whose identity then anchors nothing.
    #[instruction]
    pub fn claim_agent(
        #[account(init, pda = [literal("agent-owner/v1"), account("agent")])]
        claim: AccountWithMetadata,
        #[account(signer)]
        agent: AccountWithMetadata,
        owner_id: [u8; 32],
    ) -> SpelResult {
        // The all-zero id is not an account anyone can sign for. Designating it
        // would spend this agent's one claim on an owner that can never anchor,
        // which is the one way an honest agent can brick itself here.
        if owner_id == [0u8; 32] {
            return Err(SpelError::custom(
                E_OWNER_UNSET,
                "an agent cannot designate the all-zero account as its owner",
            ));
        }
        let mut claim = claim;
        write_data(
            &mut claim,
            &agent_policy_core::OwnerClaim { owner: owner_id }.encode(),
        )?;
        Ok(SpelOutput::execute(vec![claim, agent], vec![]))
    }

    /// Publish an agent's spending policy. Once, per agent, by the account that
    /// agent designated.
    ///
    /// Accounts:
    /// - `policy` (init, PDA seeded by `["agent-policy/v1", agent_id]`): the
    ///   on-chain commitment. Its address is a function of the agent and nothing
    ///   else, so this is *the* policy account for that agent rather than one of
    ///   many, and `init` refuses to overwrite it.
    ///
    ///   Its data is the record — owner, limits, period, running total — written
    ///   here and afterwards only by `spend` and `update_policy`.
    ///
    /// - `claim` (PDA seeded by `["agent-owner/v1", agent_id]`): what the AGENT
    ///   signed. Required to be owned by this program, because anyone can fund
    ///   an address and a claim this program did not write is not a claim
    ///   (6019). It is the account that turns `agent_id` from a label into a
    ///   binding: naming an agent you were not designated by resolves to that
    ///   agent's claim, whose owner is not you (6020).
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
    /// `per_tx = 0` is accepted and means what it says: this agent may spend
    /// nothing unattended, and every payment needs an approval. That is a policy
    /// an owner may legitimately want, it is the state `update_policy` puts a
    /// suspect agent into, and it is reversible — so refusing it here would
    /// remove a control rather than add one. It is safe to accept only because
    /// the designated owner is the only account that can set it; under the
    /// previous deployment the identical value, from any stranger, was a
    /// permanent denial of service.
    ///
    /// `period_blocks = 1` is accepted too, and is worth understanding before
    /// choosing it: a one-block period means `per_period` bounds a single block
    /// rather than a horizon, so it stops constraining the aggregate, and the
    /// transaction must land in exactly the block it names. Both are the owner's
    /// call to make. The only value refused is 0 (6017), because a policy with no
    /// period cannot be accounted against at all — `authorize` would have nothing
    /// to align a window to and would divide by zero.
    #[instruction]
    pub fn create_policy(
        ctx: ProgramContext,
        #[account(init, pda = [literal("agent-policy/v1"), arg("agent_id")])]
        policy: AccountWithMetadata,
        #[account(pda = [literal("agent-owner/v1"), arg("agent_id")])]
        claim: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    ) -> SpelResult {
        // `agent_id` is read by the macro to derive and check BOTH PDAs above.
        // The body has nothing left to compare it against, because the claim
        // account it resolves to does the comparing.
        let _ = agent_id;
        // Owned by *this* program, not merely non-default: anyone can fund an
        // address, and an account this program never created is not a claim.
        if claim.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_AGENT_UNCLAIMED,
                "this agent has not designated an owner, so no policy can be anchored over it",
            ));
        }
        let designated = agent_policy_core::OwnerClaim::decode(&claim.account.data)
            .map_err(|_| {
                SpelError::custom(
                    E_RECORD_MALFORMED,
                    "the claim account does not hold a record this program wrote",
                )
            })?;
        if *owner.account_id.value() != designated.owner {
            return Err(SpelError::custom(
                E_NOT_DESIGNATED_OWNER,
                "the signer is not the owner this agent designated",
            ));
        }
        if period_blocks == 0 {
            return Err(SpelError::custom(
                E_PERIOD_ZERO,
                "period_blocks must not be zero",
            ));
        }
        let record = agent_policy_core::PolicyRecord {
            // The signer, off the pre-state. Not a claim in the instruction —
            // and equal to `designated.owner` by the check above, so the record
            // and the agent's own designation cannot drift apart.
            owner: *owner.account_id.value(),
            policy: agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks },
            ledger: agent_policy_core::SpendLedger::default(),
        };
        let mut policy = policy;
        write_data(&mut policy, &record.encode())?;
        Ok(SpelOutput::execute(vec![policy, claim, owner], vec![]))
    }

    /// Re-fix an anchored envelope. The way back from an anchor that was wrong,
    /// and the brake on an agent that is suspect.
    ///
    /// This exists because `init` cannot be undone. LEZ rule 4 forbids changing
    /// an account's program owner (`ModifiedProgramOwner`), so a policy account
    /// this program has claimed is claimed forever and no `close` can exist. If
    /// the terms were immutable as well, a single wrong anchor would burn the
    /// agent identity — and the previous deployment made exactly that outcome
    /// reachable by a stranger.
    ///
    /// Accounts:
    /// - `policy` (mut, PDA seeded by `["agent-policy/v1", agent_id]`): the
    ///   anchored record, rewritten in place.
    /// - `owner` (signer): must be the owner the RECORD names (6012), which is
    ///   the account the agent designated and `create_policy` recorded. The
    ///   agent does not sign here on purpose: an owner who has to reach a
    ///   compromised agent for a signature cannot rein it in, and the agent
    ///   already consented to this owner deciding its terms.
    ///
    /// The running total is carried through untouched. Raising a ceiling is not
    /// forgiveness for what was already spent under the old one, and lowering it
    /// must not be softened by a reset either; the ledger belongs to the period,
    /// not to the terms.
    #[instruction]
    pub fn update_policy(
        ctx: ProgramContext,
        #[account(mut, pda = [literal("agent-policy/v1"), arg("agent_id")])]
        policy: AccountWithMetadata,
        #[account(signer)]
        owner: AccountWithMetadata,
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    ) -> SpelResult {
        let _ = agent_id;
        if policy.account.program_owner != ctx.self_program_id {
            return Err(SpelError::custom(
                E_POLICY_NOT_ANCHORED,
                "no policy is committed for this agent",
            ));
        }
        let mut policy = policy;
        let mut record = read_record(&policy)?;
        if *owner.account_id.value() != record.owner {
            return Err(SpelError::custom(
                E_OWNER_MISMATCH,
                "the signer is not the owner this agent's policy records",
            ));
        }
        if period_blocks == 0 {
            return Err(SpelError::custom(
                E_PERIOD_ZERO,
                "period_blocks must not be zero",
            ));
        }
        record.policy = agent_policy_core::SpendPolicy { per_tx, per_period, period_blocks };
        write_data(&mut policy, &record.encode())?;
        Ok(SpelOutput::execute(vec![policy, owner], vec![]))
    }

    /// Record that the owner approved one specific above-threshold spend, and
    /// the block at which that approval stops being good.
    ///
    /// Accounts:
    /// - `approval` (init, PDA seeded by `marker_seed`): exists only because the
    ///   owner signed. `init` makes it single-issue; `spend_approved` stamping
    ///   its data is what makes it single-*use*; `expiry_block` is what stops it
    ///   being a bearer instrument that outlives the reason it was granted.
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
    ///
    /// `expiry_block` is the first block at which the approval is dead. It is not
    /// in the marker seed, because the seed is what the AGENT has to reproduce
    /// and the expiry is what the OWNER alone decides; it lives in the account's
    /// data, where only this program can write it.
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
        expiry_block: u64,
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
        // `[0, 0)` is an empty range and cannot be pinned to a transaction, so a
        // zero expiry is not "never expires" — it is an approval that would have
        // to be exempted from the window, which is the defect this field fixes.
        if expiry_block == 0 {
            return Err(SpelError::custom(
                E_APPROVAL_EXPIRY_INVALID,
                "an approval must name the block at which it stops being good",
            ));
        }
        let mut approval = approval;
        write_data(
            &mut approval,
            &agent_policy_core::ApprovalRecord { expiry_block, spent: false }.encode(),
        )?;
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
        write_data(&mut policy, &record.encode())?;

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

    /// The agent pays above its envelope, on an approval the owner signed and
    /// inside the lifetime that approval was given.
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
    /// It does not last forever either, and that is the change. The approval
    /// carries the block it dies at, and the transaction is pinned to `[0,
    /// expiry_block)` — so an approval granted for a payment due this week is not
    /// still redeemable next year by whoever holds the agent's key by then.
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
        let mut granted = agent_policy_core::ApprovalRecord::decode(&approval.account.data)
            .map_err(|_| {
                SpelError::custom(
                    E_RECORD_MALFORMED,
                    "the approval account does not hold a record this program wrote",
                )
            })?;
        if granted.spent {
            return Err(SpelError::custom(
                E_APPROVAL_ALREADY_SPENT,
                "this owner approval has already been spent",
            ));
        }
        granted.spent = true;
        let expiry = granted.expiry_block;
        let mut approval = approval;
        write_data(&mut approval, &granted.encode())?;

        let transfer = delegated_transfer(&agent, &recipient, amount)?;

        let out = SpelOutput::execute(
            vec![policy, approval, agent, recipient],
            vec![transfer],
        );
        // 6. And within its lifetime. This is the only check here the program
        //    does not make itself: the window goes into `ProgramOutput`, and the
        //    state machine refuses to include the transaction outside it
        //    (`OutOfValidityWindow`). An approval past its expiry is therefore
        //    not a refusal an attacker can read — it is a transaction that never
        //    lands, which is the same thing the chain does to a stale window in
        //    `spend`.
        out.try_with_block_validity_window(0..expiry)
            .map_err(|_| {
                SpelError::custom(
                    E_APPROVAL_EXPIRY_INVALID,
                    "the approval's expiry is not a valid block range",
                )
            })
    }
}

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

/// Put bytes in an account this program owns.
///
/// Rule 6 allows this because every account written here is a PDA of this
/// program — or, at an `init`, because its pre-state is default — and forbids it
/// to everyone else. One helper for all three records so that "the encoding did
/// not fit" reports the same way wherever it happens.
fn write_data(account: &mut AccountWithMetadata, bytes: &[u8]) -> Result<(), SpelError> {
    account.account.data = nssa_core::account::Data::try_from(bytes.to_vec())
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
