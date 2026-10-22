#pragma once
#include "storage_store.hpp"

namespace mikumikustudio::plugins {
struct PluginProject {
    std::string projectId, saveId, mainHash;
    StoredRecords records;
};
std::string StorageId();
std::string FileHash(const fs::path&);
std::string EncodeProject(const PluginProject&);
PluginProject DecodeProject(std::string_view);
fs::path PluginSidecar(const fs::path&);
// These take an exclusive same-directory lock. Recovery touches only paths
// derived from the requested project and a validated hexadecimal save ID.
void RecoverProjectPair(const fs::path& target);
PluginProject ReadPluginProject(const fs::path& target);
// stagedMain is a complete, closed PMM in the target directory. This function
// does not delete it. step is a fault-injection hook used only by tests.
void PublishProjectPair(const fs::path& target, const fs::path& stagedMain,
    PluginProject& data, bool createOnly, const std::function<void(unsigned)>& step = {},
    bool keepPrevious = false, bool numberedManualVersions = false);
}
