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
//! WHAT THIS FILE GOT WRONG BEFORE, WHICH IS WORTH MORE THAN ANY CASE IN IT
//!
//! The previous version contained half the live attack and asserted it as the
//! BENIGN control: `"the same call, naming the account that actually signs —
//! accepted"`, `expect: None`. That call is an attacker anchoring an unlimited
//! policy over somebody else's agent while honestly naming itself as the owner
//! — the exact bypass — and the suite whose whole job was to prove the bypass
//! closed had it written down as required behaviour. It also never followed the
//! anchor with a `spend`, so the second step, the one that actually empties the
//! account, was never executed here at all.
//!
//! Both are fixed below. That anchoring call now has to REFUSE, and the two-step
//! attack — anchor, then spend the whole balance under it — runs end to end as a
//! regression, in the three shapes that were accepted by the previous binary on
//! chain. `Expect::Accepted` appears only where acceptance is the property under
//! test.

use std::path::PathBuf;

use agent_policy_core::{
    compute_approval_marker, compute_spend_ref, PolicyRecord, SpendLedger, SpendPolicy,
    POLICY_PDA_PREFIX,
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
/// Half the fields the previous deployment carried are gone, and that is the
/// fix rather than tidying: `spend` has no `agent_id`, no `owner_id`, no limits.
/// The policy account's address is derived from the paying account itself and
/// the limits are read out of that account, so there is nothing left in the call
/// for a caller to disagree with.
#[derive(serde::Serialize)]
#[allow(dead_code)]
enum Instruction {
    CreatePolicy {
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
/// worth stating because the attack is now closed by the second one.
///
/// A refusal the program body returns comes back through `SpelError`, which
/// `#[lez_program]` turns into
///
///   Guest panicked: Program error [12012]: Program error 6012: …
///
/// The bracketed number is `SpelError::error_code()`, which offsets a *custom*
/// code by 6000 — so this program's documented 6012 appears twice, once
/// unrecognisably. Matching the bracketed form alone would match 6012 against
/// 12012 and pass a case that halted for a completely different reason.
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
/// is reported. This matters here more than it looks: `AccountAlreadyInitialized`
/// *is* the fix for the anchoring bypass.
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
/// exists. One policy account per agent, and this is the refusal that says so.
const ALREADY_ANCHORED: Expect = Expect::Validation("AccountAlreadyInitialized");
/// The policy account presented is not the one this agent's id derives.
const WRONG_POLICY_ACCOUNT: Expect = Expect::Validation("PdaMismatch");

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
    // attacker holds — the property this deployment has to have is that holding
    // a key is not the same as being able to anchor.
    let attacker = [0x11u8; 32];
    let real_owner = [0x22u8; 32];
    let agent = [0x33u8; 32];
    let other_agent = [0x44u8; 32];
    let recipient = [0x55u8; 32];
    // The agent's own PUBLIC pay account. It is program-owned, which is what let
    // it anchor repeatedly against the previous binary, and the third executed
    // variant of the attack used exactly that account.
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

    // The policy account's address: this program, the constant prefix, the
    // agent. Nothing else — no owner, no limits, so there is exactly one per
    // agent. Recomputed here from the SAME constant the guest declares; if the
    // two ever drift, every PDA below misses and the macro's own check fails the
    // suite rather than letting it pass against an address nothing reads.
    let prefix = seed_from_str(POLICY_PDA_PREFIX);
    let policy_pda = |a: &[u8; 32]| *compute_pda(&program_id, &[&prefix, a]).value();

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

    // The one approval the honest owner grants: 900 to `recipient`, nonce 7.
    // Above the 200 per-transaction envelope, which is the only reason to have
    // an approval at all.
    let approved_marker =
        compute_approval_marker(&compute_spend_ref(&agent, &recipient, 900, 7));
    let marker_pda = *compute_pda(&program_id, &[&approved_marker]).value();
    let approve = |amount: u128, nonce: u64| Instruction::ApproveSpend {
        marker_seed: approved_marker,
        agent_id: agent,
        recipient,
        amount,
        nonce,
    };
    let spend_approved = |amount: u128, nonce: u64| Instruction::SpendApproved {
        recipient_id: recipient,
        amount,
        nonce,
        marker_seed: approved_marker,
    };

    let cases = vec![
        // ── anchoring: one policy account per agent, first writer wins ────
        Case {
            what: "the owner anchors its agent's policy, at an address nobody has taken — accepted",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&agent)),
                fresh_signer(real_owner),
            ],
            instruction: anchor(agent, real),
            expect: Expect::Accepted,
        },
        Case {
            what: "an attacker anchors an UNLIMITED policy over that agent, naming itself as owner",
            accounts: vec![agents_policy(&anchored), fresh_signer(attacker)],
            instruction: anchor(agent, unlimited),
            expect: ALREADY_ANCHORED,
        },
        Case {
            what: "the same, signed by the agent's own program-owned public pay account",
            accounts: vec![agents_policy(&anchored), owned_signer(agent_pay)],
            instruction: anchor(agent, unlimited),
            expect: ALREADY_ANCHORED,
        },
        Case {
            what: "the compromised agent itself anchors, as both owner and agent",
            accounts: vec![agents_policy(&anchored), payer(agent)],
            instruction: anchor(agent, unlimited),
            expect: ALREADY_ANCHORED,
        },
        Case {
            what: "an agent nobody has anchored yet still can be — accepted",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                fresh_signer(attacker),
            ],
            instruction: anchor(other_agent, unlimited),
            expect: Expect::Accepted,
        },
        Case {
            what: "an anchor whose policy account is not the PDA for the agent it names",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                fresh_signer(real_owner),
            ],
            instruction: anchor(agent, real),
            expect: WRONG_POLICY_ACCOUNT,
        },
        Case {
            what: "a policy with no period, which nothing could ever be accounted against",
            accounts: vec![
                with_id(Account::default(), false, policy_pda(&other_agent)),
                fresh_signer(real_owner),
            ],
            instruction: anchor(
                other_agent,
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
        Case {
            what: "a different agent presents this agent's policy account",
            accounts: vec![agents_policy(&anchored), payer(other_agent), payee()],
            instruction: spend(200, 8000),
            expect: WRONG_POLICY_ACCOUNT,
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
            instruction: approve(900, 7),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the same, signed by the agent's own public pay account",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                owned_signer(agent_pay),
            ],
            instruction: approve(900, 7),
            expect: Expect::Custom(6012),
        },
        Case {
            what: "the owner the policy record names signs it — accepted",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                fresh_signer(real_owner),
            ],
            instruction: approve(900, 7),
            expect: Expect::Accepted,
        },
        Case {
            what: "the owner signs, but the marker does not commit to the amount approved",
            accounts: vec![
                with_id(Account::default(), false, marker_pda),
                agents_policy(&anchored),
                fresh_signer(real_owner),
            ],
            instruction: approve(901, 7),
            expect: Expect::Custom(6003),
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
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(program_id, 0, vec![]), false, marker_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: Expect::Accepted,
        },
        Case {
            what: "presenting that approval a second time, after it was stamped",
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(program_id, 0, vec![1]), false, marker_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: Expect::Custom(6018),
        },
        Case {
            what: "a different agent presents an approval granted for this one",
            accounts: vec![
                agents_policy(&anchored),
                with_id(account(program_id, 0, vec![]), false, marker_pda),
                payer(other_agent),
                payee(),
            ],
            instruction: spend_approved(900, 7),
            expect: WRONG_POLICY_ACCOUNT,
        },
    ];

    let mut failures = 0;
    for case in &cases {
        match (run(&elf, program_id, case), case.expect) {
            (Ok(Some(output)), Expect::Accepted) => {
                // Accepted. Say what it wrote, so "accepted" is not just the
                // absence of an error.
                let record = output
                    .post_states
                    .first()
                    .and_then(|p| PolicyRecord::decode(&p.account().data).ok());
                match record {
                    Some(r) => println!(
                        "  ok    {}\n          policy account: per-tx {}, per-period {}, {} spent in period {}",
                        case.what,
                        r.policy.per_tx,
                        r.policy.per_period,
                        r.ledger.spent,
                        r.ledger.window_start
                    ),
                    None => println!("  ok    {}", case.what),
                }
            }
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

    // ── the attack, both steps, as it was executed on chain ──────────────
    //
    // The cases above are single calls. What emptied an agent under the previous
    // program was two: anchor an unlimited policy, then spend under it. The old
    // suite ran the first half and asserted it as correct; it never ran the
    // second at all. So run both, in order, in the three shapes the previous
    // binary accepted at halt 0 — and do not stop at the anchor being refused,
    // because "the anchor failed" is not the property. The property is that the
    // balance is still out of reach afterwards.
    println!("\n  the two-step attack that emptied an agent under the previous program:");
    for (who, signing) in [
        ("the compromised agent itself", payer(agent)),
        ("a separate account the attacker controls", fresh_signer(attacker)),
        ("the agent's own public pay account", owned_signer(agent_pay)),
    ] {
        let step1 = Case {
            what: "",
            accounts: vec![agents_policy(&anchored), signing],
            instruction: anchor(agent, unlimited),
            expect: ALREADY_ANCHORED,
        };
        match run(&elf, program_id, &step1) {
            Ok(None) => {}
            Ok(Some(_)) => {
                failures += 1;
                println!("  FAIL  {who}: anchored an unlimited policy over the agent");
                continue;
            }
            Err(e) => {
                failures += 1;
                println!("  FAIL  {who}: step 1 halted for the wrong reason: {e}");
                continue;
            }
        }
        // Step 2. The unlimited policy does not exist, so the only policy account
        // this agent has an address for is the owner's — present that one and try
        // to move everything.
        let step2 = Case {
            what: "",
            accounts: vec![agents_policy(&anchored), payer(agent), payee()],
            instruction: spend(10_000, 8000),
            expect: Expect::Custom(6005),
        };
        match run(&elf, program_id, &step2) {
            Ok(None) => println!(
                "  ok    {who}: the anchor refused [AccountAlreadyInitialized], and the\n          follow-up spend of the whole balance refused [6005] against the\n          owner's own policy"
            ),
            Ok(Some(_)) => {
                failures += 1;
                println!("  FAIL  {who}: the follow-up spend moved the agent's whole balance");
            }
            Err(e) => {
                failures += 1;
                println!("  FAIL  {who}: the follow-up spend halted for the wrong reason: {e}");
            }
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
                    println!(
                        "  ok    spend {} of {}: {} moved in period {}",
                        i + 1,
                        amount,
                        r.ledger.spent,
                        r.ledger.window_start
                    );
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
