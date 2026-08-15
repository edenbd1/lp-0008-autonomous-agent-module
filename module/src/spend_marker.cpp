#include "spend_marker.h"

#include <array>
#include <cstring>
#include <vector>

namespace logos::agent {

namespace {

// ---------------------------------------------------------------------------
// SHA-256, FIPS 180-4
//
// Written out rather than pulled in: this plugin links no crypto library, and
// the one other place in this repository that needs a digest — signing an Agent
// Card — delegates to an external signer precisely so no curve arithmetic lives
// here. A hash is a different proposition from a signature: it holds no key, it
// has published test vectors, and getting it wrong is detectable by anyone.
// `spend_marker_test.cpp` runs the FIPS examples and the crate's own vectors
// through it, and a wrong digest fails both.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void sha256Compress(std::uint32_t state[8], const unsigned char block[64])
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

std::array<unsigned char, 32> sha256Raw(const std::string &data)
{
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::size_t length = data.size();
    std::size_t offset = 0;
    for (; offset + 64 <= length; offset += 64) {
        sha256Compress(state, reinterpret_cast<const unsigned char *>(data.data()) + offset);
    }
    unsigned char tail[128] = {0};
    const std::size_t remainder = length - offset;
    if (remainder != 0) {
        std::memcpy(tail, data.data() + offset, remainder);
    }
    tail[remainder] = 0x80;
    // One padded block when the length field fits, two when it does not.
    const std::size_t blocks = remainder + 1 + 8 > 64 ? 2 : 1;
    const std::uint64_t bits = static_cast<std::uint64_t>(length) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[blocks * 64 - 1 - i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
    }
    for (std::size_t i = 0; i < blocks; ++i) {
        sha256Compress(state, tail + i * 64);
    }
    std::array<unsigned char, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xFF);
    }
    return digest;
}

std::string toHex(const unsigned char *bytes, std::size_t n)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

/// Decimal string to little-endian u128 bytes. False on anything that is not
/// digits, or on overflow — an amount that does not fit is not an amount.
bool amountToLe128(const std::string &amount, unsigned char out[16])
{
    if (amount.empty() || amount.size() > 39) return false;
    unsigned __int128 value = 0;
    for (const char c : amount) {
        if (c < '0' || c > '9') return false;
        const unsigned __int128 next = value * 10 + static_cast<unsigned>(c - '0');
        if (next < value) return false;   // wrapped
        value = next;
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
    }
    return true;
}

} // namespace

std::string sha256Hex(const std::string &data)
{
    const auto digest = sha256Raw(data);
    return toHex(digest.data(), digest.size());
}

bool base58Decode(const std::string &text, std::size_t expectedBytes, std::string &out)
{
    static const char *alphabet =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    out.clear();
    if (text.empty()) return false;

    std::vector<unsigned char> bytes;
    bytes.reserve(expectedBytes + 1);
    for (const char c : text) {
        const char *found = std::strchr(alphabet, c);
        // `strchr` finds the terminating NUL, so a NUL byte in the input would
        // otherwise be accepted as digit 58.
        if (found == nullptr || c == '\0') return false;
        int carry = static_cast<int>(found - alphabet);
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            carry += 58 * static_cast<int>(*it);
            *it = static_cast<unsigned char>(carry & 0xFF);
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.insert(bytes.begin(), static_cast<unsigned char>(carry & 0xFF));
            carry >>= 8;
        }
    }
    // Leading '1's are leading zero bytes, which the loop above cannot produce.
    std::size_t leadingZeros = 0;
    for (const char c : text) {
        if (c != '1') break;
        ++leadingZeros;
    }
    std::string decoded(leadingZeros, '\0');
    decoded.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (decoded.size() != expectedBytes) return false;
    out = std::move(decoded);
    return true;
}

std::string approvalMarkerSeed(const std::string &agentAccount,
                               const std::string &recipientAccount,
                               const std::string &amount,
                               std::uint64_t nonce)
{
    // Both accounts arrive from configuration or from a peer, and either may be
    // qualified. `spel` writes `Public/<id>` and `Private/<id>`; the chain
    // hashes the 32 raw bytes, so the qualifier is stripped rather than hashed.
    const auto bare = [](const std::string &account) {
        const auto slash = account.find('/');
        return slash == std::string::npos ? account : account.substr(slash + 1);
    };

    std::string agent32;
    std::string recipient32;
    if (!base58Decode(bare(agentAccount), 32, agent32)) return {};
    if (!base58Decode(bare(recipientAccount), 32, recipient32)) return {};
    unsigned char amount16[16];
    if (!amountToLe128(amount, amount16)) return {};

    // The two prefixes are `SPEND_REF_PREFIX` and `APPROVAL_MARKER_PREFIX` in
    // crates/agent-policy-core/src/lib.rs. They are spelled out rather than
    // derived from anything, and `spend_marker_test.cpp` is what keeps the two
    // copies from drifting: it re-runs the crate and compares.
    std::string spendRefInput = "/lp-0008/v0.2/SpendRef/";
    spendRefInput += agent32;
    spendRefInput += recipient32;
    spendRefInput.append(reinterpret_cast<const char *>(amount16), 16);
    for (int i = 0; i < 8; ++i) {
        spendRefInput.push_back(static_cast<char>((nonce >> (8 * i)) & 0xFF));
    }
    const auto spendRef = sha256Raw(spendRefInput);

    std::string markerInput = "/lp-0008/v0.2/ApprovalMark/";
    markerInput.append(reinterpret_cast<const char *>(spendRef.data()), spendRef.size());
    const auto marker = sha256Raw(markerInput);
    return toHex(marker.data(), marker.size());
}

} // namespace logos::agent
