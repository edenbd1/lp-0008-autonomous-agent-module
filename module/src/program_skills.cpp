#include "program_skills.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cstddef>

namespace logos::agent {

using nlohmann::json;

namespace {

// --- SHA-256 -----------------------------------------------------------
//
// Vendored rather than linked. `program.deploy` is only worth anything if it
// recomputes the deployment's content address itself, and a verification step
// that drags in a link-time dependency is a verification step that eventually
// gets compiled out of the test build and then out of the product.

constexpr std::uint32_t kK[64] = {
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

inline std::uint32_t rotr(std::uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

void sha256Block(std::uint32_t h[8], const std::uint8_t *p)
{
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24)
             | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16)
             | (static_cast<std::uint32_t>(p[4 * i + 2]) << 8)
             | static_cast<std::uint32_t>(p[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = k + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

// --- small JSON helpers, same shape as storage_skills.cpp --------------

std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
}

/// A refusal that still carries what was learned before refusing — the hash a
/// transaction was submitted under, say. A bare `fail` would throw that away,
/// and an operator cannot chase a transaction whose hash nobody printed.
std::string failWith(const std::string &why, json extra)
{
    extra["ok"] = false;
    extra["error"] = why;
    return extra.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
}

json parse(const std::string &s, std::string &err)
{
    auto j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "parameters must be a JSON object";
        return json::object();
    }
    return j;
}

bool field(const json &j, const char *key, std::string &out, std::string &err)
{
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        err = std::string("missing or empty '") + key + "'";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

// --- spel's output -----------------------------------------------------

/// The first complete transaction hash `spel` printed, lowercased, or empty.
///
/// Exactly 64 hex digits are required. A shorter run is a truncated line or an
/// unrelated identifier, and accepting it would produce a hash that looks
/// plausible and matches nothing on chain.
std::string firstTxHash(const std::string &out)
{
    static const std::string key = "tx_hash:";
    for (std::size_t at = out.find(key); at != std::string::npos;
         at = out.find(key, at + key.size())) {
        std::size_t i = at + key.size();
        while (i < out.size() && (out[i] == ' ' || out[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < out.size() && std::isxdigit(static_cast<unsigned char>(out[j]))) ++j;
        if (j - i == 64) {
            std::string hex = out.substr(i, 64);
            for (char &c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return hex;
        }
    }
    return {};
}

/// The tail of a command's output, for a refusal message. Whole transcripts do
/// not belong in a JSON error field; the last lines are where the verdict is.
std::string tail(const std::string &out, std::size_t lines = 3)
{
    if (out.empty()) return "(no output)";
    std::size_t at = out.size();
    while (at > 0 && (out[at - 1] == '\n' || out[at - 1] == '\r')) --at;
    std::size_t start = at;
    for (std::size_t n = 0; n < lines && start > 0; ++n) {
        const std::size_t nl = out.rfind('\n', start - 1);
        if (nl == std::string::npos) { start = 0; break; }
        start = nl;
    }
    while (start < at && (out[start] == '\n' || out[start] == '\r')) ++start;
    return out.substr(start, at - start);
}

// --- params -> spel flags ----------------------------------------------

/// Turn `{"policy_hash":"ab…","per_tx":50}` into
/// `--policy-hash ab… --per-tx 50`, the way the IDL's snake_case argument names
/// appear on `spel`'s command line.
///
/// Names are checked rather than trusted. These become argv entries, and a name
/// carrying its own spaces or dashes could introduce a flag the caller never
/// asked for — `--bin-auth-transfer`, for instance, decides which program's ELF
/// is admitted into the proof.
bool flagsFromParams(const json &p, std::vector<std::string> &args, std::string &err)
{
    if (!p.is_object()) {
        err = "'params' must be a JSON object of instruction arguments";
        return false;
    }
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string &name = it.key();
        if (name.empty()) {
            err = "an instruction argument with an empty name";
            return false;
        }
        std::string flag = "--";
        for (const char c : name) {
            if (c == '_' || c == '-') {
                flag += '-';
            } else if (std::isalnum(static_cast<unsigned char>(c))) {
                flag += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else {
                err = "the instruction argument '" + name
                    + "' is not a name spel could take on its command line";
                return false;
            }
        }

        const json &v = it.value();
        std::string value;
        if (v.is_string()) {
            value = v.get<std::string>();
        } else if (v.is_boolean()) {
            value = v.get<bool>() ? "true" : "false";
        } else if (v.is_number()) {
            value = v.dump();
        } else {
            err = "the instruction argument '" + name
                + "' must be a string, number or boolean: spel takes flags, not nested values";
            return false;
        }
        args.push_back(flag);
        args.push_back(value);
    }
    return true;
}

// --- the sequencer -----------------------------------------------------

/// What the chain can say about a transaction hash. `Unknown` is not a third
/// kind of answer from the RPC — it is the RPC not answering, and it is kept
/// separate because "we could not ask" must never be reported as "it is not
/// there".
enum class Presence { Yes, No, Unknown };

Presence txPresence(const SequencerPort &seq, const std::string &hash, std::string &err)
{
    if (!seq.rpc) {
        err = "no sequencer to ask";
        return Presence::Unknown;
    }
    const std::string raw = seq.rpc("getTransaction", json::array({hash}).dump());
    if (raw.empty()) {
        err = "the sequencer did not answer";
        return Presence::Unknown;
    }
    auto j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "the sequencer returned a body that did not parse";
        return Presence::Unknown;
    }
    if (j.contains("error") && !j["error"].is_null()) {
        err = "the sequencer refused getTransaction";
        if (j["error"].is_object() && j["error"].contains("message")
            && j["error"]["message"].is_string()) {
            err += ": " + j["error"]["message"].get<std::string>();
        }
        return Presence::Unknown;
    }
    if (!j.contains("result")) {
        err = "the sequencer answered without a result";
        return Presence::Unknown;
    }
    return j["result"].is_null() ? Presence::No : Presence::Yes;
}

/// Why absence proves nothing on this chain. Repeated verbatim wherever a
/// transaction is missing, because the temptation to shorten it to "failed" is
/// exactly the mistake it exists to prevent.
constexpr const char *kAbsenceNote =
    "not included: this RPC answers with the same null whether a transaction was "
    "dropped, is still pending, was refused, or was never submitted, so absence "
    "here is not a verdict";

} // namespace

// --- exported helpers --------------------------------------------------

std::string sha256Hex(const std::vector<std::uint8_t> &data)
{
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    const std::size_t full = data.size() / 64;
    for (std::size_t i = 0; i < full; ++i) sha256Block(h, data.data() + i * 64);

    // The tail, its 0x80 terminator and the 64-bit big-endian bit length. Two
    // blocks are needed when the remainder leaves no room for the length.
    std::array<std::uint8_t, 128> last{};
    const std::size_t rest = data.size() - full * 64;
    for (std::size_t i = 0; i < rest; ++i) last[i] = data[full * 64 + i];
    last[rest] = 0x80;
    const std::size_t padded = (rest < 56) ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8u;
    for (std::size_t i = 0; i < 8; ++i) {
        last[padded - 1 - i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu);
    }
    for (std::size_t i = 0; i < padded; i += 64) sha256Block(h, last.data() + i);

    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::size_t i = 0; i < 8; ++i) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out += hex[(h[i] >> shift) & 0xfu];
        }
    }
    return out;
}

std::string deployTxHash(const std::vector<std::uint8_t> &bytecode)
{
    // The borsh encoding of a byte vector is its length as a little-endian u32
    // followed by the bytes, and the deployment transaction hashes exactly
    // that. Nothing about the deployer enters it, which is what makes a
    // deployment reproducible by anyone holding the same binary.
    std::vector<std::uint8_t> framed;
    framed.reserve(bytecode.size() + 4);
    const std::uint32_t len = static_cast<std::uint32_t>(bytecode.size());
    framed.push_back(static_cast<std::uint8_t>(len & 0xffu));
    framed.push_back(static_cast<std::uint8_t>((len >> 8) & 0xffu));
    framed.push_back(static_cast<std::uint8_t>((len >> 16) & 0xffu));
    framed.push_back(static_cast<std::uint8_t>((len >> 24) & 0xffu));
    framed.insert(framed.end(), bytecode.begin(), bytecode.end());
    return sha256Hex(framed);
}

bool isReadMethod(const std::string &method)
{
    return method == "getTransaction" || method == "getBlock" || method == "getAccount"
        || method == "getLastBlockId";
}

// --- program.query -----------------------------------------------------

std::string ProgramQuerySkill::parameterSchema() const
{
    return R"({"type":"object","required":["program_id","method"],)"
           R"("properties":{"program_id":{"type":"string"},)"
           R"("method":{"type":"string",)"
           R"("enum":["getTransaction","getBlock","getAccount","getLastBlockId"]},)"
           R"("params":{"type":"array"}}})";
}

std::string ProgramQuerySkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string programId, method;
    if (!field(p, "program_id", programId, err)) return fail(err);
    if (!field(p, "method", method, err)) return fail(err);

    // sendTransaction is a real method of this RPC, and it is the one method a
    // skill called `query` must not reach. Naming it in the refusal is the
    // point: a caller who wanted to submit needs to be sent to the skill that
    // goes through the anchored spending policy, not quietly refused.
    if (method == "sendTransaction") {
        return fail("program.query only reads; sendTransaction submits state — use program.call, "
                    "which goes through the policy the owner anchored");
    }
    if (!isReadMethod(method)) {
        return fail("the sequencer has no '" + method
                    + "' method; it exposes getTransaction, getBlock, getAccount and "
                      "getLastBlockId, and nothing that reports a transaction's status");
    }
    if (!seq_.rpc) return fail("no sequencer to query");

    json rpcParams = json::array();
    if (p.contains("params") && !p["params"].is_null()) {
        if (!p["params"].is_array()) return fail("'params' must be a JSON array");
        rpcParams = p["params"];
    } else if (method == "getTransaction") {
        // A program's on-chain identity is its deployment hash, so asking about
        // the program itself is the useful default — "is this program there".
        rpcParams = json::array({programId});
    } else if (method != "getLastBlockId") {
        return fail("'params' is required for " + method);
    }

    const std::string raw = seq_.rpc(method, rpcParams.dump());
    if (raw.empty()) return fail("the sequencer did not answer");

    auto j = json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return fail("the sequencer returned a body that did not parse");
    }
    if (j.contains("error") && !j["error"].is_null()) {
        std::string why = "the sequencer refused the query";
        if (j["error"].is_object() && j["error"].contains("message")
            && j["error"]["message"].is_string()) {
            why += ": " + j["error"]["message"].get<std::string>();
        }
        return fail(why);
    }
    if (!j.contains("result")) return fail("the sequencer answered without a result");

    const bool present = !j["result"].is_null();
    json out{{"program_id", programId}, {"method", method}, {"found", present}};
    if (method == "getTransaction") {
        // The honest word for a transaction is `included`. The query succeeded
        // either way — what is absent is the transaction, not the answer — so
        // this is a result and not an error, and it says only what the null
        // supports.
        out["included"] = present;
        if (!present) out["note"] = kAbsenceNote;
    } else if (!present) {
        out["note"] = "the sequencer has no record under that key";
    }
    if (present) out["result"] = j["result"];
    return done(out);
}

// --- program.call ------------------------------------------------------

std::string ProgramCallSkill::parameterSchema() const
{
    return R"({"type":"object","required":["program_id","instruction"],)"
           R"("properties":{"program_id":{"type":"string"},)"
           R"("instruction":{"type":"string"},"params":{"type":"object"}}})";
}

std::string ProgramCallSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string programId, instruction;
    if (!field(p, "program_id", programId, err)) return fail(err);
    if (!field(p, "instruction", instruction, err)) return fail(err);

    std::vector<std::string> args;
    if (p.contains("params") && !p["params"].is_null()) {
        if (!flagsFromParams(p["params"], args, err)) return fail(err);
    }
    if (!port_.call) return fail("no spel invocation is wired");

    const ProcessResult r = port_.call(programId, instruction, args);
    const std::string tx = firstTxHash(r.output);

    // The verdict, not the hash, decides. `spel` prints `tx_hash:` the moment it
    // has built and sent a transaction, and prints `Transaction NOT confirmed`
    // and exits non-zero when that transaction never lands. Reading only the
    // hash turns a reported failure into a reported success — which is exactly
    // how three anchors in this repository "succeeded" while landing nothing.
    // The hash is still returned, under a name that does not claim inclusion,
    // because it is what an operator needs to go looking.
    const bool notConfirmed = r.output.find("NOT confirmed") != std::string::npos;
    if (r.exitCode != 0 || notConfirmed) {
        json out{{"program_id", programId},
                 {"instruction", instruction},
                 {"confirmed", false}};
        if (!tx.empty()) out["submitted_tx"] = tx;
        const std::string why =
            notConfirmed
                ? "spel submitted the transaction and reported it was not confirmed; a "
                  "submitted hash is not a landed transaction"
                : "spel failed: " + tail(r.output);
        return failWith(why, out);
    }
    if (tx.empty()) {
        return fail("spel exited cleanly but printed no transaction hash, so nothing was "
                    "submitted: " + tail(r.output));
    }

    // No threshold check here, deliberately. The owner's limits are enforced by
    // the anchored policy program, for the reason set out in
    // `agent_module_interface.h`: anything this process can decide, whoever
    // holds this process can decide differently.
    return done(json{{"program_id", programId},
                     {"instruction", instruction},
                     {"tx", tx},
                     {"confirmed", true}});
}

// --- program.deploy ----------------------------------------------------

std::string ProgramDeploySkill::parameterSchema() const
{
    return R"({"type":"object","required":["binary_path"],)"
           R"("properties":{"binary_path":{"type":"string"}}})";
}

std::string ProgramDeploySkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string path;
    if (!field(p, "binary_path", path, err)) return fail(err);
    if (!port_.read) return fail("no way to read the program binary");

    const std::vector<std::uint8_t> bytecode = port_.read(path);
    if (bytecode.empty()) {
        return fail("could not read a program binary at '" + path + "'");
    }

    // Computed before anything is submitted. This is the one on-chain operation
    // whose result is a function of its input alone, so the hash is derived
    // here and the toolchain is checked against it — not asked for it.
    const std::string expected = deployTxHash(bytecode);

    // Refuse when the check cannot be made, not only when it fails. Without a
    // sequencer this skill could still shell out and still return a hash, and
    // that hash would be a claim about a chain nobody consulted.
    if (!seq_.rpc) {
        return failWith("no sequencer to verify the deployment against",
                        json{{"expected_tx", expected}, {"binary_path", path}});
    }

    Presence before = txPresence(seq_, expected, err);
    if (before == Presence::Unknown) {
        return failWith("the deployment cannot be verified: " + err,
                        json{{"expected_tx", expected}, {"binary_path", path}});
    }
    if (before == Presence::Yes) {
        // Content addressing makes redeployment a no-op rather than a
        // duplicate: an identical binary hashes to the same transaction, which
        // is already there.
        return done(json{{"program_id", expected},
                         {"tx", expected},
                         {"binary_path", path},
                         {"already_deployed", true},
                         {"confirmed", true}});
    }

    if (!port_.deploy) return fail("no way to submit the deployment");
    const ProcessResult r = port_.deploy(path);
    if (r.exitCode != 0) {
        return failWith("the wallet refused the deployment: " + tail(r.output),
                        json{{"expected_tx", expected}, {"binary_path", path}});
    }

    // If the toolchain named a hash at all, it has to be this one. A different
    // hash means the bytes that went to the chain are not the bytes that were
    // read from disk, and reporting the computed hash anyway would produce a
    // deployment record that verifies against a binary nobody deployed.
    const std::string reported = firstTxHash(r.output);
    if (!reported.empty() && reported != expected) {
        return failWith(
            "the wallet reported a different transaction than this binary's content address, "
            "so what was deployed is not what was read",
            json{{"expected_tx", expected}, {"reported_tx", reported}, {"binary_path", path}});
    }

    const Presence after = txPresence(seq_, expected, err);
    if (after == Presence::Unknown) {
        return failWith("submitted, but the deployment cannot be verified: " + err,
                        json{{"submitted_tx", expected}, {"binary_path", path}});
    }
    if (after == Presence::No) {
        // Deliberately not an error about the program: blocks are a minute
        // apart and this skill does not sleep. The caller polls with
        // `program.query`, and what is reported meanwhile is absence, not
        // failure.
        return failWith(std::string("submitted, but ") + kAbsenceNote,
                        json{{"submitted_tx", expected},
                             {"binary_path", path},
                             {"included", false},
                             {"confirmed", false}});
    }
    return done(json{{"program_id", expected},
                     {"tx", expected},
                     {"binary_path", path},
                     {"already_deployed", false},
                     {"confirmed", true}});
}

} // namespace logos::agent
