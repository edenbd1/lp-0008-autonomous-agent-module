#include "wallet_skills.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace logos::agent {

using nlohmann::json;

namespace {

constexpr std::size_t kDefaultHistoryLimit = 20;
constexpr std::size_t kMaxHistoryLimit = 1000;

/// The largest amount the chain can hold, as digits: 2^128 - 1. Amounts are
/// checked against it here so an unrepresentable one is refused before it is
/// submitted, rather than truncated somewhere downstream.
constexpr const char *kU128Max = "340282366920938463463374607431768211455";

/// Every skill answers in the same shape, so a caller — including another agent
/// over A2A — can branch without knowing which skill it called. `extra` carries
/// the fields that matter on a refusal too, above all `submitted`: a caller must
/// be able to tell "we did not pay" from "we cannot tell whether we paid".
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

// ---- decimal amounts ------------------------------------------------------
//
// One comparison and one addition, done on the digits. There is no portable
// u128 in C++17 and a double would round a payment; digits are exact, and a
// period total that cannot overflow cannot wrap around into "plenty of room
// left" either — which is the failure `agent-policy-core` guards against with a
// saturating add on the Rust side.

bool isDecimal(const std::string &s)
{
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string normalise(const std::string &s)
{
    const auto first = s.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : s.substr(first);
}

/// -1, 0 or 1. Both arguments must already be decimal.
int compareDecimal(const std::string &a, const std::string &b)
{
    const std::string x = normalise(a), y = normalise(b);
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    if (x == y) return 0;
    return x < y ? -1 : 1;
}

std::string addDecimal(const std::string &a, const std::string &b)
{
    const std::string x = normalise(a), y = normalise(b);
    std::string out;
    int carry = 0;
    for (std::size_t i = 0; i < x.size() || i < y.size() || carry != 0; ++i) {
        const int dx = i < x.size() ? x[x.size() - 1 - i] - '0' : 0;
        const int dy = i < y.size() ? y[y.size() - 1 - i] - '0' : 0;
        const int sum = dx + dy + carry;
        out.push_back(static_cast<char>('0' + sum % 10));
        carry = sum / 10;
    }
    std::reverse(out.begin(), out.end());
    return normalise(out);
}

/// A JSON amount, as digits. A JSON number is exact only to 2^64 here — past
/// that nlohmann parses a double — so the wide form is a decimal string, and a
/// fraction is refused outright rather than rounded into a payment nobody asked
/// for.
bool parseAmount(const json &v, std::string &out, std::string &err)
{
    std::string digits;
    if (v.is_number_float()) {
        err = "'amount' must be a whole number of base units, not a fraction";
        return false;
    } else if (v.is_number_integer() && !v.is_number_unsigned()) {
        err = "'amount' must be greater than zero";
        return false;
    } else if (v.is_number_unsigned()) {
        digits = std::to_string(v.get<std::uint64_t>());
    } else if (v.is_string()) {
        digits = v.get<std::string>();
        if (!isDecimal(digits)) {
            err = "'amount' as a string must be decimal digits only";
            return false;
        }
    } else {
        err = "'amount' must be a number, or a decimal string for values above 2^64";
        return false;
    }
    digits = normalise(digits);
    if (digits == "0") {
        err = "'amount' must be greater than zero";
        return false;
    }
    if (compareDecimal(digits, kU128Max) > 0) {
        err = "'amount' is larger than the chain's u128 amount type";
        return false;
    }
    out = digits;
    return true;
}

// ---- account ids ----------------------------------------------------------

/// Account ids are base58 — the alphabet with 0, O, I and l removed, so a
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

/// A shielded recipient named by its published keys: `<npk>:<vpk>`, lower-case
/// hex, 32 and 1184 bytes.
///
/// This is the form an Agent Card carries in `x-logos.shieldedPaymentKeys`, and
/// the form `spel --recipient PrivateKeys/…` takes. Both halves are *public*
/// keys — a nullifier public key and an ML-KEM-768 encapsulation key — so
/// nothing secret passes through here; they let a payer mint a note only the
/// payee can open, and neither of them can spend one.
///
/// The lengths are checked rather than trusted because a truncated `vpk` does
/// not fail at the payer: it derives a different account id, which is a real
/// account under keys nobody holds, and the payment lands somewhere it can
/// never be spent from.
bool isShieldedPaymentKeys(const std::string &s, std::string &err)
{
    const auto colon = s.find(':');
    if (colon == std::string::npos) {
        err = "shielded payment keys are '<npk-hex>:<vpk-hex>', from the payee's Agent Card";
        return false;
    }
    const std::string npk = s.substr(0, colon);
    const std::string vpk = s.substr(colon + 1);
    const auto hex = [](const std::string &v) {
        for (const char c : v) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!ok) return false;
        }
        return !v.empty();
    };
    if (npk.size() != 64 || !hex(npk)) {
        err = "npk must be 32 bytes of lower-case hex";
        return false;
    }
    if (vpk.size() != 2368 || !hex(vpk)) {
        err = "vpk must be a 1184-byte ML-KEM-768 encapsulation key in lower-case hex";
        return false;
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

/// Unwrap a JSON-RPC envelope. The bare result is accepted too, so a port that
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
/// All three fields must be empty, deliberately. A zero `program_owner` alone
/// means only "no program has claimed this account yet", which is true of a
/// funded public account that has never been through `auth-transfer init` —
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
            // Hex zeros, or base58's zero digit, depending on the encoding.
            const std::string s = owner.get<std::string>();
            defaultOwner = !s.empty() && (s.find_first_not_of('0') == std::string::npos ||
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
        // A shielded balance is on the other side of the wall. Falling back to
        // `getAccount` here would return the default account and read as a
        // confident zero, which is the most misleading answer this skill could
        // give.
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
        return done(json{{"account", account},
                         {"balance", j["balance"]},
                         {"source", "wallet"},
                         {"shielded", true}});
    }

    if (!port_.getAccount) return fail("no sequencer connection is wired");
    // Bare base58 on the wire: `getAccount` takes the id, not the wallet's
    // qualified form, and passing `Public/<id>` through would simply miss.
    json state;
    if (!rpcResult(port_.getAccount(ref.id), state, err)) return fail(err);
    if (!state.is_object()) {
        return fail("the sequencer's account record was not an object");
    }
    if (looksLikeAnUntouchedAccount(state)) {
        return fail("the sequencer returns the default account for '" + ref.id +
                    "': nothing has ever been on chain under that id, or it is a shielded "
                    "account whose balance needs its viewing key");
    }
    if (!state.contains("balance") || !state["balance"].is_number()) {
        return fail("the sequencer's account record carries no numeric 'balance'");
    }
    return done(json{{"account", ref.id},
                     {"balance", state["balance"]},
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

    std::string amount;
    if (!parseAmount(p["amount"], amount, err)) return fail(err, json{{"submitted", false}});

    const AccountRef ref = splitAccount(recipient);
    std::string qualified;
    if (ref.scope == "PrivateKeys") {
        // A shielded payee, named by the keys its Agent Card publishes. The
        // note is minted at hash(npk, vpk, identifier) for an identifier the
        // payer draws, so there is no account id to name and none to check
        // against — which is why this form exists at all.
        std::string why;
        if (!isShieldedPaymentKeys(ref.id, why)) {
            return fail(why, json{{"submitted", false}});
        }
        qualified = recipient;
    } else if (ref.scope == "Private") {
        // Refused, and not for the reason this used to give. An id is genuinely
        // not enough to pay a shielded account — `Private/<id>` makes the
        // wallet look the id up in its own key chain and fail with
        // KeyNotFoundError, because that spelling means "an account I can
        // spend". But that is a fact about the spelling, not about shielded
        // payees: name the same agent by the keys it publishes and the payment
        // goes through.
        return fail("a shielded account cannot be paid by id — 'Private/<id>' means an account "
                    "this wallet can spend. Use 'PrivateKeys/<npk>:<vpk>' from the payee's "
                    "Agent Card, or pay its public account",
                    json{{"submitted", false}});
    } else {
        if (!ref.scope.empty() && ref.scope != "Public") {
            return fail("'recipient' is a base58 account id, optionally prefixed 'Public/', "
                        "or 'PrivateKeys/<npk>:<vpk>' for a shielded payee",
                        json{{"submitted", false}});
        }
        if (!isBase58Account(ref.id)) {
            return fail("'" + ref.id + "' is not a base58 account id", json{{"submitted", false}});
        }
        qualified = "Public/" + ref.id;
    }

    // THE LOCAL CHECK IS A COURTESY.
    //
    // The envelope is enforced by the anchored policy account, whose address is
    // derived from these very limits; this process cannot raise them, and cannot
    // usefully lower them either. What the comparison buys is that an
    // over-envelope spend goes to the owner *before* the agent pays to prove a
    // transaction the chain will reject. It mirrors `SpendPolicy::is_autonomous`
    // in `agent-policy-core` so that the two answers agree.
    //
    // Everything unknown is treated as outside the envelope. An agent that
    // cannot say how much it has already moved, or what its limits are, cannot
    // say a spend is inside them — and assuming zero is precisely how a
    // threshold becomes decorative.
    std::string spent;
    if (port_.spentThisPeriod) spent = port_.spentThisPeriod();

    std::string held;
    if (spent.empty() || !isDecimal(spent)) {
        held = "the period total is unknown, so this spend cannot be shown to be inside the envelope";
    } else if (!isDecimal(envelope_.perTx) || !isDecimal(envelope_.perPeriod)) {
        held = "the anchored envelope is not known to this process, so nothing can be shown to fall inside it";
    } else if (compareDecimal(amount, envelope_.perTx) > 0) {
        held = "over the per-transaction limit of " + normalise(envelope_.perTx);
    } else if (compareDecimal(addDecimal(spent, amount), envelope_.perPeriod) > 0) {
        held = "over the per-period limit of " + normalise(envelope_.perPeriod) + ", of which " +
               normalise(spent) + " is already spent";
    }

    if (!held.empty()) {
        const json base{{"submitted", false},
                        {"recipient", qualified},
                        {"amount", amount},
                        {"reason", held}};
        if (!owner_.requestApproval) {
            json e = base;
            e["outcome"] = "owner_unreachable";
            // Zero, and stated rather than left out: "we notified nobody" and
            // "we notified and were not answered" are the two halves of this
            // criterion and a caller has to be able to tell them apart.
            e["attempts"] = 0;
            e["delivered"] = 0;
            return fail("no owner channel to ask for approval, so the spend was not submitted", e);
        }
        if (!owner_.nextNonce) {
            json e = base;
            e["outcome"] = "owner_unreachable";
            e["attempts"] = 0;
            e["delivered"] = 0;
            return fail("no approval nonce source: an approval that does not name one specific "
                        "spend could be replayed for the next identical one",
                        e);
        }
        const std::uint64_t nonce = owner_.nextNonce();
        // The correlation id names *these terms*. It is derived from the nonce
        // rather than minted separately because the nonce is already what makes
        // one spend distinct from an identical later one: two ids for one
        // payment would let the answer to the first settle the second, which is
        // the trap `OwnerChannel::asked_` exists to close.
        const std::string requestId = "spend-" + std::to_string(nonce);
        const json request{{"kind", "spend-approval-request"},
                           {"id", requestId},
                           {"recipient", qualified},
                           {"amount", amount},
                           {"nonce", nonce},
                           {"perTx", envelope_.perTx},
                           {"perPeriod", envelope_.perPeriod},
                           {"reason", held}};
        const std::string requestJson = request.dump();

        // A deployment with no way to hear an answer, or no clock to measure a
        // deadline with, does not get a retry loop: it would be a wait for a
        // reply that has no route here, ending in a timeout that blames an owner
        // who was never given a way to answer. One attempt, and the reply says
        // plainly that nothing can resolve it. Nothing is submitted either way.
        if (!owner_.pollDecision || !owner_.nowMs) {
            if (!owner_.requestApproval(requestJson)) {
                json e = base;
                e["outcome"] = "owner_unreachable";
                e["request_id"] = requestId;
                e["attempts"] = 1;
                e["delivered"] = 0;
                return fail("the owner could not be reached, so the spend was not submitted", e);
            }
            json out = base;
            out["outcome"] = "awaiting_owner_approval";
            out["nonce"] = nonce;
            out["request_id"] = requestId;
            out["attempts"] = 1;
            out["delivered"] = 1;
            out["answer_path"] = false;
            out["note"] = "the request was delivered, and no answer can reach this process: "
                          "nothing will be submitted for it here";
            return done(out);
        }

        // ---- the wait the prize describes ---------------------------------
        //
        // Retry the *notification* while waiting for the *answer*, and let the
        // deadline end it. Every exit from this loop leaves the spend
        // unsubmitted; the only thing that varies is what the operator is told.
        std::int64_t timeout = owner_.timeoutMs ? owner_.timeoutMs() : kDefaultApprovalTimeoutMs;
        std::int64_t resend =
            owner_.resendIntervalMs ? owner_.resendIntervalMs() : kDefaultApprovalResendMs;
        if (timeout < 0) timeout = 0;
        // Clamped rather than accepted: a resend interval of zero against a
        // clock that has not moved yet re-notifies on every single pass, which
        // is a flood on the owner's channel and not a retry policy.
        if (resend < 1) resend = 1;

        const std::int64_t start = owner_.nowMs();
        std::int64_t lastAttemptAt = start;
        int attempts = 0;
        int delivered = 0;
        int unusable = 0;
        std::int64_t lastClockReading = start;
        int passesAtThatReading = 0;
        bool clockStalled = false;
        std::string verdict;

        for (;;) {
            const std::int64_t now = owner_.nowMs();
            if (attempts == 0 || now - lastAttemptAt >= resend) {
                lastAttemptAt = now;
                ++attempts;
                if (owner_.requestApproval(requestJson)) {
                    ++delivered;
                }
            }

            // Asked by id, so an answer to some other spend cannot settle this
            // one. Anything that is neither verdict is counted and ignored
            // rather than treated as a "no": a garbled answer is not a denial,
            // and it is certainly not an approval.
            const std::string answer = owner_.pollDecision(requestId);
            if (answer == "approved" || answer == "denied") {
                verdict = answer;
                break;
            }
            if (!answer.empty()) {
                ++unusable;
            }

            const std::int64_t afterPoll = owner_.nowMs();
            if (afterPoll - start >= timeout) {
                break;
            }
            if (afterPoll == lastClockReading) {
                if (++passesAtThatReading >= kMaxApprovalPollsAtOneInstant) {
                    clockStalled = true;
                    break;
                }
            } else {
                lastClockReading = afterPoll;
                passesAtThatReading = 0;
            }
            if (owner_.idle) {
                owner_.idle();
            }
        }

        json e = base;
        e["request_id"] = requestId;
        e["nonce"] = nonce;
        e["attempts"] = attempts;
        e["delivered"] = delivered;
        e["waited_ms"] = owner_.nowMs() - start;
        e["answer_path"] = true;
        if (unusable > 0) {
            // Reported on every outcome, including an approval that arrived
            // afterwards: an operator whose owner app is answering with
            // something this agent cannot read needs to know that.
            e["unusable_answers"] = unusable;
        }

        if (verdict == "denied") {
            // `ok:false` because the *spend* did not happen, which is what this
            // skill is reporting on. `ApprovalDecision::json()` in
            // owner_channel.h deliberately does the opposite for the same event,
            // because it reports on the *exchange*, and an exchange that ended
            // in a clear "no" succeeded.
            e["outcome"] = "denied";
            return fail("the owner denied this spend, so it was not submitted", e);
        }
        if (verdict == "approved") {
            // An approval unlocks the policy program's `spend_approved` path,
            // which needs an approval account only the owner's own signature can
            // create. This module does not wire that path — `WalletPort::spend`
            // is the autonomous one, and putting an above-envelope amount
            // through it would build a transaction the chain refuses while
            // reporting it as an approved spend. So: say the approval arrived,
            // hand back the terms it is bound to, and submit nothing.
            // docs/limitations.md records why no owner on testnet can create
            // that account today.
            e["outcome"] = "approved";
            e["approved"] = true;
            return fail("the owner approved this spend; submitting it goes through the policy "
                        "program's spend_approved path, which this module does not wire, so "
                        "nothing was submitted",
                        e);
        }

        // Terminal, not a fallback. An above-threshold spend that fails to reach
        // the owner must not execute — so this returns without ever touching
        // `port_.spend`, and says how hard it tried.
        e["outcome"] = "owner_unreachable";
        if (clockStalled) {
            e["clock_stalled"] = true;
            return fail("the approval wait gave up because the clock did not advance across " +
                            std::to_string(kMaxApprovalPollsAtOneInstant) +
                            " passes; the owner did not answer and the spend was not submitted",
                        e);
        }
        return fail("the owner did not answer within " + std::to_string(timeout) + "ms: " +
                        std::to_string(attempts) + " notification attempt(s), " +
                        std::to_string(delivered) +
                        " of which the channel accepted; the spend was not submitted",
                    e);
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
    // instructions in this repository produced confirmed, on-chain proofs that a
    // policy *permitted* an amount while moving nothing, and were nearly written
    // up as settlements; insisting on the hash is the least this can do.
    if (!isTxHash(receipt.txHash)) {
        return fail("the policy path reported success without a transaction hash",
                    json{{"submitted", false}, {"outcome", "refused"}});
    }
    return done(json{{"submitted", true},
                     {"outcome", "autonomous"},
                     {"tx", receipt.txHash},
                     {"recipient", qualified},
                     {"amount", amount}});
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
    json rows = json::array();
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
            if (rpcResult(raw, result, rpcErr)) {
                // `getTransaction` answers with a result array for a hash it
                // knows. An empty one is not a confirmation.
                row["status"] = (result.is_array() && result.empty()) ? "not-yet-included"
                                                                      : "confirmed";
            } else if (rpcErr == "the sequencer has no record of it") {
                // Pending, dropped or never submitted — the RPC cannot tell
                // those apart, so neither will this.
                row["status"] = "not-yet-included";
                row["statusDetail"] = rpcErr;
            } else {
                // A node that cannot be reached says nothing about the
                // transaction. Not knowing is its own answer, and the summary
                // carries it upwards so a caller cannot mistake this list for a
                // confirmed one.
                row["status"] = "unknown";
                row["statusDetail"] = rpcErr;
                confirmedAgainstChain = false;
            }
        }
        rows.push_back(row);
    }

    return done(json{{"transactions", rows},
                     {"count", rows.size()},
                     {"complete", false},
                     {"confirmedAgainstChain", confirmedAgainstChain},
                     {"source", "agent submission journal, confirmed via getTransaction"},
                     {"note", "the sequencer exposes no per-account history: this lists only what "
                              "this agent submitted itself. Payments received, and anything sent "
                              "by another instance of the agent, are not here"}});
}

} // namespace logos::agent
