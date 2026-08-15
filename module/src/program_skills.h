#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_module_interface.h"

/**
 * @brief The three LEZ program skills: `program.query`, `program.call`,
 *        `program.deploy`.
 *
 * These are the blockchain skills that are not token transfers. They differ
 * from the storage and messaging skills in one important way: the chain gives
 * *less* information back than a normal service would, and most of the care
 * here is about not overstating what came back.
 *
 * Three facts about LEZ shape everything below.
 *
 * **Deployment is content addressed.** A program-deployment transaction hashes
 * to `SHA256(u32_le(len) || bytecode)` — a function of the binary alone. So the
 * expected transaction hash can be computed *before* submitting, and
 * `program.deploy` computes it rather than believing whatever the wallet prints.
 * That turns deployment into something verifiable instead of something trusted,
 * and it is also why redeploying an identical binary is a no-op rather than an
 * error.
 *
 * **A submitted hash is not a landed transaction.** `spel` prints `tx_hash:` as
 * soon as it has built and sent one, and separately prints
 * `Transaction NOT confirmed` and exits non-zero when it never lands. Grepping
 * only for the hash turns a reported failure into a silent success — that
 * mistake cost this repository a working afternoon (see the comment in
 * `scripts/deploy-agents.sh`), so `program.call` treats the exit status and the
 * NOT-confirmed line as authoritative and the hash as incidental.
 *
 * **The sequencer cannot tell you why something is missing.** Its JSON-RPC
 * exposes exactly five methods — `sendTransaction`, `getTransaction`,
 * `getBlock`, `getAccount`, `getLastBlockId` — and there is no pending, status
 * or mempool endpoint. `getTransaction` returns the same null for a dropped
 * transaction, a pending one, a rejected one, and one that was never submitted.
 * `program.query` therefore reports *not included*, and is careful never to
 * report *rejected*, because it has no way to know that.
 *
 * None of these skills enforces the spending threshold. That is deliberate and
 * is argued in `agent_module_interface.h`: the limit is enforced by the
 * anchored on-chain policy program, because a check made in this process could
 * be made differently by anyone holding this process.
 */
namespace logos::agent {

/// One command run to completion.
///
/// `output` is stdout and stderr together, as the shell scripts capture them
/// with `2>&1` — `spel` reports failure on both streams and splitting them
/// loses the correlation between the hash it printed and the verdict it
/// printed afterwards.
struct ProcessResult {
    int exitCode = -1;
    std::string output;
};

/// What the program skills need from the local LEZ toolchain. Named so they can
/// be exercised against a fake, and so the agent module does not shell out from
/// inside the skill itself.
struct ProgramPort {
    /// `spel --idl <idl> --program <programId> -- <instruction> [--flag value ...]`
    std::function<ProcessResult(const std::string &programId,
                                const std::string &instruction,
                                const std::vector<std::string> &args)>
        call;
    /// `wallet deploy-program <binaryPath>`
    std::function<ProcessResult(const std::string &binaryPath)> deploy;
    /// The bytes of a local file; empty when it could not be read.
    std::function<std::vector<std::uint8_t>(const std::string &binaryPath)> read;
};

/// The sequencer's JSON-RPC. It is the only read path this chain offers, and it
/// offers five methods; there is no status endpoint behind it.
struct SequencerPort {
    /// `rpc(method, paramsJson)` — the raw response body, or empty if the call
    /// did not complete. An empty string means "no answer", which is not the
    /// same as a `null` result and is not reported as one.
    std::function<std::string(const std::string &method, const std::string &paramsJson)> rpc;
};

/// Lowercase hex SHA-256 of `data`.
///
/// Implemented here rather than pulled in, so that verifying a deployment costs
/// no link-time dependency: a verification step that is awkward to build is a
/// verification step that gets skipped.
std::string sha256Hex(const std::vector<std::uint8_t> &data);

/// The transaction hash a LEZ deployment of `bytecode` will have:
/// `SHA256(u32_le(len) || bytecode)`.
///
/// Computable offline, which is the whole point — the caller can check the
/// chain for this hash before submitting anything, and can check afterwards
/// that what landed is the binary it meant to deploy.
std::string deployTxHash(const std::vector<std::uint8_t> &bytecode);

/// Whether `method` is one of the sequencer's read methods.
///
/// `sendTransaction` is excluded on purpose: it is a real method of this RPC,
/// but it writes, and a skill named `query` that can submit transactions is a
/// footgun aimed at the spending threshold.
bool isReadMethod(const std::string &method);

/// `program.query(program_id, params)` — reads state through the sequencer RPC.
class ProgramQuerySkill final : public ISkill {
public:
    explicit ProgramQuerySkill(SequencerPort seq) : seq_(std::move(seq)) {}
    std::string name() const override { return "program.query"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    SequencerPort seq_;
};

/// `program.call(program_id, instruction, params)` — submits a transaction to a
/// LEZ program through `spel`.
class ProgramCallSkill final : public ISkill {
public:
    explicit ProgramCallSkill(ProgramPort port) : port_(std::move(port)) {}
    std::string name() const override { return "program.call"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    ProgramPort port_;
};

/// `program.deploy(binary_path)` — deploys a compiled LEZ program.
///
/// Takes the sequencer too, and refuses without it. Deployment is the one
/// operation whose result can be predicted exactly, so submitting without
/// checking would be throwing away the only strong evidence available.
class ProgramDeploySkill final : public ISkill {
public:
    ProgramDeploySkill(ProgramPort port, SequencerPort seq)
        : port_(std::move(port)), seq_(std::move(seq))
    {
    }
    std::string name() const override { return "program.deploy"; }
    std::string parameterSchema() const override;
    std::string invoke(const std::string &paramsJson) override;

private:
    ProgramPort port_;
    SequencerPort seq_;
};

} // namespace logos::agent
