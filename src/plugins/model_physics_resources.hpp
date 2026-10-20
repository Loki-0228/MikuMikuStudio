#pragma once
#include "scene_history.hpp"
#include "scene_resource_budget.hpp"
#include "mikumikustudio/history_physics.hpp"
#include <memory>

namespace mikumikustudio {
class PhysicsScene;
namespace mdl { struct ModelRecord; struct RigidRecord; struct JointRecord; }
namespace plugins {
class ModelPhysicsResources;
class ModelPhysicsVersion {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ModelPhysicsVersion(std::unique_ptr<Impl>);
    friend class ModelPhysicsResources;
public:
    ~ModelPhysicsVersion();
    ModelPhysicsVersion(const ModelPhysicsVersion&) = delete;
    ModelPhysicsVersion& operator=(const ModelPhysicsVersion&) = delete;
    mdl::RigidRecord* Rigids() const noexcept;
    mdl::JointRecord* Joints() const noexcept;
    unsigned RigidCount() const noexcept;
    unsigned JointCount() const noexcept;
    std::shared_ptr<history_physics::Retained> Membership() const noexcept;
    void MeasureResources(resource_budget::Collector&, bool includeMembership = true) const;
    void MeasureBookkeeping(resource_budget::Collector&, bool includeMembership = true) const;
};

// Internal scene resource ownership. All calls use the UI/scene mutation gate;
// the app and its Bullet world outlive versions and prepared candidates.
class ModelPhysicsResources : public std::enable_shared_from_this<ModelPhysicsResources> {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ModelPhysicsResources(std::unique_ptr<Impl>);
    static bool DisposeModel(unsigned char*) noexcept;
public:
    ~ModelPhysicsResources();
    ModelPhysicsResources(const ModelPhysicsResources&) = delete;
    ModelPhysicsResources& operator=(const ModelPhysicsResources&) = delete;
    static std::shared_ptr<ModelPhysicsResources> Capture(unsigned char* model);
    static std::shared_ptr<ModelPhysicsResources> Find(unsigned char* model) noexcept;
    // Existing private model import: ownership transfers only after its parser
    // releases the model, by the caller's following AdoptOwnership() call.
    static std::shared_ptr<ModelPhysicsResources> CaptureImport(unsigned char* model,
        PhysicsScene& destination, std::shared_ptr<history_physics::ImportScene>,
        std::shared_ptr<history_physics::Retained>);
    void AdoptOwnership() noexcept;
    // The original, still-model-owned baseline may be parked by object history.
    // Release its detached joints immediately before normal ModelDispose; an
    // adopted resource version performs its own complete teardown instead.
    void ReleaseBorrowedParkedJoints() noexcept;
    void PublishedImport() noexcept;
    std::shared_ptr<ModelPhysicsVersion> Current() const noexcept;
    bool IsCurrent(const std::shared_ptr<ModelPhysicsVersion>&) const noexcept;
    // Adopts only the physics fields/native objects of a scratch ModelRecord.
    // On success clears its two table fields/counts; on failure it keeps them.
    std::shared_ptr<ModelPhysicsVersion> AdoptPrivate(mdl::ModelRecord&,
        const std::shared_ptr<history_physics::ImportScene>&);
    // First exchange uses ImportScene::PrepareReplacing; both replay directions
    // use Retained::PrepareExchange. The candidate holds both full versions and
    // reports a complete native target charge; callers add their own record cost.
    std::unique_ptr<PreparedHistoryChange> PrepareExchange(
        const std::shared_ptr<ModelPhysicsVersion>& expected,
        const std::shared_ptr<ModelPhysicsVersion>& target);
    void MeasureResources(resource_budget::Collector&, bool includeMembership = true) const;
    void MeasureBookkeeping(resource_budget::Collector&, bool includeMembership = true) const;
};
}
}
