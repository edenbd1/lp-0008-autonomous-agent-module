#pragma once

#include <cstdint>
#include <functional>
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
};

/// How a shared address reaches its recipient. Sharing a content address is a
/// messaging act; keeping it separate is what lets `storage.share` be honest
/// about which half failed.
struct SharePort {
    std::function<bool(const std::string &recipient, const std::string &message)> send;
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
