// Exercise the wallet skills through their port, with no node and no wallet.
//
// That is what the WalletPort indirection is for. A sequencer is needed to prove
// that money moved; it is not needed to prove that `wallet.send` refuses a
// negative amount, that it holds an over-envelope spend for the owner *without
// submitting anything*, or that `wallet.balance` refuses to read the chain's
// default account record back as "you have zero". Those are the behaviours a
// reviewer cannot see from a screenshot, and they are exactly what a stub gets
// wrong — a stub returns ok:true.
//
// Every check here is written to fail if the corresponding line of
// wallet_skills.cpp is deleted. The mutations that were actually applied, built
// and run against this suite are listed at the bottom of the file.
#include "../src/wallet_skills.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static json parsed(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

static bool okOf(const std::string &r) { return parsed(r).value("ok", false); }

static std::string errOf(const std::string &r) { return parsed(r).value("error", std::string{}); }

static std::string strOf(const std::string &r, const char *key)
{
    return parsed(r).value(key, std::string{});
}

static bool mentions(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

/// Nothing was submitted, and the reply says so — the distinction a caller has
/// to be able to make without reading prose.
static bool notSubmitted(const std::string &r) { return parsed(r).value("submitted", true) == false; }

// Real-shaped ids: 44 characters of base58, the alphabet the scripts match with
// [1-9A-HJ-NP-Za-km-z]{32,}.
static const std::string kAgent = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";
static const std::string kBob = "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb72Ntu8bT";
static const std::string kTx1(64, 'a');
static const std::string kTx2(64, 'b');
static const std::string kTx3(64, 'c');
static const std::string kU128Max = "340282366920938463463374607431768211455";

/// The shape `getAccount` really returns: {program_owner, balance, data, nonce}.
static std::string accountJson(long long balance, bool programOwned, long long nonce)
{
    json owner = json::array();
    for (int i = 0; i < 32; ++i) owner.push_back(programOwned && i == 0 ? 7 : 0);
    return json{{"jsonrpc", "2.0"},
                {"id", 1},
                {"result", json{{"program_owner", owner},
                                {"balance", balance},
                                {"data", json::array()},
                                {"nonce", nonce}}}}
        .dump();
}

static SpendEnvelope envelopeOf(const std::string &perTx, const std::string &perPeriod)
{
    SpendEnvelope e;
    e.perTx = perTx;
    e.perPeriod = perPeriod;
    e.periodBlocks = 1000;
    return e;
}

/// A port whose spend path always succeeds, so that anything which does *not*
/// reach it did so because a check stopped it.
static WalletPort willingPort(int &spends, SpendRequest &seen, const std::string &spentThisPeriod)
{
    WalletPort port;
    port.spentThisPeriod = [spentThisPeriod] { return spentThisPeriod; };
    port.spend = [&spends, &seen](const SpendRequest &req) {
        ++spends;
        seen = req;
        SpendReceipt r;
        r.submitted = true;
        r.txHash = kTx1;
        return r;
    };
    return port;
}

static OwnerApprovalPort reachableOwner(int &asked, std::string &request, std::uint64_t nonce)
{
    OwnerApprovalPort owner;
    owner.requestApproval = [&asked, &request](const std::string &j) {
        ++asked;
        request = j;
        return true;
    };
    owner.nextNonce = [nonce] { return nonce; };
    return owner;
}

int main()
{
    std::printf("wallet.balance\n");
    {
        // A node that cannot be reached must refuse. The empty string is what a
        // curl that timed out hands back, and it is the realistic failure.
        WalletPort down;
        down.getAccount = [](const std::string &) { return std::string(); };
        WalletBalanceSkill b(down, "Public/" + kAgent);
        const auto r = b.invoke("{}");
        check(!okOf(r), "balance refuses when the sequencer does not answer");
        check(mentions(errOf(r), "did not answer"), "and says the node is the reason");
    }
    {
        WalletPort garbage;
        garbage.getAccount = [](const std::string &) { return std::string("<html>502</html>"); };
        WalletBalanceSkill b(garbage, "Public/" + kAgent);
        check(!okOf(b.invoke("{}")), "a non-JSON answer from the node is refused, not thrown");

        WalletPort rpcError;
        rpcError.getAccount = [](const std::string &) {
            return std::string(
                R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"bad account id"}})");
        };
        WalletBalanceSkill e(rpcError, "Public/" + kAgent);
        const auto r = e.invoke("{}");
        check(!okOf(r), "a JSON-RPC error is a refusal");
        check(mentions(errOf(r), "bad account id"), "and the node's own message survives");
    }
    {
        // The most important refusal in this file. An id the chain has never
        // seen comes back as the default account — zero owner, zero nonce, zero
        // balance — and reporting that as a balance of 0 is a confident lie.
        WalletPort unknown;
        unknown.getAccount = [](const std::string &) { return accountJson(0, false, 0); };
        WalletBalanceSkill b(unknown, "Public/" + kAgent);
        const auto r = b.invoke("{}");
        check(!okOf(r), "an account the chain has never seen is not reported as zero");
        check(mentions(errOf(r), "default account"), "and is named as the default record it is");
    }
    {
        // The other half of that conjunction: an account no program has claimed
        // is still a real account, and refusing it would be its own kind of
        // wrong.
        WalletPort unclaimed;
        unclaimed.getAccount = [](const std::string &) { return accountJson(500, false, 3); };
        WalletBalanceSkill b(unclaimed, "Public/" + kAgent);
        const auto r = b.invoke("{}");
        check(okOf(r), "a funded account with no program owner still reads");
        check(parsed(r)["balance"] == 500, "with its balance");
    }
    {
        std::string seen;
        int calls = 0;
        WalletPort up;
        up.getAccount = [&](const std::string &id) {
            seen = id;
            ++calls;
            return accountJson(4242, true, 9);
        };
        WalletBalanceSkill b(up, "Public/" + kAgent);
        const auto r = b.invoke("{}");
        check(okOf(r) && parsed(r)["balance"] == 4242, "balance reads the account off the chain");
        check(seen == kAgent, "and asks getAccount for the bare base58 id, not the qualified one");
        check(strOf(r, "source") == "sequencer.getAccount", "and says where the number came from");

        check(!okOf(b.invoke("not json")), "malformed parameters are refused, not thrown");
        check(!okOf(b.invoke(R"({"account":"not-an-account"})")), "a non-base58 account is refused");
        check(!okOf(b.invoke(R"({"account":"CbgR6tj5kWx5OziiFptM7jMvrQeYY3Mzaao6ciuhSr2r"})")),
              "and so is one carrying a character base58 does not have");
        check(calls == 1, "neither of which reached the node at all");
    }
    {
        WalletPort noBalance;
        noBalance.getAccount = [](const std::string &) {
            return std::string(R"({"result":{"program_owner":[7],"nonce":4,"data":[]}})");
        };
        WalletBalanceSkill b(noBalance, "Public/" + kAgent);
        check(!okOf(b.invoke("{}")), "a record with no balance field is refused");

        WalletPort stringBalance;
        stringBalance.getAccount = [](const std::string &) {
            return std::string(R"({"result":{"program_owner":[7],"nonce":4,"balance":"lots"}})");
        };
        WalletBalanceSkill s(stringBalance, "Public/" + kAgent);
        check(!okOf(s.invoke("{}")), "and so is a balance that is not a number");
    }
    {
        // Shielded balances are on the other side of the wall: getAccount
        // answers for a private id with the default account, so a fallback here
        // would read as a confident zero. It must refuse instead.
        bool rpcTouched = false;
        WalletPort shielded;
        shielded.getAccount = [&](const std::string &) {
            rpcTouched = true;
            return accountJson(0, false, 0);
        };
        WalletBalanceSkill noKey(shielded, "Private/" + kAgent);
        const auto r = noKey.invoke("{}");
        check(!okOf(r), "a shielded balance with no wallet wired is refused");
        check(mentions(errOf(r), "viewing key"), "and the reason is the viewing key");
        check(!rpcTouched, "and it never falls back to the RPC, which would answer zero");

        shielded.walletAccount = [](const std::string &id) {
            return json{{"account_id", id}, {"balance", 1000}}.dump();
        };
        WalletBalanceSkill withKey(shielded, "Private/" + kAgent);
        const auto w = withKey.invoke("{}");
        check(okOf(w) && parsed(w)["balance"] == 1000, "with the wallet wired, it reads");
        check(strOf(w, "source") == "wallet" && parsed(w)["shielded"] == true,
              "and reports that the number came from the wallet, not the chain");

        WalletPort mute;
        mute.walletAccount = [](const std::string &) { return std::string(); };
        WalletBalanceSkill m(mute, "Private/" + kAgent);
        check(!okOf(m.invoke("{}")), "a silent wallet is a refusal too");
    }

    std::printf("wallet.send\n");
    {
        // Nothing may reach the spend path until every parameter has been
        // accepted. The counter is the assertion: an implementation that
        // validated after submitting would still pass an ok/!ok check.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 1), envelopeOf("100", "500"));

        check(!okOf(s.invoke("not json")), "malformed parameters are refused, not thrown");
        check(!okOf(s.invoke(R"({"amount":10})")), "a missing recipient is refused");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"("})")), "a missing amount is refused");
        check(!okOf(s.invoke(R"({"recipient":"","amount":10})")), "an empty recipient is refused");

        const auto zero = s.invoke(R"({"recipient":")" + kBob + R"(","amount":0})");
        check(!okOf(zero), "an amount of zero is refused");
        check(mentions(errOf(zero), "greater than zero"), "and says why");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":-5})")),
              "a negative amount is refused");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":1.5})")),
              "a fractional amount is refused rather than rounded");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":"0"})")),
              "zero written as a string is still zero");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":"000"})")),
              "and so is zero written with leading zeros");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":"12x"})")),
              "an amount that is not a number is refused");
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":"9)" + kU128Max + R"("})")),
              "and one the chain's u128 amount could not hold");
        check(!okOf(s.invoke(R"({"recipient":"nope","amount":10})")),
              "a recipient that is not a base58 id is refused");

        const auto priv = s.invoke(R"({"recipient":"Private/)" + kBob + R"(","amount":10})");
        check(!okOf(priv), "a shielded recipient named by ID is refused");
        check(mentions(errOf(priv), "PrivateKeys/"),
              "and the refusal names the spelling that does work, not just the public account");

        // Malformed keys are refused rather than paid to. Each of these derives
        // a valid-looking account id under keys nobody holds, so a payment made
        // on one is unrecoverable — which is why the shape is checked here and
        // not left to the payer's wallet.
        const auto shortVpk =
            s.invoke(R"({"recipient":"PrivateKeys/)" + std::string(64, 'a') + ":" +
                     std::string(64, 'b') + R"(","amount":10})");
        check(!okOf(shortVpk), "a truncated vpk is refused");
        check(mentions(errOf(shortVpk), "1184"), "and the refusal says what length was expected");
        const auto noColon =
            s.invoke(R"({"recipient":"PrivateKeys/)" + std::string(64, 'a') + R"(","amount":10})");
        check(!okOf(noColon), "keys with no vpk at all are refused");

        check(spends == 0, "none of the above reached the policy path");
        check(asked == 0, "and none of them woke the owner either");
        check(notSubmitted(zero) && notSubmitted(priv), "each refusal says nothing was submitted");
    }
    {
        // The autonomous path. What matters is not only that it succeeds, but
        // that the policy path is handed exactly the spend that was checked.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "40");
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 1), envelopeOf("100", "500"));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(okOf(r) && parsed(r)["submitted"] == true, "a spend inside the envelope is submitted");
        check(strOf(r, "outcome") == "autonomous" && asked == 0, "unattended");
        check(strOf(r, "tx") == kTx1, "and returns the transaction hash");
        check(seen.recipient == "Public/" + kBob, "the policy path gets the qualified recipient");
        check(seen.amount == "50", "the amount that was checked, unaltered");
        check(seen.spentThisPeriod == "40", "and the period total the check was made against");
        check(strOf(r, "amount") == "50", "and the reply echoes the amount");

        // A bare id and a qualified one name the same account.
        const auto q = s.invoke(R"({"recipient":"Public/)" + kBob + R"(","amount":50})");
        check(okOf(q) && seen.recipient == "Public/" + kBob, "'Public/<id>' is accepted as well");

        // A shielded payee, named by the keys its Agent Card publishes. The
        // envelope check is the same one — being paid privately is not a way
        // round the policy — and the recipient reaches the policy path
        // unaltered, because there is no id to normalise it to.
        const std::string keys = "PrivateKeys/" + std::string(64, 'a') + ":" + std::string(2368, 'b');
        const auto shielded = s.invoke(R"({"recipient":")" + keys + R"(","amount":50})");
        check(okOf(shielded) && parsed(shielded)["submitted"] == true,
              "a spend to a shielded payee is submitted");
        check(strOf(shielded, "outcome") == "autonomous", "unattended, like any other spend");
        check(seen.recipient == keys, "and the policy path is handed the keys, not an account id");
        const auto overShielded = s.invoke(R"({"recipient":")" + keys + R"(","amount":500})");
        check(!okOf(overShielded) || parsed(overShielded)["submitted"] == false,
              "and the per-transaction limit still applies to it");
    }
    {
        // 2^64 exactly: an implementation that carries amounts in a uint64
        // truncates this to zero, and one that goes through a double loses the
        // low bits. The value has to survive intact all the way to the port.
        const std::string big = "18446744073709551616";
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 1), envelopeOf(kU128Max, kU128Max));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":")" + big + R"("})");
        check(okOf(r), "an amount above 2^64 is accepted as a decimal string");
        check(strOf(r, "amount") == big, "and survives the round trip intact");
        check(seen.amount == big, "as the value it names");
        check(spends == 1 && asked == 0, "and is inside a wide envelope, so it is autonomous");
    }
    {
        // Over the per-transaction limit: held, and — the part that matters —
        // not submitted. The request the owner receives must name this exact
        // spend, or an approval could be replayed against a different one.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 77), envelopeOf("100", "500"));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":101})");
        check(notSubmitted(r), "an over-limit spend is not submitted");
        check(spends == 0, "and the policy path is never called");
        check(strOf(r, "outcome") == "awaiting_owner_approval" && asked == 1,
              "it waits for the owner");
        check(mentions(strOf(r, "reason"), "per-transaction"), "and says which limit it crossed");
        const json ask = parsed(request);
        check(ask.value("recipient", std::string{}) == "Public/" + kBob &&
                  ask.value("amount", std::string{}) == "101",
              "the owner is asked about this recipient and this amount");
        check(ask.value("nonce", 0ULL) == 77ULL,
              "and the request carries a nonce, so approving it approves one spend only");

        // The boundary itself is inside the envelope, as it is in
        // agent-policy-core: `amount <= per_tx`.
        const auto edge = s.invoke(R"({"recipient":")" + kBob + R"(","amount":100})");
        check(okOf(edge) && spends == 1, "a spend at exactly the limit is still autonomous");
    }
    {
        // Under the per-transaction limit, over the period cap. A per-transaction
        // limit alone is drained by repetition, so this check is not redundant.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "470");
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 2), envelopeOf("100", "500"));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(spends == 0 && asked == 1, "a spend that would break the period cap goes to the owner");
        check(mentions(strOf(r, "reason"), "per-period"), "and the reason names the period limit");
        check(mentions(strOf(r, "reason"), "470"), "and how much of it is already spent");

        const auto fits = s.invoke(R"({"recipient":")" + kBob + R"(","amount":30})");
        check(okOf(fits) && spends == 1, "while one that fits exactly is still autonomous");
    }
    {
        // A hostile or merely stale period total must not wrap around into
        // "plenty of room left". agent-policy-core saturates for the same
        // reason; an implementation that added these as machine integers would
        // make this spend autonomous.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, kU128Max);
        int asked = 0;
        std::string request;
        WalletSendSkill s(port, reachableOwner(asked, request, 3), envelopeOf("100", "500"));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(spends == 0, "a period total at the u128 ceiling does not wrap into autonomy");
        check(strOf(r, "outcome") == "awaiting_owner_approval", "it goes to the owner instead");
    }
    {
        // An unknown period total is not zero, and an unknown envelope is not a
        // wide one. An agent that cannot say how much it has moved, or what its
        // limits are, cannot say a spend is inside them.
        int spends = 0;
        SpendRequest seen;
        int asked = 0;
        std::string request;

        WalletPort noTotal = willingPort(spends, seen, "0");
        noTotal.spentThisPeriod = nullptr; // never wired
        WalletSendSkill s(noTotal, reachableOwner(asked, request, 4), envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(spends == 0 && asked == 1, "an unwired period total is not treated as zero");
        check(mentions(strOf(r, "reason"), "unknown"), "and the owner is told that is why");

        WalletPort blankTotal = willingPort(spends, seen, "");
        WalletSendSkill b(blankTotal, reachableOwner(asked, request, 4), envelopeOf("100", "500"));
        const auto br = b.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(strOf(br, "outcome") == "awaiting_owner_approval" && spends == 0,
              "an empty period total is unknown too, and still holds the spend");

        WalletPort port = willingPort(spends, seen, "0");
        WalletSendSkill u(port, reachableOwner(asked, request, 4), envelopeOf("", ""));
        const auto ur = u.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(spends == 0, "an unknown envelope does not become a permissive one");
        check(mentions(strOf(ur, "reason"), "envelope"), "and the reason says so");
    }
    {
        // The prize is explicit: an above-threshold transaction that fails to
        // reach the owner must not execute. Not "execute anyway", and not
        // "report success and hope".
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        OwnerApprovalPort unreachable;
        unreachable.requestApproval = [](const std::string &) { return false; };
        unreachable.nextNonce = [] { return std::uint64_t{5}; };
        WalletSendSkill s(port, unreachable, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r), "an unreachable owner fails the spend");
        check(spends == 0, "and above all does not submit it anyway");
        check(strOf(r, "outcome") == "owner_unreachable" && notSubmitted(r),
              "and says so in a form a caller can branch on");

        OwnerApprovalPort none; // no channel at all
        WalletSendSkill s2(port, none, envelopeOf("100", "500"));
        const auto r2 = s2.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r2) && spends == 0, "no owner channel is a refusal, not a licence to spend");

        OwnerApprovalPort noNonce;
        noNonce.requestApproval = [](const std::string &) { return true; };
        WalletSendSkill s3(port, noNonce, envelopeOf("100", "500"));
        const auto r3 = s3.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r3) && mentions(errOf(r3), "replayed"),
              "an approval that cannot name one spend is refused: it could be replayed");
        check(spends == 0, "and still nothing was submitted");
    }

    // -----------------------------------------------------------------------
    // "The agent retries notification before timing out and reports the
    // failure."
    //
    // Every case below is written so that a PASS means the spend did NOT
    // execute. `spends == 0` is asserted in each one, and it is the assertion
    // that matters: an implementation that submitted the payment would satisfy
    // "it answered something" and fail here. The retry counts and the outcome
    // strings are the second half — an agent that gives up after one attempt,
    // or that never says it gave up, is what this suite is for.
    //
    // The clock is a fake that advances only when the agent calls `idle`, so
    // these run in microseconds and a timeout is not a sleep. That is also the
    // reason a fake `idle` is what moves it: an agent that waits without ever
    // yielding would never advance this clock and would be caught by the stuck
    // -clock case at the end rather than passing quietly.
    // -----------------------------------------------------------------------
    std::printf("\nwallet.send — the owner who does not answer\n");
    {
        // The headline case. An owner who is simply not there.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        std::vector<std::string> sent;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{41}; };
        owner.requestApproval = [&sent](const std::string &j) {
            sent.push_back(j);
            return true;
        };
        owner.pollDecision = [](const std::string &) { return std::string{}; };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{1000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");

        check(spends == 0, "an owner who never answers does not get the spend made anyway");
        check(notSubmitted(r) && !okOf(r), "the call fails, and says nothing was submitted");
        check(strOf(r, "outcome") == "owner_unreachable",
              "and reports it as the owner being unreachable");
        check(int(sent.size()) >= 4 && parsed(r).value("attempts", 0) == int(sent.size()),
              "the request is retried while waiting, and the reply says how many times");
        check(parsed(r).value("attempts", 0) > 1,
              "which is more than once: a single attempt is not a retry");
        check(parsed(r).value("waited_ms", 0) >= 1000,
              "it waited out the whole timeout before giving up");
        check(mentions(errOf(r), "did not answer") && mentions(errOf(r), "not submitted"),
              "and the failure is reported in words, not only in a field");

        // One question asked again — not several different questions. An id or
        // a nonce that moved between attempts would let the answer to attempt 1
        // authorise the terms of attempt 4.
        bool identical = !sent.empty();
        for (const auto &one : sent) identical = identical && one == sent.front();
        check(identical, "every retry carries the byte-identical request");
        check(parsed(sent.front()).value("id", std::string{}) == "spend-41",
              "which names this one spend by a correlation id derived from its nonce");
    }
    {
        // The transport itself refusing every attempt. The old code stopped at
        // the first refusal; the criterion asks for retries before timing out.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        int attempts = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{42}; };
        owner.requestApproval = [&attempts](const std::string &) {
            ++attempts;
            return false; // the channel will not carry it
        };
        owner.pollDecision = [](const std::string &) { return std::string{}; };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{1000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");

        check(spends == 0, "a channel that carries nothing does not make the spend autonomous");
        check(attempts > 1, "the refused notification is retried, not abandoned on the first no");
        check(parsed(r).value("delivered", -1) == 0,
              "and the reply separates attempts from attempts the channel accepted");
        check(strOf(r, "outcome") == "owner_unreachable" && notSubmitted(r),
              "the outcome is the terminal one");
    }
    {
        // An owner who answers late — after the agent has already retried. The
        // approval is honoured as an answer and still does not submit anything,
        // because submitting an approved spend goes through the policy
        // program's `spend_approved`, which this module does not wire.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        int attempts = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{43}; };
        owner.requestApproval = [&attempts](const std::string &) {
            ++attempts;
            return true;
        };
        owner.pollDecision = [&attempts](const std::string &id) {
            return (attempts >= 3 && id == "spend-43") ? std::string("approved") : std::string{};
        };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");

        check(attempts >= 3, "a late answer is still heard, after the retries");
        check(strOf(r, "outcome") == "approved" && parsed(r).value("approved", false),
              "and an approval is reported as one");
        check(spends == 0 && notSubmitted(r),
              "and it is STILL not submitted where no submitter for it is wired");
        check(mentions(errOf(r), "spend_approved"),
              "and the reply names the path that would have to carry it");
    }

    std::printf("\nwallet.send — the spend the owner approved, submitted\n");
    {
        // THE STEP THAT USED TO BE MISSING.
        //
        // Both halves of the exchange were real — the agent asks, the owner
        // answers — and the payment stopped between them: an approval was
        // reported and nothing was ever submitted for it, because
        // `spend_approved` is a different instruction from `spend` and nothing
        // here could reach it. `WalletPort::spendApproved` is that instruction.
        int spends = 0, approvedSubmissions = 0;
        SpendRequest seen;
        ApprovedSpendRequest seenApproved;
        WalletPort port = willingPort(spends, seen, "0");
        port.spendApproved = [&](const ApprovedSpendRequest &req) {
            ++approvedSubmissions;
            seenApproved = req;
            SpendReceipt receipt;
            receipt.submitted = true;
            receipt.txHash = kTx3;
            return receipt;
        };

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{4242}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &id) {
            return id == "spend-4242" ? std::string("approved") : std::string{};
        };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");

        check(okOf(r) && parsed(r).value("submitted", false),
              "an approved above-threshold spend is SUBMITTED, not merely acknowledged");
        check(strOf(r, "tx") == kTx3,
              "and the reply carries the transaction that carried it");
        check(strOf(r, "outcome") == "approved" && parsed(r).value("approved", false),
              "reported as the approved spend it is");
        check(approvedSubmissions == 1,
              "the approved-spend path is used exactly once — the marker is single-use, so a "
              "retry is either refused by the chain or a duplicate of a payment in flight");
        check(spends == 0,
              "and the AUTONOMOUS instruction is not touched: the chain refuses it above the "
              "per-transaction ceiling, so putting the payment through it would report a "
              "settlement the chain had rejected");
        check(seenApproved.recipient == "Public/" + kBob && seenApproved.amount == "500",
              "the submission carries the terms the owner was asked about");
        check(seenApproved.nonce == 4242,
              "and the nonce it was approved under, which is what binds the approval to this "
              "one payment rather than to every identical later one");
        check(seenApproved.requestId == "spend-4242",
              "and the correlation id the exchange ran under");
    }
    {
        // A submitter that refused. The owner said yes and the chain did not
        // take it — an approval marker already spent, an expired one, a
        // sequencer that could not be reached. Reported as what it is, and never
        // as a payment.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        port.spendApproved = [](const ApprovedSpendRequest &) {
            SpendReceipt receipt;
            receipt.error = "the approval marker for this payment has already been spent";
            return receipt;
        };

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{4243}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string("approved"); };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r) && notSubmitted(r), "a refused approved spend is not reported as submitted");
        check(strOf(r, "outcome") == "approved_not_submitted",
              "and the outcome separates it from an owner who never answered");
        check(mentions(errOf(r), "already been spent"),
              "passing the submitter's own reason through rather than rewording it");
        check(spends == 0, "with no fallback to the autonomous instruction");
    }
    {
        // A success flag with no transaction hash. This repository has produced
        // confirmed, on-chain proofs that a policy PERMITTED an amount while
        // moving nothing; here it would be one an owner personally authorised.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        port.spendApproved = [](const ApprovedSpendRequest &) {
            SpendReceipt receipt;
            receipt.submitted = true;
            receipt.txHash = "ok";
            return receipt;
        };

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{4244}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string("approved"); };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r) && notSubmitted(r),
              "an approved spend that reports success without a hash is not a settlement");
        check(mentions(errOf(r), "without a transaction hash"), "and says exactly that");
    }
    {
        // The submitter exists and the owner said no. Wiring the approved path
        // must not make a denial reach it — that would be a payment nobody
        // approved, submitted through the instruction that exists for approvals.
        int spends = 0, approvedSubmissions = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");
        port.spendApproved = [&approvedSubmissions](const ApprovedSpendRequest &) {
            ++approvedSubmissions;
            SpendReceipt receipt;
            receipt.submitted = true;
            receipt.txHash = kTx3;
            return receipt;
        };

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{4245}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string("denied"); };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto denied = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(strOf(denied, "outcome") == "denied" && approvedSubmissions == 0,
              "a denial does not reach the approved-spend path");

        // And an owner who never answers. The prize's clause is that such a
        // spend "is not executed"; a wired submitter must not weaken it.
        OwnerApprovalPort silent = owner;
        silent.nextNonce = [] { return std::uint64_t{4246}; };
        silent.pollDecision = [](const std::string &) { return std::string{}; };
        silent.timeoutMs = [] { return std::int64_t{1000}; };
        WalletSendSkill quiet(port, silent, envelopeOf("100", "500"));
        const auto unanswered = quiet.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(strOf(unanswered, "outcome") == "owner_unreachable" && approvedSubmissions == 0,
              "and neither does an owner who never answers");
        check(spends == 0, "nor does either fall back to the autonomous instruction");
    }
    {
        // A denial. The owner was reached and said no, which is not the same
        // failure as not reaching them — and must not be reported as one.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{44}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string("denied"); };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{10000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(strOf(r, "outcome") == "denied", "a refusal by the owner is reported as a denial");
        check(spends == 0 && notSubmitted(r), "and nothing is submitted");
        // The field has to BE there, and only then be small. `value("waited_ms",
        // -1) < 1000` was satisfied by a reply that carried no `waited_ms` at
        // all: deleting the one line in wallet_skills.cpp that writes it left
        // this printing `ok` while its mirror at "it waited out the whole
        // timeout before giving up" — the same field, read with a default that
        // makes absence fail — went red. A denial that arrives early and a
        // module that stopped reporting how long it waited are not the same
        // event, and this is the assertion that tells them apart.
        check(parsed(r).contains("waited_ms"),
              "the reply says how long it waited");
        check(parsed(r).value("waited_ms", -1) >= 0
                  && parsed(r).value("waited_ms", -1) < 1000,
              "and it does not sit out the whole timeout once the answer is in");
    }
    {
        // An answer this agent cannot read is not a "no", and it is certainly
        // not a "yes". It is counted and the wait goes on — which is what makes
        // the outcome below a timeout rather than a denial.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{45}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string("APPROVED, obviously"); };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{1000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(spends == 0, "an answer that is not one of the two verdicts does not authorise a spend");
        check(strOf(r, "outcome") == "owner_unreachable",
              "it times out rather than reading an unparsed word as consent");
        check(parsed(r).value("unusable_answers", 0) > 0,
              "and the operator is told their owner app is answering with something unreadable");
    }
    {
        // The answer must name *this* request. An approval for some other spend
        // sitting in the same answer table must not settle this one.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        std::int64_t clock = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{46}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &id) {
            return id == "spend-999" ? std::string("approved") : std::string{};
        };
        owner.nowMs = [&clock] { return clock; };
        owner.idle = [&clock] { clock += 100; };
        owner.timeoutMs = [] { return std::int64_t{1000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(spends == 0 && strOf(r, "outcome") == "owner_unreachable",
              "an approval belonging to another spend does not settle this one");
    }
    {
        // No answer path at all. The agent does not pretend to wait for a reply
        // that has no route to it, and it says so — but it still does not spend.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        int attempts = 0;
        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{47}; };
        owner.requestApproval = [&attempts](const std::string &) {
            ++attempts;
            return true;
        };
        // pollDecision and nowMs deliberately left unwired.

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(spends == 0, "an agent with no answer path still does not spend");
        check(attempts == 1 && strOf(r, "outcome") == "awaiting_owner_approval",
              "it notifies once instead of timing out against an owner it gave no way to reply");
        check(parsed(r).value("answer_path", true) == false,
              "and the reply says outright that no answer can reach this process");
    }
    {
        // A clock that has stopped must not hang the module inside a skill call.
        // A host sees that as the module having died, and it is exactly the
        // failure mode `invoke()` drops its lock to avoid.
        int spends = 0;
        SpendRequest seen;
        WalletPort port = willingPort(spends, seen, "0");

        OwnerApprovalPort owner;
        owner.nextNonce = [] { return std::uint64_t{48}; };
        owner.requestApproval = [](const std::string &) { return true; };
        owner.pollDecision = [](const std::string &) { return std::string{}; };
        owner.nowMs = [] { return std::int64_t{7}; }; // never advances
        owner.timeoutMs = [] { return std::int64_t{60000}; };
        owner.resendIntervalMs = [] { return std::int64_t{250}; };
        // No idle: nothing will ever move this clock.

        WalletSendSkill s(port, owner, envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        // `check(true, …)` stood here, which is an assertion that cannot fail,
        // and it stayed green when the skill was made to answer `{}` — nothing
        // at all. The claim is that the call TERMINATED, and the evidence for
        // termination is that a reply came back and that it parses: a module
        // that hung would never reach this line, and one that returned an empty
        // string reached it having answered nothing. Both are now told apart.
        check(!r.empty() && !parsed(r).is_discarded(),
              "a stopped clock terminates the wait rather than hanging the module");
        check(parsed(r).value("clock_stalled", false),
              "and it is reported as a stopped clock, not as a deadline that passed");
        check(spends == 0 && notSubmitted(r) && strOf(r, "outcome") == "owner_unreachable",
              "and the spend is still not submitted");
    }
    {
        // The chain is the enforcer, so its refusal has to survive intact rather
        // than being reworded into something reassuring.
        WalletPort refused;
        refused.spentThisPeriod = [] { return std::string("0"); };
        refused.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = false;
            r.error = "SpendOverPolicy: the anchored account rejected it";
            return r;
        };
        int asked = 0;
        std::string request;
        WalletSendSkill s(refused, reachableOwner(asked, request, 6), envelopeOf("100", "500"));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(!okOf(r), "a spend the chain refuses is a failure here too");
        check(mentions(errOf(r), "SpendOverPolicy"), "and the chain's own reason survives");
        check(notSubmitted(r), "and nothing is claimed to have been submitted");

        // A success flag with no transaction hash is not a payment. This exact
        // shape — a policy that permits an amount and moves nothing — has been
        // produced on chain in this repository before.
        WalletPort hollow;
        hollow.spentThisPeriod = [] { return std::string("0"); };
        hollow.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = true;
            return r;
        };
        WalletSendSkill h(hollow, reachableOwner(asked, request, 6), envelopeOf("100", "500"));
        const auto hr = h.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(!okOf(hr), "success without a transaction hash is not accepted as a payment");
        check(mentions(errOf(hr), "without a transaction hash"), "and is named for what it is");

        WalletPort junkHash;
        junkHash.spentThisPeriod = [] { return std::string("0"); };
        junkHash.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = true;
            r.txHash = "0xdeadbeef";
            return r;
        };
        WalletSendSkill jh(junkHash, reachableOwner(asked, request, 6), envelopeOf("100", "500"));
        check(!okOf(jh.invoke(R"({"recipient":")" + kBob + R"(","amount":50})")),
              "nor is a hash that is not 32 bytes of hex");

        WalletPort noPath; // no spend function wired at all
        noPath.spentThisPeriod = [] { return std::string("0"); };
        WalletSendSkill np(noPath, reachableOwner(asked, request, 6), envelopeOf("100", "500"));
        check(!okOf(np.invoke(R"({"recipient":")" + kBob + R"(","amount":50})")),
              "with no policy path wired there is no other way to move funds");
    }

    std::printf("wallet.history\n");
    {
        WalletPort none;
        WalletHistorySkill h(none);
        const auto r = h.invoke("{}");
        check(!okOf(r), "history refuses when there is no journal");
        check(mentions(errOf(r), "no per-account history endpoint"),
              "and is honest that the chain has nothing to fall back on");
    }
    {
        WalletPort garbage;
        garbage.journal = [] { return std::string("{oh dear"); };
        WalletHistorySkill g(garbage);
        check(!okOf(g.invoke("{}")), "a journal that does not parse is refused, not thrown");

        WalletPort wrongShape;
        wrongShape.journal = [] { return std::string(R"({"tx":"..."})"); };
        WalletHistorySkill w(wrongShape);
        check(!okOf(w.invoke("{}")), "and so is one that is not an array");

        WalletPort noHash;
        noHash.journal = [] { return std::string(R"([{"recipient":"bob","amount":"5"}])"); };
        WalletHistorySkill n(noHash);
        const auto r = n.invoke("{}");
        check(!okOf(r), "an entry with no transaction hash is refused");
        check(mentions(errOf(r), "cannot"), "because it cannot be confirmed against anything");

        WalletPort badHash;
        badHash.journal = [] { return std::string(R"([{"tx":"deadbeef"}])"); };
        WalletHistorySkill b(badHash);
        check(!okOf(b.invoke("{}")), "and so is one whose hash is not 32 bytes of hex");
    }
    {
        // Confirmation is a real chain read, one hash at a time, because
        // getTransaction is the only endpoint that can speak to it.
        std::vector<std::string> queried;
        WalletPort port;
        port.journal = [] {
            return json::array(
                       {json{{"tx", kTx1}, {"recipient", "Public/" + kBob}, {"amount", "25"}},
                        json{{"tx", kTx2}},
                        json{{"tx", kTx3}}})
                .dump();
        };
        port.getTransaction = [&](const std::string &tx) {
            queried.push_back(tx);
            // The two ways the RPC says "I have nothing for this hash", and the
            // one way it says otherwise.
            if (tx == kTx2) return std::string(R"({"jsonrpc":"2.0","id":1,"result":null})");
            if (tx == kTx1) return std::string(R"({"result":[]})");
            return std::string(R"({"result":[{"block":42}]})");
        };
        WalletHistorySkill h(port);
        const auto r = h.invoke("{}");
        check(okOf(r), "history reports the agent's own submissions");
        const json rows = parsed(r)["transactions"];
        check(rows.size() == 3, "all of them, by default");
        check(rows[0].value("tx", std::string{}) == kTx3, "newest first");
        check(rows[2].value("tx", std::string{}) == kTx1, "oldest last");
        check(rows[0].value("status", std::string{}) == "confirmed",
              "a hash the chain knows is confirmed");
        check(rows[1].value("status", std::string{}) == "not-yet-included",
              "a hash it has no record of is not");
        check(rows[2].value("status", std::string{}) == "not-yet-included",
              "and neither is one it answers for with an empty result");
        check(rows[2].value("recipient", std::string{}) == "Public/" + kBob,
              "and the journal's own fields survive");
        check(parsed(r)["complete"] == false && parsed(r)["confirmedAgainstChain"] == true,
              "the summary admits it is not the whole story");
        check(mentions(parsed(r).value("note", std::string{}), "received"),
              "and names payments received as the thing it cannot show");
        check(queried.size() == 3, "each hash was actually asked about");

        queried.clear();
        const auto two = h.invoke(R"({"limit":2})");
        const json trimmed = parsed(two)["transactions"];
        check(trimmed.size() == 2 && trimmed[0].value("tx", std::string{}) == kTx3,
              "a limit takes the most recent, not the first written");
        check(queried.size() == 2, "and confirms only what it returns");
        check(!okOf(h.invoke(R"({"limit":0})")), "a limit of zero is refused");
        check(!okOf(h.invoke(R"({"limit":-1})")), "and so is a negative one");
        check(!okOf(h.invoke(R"({"limit":"lots"})")), "and one that is not a number");
    }
    {
        // A node that cannot be reached says nothing about a transaction. The
        // status must not collapse to "confirmed" or to "failed" — both would be
        // inventing a fact — and the summary has to carry the doubt upwards.
        WalletPort mute;
        mute.journal = [] { return json::array({json{{"tx", kTx1}}}).dump(); };
        mute.getTransaction = [](const std::string &) { return std::string(); };
        WalletHistorySkill h(mute);
        const auto r = h.invoke("{}");
        check(okOf(r), "history still answers while the node is down");
        check(parsed(r)["transactions"][0].value("status", std::string{}) == "unknown",
              "but an unreachable node leaves the status unknown");
        check(parsed(r)["confirmedAgainstChain"] == false,
              "and the summary stops claiming the list was confirmed");

        WalletPort unwired;
        unwired.journal = [] { return json::array({json{{"tx", kTx1}}}).dump(); };
        WalletHistorySkill u(unwired);
        const auto ur = u.invoke("{}");
        check(parsed(ur)["transactions"][0].value("status", std::string{}) == "unverified",
              "with no RPC wired at all, entries are marked unverified");
        check(parsed(ur)["confirmedAgainstChain"] == false, "and nothing is claimed as confirmed");
    }
    {
        WalletPort empty;
        empty.journal = [] { return std::string("[]"); };
        WalletHistorySkill h(empty);
        const auto r = h.invoke("{}");
        check(okOf(r) && parsed(r)["count"] == 0,
              "an empty journal is an empty history, not an error");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all wallet behaviours hold");
    return failures ? 1 : 0;
}

// MUTATIONS RUN AGAINST THIS SUITE
//
// Each line below was applied to module/src/wallet_skills.cpp on its own, built
// with the same command as this file, and run. Every one of them turns the suite
// red; the number is how many checks noticed. They are recorded because a test
// nobody has tried to break is a test nobody has checked — and the first version
// of this file left the last one green.
//
//   balance   drop the default-account check ...........................  3
//   balance   read a shielded account over the RPC like any other ......  4
//   balance   hand getAccount the qualified id instead of the bare one .  2
//   balance   accept a balance field of any type .......................  3
//   both      skip base58 validation of an account id .................  6
//   send      treat an unknown period total as zero ....................  5
//   send      treat an unknown envelope as permissive ..................  2
//   send      add the period total as a machine integer ................  3
//   send      drop the per-period comparison ............................ 7
//   send      drop the per-transaction comparison ....................... 14
//   send      skip the envelope and let the chain sort it out ........... 25
//   send      submit anyway when the owner cannot be reached ............  3
//   send      drop the approval-nonce requirement ............ (terminates)
//   send      notify once instead of retrying while waiting ............  6
//   send      submit anyway once the wait has timed out ................ 16
//   send      read any answer at all as consent ........................  1
//   send      report a timeout as still awaiting the owner ..............  5
//   send      never bound the wait on a stopped clock ......... (hangs, and
//             that hang IS the catch: the suite never returns, which is what
//             the guard prevents inside a real skill call)
//   send      accept a receipt with no transaction hash ................   4
//   send      accept any amount at all ................................. 32
//   send      accept a zero amount ......................................  7
//   send      carry the amount through a uint64 .........................  3
//   history   claim confirmation even with no RPC wired .................  2
//   history   never ask getTransaction anything .........................  8
//   history   report every entry as confirmed ...........................  2
//   history   leave the journal in its own order ........................  7
//   history   accept an entry with no transaction hash ..................  6
//   history   accept any limit ..........................................  2
//
// One caveat, since it is the sort of thing this list exists to admit: deleting
// the *negative number* branch of parseAmount does NOT turn the suite red,
// because a negative JSON number then falls through to the final `else` and is
// refused there anyway. The behaviour is unchanged, so there is nothing for a
// test to catch — the branch buys a better error message, not a stronger check.
