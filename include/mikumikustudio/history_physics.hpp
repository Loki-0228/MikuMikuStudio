#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletCollision/BroadphaseCollision/btAxisSweep3.h"
#include <memory>
#include <vector>

class btRigidBody;
class btTypedConstraint;
namespace mikumikustudio {
namespace plugins::resource_budget { class Collector; }
class DynamicsWorld;
class PhysicsScene;
namespace history_physics {
class ImportScene;
struct ImportBroadphaseState;
// These classes are internal to MMS. They do not change Bullet or plugin ABI.
class PairCache final : public btHashedOverlappingPairCache {
public:
    bool HasGhostCallback() const noexcept { return m_ghostPairCallback != nullptr; }
};
class Broadphase final : public bt32BitAxisSweep3 {
    friend struct ImportBroadphaseState;
    Broadphase(const btVector3&, const btVector3&, unsigned, std::unique_ptr<PairCache>, bool disableRaycast = false);
public:
    Broadphase(const btVector3&, const btVector3&, unsigned maxHandles);
    ~Broadphase() override;
    btBroadphaseProxy* createProxy(const btVector3&, const btVector3&, int shapeType, void* object,
        short group, short mask, btDispatcher*, void* multiSapProxy) override;
    PairCache* Cache() const noexcept;
    PairCache* Exchange(PairCache*) noexcept;
    static btBroadphaseProxy* RayProxy(btBroadphaseProxy*) noexcept;
    static void SetMask(btBroadphaseProxy*, short) noexcept;
};

// The caller owns the bodies/constraints and keeps the world alive. Parked
// bodies retain broadphase handles, but leave simulation and ray filtering.
// Parked bodies must remain untouched. Constraints must connect retained bodies.
// A transition keeps this object's internal state alive, not the world or bodies.
// Destroy every published transition before stepping or destroying the world.
class Retained {
    friend class ImportScene;
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    class Transition {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class Retained;
        explicit Transition(std::unique_ptr<Impl>);
    public:
        ~Transition();
        bool IsCurrent() const noexcept;
        // Measure only these owners' history bookkeeping after publication.
        // Includes the prepared Saved state and shared world-order snapshot;
        // excludes temporary candidates/caches and the native Bullet resources
        // (measured separately). Does not publish or change the world. A stale,
        // consumed or unavailable target returns false without changing out.
        // Collector allocation/overflow may throw; the candidate remains usable.
        // This is not a future-transition bound or an admission decision.
        bool MeasureTargetResources(plugins::resource_budget::Collector& out) const;
        // No allocation, resource destruction or callbacks. Old contacts are
        // disposed by this transition's destructor, before the next step.
        bool Publish() noexcept;
    };
    Retained(DynamicsWorld&, std::vector<btRigidBody*>, std::vector<btTypedConstraint*>);
    ~Retained();
    Retained(const Retained&) = delete;
    Retained& operator=(const Retained&) = delete;
    bool Present() const noexcept;
    // The wrapper and Impl share one accounting identity. This conservatively
    // includes the wrapper's size if only a transition still retains the Impl.
    void MeasureResources(plugins::resource_budget::Collector&) const;
    std::unique_ptr<Transition> Prepare(bool present);
    // One world publication for all objects, never a chain of single-object
    // publications. Objects must be distinct, share a world and all change to
    // the requested presence. Body/constraint ownership must not overlap.
    // The input order does not affect restored simulation order. Conflicting
    // historical/current order, including an external reorder, is rejected.
    // Preparation/cancellation never replace or reserve live world arrays.
    // Publication exchanges complete private arrays without allocation; the
    // consumed transition releases the displaced storage on destruction.
    static std::unique_ptr<Transition> PrepareBatch(const std::vector<Retained*>& objects, bool present);
    struct PresenceChange { Retained* object; bool present; };
    // One atomic mixed presence exchange between already-retained bundles.
    // Each owner must switch from present to parked or parked to present;
    // duplicate owners, overlapping resources and different worlds reject.
    // Incoming historical order is projected after removing outgoing members.
    // Live peers keep their relative order; conflicting anchors reject before
    // publication. This does not publish a private ImportScene for the first
    // time and does not replace model tables or their resource ownership.
    static std::unique_ptr<Transition> PrepareExchange(const std::vector<PresenceChange>&);
};

// Loading uses an isolated world, with no ground, renderer resources or steps.
// Scene().broadphase is null: the private world uses its own growable DBVT.
// The loader and ModelDispose must use Scene() until publication succeeds.
// Native bodies/joints remain caller-owned; dispose them before releasing this
// scene on failure. The live scene must outlive this object and its candidates.
class ImportScene {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    class Publication {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class ImportScene;
        explicit Publication(std::unique_ptr<Impl>);
    public:
        ~Publication();
        bool IsCurrent() const noexcept;
        // Pending owner can be measured before publication, but cannot prepare
        // ordinary presence changes. Success makes it a normal present owner.
        std::shared_ptr<Retained> RetainedOwner() const noexcept;
        // Published incoming owners have no Saved/order allocation. Replacement
        // additionally measures each outgoing owner's prepared Saved and shared
        // order allocation. Uses the same identities as the published owners'
        // MeasureResources, without including temporary caches/world arrays.
        // Native bodies/joints are measured by the enclosing resource version.
        bool MeasureTargetResources(plugins::resource_budget::Collector&) const;
        // No allocation, resource destruction or callbacks. Rebind the loaded
        // model's scenePtr to the live scene in the same enclosing commit.
        // Destroy published candidates before stepping or destroying the world.
        bool Publish() noexcept;
    };
    explicit ImportScene(PhysicsScene& live);
    ~ImportScene();
    ImportScene(const ImportScene&) = delete;
    ImportScene& operator=(const ImportScene&) = delete;
    PhysicsScene& Scene() noexcept;
    // A candidate freezes both worlds. Cancellation changes neither world.
    // Repeated preparation is allowed until one candidate publishes.
    std::unique_ptr<Publication> Prepare();
    // First publication of this private scene and parking of existing bundles
    // share one world/cache transition. Outgoing owners must be distinct,
    // present, nonpending and belong to this scene's live world. Their native
    // bodies/joints remain caller-owned and must outlive owners/candidates.
    // Surviving live members keep their order; private members follow them in
    // private order. An empty list is ordinary Prepare(). No live array storage
    // or capacity is changed by preparation/cancellation. After success use
    // Retained::PrepareExchange for both directions, without reimporting.
    std::unique_ptr<Publication> PrepareReplacing(const std::vector<Retained*>& outgoing);
};
}
}
#endif
