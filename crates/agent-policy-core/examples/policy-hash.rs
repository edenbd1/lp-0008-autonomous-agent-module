//! Compute a policy hash with the same code the on-chain program runs.
//!
//! The deployment script needs the hash before it can call `create_policy`, and
//! computing it a second way — in shell, in python — is how the two drift and a
//! policy account gets created that nothing can spend against.
//!
//!   cargo run -p agent-policy-core --example policy-hash -- \
//!     <owner-hex> <agent-hex> <per_tx> <per_period> <period_blocks>
use agent_policy_core::{compute_policy_hash, SpendPolicy};

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() != 5 {
        eprintln!("usage: policy-hash <owner-hex> <agent-hex> <per_tx> <per_period> <period_blocks>");
        std::process::exit(2);
    }
    let owner = decode32(&a[0], "owner");
    let agent = decode32(&a[1], "agent");
    let policy = SpendPolicy {
        per_tx: a[2].parse().expect("per_tx must be an integer"),
        per_period: a[3].parse().expect("per_period must be an integer"),
        period_blocks: a[4].parse().expect("period_blocks must be an integer"),
    };
    println!("{}", hex(&compute_policy_hash(&owner, &agent, &policy)));
}

fn decode32(s: &str, what: &str) -> [u8; 32] {
    let bytes: Vec<u8> = (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex"))
        .collect();
    assert_eq!(bytes.len(), 32, "{what} must be 32 bytes as 64 hex characters");
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    out
}

fn hex(b: &[u8; 32]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
