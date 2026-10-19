#include "mikumikustudio/app_layout.hpp"
// MAINTENANCE: regression-keep; scaled shapes, joint frames and physics readback.
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <array>
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
using namespace mikumikustudio;
namespace {
int failures = 0;
void Check(bool pass, const char *text) {
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", text);
    }
}
bool Near(float a, float b) { return std::abs(a - b) < .002f * (std::max)(1.f, std::abs(b)); }
void Identity(float *m) {
    std::fill(m, m + 16, 0.f);
    m[0] = m[5] = m[10] = m[15] = 1;
}
bone_scale::Matrix Uniform(float s) { return {{s, 0, 0, 0, s, 0, 0, 0, s}}; }
struct Fixture {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
    PhysicsScene scene{};
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 4> bones{};
    std::array<mdl::RigidRecord, 4> rigids{};
    mdl::JointRecord joint{};
    std::array<std::unique_ptr<btConvexShape>, 4> shapes;
    std::array<std::unique_ptr<btDefaultMotionState>, 4> motions;
    std::array<std::unique_ptr<btRigidBody>, 4> bodies;
    std::unique_ptr<btGeneric6DofSpringConstraint> constraint;
    unsigned char *raw() { return reinterpret_cast<unsigned char *>(model.get()); }
    Fixture() {
        scene.world = &world;
        world.setGravity(btVector3(0, 0, 0));
        model->scenePtr = &scene;
        model->boneCount = 4;
        model->boneTable = bones.data();
        model->rigidCount = 4;
        model->rigidTable = rigids.data();
        model->jointCount = 1;
        model->jointTable = &joint;
        for (int i = 0; i < 4; ++i) {
            auto &b = bones[i];
            b.parent = i == 1 || i == 2 ? 0 : -1;
            b.position[1] = float(i * 3);
            b.rotQuat[3] = b.ikBackup[6] = b.physicsQuat[3] = 1;
            b.hasRigidBody = i == 1 || i == 2;
            b.tailIdx = -1;
            Identity(b.matInit);
            Identity(b.matLocal);
            Identity(b.matExtra);
            auto &r = rigids[i];
            r.boneIndex = i == 3 ? -1 : i;
            r.mode = i == 3 ? 1 : i;
            r.staticFlag = i == 0;
            r.kinematicFlag = i == 2;
            r.position[0] = 1;
            Identity(r.invTransform);
            r.invTransform[12] = -1;
            r.invTransform[13] = -b.position[1];
            if (i == 0)
                shapes[i] = std::make_unique<btSphereShape>(1);
            else if (i == 1)
                shapes[i] = std::make_unique<btBoxShape>(btVector3(1, 2, 1));
            else
                shapes[i] = std::make_unique<btCapsuleShape>(.5f, 2);
            btTransform t = btTransform::getIdentity();
            t.setOrigin(btVector3(1, b.position[1], 0));
            motions[i] = std::make_unique<btDefaultMotionState>(t);
            btVector3 inertia(0, 0, 0);
            shapes[i]->calculateLocalInertia(i ? 2 : 0, inertia);
            bodies[i] = std::make_unique<btRigidBody>(btRigidBody::btRigidBodyConstructionInfo(
                i ? 2 : 0, motions[i].get(), shapes[i].get(), inertia));
            if (i == 0)
                bodies[i]->setCollisionFlags(bodies[i]->getCollisionFlags() |
                                             btCollisionObject::CF_KINEMATIC_OBJECT);
            bodies[i]->setActivationState(DISABLE_DEACTIVATION);
            world.addRigidBody(bodies[i].get());
            r.body = bodies[i].get();
        }
        btTransform a = btTransform::getIdentity(), b = a;
        a.setOrigin(btVector3(0, 1.5f, 0));
        b.setOrigin(btVector3(0, -1.5f, 0));
        constraint = std::make_unique<btGeneric6DofSpringConstraint>(*bodies[0], *bodies[1], a, b, true);
        constraint->setUserConstraintId(41);
        constraint->setLinearLowerLimit(btVector3(-1, -2, -3));
        constraint->setLinearUpperLimit(btVector3(1, 2, 3));
        constraint->setAngularLowerLimit(btVector3(-.1f, -.2f, -.3f));
        constraint->setAngularUpperLimit(btVector3(.1f, .2f, .3f));
        joint.constraint = 41;
        joint.rigidA = 0;
        joint.rigidB = 1;
        joint.radiusBound = 100;
        world.addConstraint(constraint.get());
    }
    ~Fixture() {
        physics_scale::Forget(raw());
        bone_scale::Forget(raw());
        world.removeConstraint(constraint.get());
        for (auto &b : bodies)
            world.removeRigidBody(b.get());
    }
};
void Basic() {
    Fixture f;
    const auto original = f.rigids;
    const auto baseline = f.constraint->getFrameOffsetA();
    physics_scale::Sync(f.raw());
    for (int i = 0; i < 4; ++i)
        Check(f.bodies[i]->getCollisionShape() == f.shapes[i].get(),
              "unit scale retains original shapes");
    bone_scale::SetPose(f.raw(), 0, Uniform(2));
    ModelKinematicSync(f.raw());
    for (int i = 0; i < 3; ++i) {
        Check(f.bodies[i]->getCollisionShape() != f.shapes[i].get(),
              "all three physics modes receive scaled shape");
        Check(Near(f.bodies[i]->getWorldTransform().getOrigin().x(), 2), "bone-relative offset scales");
        Check(Near(f.bodies[i]->getWorldTransform().getOrigin().y(), i * 6.f),
              "inherited body spacing scales");
        const auto *convex = static_cast<const btConvexShape *>(f.bodies[i]->getCollisionShape());
        Check(Near(convex->localGetSupportingVertex(btVector3(1, 0, 0)).x(), i == 2 ? 1 : 2),
              "shape dimensions double");
        Check(std::memcmp(original[i].size, f.rigids[i].size, sizeof original[i].size) == 0,
              "authored size unchanged");
    }
    Check(f.bodies[3]->getCollisionShape() == f.shapes[3].get() &&
              Near(f.bodies[3]->getWorldTransform().getOrigin().x(), 1),
          "unbound body unchanged");
    Check(Near(f.constraint->getFrameOffsetA().getOrigin().y(), 3) &&
              Near(f.constraint->getFrameOffsetB().getOrigin().y(), -3),
          "both joint anchors scale");
    Check(Near(f.constraint->getTranslationalLimitMotor()->m_upperLimit.z(), 6),
          "joint linear limit scales");
    Check(Near(f.constraint->getRotationalLimitMotor(1)->m_hiLimit, .2f),
          "angular limits retain angles");
    const auto pose = f.bodies[1]->getWorldTransform();
    for (int i = 0; i < 100; ++i) {
        bone_scale::ApplyTransforms(f.raw());
        physics_scale::Sync(f.raw());
    }
    Check(f.bodies[1]->getWorldTransform() == pose, "unchanged scale never accumulates on dynamic pose");
    bone_scale::SetPose(f.raw(), 0, Uniform(3));
    physics_scale::Sync(f.raw());
    Check(Near(f.bodies[1]->getWorldTransform().getOrigin().y(), 9),
          "second edit uses original relative scale");
    bone_scale::SetPose(f.raw(), 0, {});
    physics_scale::Sync(f.raw());
    for (int i = 0; i < 4; ++i)
        Check(f.bodies[i]->getCollisionShape() == f.shapes[i].get(),
              "reset restores original shape ownership");
    Check(f.constraint->getFrameOffsetA() == baseline && f.joint.radiusBound == 100,
          "reset restores exact joint baseline");
    Check(Near(f.bodies[1]->getWorldTransform().getOrigin().y(), 3), "reset restores body spacing");
}
void Readback() {
    Fixture f;
    f.world.removeConstraint(f.constraint.get());
    f.model->jointCount = 0;
    bone_scale::SetPose(f.raw(), 0, {{2, .2f, 0, .2f, 1.1f, 0, 0, 0, .7f}});
    ModelDynamicReseat(f.raw());
    ModelKinematicSync(f.raw());
    auto pose = f.bodies[1]->getWorldTransform();
    pose.setRotation(btQuaternion(btVector3(0, 0, 1), .6f));
    pose.setOrigin(btVector3(4, 7, 1));
    f.bodies[1]->setWorldTransform(pose);
    float full[16];
    Check(physics_scale::CollisionTransform(f.bodies[1].get(), full),
          "deformed debug transform available");
    float expected[16]{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                expected[i * 4 + j] += f.rigids[1].invTransform[i * 4 + k] * full[k * 4 + j];
    float aligned[16];
    Check(physics_scale::CollisionTransform(f.bodies[2].get(), aligned), "mode 2 collision transform available");
    ModelPhysicsReadback(f.raw());
    unsigned char *slots[kModelSlotCount]{};
    slots[0] = f.raw();
    SetPhysicsMode(f.raw(), 1, slots, 2);
    bone_scale::ApplyTransforms(f.raw());
    for (int k = 0; k < 16; ++k)
        Check(Near(f.bones[1].matInit[k], expected[k]),
              "physics readback produces full affine pose exactly once");
    for (int k = 0; k < 16; ++k)
        Check(std::isfinite(f.bones[2].matInit[k]), "mode 2 pose remains finite");
    for (int row=0;row<3;++row)for(int col=0;col<3;++col)
        Check(Near(f.bones[2].matInit[row*4+col],aligned[row*4+col]), "mode 2 retains scaled orientation");
    Check(bone_scale::Valid(bone_scale::Pose(f.raw(), 1)), "resolved local stretch is serializable");
    f.model->jointCount = 1;
    f.world.addConstraint(f.constraint.get());
}
void Contact() {
    Fixture f;
    bone_scale::SetPose(f.raw(), 0, Uniform(2));
    ModelKinematicSync(f.raw());
    btSphereShape probeShape(.25f);
    btTransform pose = btTransform::getIdentity();
    pose.setOrigin(btVector3(4.1f, 0, 0));
    btDefaultMotionState motion(pose);
    btRigidBody probe(
        btRigidBody::btRigidBodyConstructionInfo(1, &motion, &probeShape, btVector3(1, 1, 1)));
    f.world.addRigidBody(&probe);
    f.world.performDiscreteCollisionDetection();
    bool contact = false;
    for (int i = 0; i < f.dispatcher.getNumManifolds(); ++i) {
        auto *m = f.dispatcher.getManifoldByIndexInternal(i);
        if ((m->getBody0() == &probe && m->getBody1() == f.bodies[0].get()) ||
            (m->getBody1() == &probe && m->getBody0() == f.bodies[0].get()))
            for (int j = 0; j < m->getNumContacts(); ++j)
                if (m->getContactPoint(j).getDistance() < 0)
                    contact = true;
    }
    Check(contact, "actual GJK collision reaches enlarged sphere boundary");
    f.world.removeRigidBody(&probe);
}
void RepeatedParentScale(){
    Fixture f;f.world.removeConstraint(f.constraint.get());f.model->jointCount=0;
    unsigned char* slots[kModelSlotCount]{};slots[0]=f.raw();
    for(int n=0;n<120;++n){
        const float y=n%2?1.f:1.2f;
        bone_scale::SetPose(f.raw(),0,{{1,0,0,0,y,0,0,0,1}});
        ModelKinematicSync(f.raw());
        auto pose=f.bodies[1]->getWorldTransform();
        pose.setRotation(btQuaternion(btVector3(0,0,1),n*.13f));
        f.bodies[1]->setWorldTransform(pose);
        ModelPhysicsReadback(f.raw());
        SetPhysicsMode(f.raw(),1,slots,2);
        bone_scale::ApplyTransforms(f.raw());
    }
    auto* shape=static_cast<btConvexShape*>(f.bodies[1]->getCollisionShape());
    Check(Near(shape->localGetSupportingVertex(btVector3(0,1,0)).y(),2),"repeated parent Y scaling returns to original rigid height");
    Check(Near(shape->localGetSupportingVertex(btVector3(1,0,0)).x(),1),"repeated parent Y scaling preserves rigid width");
    f.model->jointCount=1;f.world.addConstraint(f.constraint.get());
}
} // namespace
int main() {
    Basic();
    Readback();
    Contact();
    RepeatedParentScale();
    std::printf("physics scaling: %d failures\n", failures);
    return failures ? 1 : 0;
}
