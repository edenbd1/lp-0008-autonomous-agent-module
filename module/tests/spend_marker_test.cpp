// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The module's copy of the approval-marker derivation, against the crate the
// chain runs.
//
//   spend_marker_test --rust 'cargo run -q -p agent-policy-core --example spend-marker --'
//
// WHY THE CROSS-CHECK IS THE WHOLE POINT
//
// `module/src/spend_marker.cpp` is a SECOND implementation of a derivation
// whose first implementation is `crates/agent-policy-core`. Two
// implementations of one derivation are two answers to one question, and the
// disagreement would be invisible until an owner approved a payment against a
// marker the chain never looks for — a spend that fails long after everybody
// believed it was authorised.
//
// So this does not compare against a table of expected values pasted into a
// header. It RUNS the crate, on inputs chosen here, and compares. A table would
// go stale the day the prefix changes and would go stale silently, which is the
// exact failure this repository has now paid for four times.
//
// Without `--rust` the cross-check cannot run, and this exits non-zero saying
// so rather than passing on the checks it could do. A skipped check counts as
// not run.

#include "../src/spend_marker.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using logos::agent::approvalMarkerSeed;
using logos::agent::base58Decode;
using logos::agent::sha256Hex;

namespace {

int failures = 0;

void check(bool condition, const std::string &what)
{
    std::printf(condition ? "  ok   %s\n" : "  FAIL %s\n", what.c_str());
    if (!condition) ++failures;
    std::fflush(stdout);
}

std::string run(const std::string &command)
{
    std::unique_ptr<FILE, int (*)(FILE *)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) return {};
    std::string out;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) {
        out += buffer.data();
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    // The generator prints the marker on its own line; cargo may print build
    // chatter before it, so the last line is the answer.
    const auto nl = out.find_last_of('\n');
    return nl == std::string::npos ? out : out.substr(nl + 1);
}

std::string toHex(const std::string &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(digits[u >> 4]);
        out.push_back(digits[u & 0x0F]);
    }
    return out;
}

std::string arg(int argc, char **argv, const std::string &name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) return argv[i + 1];
    }
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    std::printf("\n1. SHA-256, against FIPS 180-4\n");
    // The two worked examples in the standard, plus the empty string, plus an
    // input that crosses the padding boundary — 55, 56 and 64 bytes are where a
    // hand-written padding routine goes wrong, and it goes wrong quietly.
    check(sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "\"abc\"");
    check(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "the 56-byte example");
    check(sha256Hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "the empty string");
    check(sha256Hex(std::string(55, 'a')) ==
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
          "55 bytes — the last length that pads into one block");
    check(sha256Hex(std::string(56, 'a')) ==
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
          "56 bytes — the first that needs two");
    check(sha256Hex(std::string(64, 'a')) ==
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
          "64 bytes — a whole block, then a whole block of padding");
    check(sha256Hex(std::string(1000, 'a')) ==
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "1000 bytes");

    std::printf("\n2. base58, and the length it must refuse\n");
    std::string decoded;
    check(base58Decode("5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ", 32, decoded) &&
              toHex(decoded) ==
                  "41fb94faba6f98fc41deb409eb9d2aff6f5da11289c5e410f62f72e99fccad32",
          "a live LEZ account id decodes to its 32 bytes");
    // TWO characters short, not one, and the difference is the reason this check
    // is here at all. Dropping one base58 character divides the value by 58 and
    // usually leaves the byte length alone, so `…HtjnZ` and `…Htjn` both decode
    // to 32 bytes and both would pass a length check. Dropping two is what
    // actually shortens it. Written down because the first version of this
    // assertion asserted the wrong thing and went red on correct code.
    check(!base58Decode("5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtj", 32, decoded),
          "an id two characters short decodes to 31 bytes and is refused");
    check(!base58Decode("5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN570AYqhNddHtjnZ", 32, decoded),
          "'0' is not in the alphabet, so an id carrying one is refused");
    check(!base58Decode("", 32, decoded), "the empty string is not an account");
    check(!base58Decode("IOl", 32, decoded), "nor are the three lookalike characters");
    // Leading '1's are leading zero bytes and are the case a naive big-integer
    // decoder loses, because they carry no magnitude.
    check(base58Decode(std::string(32, '1'), 32, decoded) &&
              toHex(decoded) == std::string(64, '0'),
          "thirty-two '1's are thirty-two zero bytes, not one");
    check(!base58Decode(std::string(33, '1'), 32, decoded),
          "and thirty-three of them are thirty-three bytes, which is not an account");

    std::printf("\n3. the marker seed, against the crate the chain runs\n");
    const std::string rust = arg(argc, argv, "--rust");
    if (rust.empty()) {
        std::printf("  FAIL no --rust generator was given, so the derivation was compared\n"
                    "       with nothing. Run it as:\n"
                    "       %s --rust 'cargo run -q -p agent-policy-core --example spend-marker --'\n",
                    argv[0]);
        std::printf("\nFAILED (%d failure(s))\n", failures + 1);
        return 1;
    }

    struct Case {
        const char *agent;
        const char *agentHex;
        const char *recipient;
        const char *recipientHex;
        const char *amount;
        std::uint64_t nonce;
        const char *what;
    };
    // Two real accounts from artifacts/agents.tsv, and the boundary values a
    // little-endian encoder gets wrong: zero, one, and u64/u128 maxima.
    const Case cases[] = {
        {"5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ",
         "41fb94faba6f98fc41deb409eb9d2aff6f5da11289c5e410f62f72e99fccad32",
         "BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu",
         "a352e1e00fdf00b6abc9872c3c8106f55ce0402667af29c33c4d864516ce0192",
         "250", 12345, "an ordinary spend"},
        {"5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ",
         "41fb94faba6f98fc41deb409eb9d2aff6f5da11289c5e410f62f72e99fccad32",
         "BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu",
         "a352e1e00fdf00b6abc9872c3c8106f55ce0402667af29c33c4d864516ce0192",
         "0", 0, "zero amount, zero nonce"},
        {"5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ",
         "41fb94faba6f98fc41deb409eb9d2aff6f5da11289c5e410f62f72e99fccad32",
         "BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu",
         "a352e1e00fdf00b6abc9872c3c8106f55ce0402667af29c33c4d864516ce0192",
         "18446744073709551616", 18446744073709551615ULL,
         "an amount above 2^64 and the largest nonce"},
        {"5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ",
         "41fb94faba6f98fc41deb409eb9d2aff6f5da11289c5e410f62f72e99fccad32",
         "BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu",
         "a352e1e00fdf00b6abc9872c3c8106f55ce0402667af29c33c4d864516ce0192",
         "340282366920938463463374607431768211455", 1,
         "the largest u128 amount"},
    };

    std::string firstMarker;
    for (const Case &c : cases) {
        const std::string command = rust + " " + c.agentHex + " " + c.recipientHex + " " +
                                    c.amount + " " + std::to_string(c.nonce);
        const std::string expected = run(command);
        const std::string got = approvalMarkerSeed(c.agent, c.recipient, c.amount, c.nonce);
        if (firstMarker.empty()) firstMarker = got;
        check(expected.size() == 64,
              std::string("the crate produced a seed for ") + c.what + ": " + expected);
        check(!got.empty() && got == expected,
              std::string("and this module derives the same one — ") + c.what);
        if (got != expected) {
            std::printf("       crate:  %s\n       module: %s\n", expected.c_str(), got.c_str());
        }
    }

    std::printf("\n4. the derivation is of the payment, not of something near it\n");
    // Every field has to reach the digest. A marker that ignored one of them
    // would let an approval for 250 settle a payment of 251, and the check that
    // catches it is not "does it look like a hash".
    const Case &base = cases[0];
    const std::string same = approvalMarkerSeed(base.agent, base.recipient, base.amount, base.nonce);
    check(same == firstMarker, "the same four fields give the same seed twice");
    check(approvalMarkerSeed(base.recipient, base.agent, base.amount, base.nonce) != same,
          "swapping agent and recipient changes it");
    check(approvalMarkerSeed(base.agent, base.recipient, "251", base.nonce) != same,
          "one LEZ more changes it");
    check(approvalMarkerSeed(base.agent, base.recipient, base.amount, base.nonce + 1) != same,
          "the next nonce changes it");
    check(approvalMarkerSeed("Public/" + std::string(base.agent), base.recipient, base.amount,
                             base.nonce) == same,
          "and a `Public/` qualifier is stripped rather than hashed");
    check(approvalMarkerSeed("not-an-account", base.recipient, base.amount, base.nonce).empty(),
          "an account that is not base58 gives no seed at all, rather than a plausible one");
    check(approvalMarkerSeed(base.agent, base.recipient, "12x", base.nonce).empty(),
          "and neither does an amount that is not decimal digits");

    std::printf("\n%s (%d failure(s))\n",
                failures ? "FAILED" : "the module derives the marker the chain will look for",
                failures);
    return failures ? 1 : 0;
}
