#pragma once
class btRigidBody;
class btCollisionShape;
class btGeneric6DofConstraint;
class btTransform;
namespace mikumikustudio::physics_scale {
// Update physical shapes and constraint frames from native bone scaling.
void Sync(unsigned char *model, bool reseat = false);
bool Follow(const unsigned char *model, int rigid, float matrix[16]);
void Readback(unsigned char *model);
void FinishReadback(unsigned char *model);
void Forget(unsigned char *model);
const btCollisionShape *BaseShape(const btRigidBody *body);
bool CollisionTransform(const btRigidBody *body, float matrix[16]);
void JointBaseline(const btGeneric6DofConstraint &joint, btTransform &a, btTransform &b);
float JointRadiusBaseline(const unsigned char *model, int joint);
} // namespace mikumikustudio::physics_scale
