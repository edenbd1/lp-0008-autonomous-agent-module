#include "wallet_skills.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace logos::agent {

using nlohmann::json;

namespace {

constexpr std::size_t kDefaultHistoryLimit = 20;
constexpr std::size_t kMaxHistoryLimit = 1000;

constexpr unsigned __int128 kU128Max = ~static_cast<unsigned __int128>(0);

/// Every skill answers in the same shape, so a caller — including another agent
/// over A2A — can branch without knowing which skill it called. `extra` carries
/// the fields that matter on a refusal too, above all `submitted`: a caller must
/// be able to tell "we did not pay" from "we could not tell whether we paid".
std::string fail(const std::string &why, json extra = json::object())
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

/// Parse and require a field, rather than letting nlohmann throw out of invoke()
/// and take the module down with it.
bool field(const json &j, const char *key, std::string &out, std::string &err)
{
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        err = std::string("missing or empty '") + key + "'";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

std::string u128ToString(unsigned __int128 v)
{
    if (v == 0) return "0";
    std::string out;
    while (v > 0) {
        out.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// Saturating on purpose, and for the same reason `agent-policy-core` saturates:
/// a hostile or merely stale `spentThisPeriod` must not wrap the period total
/// around into "plenty of room left".
unsigned __int128 saturatingAdd(unsigned __int128 a, unsigned __int128 b)
{
    return a > kU128Max - b ? kU128Max : a + b;
}

/// Amounts are u128 on chain. A JSON number is exact only to 2^64 here — past
/// that nlohmann parses a double — so a decimal string is the wide form, and a
/// float is refused outright rather than rounded into a payment nobody asked
/// for.
bool parseAmount(const json &v, unsigned __int128 &out, std::string &err)
{
    if (v.is_number_float()) {
        err = "'amount' must be a whole number of base units, not a fraction";
        return false;
    }
    if (v.is_number_integer() && !v.is_number_unsigned()) {
        err = "'amount' must be greater than zero";
        return false;
    }
    if (v.is_number_unsigned()) {
        const std::uint64_t n = v.get<std::uint64_t>();
        if (n == 0) {
            err = "'amount' must be greater than zero";
            return false;
        }
        out = n;
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s.empty()) {
            err = "'amount' must be greater than zero";
            return false;
        }
        unsigned __int128 acc = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') {
                err = "'amount' as a string must be decimal digits only";
                return false;
            }
            const unsigned digit = static_cast<unsigned>(c - '0');
            if (acc > (kU128Max - digit) / 10) {
                err = "'amount' does not fit in 128 bits";
                return false;
            }
            acc = acc * 10 + digit;
        }
        if (acc == 0) {
            err = "'amount' must be greater than zero";
            return false;
        }
        out = acc;
        return true;
    }
    err = "'amount' must be a number, or a decimal string for values above 2^64";
    return false;
}

/// Account ids are base58 — the alphabet with 0, O, I and l removed, so that a
/// mistyped id fails here rather than resolving to some other account. The
/// scripts that drive the real chain match `[1-9A-HJ-NP-Za-km-z]{32,}`.
bool isBase58Account(const std::string &s)
{
    static const std::string alphabet =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (s.size() < 32 || s.size() > 64) return false;
    for (const char c : s) {
        if (alphabet.find(c) == std::string::npos) return false;
    }
    return true;
}

/// 32 bytes of hex, which is what the wallet prints as `tx_hash:` and what
/// `getTransaction` takes.
bool isTxHash(const std::string &s)
{
    if (s.size() != 64) return false;
    for (const char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

struct AccountRef {
    /// "", "Public" or "Private".
    std::string scope;
    std::string id;
};

AccountRef splitAccount(const std::string &s)
{
    const auto slash = s.find('/');
    if (slash == std::string::npos) return AccountRef{std::string(), s};
    return AccountRef{s.substr(0, slash), s.substr(slash + 1)};
}

/// Unwrap a JSON-RPC envelope. Accepts the bare result too, so a port that
/// already unwrapped is not punished for it.
bool rpcResult(const std::string &raw, json &out, std::string &err)
{
    if (raw.empty()) {
        err = "the sequencer did not answer";
        return false;
    }
    auto j = json::parse(raw, nullptr, false);
    if (j.is_discarded()) {
        err = "the sequencer's answer did not parse as JSON";
        return false;
    }
    if (j.is_object() && j.contains("error") && !j["error"].is_null()) {
        std::string message = "the sequencer refused the call";
        if (j["error"].is_object() && j["error"].contains("message") &&
            j["error"]["message"].is_string()) {
            message += ": " + j["error"]["message"].get<std::string>();
        }
        err = message;
        return false;
    }
    out = (j.is_object() && j.contains("result")) ? j["result"] : j;
    if (out.is_null()) {
        err = "the sequencer has no record of it";
        return false;
    }
    return true;
}

/// An account record that carries nothing at all.
///
/// `getAccount` on an id the chain has never seen does not error — it answers
/// with the default account. So does a *shielded* id, whose real state is a
/// commitment the RPC cannot open. Reporting either as "balance: 0" would be a
/// confident lie, and it is exactly what a stub returns.
///
/// All three fields are required to be empty, deliberately. Zero `program_owner`
/// alone means only "no program has claimed this account yet", which is true of
/// a funded public account that has never been through `auth-transfer init` —
/// refusing that one would be its own kind of wrong.
bool looksLikeAnUntouchedAccount(const json &account)
{
    const bool zeroNonce = !account.contains("nonce") ||
                           (account["nonce"].is_number() && account["nonce"] == 0);
    const bool zeroBalance = account.contains("balance") && account["balance"].is_number() &&
                             account["balance"] == 0;
    bool defaultOwner = false;
    if (account.contains("program_owner")) {
        const auto &owner = account["program_owner"];
        if (owner.is_array()) {
            defaultOwner = true;
            for (const auto &byte : owner) {
                if (!byte.is_number() || byte != 0) {
                    defaultOwner = false;
                    break;
                }
            }
        } else if (owner.is_string()) {
            // Either hex zeros or base58's zero digit, depending on the encoding.
            const std::string s = owner.get<std::string>();
            defaultOwner = !s.empty() &&
                           (s.find_first_not_of('0') == std::string::npos ||
                            s.find_first_not_of('1') == std::string::npos);
        }
    }
    return defaultOwner && zeroNonce && zeroBalance;
}

} // namespace

// ---------------------------------------------------------------- wallet.balance

std::string WalletBalanceSkill::parameterSchema() const
{
    return R"({"type":"object","properties":{"account":{"type":"string",)"
           R"("description":"account id, optionally prefixed Public/ or Private/;)"
           R"( defaults to the agent's own"}}})";
}

std::string WalletBalanceSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    // `wallet.balance()` takes no parameters, so an empty payload is normal.
    const json p = paramsJson.empty() ? json::object() : parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string account = agentAccount_;
    if (p.contains("account") && !p["account"].is_null()) {
        if (!field(p, "account", account, err)) return fail(err);
    }
    if (account.empty()) {
        return fail("no account to read: the agent has none configured and none was given");
    }

    const AccountRef ref = splitAccount(account);
    if (!ref.scope.empty() && ref.scope != "Public" && ref.scope != "Private") {
        return fail("an account id is bare base58, or prefixed 'Public/' or 'Private/'");
    }
    if (!isBase58Account(ref.id)) {
        return fail("'" + ref.id + "' is not a base58 account id");
    }

    if (ref.scope == "Private") {
        // A shielded balance is not on the RPC's side of the wall. Falling back
        // to `getAccount` here would return the default account and read as a
        // confident zero, which is the single most misleading answer this skill
        // could give.
        if (!port_.walletAccount) {
            return fail("a shielded balance needs the account's viewing key, and no wallet is wired");
        }
        const std::string raw = port_.walletAccount(account);
        if (raw.empty()) return fail("the wallet did not answer for " + account);
        auto j = json::parse(raw, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return fail("the wallet's answer for " + account + " did not parse as JSON");
        }
        if (!j.contains("balance") || !j["balance"].is_number()) {
            return fail("the wallet's answer for " + account + " carries no numeric 'balance'");
        }
        return done(json{{"account", account}, {"balance", j["balance"]}, {"source", "wallet"},
                         {"shielded", true}});
    }

    if (!port_.getAccount) return fail("no sequencer connection is wired");
    // Bare base58 on the wire: `getAccount` takes the id, not the wallet's
    // qualified form, and passing `Public/<id>` through would simply miss.
    json account_state;
    if (!rpcResult(port_.getAccount(ref.id), account_state, err)) {
        return fail(err);
    }
    if (!account_state.is_object()) {
        return fail("the sequencer's account record was not an object");
    }
    if (looksLikeAnUntouchedAccount(account_state)) {
        return fail("the sequencer returns the default account for '" + ref.id +
                    "': nothing has ever been on chain under that id, or it is a "
                    "shielded account whose balance needs its viewing key");
    }
    if (!account_state.contains("balance") || !account_state["balance"].is_number()) {
        return fail("the sequencer's account record carries no numeric 'balance'");
    }
    return done(json{{"account", ref.id},
                     {"balance", account_state["balance"]},
                     {"source", "sequencer.getAccount"},
                     {"shielded", false}});
}

// ------------------------------------------------------------------- wallet.send

std::string WalletSendSkill::parameterSchema() const
{
    return R"({"type":"object","required":["recipient","amount"],)"
           R"("properties":{"recipient":{"type":"string","description":"public LEZ account id"},)"
           R"("amount":{"type":["integer","string"],"description":"base units; a decimal )"
           R"(string above 2^64"}}})";
}

std::string WalletSendSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err, json{{"submitted", false}});

    std::string recipient;
    if (!field(p, "recipient", recipient, err)) return fail(err, json{{"submitted", false}});
    if (!p.contains("amount")) return fail("missing 'amount'", json{{"submitted", false}});

    unsigned __int128 amount = 0;
    if (!parseAmount(p["amount"], amount, err)) return fail(err, json{{"submitted", false}});

    const AccountRef ref = splitAccount(recipient);
    if (ref.scope == "Private") {
        // Not a policy decision — a fact about the wallet. A private account can
        // only be addressed by a wallet holding its keys, so naming another
        // agent's shielded account fails with KeyNotFoundError before anything
        // is built. Agents publish a public receiving account in their Agent
        // Card for exactly this reason.
        return fail("a shielded account cannot be paid by id: pay the recipient's public "
                    "account, which is what its Agent Card advertises",
                    json{{"submitted", false}});
    }
    if (!ref.scope.empty() && ref.scope != "Public") {
        return fail("'recipient' is a base58 account id, optionally prefixed 'Public/'",
                    json{{"submitted", false}});
    }
    if (!isBase58Account(ref.id)) {
        return fail("'" + ref.id + "' is not a base58 account id", json{{"submitted", false}});
    }
    const std::string qualified = "Public/" + ref.id;

    // THE LOCAL CHECK IS A COURTESY.
    //
    // The envelope is enforced by the anchored policy account, whose address is
    // derived from these very limits; this process cannot raise them, and cannot
    // usefully lower them either. What the comparison buys is that an
    // over-envelope spend is routed to the owner *before* the agent pays to
    // prove a transaction the chain will reject. It mirrors
    // `SpendPolicy::is_autonomous` in `agent-policy-core`, saturation included,
    // so the two answers agree.
    //
    // An unknown period total is not zero. If the agent cannot tell how much it
    // has already moved, it cannot claim a spend is inside the envelope, so the
    // spend goes to the owner. Assuming zero here is exactly how a threshold
    // becomes decorative.
    const bool periodKnown = static_cast<bool>(port_.spentThisPeriod);
    const unsigned __int128 spent = periodKnown ? port_.spentThisPeriod() : 0;

    std::string held;
    if (!periodKnown) {
        held = "the period total is unknown, so this spend cannot be shown to be inside the envelope";
    } else if (amount > policy_.perTx) {
        held = "over the per-transaction limit of " + u128ToString(policy_.perTx);
    } else if (saturatingAdd(spent, amount) > policy_.perPeriod) {
        held = "over the per-period limit of " + u128ToString(policy_.perPeriod) + ", of which " +
               u128ToString(spent) + " is already spent";
    }

    if (!held.empty()) {
        const json base{{"submitted", false},
                        {"recipient", qualified},
                        {"amount", u128ToString(amount)},
                        {"reason", held}};
        if (!owner_.requestApproval) {
            json e = base;
            e["outcome"] = "owner_unreachable";
            return fail("no owner channel to ask for approval, so the spend was not submitted", e);
        }
        if (!owner_.nextNonce) {
            json e = base;
            e["outcome"] = "owner_unreachable";
            return fail("no approval nonce source: an approval that does not name one specific "
                        "spend could be replayed for the next identical one", e);
        }
        const std::uint64_t nonce = owner_.nextNonce();
        const json request{{"kind", "spend-approval-request"},
                           {"recipient", qualified},
                           {"amount", u128ToString(amount)},
                           {"nonce", nonce},
                           {"perTx", u128ToString(policy_.perTx)},
                           {"perPeriod", u128ToString(policy_.perPeriod)},
                           {"reason", held}};
        if (!owner_.requestApproval(request.dump())) {
            // Terminal, not a fallback. An above-threshold spend that fails to
            // reach the owner must not execute.
            json e = base;
            e["outcome"] = "owner_unreachable";
            return fail("the owner could not be reached, so the spend was not submitted", e);
        }
        json out = base;
        out["outcome"] = "awaiting_owner_approval";
        out["nonce"] = nonce;
        return done(out);
    }

    if (!port_.spend) {
        return fail("no policy path is wired, and there is no other way to move funds here",
                    json{{"submitted", false}});
    }
    SpendRequest request;
    request.recipient = qualified;
    request.amount = amount;
    request.spentThisPeriod = spent;
    const SpendReceipt receipt = port_.spend(request);

    if (!receipt.submitted) {
        return fail(receipt.error.empty() ? "the policy program refused the spend" : receipt.error,
                    json{{"submitted", false}, {"outcome", "refused"}});
    }
    // A success flag with no transaction hash is not a payment. Earlier
    // instructions in this repository produced confirmed on-chain proofs that a
    // policy *permitted* an amount while moving nothing, and were nearly written
    // up as settlements; the hash is the least this can insist on.
    if (!isTxHash(receipt.txHash)) {
        return fail("the policy path reported success without a transaction hash",
                    json{{"submitted", false}, {"outcome", "refused"}});
    }
    return done(json{{"submitted", true},
                     {"outcome", "autonomous"},
                     {"tx", receipt.txHash},
                     {"recipient", qualified},
                     {"amount", u128ToString(amount)}});
}

// ---------------------------------------------------------------- wallet.history

std::string WalletHistorySkill::parameterSchema() const
{
    return R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,)"
           R"("maximum":1000,"description":"how many of the most recent entries"}}})";
}

std::string WalletHistorySkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = paramsJson.empty() ? json::object() : parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::size_t limit = kDefaultHistoryLimit;
    if (p.contains("limit") && !p["limit"].is_null()) {
        if (!p["limit"].is_number_unsigned()) {
            return fail("'limit' must be a positive whole number");
        }
        const std::uint64_t n = p["limit"].get<std::uint64_t>();
        if (n == 0 || n > kMaxHistoryLimit) {
            return fail("'limit' must be between 1 and " + std::to_string(kMaxHistoryLimit));
        }
        limit = static_cast<std::size_t>(n);
    }

    // The honest part. There is no per-account history method on the sequencer,
    // so this is not a chain query dressed up as one: it is the agent's own
    // record of what it submitted, checked against the chain one hash at a time.
    if (!port_.journal) {
        return fail("no submission journal is wired, and the sequencer has no per-account "
                    "history endpoint to fall back on");
    }
    auto entries = json::parse(port_.journal(), nullptr, false);
    if (entries.is_discarded() || !entries.is_array()) {
        return fail("the agent's submission journal did not parse as a JSON array");
    }

    // Newest first, which means the tail of an append-ordered journal, reversed.
    std::vector<json> picked;
    for (auto it = entries.rbegin(); it != entries.rend() && picked.size() < limit; ++it) {
        picked.push_back(*it);
    }

    bool confirmedAgainstChain = static_cast<bool>(port_.getTransaction);
    json out = json::array();
    for (const json &entry : picked) {
        if (!entry.is_object()) {
            return fail("every journal entry must be a JSON object");
        }
        std::string tx;
        if (!field(entry, "tx", tx, err) || !isTxHash(tx)) {
            return fail("a journal entry carries no 32-byte hex transaction hash, so it cannot "
                        "be confirmed and will not be reported as history");
        }
        json row = entry;
        if (!port_.getTransaction) {
            row["status"] = "unverified";
            row["statusDetail"] = "no sequencer connection is wired";
        } else {
            json result;
            std::string rpcErr;
            const std::string raw = port_.getTransaction(tx);
            if (raw.empty()) {
                // The node being down says nothing about the transaction. Not
                // knowing is its own answer, and the summary carries it upwards
                // so a caller cannot mistake this list for a confirmed one.
                row["status"] = "unknown";
                row["statusDetail"] = "the sequencer did not answer";
                confirmedAgainstChain = false;
            } else if (!rpcResult(raw, result, rpcErr)) {
                // A hash with no record is pending, dropped or never submitted,
                // and the RPC cannot tell those apart — so neither will this.
                const bool noRecord = rpcErr == "the sequencer has no record of it";
                row["status"] = noRecord ? "not-yet-included" : "unknown";
                row["statusDetail"] = rpcErr;
                if (!noRecord) confirmedAgainstChain = false;
            } else if (result.is_array() && result.empty()) {
                row["status"] = "not-yet-included";
            } else {
                row["status"] = "confirmed";
            }
        }
        out.push_back(row);
    }

    return done(json{{"transactions", out},
                     {"count", out.size()},
                     {"complete", false},
                     {"confirmedAgainstChain", confirmedAgainstChain},
                     {"source", "agent submission journal, confirmed via getTransaction"},
                     {"note", "the sequencer exposes no per-account history: this lists only "
                              "what this agent submitted itself. Payments received, and "
                              "anything sent by another instance of the agent, are not here"}});
}

} // namespace logos::agent
