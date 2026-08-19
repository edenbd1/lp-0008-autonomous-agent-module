//! Run the committed guest against instructions that must be refused.
//!
//! WHY THIS EXISTS
//!
//! Every defect this program has been fixed for was found by *executing* the
//! deployed binary, and the fix has to be checked the same way. On chain that is
//! awkward: a refusal is not a transaction. A rejected transaction is submitted,
//! given a hash, and then never included — and `getTransaction` answers `null`
//! for a rejected hash, a pending one and a hash nobody ever sent. Absence is
//! not evidence, so "the attack no longer lands" cannot be read off the chain.
//!
//! What can be shown, deterministically and offline, is the program refusing:
//! this runs `artifacts/programs/agent_verifier.bin` — the same bytes whose
//! SHA-256 is the deploy transaction — in the risc0 executor, with pre-states
//! and instruction words built exactly as the state machine builds them, and
//! asserts the error code each hostile call halts with.
//!
//! Every case comes in pairs: the hostile call and the honest one that differs
//! from it in a single field. A check that only shows a refusal proves nothing
//! about whether anything is ever accepted.
//!
//! WHAT THIS FILE GOT WRONG BEFORE, TWICE, WHICH IS WORTH MORE THAN ANY CASE
//!
//! The first version contained half the live attack and asserted it as the
//! BENIGN control: `"the same call, naming the account that actually signs —
//! accepted"`. That call was an attacker anchoring an unlimited policy over
//! somebody else's agent while honestly naming itself as the owner.
//!
//! The second version — the one that shipped with the `AccountAlreadyInitialized`
//! fix, and whose header said the sentence above — did it again, in a form that
//! looks like housekeeping:
//!
//!     "an agent nobody has anchored yet still can be — accepted"
//!         accounts: [policy_pda(other_agent), fresh_signer(attacker)]
//!         instruction: anchor(other_agent, unlimited)
//!         expect: Accepted
//!
//! That is an unrelated account anchoring `per_tx = per_period = u128::MAX` over
//! an agent it does not control, asserted as required behaviour. It was required
//! behaviour, because `create_policy` never declared the agent's account at all;
//! the suite was faithfully recording a program in which anchoring needed no key
//! whatsoever. The four steps that make it an attack — the honest owner then
//! being refused for good, the agent spending its balance, the attacker signing
//! its own approval — were not run.
//!
//! Both are fixed below. That anchoring call now has to REFUSE, with 6019
//! (nobody has claimed this agent) or 6020 (the signer is not the owner it
//! claimed), and the four-step attack runs end to end as a regression, in the
//! shapes the previous binaries accepted. `Expect::Accepted` appears only where
//! acceptance is the property under test, and the *second* step of the attack is
//! now an `Accepted` on purpose: the property is not only that the stranger is
//! refused, it is that the honest owner can still anchor afterwards.

use std::path::PathBuf;

use agent_policy_core::{
    compute_approval_marker, compute_spend_ref, ApprovalRecord, OwnerClaim, PolicyRecord,
    SpendLedger, SpendPolicy, OWNER_CLAIM_PDA_PREFIX, POLICY_PDA_PREFIX,
};
use anyhow::{bail, Context, Result};
use nssa_core::{
    account::{Account, AccountId, AccountWithMetadata, Data, Nonce},
    program::{ProgramId, ProgramOutput},
};
use risc0_zkvm::{compute_image_id, default_executor, ExecutorEnv};
use spel_framework_core::pda::{compute_pda, seed_from_str};

/// The guest's instruction enum, mirrored.
///
/// risc0's serde writes a variant index and then the fields in declaration
/// order, so the ORDER of these variants and of their fields is the ABI.
///
/// `ClaimAgent` is first because it happens first: an agent designates the
/// account that may anchor its policy before any policy exists. That ordering is
/// the fix expressed in the ABI — under the previous binary there was nothing to
/// do before `create_policy`, which is precisely why anybody could call it.
#[derive(serde::Serialize)]
#[allow(dead_code)]
enum Instruction {
    ClaimAgent {
        owner_id: [u8; 32],
    },
    CreatePolicy {
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    },
    UpdatePolicy {
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    },
    ApproveSpend {
        marker_seed: [u8; 32],
        agent_id: [u8; 32],
        recipient: [u8; 32],
        amount: u128,
        nonce: u64,
        expiry_block: u64,
    },
    Spend {
        amount: u128,
        window_start: u64,
    },
    SpendApproved {
        recipient_id: [u8; 32],
        amount: u128,
        nonce: u64,
        marker_seed: [u8; 32],
    },
}

/// A stand-in for LEZ's authenticated transfer program: `spend` refuses to
/// delegate to a program id of zero, and the id it delegates to is read off the
/// agent's account rather than chosen, so any non-default value exercises the
/// same path.
const HOLDER_PROGRAM: ProgramId = [7; 8];

/// What a case must do.
///
/// The two refusal kinds do not look alike from out here, and the difference is
/// worth stating because the attack is now closed by the first one.
///
/// A refusal the program body returns comes back through `SpelError`, which
/// `#[lez_program]` turns into
///
///   Guest panicked: Program error [12019]: Program error 6019: …
///
/// The bracketed number is `SpelError::error_code()`, which offsets a *custom*
/// code by 6000 — so this program's documented 6019 appears twice, once
/// unrecognisably. Matching the bracketed form alone would match 6019 against
/// 12019 and pass a case that halted for a completely different reason.
///
/// A refusal from the macro's own account validation never reaches that path at
/// all. The generated dispatcher does
///
///   __validate_create_policy(…).expect("account validation failed");
///
/// so it panics with the `Debug` of the variant and **no numeric code**:
///
///   account validation failed: AccountAlreadyInitialized { account_index: 0 }
///
/// `SpelError::error_code()` would call that 1002, but 1002 appears nowhere in
/// what the executor prints, and an integration cannot branch on it either — it
/// has the variant name and nothing else. So that is what is matched, and what
/// is reported.
///
/// `AccountAlreadyInitialized` is no longer load-bearing for the anchoring
/// attack, and that is the point of this deployment: it fires only on the honest
/// owner's own second anchor. A stranger is stopped one step earlier, by a
/// numbered refusal that says why.
#[derive(Clone, Copy)]
enum Expect {
    /// Must run to completion and commit a `ProgramOutput`.
    Accepted,
    /// Must halt with one of this program's own 6xxx codes.
    Custom(u32),
    /// Must be refused by the macro's account validation, with this
    /// `SpelError` variant.
    Validation(&'static str),
}

impl Expect {
    fn matches(self, text: &str) -> bool {
        match self {
            Expect::Accepted => false,
            Expect::Custom(c) => {
                text.contains(&format!("Program error [{}]: Program error {c}: ", 6000 + c))
            }
            Expect::Validation(name) => {
                text.contains(&format!("account validation failed: {name} {{"))
            }
        }
    }

    fn label(self) -> String {
        match self {
            Expect::Accepted => "accepted".to_string(),
            Expect::Custom(c) => format!("{c}"),
            Expect::Validation(name) => name.to_string(),
        }
    }
}

/// The macro refuses a second `#[account(init)]` on an account that already
/// exists. One claim and one policy account per agent, and this is the refusal
/// that says so — to the party that already holds them, never to a stranger.
const ALREADY_ANCHORED: Expect = Expect::Validation("AccountAlreadyInitialized");
/// The account presented is not the PDA the seeds derive.
const WRONG_ACCOUNT: Expect = Expect::Validation("PdaMismatch");

/// Nobody has designated an owner for this agent, so no policy may be anchored.
const E_AGENT_UNCLAIMED: u32 = 6019;
/// The signer is not the account the agent designated.
const E_NOT_DESIGNATED_OWNER: u32 = 6020;

struct Case {
    what: &'static str,
    accounts: Vec<AccountWithMetadata>,
    instruction: Instruction,
    expect: Expect,
}

fn account(owner: ProgramId, balance: u128, data: Vec<u8>) -> Account {
    Account {
        program_owner: owner,
        balance,
        data: Data::try_from(data).expect("account data fits"),
        nonce: Nonce(0),
    }
}

fn with_id(account: Account, authorized: bool, id: [u8; 32]) -> AccountWithMetadata {
    AccountWithMetadata::new(account, authorized, AccountId::new(id))
}

fn run(elf: &[u8], program_id: ProgramId, case: &Case) -> Result<Option<ProgramOutput>> {
    let words = risc0_zkvm::serde::to_vec(&case.instruction).context("serialising instruction")?;
    let env = ExecutorEnv::builder()
        .write(&program_id)?
        .write(&None::<ProgramId>)?
        .write(&case.accounts)?
        .write(&words)?
        .build()?;
    match default_executor().execute(env, elf) {
        Ok(session) => Ok(Some(
            session
                .journal
                .decode::<ProgramOutput>()
                .context("decoding the program output the guest committed")?,
        )),
        Err(e) => {
            let text = format!("{e:#}");
            if case.expect.matches(&text) {
                Ok(None)
            } else {
                bail!(text)
            }
        }
    }
}

/// Say what an accepted call actually wrote, so "accepted" is not merely the
/// absence of an error. Each of the three records this program writes is tried
/// against the first post-state, because each of them is a claim about who may
/// spend what.
fn wrote(output: &ProgramOutput) -> Option<String> {
    let data = output.post_states.first()?.account().data.to_vec();
    if let Ok(r) = PolicyRecord::decode(&data) {
        return Some(format!(
            "policy account: per-tx {}, per-period {}, {} spent in period {}",
            r.policy.per_tx, r.policy.per_period, r.ledger.spent, r.ledger.window_start
        ));
    }
    if let Ok(c) = OwnerClaim::decode(&data) {
        return Some(format!(
            "claim account: only {}… may anchor this agent",
            hex8(&c.owner)
        ));
    }
    if let Ok(a) = ApprovalRecord::decode(&data) {
        return Some(format!(
            "approval account: dead at block {}, spent {}",
            a.expiry_block, a.spent
        ));
    }
    None
}

fn hex8(id: &[u8; 32]) -> String {
    id[..4].iter().map(|b| format!("{b:02x}")).collect()
}

fn main() -> Result<()> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let elf = std::fs::read(root.join("artifacts/programs/agent_verifier.bin"))
        .context("reading artifacts/programs/agent_verifier.bin")?;
    let program_id: ProgramId = compute_image_id(&elf)?.as_words().try_into()?;
    println!(
        "  program  {}  ({} bytes, the file whose SHA-256 is the deploy transaction)",
        program_id
            .iter()
            .flat_map(|w| w.to_le_bytes())
            .map(|b| format!("{b:02x}"))
            .collect::<String>(),
        elf.len()
    );

    // The cast. `attacker` and `agent_pay` are real accounts whose keys the
    // attacker holds; `agent` is one whose key it does NOT. The property this
    // deployment has to have is that knowing an agent's id — which is public,
    // in `artifacts/agents.tsv` and in every signed Agent Card — buys nothing.
    let attacker = [0x11u8; 32];
    let real_owner = [0x22u8; 32];
    let agent = [0x33u8; 32];
    let other_agent = [0x44u8; 32];
    let recipient = [0x55u8; 32];
    // The agent's own PUBLIC pay account. It is program-owned, which is what let
    // it anchor repeatedly against an earlier binary, and one executed variant of
    // the attack used exactly that account.
    let agent_pay = [0x66u8; 32];

    let unlimited = SpendPolicy {
        per_tx: u128::MAX,
        per_period: u128::MAX,
        period_blocks: 1000,
    };
    let real = SpendPolicy {
        per_tx: 200,
        per_period: 1000,
        period_blocks: 1000,
    };
    // What a hostile anchor looks like when the goal is denial of service rather
    // than theft: an agent that may spend nothing, for the life of its identity.
    let inert = SpendPolicy {
        per_tx: 0,
        per_period: 0,
        period_blocks: 1000,
    };

    // The two accounts an agent has under this program, and both are addressed
    // from the agent alone: no owner, no limits, so there is exactly one of each
    // per agent. Recomputed here from the SAME constants the guest declares; if
    // they ever drift, every PDA below misses and the macro's own check fails the
    // suite rather than letting it pass against an address nothing reads.
    let policy_prefix = seed_from_str(POLICY_PDA_PREFIX);
    let claim_prefix = seed_from_str(OWNER_CLAIM_PDA_PREFIX);
    let policy_pda = |a: &[u8; 32]| *compute_pda(&program_id, &[&policy_prefix, a]).value();
    let claim_pda = |a: &[u8; 32]| *compute_pda(&program_id, &[&claim_prefix, a]).value();
    assert_ne!(
        policy_pda(&agent),
        claim_pda(&agent),
        "the two prefixes must not collapse onto one address"
    );

    // What the honest owner anchored for `agent`, as bytes, exactly as
    // `create_policy` writes them.
    let anchored = PolicyRecord {
        owner: real_owner,
        policy: real,
        ledger: SpendLedger::default(),
    };
    // The same after 1000 has been moved in period 8000.
    let period_spent_out = PolicyRecord {
        ledger: SpendLedger {
            window_start: 8000,
            spent: 1000,
        },
        ..anchored
    };

    let agents_policy = |r: &PolicyRecord| {
        with_id(
            account(program_id, 0, r.encode().to_vec()),
            false,
            policy_pda(&agent),
        )
    };
    // The claim `agent` signed: only `real_owner` may anchor over it.
    let agents_claim = || {
        with_id(
            account(
                program_id,
                0,
                OwnerClaim { owner: real_owner }.encode().to_vec(),
            ),
            false,
            claim_pda(&agent),
        )
    };
    let unclaimed = |a: &[u8; 32]| with_id(Account::default(), false, claim_pda(a));
    let payer = |id: [u8; 32]| with_id(account(HOLDER_PROGRAM, 10_000, vec![]), true, id);
    let payee = || with_id(account(HOLDER_PROGRAM, 0, vec![]), false, recipient);
    let fresh_signer = |id: [u8; 32]| with_id(Account::default(), true, id);
    let owned_signer = |id: [u8; 32]| with_id(account(HOLDER_PROGRAM, 5, vec![]), true, id);
    let spend = |amount: u128, window_start: u64| Instruction::Spend {
        amount,
        window_start,
    };
    let anchor = |a: [u8; 32], p: SpendPolicy| Instruction::CreatePolicy {
        agent_id: a,
        per_tx: p.per_tx,
        per_period: p.per_period,
        period_blocks: p.period_blocks,
    };
    let update = |a: [u8; 32], p: SpendPolicy| Instruction::UpdatePolicy {
        agent_id: a,
        per_tx: p.per_tx,
        per_period: p.per_period,
        period_blocks: p.period_blocks,
    };
    let claim = |owner_id: [u8; 32]| Instruction::ClaimAgent { owner_id };

    // The one approval the honest owner grants: 900 to `recipient`, nonce 7,
    // dead at block 9000. Above the 200 per-transaction envelope, which is the
    // only reason to have an approval at all.
    const EXPIRY: u64 = 9000;
    let approved_marker = compute_approval_marker(&compute_spend_ref(&agent, &recipient, 900, 7));
    let marker_pda = *compute_pda(&program_id, &[&approved_marker]).value();
    let approve = |amount: u128, nonce: u64, expiry_block: u64| Instruction::ApproveSpend {
        marker_seed: approved_marker,
        agent_id: agent,
        recipient,
        amount,
        nonce,
        expiry_block,
    };
    let granted = |spent: bool| {
        with_id(
            account(
                program_id,
                0,
                ApprovalRecord {
                    expiry_block: EXPIRY,
                    spent,
                }
                .encode()
                .to_vec(),
            ),
            false,
            marker_pda,
        )
    };
    let spend_approved = |amount: u128, nonce: u64| Instruction::SpendApproved {
        recipient_id: recipient,
        amount,
        nonce,
        marker_seed: approved_marker,
    };

    let cases = vec![
        // ── the agent designates its owner: the half that was missing ─────
        Case {
            what: "the agent designates the account that may anchor its policy — accepted",
            accounts: vec![unclaimed(&agent), payer(agent)],
            instruction: claim(real_owner),
            expect: Expect::Accepted,
        },
        Case {
            what: "an attacker designates ITSELF as the owner of an agent whose key it lacks",
            // The claim account's address is derived from the account that
            // SIGNS, so an attacker signing for itself cannot reach the victim's
            // claim address at all — there is no argument here to lie about.
            accounts: vec![unclaimed(&agent), fresh_signer(attacker)],
            instruction: claim(attacker),
            expect: WRONG_ACCOUNT,
        },
        Case {
            what: "the agent designates a second owner, having designated one already",
            accounts: vec![agents_claim(), payer(agent)],
            instruction: claim(attacker),
            expect: ALREADY_ANCHORED,
        },
        Case {
            what: "an agent designates the all-zero account, which no key produces",
            accounts: vec![unclaimed(&agent), payer(agent)],
            instruction: claim([0u8; 32]),
            expect: Expect::Custom(6022),
        },
        // ── anchoring: two parties, and a stranger is neither ─────────────
        Case {
            // THE CASE THIS SUITE USED TO ASSERT AS CORRECT, with the same
            // accounts and the same instruction. It is the whole defect.
            what: "an attacker anchors an UNLIMITED policy over an agent nobody has claimed",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                unclaimed(&other_agent),
                fresh_signer(attacker),
            ],
            instruction: anchor(other_agent, unlimited),
            expect: Expect::Custom(E_AGENT_UNCLAIMED),
        },
        Case {
            what: "the same call with per_tx = 0 — the denial of service, not the theft",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                unclaimed(&other_agent),
                fresh_signer(attacker),
            ],
            instruction: anchor(other_agent, inert),
            expect: Expect::Custom(E_AGENT_UNCLAIMED),
        },
        Case {
            what: "an attacker anchors over an agent that designated somebody else",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                fresh_signer(attacker),
            ],
            instruction: anchor(agent, unlimited),
            expect: Expect::Custom(E_NOT_DESIGNATED_OWNER),
        },
        Case {
            what: "the same, signed by the agent's own program-owned public pay account",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                owned_signer(agent_pay),
            ],
            instruction: anchor(agent, unlimited),
            expect: Expect::Custom(E_NOT_DESIGNATED_OWNER),
        },
        Case {
            what: "the compromised agent itself anchors, as both owner and agent",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                payer(agent),
            ],
            instruction: anchor(agent, unlimited),
            expect: Expect::Custom(E_NOT_DESIGNATED_OWNER),
        },
        Case {
            what: "a claim account an outsider merely funded at the right address",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                with_id(
                    account(
                        HOLDER_PROGRAM,
                        0,
                        OwnerClaim { owner: attacker }.encode().to_vec(),
                    ),
                    false,
                    claim_pda(&agent),
                ),
                fresh_signer(attacker),
            ],
            instruction: anchor(agent, unlimited),
            expect: Expect::Custom(E_AGENT_UNCLAIMED),
        },
        Case {
            what: "a claim account holding data this program did not write",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                with_id(account(program_id, 0, vec![0u8; 33]), false, claim_pda(&agent)),
                fresh_signer(attacker),
            ],
            instruction: anchor(agent, unlimited),
            expect: Expect::Custom(6016),
        },
        Case {
            what: "the owner the agent designated anchors its policy — accepted",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                fresh_signer(real_owner),
            ],
            instruction: anchor(agent, real),
            expect: Expect::Accepted,
        },
        Case {
            what: "…and an envelope of zero, which is a policy and not a mistake — accepted",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                fresh_signer(real_owner),
            ],
            instruction: anchor(agent, inert),
            expect: Expect::Accepted,
        },
        Case {
            what: "the designated owner anchors a second time",
            accounts: vec![agents_policy(&anchored), agents_claim(), fresh_signer(real_owner)],
            instruction: anchor(agent, unlimited),
            expect: ALREADY_ANCHORED,
        },
        Case {
            what: "an anchor whose policy account is not the PDA for the agent it names",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                agents_claim(),
                fresh_signer(real_owner),
            ],
            instruction: anchor(agent, real),
            expect: WRONG_ACCOUNT,
        },
        Case {
            what: "a policy with no period, which nothing could ever be accounted against",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                agents_claim(),
                fresh_signer(real_owner),
            ],
            instruction: anchor(
                agent,
                SpendPolicy {
                    period_blocks: 0,
                    ..real
                },
            ),
            expect: Expect::Custom(6017),
        },
        // ── the way back: an anchored envelope is not a life sentence ─────
        Case {
            what: "a stranger re-fixes the anchored envelope to unlimited",
            accounts: vec![agents_policy(&period_spent_out), fresh_signer(attacker)],
            instruction: update(agent, unlimited),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the compromised agent re-fixes its own envelope",
            accounts: vec![agents_policy(&period_spent_out), payer(agent)],
            instruction: update(agent, unlimited),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the owner the record names freezes the agent at per_tx = 0 — accepted",
            accounts: vec![agents_policy(&period_spent_out), fresh_signer(real_owner)],
            instruction: update(agent, inert),
            expect: Expect::Accepted,
        },
        Case {
            what: "the agent spends 1 under the envelope its owner just froze",
            accounts: vec![
                with_id(
                    account(
                        program_id,
                        0,
                        PolicyRecord {
                            policy: inert,
                            ..period_spent_out
                        }
                        .encode()
                        .to_vec(),
                    ),
                    false,
                    policy_pda(&agent),
                ),
                payer(agent),
                payee(),
            ],
            instruction: spend(1, 8000),
            expect: Expect::Custom(6005),
        },
        Case {
            what: "an update against a policy account this program never created",
            accounts: vec![
                with_id(
                    account(HOLDER_PROGRAM, 0, anchored.encode().to_vec()),
                    false,
                    policy_pda(&agent),
                ),
                fresh_signer(real_owner),
            ],
            instruction: update(agent, real),
            expect: Expect::Custom(6002),
        },
        Case {
            what: "an update that would leave the policy with no period",
            accounts: vec![agents_policy(&anchored), fresh_signer(real_owner)],
            instruction: update(
                agent,
                SpendPolicy {
                    period_blocks: 0,
                    ..real
                },
            ),
            expect: Expect::Custom(6017),
        },
        // ── spending ─────────────────────────────────────────────────────
        Case {
            what: "the agent spends inside its anchored envelope — accepted",
            accounts: vec![agents_policy(&anchored), payer(agent), payee()],
            instruction: spend(200, 8000),
            expect: Expect::Accepted,
        },
        Case {
            what: "the agent spends its whole balance, as it would under an unlimited policy",
            accounts: vec![agents_policy(&anchored), payer(agent), payee()],
            instruction: spend(10_000, 8000),
            expect: Expect::Custom(6005),
        },
        // The three refusals `delegated_transfer` raises, which nothing exercised
        // until now. They are not the enforcement — the transfer program does its
        // own checked_sub/checked_add — they are there so a caller gets a code
        // instead of a panic inside a program it did not write, and that sentence
        // was an assertion nothing executed. Each is the accepted spend above with
        // exactly one account field changed.
        Case {
            what: "the paying account is held by no program, so it cannot pay at all",
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(Account::default().program_owner, 10_000, vec![]), true, agent),
                payee(),
            ],
            instruction: spend(200, 8000),
            expect: Expect::Custom(6010),
        },
        Case {
            what: "the spend is inside the envelope and larger than the balance",
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(HOLDER_PROGRAM, 5, vec![]), true, agent),
                payee(),
            ],
            instruction: spend(100, 8000),
            expect: Expect::Custom(6008),
        },
        Case {
            what: "the recipient is one payment short of overflowing u128",
            accounts: vec![
                agents_policy(&anchored),
                payer(agent),
                with_id(account(HOLDER_PROGRAM, u128::MAX, vec![]), false, recipient),
            ],
            instruction: spend(100, 8000),
            expect: Expect::Custom(6009),
        },
        Case {
            what: "a different agent presents this agent's policy account",
            accounts: vec![agents_policy(&anchored), payer(other_agent), payee()],
            instruction: spend(200, 8000),
            expect: WRONG_ACCOUNT,
        },
        Case {
            what: "a policy account at the right address that this program never created",
            accounts: vec![
                with_id(
                    account(HOLDER_PROGRAM, 0, anchored.encode().to_vec()),
                    false,
                    policy_pda(&agent),
                ),
                payer(agent),
                payee(),
            ],
            instruction: spend(200, 8000),
            expect: Expect::Custom(6002),
        },
        Case {
            what: "a policy account holding data this program did not write",
            accounts: vec![
                with_id(account(program_id, 0, vec![0u8; 24]), false, policy_pda(&agent)),
                payer(agent),
                payee(),
            ],
            instruction: spend(200, 8000),
            expect: Expect::Custom(6016),
        },
        Case {
            what: "a spend that would carry the period total past the per-period limit",
            accounts: vec![agents_policy(&period_spent_out), payer(agent), payee()],
            instruction: spend(1, 8000),
            expect: Expect::Custom(6006),
        },
        Case {
            what: "the same spend in the next period, where the budget starts again — accepted",
            accounts: vec![agents_policy(&period_spent_out), payer(agent), payee()],
            instruction: spend(1, 9000),
            expect: Expect::Accepted,
        },
        Case {
            what: "a period that does not start on a multiple of period_blocks",
            accounts: vec![agents_policy(&anchored), payer(agent), payee()],
            instruction: spend(1, 8001),
            expect: Expect::Custom(6014),
        },
        Case {
            what: "a period older than the one the policy record already holds",
            accounts: vec![agents_policy(&period_spent_out), payer(agent), payee()],
            instruction: spend(1, 7000),
            expect: Expect::Custom(6015),
        },
        // ── the above-threshold path ─────────────────────────────────────
        //
        // Nothing else in this repository executes it: `scripts/a2a-task.sh`
        // settles inside the envelope and `scripts/e2e-local-sequencer.sh`
        // exercises `spend`. An untested path in the instruction whose purpose
        // is to let an agent spend MORE than its ceiling is the last one to
        // leave unexercised.
        Case {
            what: "the compromised agent signs its own above-threshold approval",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                payer(agent),
            ],
            instruction: approve(900, 7, EXPIRY),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the same, signed by the agent's own public pay account",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                owned_signer(agent_pay),
            ],
            instruction: approve(900, 7, EXPIRY),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the owner the policy record names signs it — accepted",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                fresh_signer(real_owner),
            ],
            instruction: approve(900, 7, EXPIRY),
            expect: Expect::Accepted,
        },
        Case {
            what: "the owner signs, but the marker does not commit to the amount approved",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                fresh_signer(real_owner),
            ],
            instruction: approve(901, 7, EXPIRY),
            expect: Expect::Custom(6003),
        },
        Case {
            what: "the owner grants an approval that never expires — the old bearer instrument",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                fresh_signer(real_owner),
            ],
            instruction: approve(900, 7, 0),
            expect: Expect::Custom(6021),
        },
        Case {
            what: "an above-threshold spend on an approval anyone could have funded",
            accounts: vec![
                agents_policy(&anchored),
                // Program-owned, at the right address, and not by this program:
                // exactly what funding an address gets you.
                with_id(account(HOLDER_PROGRAM, 0, vec![]), false, marker_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: Expect::Custom(6007),
        },
        Case {
            what: "the same spend on the approval this program created — accepted",
            accounts: vec![agents_policy(&anchored), granted(false), payer(agent), payee()],
            instruction: spend_approved(900, 7),
            expect: Expect::Accepted,
        },
        Case {
            what: "presenting that approval a second time, after it was stamped",
            accounts: vec![agents_policy(&anchored), granted(true), payer(agent), payee()],
            instruction: spend_approved(900, 7),
            expect: Expect::Custom(6018),
        },
        Case {
            what: "an approval in the shape the previous deployment used: empty data, no expiry",
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(program_id, 0, vec![]), false, marker_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: Expect::Custom(6016),
        },
        // The last declared refusal nothing executed. security-model.md lists it
        // among the things a key-holder cannot do — "point an approved payment at
        // a different recipient (6011)" — which was a claim about a branch no case
        // reached. The approval and its marker are unchanged; only the account in
        // the recipient position differs from the id the approval commits to.
        Case {
            what: "an approved payment pointed at a recipient the approval does not name",
            accounts: vec![
                agents_policy(&anchored),
                granted(false),
                payer(agent),
                with_id(account(HOLDER_PROGRAM, 0, vec![]), false, other_agent),
            ],
            instruction: spend_approved(900, 7),
            expect: Expect::Custom(6011),
        },
        Case {
            what: "a different agent presents an approval granted for this one",
            accounts: vec![
                agents_policy(&anchored),
                granted(false),
                payer(other_agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: WRONG_ACCOUNT,
        },
    ];

    let mut failures = 0;
    for case in &cases {
        match (run(&elf, program_id, case), case.expect) {
            // `wrote()`'s own doc comment says it exists "so 'accepted' is not
            // merely the absence of an error", and for as long as its `None`
            // arm printed `ok` it was exactly that: forcing `wrote()` to return
            // None left all eight accepted cases and the three four-step step-2s
            // printing `ok` and the suite exiting 0. An accepted call that
            // committed no record this program can decode is now a failure.
            (Ok(Some(output)), Expect::Accepted) => match wrote(&output) {
                Some(what) => println!("  ok    {}\n          {what}", case.what),
                None => {
                    failures += 1;
                    println!(
                        "  FAIL  {}\n          accepted, but its first post-state holds no \
                         policy, claim or approval record: 'accepted' here would mean \
                         nothing but the absence of an error",
                        case.what
                    );
                }
            },
            (Ok(None), Expect::Accepted) => {
                failures += 1;
                println!("  FAIL  {}\n          the guest halted without committing", case.what);
            }
            (Ok(None), e) => println!("  ok    refused [{}]: {}", e.label(), case.what),
            (Ok(_), e) => {
                failures += 1;
                println!("  FAIL  ACCEPTED, and it must halt [{}]: {}", e.label(), case.what);
            }
            (Err(err), Expect::Accepted) => {
                failures += 1;
                println!("  FAIL  {}\n          must be accepted, got: {err}", case.what);
            }
            (Err(err), e) => {
                failures += 1;
                println!(
                    "  FAIL  {}\n          expected [{}], got: {err}",
                    case.what,
                    e.label()
                );
            }
        }
    }

    // ── the attack, all four steps, as it was executed on chain ──────────
    //
    // The cases above are single calls. What happened against the deployed
    // binary was a sequence, and the suite that shipped with it ran one step of
    // it and called that step correct. So run the whole thing, in the shapes
    // that were accepted, and do not stop when the first step is refused —
    // "the anchor failed" is not the property. The property is what is still
    // true afterwards: the honest owner can anchor, the agent is inside its
    // envelope, and the attacker cannot approve its way out.
    println!("\n  the four-step attack, executed against the previous program at halt 0:");
    for (who, signing) in [
        ("a separate account the attacker controls", fresh_signer(attacker)),
        ("the agent's own public pay account", owned_signer(agent_pay)),
        ("the compromised agent itself", payer(agent)),
    ] {
        let mut ok = true;
        let mut step = |case: Case, note: &str| {
            if !ok {
                return;
            }
            match (run(&elf, program_id, &case), case.expect) {
                (Ok(None), Expect::Accepted) | (Ok(Some(_)), Expect::Custom(_))
                | (Ok(Some(_)), Expect::Validation(_)) => {
                    ok = false;
                    println!("  FAIL  {who}: {note}");
                }
                (Err(e), _) => {
                    ok = false;
                    println!("  FAIL  {who}: {note} — halted for the wrong reason: {e}");
                }
                _ => {}
            }
        };

        // 1. Anchor an unlimited policy over an agent that has designated an
        //    owner it is not. This is the step that used to be accepted.
        step(
            Case {
                what: "",
                accounts: vec![
                    with_id(Account::default(), false, policy_pda(&agent)),
                    agents_claim(),
                    signing,
                ],
                instruction: anchor(agent, unlimited),
                expect: Expect::Custom(E_NOT_DESIGNATED_OWNER),
            },
            "anchored an unlimited policy over an agent it does not own",
        );
        // 2. The honest owner anchors afterwards. Under the previous program
        //    this was refused `AccountAlreadyInitialized`, for good, at the only
        //    address the agent has. Losing the race must not be permanent, so
        //    this one has to be ACCEPTED.
        step(
            Case {
                what: "",
                accounts: vec![
                    with_id(Account::default(), false, policy_pda(&agent)),
                    agents_claim(),
                    fresh_signer(real_owner),
                ],
                instruction: anchor(agent, real),
                expect: Expect::Accepted,
            },
            "the honest owner could not anchor afterwards",
        );
        // 3. The agent spends its whole balance, as it would have under the
        //    hostile policy.
        step(
            Case {
                what: "",
                accounts: vec![agents_policy(&anchored), payer(agent), payee()],
                instruction: spend(10_000, 8000),
                expect: Expect::Custom(6005),
            },
            "the agent moved its whole balance",
        );
        // 4. And the attacker signs an approval for 9,999,999 to its own wallet.
        let big = compute_approval_marker(&compute_spend_ref(&agent, &attacker, 9_999_999, 1));
        step(
            Case {
                what: "",
                accounts: vec![
                    with_id(Account::default(), false, *compute_pda(&program_id, &[&big]).value()),
                    agents_policy(&anchored),
                    fresh_signer(attacker),
                ],
                instruction: Instruction::ApproveSpend {
                    marker_seed: big,
                    agent_id: agent,
                    recipient: attacker,
                    amount: 9_999_999,
                    nonce: 1,
                    expiry_block: EXPIRY,
                },
                expect: Expect::Custom(6012),
            },
            "the attacker signed itself an approval for 9,999,999",
        );

        if ok {
            println!(
                "  ok    {who}: the anchor refused [{E_NOT_DESIGNATED_OWNER}], the honest owner\n          anchored afterwards, the whole-balance spend refused [6005] against\n          that policy, and the attacker's own approval refused [6012]"
            );
        } else {
            failures += 1;
        }
    }

    // The accumulation, end to end: run the accepted spend again with the record
    // the guest itself wrote, twice more, and watch the budget close.
    if failures == 0 {
        println!();
        let mut data = anchored.encode().to_vec();
        for (i, amount) in [200u128, 200, 200].iter().enumerate() {
            let case = Case {
                what: "",
                accounts: vec![
                    with_id(account(program_id, 0, data.clone()), false, policy_pda(&agent)),
                    payer(agent),
                    payee(),
                ],
                instruction: spend(*amount, 8000),
                expect: Expect::Accepted,
            };
            match run(&elf, program_id, &case) {
                Ok(Some(output)) => {
                    data = output.post_states[0].account().data.to_vec();
                    let r = PolicyRecord::decode(&data).expect("the guest wrote a record");
                    // The running total is the whole point of this loop and it
                    // was printed, never compared. Stopping the guest's own
                    // post-state from being fed forward — a `spend` that never
                    // persists the total — printed
                    //     ok    spend 1 of 200: 0 moved in period 0
                    // three times over and exited 0. Each spend has to move the
                    // total by exactly its amount, in the period it named.
                    let want = (i as u128 + 1) * amount;
                    if r.ledger.spent != want || r.ledger.window_start != 8000 {
                        failures += 1;
                        println!(
                            "  FAIL  spend {} of {}: the ledger reads {} in period {}, \
                             not {} in period 8000",
                            i + 1,
                            amount,
                            r.ledger.spent,
                            r.ledger.window_start,
                            want
                        );
                    } else {
                        println!(
                            "  ok    spend {} of {}: {} moved in period {}",
                            i + 1,
                            amount,
                            r.ledger.spent,
                            r.ledger.window_start
                        );
                    }
                }
                other => {
                    failures += 1;
                    println!("  FAIL  spend {} of {amount} was refused: {other:?}", i + 1);
                }
            }
        }
    }

    if failures == 0 {
        println!("  every hostile call halted with the code it must, and every honest one ran");
        Ok(())
    } else {
        bail!("{failures} adversarial case(s) did not behave as they must")
    }
}
