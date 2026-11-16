/*
Bullet Continuous Collision Detection and Physics Library
Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the use of this software.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/
// Adapted solver completion for the project CFM normalization below.
#pragma once

#include <BulletDynamics/ConstraintSolver/btSequentialImpulseConstraintSolver.h>
#include <BulletDynamics/ConstraintSolver/btContactSolverInfo.h>
#include <cmath>

namespace mikumikustudio {

// Bullet 2.75's row loop expects normalized CFM, but its setup does not add
// compliance to the diagonal. Normalize both terms together before iterating.
// A zero-CFM world follows the original solver without any numerical change.
class TunedConstraintSolver final : public btSequentialImpulseConstraintSolver {
public:
    btScalar solveGroup(btCollisionObject** bodies, int bodyCount,
        btPersistentManifold** manifolds, int manifoldCount, btTypedConstraint** constraints,
        int constraintCount, const btContactSolverInfo& info, btIDebugDraw* debug,
        btStackAlloc* stack, btDispatcher* dispatcher) override {
        if (info.m_globalCfm == 0)
            return btSequentialImpulseConstraintSolver::solveGroup(bodies, bodyCount,
                manifolds, manifoldCount, constraints, constraintCount, info, debug, stack, dispatcher);
        solveGroupCacheFriendlySetup(bodies, bodyCount, manifolds, manifoldCount,
            constraints, constraintCount, info, debug, stack);
        for (int i = 0; i < m_tmpSolverNonContactConstraintPool.size(); ++i) {
            auto& row = m_tmpSolverNonContactConstraintPool[i];
            if (row.m_cfm <= 0 || row.m_jacDiagABInv <= 0) continue;
            const double compliance = double(row.m_cfm) / info.m_timeStep;
            const double diagonal = 1.0 / row.m_jacDiagABInv;
            const double inverse = 1.0 / (diagonal + compliance);
            row.m_rhs = static_cast<btScalar>(double(row.m_rhs) * diagonal * inverse);
            row.m_jacDiagABInv = static_cast<btScalar>(inverse);
            row.m_cfm = static_cast<btScalar>(compliance * inverse);
        }
        solveGroupCacheFriendlyIterations(bodies, bodyCount, manifolds, manifoldCount,
            constraints, constraintCount, info, debug, stack);
        for (int i = 0; i < m_tmpSolverContactConstraintPool.size(); ++i) {
            const auto& row = m_tmpSolverContactConstraintPool[i];
            auto* point = static_cast<btManifoldPoint*>(row.m_originalContactPoint);
            point->m_appliedImpulse = row.m_appliedImpulse;
            if (info.m_solverMode & SOLVER_USE_FRICTION_WARMSTARTING) {
                point->m_appliedImpulseLateral1 = m_tmpSolverContactFrictionConstraintPool[row.m_frictionIndex].m_appliedImpulse;
                point->m_appliedImpulseLateral2 = m_tmpSolverContactFrictionConstraintPool[row.m_frictionIndex + 1].m_appliedImpulse;
            }
        }
        for (int i = 0; i < m_tmpSolverBodyPool.size(); ++i) {
            if (info.m_splitImpulse) m_tmpSolverBodyPool[i].writebackVelocity(info.m_timeStep);
            else m_tmpSolverBodyPool[i].writebackVelocity();
        }
        m_tmpSolverBodyPool.resize(0);
        m_tmpSolverContactConstraintPool.resize(0);
        m_tmpSolverNonContactConstraintPool.resize(0);
        m_tmpSolverContactFrictionConstraintPool.resize(0);
        return 0;
    }
};
}
