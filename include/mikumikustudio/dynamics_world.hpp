#pragma once

#include "BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "BulletSoftBody/btSoftRigidDynamicsWorld.h"
#endif
#include <cstdint>

namespace mikumikustudio {

// The diagnostics use the same accumulator as Bullet's stepSimulation.
// Access the protected member through its owning class, independent of ABI.
#if MMD_ORIGINAL_PROFILE
using DynamicsWorldBase = btDiscreteDynamicsWorld;
#else
using DynamicsWorldBase = btSoftRigidDynamicsWorld;
#endif
class DynamicsWorld final : public DynamicsWorldBase {
public:
    using DynamicsWorldBase::DynamicsWorldBase;
    btScalar LocalTime() const { return m_localTime; }
    void ResetLocalTime() { m_localTime = btScalar(0); }
#if !MMD_ORIGINAL_PROFILE
    using StepHook = void (*)(btDiscreteDynamicsWorld&);
    inline static StepHook beforeStep = nullptr;
    using SoftBodySolver = bool (*)(btSoftBody&);
    inline static SoftBodySolver solveSoftBody = nullptr;
    using ClusterSolver = bool (*)(btAlignedObjectArray<btSoftBody*>&);
    inline static ClusterSolver solveClusters = nullptr;
    // History reserves the existing arrays during prepare, then replaces
    // pointer entries without destroying/recreating Bullet bodies or proxies.
    btAlignedObjectArray<btRigidBody*>& HistoryBodies() noexcept { return m_nonStaticRigidBodies; }
    btAlignedObjectArray<btTypedConstraint*>& HistoryConstraints() noexcept { return m_constraints; }
    bool HasActions() const noexcept { return m_actions.size() != 0; }
    std::uint64_t StepSerial() const noexcept { return stepSerial_; }
    int stepSimulation(btScalar dt, int steps = 1,
                       btScalar fixed = btScalar(1.) / btScalar(60.)) override {
        ++stepSerial_;
        getWorldInfo().m_gravity = getGravity();
        return DynamicsWorldBase::stepSimulation(dt, steps, fixed);
    }
    void performDiscreteCollisionDetection() override {
        ++stepSerial_;
        btDiscreteDynamicsWorld::performDiscreteCollisionDetection();
    }
private:
    void internalSingleStepSimulation(btScalar dt) override {
        if (beforeStep) beforeStep(*this);
        if (!solveSoftBody) {
            DynamicsWorldBase::internalSingleStepSimulation(dt);
            return;
        }
        // Keep Bullet's prediction, collision and cluster stages in the same
        // order. Only an accepted body's constraint iterations move to compute.
        btDiscreteDynamicsWorld::internalSingleStepSimulation(dt);
        auto& bodies=getSoftBodyArray();
        if(bodies.size() && (!solveClusters || !solveClusters(bodies)))btSoftBody::solveClusters(bodies);
        for(int i=0;i<bodies.size();++i)
            if(!solveSoftBody(*bodies[i]))bodies[i]->solveConstraints();
        for(int i=0;i<bodies.size();++i)bodies[i]->defaultCollisionHandler(bodies[i]);
        updateSoftBodies();
    }
    std::uint64_t stepSerial_ = 0;
#endif
};

}  // namespace mikumikustudio
