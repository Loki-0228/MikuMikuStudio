#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace mikumikustudio::cache_policy {
// Policy persistence only. A valid policy is not a disk-write reservation or
// permission to delete a file. Storage/leases and consumers are separate work.
enum class Result { Ok, Invalid, Busy, Io, Conflict, Disabled };
struct Configuration {
    bool enabled = true;
    bool automaticCleanup = true;
    std::uint32_t limitMiB = 4096;
    std::uint32_t recentProjects = 10;
    std::uint64_t LimitBytes() const noexcept { return std::uint64_t(limitMiB) * 1024 * 1024; }
};
bool Valid(const Configuration&) noexcept;

class Store;
class Snapshot {
public:
    bool IsValid() const noexcept { return valid_; }
    const Configuration& Value() const noexcept { return value_; }
private:
    friend class Store;
    void Swap(Snapshot&) noexcept;
    Configuration value_{false, false, 4096, 10};
    std::filesystem::path root_, settings_;
    std::string section_;
    bool present_ = false, valid_ = false;
};

class Store {
public:
    Store(std::filesystem::path absoluteCacheRoot, std::filesystem::path absoluteSettingsFile);
    const std::filesystem::path& Root() const noexcept { return root_; }
    // Missing settings use defaults. Every other failure preserves output.
    Result Read(Snapshot& output) const noexcept;
    // Compare with the exact section read by expected. Preserve unknown fields,
    // reject stale/cross-store snapshots, and publish output only after save.
    // expected and output may be the same object.
    Result Commit(const Snapshot& expected, const Configuration& desired, Snapshot& output) const noexcept;
private:
    std::filesystem::path root_, settings_;
};

// Per-installation production entry points: EXE-directory/data/cache and
// data/settings.json only. Original compatibility builds remain disabled.
std::filesystem::path Root() noexcept;
Result ReadCurrent(Snapshot& output) noexcept;
Result Configure(const Snapshot& expected, const Configuration& desired, Snapshot& output) noexcept;
bool Enabled() noexcept;
}
