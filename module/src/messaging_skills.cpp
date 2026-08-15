#include "messaging_skills.h"

#include <nlohmann/json.hpp>

namespace logos::agent {

using nlohmann::json;

// Content topics follow the grammar Logos documents:
// /<application>/<version>/<name>/<encoding>
// https://lip.logos.co/messaging/informational/23/topics.html
std::string ownerTopic(const std::string &account)
{
    return "/lp-0008/1/owner-" + account + "/json";
}

std::string discoveryTopic(const std::string &ns)
{
    return "/lp-0008/1/discovery-" + ns + "/json";
}

namespace {

/// Every skill answers in the same shape, so a caller — including another agent
/// over A2A — can branch without knowing which skill it called.
std::string fail(const std::string &why)
{
    return json{{"ok", false}, {"error", why}}.dump();
}

std::string done(json extra = json::object())
{
    extra["ok"] = true;
    return extra.dump();
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

json parse(const std::string &s, std::string &err)
{
    auto j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "parameters must be a JSON object";
        return json::object();
    }
    return j;
}

} // namespace

std::string SendSkill::parameterSchema() const
{
    return R"({"type":"object","required":["recipient","message"],)"
           R"("properties":{"recipient":{"type":"string","description":"Logos account id"},)"
           R"("message":{"type":"string"}}})";
}

std::string SendSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string recipient, message;
    if (!field(p, "recipient", recipient, err)) return fail(err);
    if (!field(p, "message", message, err)) return fail(err);

    // Refuse rather than drop. `start` returns once dispatched, so a node that
    // has not reported nodeStarted yet would swallow this silently.
    if (!port_.ready || !port_.ready()) {
        return fail("delivery node is not started");
    }

    const std::string topic = ownerTopic(recipient);
    const std::vector<std::uint8_t> payload(message.begin(), message.end());
    if (!port_.send || !port_.send(topic, payload)) {
        return fail("delivery refused the message");
    }
    return done(json{{"topic", topic}, {"bytes", payload.size()}});
}

std::string JoinSkill::parameterSchema() const
{
    return R"({"type":"object","required":["group_id"],)"
           R"("properties":{"group_id":{"type":"string"}}})";
}

std::string JoinSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string group;
    if (!field(p, "group_id", group, err)) return fail(err);
    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    const std::string topic = discoveryTopic(group);
    if (!port_.subscribe || !port_.subscribe(topic)) {
        return fail("delivery refused the subscription");
    }
    return done(json{{"topic", topic}});
}

std::string CreateGroupSkill::parameterSchema() const
{
    return R"({"type":"object","required":["group_id","members"],)"
           R"("properties":{"group_id":{"type":"string"},)"
           R"("members":{"type":"array","items":{"type":"string"}}}})";
}

std::string CreateGroupSkill::invoke(const std::string &paramsJson)
{
    std::string err;
    const json p = parse(paramsJson, err);
    if (!err.empty()) return fail(err);

    std::string group;
    if (!field(p, "group_id", group, err)) return fail(err);
    if (!p.contains("members") || !p["members"].is_array() || p["members"].empty()) {
        return fail("'members' must be a non-empty array");
    }
    if (!port_.ready || !port_.ready()) return fail("delivery node is not started");

    // A reliable channel, not a bare topic: group traffic that silently drops
    // messages is worse than a group that fails to open.
    if (!port_.channelCreate || !port_.channelCreate(group)) {
        return fail("delivery refused to create the channel");
    }

    // Each member is invited on their own owner topic, which is the only address
    // an agent can derive from an Agent Card.
    //
    // Count what was asked for as well as what succeeded. Reporting only the
    // successes made every failure look like an empty guest list: a group whose
    // invitations were all refused, and one created with no members, both came
    // back ok:true with invited:0. A member that is not a string is a malformed
    // request, not something to skip quietly.
    std::size_t invited = 0, asked = 0;
    for (const auto &m : p["members"]) {
        if (!m.is_string()) {
            return fail("every member must be a string account id");
        }
        ++asked;
        const std::string note = json{{"invite", group}}.dump();
        if (port_.send && port_.send(ownerTopic(m.get<std::string>()),
                                     std::vector<std::uint8_t>(note.begin(), note.end()))) {
            ++invited;
        }
    }
    if (invited != asked) {
        return fail("the channel opened but " + std::to_string(asked - invited) +
                    " of " + std::to_string(asked) + " invitations could not be delivered");
    }
    return done(json{{"channel", group}, {"invited", invited}});
}

} // namespace logos::agent
