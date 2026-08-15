#pragma once

#include <cstdint>
#include <string>

/**
 * @brief The approval marker seed, derived exactly as the chain derives it.
 *
 * WHY THIS FILE HAD TO EXIST BEFORE THE OWNER CHANNEL COULD WORK
 *
 * `OwnerChannel::requestApproval` refuses a request whose `markerSeed` is empty
 * — "the request carries no approval marker seed" — before it sends anything.
 * The module's Delivery-backed approval path was wired with an empty one and a
 * comment saying the derivation lived in a crate this module does not link, so
 * the composition could never have produced an approval. Every part of it was
 * separately tested and the assembly was not, which is the failure mode this
 * repository has now hit four times. Deriving it here is what closes that.
 *
 * WHY THE SEED MATTERS AT ALL, RATHER THAN BEING A CORRELATION TOKEN
 *
 * It names the account `spend_approved` will look for on chain: the owner's
 * `approve_spend` creates an account at a PDA seeded by this value, once, for
 * this exact payment. So it is the one field in the exchange that is *not*
 * bookkeeping — an agent shown the wrong seed asks the owner to authorise a
 * different payment than the one it will submit, and an owner who signs against
 * a seed the agent did not derive creates an account the agent will never
 * present.
 *
 * Both sides therefore derive it independently from the same four facts, and
 * the reply echoing it is a real agreement rather than a copy of something the
 * agent asserted. That is why `OwnerChannel` compares it and why this is a pure
 * function of the request.
 *
 * THE DERIVATION, WHICH IS NOT OURS TO CHOOSE
 *
 * `crates/agent-policy-core/src/lib.rs` is the authority, and the guest program
 * on chain re-derives it:
 *
 *   spend_ref = SHA-256("/lp-0008/v0.2/SpendRef/"    || agent32 || recipient32
 *                                                    || amount_le128 || nonce_le64)
 *   marker    = SHA-256("/lp-0008/v0.2/ApprovalMark/" || spend_ref32)
 *
 * `agent32` and `recipient32` are the base58 account ids decoded to bytes;
 * `amount` is little-endian u128; `nonce` little-endian u64. A second
 * implementation of a derivation is a second answer to one question, so this
 * one is checked against the first: `module/tests/spend_marker_test.cpp`
 * compares it against vectors produced by the Rust crate itself, and the
 * `skills` job in CI regenerates those vectors rather than trusting a table
 * somebody pasted.
 */
namespace logos::agent {

/// SHA-256 of `data`, as 64 lower-case hex characters.
///
/// Exposed rather than hidden because a hash with no way to test it against
/// published vectors is a hash nobody has checked; `spend_marker_test.cpp` runs
/// the FIPS 180-4 examples through this.
std::string sha256Hex(const std::string &data);

/// Base58 (Bitcoin alphabet) decode. False when `text` is not base58 or does
/// not decode to exactly `expectedBytes` bytes.
///
/// The length check is not pedantry: a LEZ account id is 32 bytes, and a
/// truncated decode would silently produce a seed for an account nobody
/// created — which surfaces as a spend the chain refuses, long after the owner
/// said yes.
bool base58Decode(const std::string &text, std::size_t expectedBytes, std::string &out);

/// The approval marker seed for one payment, as 64 lower-case hex characters,
/// or empty when an input cannot be used.
///
/// Empty is a refusal and never a fallback: a spend whose marker cannot be
/// derived is one the owner cannot be asked about correctly, and the caller
/// must not send it. `amount` is decimal digits, up to u128.
std::string approvalMarkerSeed(const std::string &agentAccount,
                               const std::string &recipientAccount,
                               const std::string &amount,
                               std::uint64_t nonce);

} // namespace logos::agent
