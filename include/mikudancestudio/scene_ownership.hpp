// ===========================================================================
// MikuMikuStudio - ownership of scene-resident heap allocations
// ===========================================================================
#pragma once

namespace mikudancestudio {

class MMDApp;

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

}  // namespace mikudancestudio
