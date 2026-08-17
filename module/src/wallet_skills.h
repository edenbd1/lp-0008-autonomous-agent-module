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
 *   the shielded read goes through the wallet, which holds the viewing key, and
 *   the two paths are kept apart rather than one silently falling back to the
 *   other and reporting an empty record as "balance: 0".
 * - `wallet.send` has a real path, and it is not `sendTransaction`. It goes
 *   through the anchored policy program (see `WalletPort::spend`).
 * - `wallet.history` has NO endpoint. There is no "transactions for account X"
 *   method, and there cannot easily be one: `getBlock` walks the chain by id
 *   and `getLastBlockId` gives the tip, so a full backwards scan is possible in
 *   principle but is O(chain) per call and *still* cannot attribute a shielded
 *   transfer to an account, because the notes are commitments. What the agent
 *   can report truthfully is its own submission journal — what this process
 *   recorded when it submitted — with each entry confirmed against the real
 *   `getTransaction`. Payments *received*, and anything submitted by another
 *   instance of the agent, are not in it, and the result says so rather than
 *   presenting a partial list as a complete one.
 *
 * WHERE THE SPENDING LIMIT LIVES
 *
 * Not here. The owner's envelope is anchored on chain as a policy account whose
 * *address* is derived from the limits themselves (`crates/agent-policy-core`),
 * so raising a limit renames the account and the spend is rejected before the
 * program body runs. `wallet.send` never builds a transfer itself; it hands the
 * spend to the same policy path the demo scripts drive, and the comparison
 * against `SpendEnvelope` below is a *courtesy* — it avoids paying proving cost
 * for a transaction the chain would refuse, and it decides when to ask the owner
 * before spending anything. It is not the enforcer. An attacker holding this
 * process can delete every line of it and change nothing on chain.
 *
 * WHY AMOUNTS ARE DECIMAL STRINGS
 *
 * On-chain amounts are u128 and C++17 has no portable spelling for one —
 * `unsigned __int128` is a GCC/Clang extension, absent on MSVC and on 32-bit
 * targets, and this is a header third parties are told to include. A `double`
 * would silently round a payment. So amounts travel as decimal digits and the
 * arithmetic this file needs (one comparison, one addition) is done on them,
 * which also removes any question of a period total wrapping around.
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
    /// Base units, in decimal.
    std::string amount;
    /// What the agent has already moved in the current period, in decimal. The
    /// program re-derives this rather than trusting it; it is passed so the
    /// local check and the on-chain check are computed over the same number.
    std::string spentThisPeriod;
};

/// One spend the owner has approved, named the way the policy program's
/// `spend_approved` instruction names it.
///
/// Four fields and no fifth, because these are exactly what the approval is
/// bound to. The owner's `approve_spend` creates a marker account at a PDA
/// seeded by `SHA-256(agent ‖ recipient ‖ amount ‖ nonce)` — see
/// `module/src/spend_marker.h` and `crates/agent-policy-core` — so the agent
/// cannot present terms other than the ones it asked about: a different amount
/// or a different recipient derives a different account, and no such account
/// exists. The agent's own id is not here because it is not the caller's to
/// choose; whatever submits this supplies it, and supplying it from the request
/// would let a caller ask for an approval belonging to another agent.
///
/// **The marker is single-use.** `spend_approved` sets its `spent` byte as it
/// consumes it, so this request cannot be replayed — which is also why nothing
/// here retries it. A second attempt at the same terms would need a second
/// nonce, and a second nonce is a second approval, which only the owner can
/// give.
struct ApprovedSpendRequest {
    /// Qualified account id, e.g. `Public/<base58>` — the same string the owner
    /// was asked about, not a re-derivation of it.
    std::string recipient;
    /// Base units, in decimal, as approved.
    std::string amount;
    /// The nonce this approval was minted under. Half of what binds an approval
    /// to one payment: without it, "send 100 to Bob" approved once authorises
    /// the same transfer forever.
    std::uint64_t nonce = 0;
    /// The correlation id the exchange ran under, for the operator's logs. It
    /// is not part of the on-chain derivation.
    std::string requestId;
};

/// A display mirror of the anchored envelope: a cache of an on-chain fact, never
/// the source of it.
///
/// Deliberately declared here and not in `agent_module_interface.h`. A type in
/// the shared interface header reads as part of the module's contract, and this
/// is not one — nothing outside `wallet.send` may treat these numbers as a
/// permission. Limits are decimal strings; an empty or malformed one means the
/// envelope is unknown, which sends every spend to the owner.
struct SpendEnvelope {
    std::string perTx;
    std::string perPeriod;
    std::uint64_t periodBlocks = 0;
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
    /// reported as such and never as "failed".
    std::function<std::string(const std::string &txHash)> getTransaction;
    /// The agent's own record of what it has submitted, as a JSON array, oldest
    /// first. It exists because the chain has no history endpoint.
    std::function<std::string()> journal;
    /// Hand a spend to the anchored policy program. This is the whole point of
    /// the indirection: there is no path from `wallet.send` to a transfer that
    /// does not go through here.
    std::function<SpendReceipt(const SpendRequest &)> spend;
    /// **The other half of the same program, and the one an approval unlocks.**
    ///
    /// `spend` is the autonomous instruction; the chain refuses it outright for
    /// an amount above the anchored per-transaction ceiling — "the spend needs
    /// an owner approval: use spend_approved" — so an approved above-envelope
    /// payment cannot be put through it, and putting it through anyway would
    /// report a settlement the chain had rejected. This is the instruction that
    /// carries one, and it is reached only after the owner has answered
    /// `approved` for these exact terms.
    ///
    /// Unwired means this deployment has no way to submit an approved spend,
    /// and `wallet.send` then reports the approval and submits nothing — which
    /// is what it did for every deployment before this field existed. It is
    /// deliberately separate from `spend` rather than a flag on it: a
    /// submitter that does not know about approvals must not be handed an
    /// approved payment and quietly run the autonomous instruction with it,
    /// which would meter it against the per-period budget and leave the
    /// owner's marker unspent and redeemable.
    std::function<SpendReceipt(const ApprovedSpendRequest &)> spendApproved;
    /// How much the agent has already moved this period, in decimal. An absent
    /// function, or an empty answer, means *unknown* — which is not zero, and
    /// sends the spend to the owner rather than through the autonomous path.
    std::function<std::string()> spentThisPeriod;
};

/// How long an above-threshold spend waits for its owner before the agent gives
/// up on it. Terminal: the wait never falls back to spending unattended.
constexpr std::int64_t kDefaultApprovalTimeoutMs = 120000;

/// How often the same request — same id, same terms — is put back on the wire
/// while that wait runs. "The agent retries notification before timing out."
constexpr std::int64_t kDefaultApprovalResendMs = 15000;

/// How many consecutive passes of the approval wait may read the *same* clock
/// value before the agent concludes the clock is stuck and stops.
///
/// Not a policy — a guard against a clock that does not advance. `nowMs` is
/// injected, so a fake that returns a constant (or a monotonic source that has
/// stopped) makes `now >= deadline` false forever, and the result is a hang
/// *inside a skill call*, which from the host's side is indistinguishable from
/// the registry deadlock this module was restructured to avoid.
///
/// Counted as *consecutive passes at one clock reading*, not as a total pass
/// count, and the difference matters: a host that wires no `idle` spins, so a
/// 120-second wait on a real clock runs millions of passes and any total cap
/// would fire on it and report a stuck clock that is running perfectly. The
/// counter resets the moment the clock moves, so only a clock that has genuinely
/// stopped trips it — and that is reported as the stuck clock it is, never as a
/// deadline that passed.
constexpr int kMaxApprovalPollsAtOneInstant = 100000;

/// How an above-threshold spend reaches the owner for approval, and how the
/// answer — or the silence — comes back.
///
/// Separate from `WalletPort` for the same reason `SharePort` is separate from
/// `StoragePort`: it is a messaging act, over the owner channel, and a failure
/// to reach the owner must be reported as that and not as a chain failure.
///
/// A *port*, and named one. `owner_channel.h` declares the class that satisfies
/// it — `OwnerChannel`, the correlated retrying implementation — and this used to
/// carry that same name in the same namespace, so the interface and the thing
/// that implements it could not be included in one translation unit. That is the
/// wiring this port exists for, so the collision would have surfaced exactly when
/// somebody first tried to do it.
///
/// WHY THE RETRY IS HERE AND NOT ONLY IN `OwnerChannel`
///
/// The prize asks that "above-threshold transactions that fail to reach the
/// owner for approval are not executed — the agent retries notification before
/// timing out and reports the failure". All three clauses are decided on this
/// path, and this path is what ships: `OwnerChannel` implements the same
/// discipline over Logos Delivery, and it is constructed by a host that links
/// this module, never by a host that *loads* it as a plugin — a port is a
/// `std::function` and there is no wire format for one. A module loaded into
/// Basecamp with the retry living only in `OwnerChannel` therefore retries
/// nothing, which was measured: one notification attempt, no wait, no timeout
/// and no report. So the discipline is here, where `wallet.send` is, and any
/// transport wired underneath inherits it.
struct OwnerApprovalPort {
    /// **One** notification attempt. False means this attempt did not reach the
    /// owner; the wait below tries again until the deadline. It is not the
    /// verdict — a delivered request is not an approved one.
    std::function<bool(const std::string &requestJson)> requestApproval;
    /// A fresh nonce for the spend being approved. Without one, an approval for
    /// "send 100 to Bob" authorises the same transfer forever, so a missing
    /// nonce source is a refusal rather than a zero.
    std::function<std::uint64_t()> nextNonce;

    /// The owner's answer to `requestId`, if one has arrived yet: `"approved"`,
    /// `"denied"`, or empty for "nothing yet". Polled rather than pushed so the
    /// answer can arrive on any transport — a Delivery channel, a module method,
    /// a host's own console — without this file naming one.
    ///
    /// **Absent means this deployment has no path by which an answer could
    /// arrive at all**, which is a different thing from an owner who is slow.
    /// The agent then does not pretend to wait: it notifies once and says
    /// `answer_path:false`, because a wait for an answer that has no route here
    /// is theatre, and reporting `owner_unreachable` after it would blame an
    /// owner who was never given a way to reply. Nothing is submitted either
    /// way.
    std::function<std::string(const std::string &requestId)> pollDecision;

    /// Monotonic milliseconds. Injected so a timeout is testable without
    /// sleeping through one — the same reason `OwnerChannelPort` injects it.
    /// Absent means the agent cannot measure a deadline, so it does not claim
    /// one: same treatment as an absent `pollDecision`.
    std::function<std::int64_t()> nowMs;
    /// Called between polls so waiting does not spin a core. Optional; a fake
    /// clock leaves it empty and a real adapter sleeps a little.
    std::function<void()> idle;

    /// The deadline and the resend interval, as functions rather than plain
    /// numbers, so a host can move them while the agent runs — `meta.configure`
    /// does exactly that. Empty falls back to @ref kDefaultApprovalTimeoutMs and
    /// @ref kDefaultApprovalResendMs.
    std::function<std::int64_t()> timeoutMs;
    std::function<std::int64_t()> resendIntervalMs;
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
    /// Qualified, e.g. `Private/<base58>`: which read path an account needs is
    /// part of its identity, not a guess made at call time.
    std::string agentAccount_;
};

/// `wallet.send(recipient, amount)` — routed through the anchored policy path,
/// held for the owner when it falls outside the envelope.
class WalletSendSkill final : public ISkill {
public:
    WalletSendSkill(WalletPort port, OwnerApprovalPort owner, SpendEnvelope envelope)
        : port_(std::move(port)), owner_(std::move(owner)), envelope_(std::move(envelope)) {}
    std::string name() const override { return "wallet.send"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    WalletPort port_;
    OwnerApprovalPort owner_;
    SpendEnvelope envelope_;
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
