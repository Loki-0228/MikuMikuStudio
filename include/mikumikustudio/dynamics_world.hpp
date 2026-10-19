#pragma once

#include "BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h"
#include "mikumikustudio/build_profile.hpp"
#include <cstdint>

namespace mikumikustudio {

// The diagnostics use the same accumulator as Bullet's stepSimulation.
// Access the protected member through its owning class, independent of ABI.
class DynamicsWorld final : public btDiscreteDynamicsWorld {
public:
    using btDiscreteDynamicsWorld::btDiscreteDynamicsWorld;
    btScalar LocalTime() const { return m_localTime; }
    void ResetLocalTime() { m_localTime = btScalar(0); }
#if !MMD_ORIGINAL_PROFILE
    // History reserves the existing arrays during prepare, then replaces
    // pointer entries without destroying/recreating Bullet bodies or proxies.
    btAlignedObjectArray<btRigidBody*>& HistoryBodies() noexcept { return m_nonStaticRigidBodies; }
    btAlignedObjectArray<btTypedConstraint*>& HistoryConstraints() noexcept { return m_constraints; }
    bool HasActions() const noexcept { return m_actions.size() != 0; }
    std::uint64_t StepSerial() const noexcept { return stepSerial_; }
    int stepSimulation(btScalar dt, int steps = 1,
                       btScalar fixed = btScalar(1.) / btScalar(60.)) override {
        ++stepSerial_;
        return btDiscreteDynamicsWorld::stepSimulation(dt, steps, fixed);
    }
    void performDiscreteCollisionDetection() override {
        ++stepSerial_;
        btDiscreteDynamicsWorld::performDiscreteCollisionDetection();
    }
private:
    std::uint64_t stepSerial_ = 0;
#endif
};

}  // namespace mikumikustudio
