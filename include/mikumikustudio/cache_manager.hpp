#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio::cache_manager {
struct Settings {
    bool enabled=true, automaticCleanup=true;
    std::uint64_t limitBytes=4ull*1024*1024*1024;
    unsigned recentProjects=10;
};
struct Project {
    std::wstring path;
    std::uint64_t lastOpened=0, totalBytes=0, sharedBytes=0, reclaimableBytes=0;
    bool pinned=false, current=false;
};
struct Snapshot {
    Settings settings;
    std::uint64_t totalBytes=0, effectBytes=0, textureBytes=0, reclaimableBytes=0;
    std::vector<Project> projects;
    std::wstring error;
};
struct CleanupResult {
    std::uint64_t bytes=0;
    unsigned files=0, skipped=0;
    std::wstring error;
};
enum class Cleanup { Reclaimable, Project, All };
Settings Current() noexcept;
bool Enabled() noexcept;
bool Configure(const Settings&,std::wstring& error) noexcept;
std::filesystem::path Root() noexcept;
bool Managed(const std::filesystem::path&) noexcept;
Snapshot Inspect() noexcept;
CleanupResult Clean(Cleanup,const std::wstring& project={}) noexcept;
bool Pin(const std::wstring& project,bool pinned) noexcept;
void BeginProject(const wchar_t*) noexcept;
void ProjectOpened(const wchar_t*) noexcept;
void EndProjectLoad() noexcept;
void ResetScene() noexcept;
void Shutdown() noexcept;
void Touch(const std::filesystem::path&) noexcept;
void Invalidate(const std::filesystem::path&) noexcept;

// Holds a cross-process publication lock until the atomic cache-file rename.
// Only registered .fxb/.mtc files under the managed root can be reclaimed.
class WriteReservation {
public:
    WriteReservation(const std::filesystem::path&,std::uint64_t bytes) noexcept;
    ~WriteReservation();
    WriteReservation(WriteReservation&&) noexcept;
    WriteReservation& operator=(WriteReservation&&) noexcept;
    explicit operator bool() const noexcept;
    void Commit() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
