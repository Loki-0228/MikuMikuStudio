#pragma once

#include "BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h"

namespace mikumikustudio {

// The diagnostics use the same accumulator as Bullet's stepSimulation.
// Access the protected member through its owning class, independent of ABI.
class DynamicsWorld final : public btDiscreteDynamicsWorld {
public:
    using btDiscreteDynamicsWorld::btDiscreteDynamicsWorld;
    btScalar LocalTime() const { return m_localTime; }
    void ResetLocalTime() { m_localTime = btScalar(0); }
};

}  // namespace mikumikustudio
