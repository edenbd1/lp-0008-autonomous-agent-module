// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Authenticated encryption for the storage skills, self-contained: SHA-256 in
// counter mode for the keystream and HMAC-SHA256 for the tag, encrypt-then-MAC.
// No external dependency and no new link, because the alternative — a library
// linked into the plugin — is a runtime dependency a reviewer's Logos Core may
// not have, and a package plugin that fails to dlopen is worse than a documented
// gap. The primitives are standard and carry known-answer tests beside them.
namespace logos::agent::vault {

// 32-byte SHA-256 digest of `data`.
std::vector<std::uint8_t> sha256(const std::vector<std::uint8_t> &data);

// HMAC-SHA256(key, message), 32 bytes.
std::vector<std::uint8_t> hmacSha256(const std::vector<std::uint8_t> &key,
                                     const std::vector<std::uint8_t> &message);

// Encrypt `plaintext` under a 32-byte `key`. Output is
// nonce(24) || ciphertext || tag(32) and is safe to store or upload as-is.
// A fresh random nonce is drawn per call, so the same file uploaded twice
// yields two unrelated blobs.
std::vector<std::uint8_t> seal(const std::vector<std::uint8_t> &key,
                               const std::vector<std::uint8_t> &plaintext);

// Reverse of seal. Returns false and leaves `out` empty if the tag does not
// verify or the blob is too short — a corrupted or wrong-key blob never
// produces plausible plaintext.
bool open(const std::vector<std::uint8_t> &key,
          const std::vector<std::uint8_t> &blob,
          std::vector<std::uint8_t> &out);

// A fresh 32-byte key from the system CSPRNG.
std::vector<std::uint8_t> newKey();

} // namespace logos::agent::vault
