// Exercise the skills through their ports, with no Logos node running.
//
// This is what the DeliveryPort / StoragePort indirection is for. A node is
// needed to prove a message arrives; it is not needed to prove that a skill
// refuses when the node is down, reports which half of a share failed, or does
// not throw on a malformed payload. Those are the behaviours a reviewer cannot
// see from a screenshot, and they are exactly what a stub would get wrong.
#include "../src/messaging_skills.h"
#include "../src/storage_skills.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>

using nlohmann::json;
using namespace logos::agent;

static int failures = 0;

static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static bool okOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return !j.is_discarded() && j.value("ok", false);
}

static std::string errOf(const std::string &r)
{
    auto j = json::parse(r, nullptr, false);
    return j.is_discarded() ? "" : j.value("error", std::string{});
}

int main()
{
    std::printf("messaging\n");
    {
        // A node that has not started must refuse, not report success. `start`
        // returns as soon as it is dispatched, so this is the realistic state.
        DeliveryPort down{[] { return false; }, {}, {}, {}};
        SendSkill s(down);
        const auto r = s.invoke(R"({"recipient":"abc","message":"hi"})");
        check(!okOf(r), "send refuses while the node is not started");
        check(errOf(r).find("not started") != std::string::npos, "and says why");
    }
    {
        std::string sentTopic;
        DeliveryPort up{[] { return true; },
                        [&](const std::string &t, const std::vector<std::uint8_t> &) {
                            sentTopic = t; return true; },
                        [](const std::string &) { return true; },
                        [](const std::string &) { return true; }};
        SendSkill s(up);
        check(okOf(s.invoke(R"({"recipient":"abc","message":"hi"})")), "send succeeds when up");
        check(sentTopic == ownerTopic("abc"), "and addresses the recipient's owner topic");

        // Malformed input must not throw out of invoke: skill failures are
        // required to stay isolated from the module.
        check(!okOf(s.invoke("not json")), "malformed json is refused, not thrown");
        check(!okOf(s.invoke(R"({"recipient":"abc"})")), "a missing field is refused");
        check(!okOf(s.invoke(R"({"recipient":"","message":"hi"})")), "an empty field is refused");
    }
    {
        bool created = false;
        int invites = 0;
        DeliveryPort up{[] { return true; },
                        [&](const std::string &, const std::vector<std::uint8_t> &) { ++invites; return true; },
                        [](const std::string &) { return true; },
                        [&](const std::string &) { created = true; return true; }};
        CreateGroupSkill g(up);
        const auto r = g.invoke(R"({"group_id":"g1","members":["a","b"]})");
        check(okOf(r) && created, "create_group opens a reliable channel");
        check(invites == 2, "and invites every member on their own topic");
        check(!okOf(g.invoke(R"({"group_id":"g1","members":[]})")), "an empty member list is refused");
    }

    std::printf("storage\n");
    {
        StoragePort down{[] { return false; }, {}, {}, {}, {}};
        UploadSkill u(down);
        check(!okOf(u.invoke(R"({"path":"/tmp/x"})")), "upload refuses while storage is down");
    }
    {
        StoragePort up{[] { return true; },
                       [](const std::string &, std::int64_t) { return std::string("cid-1"); },
                       [](const std::string &, const std::string &) { return true; },
                       [] { return std::string("[]"); },
                       [](const std::string &c) { return c == "cid-1"; }};
        UploadSkill u(up);
        const auto r = u.invoke(R"({"path":"/tmp/x","label":"notes"})");
        auto j = json::parse(r, nullptr, false);
        check(okOf(r) && j.value("address", std::string{}) == "cid-1", "upload returns the content address");

        DownloadSkill d(up);
        check(!okOf(d.invoke(R"({"address":"nope","path":"/tmp/y"})")),
              "download reports an unknown address as unknown");
        check(okOf(d.invoke(R"({"address":"cid-1","path":"/tmp/y"})")), "and succeeds on a known one");
    }
    {
        // share must refuse when it CANNOT check the address, not only when the
        // check comes back negative. Without these two, deleting the existence
        // check outright leaves the suite green while share reports ok:true for
        // an address nobody ever verified.
        SharePort willSend{[](const std::string &, const std::string &) { return true; }};

        StoragePort down{[] { return false; }, {}, {}, {},
                         [](const std::string &) { return true; }};
        ShareSkill sDown(down, willSend);
        check(!okOf(sDown.invoke(R"({"address":"cid-1","recipient":"bob"})")),
              "share refuses while the storage node is stopped");

        StoragePort noExists{[] { return true; }, {}, {}, {}, {}};
        ShareSkill sNo(noExists, willSend);
        check(!okOf(sNo.invoke(R"({"address":"cid-1","recipient":"bob"})")),
              "and refuses when it has no way to verify the address");
    }
    {
        // The point of share taking two ports: a delivery failure must not be
        // reported as a storage failure.
        StoragePort up{[] { return true; }, {}, {}, {},
                       [](const std::string &) { return true; }};
        SharePort broken{[](const std::string &, const std::string &) { return false; }};
        ShareSkill s(up, broken);
        const auto r = s.invoke(R"({"address":"cid-1","recipient":"bob"})");
        check(!okOf(r), "share fails when delivery fails");
        check(errOf(r).find("could not be delivered") != std::string::npos,
              "and blames delivery, not storage");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all skill behaviours hold");
    return failures ? 1 : 0;
}
