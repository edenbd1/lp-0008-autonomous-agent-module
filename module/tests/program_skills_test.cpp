// Exercise the three LEZ program skills against fakes, with no node running.
//
// A live sequencer proves that a transaction can land. It cannot prove any of
// the things that actually go wrong here, because they are all cases where
// something is *missing*: spel printed a hash and then said the transaction was
// never confirmed; the wallet deployed bytes that are not the bytes on disk; the
// RPC returned null and the null means four different things. Those are the
// behaviours a reviewer cannot see from a screenshot and the ones a plausible
// stub gets wrong, so they are driven from here.
//
// Every check below is written so that deleting the guard it covers turns this
// suite red. Where a check exists only for that reason, the comment says so.
#include "../src/program_skills.h"

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

static bool okOf(const std::string &r)
{
    return parsed(r).value("ok", false);
}

static std::string errOf(const std::string &r)
{
    return parsed(r).value("error", std::string{});
}

static std::vector<std::uint8_t> bytesOf(const std::string &s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

/// A sequencer that answers one canned body regardless of the question, and
/// records what it was asked.
struct FakeSequencer {
    std::string body;
    std::string lastMethod;
    std::string lastParams;
    int calls = 0;

    SequencerPort port()
    {
        return SequencerPort{[this](const std::string &m, const std::string &p) {
            lastMethod = m;
            lastParams = p;
            ++calls;
            return body;
        }};
    }
};

static std::string rpcResult(const std::string &resultJson)
{
    return R"({"jsonrpc":"2.0","id":1,"result":)" + resultJson + "}";
}

int main()
{
    std::printf("hashing\n");
    {
        // Known-answer vectors. The deploy skill is only worth anything if the
        // hash it computes is the hash the chain computes, so the primitive is
        // pinned rather than assumed — including both padding paths, since a
        // message whose tail leaves no room for the length needs a second
        // block and that is the classic place a hand-written SHA-256 is wrong.
        check(sha256Hex(bytesOf(""))
                  == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "sha256 of the empty string");
        check(sha256Hex(bytesOf("abc"))
                  == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "sha256 of \"abc\"");
        check(sha256Hex(bytesOf(std::string(55, 'a')))
                  == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
              "sha256 at 55 bytes, one padding block");
        check(sha256Hex(bytesOf(std::string(56, 'a')))
                  == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
              "sha256 at 56 bytes, where the length spills into a second block");
        check(sha256Hex(bytesOf(std::string(64, 'a')))
                  == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
              "sha256 at exactly one block");
        check(sha256Hex(bytesOf(std::string(1000, 'a')))
                  == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
              "sha256 over many blocks");

        // SHA256(u32_le(len) || bytecode). The length prefix is what makes this
        // different from a plain digest of the file, and dropping it would
        // still produce a well-formed 64-hex string that matches nothing.
        const std::vector<std::uint8_t> four{0xde, 0xad, 0xbe, 0xef};
        check(deployTxHash(four)
                  == "26ea22a97ba0d3f6ceab2dfc3e3e7a31e7fc618262c71ba5fd6f43956cc8c84f",
              "the deploy hash frames the bytecode with its little-endian length");
        check(deployTxHash({}) == "df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119",
              "and an empty program still hashes its zero length");
        check(deployTxHash(four) != sha256Hex(four),
              "so it is not the digest of the bytecode alone");
    }

    std::printf("program.query\n");
    {
        FakeSequencer seq;
        seq.body = rpcResult("null");
        ProgramQuerySkill q(seq.port());

        check(!okOf(q.invoke("not json")), "malformed json is refused, not thrown");
        check(!okOf(q.invoke(R"({"method":"getTransaction"})")), "a missing program_id is refused");
        check(!okOf(q.invoke(R"({"program_id":"p1"})")), "a missing method is refused");
        check(!okOf(q.invoke(R"({"program_id":"","method":"getTransaction"})")),
              "an empty program_id is refused");
        check(seq.calls == 0, "and none of those reached the sequencer");
    }
    {
        // The five methods this RPC has, minus the one that writes. A `query`
        // that can submit is a hole straight through the spending threshold.
        FakeSequencer seq;
        seq.body = rpcResult("null");
        ProgramQuerySkill q(seq.port());
        const auto r = q.invoke(R"({"program_id":"p1","method":"sendTransaction","params":["x"]})");
        check(!okOf(r), "query refuses sendTransaction even though the RPC has it");
        check(errOf(r).find("program.call") != std::string::npos, "and says where to go instead");
        check(seq.calls == 0, "without submitting anything");

        check(!okOf(q.invoke(R"({"program_id":"p1","method":"getStatus","params":[]})")),
              "a method this sequencer does not have is refused");
        check(!okOf(q.invoke(R"({"program_id":"p1","method":"getAccount"})")),
              "getAccount without params is refused rather than guessed at");
    }
    {
        // The case the whole skill exists for: an absent hash. The query
        // succeeded; the transaction is not there; and this RPC cannot say why.
        // Reporting anything stronger than absence would be inventing a verdict.
        FakeSequencer seq;
        seq.body = rpcResult("null");
        ProgramQuerySkill q(seq.port());
        const auto r = q.invoke(
            R"({"program_id":"p1","method":"getTransaction","params":["00ff"]})");
        const json j = parsed(r);
        check(okOf(r), "a query for an absent hash is a successful query");
        check(j.contains("included") && j["included"].is_boolean() && !j["included"].get<bool>(),
              "and reports the transaction as not included");
        check(!j.contains("result"), "with no result invented for it");
        // Deleting the honesty note, or replacing absence with a status, must
        // fail here: on this chain a null covers dropped, pending, refused and
        // never-submitted alike.
        check(r.find("reject") == std::string::npos,
              "and never calls an absent transaction rejected");
        check(j.value("note", std::string{}).find("pending") != std::string::npos,
              "it says what else the null could mean");
        check(seq.lastParams == R"(["00ff"])", "the hash asked about is the hash passed");
    }
    {
        // "We could not ask" must never be reported as "it is not there". If
        // this collapses into the absent case, a sequencer outage starts
        // reading as a chain that rejected everything.
        FakeSequencer silent;
        silent.body = "";
        ProgramQuerySkill q(silent.port());
        const auto r = q.invoke(R"({"program_id":"p1","method":"getTransaction","params":["00ff"]})");
        const json j = parsed(r);
        check(!okOf(r), "a sequencer that does not answer is an error, not an absence");
        check(j.find("included") == j.end(), "and no inclusion verdict is reported for it");

        FakeSequencer garbage;
        garbage.body = "<html>502</html>";
        ProgramQuerySkill q2(garbage.port());
        check(!okOf(q2.invoke(R"({"program_id":"p1","method":"getTransaction","params":["00ff"]})")),
              "a body that is not JSON-RPC is an error too");

        FakeSequencer refused;
        // The message the sequencer sends has to be a string that could not have
        // come from anywhere else. It was `"no"` — two characters that occur in
        // `not`, `nothing`, `node` and in this module's own `no sequencer to
        // query` — so `errOf(r3).find("no")` was satisfied by every refusal the
        // module can emit. Replacing the line in program_skills.cpp that appends
        // the sequencer's message with a canned sentence of the module's own
        // left this assertion printing `ok` and the whole suite green.
        refused.body =
            R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,)"
            R"("message":"zq-sequencer-said-this-and-nothing-else-did"}})";
        ProgramQuerySkill q3(refused.port());
        const auto r3 = q3.invoke(R"({"program_id":"p1","method":"getBlock","params":[1]})");
        check(!okOf(r3), "an RPC error is surfaced as an error");
        check(errOf(r3).find("zq-sequencer-said-this-and-nothing-else-did")
                  != std::string::npos,
              "carrying the sequencer's own message");
    }
    {
        FakeSequencer seq;
        seq.body = rpcResult(R"([{"block":8590}])");
        ProgramQuerySkill q(seq.port());
        const auto r = q.invoke(R"({"program_id":"deadbeef","method":"getTransaction"})");
        const json j = parsed(r);
        check(okOf(r) && j.value("included", false), "a present transaction is reported included");
        check(j.contains("result"), "with the sequencer's own result passed through");
        check(seq.lastParams == R"(["deadbeef"])",
              "and getTransaction with no params asks after the program itself");
        check(seq.lastMethod == "getTransaction", "using the method it was given");
    }

    std::printf("program.call\n");
    {
        ProgramPort unwired{};
        ProgramCallSkill c(unwired);
        check(!okOf(c.invoke("{")), "malformed json is refused, not thrown");
        check(!okOf(c.invoke(R"({"program_id":"p.bin"})")), "a missing instruction is refused");
        check(!okOf(c.invoke(R"({"instruction":"spend"})")), "a missing program_id is refused");
        check(!okOf(c.invoke(R"({"program_id":"p.bin","instruction":"spend"})")),
              "and with no spel wired it refuses rather than claiming a call");
    }
    {
        std::vector<std::string> seen;
        std::string seenProgram, seenInstruction;
        ProgramPort ok{[&](const std::string &prog, const std::string &instr,
                           const std::vector<std::string> &args) {
                           seenProgram = prog;
                           seenInstruction = instr;
                           seen = args;
                           return ProcessResult{0, "submitting\ntx_hash: " + std::string(64, 'a')
                                                       + "\nTransaction confirmed\n"};
                       },
                       {}, {}};
        ProgramCallSkill c(ok);
        const auto r = c.invoke(
            R"({"program_id":"agent_verifier.bin","instruction":"create_policy",)"
            R"("params":{"policy_hash":"ab12","per_tx":50,"dry_run":true}})");
        const json j = parsed(r);
        check(okOf(r), "a confirmed call succeeds");
        check(j.value("tx", std::string{}) == std::string(64, 'a'),
              "and returns the hash spel printed");
        check(j.value("confirmed", false), "marked confirmed");
        check(seenProgram == "agent_verifier.bin" && seenInstruction == "create_policy",
              "the program and instruction reach spel unaltered");
        // The IDL declares snake_case argument names; spel takes them as
        // kebab-case flags. Passing them through unchanged produces flags spel
        // does not know and an instruction that never builds.
        std::string joined;
        for (const auto &a : seen) joined += a + " ";
        check(joined == "--dry-run true --per-tx 50 --policy-hash ab12 ",
              "and snake_case params become spel's kebab-case flags, values beside them");

        check(!okOf(c.invoke(R"({"program_id":"p.bin","instruction":"i","params":[1,2]})")),
              "params that are not an object are refused");
        check(!okOf(c.invoke(
                  R"({"program_id":"p.bin","instruction":"i","params":{"a":{"b":1}}})")),
              "a nested param is refused: spel takes flags, not structures");
        check(!okOf(c.invoke(
                  R"({"program_id":"p.bin","instruction":"i","params":{"a b":"1"}})")),
              "a param name that would smuggle a second flag is refused");

        // THE NAME CHECK ALONE WAS NOT ENOUGH.
        //
        // A value lands in argv immediately after its flag and spel reads argv
        // positionally, so a value that looks like a flag *is* one. Measured
        // before the fix: `{"amount":"--bin-auth-transfer"}` passed the name
        // check and pushed `[--amount] [--bin-auth-transfer]` — putting the flag
        // that decides which program's ELF is admitted into the proof onto the
        // command line the caller never asked for.
        seen.clear();
        const auto smuggled = c.invoke(
            R"({"program_id":"p.bin","instruction":"spend",)"
            R"("params":{"amount":"--bin-auth-transfer"}})");
        check(!okOf(smuggled), "a param value that is itself a flag is refused");
        check(parsed(smuggled).value("error", std::string{}).find("--bin-auth-transfer") !=
                  std::string::npos,
              "and the refusal quotes the value, so an operator sees what was attempted");
        check(seen.empty(), "spel is never invoked, so the flag never reaches argv");

        check(!okOf(c.invoke(
                  R"({"program_id":"p.bin","instruction":"i","params":{"amount":-5}})")),
              "a negative number is refused for the same reason: argv cannot tell it from a flag");
        check(!okOf(c.invoke(
                  R"({"program_id":"p.bin","instruction":"i","params":{"a":"-"}})")),
              "and so is a bare dash");
        check(okOf(c.invoke(
                  R"({"program_id":"p.bin","instruction":"i","params":{"a":"x-y","b":"5"}})")),
              "while a dash inside a value is ordinary text and still goes through");
    }
    {
        // The failure this repository actually hit. spel prints tx_hash the
        // moment it submits, then prints its verdict. Reading only the hash
        // reports a landed transaction that never landed.
        ProgramPort never{[](const std::string &, const std::string &,
                             const std::vector<std::string> &) {
                              return ProcessResult{1, "tx_hash: " + std::string(64, 'b')
                                                          + "\nTransaction NOT confirmed\n"};
                          },
                          {}, {}};
        ProgramCallSkill c(never);
        const auto r = c.invoke(R"({"program_id":"p.bin","instruction":"spend"})");
        const json j = parsed(r);
        check(!okOf(r), "a call whose transaction never lands is a failure");
        check(!j.value("confirmed", true), "reported as not confirmed");
        check(j.value("submitted_tx", std::string{}) == std::string(64, 'b'),
              "and the submitted hash is still handed back, under a name that does not "
              "claim inclusion");
        check(j.find("tx") == j.end(), "never as a plain 'tx'");
    }
    {
        // Two guards, checked one at a time. Without the exit-status guard the
        // first of these goes green; without the NOT-confirmed guard, the
        // second does.
        ProgramPort badExit{[](const std::string &, const std::string &,
                               const std::vector<std::string> &) {
                                return ProcessResult{1, "tx_hash: " + std::string(64, 'c') + "\n"};
                            },
                            {}, {}};
        ProgramCallSkill c1(badExit);
        check(!okOf(c1.invoke(R"({"program_id":"p.bin","instruction":"spend"})")),
              "a non-zero exit is a failure even when a hash was printed");

        ProgramPort quietlyUnconfirmed{
            [](const std::string &, const std::string &, const std::vector<std::string> &) {
                return ProcessResult{0, "tx_hash: " + std::string(64, 'd')
                                            + "\nTransaction NOT confirmed\n"};
            },
            {}, {}};
        ProgramCallSkill c2(quietlyUnconfirmed);
        check(!okOf(c2.invoke(R"({"program_id":"p.bin","instruction":"spend"})")),
              "and NOT confirmed is a failure even when the exit status is clean");

        ProgramPort nothing{[](const std::string &, const std::string &,
                               const std::vector<std::string> &) {
                                return ProcessResult{0, "built the instruction\n"};
                            },
                            {}, {}};
        ProgramCallSkill c3(nothing);
        check(!okOf(c3.invoke(R"({"program_id":"p.bin","instruction":"spend"})")),
              "a clean exit that submitted nothing is a failure");

        ProgramPort truncated{[](const std::string &, const std::string &,
                                 const std::vector<std::string> &) {
                                  return ProcessResult{0, "tx_hash: abc123\n"};
                              },
                              {}, {}};
        ProgramCallSkill c4(truncated);
        check(!okOf(c4.invoke(R"({"program_id":"p.bin","instruction":"spend"})")),
              "and a short hash is not accepted as one");
    }

    std::printf("program.deploy\n");
    {
        const std::vector<std::uint8_t> code = bytesOf("PROGRAM BYTES");
        const std::string expected = deployTxHash(code);

        auto reader = [code](const std::string &) { return code; };

        {
            FakeSequencer seq;
            seq.body = rpcResult("null");
            ProgramPort p{{}, {}, reader};
            ProgramDeploySkill d(p, seq.port());
            check(!okOf(d.invoke("nope")), "malformed json is refused, not thrown");
            check(!okOf(d.invoke(R"({})")), "a missing binary_path is refused");
            check(!okOf(d.invoke(R"({"binary_path":""})")), "an empty binary_path is refused");
        }
        {
            FakeSequencer seq;
            seq.body = rpcResult("null");
            bool submitted = false;
            ProgramPort empty{{},
                              [&](const std::string &) {
                                  submitted = true;
                                  return ProcessResult{0, ""};
                              },
                              [](const std::string &) { return std::vector<std::uint8_t>{}; }};
            ProgramDeploySkill d(empty, seq.port());
            check(!okOf(d.invoke(R"({"binary_path":"/no/such.bin"})")),
                  "a binary that cannot be read is refused");
            // Not just refused — refused before submitting. Zero bytes hash to a
            // perfectly valid content address, so a deploy that reads nothing
            // and carries on would submit an empty program under a hash that
            // looks exactly as legitimate as any other.
            check(!submitted, "before anything is submitted");
        }
        {
            // Refuse when the check cannot be made, not only when it fails. The
            // sibling lesson from storage.share: without this, deploy still
            // shells out and still returns a hash, and that hash is a claim
            // about a chain nobody consulted.
            bool submitted = false;
            ProgramPort p{{}, [&](const std::string &) {
                              submitted = true;
                              return ProcessResult{0, ""};
                          },
                          reader};
            ProgramDeploySkill d(p, SequencerPort{});
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            check(!okOf(r), "deploy refuses with no sequencer to verify against");
            check(!submitted, "and does not submit what it could not have checked");
            check(parsed(r).value("expected_tx", std::string{}) == expected,
                  "while still reporting the content address it computed");
        }
        {
            FakeSequencer silent;
            silent.body = "";
            bool submitted = false;
            ProgramPort p{{}, [&](const std::string &) {
                              submitted = true;
                              return ProcessResult{0, ""};
                          },
                          reader};
            ProgramDeploySkill d(p, silent.port());
            check(!okOf(d.invoke(R"({"binary_path":"/p.bin"})")),
                  "a sequencer that does not answer stops the deployment");
            check(!submitted, "before it is submitted, not after");
        }
        {
            // Content addressing makes a redeploy a no-op rather than a
            // duplicate. Getting this wrong costs a real deployment fee for
            // nothing, every time an idempotent script runs.
            FakeSequencer present;
            present.body = rpcResult(R"([{"block":1}])");
            bool submitted = false;
            ProgramPort p{{}, [&](const std::string &) {
                              submitted = true;
                              return ProcessResult{0, ""};
                          },
                          reader};
            ProgramDeploySkill d(p, present.port());
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            const json j = parsed(r);
            check(okOf(r) && j.value("already_deployed", false),
                  "a binary already on chain is reported as already deployed");
            check(!submitted, "and is not deployed a second time");
            check(present.lastParams == "[\"" + expected + "\"]",
                  "the chain was asked about the hash computed from the binary");
        }
        {
            // The check that makes deployment verifiable instead of trusted. If
            // the toolchain names a hash at all it must be this one; a
            // different hash means the bytes on the wire are not the bytes on
            // disk, and reporting the computed hash anyway would produce a
            // record that verifies against a binary nobody deployed.
            FakeSequencer seq;
            seq.body = rpcResult("null");
            const std::string wrong(64, 'e');
            ProgramPort p{{}, [&](const std::string &) {
                              return ProcessResult{0, "tx_hash: " + wrong + "\n"};
                          },
                          reader};
            ProgramDeploySkill d(p, seq.port());
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            const json j = parsed(r);
            check(!okOf(r), "a reported hash that disagrees with the computed one is refused");
            check(j.value("reported_tx", std::string{}) == wrong
                      && j.value("expected_tx", std::string{}) == expected,
                  "with both hashes shown, so the disagreement can be read");
            check(j.find("tx") == j.end(), "and no transaction is claimed");
        }
        {
            FakeSequencer seq;
            seq.body = rpcResult("null");
            ProgramPort p{{}, [](const std::string &) {
                              return ProcessResult{1, "wallet: could not sign\n"};
                          },
                          reader};
            ProgramDeploySkill d(p, seq.port());
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            check(!okOf(r), "a wallet that refuses the deployment is a failure");
            check(errOf(r).find("could not sign") != std::string::npos,
                  "carrying the wallet's own words");
        }
        {
            // Submitted and not there yet. Blocks are a minute apart and this
            // skill does not sleep, so the only honest report is absence — and
            // absence on this chain is not a verdict.
            FakeSequencer seq;
            seq.body = rpcResult("null");
            ProgramPort p{{}, [](const std::string &) { return ProcessResult{0, ""}; }, reader};
            ProgramDeploySkill d(p, seq.port());
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            const json j = parsed(r);
            check(!okOf(r), "a deployment the chain has not included is not a success");
            check(j.value("submitted_tx", std::string{}) == expected,
                  "the hash it will have is reported, so the caller can poll for it");
            check(r.find("reject") == std::string::npos,
                  "and it is not called rejected, which this RPC cannot support");
        }
        {
            // The happy path, last: everything above has to be able to fail
            // while this still passes, or the checks are just noise.
            int asked = 0;
            SequencerPort seq{[&](const std::string &, const std::string &) {
                // Absent before the deployment, present after it.
                return rpcResult(++asked == 1 ? "null" : R"([{"block":8590}])");
            }};
            std::string deployedPath;
            ProgramPort p{{}, [&](const std::string &path) {
                              deployedPath = path;
                              return ProcessResult{0, "deploying\ntx_hash: " + deployTxHash(code)
                                                          + "\n"};
                          },
                          reader};
            ProgramDeploySkill d(p, seq);
            const auto r = d.invoke(R"({"binary_path":"/p.bin"})");
            const json j = parsed(r);
            check(okOf(r), "a deployment that lands succeeds");
            check(j.value("tx", std::string{}) == expected
                      && j.value("program_id", std::string{}) == expected,
                  "identified by the content address computed from the binary");
            check(j.value("confirmed", false) && !j.value("already_deployed", true),
                  "confirmed, and not mistaken for one that was already there");
            check(deployedPath == "/p.bin", "and the path given is the path deployed");
            check(asked == 2, "with the chain consulted both before and after");
        }
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all program skill behaviours hold");
    return failures ? 1 : 0;
}
