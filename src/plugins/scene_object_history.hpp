#pragma once
#include "scene_history.hpp"
#include "scene_resource_budget.hpp"
#include "mikudancestudio/accessory_import.hpp"
#include "mikudancestudio/model_import.hpp"
#include "mikudancestudio/import_texture_cache.hpp"
#include "mikudancestudio/history_physics.hpp"

namespace mikudancestudio::plugins {
// One shared owner per native object. The active scene owns the live object;
// this holder owns it while absent. Callers must use this lifecycle for every
// removal and release all holders before destroying the app/physics world.
class RetainedSceneObject : public std::enable_shared_from_this<RetainedSceneObject> {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit RetainedSceneObject(std::unique_ptr<Impl>);
    static MmsResult MeasureNativeResources(const std::vector<std::shared_ptr<RetainedSceneObject>>&,
                                           resource_budget::Collector&, bool);
    MmsResult MeasureBookkeepingImpl(resource_budget::Collector&, bool includePhysics, bool includeParkedKeys) const;
    static std::shared_ptr<HistoryChange> CaptureRemovalImpl(
        const std::vector<std::shared_ptr<RetainedSceneObject>>&, uint64_t, bool);
    static MmsResult PrepareInsertionImpl(
        const std::vector<std::shared_ptr<RetainedSceneObject>>&, uint64_t, bool,
        std::shared_ptr<HistoryChange>&, std::unique_ptr<PreparedHistoryChange>&);
    friend std::shared_ptr<RetainedSceneObject> RetainSceneObject(MMDApp&, uint32_t, unsigned);
    static std::shared_ptr<HistoryChange> MakeImport(std::shared_ptr<RetainedSceneObject>,
        std::shared_ptr<import_texture_cache::Retained>, std::unique_ptr<import_texture_cache::Publication>,
        std::unique_ptr<history_physics::ImportScene::Publication>);
public:
    using Objects = std::vector<std::shared_ptr<RetainedSceneObject>>;
    ~RetainedSceneObject();
    bool Present() const noexcept;
    MmsId Id() const noexcept;
    // Measure native allocations using this owner's object identity, including
    // parked accessory keys/curves. The scene mutation gate must be held.
    // Report::missing distinguishes incomplete measurements; no fixed fallback
    // charge is invented. Includes owner bookkeeping; record and incoming
    // binding charges remain CaptureRemoval's job.
    MmsResult MeasureResources(resource_budget::Report&) const noexcept;
    static MmsResult MeasureResourcesBatch(const Objects&, resource_budget::Report&) noexcept;
    // Measure allocations retained by this published holder, excluding the
    // active object's model/mesh/Bullet resources. Includes parked accessory
    // keys, authoring state and physics lifetime containers. Reuse one Collector
    // across all holders in a change so shared allocation identities count once.
    // Call on the owning UI thread while native edits/teardown are excluded.
    // No seek, evaluation, plugin callback or graphics-resource query is made.
    // Unpublished private imports return NOT_READY. On any failure discard the
    // partial collector. Check Result().Complete() before budget admission.
    MmsResult MeasureBookkeeping(resource_budget::Collector&) const noexcept;
    // Validate the recorded identities/capacities without allocating or changing
    // the ledger. Growth, replacement, incomplete data or stale membership fails.
    // The collector is a payload estimate, not a process/driver-memory bound.
    bool CoversBookkeeping(resource_budget::Collector&) const noexcept;
    // Prepared candidates retain this holder until publication cleanup or
    // cancellation. The app/world must outlive both holders and candidates.
    std::unique_ptr<PreparedHistoryChange> PreparePresence(bool present);
    // One world/cache transition for the complete set. These functions must
    // be used instead of composing independently prepared object candidates.
    static std::unique_ptr<PreparedHistoryChange> PreparePresenceBatch(const Objects&, bool present,
        std::shared_ptr<resource_budget::Collector> targetResources = {});
    static std::shared_ptr<HistoryChange> CaptureRemovalBatch(const Objects&, uint64_t resourceBytes);
    // Incoming bindings/order are captured immediately before a deletion.
    // resourceBytes must include all retained allocations and shared resources
    // charged to this object; the holder does not estimate renderer memory.
    std::shared_ptr<HistoryChange> CaptureRemoval(uint64_t resourceBytes);
    // Production admission: measure the complete target resources on every
    // Prepare (initial deletion, undo and redo). Unknown resources reject.
    // The journal uses the candidate charge, including growth since capture.
    std::shared_ptr<HistoryChange> CaptureRemoval();
    static std::shared_ptr<HistoryChange> CaptureRemovalBatch(const Objects&);
    // The loader holds the scene mutation gate and has not published UI/dirty
    // changes yet. Publish/destroy staging, then reserve/commit insertion in
    // the common journal. A failed reservation leaves the new object absent;
    // releasing its holders disposes it. Source files are never read by redo.
    MmsResult PrepareInsertion(uint64_t resourceBytes, std::shared_ptr<HistoryChange>& insertion,
                               std::unique_ptr<PreparedHistoryChange>& staging);
    static MmsResult PrepareInsertionBatch(const Objects&, uint64_t resourceBytes,
        std::shared_ptr<HistoryChange>& insertion, std::unique_ptr<PreparedHistoryChange>& staging);
    // Measured production overloads: staging and every journal direction must
    // have complete resource measurements. Failures leave both outputs and
    // scene membership unchanged. The insertion candidate includes its own
    // record wrapper in the target charge; it never substitutes a fixed cap.
    MmsResult PrepareInsertion(std::shared_ptr<HistoryChange>& insertion,
                               std::unique_ptr<PreparedHistoryChange>& staging);
    static MmsResult PrepareInsertionBatch(const Objects&,
        std::shared_ptr<HistoryChange>& insertion, std::unique_ptr<PreparedHistoryChange>& staging);
    // Adopt private parser output without ever parking an already-live object.
    // On failure the parser output keeps ownership. On success the returned
    // record owns it, including cancellation before its first publication.
    static std::shared_ptr<HistoryChange> ImportAccessory(MMDApp&, unsigned, accessory_import::Prepared&,
        std::shared_ptr<import_texture_cache::Retained>, std::unique_ptr<import_texture_cache::Publication>);
    static std::shared_ptr<HistoryChange> ImportModel(MMDApp&, unsigned, model_import::Prepared&,
        const std::shared_ptr<history_physics::ImportScene>&, std::unique_ptr<history_physics::ImportScene::Publication>,
        std::shared_ptr<import_texture_cache::Retained>, std::unique_ptr<import_texture_cache::Publication>);
};
std::shared_ptr<RetainedSceneObject> RetainSceneObject(MMDApp&, uint32_t kind, unsigned slot);
}
