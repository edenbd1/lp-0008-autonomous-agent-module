//! Foreign private (shielded) accounts: recipients addressed by keys, not by id.
//!
//! A private account on LEZ is not addressed the way a public one is. Its
//! `AccountId` is a hash of `(npk, vpk, identifier)`
//! (`lee/state_machine/core/src/nullifier.rs`, `for_regular_private_account`),
//! and the balance behind it lives in an encrypted note rather than in the
//! public state. So there are two entirely different things a `Private/<id>`
//! on the command line could mean:
//!
//! * **owned** — the signing wallet holds the nullifier secret key for that id,
//!   can prove membership of its existing commitment, and can therefore *spend*
//!   it. That is [`wallet::AccountIdentity::PrivateOwned`], and it is what this
//!   CLI used to build for every `Private/` account it was handed.
//! * **foreign** — the signing wallet holds no key for it at all and only wants
//!   to *credit* it. That is [`wallet::AccountIdentity::PrivateForeign`], which
//!   needs `(npk, vpk, identifier)` and no secret whatsoever.
//!
//! Building `PrivateOwned` for a recipient is what produced
//!
//! ```text
//! ❌ Failed to submit privacy-preserving transaction: KeyNotFoundError
//! ```
//!
//! before anything was proved: `AccountManager::new` sends `PrivateOwned`
//! through `private_key_tree_acc_preparation`, whose first act is
//! `wallet.storage.key_chain().private_account(account_id)`, and a wallet that
//! does not own the account has nothing to return. It is a **key lookup in the
//! local wallet**, not a limit of the protocol — the circuit has carried a
//! `PrivateForeignInit` output since `/LEE/v0.3`, and it is how every shielded
//! account on this chain was funded in the first place
//! (`wallet auth-transfer send --to-keys`).
//!
//! What was missing was only a way to *say* "foreign" on the command line, so
//! that is what this module adds:
//!
//! ```text
//! --recipient PrivateKeys/<path-to-keys-file>
//! --recipient PrivateKeys/<npk-hex>:<vpk-hex>
//! ```
//!
//! with an optional `#<identifier>` suffix on either form. The keys file is the
//! two-line format `wallet account show-keys` writes and `wallet --to-keys`
//! reads, so a payer can use the same file for a program call and for a plain
//! transfer.
//!
//! Nothing secret is involved. `npk` is a *public* nullifier key and `vpk` is an
//! ML-KEM-768 encapsulation key: they let anyone mint a note the recipient can
//! decrypt, and neither lets anyone spend it. That is why they can be published
//! in an agent's signed Agent Card.

use nssa::AccountId;
use nssa_core::NullifierPublicKey;
use nssa_core::encryption::ViewingPublicKey;

/// The prefix that marks an account argument as a foreign shielded recipient.
pub const PREFIX: &str = "PrivateKeys/";

/// A shielded recipient the signing wallet holds no key for.
#[derive(Clone, Debug)]
pub struct ForeignPrivate {
    pub npk: NullifierPublicKey,
    pub vpk: ViewingPublicKey,
    /// Diversifies the recipient's note. The payer chooses it, so a payee is
    /// paid at a *new* account id under its own keys rather than into the note
    /// it already holds — which is what "a shielded transfer mints a new note"
    /// means, and why the payee's id after a payment is not the one before it.
    pub identifier: u128,
}

impl ForeignPrivate {
    /// The account id this recipient will hold the new note under.
    ///
    /// Derived, not looked up: `(npk, vpk, identifier)` is everything the hash
    /// takes, so the payer knows the payee's post-payment id before submitting
    /// and can record it without asking the payee.
    #[must_use]
    pub fn account_id(&self) -> AccountId {
        AccountId::for_regular_private_account(&self.npk, &self.vpk, self.identifier)
    }
}

/// Split off a trailing `#<identifier>`, if present.
fn split_identifier(spec: &str) -> Result<(&str, Option<u128>), String> {
    match spec.rsplit_once('#') {
        None => Ok((spec, None)),
        Some((body, id)) => {
            let id = id.trim().parse::<u128>().map_err(|e| {
                format!("identifier after '#' must be a decimal u128: {e}")
            })?;
            Ok((body, Some(id)))
        },
    }
}

/// Decode the two hex strings a keys file (or an inline spec) carries.
fn decode_keys(npk_hex: &str, vpk_hex: &str) -> Result<(NullifierPublicKey, ViewingPublicKey), String> {
    let npk_bytes = crate::hex::hex_decode(npk_hex.trim())
        .map_err(|e| format!("npk must be valid hex: {e}"))?;
    let npk: [u8; 32] = npk_bytes.try_into().map_err(|v: Vec<u8>| {
        format!("npk must be exactly 32 bytes, got {}", v.len())
    })?;
    let vpk_bytes = crate::hex::hex_decode(vpk_hex.trim())
        .map_err(|e| format!("vpk must be valid hex: {e}"))?;
    let vpk_len = vpk_bytes.len();
    let vpk = ViewingPublicKey::from_bytes(vpk_bytes).map_err(|e| {
        format!(
            "vpk must be a {}-byte ML-KEM-768 encapsulation key (got {vpk_len}): {e:?}",
            ViewingPublicKey::LEN
        )
    })?;
    Ok((NullifierPublicKey(npk), vpk))
}

/// Parse a `PrivateKeys/...` account argument.
///
/// Returns `None` when the argument is not one — every other account spelling is
/// left exactly as it was, so this is additive: `Public/<id>` and `Private/<id>`
/// keep meaning what they meant.
///
/// # Errors
///
/// Returns `Err` when the argument *is* a `PrivateKeys/` spec but the keys
/// cannot be read or decoded. That is deliberately not silent: falling back to
/// `Private/<id>` would rebuild the `PrivateOwned` identity this exists to
/// avoid and fail later with `KeyNotFoundError`, pointing at the wrong thing.
pub fn parse(spec: &str) -> Option<Result<ForeignPrivate, String>> {
    let rest = spec.strip_prefix(PREFIX)?;
    Some(parse_body(rest))
}

fn parse_body(rest: &str) -> Result<ForeignPrivate, String> {
    let (body, identifier) = split_identifier(rest)?;
    let body = body.trim();
    if body.is_empty() {
        return Err(format!(
            "{PREFIX} needs a keys file or <npk-hex>:<vpk-hex>"
        ));
    }

    // Inline `npk:vpk` if there is a colon, otherwise a path to the two-line
    // file `wallet account show-keys` writes. A colon cannot appear in the hex
    // itself, so the two forms cannot be confused.
    let (npk_hex, vpk_hex) = if let Some((npk, vpk)) = body.split_once(':') {
        (npk.to_string(), vpk.to_string())
    } else {
        let content = std::fs::read_to_string(body)
            .map_err(|e| format!("could not read keys file '{body}': {e}"))?;
        let mut lines = content.lines().filter(|l| !l.trim().is_empty());
        let npk = lines
            .next()
            .ok_or_else(|| format!("keys file '{body}' is missing npk (line 1)"))?;
        let vpk = lines
            .next()
            .ok_or_else(|| format!("keys file '{body}' is missing vpk (line 2)"))?;
        (npk.to_string(), vpk.to_string())
    };

    let (npk, vpk) = decode_keys(&npk_hex, &vpk_hex)?;
    // A payer-chosen identifier, random when not supplied — the same default the
    // wallet's own `--to-identifier` uses. Random rather than derived, because a
    // guessable identifier gives an observer a candidate account id to test
    // commitments against.
    let identifier = identifier.unwrap_or_else(rand::random::<u128>);
    Ok(ForeignPrivate {
        npk,
        vpk,
        identifier,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn npk_hex() -> String {
        "ab".repeat(32)
    }

    fn vpk_hex() -> String {
        "cd".repeat(ViewingPublicKey::LEN)
    }

    #[test]
    fn non_prefixed_arguments_are_not_claimed() {
        assert!(parse("Public/abc").is_none());
        assert!(parse("Private/abc").is_none());
        assert!(parse("0xdeadbeef").is_none());
    }

    #[test]
    fn inline_keys_parse() {
        let spec = format!("{PREFIX}{}:{}", npk_hex(), vpk_hex());
        let f = parse(&spec).unwrap().unwrap();
        assert_eq!(f.npk.0, [0xab; 32]);
        assert_eq!(f.vpk.to_bytes(), vec![0xcd; ViewingPublicKey::LEN]);
    }

    #[test]
    fn explicit_identifier_is_honoured() {
        let spec = format!("{PREFIX}{}:{}#42", npk_hex(), vpk_hex());
        let f = parse(&spec).unwrap().unwrap();
        assert_eq!(f.identifier, 42);
    }

    #[test]
    fn identifier_defaults_to_random() {
        let spec = format!("{PREFIX}{}:{}", npk_hex(), vpk_hex());
        let a = parse(&spec).unwrap().unwrap();
        let b = parse(&spec).unwrap().unwrap();
        assert_ne!(
            a.identifier, b.identifier,
            "a fixed default identifier would make every payment to one payee \
             collide on the same account id"
        );
    }

    #[test]
    fn account_id_is_a_function_of_the_keys_and_the_identifier() {
        let spec = format!("{PREFIX}{}:{}#7", npk_hex(), vpk_hex());
        let a = parse(&spec).unwrap().unwrap();
        let b = parse(&spec).unwrap().unwrap();
        assert_eq!(a.account_id(), b.account_id());

        let other = format!("{PREFIX}{}:{}#8", npk_hex(), vpk_hex());
        let c = parse(&other).unwrap().unwrap();
        assert_ne!(
            a.account_id(),
            c.account_id(),
            "a different identifier must name a different note"
        );
    }

    #[test]
    fn keys_file_round_trips_the_wallet_format() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("payee.keys");
        // Exactly what `wallet account show-keys` writes.
        std::fs::write(&path, format!("{}\n{}\n", npk_hex(), vpk_hex())).unwrap();

        let spec = format!("{PREFIX}{}", path.to_str().unwrap());
        let f = parse(&spec).unwrap().unwrap();
        assert_eq!(f.npk.0, [0xab; 32]);
        assert_eq!(f.vpk.to_bytes(), vec![0xcd; ViewingPublicKey::LEN]);
    }

    #[test]
    fn a_short_vpk_is_rejected_rather_than_truncated() {
        let spec = format!("{PREFIX}{}:{}", npk_hex(), "cd".repeat(32));
        let err = parse(&spec).unwrap().unwrap_err();
        assert!(
            err.contains("ML-KEM-768"),
            "the error must name what was wrong, got: {err}"
        );
    }

    #[test]
    fn a_missing_keys_file_is_an_error_not_a_fallback() {
        let spec = format!("{PREFIX}/nonexistent/payee.keys");
        let err = parse(&spec).unwrap().unwrap_err();
        assert!(err.contains("could not read keys file"), "got: {err}");
    }
}
