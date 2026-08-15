#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "agent_module_interface.h"

/**
 * @brief The Blockchain skills, over the LEZ wallet and the sequencer's RPC.
 *
 * WHAT THE CHAIN ACTUALLY OFFERS
 *
 * The sequencer answers exactly five methods — `sendTransaction`,
 * `getTransaction`, `getBlock`, `getAccount`, `getLastBlockId`. That short list
 * decides what each skill here can honestly claim:
 *
 * - `wallet.balance` has a real endpoint. `getAccount` takes a base58 account
 *   id and returns `{program_owner, balance, data, nonce}`, so a *public*
 *   balance is read straight off the chain. A *shielded* balance is not: a
 *   private account is a commitment in the private state, and `getAccount`
 *   answers for it with the default account — zero owner, zero nonce, zero
 *   balance — which is indistinguishable from an id nobody has ever used. So
 *   the shielded read goes through the wallet, which holds the viewing key,
 *   and the two paths are kept separate rather than one silently falling back
 *   to the other and reporting an empty record as "balance: 0".
 * - `wallet.send` has a real path, and it is not `sendTransaction`. It goes
 *   through the anchored policy program (see `SpendPort::spend` below).
 * - `wallet.history` has NO endpoint. There is no "transactions for account X"
 *   method, and there cannot easily be one: `getBlock` walks the chain by id
 *   and `getLastBlockId` gives the tip, so a full backwards scan is possible in
 *   principle but is O(chain) per call and *still* cannot attribute a shielded
 *   transfer to an account, because the notes are commitments. What the agent
 *   can report truthfully is its own submission journal — what this process
 *   recorded when it submitted — with each entry confirmed against the real
 *   `getTransaction`. Payments *received*, and anything submitted by another
 *   instance of the agent, are not in it, and the result says so.
 *
 * WHERE THE SPENDING LIMIT LIVES
 *
 * Not here. The owner's envelope is anchored on chain as a policy account whose
 * *address* is derived from the limits themselves (`crates/agent-policy-core`),
 * so raising a limit renames the account and the spend is rejected before the
 * program body runs. `wallet.send` therefore never builds a transfer itself; it
 * hands the spend to the same policy path the demo scripts drive, and the local
 * comparison against `SpendPolicy` is a courtesy — it avoids paying proving
 * cost for a transaction the chain would refuse, and it decides when to ask the
 * owner *before* spending anything. It is not the enforcer. An attacker holding
 * this process can delete every line of it and change nothing on chain.
 */
namespace logos::agent {

/// What the policy path reports back about one spend.
struct SpendReceipt {
    /// True only if a transaction was actually submitted.
    bool submitted = false;
    /// 32 bytes of hex. A submission without one is not a payment.
    std::string txHash;
    /// Why it was refused, passed through rather than reworded.
    std::string error;
};

/// One spend, named the way the policy program names it.
struct SpendRequest {
    /// Qualified account id, e.g. `Public/<base58>`.
    std::string recipient;
    unsigned __int128 amount = 0;
    /// What the agent has already moved in the current period. The program
    /// re-derives this rather than trusting it; it is passed so the local check
    /// and the on-chain check are computed over the same number.
    unsigned __int128 spentThisPeriod = 0;
};

/// What the skills need from the wallet and the chain. Injected so the skills
/// can be exercised against fakes, and so the agent module links neither the
/// wallet CLI nor an RPC client directly.
struct WalletPort {
    /// `getAccount(base58)` — the sequencer's raw answer, or empty if the node
    /// could not be reached. Public accounts only; see the note above.
    std::function<std::string(const std::string &accountId)> getAccount;
    /// The wallet's own view of an account it holds keys for, as JSON. This is
    /// the only way to read a shielded balance, and the implementation is
    /// expected to have synced the private notes first — a stale note set reads
    /// low and nothing here can tell.
    std::function<std::string(const std::string &qualifiedAccountId)> walletAccount;
    /// `getTransaction(txHash)` — the sequencer's raw answer, or empty if the
    /// node could not be reached. A dropped, a pending and a never-submitted
    /// hash are indistinguishable to it, which is why "not confirmed" is
    /// reported as such rather than as "failed".
    std::function<std::string(const std::string &txHash)> getTransaction;
    /// The agent's own record of what it has submitted, as a JSON array, oldest
    /// first. This exists because the chain has no history endpoint.
    std::function<std::string()> journal;
    /// Hand a spend to the anchored policy program. This is the whole point of
    /// the indirection: there is no path from `wallet.send` to a transfer that
    /// does not go through here.
    std::function<SpendReceipt(const SpendRequest &)> spend;
    /// How much the agent has already moved this period. Absent means *unknown*,
    /// which is not the same as zero — an unknown period total sends the spend
    /// to the owner rather than through the autonomous path.
    std::function<unsigned __int128()> spentThisPeriod;
};

/// How an above-threshold spend reaches the owner for approval.
///
/// Separate from `WalletPort` for the same reason `SharePort` is separate from
/// `StoragePort`: it is a messaging act, over the owner channel, and a failure
/// to reach the owner must be reported as that and not as a chain failure.
struct OwnerChannel {
    /// Deliver an approval request. False means the owner was not reached — and
    /// the prize is explicit that such a spend must not execute.
    std::function<bool(const std::string &requestJson)> requestApproval;
    /// A fresh nonce for the spend being approved. Without one, an approval for
    /// "send 100 to Bob" authorises the same transfer forever, so a missing
    /// nonce source is a refusal rather than a zero.
    std::function<std::uint64_t()> nextNonce;
};

/// `wallet.balance()` — optionally `{"account": "<id>"}` to read another one.
class WalletBalanceSkill final : public ISkill {
public:
    WalletBalanceSkill(WalletPort port, std::string agentAccount)
        : port_(std::move(port)), agentAccount_(std::move(agentAccount)) {}
    std::string name() const override { return "wallet.balance"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    WalletPort port_;
    /// Qualified, e.g. `Private/<base58>`: which read path to use is part of
    /// the account's identity, not a guess made at call time.
    std::string agentAccount_;
};

/// `wallet.send(recipient, amount)` — routed through the anchored policy path,
/// held for the owner when it falls outside the envelope.
class WalletSendSkill final : public ISkill {
public:
    WalletSendSkill(WalletPort port, OwnerChannel owner, SpendPolicy policy)
        : port_(std::move(port)), owner_(std::move(owner)), policy_(policy) {}
    std::string name() const override { return "wallet.send"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    WalletPort port_;
    OwnerChannel owner_;
    /// A cache of an on-chain fact, never the source of it.
    SpendPolicy policy_;
};

/// `wallet.history()` — optionally `{"limit": n}`, newest first.
class WalletHistorySkill final : public ISkill {
public:
    explicit WalletHistorySkill(WalletPort port) : port_(std::move(port)) {}
    std::string name() const override { return "wallet.history"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    WalletPort port_;
};

} // namespace logos::agent
