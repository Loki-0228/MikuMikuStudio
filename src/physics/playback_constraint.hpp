#pragma once
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>

namespace mikumikustudio {
// The live world and offline bake both apply ERP/CFM to every joint row.
class PlaybackSpringConstraint final : public btGeneric6DofSpringConstraint {
    btScalar erp_ = .2f;
    btScalar cfm_ = 0;
public:
    using btGeneric6DofSpringConstraint::btGeneric6DofSpringConstraint;
    void Configure(btScalar erp, btScalar cfm) { erp_ = erp; cfm_ = cfm; }

    void getInfo2(btConstraintInfo2* info) override {
        info->erp = erp_;
        for (int axis = 0; axis < 3; ++axis)
            getRotationalLimitMotor(axis)->m_ERP = erp_;
        getTranslationalLimitMotor()->m_restitution = erp_;
        btGeneric6DofSpringConstraint::getInfo2(info);
        btConstraintInfo1 rows{};
        getInfo1(&rows);
        for (int row = 0; row < rows.m_numConstraintRows; ++row)
            info->cfm[row * info->rowskip] = cfm_;
    }
};
}
