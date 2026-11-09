#pragma once

#include "BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h"

namespace mikumikustudio {

// ABI import publishes native constraints from a temporary world. Keep the
// tuning value on the constraint so publication never retains that world.
class PlaybackSpringConstraint final : public btGeneric6DofSpringConstraint {
    btScalar cfm_ = 0;

public:
    using btGeneric6DofSpringConstraint::btGeneric6DofSpringConstraint;

    void SetCfm(btScalar cfm) noexcept { cfm_ = cfm; }

    void getInfo2(btConstraintInfo2* info) override {
        for (int axis = 0; axis < 3; ++axis)
            getRotationalLimitMotor(axis)->m_ERP = info->erp;
        getTranslationalLimitMotor()->m_restitution = info->erp;
        btGeneric6DofSpringConstraint::getInfo2(info);
        btConstraintInfo1 rows{};
        getInfo1(&rows);
        for (int row = 0; row < rows.m_numConstraintRows; ++row)
            info->cfm[row * info->rowskip] = cfm_;
    }
};

}  // namespace mikumikustudio
