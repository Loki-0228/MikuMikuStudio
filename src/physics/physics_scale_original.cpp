#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/model.hpp"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofConstraint.h>

static_assert(MMD_ORIGINAL_PROFILE, "Original profile provider must not enter the full build");
namespace mikumikustudio::physics_scale {
void Sync(unsigned char*, bool) {}
bool Follow(const unsigned char*, int, float[16]) { return false; }
void Readback(unsigned char*) {}
void FinishReadback(unsigned char*) {}
void Forget(unsigned char*) {}
const btCollisionShape* BaseShape(const btRigidBody* body) { return body ? body->getCollisionShape() : nullptr; }
bool CollisionTransform(const btRigidBody*, float[16]) { return false; }
void JointBaseline(const btGeneric6DofConstraint& joint, btTransform& a, btTransform& b) {
    a = joint.getFrameOffsetA();
    b = joint.getFrameOffsetB();
}
float JointRadiusBaseline(const unsigned char* model, int joint) {
    const auto* record = model ? mdl::Mdl(model) : nullptr;
    return record && record->jointTable && joint >= 0 && static_cast<unsigned>(joint) < record->jointCount
        ? record->jointTable[joint].radiusBound : 0;
}
}
