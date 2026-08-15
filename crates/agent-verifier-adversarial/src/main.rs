//! Run the committed guest against instructions that must be refused.
//!
//! WHY THIS EXISTS
//!
//! The two defects this program was fixed for were both found by *executing* the
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

use std::path::PathBuf;

use agent_policy_core::{
    compute_approval_marker, compute_policy_hash, compute_spend_ref, SpendLedger, SpendPolicy,
};
use anyhow::{bail, Context, Result};
use nssa_core::{
    account::{Account, AccountId, AccountWithMetadata, Data, Nonce},
    program::{ProgramId, ProgramOutput},
};
use risc0_zkvm::{compute_image_id, default_executor, ExecutorEnv};
use spel_framework_core::pda::compute_pda;

/// The guest's instruction enum, mirrored.
///
/// risc0's serde writes a variant index and then the fields in declaration
/// order, so the ORDER of these variants and of their fields is the ABI. Only
/// `CreatePolicy` and `Spend` are constructed here; the other two are present so
/// the indices are the guest's own.
#[derive(serde::Serialize)]
#[allow(dead_code)]
enum Instruction {
    CreatePolicy {
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
    },
    ApproveSpend {
        marker_seed: [u8; 32],
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        recipient: [u8; 32],
        amount: u128,
        nonce: u64,
    },
    Spend {
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
        amount: u128,
        window_start: u64,
    },
    SpendApproved {
        policy_hash: [u8; 32],
        owner_id: [u8; 32],
        agent_id: [u8; 32],
        per_tx: u128,
        per_period: u128,
        period_blocks: u64,
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

struct Case {
    what: &'static str,
    accounts: Vec<AccountWithMetadata>,
    instruction: Instruction,
    /// `None` = must be accepted, `Some(code)` = must halt with that code.
    expect: Option<u32>,
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
            // A halt looks like this from out here:
            //
            //   Guest panicked: Program error [12012]: Program error 6012: …
            //
            // The bracketed number is `SpelError::error_code()`, which offsets a
            // custom code by 6000 — so the code the program documents, and the
            // one an integration branches on, is the second one. Matching the
            // bracketed form would have matched 6012 against 12012 and passed a
            // case that halted for a different reason entirely.
            match case.expect {
                Some(code) if text.contains(&format!("Program error {code}: ")) => Ok(None),
                _ => bail!(text),
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

    // The cast of accounts. `attacker` is a real account with a real id; the
    // owner it names in the hostile cases is 32 bytes it made up.
    let attacker = [0x11u8; 32];
    let invented_owner = [0xaau8; 32];
    let real_owner = [0x22u8; 32];
    let agent = [0x33u8; 32];
    let other_agent = [0x44u8; 32];
    let recipient = [0x55u8; 32];

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

    // ── the anchoring hole ───────────────────────────────────────────────
    let unlimited_hash = compute_policy_hash(&invented_owner, &agent, &unlimited);
    let unlimited_pda = *compute_pda(&program_id, &[&unlimited_hash]).value();
    let attackers_own_hash = compute_policy_hash(&attacker, &agent, &unlimited);
    let attackers_own_pda = *compute_pda(&program_id, &[&attackers_own_hash]).value();

    // ── the spending path ────────────────────────────────────────────────
    let real_hash = compute_policy_hash(&real_owner, &agent, &real);
    let real_pda = *compute_pda(&program_id, &[&real_hash]).value();
    let policy_account = |data: Vec<u8>| with_id(account(program_id, 0, data), false, real_pda);
    let payer = |id: [u8; 32]| with_id(account(HOLDER_PROGRAM, 10_000, vec![]), true, id);
    let payee = || with_id(account(HOLDER_PROGRAM, 0, vec![]), false, recipient);
    let spend = |amount: u128, window_start: u64| Instruction::Spend {
        policy_hash: real_hash,
        owner_id: real_owner,
        agent_id: agent,
        per_tx: real.per_tx,
        per_period: real.per_period,
        period_blocks: real.period_blocks,
        amount,
        window_start,
    };
    // One approved payment, above the per-transaction ceiling, named exactly:
    // this policy, this recipient, this amount, this nonce.
    let approved_amount = 500u128;
    let approved_nonce = 7u64;
    let marker_seed = compute_approval_marker(&compute_spend_ref(
        &real_hash,
        &recipient,
        approved_amount,
        approved_nonce,
    ));
    let approval_pda = *compute_pda(&program_id, &[&marker_seed]).value();
    let approve = || Instruction::ApproveSpend {
        marker_seed,
        policy_hash: real_hash,
        owner_id: real_owner,
        agent_id: agent,
        per_tx: real.per_tx,
        per_period: real.per_period,
        period_blocks: real.period_blocks,
        recipient,
        amount: approved_amount,
        nonce: approved_nonce,
    };
    let spend_approved = || Instruction::SpendApproved {
        policy_hash: real_hash,
        owner_id: real_owner,
        agent_id: agent,
        per_tx: real.per_tx,
        per_period: real.per_period,
        period_blocks: real.period_blocks,
        recipient_id: recipient,
        amount: approved_amount,
        nonce: approved_nonce,
        marker_seed,
    };

    // What the policy account holds after 1000 has been moved in period 8000 —
    // not written by hand: this is `SpendLedger` as the guest encodes it, and
    // the run below checks the guest produces exactly these bytes.
    let period_spent_out = SpendLedger {
        window_start: 8000,
        spent: 1000,
    }
    .encode()
    .to_vec();

    let cases = vec![
        Case {
            what: "an agent anchors itself an unlimited policy, naming an owner it invented",
            accounts: vec![
                with_id(Account::default(), false, unlimited_pda),
                with_id(Account::default(), true, attacker),
            ],
            instruction: Instruction::CreatePolicy {
                policy_hash: unlimited_hash,
                owner_id: invented_owner,
                agent_id: agent,
                per_tx: unlimited.per_tx,
                per_period: unlimited.per_period,
                period_blocks: unlimited.period_blocks,
            },
            expect: Some(6012),
        },
        Case {
            what: "the same call, naming the account that actually signs — accepted",
            accounts: vec![
                with_id(Account::default(), false, attackers_own_pda),
                with_id(Account::default(), true, attacker),
            ],
            instruction: Instruction::CreatePolicy {
                policy_hash: attackers_own_hash,
                owner_id: attacker,
                agent_id: agent,
                per_tx: unlimited.per_tx,
                per_period: unlimited.per_period,
                period_blocks: unlimited.period_blocks,
            },
            expect: None,
        },
        Case {
            what: "an agent spends under a policy anchored for a different agent",
            accounts: vec![policy_account(vec![]), payer(other_agent), payee()],
            instruction: spend(200, 8000),
            expect: Some(6013),
        },
        Case {
            what: "the agent the policy names spends the same amount — accepted",
            accounts: vec![policy_account(vec![]), payer(agent), payee()],
            instruction: spend(200, 8000),
            expect: None,
        },
        Case {
            what: "a spend that would carry the period total past the per-period limit",
            accounts: vec![
                policy_account(period_spent_out.clone()),
                payer(agent),
                payee(),
            ],
            instruction: spend(1, 8000),
            expect: Some(6006),
        },
        Case {
            what: "the same spend in the next period, where the budget starts again — accepted",
            accounts: vec![policy_account(period_spent_out), payer(agent), payee()],
            instruction: spend(1, 9000),
            expect: None,
        },
        Case {
            what: "a period that does not start on a multiple of period_blocks",
            accounts: vec![policy_account(vec![]), payer(agent), payee()],
            instruction: spend(1, 8001),
            expect: Some(6014),
        },
        // ── the above-threshold path ─────────────────────────────────────
        //
        // Nothing else in this repository executes it: `scripts/a2a-task.sh`
        // settles inside the envelope and `scripts/e2e-local-sequencer.sh`
        // exercises `spend`. An untested path in the instruction that exists to
        // let an agent spend MORE than its ceiling is the last place to leave
        // unexercised.
        Case {
            what: "an agent signs its own owner approval",
            accounts: vec![
                with_id(Account::default(), false, approval_pda),
                policy_account(vec![]),
                with_id(account(HOLDER_PROGRAM, 0, vec![]), true, agent),
            ],
            instruction: approve(),
            expect: Some(6012),
        },
        Case {
            what: "the owner signs it — accepted",
            accounts: vec![
                with_id(Account::default(), false, approval_pda),
                policy_account(vec![]),
                with_id(account(HOLDER_PROGRAM, 0, vec![]), true, real_owner),
            ],
            instruction: approve(),
            expect: None,
        },
        Case {
            what: "an above-threshold spend on an approval anyone could have funded",
            accounts: vec![
                policy_account(vec![]),
                // Program-owned, at the right address, and not by this program:
                // exactly what funding an address gets you.
                with_id(account(HOLDER_PROGRAM, 0, vec![]), false, approval_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(),
            expect: Some(6007),
        },
        Case {
            what: "the same spend on the approval this program created — accepted",
            accounts: vec![
                policy_account(vec![]),
                with_id(account(program_id, 0, vec![]), false, approval_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(),
            expect: None,
        },
        Case {
            what: "presenting that approval a second time, after it was stamped",
            accounts: vec![
                policy_account(vec![]),
                with_id(account(program_id, 0, vec![1]), false, approval_pda),
                payer(agent),
                payee(),
            ],
            instruction: spend_approved(),
            expect: Some(6018),
        },
    ];

    let mut failures = 0;
    for case in &cases {
        match (run(&elf, program_id, case), case.expect) {
            (Ok(Some(output)), None) => {
                // Accepted. Say what it wrote, so "accepted" is not just the
                // absence of an error.
                let ledger = output
                    .post_states
                    .first()
                    .map(|p| SpendLedger::decode(&p.account().data))
                    .transpose()
                    .ok()
                    .flatten();
                match ledger {
                    Some(l) if l != SpendLedger::default() => println!(
                        "  ok    {}\n          policy account now records {} spent in period {}",
                        case.what, l.spent, l.window_start
                    ),
                    _ => println!("  ok    {}", case.what),
                }
            }
            (Ok(None), None) => {
                failures += 1;
                println!("  FAIL  {}\n          the guest halted without committing", case.what);
            }
            (Ok(None), Some(code)) => println!("  ok    refused [{code}]: {}", case.what),
            (Ok(_), Some(code)) => {
                failures += 1;
                println!("  FAIL  ACCEPTED, and it must halt [{code}]: {}", case.what);
            }
            (Err(e), Some(code)) => {
                failures += 1;
                println!("  FAIL  {}\n          expected [{code}], got: {e}", case.what);
            }
            (Err(e), None) => {
                failures += 1;
                println!("  FAIL  {}\n          must be accepted, got: {e}", case.what);
            }
        }
    }

    // The accumulation, end to end: run the accepted spend again with the
    // ledger the guest itself wrote, twice more, and watch the budget close.
    if failures == 0 {
        let mut data = Vec::new();
        for (i, amount) in [200u128, 200, 200].iter().enumerate() {
            let case = Case {
                what: "",
                accounts: vec![policy_account(data.clone()), payer(agent), payee()],
                instruction: spend(*amount, 8000),
                expect: None,
            };
            match run(&elf, program_id, &case) {
                Ok(Some(output)) => {
                    data = output.post_states[0].account().data.to_vec();
                    let l = SpendLedger::decode(&data).expect("the guest wrote a ledger");
                    println!(
                        "  ok    spend {} of {}: {} moved in period {}",
                        i + 1,
                        amount,
                        l.spent,
                        l.window_start
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
