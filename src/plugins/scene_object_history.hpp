#pragma once
#include "scene_history.hpp"

namespace mikudancestudio::plugins {
// One shared owner per native object. The active scene owns the live object;
// this holder owns it while absent. Callers must use this lifecycle for every
// removal and release all holders before destroying the app/physics world.
class RetainedSceneObject : public std::enable_shared_from_this<RetainedSceneObject> {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit RetainedSceneObject(std::unique_ptr<Impl>);
    friend std::shared_ptr<RetainedSceneObject> RetainSceneObject(MMDApp&, uint32_t, unsigned);
public:
    ~RetainedSceneObject();
    bool Present() const noexcept;
    MmsId Id() const noexcept;
    // Prepared candidates retain this holder until publication cleanup or
    // cancellation. The app/world must outlive both holders and candidates.
    std::unique_ptr<PreparedHistoryChange> PreparePresence(bool present);
    // Incoming bindings/order are captured immediately before a deletion.
    // resourceBytes must include all retained allocations and shared resources
    // charged to this object; the holder does not estimate renderer memory.
    std::shared_ptr<HistoryChange> CaptureRemoval(uint64_t resourceBytes);
    // The loader holds the scene mutation gate and has not published UI/dirty
    // changes yet. Publish/destroy staging, then reserve/commit insertion in
    // the common journal. A failed reservation leaves the new object absent;
    // releasing its holders disposes it. Source files are never read by redo.
    MmsResult PrepareInsertion(uint64_t resourceBytes, std::shared_ptr<HistoryChange>& insertion,
                               std::unique_ptr<PreparedHistoryChange>& staging);
};
std::shared_ptr<RetainedSceneObject> RetainSceneObject(MMDApp&, uint32_t kind, unsigned slot);
}
