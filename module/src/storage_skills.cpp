#include "storage_skills.h"

#include <nlohmann/json.hpp>

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

} // namespace logos::agent
