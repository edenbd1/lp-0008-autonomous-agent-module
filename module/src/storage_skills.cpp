#include "storage_skills.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <dlfcn.h>

namespace logos::agent {

using nlohmann::json;

namespace {

constexpr std::int64_t kChunkSize = 1 << 20; // 1 MiB, the module's usual default

std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
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

} // namespace

std::string UploadSkill::parameterSchema() const
{
    return R"({"type":"object","required":["path"],)"
           R"("properties":{"path":{"type":"string"},"label":{"type":"string"}}})";
}

std::string UploadSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string path;
    if (!field(p, "path", path, err)) return fail(err);
    if (!port_.ready || !port_.ready()) return fail("storage node is not started");
    if (!port_.upload) return fail("storage upload is not wired");

    const std::string cid = port_.upload(path, kChunkSize);
    if (cid.empty()) return fail("storage refused the upload");

    // The label is the agent's own bookkeeping — Storage addresses by content,
    // so it has nowhere to put a name.
    json out{{"address", cid}};
    if (p.contains("label") && p["label"].is_string()) out["label"] = p["label"];
    return done(out);
}

std::string DownloadSkill::parameterSchema() const
{
    return R"({"type":"object","required":["address","path"],)"
           R"("properties":{"address":{"type":"string"},"path":{"type":"string"}}})";
}

std::string DownloadSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string cid, path;
    if (!field(p, "address", cid, err)) return fail(err);
    if (!field(p, "path", path, err)) return fail(err);
    if (!port_.ready || !port_.ready()) return fail("storage node is not started");

    // Ask first, so an unknown address is reported as unknown rather than as a
    // download that failed for some unstated reason.
    if (port_.exists && !port_.exists(cid)) {
        return fail("no such content address");
    }
    if (!port_.download || !port_.download(cid, path)) {
        return fail("storage refused the download");
    }
    return done(json{{"address", cid}, {"path", path}});
}

std::string ListSkill::parameterSchema() const
{
    return R"({"type":"object","properties":{}})";
}

std::string ListSkill::invoke(const std::string &)
{
    if (!port_.ready || !port_.ready()) return fail("storage node is not started");
    if (!port_.manifests) return fail("storage manifests are not wired");

    // Passed through rather than reshaped: the module owns this schema, and a
    // translation layer here would drift from it silently.
    const std::string raw = port_.manifests();
    auto j = json::parse(raw, nullptr, false);
    if (j.is_discarded()) return fail("storage returned a manifest list that did not parse");
    return done(json{{"manifests", j}});
}

std::string ShareSkill::parameterSchema() const
{
    return R"({"type":"object","required":["address","recipient"],)"
           R"("properties":{"address":{"type":"string"},"recipient":{"type":"string"}}})";
}

std::string ShareSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string cid, recipient;
    if (!field(p, "address", cid, err)) return fail(err);
    if (!field(p, "recipient", recipient, err)) return fail(err);

    // Sharing content-addressed data is not a storage operation: there is
    // nothing to copy or re-permission. It is sending someone the address. Both
    // halves are reported separately so a caller can tell which one failed.
    // Refuse when the check cannot be made, rather than when it fails. The
    // earlier form only rejected an address the node positively denied, so a
    // stopped node or an unwired `exists` made the whole condition false and
    // the share went out unverified, reporting ok:true for an address that may
    // not exist. Deleting the check entirely used to pass the test suite.
    if (!storage_.ready || !storage_.ready()) {
        return fail("the storage node is not started, so the address cannot be verified");
    }
    if (!storage_.exists) {
        return fail("no storage port to verify the address against");
    }
    if (!storage_.exists(cid)) {
        return fail("no such content address to share");
    }
    if (!share_.send) return fail("no messaging transport to share over");

    const std::string note = json{{"sharedAddress", cid}}.dump();
    if (!share_.send(recipient, note)) {
        return fail("the address exists but could not be delivered to the recipient");
    }
    return done(json{{"address", cid}, {"recipient", recipient}});
}

// ---------------------------------------------------------------------------
// The node itself
//
// Everything above this line is skill logic over a port somebody else filled in.
// Everything below opens a Logos Storage node and fills that port in from the
// module's own side of the plugin boundary. See the class comment in the header
// for why that is possible at all, and for the three ways it differs from
// `DeliveryRuntime`.
// ---------------------------------------------------------------------------

namespace {

/// The library's own return codes, from `libstorage.h`. Spelled here because
/// that header is deliberately not a build input — see difference 1 in the
/// header — and these four values are part of the published C ABI.
constexpr int kRetOk = 0;
constexpr int kRetProgress = 3;

/// The file name the library is looked for under, assembled rather than spelled
/// out per platform, for the reason `delivery_runtime.cpp` gives at length:
/// `scripts/check-package-fresh.py` corroborates the shipped package by finding
/// every source literal of >= 8 bytes inside the binary, and two full names in
/// two preprocessor branches would be two literals of which only one is ever
/// compiled. The stem is 10 bytes and is always there; the suffixes are three
/// and six, under that floor, and are not checked.
constexpr const char *kStorageLibraryStem = "libstorage";
constexpr const char *kStorageLibrarySuffix =
#if defined(__APPLE__)
    ".dylib";
#else
    ".so";
#endif

std::string storageLibraryName()
{
    return std::string(kStorageLibraryStem) + kStorageLibrarySuffix;
}

using StorageCb = void (*)(int ret, const char *msg, std::size_t len, void *userData);

/**
 * @brief The entry points this module uses, resolved at run time.
 *
 * WHY dlopen AND NOT A LINK-TIME DEPENDENCY
 *
 * The same measured reason `delivery_runtime.cpp` records. Linked normally the
 * plugin would carry `@rpath/libstorage.dylib`, and a module directory without
 * that one file makes the WHOLE PLUGIN fail to load. In a harness you can read
 * the loader's complaint; in Basecamp you cannot — a module that fails to load
 * produces no visible error at all, the tile is simply inert, and the reason
 * goes to stderr. So the single most likely installation mistake produces the
 * one failure this repository refuses to ship: something that looks installed
 * and does nothing, with nothing anywhere saying why.
 *
 * Opened this way the plugin loads either way, all skills register, and
 * `meta.status` answers with the file it wanted and every path it tried.
 */
struct StorageSymbols {
    void *(*create)(const char *, StorageCb, void *) = nullptr;
    int (*start)(void *, StorageCb, void *) = nullptr;
    int (*stop)(void *, StorageCb, void *) = nullptr;
    int (*close)(void *, StorageCb, void *) = nullptr;
    int (*destroy)(void *) = nullptr;
    int (*uploadInit)(void *, const char *, std::size_t, StorageCb, void *) = nullptr;
    int (*uploadFile)(void *, const char *, StorageCb, void *) = nullptr;
    int (*downloadInit)(void *, const char *, std::size_t, bool, StorageCb, void *) = nullptr;
    int (*downloadStream)(void *, const char *, std::size_t, bool, const char *, StorageCb,
                          void *) = nullptr;
    int (*list)(void *, StorageCb, void *) = nullptr;
    int (*exists)(void *, const char *, StorageCb, void *) = nullptr;
    /// Optional: asked for so `meta.status` can report something only a running
    /// node can produce, and deliberately NOT part of `loaded`. A library that
    /// does not export it is still a library this module can store files with,
    /// and refusing the whole transport over a status field would be the wrong
    /// trade.
    int (*peerId)(void *, StorageCb, void *) = nullptr;

    bool loaded = false;
    /// Why not, in words an operator can act on: which file, and where it looked.
    std::string error;
};

/// Where to look, in order: an operator's override, then beside this plugin,
/// then wherever the dynamic loader would look on its own.
///
/// "Beside this plugin" is the one that matters and it cannot be spelled as a
/// relative path — the process's working directory is the host's, not the
/// module's. `dladdr` on a symbol in this translation unit gives the path of the
/// binary that contains it, which is the plugin, whose directory is the module
/// directory the host unpacked.
std::vector<std::string> storageSearchPath()
{
    std::vector<std::string> candidates;
    if (const char *override_ = std::getenv("LP0008_STORAGE_LIB")) {
        if (*override_) candidates.emplace_back(override_);
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&storageSearchPath), &info) != 0 &&
        info.dli_fname != nullptr) {
        const std::string self(info.dli_fname);
        const std::size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) {
            candidates.emplace_back(self.substr(0, slash + 1) + storageLibraryName());
        }
    }
    candidates.emplace_back(storageLibraryName()); // the loader's own search
    return candidates;
}

/// Resolved once and shared: two `StorageRuntime`s in one process would
/// otherwise open two copies of a library that keeps a global Nim runtime.
StorageSymbols &storageSymbols()
{
    static StorageSymbols symbols = [] {
        StorageSymbols s;
        void *handle = nullptr;
        std::string tried;
        for (const std::string &candidate : storageSearchPath()) {
            handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
            if (!tried.empty()) tried += ", ";
            tried += candidate;
        }
        if (!handle) {
            s.error = storageLibraryName() + " could not be opened. Tried: " + tried +
                      ". It belongs in the module directory, beside the plugin";
            return s;
        }
        // Every symbol, checked. A partial resolve would leave a null pointer to
        // be called later, which is a crash inside the host rather than a
        // refusal — and the module would have reported itself ready first.
        const auto sym = [&](const char *name) {
            void *p = dlsym(handle, name);
            if (!p && s.error.empty()) {
                s.error = storageLibraryName() + " does not export " + name +
                          "; it is the wrong build or the wrong version";
            }
            return p;
        };
        s.create = reinterpret_cast<decltype(s.create)>(sym("storage_new"));
        s.start = reinterpret_cast<decltype(s.start)>(sym("storage_start"));
        s.stop = reinterpret_cast<decltype(s.stop)>(sym("storage_stop"));
        s.close = reinterpret_cast<decltype(s.close)>(sym("storage_close"));
        s.destroy = reinterpret_cast<decltype(s.destroy)>(sym("storage_destroy"));
        s.uploadInit = reinterpret_cast<decltype(s.uploadInit)>(sym("storage_upload_init"));
        s.uploadFile = reinterpret_cast<decltype(s.uploadFile)>(sym("storage_upload_file"));
        s.downloadInit = reinterpret_cast<decltype(s.downloadInit)>(sym("storage_download_init"));
        s.downloadStream =
            reinterpret_cast<decltype(s.downloadStream)>(sym("storage_download_stream"));
        s.list = reinterpret_cast<decltype(s.list)>(sym("storage_list"));
        s.exists = reinterpret_cast<decltype(s.exists)>(sym("storage_exists"));
        s.loaded = s.error.empty();
        // After `loaded` is decided, so a library without it is still usable.
        s.peerId = reinterpret_cast<decltype(s.peerId)>(dlsym(handle, "storage_peer_id"));
        return s;
    }();
    return symbols;
}

} // namespace

/**
 * @brief One outstanding call, and the node it is outstanding on.
 *
 * `arm` before the call and `settled` after it, never the other way round: the
 * callback can fire before the call returns, so clearing the flag afterwards
 * would lose the reply. `module/tests/storage_node_drive.c` says the same thing
 * in its own comment and it is the first thing to get wrong here.
 */
struct StorageRuntime::Impl {
    mutable std::mutex mu;
    void *ctx = nullptr;
    /// 0 off, 1 starting, 2 ready, 3 failed.
    std::atomic<int> phase{0};
    std::string error;
    std::string dataDir;
    std::string peerId;
    std::thread bringUpThread;

    /// Held across a whole arm-call-wait sequence. See difference 2 in the
    /// header: the library's callbacks carry no correlation, so overlapping
    /// calls hand each other's replies back.
    std::mutex callMu;
    std::atomic<bool> done{false};
    int code = -1;
    std::string reply;

    void arm()
    {
        std::lock_guard<std::mutex> lock(mu);
        code = -1;
        reply.clear();
        done.store(false);
    }

    /// True when the call answered `RET_OK` within `seconds`.
    ///
    /// A timeout is terminal for the caller — never something to carry on from.
    /// Continuing is not merely untidy: the next `arm` would be satisfied by the
    /// late reply to *this* call, so an `upload_init` that answered slowly would
    /// hand its session id to whatever asked next.
    bool settled(int seconds, const std::function<void()> &idle)
    {
        for (int i = 0; i < seconds * 20; ++i) {
            if (done.load()) {
                std::lock_guard<std::mutex> lock(mu);
                return code == kRetOk;
            }
            if (idle) {
                idle();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        return false;
    }

    std::string take()
    {
        std::lock_guard<std::mutex> lock(mu);
        return reply;
    }

    /// Runs on the library's own thread, which its documentation requires to be
    /// fast and non-blocking. Copy the bytes — the buffer is reused and is NOT
    /// NUL-terminated, so `std::string(msg, len)` and never `strlen` — and
    /// leave.
    static void callback(int ret, const char *msg, std::size_t len, void *userData)
    {
        if (ret == kRetProgress) return; // a chunk, not the answer
        auto *self = static_cast<Impl *>(userData);
        {
            std::lock_guard<std::mutex> lock(self->mu);
            self->reply = (msg != nullptr && len > 0) ? std::string(msg, len) : std::string();
            self->code = ret;
        }
        self->done.store(true);
    }
};

bool StorageRuntime::linkedIn()
{
    return true;
}

StorageRuntime::StorageRuntime() : impl_(new Impl) {}

StorageRuntime::~StorageRuntime()
{
    shutDown();
}

void StorageRuntime::setIdle(std::function<void()> idle)
{
    idle_ = std::move(idle);
}

void StorageRuntime::setDataDir(const std::string &dir)
{
    // Collapse repeated slashes and drop a trailing one. See the header: a
    // `data-dir` containing `//` produces a node that starts, answers, and
    // refuses every write with `Path is outside of 'root' directory!`.
    std::string clean;
    clean.reserve(dir.size());
    for (const char c : dir) {
        if (c == '/' && !clean.empty() && clean.back() == '/') continue;
        clean.push_back(c);
    }
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->dataDir = clean;
}

std::string StorageRuntime::dataDir() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->dataDir;
}

bool StorageRuntime::bringUp(std::string &error)
{
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        dir = impl_->dataDir;
    }
    // Refused rather than guessed. A Storage node is a repository on disk, and a
    // module that picked a directory on an operator's behalf would take a
    // LevelDB lock somewhere nobody chose and keep it.
    if (dir.empty()) {
        error = "no storage data directory is configured: set 'storage_data_dir' first, or "
                "load this module in a host that provisions one";
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = error;
        impl_->phase.store(3);
        return false;
    }
    if (impl_->phase.load() != 0) {
        // Already starting, up, or failed. Not an error: `meta.configure` is a
        // setting, and setting a value to what it already is has to be allowed.
        return true;
    }
    impl_->phase.store(1);
    impl_->bringUpThread = std::thread([this, dir] {
        Impl *impl = impl_.get();
        // Resolved on this thread, not the caller's: `dlopen` of a 40 MB Nim
        // library is not something to do inside a `meta.configure` that is
        // expected to answer immediately.
        const StorageSymbols &symbols = storageSymbols();
        if (!symbols.loaded) {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->error = symbols.error;
            impl->phase.store(3);
            return;
        }
        // Built through the JSON library rather than concatenated: the directory
        // is operator-supplied and a hand-built document would let a quote in a
        // path forge the field beside it.
        const std::string config =
            json{{"log-level", "WARN"}, {"data-dir", dir}}.dump();
        impl->arm();
        void *ctx = symbols.create(config.c_str(), &Impl::callback, impl);
        if (!ctx) {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->error = "storage_new returned no context for " + dir;
            impl->phase.store(3);
            return;
        }
        if (!impl->settled(60, nullptr)) {
            const std::string why = impl->take();
            symbols.destroy(ctx);
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->error = why.empty() ? ("the storage node was not constructed at " + dir) : why;
            impl->phase.store(3);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->ctx = ctx;
        }
        impl->arm();
        symbols.start(ctx, &Impl::callback, impl);
        if (!impl->settled(180, nullptr)) {
            const std::string why = impl->take();
            std::lock_guard<std::mutex> lock(impl->mu);
            if (impl->error.empty()) {
                impl->error =
                    why.empty() ? "the storage node did not report that it started" : why;
            }
            impl->phase.store(3);
            return;
        }
        // Something only a started node can answer, kept for `meta.status`. Not
        // fatal: see the note on the optional symbol above.
        if (symbols.peerId != nullptr) {
            impl->arm();
            if (symbols.peerId(ctx, &Impl::callback, impl) == kRetOk &&
                impl->settled(30, nullptr)) {
                const std::string id = impl->take();
                std::lock_guard<std::mutex> lock(impl->mu);
                impl->peerId = id;
            }
        }
        impl->phase.store(2);
    });
    (void)error;
    return true;
}

void StorageRuntime::shutDown()
{
    if (impl_->bringUpThread.joinable()) impl_->bringUpThread.join();
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
        impl_->ctx = nullptr;
        impl_->peerId.clear();
    }
    if (!ctx) {
        // A phase of 3 with no context is a library that could not be opened or
        // a node that failed to start; neither becomes "off" because somebody
        // asked for it to stop. Only a node that was actually up goes back.
        if (impl_->phase.load() != 3) impl_->phase.store(0);
        return;
    }
    const StorageSymbols &symbols = storageSymbols();
    // Stop, close, destroy — all three and in that order. `libstorage.h`
    // requires it, and a node left running holds a LevelDB lock on the
    // repository that the next `bringUp` would then fail on.
    std::lock_guard<std::mutex> call(impl_->callMu);
    impl_->arm();
    if (symbols.stop != nullptr && symbols.stop(ctx, &Impl::callback, impl_.get()) == kRetOk) {
        impl_->settled(60, nullptr);
    }
    impl_->arm();
    if (symbols.close != nullptr && symbols.close(ctx, &Impl::callback, impl_.get()) == kRetOk) {
        impl_->settled(30, nullptr);
    }
    if (symbols.destroy != nullptr) symbols.destroy(ctx);
    impl_->phase.store(0);
}

bool StorageRuntime::ready() const
{
    return impl_->phase.load() == 2;
}

std::string StorageRuntime::state() const
{
    switch (impl_->phase.load()) {
    case 1: return "starting";
    case 2: return "ready";
    case 3: return "failed";
    default: return "off";
    }
}

std::string StorageRuntime::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

std::string StorageRuntime::upload(const std::string &path, std::int64_t chunkSize)
{
    if (!ready()) return {};
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (ctx == nullptr) return {};
    if (chunkSize <= 0) return {};
    const StorageSymbols &symbols = storageSymbols();

    // ONE lock across BOTH calls. The session id `upload_init` answers with is
    // only meaningful to the `upload_file` that follows it, so a second upload
    // interleaving here would finalise this one's session and hand back its
    // content address for the wrong file.
    std::lock_guard<std::mutex> call(impl_->callMu);
    impl_->arm();
    if (symbols.uploadInit(ctx, path.c_str(), static_cast<std::size_t>(chunkSize),
                           &Impl::callback, impl_.get()) != kRetOk) {
        return {};
    }
    if (!impl_->settled(60, idle_)) return {};
    const std::string session = impl_->take();
    if (session.empty()) return {};

    impl_->arm();
    if (symbols.uploadFile(ctx, session.c_str(), &Impl::callback, impl_.get()) != kRetOk) {
        return {};
    }
    if (!impl_->settled(300, idle_)) return {};
    return impl_->take();
}

bool StorageRuntime::download(const std::string &cid, const std::string &path)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (ctx == nullptr) return false;
    const StorageSymbols &symbols = storageSymbols();

    // `local` is true, and that is a statement about what this skill covers
    // rather than a tuning choice. `DownloadSkill` asks `exists` first, and
    // `storage_exists` is a LOCAL datastore lookup — so an address this node
    // does not already hold has been refused before it reaches here, and a
    // download that went to the network would be waiting for content the skill
    // has already said it cannot see. Pulling a peer's content into the local
    // store is `storage_fetch`, which no skill exposes; `docs/skills.md` says so
    // rather than letting the distinction be discovered.
    std::lock_guard<std::mutex> call(impl_->callMu);
    impl_->arm();
    if (symbols.downloadInit(ctx, cid.c_str(), 65536, true, &Impl::callback, impl_.get())
        != kRetOk) {
        return false;
    }
    if (!impl_->settled(60, idle_)) return false;

    impl_->arm();
    if (symbols.downloadStream(ctx, cid.c_str(), 65536, true, path.c_str(), &Impl::callback,
                               impl_.get()) != kRetOk) {
        return false;
    }
    return impl_->settled(300, idle_);
}

std::string StorageRuntime::manifests()
{
    if (!ready()) return {};
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (ctx == nullptr) return {};
    std::lock_guard<std::mutex> call(impl_->callMu);
    impl_->arm();
    if (storageSymbols().list(ctx, &Impl::callback, impl_.get()) != kRetOk) return {};
    if (!impl_->settled(60, idle_)) return {};
    return impl_->take();
}

bool StorageRuntime::exists(const std::string &cid)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (ctx == nullptr) return false;
    std::lock_guard<std::mutex> call(impl_->callMu);
    impl_->arm();
    if (storageSymbols().exists(ctx, cid.c_str(), &Impl::callback, impl_.get()) != kRetOk) {
        return false;
    }
    if (!impl_->settled(60, idle_)) return false;
    // The node answers with a JSON document rather than a bare boolean, and it
    // is parsed rather than searched for the substring `true`: a manifest or an
    // error message mentioning the word would otherwise read as a yes.
    const std::string answer = impl_->take();
    auto parsed = json::parse(answer, nullptr, false);
    if (parsed.is_boolean()) return parsed.get<bool>();
    if (parsed.is_object() && parsed.contains("exists") && parsed["exists"].is_boolean()) {
        return parsed["exists"].get<bool>();
    }
    return answer == "true";
}

/// Reported the way `meta.status` reports the transport, plus the one thing only
/// a running node can produce.
std::string StorageRuntime::statusJson() const
{
    json out{{"state", state()}, {"linked", linkedIn()}};
    std::string err;
    std::string dir;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        err = impl_->error;
        dir = impl_->dataDir;
        id = impl_->peerId;
    }
    if (!dir.empty()) out["dataDir"] = dir;
    if (!id.empty()) out["peerId"] = id;
    if (!err.empty()) out["error"] = err;
    return out.dump();
}

StoragePort StorageRuntime::storagePort()
{
    StoragePort port;
    port.ready = [this] { return ready(); };
    port.upload = [this](const std::string &path, std::int64_t chunkSize) {
        return upload(path, chunkSize);
    };
    port.download = [this](const std::string &cid, const std::string &path) {
        return download(cid, path);
    };
    port.manifests = [this] { return manifests(); };
    port.exists = [this](const std::string &cid) { return exists(cid); };
    return port;
}

} // namespace logos::agent
