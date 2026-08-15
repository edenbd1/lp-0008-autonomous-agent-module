// SPDX-License-Identifier: MIT OR Apache-2.0
//
// A third-party skill for the LP-0008 agent module.
//
// This file is the whole point of the directory it sits in. It is compiled on
// its own, into its own shared library, and it is not part of the agent module:
// nothing under `module/src/` mentions it, `module/CMakeLists.txt` does not list
// it, and the agent module was built, packaged and shipped before it existed.
// The only thing it shares with the module is one header — `ISkill`, in
// `module/src/agent_module_interface.h` — which is the published interface the
// prize's usability criterion asks for.
//
// WHAT IT DELIBERATELY DOES NOT INCLUDE, AND WHY THAT IS THE INTERESTING PART
//
//   - `agent_module_plugin.h`. A skill does not need the module's header; it
//     needs the *skill* header. Depending on the plugin would make every skill
//     recompile when the module's internals move.
//   - `nlohmann/json.hpp`, which the agent module uses throughout. `ISkill`
//     passes `std::string` in and `std::string` out, so the skill and the module
//     need not agree on a JSON library, or link one at all. This file parses its
//     one parameter and builds its answer by hand to keep that claim true rather
//     than merely plausible: it is a demonstration, and a demonstration that
//     quietly relies on the host's dependencies demonstrates less.
//   - Any Logos SDK header, Qt, or the Logos Core module context. The skill is
//     plain C++17 and libc++/libstdc++.
//
// WHAT IT COMPUTES
//
// `notary.digest` — a SHA-256 over a UTF-8 string, which is the primitive under
// the prize's "privacy-preserving notary" use case: the agent commits to a
// document by publishing its digest, and anybody can later check a candidate
// document against that commitment without the agent republishing the document.
// It was chosen over a toy (an echo, a word count) for one reason: the answer is
// independently checkable. `printf %s '<content>' | shasum -a 256` is run by
// `run.sh` against whatever this returns, so the demonstration proves the skill
// *ran* rather than that a string was printed.
//
// SHA-256 is implemented here rather than linked because a skill that pulls in
// OpenSSL would make the build line the story. FIPS 180-4, ~90 lines, and it is
// checked against the system tool on every run.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// The one thing this skill takes from the agent module. Nothing else.
#include "agent_module_interface.h"

namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). No dependency, and checked against `shasum -a 256`.
// ---------------------------------------------------------------------------

const std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

std::string sha256Hex(const std::string &input)
{
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::vector<unsigned char> msg(input.begin(), input.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while (msg.size() % 64u != 56u) {
        msg.push_back(0x00u);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<unsigned char>((bitLength >> (i * 8)) & 0xffu));
    }

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[off + i * 4 + 0]) << 24)
                 | (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8)
                 | (static_cast<std::uint32_t>(msg[off + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(h[i] >> shift) & 0xfu]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Just enough JSON to read one string field and write an object.
//
// Hand-rolled on purpose — see the header comment. It is written to *refuse*
// rather than to guess: an input that is not an object, a `content` that is not
// a string, a truncated escape and an unterminated string all raise, and the
// skill turns that into an error result. Guessing is how a skill comes to
// notarise something other than what it was handed.
// ---------------------------------------------------------------------------

void skipWs(const std::string &s, std::size_t &i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
}

std::string parseJsonString(const std::string &s, std::size_t &i)
{
    if (i >= s.size() || s[i] != '"') {
        throw std::runtime_error("expected a JSON string");
    }
    ++i;
    std::string out;
    while (true) {
        if (i >= s.size()) {
            throw std::runtime_error("unterminated JSON string");
        }
        const char c = s[i++];
        if (c == '"') {
            return out;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= s.size()) {
            throw std::runtime_error("truncated escape in JSON string");
        }
        const char esc = s[i++];
        switch (esc) {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
            // One BMP code point, encoded as UTF-8. Surrogate pairs are refused
            // rather than mangled: half a code point is not a document, and a
            // notary that silently alters its input is worse than one that
            // declines.
            if (i + 4 > s.size()) {
                throw std::runtime_error("truncated \\u escape");
            }
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
                const char d = s[i + k];
                cp <<= 4;
                if (d >= '0' && d <= '9')      cp |= static_cast<unsigned>(d - '0');
                else if (d >= 'a' && d <= 'f') cp |= static_cast<unsigned>(d - 'a' + 10);
                else if (d >= 'A' && d <= 'F') cp |= static_cast<unsigned>(d - 'A' + 10);
                else throw std::runtime_error("bad hex in \\u escape");
            }
            i += 4;
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                throw std::runtime_error("surrogate escapes are not supported by this skill");
            }
            if (cp < 0x80u) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800u) {
                out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            }
            break;
        }
        default:
            throw std::runtime_error("unknown escape in JSON string");
        }
    }
}

/// Skip one JSON value, so an object carrying fields this skill does not read
/// is accepted rather than refused. A skill that insisted on exactly its own
/// parameters would break the moment a caller added a correlation id.
void skipJsonValue(const std::string &s, std::size_t &i)
{
    skipWs(s, i);
    if (i >= s.size()) {
        throw std::runtime_error("unexpected end of JSON");
    }
    if (s[i] == '"') {
        parseJsonString(s, i);
        return;
    }
    if (s[i] == '{' || s[i] == '[') {
        const char open = s[i];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            if (s[i] == '"') {
                parseJsonString(s, i);
                continue;
            }
            if (s[i] == open) {
                ++depth;
            } else if (s[i] == close) {
                --depth;
                if (depth == 0) {
                    ++i;
                    return;
                }
            }
            ++i;
        }
        throw std::runtime_error("unterminated JSON container");
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') {
        ++i;
    }
}

/// `true` and the value if `key` is present as a string; `false` if the object
/// simply does not carry it. Anything malformed raises.
bool jsonStringField(const std::string &doc, const std::string &key, std::string &out)
{
    std::size_t i = 0;
    skipWs(doc, i);
    if (i >= doc.size() || doc[i] != '{') {
        throw std::runtime_error("parameters must be a JSON object");
    }
    ++i;
    skipWs(doc, i);
    if (i < doc.size() && doc[i] == '}') {
        return false;
    }
    while (true) {
        skipWs(doc, i);
        const std::string name = parseJsonString(doc, i);
        skipWs(doc, i);
        if (i >= doc.size() || doc[i] != ':') {
            throw std::runtime_error("expected ':' after a key");
        }
        ++i;
        skipWs(doc, i);
        if (name == key) {
            if (i >= doc.size() || doc[i] != '"') {
                throw std::runtime_error("'" + key + "' must be a string");
            }
            out = parseJsonString(doc, i);
            return true;
        }
        skipJsonValue(doc, i);
        skipWs(doc, i);
        if (i < doc.size() && doc[i] == ',') {
            ++i;
            continue;
        }
        if (i < doc.size() && doc[i] == '}') {
            return false;
        }
        throw std::runtime_error("expected ',' or '}'");
    }
}

std::string jsonEscape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (const unsigned char c : in) {
        switch (c) {
        case '"':  out += "\\\"";  break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20u) {
                static const char *kHex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0xfu]);
                out.push_back(kHex[c & 0xfu]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The skill.
// ---------------------------------------------------------------------------

class NotaryDigestSkill final : public logos::agent::ISkill {
public:
    std::string name() const override { return "notary.digest"; }

    /// A JSON Schema object, because that is what the agent's A2A Agent Card
    /// publishes for each skill — so another agent can call this one without
    /// out-of-band knowledge. The module validates that this parses as an
    /// object and reports the skill's entry as an error if it does not; it does
    /// not otherwise interpret it.
    std::string parameterSchema() const override
    {
        return R"({"type":"object",)"
               R"("properties":{"content":{"type":"string",)"
               R"("description":"UTF-8 text to commit to. Not stored, not transmitted, not logged."}},)"
               R"("required":["content"],)"
               R"("additionalProperties":true})";
    }

    std::string invoke(const std::string &paramsJson) override
    {
        std::string content;
        try {
            if (!jsonStringField(paramsJson, "content", content)) {
                return R"({"ok":false,"error":"notary.digest requires a 'content' string"})";
            }
        } catch (const std::exception &e) {
            return std::string(R"({"ok":false,"error":")") + jsonEscape(e.what()) + R"("})";
        }

        // The digest, and the byte count that was digested. The content itself
        // is deliberately not echoed: this skill is a commitment, and a notary
        // that repeats the document back has published it.
        return std::string(R"({"ok":true,"skill":"notary.digest","algorithm":"sha256","digest":")")
             + sha256Hex(content) + R"(","bytes":)" + std::to_string(content.size())
             + R"(,"source":"examples/skills/notary-digest/notary_digest_skill.cpp"})";
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The loader entry point.
//
// `extern "C"` and a raw pointer, both on purpose:
//
//   - a C symbol name is the only thing `dlsym` can be asked for portably;
//   - a raw pointer, not a `std::shared_ptr`, so the host chooses the ownership
//     it wants. The host wraps it in a `shared_ptr<ISkill>`, whose deleter calls
//     the virtual destructor — which lands back in *this* library, so the
//     allocation is freed by the allocator that made it.
//
// The host may also link this file directly and construct the class; the entry
// point exists so a skill can be shipped as a binary that the agent module has
// never been compiled against, which is the claim under test.
// ---------------------------------------------------------------------------

extern "C" {

/// ABI generation of this entry point. The host checks it before calling
/// `logos_agent_skill_create`, so a stale library is a refusal naming the
/// mismatch rather than a crash inside a vtable that moved.
int logos_agent_skill_abi_version(void) { return 1; }

/// One skill per library, returned owning. Never null in this implementation;
/// a host must still check, because a third-party library is third-party code.
logos::agent::ISkill *logos_agent_skill_create(void) { return new NotaryDigestSkill(); }

} // extern "C"
