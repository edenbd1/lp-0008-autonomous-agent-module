#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Durable storage and recovery for the A2A task lifecycle.
 *
 * The prize's Reliability criterion asks that the module "recovers from
 * transient failures (network interruptions, node restarts) without losing
 * pending task state". `TaskStore` already owns the model and already knows how
 * to render itself (`snapshot()`) and to be replaced from that rendering
 * (`restore()`). What it does not have is anywhere to put the rendering, and a
 * store that only ever lives in memory loses every pending task on the first
 * restart.
 *
 * So this file is deliberately *not* a second task engine. It is a file format,
 * an atomic write, and a recovery decision. `TaskStore` stays the authority on
 * what a task is and which transitions A2A allows; it is reached here through
 * @ref TaskStorePort, two `std::function`s, in the same way the storage skills
 * reach Logos Storage.
 *
 * Three things a restart must not do, and how each is prevented:
 *
 * 1. **It must not pay twice.** A settlement is irreversible: if recovery
 *    re-sends a payment for a task that was already paid, that is real money
 *    gone. The dangerous window is not the one people expect. Between
 *    `TaskPort::pay()` returning a transaction hash and `TaskStore::recordPayment`
 *    storing it there is an interval in which the money has moved and nothing
 *    on disk says so — a snapshot taken there loads as *unpaid*. A snapshot
 *    alone cannot close that window, because the fact to be recorded does not
 *    exist yet when the snapshot is taken. What closes it is a write-ahead
 *    record: @ref TaskPersistence::notePaymentIntent is made durable *before*
 *    the wallet is called, and on recovery an intent with no settlement makes
 *    @ref TaskPersistence::mayPay refuse until a human reconciles it on chain.
 *    Refusing to pay a task that was in fact never paid costs a stalled task.
 *    Paying one that was costs the money.
 *
 * 2. **It must not resurrect a finished task.** Completed, canceled, failed and
 *    rejected are terminal in A2A. They are written as terminal, they are read
 *    back as terminal, and `mayPay` refuses them outright.
 *
 * 3. **It must not read a half-written file as an empty one.** A file written
 *    by a process that died mid-write is not "no tasks"; it is "we do not know".
 *    Those two have to be distinguishable, because starting with an empty task
 *    list is exactly how a paid task gets paid again. So the write is atomic
 *    (temp file, flushed to the medium, renamed over the target, parent
 *    directory flushed) and the read has three outcomes, not two:
 *    @ref LoadOutcome::Loaded, @ref LoadOutcome::Absent, @ref LoadOutcome::Failed.
 *    Truncated, malformed, oversized, wrong-kind, wrong-schema and
 *    checksum-mismatched files are all `Failed`, never `Absent`, and `Failed`
 *    leaves the store exactly as it was.
 *
 * **The file holds account ids and task metadata. It never holds keys.** Not by
 * convention: @ref projectPersistableTasks is an allowlist, and a field this
 * build does not name does not reach the disk even if a future `TaskStore`
 * starts carrying one.
 *
 * The file is *not* authenticated. The checksum detects a truncated or
 * corrupted file; it does not detect a deliberately rewritten one, because
 * anyone who can write the file can recompute it. Anything that would be a
 * disaster in the hands of whoever can write this file — a key, a signature —
 * therefore does not go in it.
 */
namespace logos::agent {

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

/// Written into every snapshot and required to match on read.
///
/// A newer schema is refused rather than best-effort parsed. A build that
/// silently drops the fields a later build added would write the file back
/// without them, and the downgrade would be permanent and invisible.
constexpr int kSnapshotSchema = 1;

/// Names the file for what it is, so a file that happens to be JSON but is not
/// ours is refused instead of parsed into an empty task list.
extern const char *const kSnapshotKind;

/// Ceiling on the snapshot, read and written. A `note` arrives from the wire —
/// it is the peer's `status.message` — so the size of this file is partly under
/// a peer's control, and durable storage with no bound is a way to fill a disk.
constexpr std::size_t kMaxSnapshotBytes = 16u * 1024u * 1024u;

/// Ceiling on one task's `note`, applied when writing. A note longer than this
/// is cut and the persisted task carries `"noteTruncated":true` so the loss is
/// visible rather than inferred.
constexpr std::size_t kMaxNoteBytes = 4096;

/// The task fields that are allowed to reach the disk, and the only ones that
/// do. Exposed so a test can assert the list rather than trust it.
const std::vector<std::string> &persistableTaskFields();

/// Whether `name` is one of A2A's four terminal states.
///
/// A mirror of `isTerminalState()` in `agent_skills.h`, kept deliberately: this
/// translation unit does not link the task engine, so that a snapshot can be
/// written and recovered without dragging the whole skill layer in. The four
/// names are A2A's, fixed by the protocol rather than by us.
bool isTerminalStateName(const std::string &state);

/// Whether `name` is one of the nine `TaskState` names A2A defines.
bool isKnownStateName(const std::string &state);

/// Re-render a `TaskStore::snapshot()` array with only @ref persistableTaskFields
/// on each task, notes cut to @ref kMaxNoteBytes, and every task checked for the
/// fields recovery needs (`id`, `agent`, `skill` non-empty, `state` one A2A
/// defines).
///
/// Returns the array as JSON text, or an empty string with `err` set. A task
/// carrying a field this build does not name — `iskSecret`, say, added upstream
/// after this file was written — loses it here rather than on disk.
std::string projectPersistableTasks(const std::string &snapshotJson, std::string &err);

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

/// The task engine this persists.
///
/// Two functions, because `TaskStore` already has both. Wiring is one line
/// each:
/// @code
///   TaskStorePort port;
///   port.snapshot = [&store] { return store.snapshot(); };
///   port.restore  = [&store](const std::string &j, std::string &e) {
///       return store.restore(j, e);
///   };
/// @endcode
/// `restore` must leave the store untouched when it refuses — `TaskStore`'s
/// does, building into a scratch map first — because a `Failed` load promises
/// the caller that nothing moved.
struct TaskStorePort {
    std::function<std::string()> snapshot;
    std::function<bool(const std::string &snapshotJson, std::string &err)> restore;
};

/// The filesystem, at the granularity durability actually needs.
///
/// Split this way — not as a single `writeFile` — because the ordering is the
/// guarantee. A test can make the write fail and then assert that no rename
/// happened, which is the difference between "the save failed" and "the save
/// failed and took the previous snapshot with it".
struct SnapshotFilePort {
    /// Read the whole file.
    ///
    /// True with `out` filled when it was read. False with `absent` true when
    /// there is no such file. False with `absent` false and `err` set when the
    /// file is there and could not be read — a permission error is not an empty
    /// task list, and conflating the two is the failure this whole file exists
    /// to prevent.
    std::function<bool(const std::string &path, std::string &out, bool &absent,
                       std::string &err)>
        read;

    /// Create or truncate `path`, write `bytes`, **flush them to the medium**,
    /// and close. The flush is not optional: a rename over a file whose
    /// contents are still in the page cache leaves, after a power loss, a
    /// correctly named file full of nothing.
    std::function<bool(const std::string &path, const std::string &bytes, std::string &err)>
        writeSync;

    /// Rename `from` over `to`. Must be atomic — same filesystem — so a reader
    /// sees either the old snapshot or the new one and never a partial file.
    std::function<bool(const std::string &from, const std::string &to, std::string &err)> rename;

    /// Flush the directory holding `path`, so the rename itself survives.
    std::function<bool(const std::string &path, std::string &err)> syncParent;

    /// Best-effort removal of the temp file a failed write left behind.
    std::function<void(const std::string &path)> remove;
};

/// A @ref SnapshotFilePort against the real filesystem: `open`/`write`/`fsync`,
/// `rename`, and `fsync` on the parent directory.
SnapshotFilePort posixSnapshotFilePort();

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

/// What a @ref TaskPersistence::load found. Three outcomes, because two would
/// force "could not read it" to be reported as one of the other two.
enum class LoadOutcome {
    /// The file was there, intact, this schema, and the store now holds it.
    Loaded,
    /// There is no file. A genuinely first run: starting with no tasks is
    /// correct here and *only* here.
    Absent,
    /// A file exists and cannot be trusted — unreadable, truncated, malformed,
    /// oversized, not ours, a schema this build does not understand, a checksum
    /// that does not match, or a payload the task engine refused. The store is
    /// untouched.
    Failed,
};

struct LoadReport {
    LoadOutcome outcome = LoadOutcome::Failed;
    /// Why, when the outcome is `Failed`. Empty otherwise.
    std::string error;
    std::size_t tasks = 0;
    std::size_t active = 0;
    /// Recovered tasks with a settlement on record. These must never be paid
    /// again.
    std::size_t settledPayments = 0;
    /// Recovered tasks with a payment intent and no settlement: the process died
    /// somewhere around the wallet call and only the chain knows which side.
    /// Each one needs a human to look before the task moves.
    std::size_t uncertainPayments = 0;

    bool ok() const { return outcome == LoadOutcome::Loaded; }

    /// Whether the agent may proceed with an empty task list.
    ///
    /// True only for `Absent`. The point of the method is that there is no way
    /// to write `if (!report.ok())` and accidentally treat a failed read as a
    /// fresh start.
    bool safeToStartEmpty() const { return outcome == LoadOutcome::Absent; }
};

/// What the durable record says about one task's payment.
enum class PaymentRecord {
    /// Nothing was ever written. Either the task is free or it has not reached
    /// the wallet.
    None,
    /// A payment was journalled as about to be attempted and no settlement
    /// followed. Unresolved: the money may or may not have moved.
    Intended,
    /// A settlement transaction is on record. The money moved.
    Settled,
    /// An intent that a human reconciled against the chain and declared did not
    /// settle. The only way back out of `Intended`.
    Abandoned,
};

const char *paymentRecordName(PaymentRecord record);

/// One task as recovery found it.
struct RecoveredTask {
    std::string id;
    std::string contextId;
    std::string agent;
    std::string skill;
    /// The A2A state name, e.g. `input-required`.
    std::string state;
    bool terminal = false;
    std::uint64_t pricePaid = 0;
    std::string payAccount;
    std::string settlementTx;
    PaymentRecord payment = PaymentRecord::None;
};

// ---------------------------------------------------------------------------
// TaskPersistence
// ---------------------------------------------------------------------------

/**
 * @brief One snapshot file, written atomically, plus the payment journal that
 *        makes recovery safe to act on.
 *
 * Intended use, in order:
 * @code
 *   TaskPersistence durable(storePort, "/var/lib/lp0008/tasks.json",
 *                           posixSnapshotFilePort());
 *   const LoadReport r = durable.load();
 *   if (!r.ok() && !r.safeToStartEmpty()) {
 *       // Refuse to start. Do not carry on with an empty store.
 *   }
 *   // ... before every wallet call for a priced task:
 *   std::string why;
 *   if (!durable.mayPay(taskId, why)) {
 *       // Report why. Do not pay.
 *   }
 *   if (!durable.notePaymentIntent(taskId, payAccount, price, err)) {
 *       // The intent is not durable. Do not pay.
 *   }
 *   const std::string tx = pay(payAccount, price);
 *   if (!tx.empty()) durable.notePaymentSettled(taskId, tx, err);
 *   // and after any lifecycle change:
 *   durable.save(err);
 * @endcode
 *
 * Locked, for the same reason `TaskStore` is: skills are served from more than
 * one thread. Single-writer per path — the temp file is derived from the path,
 * so two processes saving to one file would fight over it. One agent, one file.
 */
class TaskPersistence {
public:
    TaskPersistence(TaskStorePort store, std::string path, SnapshotFilePort file,
                    std::size_t maxBytes = kMaxSnapshotBytes);

    const std::string &path() const { return path_; }
    /// `path()` + `.tmp`, where a save lands before it is renamed into place.
    std::string tempPath() const { return path_ + ".tmp"; }

    /// Snapshot the store and write it: temp file, flush, rename, flush parent.
    ///
    /// Never writes the target in place. A failure at any step leaves the
    /// previous snapshot exactly as it was and removes the temp file, so the
    /// worst outcome of a failed save is a stale snapshot — never a destroyed
    /// one.
    bool save(std::string &err);

    /// Read the file and, on success, replace the store's contents with it.
    ///
    /// The store is untouched unless the outcome is `Loaded`.
    LoadReport load();

    /// The outcome of the last @ref load, or `Failed` if none has run — the
    /// safe reading of "we have not looked".
    LoadOutcome lastOutcome() const;
    bool recoveryRan() const;

    /// The tasks the last successful @ref load found.
    std::vector<RecoveredTask> recovered() const;

    /// Recovered tasks whose payment is `Intended`: money that may or may not
    /// have moved. Report these to the owner; do not move them on automatically.
    std::vector<RecoveredTask> needingReconciliation() const;

    /// What the durable record says about `taskId`.
    PaymentRecord paymentState(const std::string &taskId) const;

    /// **Journal that a payment is about to be attempted, before attempting it.**
    ///
    /// Made durable immediately: this returns false if the snapshot could not be
    /// written, and a caller that pays anyway has thrown away the only record
    /// that would stop the payment being repeated after a crash. If this returns
    /// false, do not call the wallet.
    ///
    /// Refuses an amount of zero — a free task has nothing to journal — and
    /// refuses a task that already has a settlement on record.
    bool notePaymentIntent(const std::string &taskId, const std::string &payAccount,
                           std::uint64_t amount, std::string &err);

    /// Record the settlement, once the wallet has returned a transaction hash.
    /// Refuses an empty hash: an amount with no transaction is not a payment,
    /// the same standard `TaskStore::recordPayment` applies.
    bool notePaymentSettled(const std::string &taskId, const std::string &settlementTx,
                            std::string &err);

    /// Close out an unresolved intent that a human has checked against the chain
    /// and found did not settle.
    ///
    /// `evidence` is required and non-empty — what was looked at, e.g. the
    /// account and the block range — because this is the one call that makes
    /// @ref mayPay say yes again for a task that once reached the wallet. It is
    /// deliberately not something the agent can do to itself: "the transaction
    /// hash came back empty" is not evidence that nothing settled, only that
    /// nothing was seen to.
    bool notePaymentAbandoned(const std::string &taskId, const std::string &evidence,
                              std::string &err);

    /// **Whether recovery may send a payment for `taskId`.**
    ///
    /// The guard on the money. False, with `why` set, when:
    /// - recovery has not run — nothing is known, so nothing is spent;
    /// - the last load failed — the durable record could not be read, and an
    ///   unreadable record is not an absent one;
    /// - a settlement is on record — this is the double payment;
    /// - a payment intent is unresolved — the process died around the wallet
    ///   call and only the chain knows;
    /// - the recovered task is in a terminal state — finished work is not
    ///   re-bought.
    ///
    /// True for a task the durable record has never heard of, once a load has
    /// completed cleanly: a `Loaded` or `Absent` file that does not mention an
    /// id is positive evidence that no payment for it was ever begun.
    bool mayPay(const std::string &taskId, std::string &why) const;

private:
    struct JournalEntry {
        std::string payAccount;
        std::uint64_t amount = 0;
        PaymentRecord state = PaymentRecord::None;
        std::string settlementTx;
        std::string evidence;
    };

    bool saveLocked(std::string &err);
    bool mayPayLocked(const std::string &taskId, std::string &why) const;

    mutable std::mutex mutex_;
    TaskStorePort store_;
    std::string path_;
    SnapshotFilePort file_;
    std::size_t maxBytes_;

    bool recoveryRan_ = false;
    LoadOutcome last_ = LoadOutcome::Failed;
    std::vector<RecoveredTask> recovered_;
    std::map<std::string, bool> terminalById_;
    std::map<std::string, JournalEntry> journal_;
};

} // namespace logos::agent
