#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/affine_collision_shape.hpp"
#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include <BulletDynamics/ConstraintSolver/btGeneric6DofConstraint.h>
#include <algorithm>
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mikumikustudio::physics_scale {
namespace {
using Mat = bone_scale::Transform;
Mat Identity() {
    Mat m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}
Mat Product(const Mat &a, const Mat &b) {
    Mat c{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int k = 0; k < 4; ++k)
                v += double(a[i * 4 + k]) * b[k * 4 + j];
            c[i * 4 + j] = float(v);
        }
    return c;
}
Mat Inverse(const Mat &a) {
    bone_scale::Matrix inv;
    if (!bone_scale::Inverse(bone_scale::Linear(a.data()), inv))
        return Identity();
    Mat b = Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            b[i * 4 + j] = inv.v[i * 3 + j];
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
            b[12 + j] -= a[12 + k] * b[k * 4 + j];
    return b;
}
Mat Matrix(const btTransform &t) {
    Mat m = Identity();
    for (int i = 0; i < 3; ++i) {
        m[12 + i] = t.getOrigin()[i];
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = t.getBasis()[j][i];
    }
    return m;
}
btMatrix3x3 Linear(const Mat &m) {
    return btMatrix3x3(m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]);
}
Mat Stretch(const btMatrix3x3 &a) {
    Mat m = Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = a[j][i];
    return m;
}
bool Close(const btMatrix3x3 &a, const btMatrix3x3 &b) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(a[i][j] - b[i][j]) > 2e-5f * (std::max)(1.f, std::abs(a[i][j])))
                return false;
    return true;
}
bool Split(const Mat &m, btMatrix3x3 &stretch, btTransform &pose) {
    const auto a = bone_scale::Linear(m.data());
    float h[9], q[4];
    if (!affine::PolarDecompose(a.v.data(), h, q))
        return false;
    stretch = btMatrix3x3(h[0], h[3], h[6], h[1], h[4], h[7], h[2], h[5], h[8]);
    pose.setOrigin(btVector3(m[12], m[13], m[14]));
    pose.setRotation(btQuaternion(q[0], q[1], q[2], q[3]));
    return true;
}
struct Rigid {
    btRigidBody *body = nullptr;
    btCollisionShape *base = nullptr;
    std::unique_ptr<AffineConvexShape> shape;
    Mat inverse = Identity(), offset = Identity(), follow = Identity();
    btMatrix3x3 stretch = btMatrix3x3::getIdentity();
    btScalar mass = 0;
    btVector3 inertia{0, 0, 0};
    bool bound = false, active = false;
};
struct Joint {
    btGeneric6DofConstraint *joint = nullptr;
    btTransform a, b;
    btVector3 lower, upper;
    btMatrix3x3 appliedA = btMatrix3x3::getIdentity(), appliedB = btMatrix3x3::getIdentity();
    float radius = 0;
};
struct State {
    std::vector<Rigid> rigids;
    std::vector<Joint> joints;
    bone_scale::Values pose;
    bone_scale::Values authoredPose;
    std::vector<Mat> geometryBaseline;
};
std::unordered_map<const unsigned char *, State> states;
std::unordered_map<const btRigidBody *, Rigid *> bodies;
std::unordered_map<const btGeneric6DofConstraint *, Joint *> constraints;
void Refresh(btDiscreteDynamicsWorld &world, btRigidBody &body) {
    if (auto *proxy = body.getBroadphaseHandle())
        world.getBroadphase()->getOverlappingPairCache()->cleanProxyFromPairs(proxy,
                                                                              world.getDispatcher());
    world.updateSingleAabb(&body);
    body.activate(true);
}
void SetPose(btRigidBody &body, const btTransform &pose) {
    body.setWorldTransform(pose);
    body.setInterpolationWorldTransform(pose);
    if (auto *motion = body.getMotionState())
        motion->setWorldTransform(pose);
    body.updateInertiaTensor();
}
void Shape(Rigid &r, const btMatrix3x3 &stretch, btDiscreteDynamicsWorld &world) {
    if (Close(r.stretch, stretch))
        return;
    r.stretch = stretch;
    if (Close(stretch, btMatrix3x3::getIdentity())) {
        r.body->setCollisionShape(r.base);
        r.body->setMassProps(r.mass, r.inertia);
    } else {
        if (!r.shape)
            r.shape = std::make_unique<AffineConvexShape>(*static_cast<const btConvexShape *>(r.base));
        r.shape->SetAffine(stretch);
        r.body->setCollisionShape(r.shape.get());
        btVector3 inertia(0, 0, 0);
        r.shape->calculateLocalInertia(r.mass, inertia);
        r.body->setMassProps(r.mass, inertia);
    }
    r.body->updateInertiaTensor();
    Refresh(world, *r.body);
}
State &Create(unsigned char *model) {
    auto &m = *mdl::Mdl(model);
    auto &s = states[model];
    s.authoredPose=bone_scale::PoseValues(model);
    s.geometryBaseline.resize(m.boneCount);
    for(unsigned bone=0;bone<m.boneCount;++bone)
        std::memcpy(s.geometryBaseline[bone].data(),m.boneTable[bone].matInit,64);
    s.rigids.resize(m.rigidCount);
    s.joints.resize(m.jointCount);
    for (unsigned i = 0; i < m.rigidCount; ++i) {
        auto &r = s.rigids[i];
        const auto &rec = m.rigidTable[i];
        r.body = static_cast<btRigidBody *>(rec.body);
        if (!r.body)
            continue;
        r.base = r.body->getCollisionShape();
        r.bound = rec.boneIndex >= 0 && rec.boneIndex < int(m.boneCount) && r.base && r.base->isConvex();
        std::memcpy(r.inverse.data(), rec.invTransform, 64);
        r.offset = Inverse(r.inverse);
        r.mass = r.body->getInvMass() > 0 ? 1 / r.body->getInvMass() : 0;
        for (int k = 0; k < 3; ++k)
            r.inertia[k] =
                r.body->getInvInertiaDiagLocal()[k] > 0 ? 1 / r.body->getInvInertiaDiagLocal()[k] : 0;
        bodies[r.body] = &r;
    }
    auto *world = m.scenePtr->world;
    for (unsigned j = 0; j < m.jointCount; ++j) {
        auto &entry = s.joints[j];
        const auto &rec = m.jointTable[j];
        entry.radius = rec.radiusBound;
        if (rec.constraint < 0)
            continue;
        for (int k = 0; k < world->getNumConstraints(); ++k) {
            auto *c = world->getConstraint(k);
            if (c->getUid() != rec.constraint)
                continue;
            entry.joint = dynamic_cast<btGeneric6DofConstraint *>(c);
            if (!entry.joint)
                break;
            entry.a = entry.joint->getFrameOffsetA();
            entry.b = entry.joint->getFrameOffsetB();
            auto *motor = entry.joint->getTranslationalLimitMotor();
            entry.lower = motor->m_lowerLimit;
            entry.upper = motor->m_upperLimit;
            constraints[entry.joint] = &entry;
            break;
        }
    }
    return s;
}
btTransform Frame(const btTransform &base, const btMatrix3x3 &deformation) {
    if (Close(deformation, btMatrix3x3::getIdentity()))
        return base;
    Mat full = Product(Matrix(base), Stretch(deformation));
    btMatrix3x3 stretch;
    btTransform result;
    if (!Split(full, stretch, result))
        return base;
    return result;
}
void Joints(unsigned char *model, State &s) {
    auto &m = *mdl::Mdl(model);
    for (unsigned i = 0; i < s.joints.size(); ++i) {
        auto &entry = s.joints[i];
        if (!entry.joint)
            continue;
        auto &rec = m.jointTable[i];
        if (rec.rigidA < 0 || rec.rigidB < 0 || rec.rigidA >= int(s.rigids.size()) ||
            rec.rigidB >= int(s.rigids.size()))
            continue;
        const auto &a = s.rigids[rec.rigidA];
        const auto &b = s.rigids[rec.rigidB];
        if (Close(a.stretch, entry.appliedA) && Close(b.stretch, entry.appliedB))
            continue;
        entry.appliedA = a.stretch;
        entry.appliedB = b.stretch;
        entry.joint->getFrameOffsetA() = Frame(entry.a, a.stretch);
        entry.joint->getFrameOffsetB() = Frame(entry.b, b.stretch);
        btVector3 lo = entry.lower, hi = entry.upper;
        // The constraint uses A's reference frame. Length limits scale along
        // its three axes; angular limits and spring stiffness keep their units.
        for (int axis = 0; axis < 3; ++axis) {
            const btScalar factor = (a.stretch * entry.a.getBasis().getColumn(axis)).length();
            lo[axis] *= factor;
            hi[axis] *= factor;
        }
        entry.joint->setLinearLowerLimit(lo);
        entry.joint->setLinearUpperLimit(hi);
        entry.joint->calculateTransforms();
        btVector3 extent(0, 0, 0);
        for (int axis = 0; axis < 3; ++axis)
            extent[axis] = (std::max)(btFabs(lo[axis]), btFabs(hi[axis]));
        if (Close(a.stretch, btMatrix3x3::getIdentity()) && Close(b.stretch, btMatrix3x3::getIdentity()))
            rec.radiusBound = entry.radius;
        else
            rec.radiusBound = entry.joint->getFrameOffsetA().getOrigin().length() +
                              entry.joint->getFrameOffsetB().getOrigin().length() + extent.length();
    }
}
} // namespace
void Sync(unsigned char *model, bool reseat) {
    if (!model)
        return;
    auto &m = *mdl::Mdl(model);
    if (!m.rigidTable || !m.boneTable || !m.scenePtr || !m.scenePtr->world)
        return;
    auto found = states.find(model);
    if (found == states.end() && !bone_scale::HasPose(model))
        return;
    bone_scale::RestoreTransforms(model);
    auto current = bone_scale::PoseValues(model);
    auto &s = found == states.end() ? Create(model) : found->second;
    const bool changed = current != s.pose;
    // Physics readback writes compensating local stretches. Only differences
    // since the last readback are author edits; never feed those compensations
    // back into collision geometry on the next drag.
    if(changed){
        for(const auto& entry:s.pose)if(current.find(entry.first)==current.end())s.authoredPose.erase(entry.first);
        for(const auto& entry:current){
            const auto previous=s.pose.find(entry.first);
            if(previous==s.pose.end()||previous->second!=entry.second)s.authoredPose[entry.first]=entry.second;
        }
    }
    std::vector<Mat> now, old;
    std::vector<Mat> geometry;
    bone_scale::BuildTransforms(model,s.authoredPose,geometry,&s.geometryBaseline);
    bone_scale::BuildTransforms(model, current, now);
    if (changed)
        bone_scale::BuildTransforms(model, s.pose, old);
    auto &world = *m.scenePtr->world;
    for (unsigned i = 0; i < s.rigids.size(); ++i) {
        auto &r = s.rigids[i];
        const auto &rec = m.rigidTable[i];
        if (!r.bound)
            continue;
        const Mat full = Product(r.offset, now[rec.boneIndex]);
        btMatrix3x3 h;
        btTransform follow;
        if (!Split(full, h, follow))
            continue;
        btMatrix3x3 geometryStretch;btTransform geometryPose;
        if(!Split(Product(r.offset,geometry[rec.boneIndex]),geometryStretch,geometryPose))continue;
        r.follow = Matrix(follow);
        const bool wasActive = r.active;
        r.active = false;
        for (int k = 0; k < 16; ++k)
            if (std::abs(now[rec.boneIndex][k] - m.boneTable[rec.boneIndex].matInit[k]) > 2e-5f)
                r.active = true;
        // Other branches of the same model keep their legacy rigid path.
        if (!r.active && !wasActive && Close(r.stretch, btMatrix3x3::getIdentity()))
            continue;
        if (rec.mode == 0 || reseat) {
            Shape(r, geometryStretch, world);
            if (rec.mode == 0) {
                r.body->setWorldTransform(follow);
                if (auto *motion = r.body->getMotionState())
                    motion->setWorldTransform(follow);
                world.updateSingleAabb(r.body);
            }
            if (reseat) {
                SetPose(*r.body, follow);
                r.body->setLinearVelocity(btVector3(0, 0, 0));
                r.body->setAngularVelocity(btVector3(0, 0, 0));
                Refresh(world, *r.body);
            }
        } else if (changed) {
            if (old[rec.boneIndex] == now[rec.boneIndex])
                continue;
            // Compare two scale poses on the same rigid skeleton. Apply only
            // that delta to the simulated body, preserving its physical motion.
            const Mat delta = Product(Inverse(Product(r.offset, old[rec.boneIndex])), full);
            const btTransform prior = r.body->getWorldTransform();
            const Mat transformed = Product(Product(Stretch(r.stretch), Matrix(prior)), delta);
            btTransform next;
            btMatrix3x3 deformation;
            if (!Split(transformed, deformation, next))
                continue;
            const btVector3 velocity = Linear(delta) * r.body->getLinearVelocity();
            const btVector3 angular =
                next.getBasis() * prior.getBasis().transpose() * r.body->getAngularVelocity();
            Shape(r, geometryStretch, world);
            SetPose(*r.body, next);
            r.body->setLinearVelocity(velocity);
            r.body->setAngularVelocity(angular);
            r.body->setInterpolationLinearVelocity(velocity);
            r.body->setInterpolationAngularVelocity(angular);
            Refresh(world, *r.body);
        }
    }
    Joints(model, s);
    s.pose = std::move(current);
    if (s.pose.empty())
        Forget(model);
}
bool Follow(const unsigned char *model, int rigid, float matrix[16]) {
    const auto s = states.find(model);
    if (s == states.end() || rigid < 0 || rigid >= int(s->second.rigids.size()))
        return false;
    const auto &r = s->second.rigids[rigid];
    if (!r.bound || !r.active)
        return false;
    std::memcpy(matrix, r.follow.data(), 64);
    return true;
}
void Readback(unsigned char *model) {
    const auto it = states.find(model);
    if (it == states.end())
        return;
    auto &m = *mdl::Mdl(model);
    std::map<int, Mat> poses;
    for (unsigned i = 0; i < it->second.rigids.size(); ++i) {
        const auto &r = it->second.rigids[i];
        const auto &rec = m.rigidTable[i];
        if (!r.bound || !r.active || rec.mode == 0 || m.boneTable[rec.boneIndex].physicsDisabled)
            continue;
        poses[rec.boneIndex] =
            Product(r.inverse, Product(Stretch(r.stretch), Matrix(r.body->getWorldTransform())));
    }
    bone_scale::ResolvePhysics(model, poses);
}
void FinishReadback(unsigned char *model) {
    const auto it = states.find(model);
    if (it != states.end())
        it->second.pose = bone_scale::PoseValues(model);
}
void Forget(unsigned char *model) {
    const auto it = states.find(model);
    if (it == states.end())
        return;
    auto &m = *mdl::Mdl(model);
    auto &s = it->second;
    for (auto &r : s.rigids)
        if (r.body) {
            if (r.shape) {
                r.body->setCollisionShape(r.base);
                r.body->setMassProps(r.mass, r.inertia);
                r.body->updateInertiaTensor();
                if (m.scenePtr && m.scenePtr->world)
                    Refresh(*m.scenePtr->world, *r.body);
            }
            bodies.erase(r.body);
        }
    for (unsigned i = 0; i < s.joints.size(); ++i) {
        auto &j = s.joints[i];
        if (j.joint) {
            j.joint->getFrameOffsetA() = j.a;
            j.joint->getFrameOffsetB() = j.b;
            j.joint->setLinearLowerLimit(j.lower);
            j.joint->setLinearUpperLimit(j.upper);
            j.joint->calculateTransforms();
            constraints.erase(j.joint);
        }
        if (i < m.jointCount)
            m.jointTable[i].radiusBound = j.radius;
    }
    states.erase(it);
}
const btCollisionShape *BaseShape(const btRigidBody *body) {
    const auto it = bodies.find(body);
    return it == bodies.end() ? body->getCollisionShape() : it->second->base;
}
bool CollisionTransform(const btRigidBody *body, float matrix[16]) {
    const auto it = bodies.find(body);
    if (it == bodies.end() || !it->second->shape || body->getCollisionShape() == it->second->base)
        return false;
    const auto m = Product(Stretch(it->second->stretch), Matrix(body->getWorldTransform()));
    std::memcpy(matrix, m.data(), 64);
    return true;
}
void JointBaseline(const btGeneric6DofConstraint &joint, btTransform &a, btTransform &b) {
    const auto it = constraints.find(&joint);
    a = it == constraints.end() ? joint.getFrameOffsetA() : it->second->a;
    b = it == constraints.end() ? joint.getFrameOffsetB() : it->second->b;
}
float JointRadiusBaseline(const unsigned char *model, int joint) {
    const auto it = states.find(model);
    return it == states.end() ? mdl::Mdl(model)->jointTable[joint].radiusBound
                              : it->second.joints[joint].radius;
}
} // namespace mikumikustudio::physics_scale
