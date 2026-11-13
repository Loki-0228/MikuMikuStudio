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
enum class SceneOperation { Load, Reset };
enum class SceneOutcome { Loaded, ResetComplete, Failed };
class SceneToken {
public:
    SceneToken() = default;
    explicit operator bool() const noexcept { return serial_ != 0; }
private:
    explicit SceneToken(std::uint64_t serial) noexcept : serial_(serial) {}
    std::uint64_t serial_ = 0;
    friend SceneToken AcceptScene(SceneOperation) noexcept;
    friend bool FinishScene(SceneToken, SceneOutcome, const wchar_t*) noexcept;
    friend SceneToken CaptureActiveScene() noexcept;
    friend bool BindSavedScene(SceneToken, const wchar_t*) noexcept;
};
// Accept only after replacement is authorized. A refused reset issues no token.
// Finish returns true for a matching local transition. Loaded takes the final
// project path. A successful Loaded retains verified uses until index settlement.
// A preparation allocation failure is retryable. If a loading Touch could not
// retain its proof, finish as Failed and start a new attempt.
SceneToken AcceptScene(SceneOperation) noexcept;
bool FinishScene(SceneToken, SceneOutcome, const wchar_t* loadedPath = nullptr) noexcept;

// Capture at save start. Only call BindSavedScene after the document accepts
// its final path (ordinary save/save-as, in publication order). No disk I/O
// is required to settle a local bind; the verified index update can follow.
// A stale ticket is inert. Preparation failure for the active ticket leaves
// it anonymous with its uses intact, so later touches cannot accrue to the
// former path. Retry only for the currently accepted document path.
SceneToken CaptureActiveScene() noexcept;
bool BindSavedScene(SceneToken, const wchar_t* savedPath) noexcept;

// A legacy BeginProject starts a legacy sequence. Its unticketed completions
// apply only to that sequence once a ticketed operation has taken ownership.
void BeginProject(const wchar_t*) noexcept;
void ProjectOpened(const wchar_t*) noexcept;
void EndProjectLoad() noexcept;
void ResetScene() noexcept;
void Shutdown() noexcept;
void Touch(const std::filesystem::path&) noexcept;
void Invalidate(const std::filesystem::path&) noexcept;

// A single restoration may reuse a successful read-side use of the same file.
// The receipt never extends the cache lease or the lifetime of a scene.
class TextureUseReceipt {
public:
    TextureUseReceipt() noexcept = default;
    TextureUseReceipt(TextureUseReceipt&&) noexcept = default;
    TextureUseReceipt& operator=(TextureUseReceipt&&) noexcept = default;
    TextureUseReceipt(const TextureUseReceipt&) = delete;
    TextureUseReceipt& operator=(const TextureUseReceipt&) = delete;
private:
    struct Impl;
    std::shared_ptr<const Impl> proof_;
    friend TextureUseReceipt TouchTextureRead(const std::filesystem::path&, std::intptr_t) noexcept;
    friend void FinishTextureUse(const std::filesystem::path&, TextureUseReceipt) noexcept;
};
TextureUseReceipt TouchTextureRead(const std::filesystem::path&, std::intptr_t readHandle) noexcept;
void FinishTextureUse(const std::filesystem::path&, TextureUseReceipt) noexcept;

// Holds a cross-process publication lock until the atomic cache-file rename.
// Only registered .fxb/.mtc files under the managed root can be reclaimed.
// Thread-affine: construction, moves, Commit and destruction must all occur on
// the same thread, because the reservation owns a Windows mutex acquisition.
class WriteReservation {
public:
    WriteReservation(const std::filesystem::path&,std::uint64_t bytes) noexcept;
    ~WriteReservation();
    WriteReservation(WriteReservation&&) noexcept;
    WriteReservation& operator=(WriteReservation&&) noexcept;
    explicit operator bool() const noexcept;
    // Register this writer's CREATE_NEW empty temporary file while its original
    // handle is still open. It must be beside the final target and use that
    // target's filename followed by .tmp-. Associates physical identity with
    // the existing reservation; it grants no extra capacity or deletion right.
    // Unmanaged destinations are a successful no-op. Failure preserves token.
    bool TrackTemporary(const std::filesystem::path&) noexcept;
    // Managed writers must first TrackTemporary, then publish that same file
    // identity at the final name. Call only after publishing the final file.
    // Failure retains a conservative
    // charge until registration can be persisted; destruction is not a refund
    // for a published but unregistered file. May be retried on the same token.
    bool Commit() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
