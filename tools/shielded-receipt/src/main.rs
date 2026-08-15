//! What a shielded account received, read out of the chain's own copy of a
//! transaction.
//!
//! ```text
//! LEE_WALLET_HOME_DIR=~/.lp0008-agents/storage \
//!   shielded-receipt --payee 9Xpkkvos… --tx <64-hex> --expect-amount 1
//! ```
//!
//! # Why this exists
//!
//! `getAccount` cannot answer this question. A private account is a commitment
//! in the private state, and the RPC replies with a *default* account for one —
//! zero balance, zero nonce, default owner — which is indistinguishable from an
//! account that does not exist. So the usual proof that a payment happened
//! ("the payee's balance went up by the price, read off the chain") is not
//! available when the payee is shielded, and every substitute is worse:
//!
//! * `getTransaction` returning non-null says a transaction was included. It
//!   says nothing about what moved, and this chain drops failing transactions
//!   silently, so "included" is the *only* thing it can be trusted for.
//! * the receiving wallet's own balance after `account sync-private` is a
//!   number in `storage.json`. It is derived from the chain, but a reader has
//!   to take the wallet's word for that, and a local file is exactly what a
//!   fabricated result would look like.
//!
//! What this does instead is decrypt the note the transaction itself carries:
//!
//! 1. fetch the transaction by hash — the sequencer also returns the block it
//!    is in, which is the inclusion proof;
//! 2. for each private action, match the payee's **view tag**, then decapsulate
//!    the ML-KEM-768 ephemeral key against the payee's viewing secret key;
//! 3. ChaCha20-decrypt the note, giving the payee's post-state account and the
//!    `identifier` the payer chose;
//! 4. derive the account id from `(npk, vpk, identifier)` and **recompute the
//!    commitment** `SHA256(prefix ‖ account_id ‖ program_owner ‖ balance ‖
//!    nonce ‖ SHA256(data))`. It must equal one of the commitments the
//!    transaction publishes — which one is deliberately unknowable, since the
//!    outputs are shuffled.
//!
//! Step 4 is the point. The balance printed is not a balance the tool was told;
//! it is the only balance that reproduces the 32 bytes the chain committed to.
//! Change the amount by one and the commitment does not match.
//!
//! The wallet home is needed for one thing — the payee's viewing secret key,
//! without which the note is opaque. That is the payee reading its own mail,
//! which is what "the agent can receive" means. Nothing about the *amount*
//! comes from local state.

use std::str::FromStr as _;

use anyhow::{anyhow, bail, Context as _, Result};
use common::{transaction::LeeTransaction, HashType};
use lee::AccountId;
use lee_core::{
    account::Account, encryption::EncryptedAccountData, Commitment, EncryptionScheme,
    PrivateAccountKind,
};
use sequencer_service_rpc::RpcClient as _;
use wallet::WalletCore;

struct Args {
    payee: String,
    tx: Option<String>,
    block: Option<u64>,
    expect_amount: Option<u128>,
}

fn usage() -> ! {
    eprintln!(
        "usage: shielded-receipt --payee <account-id> (--tx <64-hex> | --block <n>) \
         [--expect-amount <n>]\n\
         \n\
         --payee   the shielded account whose keys should be tried. Any account id the\n\
                   wallet in LEE_WALLET_HOME_DIR holds a key chain for; the note that was\n\
                   paid is under the SAME keys but a DIFFERENT id, which is what this\n\
                   prints.\n\
         --tx      a transaction hash, hex, as `spel` and `wallet` print it.\n\
         --block   scan every transaction in one block instead. Useful when the hash was\n\
                   never recorded.\n\
         --expect-amount\n\
                   exit non-zero unless a decoded note holds exactly this balance."
    );
    std::process::exit(2)
}

fn parse_args() -> Args {
    let mut payee = None;
    let mut tx = None;
    let mut block = None;
    let mut expect_amount = None;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--payee" => payee = it.next(),
            "--tx" => tx = it.next(),
            "--block" => block = it.next().and_then(|v| v.parse().ok()),
            "--expect-amount" => expect_amount = it.next().and_then(|v| v.parse().ok()),
            "-h" | "--help" => usage(),
            other => {
                eprintln!("unknown argument: {other}");
                usage()
            },
        }
    }
    let Some(payee) = payee else { usage() };
    if tx.is_none() == block.is_none() {
        eprintln!("give exactly one of --tx and --block");
        usage()
    }
    Args {
        payee,
        tx,
        block,
        expect_amount,
    }
}

/// One decoded note, and the check that ties it to the chain.
struct Decoded {
    account_id: AccountId,
    identifier: u128,
    account: Account,
    /// The recomputed commitment appears among the transaction's committed
    /// outputs.
    ///
    /// Deliberately "appears among", not "is at the same index as the note".
    /// The two lists are not aligned — `PrivateAction` says so in as many
    /// words: *"the commitment in the action is not necessarily connected to
    /// the nullifier in content"* — and the outputs are shuffled precisely so
    /// that an observer cannot read who paid whom off the ordering. Checking
    /// position would fail on a correct transaction.
    ///
    /// False is not "probably fine": it means the plaintext decrypted but does
    /// not hash to anything the chain stored, so the balance below is not a
    /// committed balance.
    commitment_matches: bool,
}

/// `shared_secret` is the payee's viewing secret key, wrapped: give it the
/// note's ephemeral key and it returns the secret that opens that note, or
/// `None` when the note was not meant for these keys.
fn decode_for<F>(
    tx: &LeeTransaction,
    npk: &lee_core::NullifierPublicKey,
    vpk: &lee_core::encryption::ViewingPublicKey,
    mut shared_secret: F,
) -> Vec<Decoded>
where
    F: FnMut(&lee_core::encryption::EphemeralPublicKey) -> Option<lee_core::SharedSecretKey>,
{
    let LeeTransaction::PrivacyPreserving(pp) = tx else {
        return vec![];
    };
    let want_tag = EncryptedAccountData::compute_view_tag(npk, vpk);
    let mut out = vec![];
    for action in &pp.message.private_actions {
        // The view tag is a one-byte filter, not a proof of anything: it lets a
        // scanner skip notes that cannot be its without doing a decapsulation
        // per note. A collision just means the decrypt below fails.
        if action.encrypted_post_state.view_tag != want_tag {
            continue;
        }
        let Some(secret) = shared_secret(&action.encrypted_post_state.epk) else {
            continue;
        };
        let Some((kind, account)) = EncryptionScheme::decrypt(
            &action.encrypted_post_state.ciphertext,
            &secret,
            &action.nullifier,
        ) else {
            continue;
        };
        let PrivateAccountKind::Regular(identifier) = kind else {
            // A private PDA under the same keys. Not a payment to the agent.
            continue;
        };
        let account_id = AccountId::for_regular_private_account(npk, vpk, identifier);
        let recomputed = Commitment::new(&account_id, &account);
        let commitment_matches = pp
            .message
            .private_actions
            .iter()
            .any(|a| a.commitment == recomputed);
        out.push(Decoded {
            account_id,
            identifier,
            account,
            commitment_matches,
        });
    }
    out
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = parse_args();

    let payee_id = AccountId::from_str(args.payee.trim_start_matches("Private/"))
        .map_err(|e| anyhow!("--payee is not a base58 account id: {e}"))?;

    let core = WalletCore::from_env()
        .await
        .context("could not open the wallet — set LEE_WALLET_HOME_DIR to the payee's home")?;

    let found = core
        .storage()
        .key_chain()
        .private_account(payee_id)
        .ok_or_else(|| {
            anyhow!(
                "the wallet in LEE_WALLET_HOME_DIR holds no key chain for {payee_id}. \
                 Without the viewing secret key this transaction cannot be opened — which is \
                 the privacy property, not a bug."
            )
        })?;
    let npk = found.key_chain.nullifier_public_key;
    let vpk = found.key_chain.viewing_public_key.clone();
    let key_chain = found.key_chain;

    println!("payee keys (public, publishable)");
    println!("  npk {}", hex::encode(npk.0));
    println!("  vpk {}…{} ({} bytes)", {
        let b = vpk.to_bytes();
        hex::encode(&b[..8])
    }, {
        let b = vpk.to_bytes();
        hex::encode(&b[b.len() - 8..])
    }, vpk.to_bytes().len());
    println!();

    let client = core.helm_owned();

    // (hash, block, transaction) for everything to be examined.
    let mut subjects: Vec<(String, u64, LeeTransaction)> = vec![];
    if let Some(hex_hash) = &args.tx {
        let raw = hex::decode(hex_hash.trim()).context("--tx must be hex")?;
        let bytes: [u8; 32] = raw
            .try_into()
            .map_err(|v: Vec<u8>| anyhow!("--tx must be 32 bytes, got {}", v.len()))?;
        // Inclusion. A dropped, a pending, a rejected and a never-submitted hash
        // are all `null` here and cannot be told apart, so the block id is the
        // only thing that distinguishes a settlement from a hash somebody typed.
        let Some((tx, block)) = client
            .get_transaction(HashType(bytes))
            .await
            .context("getTransaction failed")?
        else {
            bail!(
                "{} is not in any block. On this chain that answer is identical for a dropped, \
                 a pending, a rejected and a never-submitted transaction — it is not evidence \
                 of anything except that there is nothing to decode.",
                hex_hash
            );
        };
        subjects.push((hex_hash.trim().to_string(), block, tx));
    } else if let Some(id) = args.block {
        let block = client
            .get_block(id)
            .await
            .context("getBlock failed")?
            .ok_or_else(|| anyhow!("block {id} does not exist"))?;
        for tx in block.body.transactions {
            subjects.push((hex::encode(tx.hash().0), id, tx));
        }
    }

    let mut hits = 0_usize;
    let mut matched_expected = false;
    for (hash, block, tx) in &subjects {
        let decoded = decode_for(tx, &npk, &vpk, |epk| {
            key_chain.calculate_shared_secret_receiver(epk)
        });
        if decoded.is_empty() {
            continue;
        }
        println!("transaction {hash}");
        println!("  included in block {block}");
        for d in decoded {
            hits += 1;
            println!("  note for these keys");
            println!("    account id     Private/{}", d.account_id);
            println!("    identifier     {}", d.identifier);
            println!("    balance        {}", d.account.balance);
            println!("    program_owner  {:?}", d.account.program_owner);
            println!("    nonce          {}", d.account.nonce.0);
            if d.commitment_matches {
                println!(
                    "    commitment     matches one committed in this transaction \
                     — the balance above is the committed post-state"
                );
            } else {
                println!(
                    "    commitment     DOES NOT MATCH the transaction's commitment — the \
                     balance above is not what the chain stored"
                );
            }
            if let Some(want) = args.expect_amount {
                if d.account.balance == want && d.commitment_matches {
                    matched_expected = true;
                }
            }
        }
    }

    if hits == 0 {
        bail!(
            "no note in {} transaction(s) decrypts under these keys",
            subjects.len()
        );
    }
    if let Some(want) = args.expect_amount {
        if !matched_expected {
            bail!("no note holding exactly {want}, with a matching commitment, was found");
        }
        println!();
        println!("OK: a note of {want} is committed to these keys, on chain.");
    }
    Ok(())
}
