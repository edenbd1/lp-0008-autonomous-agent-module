//! Print the period a block falls in, with the arithmetic the chain checks.
//!
//! A spend names the period it is spending in, and the guest refuses any name
//! that is not a multiple of `period_blocks` — so a caller that computes this
//! its own way in shell gets error 6014 rather than a payment. One
//! implementation, in the crate the guest links.
//!
//!   cargo run -p agent-policy-core --example window-start -- <block> <period_blocks>
//!
//! Prints `<window_start> <window_end>`: the transaction is includable in
//! `[window_start, window_end)` and nowhere else.
use agent_policy_core::SpendPolicy;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() != 2 {
        eprintln!("usage: window-start <block> <period_blocks>");
        std::process::exit(2);
    }
    let block: u64 = a[0].parse().expect("block must be an integer");
    let policy = SpendPolicy {
        per_tx: 0,
        per_period: 0,
        period_blocks: a[1].parse().expect("period_blocks must be an integer"),
    };
    if policy.period_blocks == 0 {
        eprintln!("period_blocks must not be zero");
        std::process::exit(2);
    }
    let start = policy.window_start_for(block);
    let (start, end) = policy.window_bounds(start).expect("the window fits in u64");
    println!("{start} {end}");
}
