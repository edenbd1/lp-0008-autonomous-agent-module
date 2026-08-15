#include "delivery_runtime.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#ifdef LP0008_WITH_DELIVERY
#include "liblogosdelivery.h"

#include <dlfcn.h>
#endif

namespace logos::agent {

namespace {

using nlohmann::json;

const char *const kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// The state a build with no Delivery library reports, as distinct from `off`.
/// Named rather than spelled twice: `agent_module_plugin.cpp` turns it into the
/// sentence an operator reads, and two spellings would be two states.
constexpr const char *kStateAbsent = "absent";

/// The name that actually comes back, inside the payload's `eventType`. These
/// are two different strings; matching the first against the second is how a
/// node that receives everything appears to receive nothing.
constexpr const char *kMessageEventType = "message_received";

} // namespace

std::string base64Encode(const std::string &bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
        const std::uint32_t b =
            i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const std::uint32_t c =
            i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const std::uint32_t v = (a << 16) | (b << 8) | c;
        out += kBase64Alphabet[(v >> 18) & 63];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kBase64Alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kBase64Alphabet[v & 63] : '=';
    }
    return out;
}

bool parseMessageEvent(const std::string &eventJson, DeliveredMessage &out)
{
    auto j = json::parse(eventJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (!j.contains("eventType") || !j["eventType"].is_string()) return false;
    if (j["eventType"].get<std::string>() != kMessageEventType) return false;
    // `message`, and a payload that is an ARRAY OF BYTES. Both of those were
    // measured off a live node rather than inferred, and the version this
    // replaced got both wrong: it read `wakuMessage` and base64, which is the
    // shape the *channel* API uses and the shape `library/events/json_message_event.nim`
    // documents for the REST surface. `library/json_event.nim` flattens the Nim
    // `MessageReceivedEvent` — `messageHash` and `message` — and `std/json`
    // renders that message's `seq[byte]` payload as numbers:
    //
    //   {"eventType":"message_received","messageHash":"0x35bb…",
    //    "message":{"payload":[104,101,108,108,111],"contentTopic":"/lp-0008/…",
    //               "meta":[],"version":0,"timestamp":178…,"ephemeral":false,"proof":[]}}
    //
    // A decoder that looks for the wrong field does not fail loudly. It returns
    // false for every frame, the inbox stays empty, and `agent.discover` answers
    // "no agents" — an agent that hears nothing looks exactly like an agent
    // nobody is talking to. This one cost a full two-process run to find, and
    // was found only because that run asserted on a card from the OTHER agent.
    if (!j.contains("message") || !j["message"].is_object()) return false;
    const json &m = j["message"];
    if (!m.contains("contentTopic") || !m["contentTopic"].is_string()) return false;
    if (!m.contains("payload") || !m["payload"].is_array()) return false;
    std::string decoded;
    decoded.reserve(m["payload"].size());
    for (const auto &byte : m["payload"]) {
        if (!byte.is_number_unsigned()) return false;
        const std::uint64_t v = byte.get<std::uint64_t>();
        if (v > 255) return false;
        decoded.push_back(static_cast<char>(v));
    }
    out.contentTopic = m["contentTopic"].get<std::string>();
    out.payload = std::move(decoded);
    return true;
}

// ---------------------------------------------------------------------------
// The node itself
// ---------------------------------------------------------------------------

#ifdef LP0008_WITH_DELIVERY

namespace {

/// The name the listener is registered under. The pair with `kMessageEventType`
/// above is the trap: you register `onMessageReceived` and what arrives says
/// `"eventType":"message_received"`.
constexpr const char *kMessageListenerName = "onMessageReceived";

/// The file name the library is looked for under, assembled rather than
/// spelled out per platform.
///
/// Two full names in two preprocessor branches would be two string literals of
/// which only one is ever compiled, and `scripts/check-package-fresh.py`
/// corroborates the shipped package by finding every source literal of >= 8
/// bytes inside the binary — a literal in the branch this build did not take
/// can never satisfy it. The stem is 16 bytes and is always there; the two
/// suffixes are three and six, under that floor, and are not checked.
constexpr const char *kDeliveryLibraryStem = "liblogosdelivery";
constexpr const char *kDeliveryLibrarySuffix =
#if defined(__APPLE__)
    ".dylib";
#else
    ".so";
#endif

std::string deliveryLibraryName()
{
    return std::string(kDeliveryLibraryStem) + kDeliveryLibrarySuffix;
}

/**
 * @brief The nine entry points this module uses, resolved at run time.
 *
 * WHY dlopen AND NOT A LINK-TIME DEPENDENCY
 *
 * Because of what the alternative does when the file is missing, which was
 * measured rather than imagined. Linked normally, the plugin carries
 * `@rpath/liblogosdelivery.dylib`, and a module directory without that file
 * makes the *whole plugin* fail to load:
 *
 *   Cannot load library …/agent_plugin.dylib: dlopen(…): Library not loaded:
 *   @rpath/liblogosdelivery.dylib … (no such file)
 *
 * In a harness you can read that. In Basecamp you cannot: a module that fails
 * to load produces no visible error at all — the tile is simply inert and the
 * reason goes to stderr. So the single most likely installation mistake (unpack
 * the package, miss one file) produces the one failure this repository refuses
 * to ship: something that looks installed and does nothing, with no error
 * anywhere saying why. `docs/basecamp.md` opens on that principle and this is
 * the same principle applied to a dependency instead of to a skill.
 *
 * Resolved this way, the plugin loads either way and `meta.status` answers with
 * the path it tried and did not find. The cost is nine `dlsym` calls, once.
 */
struct DeliverySymbols {
    void *(*create_node)(const LogosdeliveryCreateNodeCtorReq *, LogosDeliveryCreateRawFn,
                         void *) = nullptr;
    int (*destroy)(void *) = nullptr;
    int (*start_node)(void *, LogosDeliveryScalarRawFn, void *) = nullptr;
    int (*stop_node)(void *, LogosDeliveryScalarRawFn, void *) = nullptr;
    int (*subscribe)(void *, LogosDeliverySubscribeReplyFn, void *,
                     const LogosdeliverySubscribeReq *) = nullptr;
    int (*send)(void *, LogosDeliverySendReplyFn, void *,
                const LogosdeliverySendReq *) = nullptr;
    int (*channel_create)(void *, LogosDeliveryChannelCreateReplyFn, void *,
                          const LogosdeliveryChannelCreateReq *) = nullptr;
    int (*channel_send)(void *, LogosDeliveryChannelSendReplyFn, void *,
                        const LogosdeliveryChannelSendReq *) = nullptr;
    std::uint64_t (*add_event_listener)(void *, const char *, FFICallBack, void *) = nullptr;

    bool loaded = false;
    /// Why not, in words an operator can act on: which file, and where it looked.
    std::string error;
};

/// Where to look, in order: an operator's override, then beside this plugin,
/// then wherever the dynamic loader would look on its own.
///
/// "Beside this plugin" is the one that matters and it cannot be spelled as a
/// relative path — the process's working directory is the host's, not the
/// module's. `dladdr` on a symbol in this translation unit gives the path of
/// the binary that contains it, which is the plugin, whose directory is the
/// module directory the host unpacked.
std::vector<std::string> deliverySearchPath()
{
    std::vector<std::string> candidates;
    if (const char *override_ = std::getenv("LP0008_DELIVERY_LIB")) {
        if (*override_) candidates.emplace_back(override_);
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&deliverySearchPath), &info) != 0 &&
        info.dli_fname != nullptr) {
        const std::string self(info.dli_fname);
        const std::size_t slash = self.find_last_of('/');
        if (slash != std::string::npos) {
            candidates.emplace_back(self.substr(0, slash + 1) + deliveryLibraryName());
        }
    }
    candidates.emplace_back(deliveryLibraryName());  // the loader's own search
    return candidates;
}

/// Resolved once and shared: two `DeliveryRuntime`s in one process would
/// otherwise open two copies of a library that keeps a global Nim runtime.
DeliverySymbols &deliverySymbols()
{
    static DeliverySymbols symbols = [] {
        DeliverySymbols s;
        void *handle = nullptr;
        std::string tried;
        for (const std::string &candidate : deliverySearchPath()) {
            handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
            if (!tried.empty()) tried += ", ";
            tried += candidate;
        }
        if (!handle) {
            s.error = deliveryLibraryName() +
                      " could not be opened. Tried: " + tried +
                      ". It belongs in the module directory, beside the plugin";
            return s;
        }
        // Every symbol, checked. A partial resolve would leave a null pointer to
        // be called later, which is a crash inside the host rather than a
        // refusal — and the module would have reported itself ready first.
        const auto sym = [&](const char *name) {
            void *p = dlsym(handle, name);
            if (!p && s.error.empty()) {
                s.error = deliveryLibraryName() + " does not export " + name +
                          "; it is the wrong build or the wrong version";
            }
            return p;
        };
        s.create_node = reinterpret_cast<decltype(s.create_node)>(sym("logosdelivery_create_node"));
        s.destroy = reinterpret_cast<decltype(s.destroy)>(sym("logosdelivery_destroy"));
        s.start_node = reinterpret_cast<decltype(s.start_node)>(sym("logosdelivery_start_node"));
        s.stop_node = reinterpret_cast<decltype(s.stop_node)>(sym("logosdelivery_stop_node"));
        s.subscribe = reinterpret_cast<decltype(s.subscribe)>(sym("logosdelivery_subscribe"));
        s.send = reinterpret_cast<decltype(s.send)>(sym("logosdelivery_send"));
        s.channel_create =
            reinterpret_cast<decltype(s.channel_create)>(sym("logosdelivery_channel_create"));
        s.channel_send =
            reinterpret_cast<decltype(s.channel_send)>(sym("logosdelivery_channel_send"));
        s.add_event_listener = reinterpret_cast<decltype(s.add_event_listener)>(
            sym("logosdelivery_add_event_listener"));
        s.loaded = s.error.empty();
        return s;
    }();
    return symbols;
}

/// A reply that carries a value, matched by the object handed to `userData`.
struct Reply {
    std::atomic<int> state{-1};  // -1 pending, 1 ok, 0 failed
    std::string value;
    std::string error;

    static void fn(int ret, const char *reply, const char *errMsg, void *ud)
    {
        auto *self = static_cast<Reply *>(ud);
        if (ret == RET_OK) {
            if (reply) self->value = reply;  // NUL-terminated: nimffi dups it
            self->state.store(1);
        } else {
            if (errMsg) self->error = errMsg;
            self->state.store(0);
        }
    }

    /// Wait up to `ms` for the library to answer. Every call below is local to
    /// the node, so this is milliseconds in practice; the bound exists so a
    /// library that never calls back cannot wedge a skill forever.
    ///
    /// `idle` is the host's, and on the thread this runs on it is usually the
    /// one that pumps the event loop the whole module is dispatched from —
    /// see `DeliveryRuntime::setIdle`. Sleeping is the fallback.
    bool wait(int ms, const std::function<void()> &idle)
    {
        for (int i = 0; i < ms / 10 && state.load() == -1; ++i) {
            if (idle) {
                idle();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return state.load() == 1;
    }
};

} // namespace

struct DeliveryRuntime::Impl {
    mutable std::mutex mu;
    void *ctx = nullptr;
    /// 0 off, 1 starting, 2 ready, 3 failed.
    std::atomic<int> phase{0};
    std::atomic<int> created{-1};
    std::atomic<int> started{-1};
    std::string error;
    std::thread bringUpThread;
    std::set<std::string> subscribed;
    std::map<std::string, std::vector<std::string>> inbox;    // topic -> payloads
    std::vector<std::string> channelEvents;                    // raw, decoded later

    static void onCreated(int ret, const char *, const char *errMsg, void *ud)
    {
        auto *self = static_cast<Impl *>(ud);
        if (ret != RET_OK && errMsg) {
            std::lock_guard<std::mutex> lock(self->mu);
            self->error = errMsg;
        }
        self->created.store(ret == RET_OK ? 1 : 0);
    }

    static void onScalar(int ret, char *msg, std::size_t len, void *ud)
    {
        auto *self = static_cast<Impl *>(ud);
        if (ret == NIMFFI_RET_STALE_WARN) return;  // progress tick, not terminal
        if (ret != RET_OK && msg) {
            std::lock_guard<std::mutex> lock(self->mu);
            self->error.assign(msg, len);  // length-bounded: the buffer is reused
        }
        self->started.store(ret == RET_OK ? 1 : 0);
    }

    /// Relay traffic. Runs on the library's event thread: copy and leave.
    static void onMessage(int ret, const char *ev, std::size_t len, void *ud)
    {
        if (ret != RET_OK || !ev || len == 0) return;
        auto *self = static_cast<Impl *>(ud);
        const std::string raw(ev, len);
        DeliveredMessage m;
        if (!parseMessageEvent(raw, m)) return;
        std::lock_guard<std::mutex> lock(self->mu);
        self->inbox[m.contentTopic].push_back(std::move(m.payload));
    }

    /// Reliable-channel frames. Decoded on the reader's thread, because
    /// `parseInboundEvent` allocates and the event thread must not.
    static void onChannel(int ret, const char *ev, std::size_t len, void *ud)
    {
        if (ret != RET_OK || !ev || len == 0) return;
        auto *self = static_cast<Impl *>(ud);
        std::lock_guard<std::mutex> lock(self->mu);
        self->channelEvents.emplace_back(ev, len);
    }
};

bool DeliveryRuntime::linkedIn()
{
    return true;
}

DeliveryRuntime::DeliveryRuntime() : impl_(new Impl) {}

DeliveryRuntime::~DeliveryRuntime()
{
    shutDown();
}

bool DeliveryRuntime::bringUp(std::string &error)
{
    // Before anything is started, and before the phase moves: a library that
    // could not be opened is not a node that failed to come up, and reporting
    // it as one would send an operator looking at their network.
    const DeliverySymbols &symbols = deliverySymbols();
    if (!symbols.loaded) {
        error = symbols.error;
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = symbols.error;
        impl_->phase.store(3);
        return false;
    }
    if (impl_->phase.load() != 0) {
        // Already starting, up, or failed. Not an error: `meta.configure` is a
        // setting, and setting a value to what it already is has to be allowed.
        return true;
    }
    impl_->phase.store(1);
    impl_->bringUpThread = std::thread([this] {
        Impl *impl = impl_.get();
        // Core mode on the dev preset: the pair `module/tests/delivery_node_drive.c`
        // and `owner_channel_live.cpp` both run against, so a failure here is
        // ours and not a guess at an unsupported network.
        const char *config = "{\"mode\":\"Core\",\"preset\":\"logos.dev\","
                             "\"messagingOverrides\":{\"log-level\":\"WARN\"}}";
        LogosdeliveryCreateNodeCtorReq req{};
        req.configJson = config;
        void *ctx = deliverySymbols().create_node(&req, &Impl::onCreated, impl);
        if (!ctx) {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->error = "logosdelivery_create_node returned no context";
            impl->phase.store(3);
            return;
        }
        for (int i = 0; i < 600 && impl->created.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (impl->created.load() != 1) {
            std::lock_guard<std::mutex> lock(impl->mu);
            if (impl->error.empty()) impl->error = "the node was not constructed";
            impl->phase.store(3);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->ctx = ctx;
        }
        // Before start, not after: a listener registered on a running node
        // misses whatever arrived in between, and the whole point of the
        // discovery inbox is that it holds what was published before anyone
        // asked for it.
        const std::uint64_t m =
            deliverySymbols().add_event_listener(ctx, kMessageListenerName, &Impl::onMessage, impl);
        const std::uint64_t c = deliverySymbols().add_event_listener(
            ctx, kInboundListenerName, &Impl::onChannel, impl);
        if (m == 0 || c == 0) {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->error = "the node refused an event listener registration";
            impl->phase.store(3);
            return;
        }
        deliverySymbols().start_node(ctx, &Impl::onScalar, impl);
        for (int i = 0; i < 1800 && impl->started.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (impl->started.load() != 1) {
            std::lock_guard<std::mutex> lock(impl->mu);
            if (impl->error.empty()) impl->error = "the node did not report that it started";
            impl->phase.store(3);
            return;
        }
        impl->phase.store(2);
    });
    (void)error;
    return true;
}

void DeliveryRuntime::shutDown()
{
    if (impl_->bringUpThread.joinable()) impl_->bringUpThread.join();
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
        impl_->ctx = nullptr;
    }
    if (!ctx) {
        // A phase of 3 with no context is either a resolve that failed or a node
        // that failed to start; neither becomes "off" because somebody asked for
        // it to stop. Only a node that was actually up goes back to off.
        if (impl_->phase.load() != 3) impl_->phase.store(0);
        return;
    }
    if (impl_->started.load() == 1) {
        impl_->started.store(-1);
        deliverySymbols().stop_node(ctx, &Impl::onScalar, impl_.get());
        for (int i = 0; i < 300 && impl_->started.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    deliverySymbols().destroy(ctx);
    impl_->phase.store(0);
}

bool DeliveryRuntime::ready() const
{
    return impl_->phase.load() == 2;
}

bool DeliveryRuntime::subscribe(const std::string &topic)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (!ctx) return false;
    LogosdeliverySubscribeReq req{};
    req.contentTopicStr = topic.c_str();
    Reply r;
    if (deliverySymbols().subscribe(ctx, &Reply::fn, &r, &req) != RET_OK) return false;
    if (!r.wait(30000, idle_)) return false;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->subscribed.insert(topic);
    return true;
}

bool DeliveryRuntime::publish(const std::string &topic, const std::string &payload)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (!ctx) return false;
    const std::string msg = json{{"contentTopic", topic},
                                 {"payload", base64Encode(payload)},
                                 {"ephemeral", false}}
                                .dump();
    LogosdeliverySendReq req{};
    req.messageJson = msg.c_str();
    Reply r;
    if (deliverySymbols().send(ctx, &Reply::fn, &r, &req) != RET_OK) return false;
    return r.wait(30000, idle_);
}

bool DeliveryRuntime::channelCreate(const std::string &channelId,
                                    const std::string &contentTopic,
                                    const std::string &senderId)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (!ctx) return false;
    LogosdeliveryChannelCreateReq req{};
    req.channelIdStr = channelId.c_str();
    req.contentTopicStr = contentTopic.c_str();
    req.senderIdStr = senderId.c_str();
    Reply r;
    if (deliverySymbols().channel_create(ctx, &Reply::fn, &r, &req) != RET_OK) return false;
    return r.wait(30000, idle_);
}

bool DeliveryRuntime::channelSend(const std::string &channelId,
                                  const std::vector<std::uint8_t> &payload)
{
    if (!ready()) return false;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ctx = impl_->ctx;
    }
    if (!ctx) return false;
    const std::string bytes(reinterpret_cast<const char *>(payload.data()), payload.size());
    const std::string msg =
        json{{"payload", base64Encode(bytes)}, {"ephemeral", false}}.dump();
    LogosdeliveryChannelSendReq req{};
    req.channelIdStr = channelId.c_str();
    req.messageJson = msg.c_str();
    Reply r;
    if (deliverySymbols().channel_send(ctx, &Reply::fn, &r, &req) != RET_OK) return false;
    return r.wait(30000, idle_);
}

std::vector<std::string> DeliveryRuntime::received(const std::string &topic)
{
    bool needSubscribe = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        needSubscribe = impl_->subscribed.count(topic) == 0;
    }
    if (needSubscribe) subscribe(topic);
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->inbox.find(topic);
    return it == impl_->inbox.end() ? std::vector<std::string>{} : it->second;
}

std::vector<InboundMessage> DeliveryRuntime::drainChannel(const std::string &channelId)
{
    std::vector<std::string> raw;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        raw.swap(impl_->channelEvents);
    }
    std::vector<InboundMessage> out;
    for (const std::string &e : raw) {
        InboundMessage m;
        if (parseInboundEvent(e, channelId, m)) out.push_back(std::move(m));
    }
    return out;
}

std::string DeliveryRuntime::state() const
{
    switch (impl_->phase.load()) {
    case 1: return "starting";
    case 2: return "ready";
    case 3: return "failed";
    default: return "off";
    }
}

std::string DeliveryRuntime::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

#else // LP0008_WITH_DELIVERY

// Built without the library. Every entry point answers "no", and @ref state
// says `absent` rather than `off`, because a caller has to be able to tell a
// node that has not been asked to start from a binary that could not start one.

struct DeliveryRuntime::Impl {};

bool DeliveryRuntime::linkedIn()
{
    return false;
}

DeliveryRuntime::DeliveryRuntime() : impl_(new Impl) {}
DeliveryRuntime::~DeliveryRuntime() = default;

bool DeliveryRuntime::bringUp(std::string &error)
{
    // Deliberately no message here. `check-package-fresh.py` asserts that every
    // string literal of >= 8 bytes in `module/src` is present in the shipped
    // plugin, which a literal inside a preprocessor branch that was not
    // compiled can never be. So the words a caller sees for this case live in
    // `agent_module_plugin.cpp`, which is compiled identically either way, and
    // this branch reports the fact and not the sentence.
    error.clear();
    return false;
}

void DeliveryRuntime::shutDown() {}
bool DeliveryRuntime::ready() const { return false; }
bool DeliveryRuntime::subscribe(const std::string &) { return false; }
bool DeliveryRuntime::publish(const std::string &, const std::string &) { return false; }
bool DeliveryRuntime::channelCreate(const std::string &, const std::string &,
                                    const std::string &)
{
    return false;
}
bool DeliveryRuntime::channelSend(const std::string &, const std::vector<std::uint8_t> &)
{
    return false;
}
std::vector<std::string> DeliveryRuntime::received(const std::string &) { return {}; }
std::vector<InboundMessage> DeliveryRuntime::drainChannel(const std::string &) { return {}; }
std::string DeliveryRuntime::state() const { return kStateAbsent; }
std::string DeliveryRuntime::lastError() const { return {}; }

#endif // LP0008_WITH_DELIVERY

void DeliveryRuntime::setIdle(std::function<void()> idle)
{
    idle_ = std::move(idle);
}

DeliveryPort DeliveryRuntime::deliveryPort()
{
    DeliveryPort port;
    port.ready = [this] { return ready(); };
    port.send = [this](const std::string &topic, const std::vector<std::uint8_t> &payload) {
        return publish(topic,
                       std::string(reinterpret_cast<const char *>(payload.data()),
                                   payload.size()));
    };
    port.subscribe = [this](const std::string &topic) { return subscribe(topic); };
    // A "channel" in `messaging.create_group` is the reliable channel, opened on
    // the group's own content topic with this node as the sender.
    port.channelCreate = [this](const std::string &channelId) {
        return channelCreate(channelId, channelId, channelId);
    };
    return port;
}

OwnerChannelPort DeliveryRuntime::ownerChannelPort()
{
    OwnerChannelPort port;
    port.ready = [this] { return ready(); };
    port.channelCreate = [this](const std::string &channelId, const std::string &contentTopic,
                                const std::string &senderId) {
        return channelCreate(channelId, contentTopic, senderId);
    };
    port.channelSend = [this](const std::string &channelId,
                              const std::vector<std::uint8_t> &payload) {
        return channelSend(channelId, payload);
    };
    port.drain = [this](const std::string &channelId) { return drainChannel(channelId); };
    port.nowMs = [] {
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
    };
    port.idle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); };
    return port;
}

} // namespace logos::agent
