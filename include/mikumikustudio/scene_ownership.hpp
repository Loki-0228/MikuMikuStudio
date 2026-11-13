// ===========================================================================
// MikuMikuStudio - ownership of scene-resident heap allocations
// ===========================================================================
#pragma once

namespace mikumikustudio {

class MMDApp;

#if !MMD_ORIGINAL_PROFILE
constexpr unsigned kSceneCloseReplayMessage = 0x8000u + 0x3f0u;
// Holds close requests across the entire PMM transaction, including plugin
// cleanup which runs after the native scene mutation scope ends.
class ScopedSceneCloseDeferral {
public:
    explicit ScopedSceneCloseDeferral(MMDApp& app) noexcept;
    ~ScopedSceneCloseDeferral();
    ScopedSceneCloseDeferral(const ScopedSceneCloseDeferral&) = delete;
    ScopedSceneCloseDeferral& operator=(const ScopedSceneCloseDeferral&) = delete;
private:
    MMDApp& app_;
};
// Safe-point retry after a transient PostMessage failure.
void RetryDeferredSceneCloses(MMDApp& app) noexcept;
bool SceneCloseBlocked(MMDApp& app) noexcept;
#endif

// Modal loaders and UI refreshes pump window messages while scene allocations
// are being replaced. This guard cannot be cleared by WM_SIZE/WM_LBUTTONUP,
// unlike the legacy windowLayoutReady flag, and supports nested load/reset.
class ScopedSceneMutation {
public:
    explicit ScopedSceneMutation(MMDApp& app);
    ~ScopedSceneMutation();
    ScopedSceneMutation(const ScopedSceneMutation&) = delete;
    ScopedSceneMutation& operator=(const ScopedSceneMutation&) = delete;
private:
    MMDApp& app_;
    int previousLayoutReady_;
};

// Preserve the reference executable's release order.  These are raw,
// zero-initialized blocks, so their matching release operation is free().
void ReleaseSceneModels(MMDApp& app);
void ReleaseGlobalTimelineTracks(MMDApp& app);
void ReleaseAccessoriesAndTracks(MMDApp& app);

}  // namespace mikumikustudio
