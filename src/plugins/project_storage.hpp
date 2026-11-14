#pragma once
#include "storage_store.hpp"
#include "../io/effect_pair_completion.hpp"
#include <stdexcept>

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
class ProjectSaveRecoveryError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
PluginProject ReadPluginProject(const fs::path& target);
// stagedMain is a complete, closed PMM in the target directory. This function
// does not delete it. step is a fault-injection hook used only by tests.
void PublishProjectPair(const fs::path& target, const fs::path& stagedMain,
    PluginProject& data, bool createOnly, const std::function<void(unsigned)>& step = {},
    bool keepPrevious = false);

// SAVE-02's private, capability-gated disk entry. The frozen request must
// name the final PMM and its actual EMM companion. Supplying plugin data
// includes its separate PMM-bound JSON in the same recoverable generation.
// No real provider currently enters this path. Retained publication is
// explicitly refused until its archive can share the same journal.
effect_save::EffectDigest FrozenEffectSha256(effect_save::EffectBytes bytes);
effect_save::PublishedPair PublishFrozenProjectPair(const fs::path& target, const fs::path& stagedMain,
    const effect_save::CapturedEffects& frozen, PluginProject* data,
    bool createOnly, bool keepPrevious, const std::function<void(unsigned)>& step = {});
}
