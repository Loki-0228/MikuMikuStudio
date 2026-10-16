#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletCollision/BroadphaseCollision/btAxisSweep3.h"
#include <memory>
#include <vector>

class btRigidBody;
class btTypedConstraint;
namespace mikudancestudio {
namespace plugins::resource_budget { class Collector; }
class DynamicsWorld;
namespace history_physics {
// These classes are internal to MMS. They do not change Bullet or plugin ABI.
class PairCache final : public btHashedOverlappingPairCache {
public:
    bool HasGhostCallback() const noexcept { return m_ghostPairCallback != nullptr; }
};
class Broadphase final : public bt32BitAxisSweep3 {
    Broadphase(const btVector3&, const btVector3&, unsigned, std::unique_ptr<PairCache>);
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
        // No allocation, resource destruction or callbacks. Old contacts are
        // disposed by this transition's destructor, before the next step.
        bool Publish() noexcept;
    };
    Retained(DynamicsWorld&, std::vector<btRigidBody*>, std::vector<btTypedConstraint*>);
    ~Retained();
    Retained(const Retained&) = delete;
    Retained& operator=(const Retained&) = delete;
    bool Present() const noexcept;
    void MeasureResources(plugins::resource_budget::Collector&) const;
    std::unique_ptr<Transition> Prepare(bool present);
    // One world publication for all objects, never a chain of single-object
    // publications. Objects must be distinct, share a world and all change to
    // the requested presence. Body/constraint ownership must not overlap.
    // The input order does not affect restored simulation order. Conflicting
    // historical/current order, including an external reorder, is rejected.
    static std::unique_ptr<Transition> PrepareBatch(const std::vector<Retained*>& objects, bool present);
};
}
}
#endif
