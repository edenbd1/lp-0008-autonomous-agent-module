// Exercise the wallet skills through their port, with no node and no wallet.
//
// The point of the WalletPort indirection is that none of the behaviour worth
// checking here needs a chain. A sequencer is needed to prove that money moved;
// it is not needed to prove that `wallet.send` refuses a negative amount, holds
// an over-envelope spend for the owner *without submitting anything*, or that
// `wallet.balance` refuses to read a default account record as "you have zero".
// Those are the behaviours a reviewer cannot see from a screenshot, and they are
// precisely what a stub gets wrong — a stub returns ok:true.
//
// Every check below is written so that deleting the corresponding line of
// wallet_skills.cpp turns the suite red. The mutations that were actually run
// against it are listed at the bottom of this file.
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

// Real-shaped ids: 44 characters of base58, the same alphabet the scripts match
// with [1-9A-HJ-NP-Za-km-z]{32,}.
static const std::string kAgent = "CbgR6tj5kWx5oziiFptM7jMvrQeYY3Mzaao6ciuhSr2r";
static const std::string kBob = "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb72Ntu8bT";
static const std::string kTx1(64, 'a');
static const std::string kTx2(64, 'b');
static const std::string kTx3(64, 'c');

/// The shape `getAccount` really returns: {program_owner, balance, data, nonce}.
static std::string accountJson(long long balance, bool programOwned, long long nonce)
{
    json owner = json::array();
    for (int i = 0; i < 32; ++i) owner.push_back(programOwned ? (i == 0 ? 7 : 0) : 0);
    return json{{"jsonrpc", "2.0"},
                {"id", 1},
                {"result", json{{"program_owner", owner},
                                {"balance", balance},
                                {"data", json::array()},
                                {"nonce", nonce}}}}
        .dump();
}

static SpendPolicy policyOf(unsigned long long perTx, unsigned long long perPeriod)
{
    SpendPolicy p;
    p.perTx = perTx;
    p.perPeriod = perPeriod;
    p.periodBlocks = 1000;
    return p;
}

int main()
{
    std::printf("wallet.balance\n");
    {
        // A node that is not reachable must refuse. The empty string is what a
        // curl that timed out hands back, and it is the realistic failure.
        WalletPort down;
        down.getAccount = [](const std::string &) { return std::string(); };
        WalletBalanceSkill b(down, "Public/" + kAgent);
        const auto r = b.invoke("{}");
        check(!okOf(r), "balance refuses when the sequencer does not answer");
        check(mentions(errOf(r), "did not answer"), "and says the node did not answer");
    }
    {
        WalletPort garbage;
        garbage.getAccount = [](const std::string &) { return std::string("<html>502</html>"); };
        WalletBalanceSkill b(garbage, "Public/" + kAgent);
        check(!okOf(b.invoke("{}")), "a non-JSON answer from the node is refused, not thrown");

        WalletPort rpcError;
        rpcError.getAccount = [](const std::string &) {
            return std::string(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"bad account id"}})");
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
        // The other half of that conjunction: a funded account that no program
        // has claimed yet is a real account, and refusing it would be its own
        // kind of wrong.
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
        // validates after submitting would still pass an ok/!ok check.
        int spends = 0;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [](const std::string &) { return true; };
        owner.nextNonce = [] { return std::uint64_t{1}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

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
        check(!okOf(s.invoke(R"({"recipient":")" + kBob + R"(","amount":"12x"})")),
              "an amount that is not a number is refused");
        check(!okOf(s.invoke(R"({"recipient":"nope","amount":10})")),
              "a recipient that is not a base58 id is refused");

        const auto priv = s.invoke(R"({"recipient":"Private/)" + kBob + R"(","amount":10})");
        check(!okOf(priv), "a shielded recipient is refused");
        check(mentions(errOf(priv), "public account"), "with the reason a payer can act on");

        check(spends == 0, "none of the above reached the policy path");
        for (const auto &r : {zero, priv}) {
            check(parsed(r).value("submitted", true) == false, "and each refusal says nothing was submitted");
        }
    }
    {
        // The autonomous path. What matters is not only that it succeeds but
        // that the policy path is handed exactly the spend that was checked.
        SpendRequest seen;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(40); };
        port.spend = [&](const SpendRequest &req) {
            seen = req;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [](const std::string &) { return true; };
        owner.nextNonce = [] { return std::uint64_t{1}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(okOf(r) && parsed(r)["submitted"] == true, "a spend inside the envelope is submitted");
        check(strOf(r, "outcome") == "autonomous", "unattended");
        check(strOf(r, "tx") == kTx1, "and returns the transaction hash");
        check(seen.recipient == "Public/" + kBob, "the policy path is given the qualified recipient");
        check(seen.amount == 50, "the amount that was checked, unaltered");
        check(seen.spentThisPeriod == 40, "and the period total the check was made against");
        check(strOf(r, "amount") == "50", "and the reply echoes the amount");
    }
    {
        // 2^64 exactly: an implementation that carries amounts in a uint64
        // truncates this to zero, and one that goes through a double loses the
        // low bits. The value has to survive the whole way to the port.
        const std::string big = "18446744073709551616";
        SpendRequest seen;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        port.spend = [&](const SpendRequest &req) {
            seen = req;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx2;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [](const std::string &) { return true; };
        owner.nextNonce = [] { return std::uint64_t{1}; };
        SpendPolicy wide;
        wide.perTx = ~static_cast<unsigned __int128>(0);
        wide.perPeriod = ~static_cast<unsigned __int128>(0);
        wide.periodBlocks = 1000;
        WalletSendSkill s(port, owner, wide);

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":")" + big + R"("})");
        check(okOf(r), "an amount above 2^64 is accepted as a decimal string");
        check(strOf(r, "amount") == big, "and survives the round trip intact");
        check(seen.amount == (static_cast<unsigned __int128>(1) << 64), "as the value it names");
    }
    {
        // Over the per-transaction limit: held, and — the part that matters —
        // not submitted. The request the owner receives must name this exact
        // spend, or an approval could be replayed against a different one.
        int spends = 0;
        std::string request;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [&](const std::string &j) {
            request = j;
            return true;
        };
        owner.nextNonce = [] { return std::uint64_t{77}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":101})");
        check(parsed(r).value("submitted", true) == false, "an over-limit spend is not submitted");
        check(spends == 0, "and the policy path is never called");
        check(strOf(r, "outcome") == "awaiting_owner_approval", "it waits for the owner");
        check(mentions(strOf(r, "reason"), "per-transaction"), "and says which limit it crossed");
        const json ask = parsed(request);
        check(ask.value("recipient", std::string{}) == "Public/" + kBob &&
                  ask.value("amount", std::string{}) == "101",
              "the owner is asked about this recipient and this amount");
        check(ask.value("nonce", 0ULL) == 77ULL,
              "and the request carries a nonce, so approving it approves one spend only");
    }
    {
        // Under the per-transaction limit, over the period cap. A per-transaction
        // limit alone is drained by repetition, so this check is not redundant.
        int spends = 0;
        int asked = 0;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(470); };
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [&](const std::string &) {
            ++asked;
            return true;
        };
        owner.nextNonce = [] { return std::uint64_t{2}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(spends == 0 && asked == 1, "a spend that would break the period cap goes to the owner");
        check(mentions(strOf(r, "reason"), "per-period"), "and the reason names the period limit");
        check(mentions(strOf(r, "reason"), "470"), "and how much of it is already spent");
    }
    {
        // A hostile or merely stale period total must not wrap around into
        // "plenty of room left". agent-policy-core saturates for the same
        // reason; a naive addition here would make this spend autonomous.
        int spends = 0;
        WalletPort port;
        port.spentThisPeriod = [] { return ~static_cast<unsigned __int128>(0); };
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [](const std::string &) { return true; };
        owner.nextNonce = [] { return std::uint64_t{3}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(spends == 0, "a period total at the u128 ceiling does not wrap into autonomy");
        check(strOf(r, "outcome") == "awaiting_owner_approval", "it goes to the owner instead");
    }
    {
        // An unknown period total is not zero. An agent that cannot say how much
        // it has already moved cannot say a spend is inside the envelope.
        int spends = 0;
        int asked = 0;
        WalletPort port; // spentThisPeriod deliberately unwired
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [&](const std::string &) {
            ++asked;
            return true;
        };
        owner.nextNonce = [] { return std::uint64_t{4}; };
        WalletSendSkill s(port, owner, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":1})");
        check(spends == 0 && asked == 1, "an unknown period total is not treated as zero");
        check(mentions(strOf(r, "reason"), "unknown"), "and the owner is told that is why");
    }
    {
        // The prize is explicit: an above-threshold transaction that fails to
        // reach the owner must not execute. Not "execute anyway", and not
        // "report success and hope".
        int spends = 0;
        WalletPort port;
        port.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        port.spend = [&](const SpendRequest &) {
            ++spends;
            SpendReceipt r;
            r.submitted = true;
            r.txHash = kTx1;
            return r;
        };
        OwnerChannel unreachable;
        unreachable.requestApproval = [](const std::string &) { return false; };
        unreachable.nextNonce = [] { return std::uint64_t{5}; };
        WalletSendSkill s(port, unreachable, policyOf(100, 500));

        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r), "an unreachable owner fails the spend");
        check(spends == 0, "and above all does not submit it anyway");
        check(strOf(r, "outcome") == "owner_unreachable" && parsed(r)["submitted"] == false,
              "and says so in a form a caller can branch on");

        OwnerChannel none; // no channel at all
        WalletSendSkill s2(port, none, policyOf(100, 500));
        const auto r2 = s2.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r2) && spends == 0, "no owner channel is a refusal, not a licence to spend");

        OwnerChannel noNonce;
        noNonce.requestApproval = [](const std::string &) { return true; };
        WalletSendSkill s3(port, noNonce, policyOf(100, 500));
        const auto r3 = s3.invoke(R"({"recipient":")" + kBob + R"(","amount":500})");
        check(!okOf(r3) && mentions(errOf(r3), "replayed"),
              "an approval that cannot name one spend is refused, because it could be replayed");
        check(spends == 0, "and still nothing was submitted");
    }
    {
        // The chain is the enforcer, so its refusal has to survive intact rather
        // than being reworded into something reassuring.
        WalletPort refused;
        refused.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        refused.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = false;
            r.error = "SpendOverPolicy: the anchored account rejected it";
            return r;
        };
        OwnerChannel owner;
        owner.requestApproval = [](const std::string &) { return true; };
        owner.nextNonce = [] { return std::uint64_t{6}; };
        WalletSendSkill s(refused, owner, policyOf(100, 500));
        const auto r = s.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(!okOf(r), "a spend the chain refuses is a failure here too");
        check(mentions(errOf(r), "SpendOverPolicy"), "and the chain's own reason survives");
        check(parsed(r)["submitted"] == false, "and nothing is claimed to have been submitted");

        // A success flag with no transaction hash is not a payment. This exact
        // shape — a policy that permits an amount and moves nothing — has been
        // produced on chain in this repository before.
        WalletPort hollow;
        hollow.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        hollow.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = true;
            r.txHash = "";
            return r;
        };
        WalletSendSkill h(hollow, owner, policyOf(100, 500));
        const auto hr = h.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(!okOf(hr), "success without a transaction hash is not accepted as a payment");
        check(mentions(errOf(hr), "without a transaction hash"), "and is named for what it is");

        WalletPort junkHash;
        junkHash.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        junkHash.spend = [](const SpendRequest &) {
            SpendReceipt r;
            r.submitted = true;
            r.txHash = "0xdeadbeef";
            return r;
        };
        WalletSendSkill jh(junkHash, owner, policyOf(100, 500));
        check(!okOf(jh.invoke(R"({"recipient":")" + kBob + R"(","amount":50})")),
              "nor is a hash that is not 32 bytes of hex");

        WalletPort noPath; // no spend function wired at all
        noPath.spentThisPeriod = [] { return static_cast<unsigned __int128>(0); };
        WalletSendSkill np(noPath, owner, policyOf(100, 500));
        const auto npr = np.invoke(R"({"recipient":")" + kBob + R"(","amount":50})");
        check(!okOf(npr), "with no policy path wired there is no other way to move funds");
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
        WalletHistorySkill h(garbage);
        check(!okOf(h.invoke("{}")), "a journal that does not parse is refused, not thrown");

        WalletPort wrongShape;
        wrongShape.journal = [] { return std::string(R"({"tx":"…"})"); };
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
            return json::array({json{{"tx", kTx1}, {"recipient", "Public/" + kBob}, {"amount", "25"}},
                                json{{"tx", kTx2}},
                                json{{"tx", kTx3}}})
                .dump();
        };
        port.getTransaction = [&](const std::string &tx) {
            queried.push_back(tx);
            if (tx == kTx3) return std::string(R"({"result":[{"block":91}]})");
            if (tx == kTx2) return std::string(R"({"jsonrpc":"2.0","id":1,"result":null})");
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
        check(rows[2].value("recipient", std::string{}) == "Public/" + kBob,
              "and the journal's own fields survive");
        check(parsed(r)["complete"] == false && parsed(r)["confirmedAgainstChain"] == true,
              "the summary admits it is not the whole story");
        check(mentions(parsed(r).value("note", std::string{}), "received"),
              "and names payments received as the thing it cannot show");
        check(queried.size() == 3, "each hash was actually asked about");

        const auto two = h.invoke(R"({"limit":2})");
        const json trimmed = parsed(two)["transactions"];
        check(trimmed.size() == 2 && trimmed[0].value("tx", std::string{}) == kTx3,
              "a limit takes the most recent, not the first written");
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
        check(parsed(ur)["confirmedAgainstChain"] == false, "and nothing is claimed to be confirmed");
    }
    {
        WalletPort empty;
        empty.journal = [] { return std::string("[]"); };
        WalletHistorySkill h(empty);
        const auto r = h.invoke("{}");
        check(okOf(r) && parsed(r)["count"] == 0, "an empty journal is an empty history, not an error");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all wallet behaviours hold");
    return failures ? 1 : 0;
}

// MUTATIONS RUN AGAINST THIS SUITE
//
// Each of these was applied to module/src/wallet_skills.cpp, compiled, and the
// suite run; every one of them turns it red. They are listed because a test
// nobody has tried to break is a test nobody has checked.
//
//   1. drop the default-account check in wallet.balance
//   2. let a shielded read fall back to getAccount when no wallet is wired
//   3. pass the qualified id (Public/<x>) to getAccount instead of the bare one
//   4. accept a balance field of any type
//   5. skip base58 validation of the account id
//   6. treat a missing spentThisPeriod as zero rather than unknown
//   7. use a plain + instead of the saturating add for the period total
//   8. drop the per-period comparison and keep only the per-transaction one
//   9. drop the per-transaction comparison and keep only the per-period one
//  10. submit anyway when the owner cannot be reached
//  11. accept a spend receipt whose txHash is empty or malformed
//  12. accept a zero amount, and accept a negative one
//  13. carry the amount in a uint64 instead of u128
//  14. report every history entry as confirmed regardless of getTransaction
//  15. leave the history in journal order instead of newest first
//  16. accept a journal entry with no transaction hash
