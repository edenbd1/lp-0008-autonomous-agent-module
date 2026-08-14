//! Derive the marker seed for one spend, with the code the chain runs.
//!
//!   cargo run -p agent-policy-core --example spend-marker -- \
//!     <policy-hash-hex> <recipient-hex> <amount> <nonce>
use agent_policy_core::{compute_approval_marker, compute_spend_ref};

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() != 4 {
        eprintln!("usage: spend-marker <policy-hash> <recipient> <amount> <nonce>");
        std::process::exit(2);
    }
    let policy = d32(&a[0]);
    let recipient = d32(&a[1]);
    let amount: u128 = a[2].parse().expect("amount");
    let nonce: u64 = a[3].parse().expect("nonce");
    let r = compute_spend_ref(&policy, &recipient, amount, nonce);
    println!("{}", compute_approval_marker(&r).iter().map(|x| format!("{x:02x}")).collect::<String>());
}

fn d32(s: &str) -> [u8; 32] {
    let v: Vec<u8> = (0..s.len()).step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex")).collect();
    assert_eq!(v.len(), 32);
    let mut o = [0u8; 32]; o.copy_from_slice(&v); o
}
