#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent_module_interface.h"

/**
 * @brief The Storage skills, over Logos Storage.
 *
 * Like Delivery, Storage is a lifecycle: `init` from a JSON config, then
 * `start`, and neither upload nor download means anything before that. Uploads
 * are content-addressed — `uploadUrl` returns a CID — so `storage.share` has
 * nothing to copy: sharing is sending the address to someone, which is a
 * messaging act, not a storage one.
 */
namespace logos::agent {

/// What the skills need from the storage module. Named so they can be exercised
/// against a fake, and so the agent module does not link Storage directly.
struct StoragePort {
    std::function<bool()> ready;
    /// `uploadUrl(filePath, chunkSize)` — returns the content address, or empty.
    std::function<std::string(const std::string &path, std::int64_t chunkSize)> upload;
    /// `downloadToUrl(cid, filePath, local, chunkSize)`
    std::function<bool(const std::string &cid, const std::string &path)> download;
    /// `manifests()` — the JSON the module returns, passed through.
    std::function<std::string()> manifests;
    /// `exists(cid)`
    std::function<bool(const std::string &cid)> exists;
    /// The node's data directory. The skills keep the vault key and the
    /// content-address to label map here, beside the node's own storage, so both
    /// survive a restart. Empty means "not wired" — the skills then refuse to
    /// encrypt rather than fall back to plaintext.
    std::function<std::string()> dataDir;
};

/// How a shared address reaches its recipient. Sharing a content address is a
/// messaging act; keeping it separate is what lets `storage.share` be honest
/// about which half failed.
struct SharePort {
    std::function<bool(const std::string &recipient, const std::string &message)> send;
};

/**
 * @brief A Logos Storage node the module brings up and owns ITSELF.
 *
 * WHY THIS EXISTS, AND WHAT IT CLOSES
 *
 * `StoragePort` above is a struct of `std::function`s a *host* fills in. That is
 * enough for a host which LINKS the module and is nothing at all for a host
 * which LOADS it: Logos Core runs a core module in its own `logos_host` process
 * and reaches it over Qt Remote Objects, and there is no wire format for a
 * closure. So `AgentModuleImpl::installBuiltinSkills` consumed `ports.storage`
 * verbatim, a loaded plugin was handed `SkillPorts{}`, and all four `storage.*`
 * skills answered `"storage node is not started"` in **every shipped
 * configuration** — not because the node was down but because there was no port
 * to reach one through, and nothing in `module/src` could open one.
 *
 * `DeliveryRuntime` had already made the argument that settles this, and it was
 * left unapplied here for one release: *a host being unable to PASS a port is
 * not the module being unable to CONSTRUCT one*. The port is a set of function
 * objects; the module can build them out of a node it opened from its own
 * configuration, and then nothing crosses the boundary except the
 * `meta.configure("storage","on")` that says go — two strings, which Qt Remote
 * Objects has carried since the beginning. Storage is the same shape as
 * Delivery, so it gets the same answer.
 *
 * THREE DIFFERENCES FROM `DeliveryRuntime`, EACH DELIBERATE
 *
 * 1. **No `#ifdef`, and therefore no build-system input.** `liblogosdelivery.h`
 *    defines request *structs*, so the delivery path cannot compile without the
 *    header and lives behind `LP0008_WITH_DELIVERY`. `libstorage.h` defines no
 *    types at all beyond a callback `typedef` — every entry point takes
 *    `void *`, `const char *`, `size_t` and `bool` — so the eleven prototypes
 *    this file needs are declared here and the header is not a build input. The
 *    storage code path is therefore compiled into *every* variant identically,
 *    which is what `scripts/check-package-fresh.py` wants (it requires every
 *    source literal of >= 8 bytes to be inside every shipped binary, and a
 *    literal in a preprocessor branch that was not compiled can never satisfy
 *    it), and it is why there is no `absent` state below: this build always
 *    knows how to open a Storage node. Whether the *library* is there is a
 *    separate question with a separate answer, which is @ref lastError.
 *
 * 2. **Every call is serialised.** The library's callbacks carry NO correlation
 *    token, so a late reply to a timed-out call satisfies the next wait —
 *    `module/tests/storage_node_drive.c` records paying for that discovery, and
 *    there the calls are sequential by construction. Here they are not:
 *    `AgentModuleImpl::invoke` is reachable from several threads at once
 *    (`module/tests/concurrent_skills_test.cpp` drives exactly that), so two
 *    concurrent `storage.upload`s would otherwise cross their session ids and
 *    one would return the other's content address. Every entry point below
 *    holds one mutex across its whole arm-call-wait sequence, and `upload`
 *    holds it across *both* of its calls, because the session id from
 *    `upload_init` is only meaningful to the `upload_file` that follows it.
 *
 * 3. **A data directory is required and is never invented.** A Storage node is
 *    a repository on disk. @ref bringUp refuses with a sentence naming what to
 *    set rather than picking a directory on an operator's behalf.
 *
 * LIFECYCLE, AND WHY NOTHING STARTS ON ITS OWN
 *
 * Constructed unstarted, exactly as `DeliveryRuntime` is. Loading this module
 * opens no repository and takes no lock on one until an operator asks; until
 * then every storage skill keeps refusing with the message it already had —
 * `"storage node is not started"` — which is true the entire time it is given.
 * @ref bringUp does the work on a thread of its own and returns immediately,
 * because `storage_new` plus `storage_start` is seconds and a `meta.configure`
 * that blocked for that long would time out on the runtime's transport and the
 * caller would be told the module was broken.
 */
class StorageRuntime {
public:
    /// Whether this build knows how to reach `libstorage` at all.
    ///
    /// Always true, and it is a method rather than a constant so that
    /// `meta.status` can report the same shape for both transports. See
    /// difference 1 above: there is no build of this module that lacks the
    /// storage code path, so there is no `absent` state to report — a library
    /// that could not be opened is a `failed` carrying every path that was
    /// tried, which is a different and more useful answer.
    static bool linkedIn();

    StorageRuntime();
    ~StorageRuntime();

    StorageRuntime(const StorageRuntime &) = delete;
    StorageRuntime &operator=(const StorageRuntime &) = delete;

    /// What to do between polls while a call into the library is outstanding.
    ///
    /// The same hazard `DeliveryRuntime::setIdle` documents and the same fix:
    /// every call here is reached from `invoke()`, which in a loaded module runs
    /// on the `logos_host` event loop that also dispatches everything this
    /// module emits, and a wait that *sleeps* on that thread queues behind
    /// itself every event the module is publishing. Left unset, the wait sleeps,
    /// which is right for every caller that has no event loop to pump.
    void setIdle(std::function<void()> idle);

    /// Where the node keeps its repository. Must be set before @ref bringUp.
    ///
    /// **Normalised, and that is not tidiness.** `$TMPDIR` on macOS ends in a
    /// slash, so a caller composing `$TMPDIR/store` hands the node a path
    /// containing `//` — and every upload then fails with `Path is outside of
    /// 'root' directory!`, because the datastore keeps the root exactly as given
    /// and compares it against a block path that has been normalised. That is a
    /// node which starts, reports a peer id, answers every query and refuses
    /// every write, which reads from outside as the *skill* being broken.
    /// `scripts/skills-live.sh` reproduced it and paid for it; collapsing
    /// repeated slashes and dropping a trailing one costs four lines here and
    /// makes the trap unreachable from configuration.
    void setDataDir(const std::string &dir);
    std::string dataDir() const;

    /// Start bringing the node up, in the background. Idempotent: a second call
    /// while one is in flight is accepted and does nothing.
    ///
    /// Returns false, with `error` set, only when it cannot even begin — which
    /// today means no data directory has been configured. A library that cannot
    /// be opened is *not* this: it is reported asynchronously, as a `failed`
    /// state carrying every path that was tried, for the same reason
    /// `DeliveryRuntime` reports it that way — an operator needs to be told
    /// which file was missing, not merely that something did not work.
    bool bringUp(std::string &error);

    /// Stop, close and destroy the node. Safe to call when nothing was started.
    ///
    /// All three, in that order, because `libstorage.h` requires exactly that
    /// before `storage_destroy` — and a leaked running node holds a LevelDB lock
    /// on the repository directory, so the next `bringUp` on the same directory
    /// would fail for a reason that has nothing to do with the caller.
    void shutDown();

    /// `"off"`, `"starting"`, `"ready"` or `"failed"`.
    std::string state() const;
    std::string lastError() const;
    bool ready() const;

    /// The whole of the above as JSON, for `meta.status`:
    /// `{"state":"ready","linked":true,"dataDir":"…","peerId":"16Uiu2…"}`, with
    /// an `"error"` when the node could not be opened.
    ///
    /// `peerId` is there because it is the one field a mock cannot produce: a
    /// state of `ready` is this module's own bookkeeping, and a libp2p peer
    /// identity is the node's. It is absent until the node is up and absent
    /// again once it is put down.
    std::string statusJson() const;

    /// The content address of `path`, or empty. A session — `upload_init` then
    /// `upload_file` — held under one lock; see difference 2 above.
    std::string upload(const std::string &path, std::int64_t chunkSize);
    /// Write the content of `cid` to `path`. **Local store only**: see
    /// @ref exists, and `docs/skills.md`'s note on what that does not cover.
    bool download(const std::string &cid, const std::string &path);
    /// The node's manifest list, as the JSON it returns, passed through.
    std::string manifests();
    /// Whether the node holds `cid` **in its local store**. This is a datastore
    /// key lookup, not a network question.
    bool exists(const std::string &cid);

    /// The port, built out of this node. What a host could not hand over.
    StoragePort storagePort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    /// @see setIdle. Set once, before the node is asked for.
    std::function<void()> idle_;
};

/// `storage.upload(path, label)`
class UploadSkill final : public ISkill {
public:
    explicit UploadSkill(StoragePort port) : port_(std::move(port)) {}
    std::string name() const override { return "storage.upload"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    StoragePort port_;
};

/// `storage.download(address, path)`
class DownloadSkill final : public ISkill {
public:
    explicit DownloadSkill(StoragePort port) : port_(std::move(port)) {}
    std::string name() const override { return "storage.download"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    StoragePort port_;
};

/// `storage.list()`
class ListSkill final : public ISkill {
public:
    explicit ListSkill(StoragePort port) : port_(std::move(port)) {}
    std::string name() const override { return "storage.list"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    StoragePort port_;
};

/// `storage.share(address, recipient)`
class ShareSkill final : public ISkill {
public:
    ShareSkill(StoragePort storage, SharePort share)
        : storage_(std::move(storage)), share_(std::move(share)) {}
    std::string name() const override { return "storage.share"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    StoragePort storage_;
    SharePort share_;
};

} // namespace logos::agent
