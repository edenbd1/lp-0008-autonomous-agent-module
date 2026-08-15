//! Derive the marker seed for one spend, with the code the chain runs.
//!
//!   cargo run -p agent-policy-core --example spend-marker -- \
//!     <agent-hex> <recipient-hex> <amount> <nonce>
//!
//! The first argument used to be a policy hash. It is the agent's account id
//! now, and that is the fix rather than a rename: a policy account's address is
//! derived from the agent alone, so naming the agent names the policy, and there
//! is no second policy for the same agent to replay an approval under.
use agent_policy_core::{compute_approval_marker, compute_spend_ref};

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() != 4 {
        eprintln!("usage: spend-marker <agent> <recipient> <amount> <nonce>");
        std::process::exit(2);
    }
    let agent = d32(&a[0]);
    let recipient = d32(&a[1]);
    let amount: u128 = a[2].parse().expect("amount");
    let nonce: u64 = a[3].parse().expect("nonce");
    let r = compute_spend_ref(&agent, &recipient, amount, nonce);
    println!("{}", compute_approval_marker(&r).iter().map(|x| format!("{x:02x}")).collect::<String>());
}

fn d32(s: &str) -> [u8; 32] {
    let v: Vec<u8> = (0..s.len()).step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex")).collect();
    assert_eq!(v.len(), 32);
    let mut o = [0u8; 32]; o.copy_from_slice(&v); o
}
