// CPU-only contracts for the mmd932 providers. No window/device/file is created.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scale.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

static_assert(MMD_ORIGINAL_PROFILE, "This test must link the mmd932 core");
using namespace mikumikustudio;
namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
}
bool Near(double value, double expected, double tolerance = 0.001) {
    return std::isfinite(value) && std::abs(value - expected) <= tolerance;
}
void Identity(float* matrix) {
    std::fill(matrix, matrix + 16, 0.f);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
}
void SetCurve(mdl::BoneKey& key, int channel, const std::array<unsigned char, 4>& controls) {
    for (int i = 0; i < 4; ++i) key.interpolation[i * 4 + channel] = controls[i];
}
constexpr std::array<unsigned char, 4> linear{20, 20, 107, 107};
constexpr std::array<unsigned char, 4> ahead{20, 20, 20, 107};
constexpr std::array<unsigned char, 4> delayed{120, 20, 120, 107};
const bone_scale::Matrix stretched{{2, 0.25f, 0, 0, 3, 0, 0, 0, 4}};

struct Motion {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    mdl::MorphRecord morph{};
    std::array<mdl::BoneKey, 2> keys{};
    std::array<mdl::MorphKey, 2> morphKeys{};
    mdl::DisplayKey display{};
    unsigned char* raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    Motion() {
        model->boneCount = 1; model->boneTable = &bone; model->boneKeys = keys.data();
        model->morphCount = 1; model->morphs = &morph; model->morphKeys = morphKeys.data();
        model->displayKeys = &display;
        display.visible = 1;
        bone.parent = -1;
        bone.slotIndex = -1; // No external-parent selector/BoneOrder entry.
        bone.type = mdl::BoneType::RotateMove;
        Identity(bone.matInit); Identity(bone.matLocal); Identity(bone.matExtra);
        for (int i = 0; i < 2; ++i) {
            keys[i].frame = i * 10; keys[i].allocated = 1; keys[i].rotation[3] = 1;
            morphKeys[i].frame = i * 10; morphKeys[i].allocated = 1;
            for (int channel = 0; channel < 4; ++channel) SetCurve(keys[i], channel, linear);
        }
        keys[0].next = 1;
        morphKeys[0].next = 1;
        keys[1].position[0] = 10; keys[1].position[1] = 20; keys[1].position[2] = 30;
        keys[1].rotation[2] = std::sqrt(0.5f); keys[1].rotation[3] = std::sqrt(0.5f);
        SetCurve(keys[1], 0, ahead); SetCurve(keys[1], 1, delayed);
        morphKeys[0].value = 0.2f; morphKeys[1].value = 0.8f;
    }
};

void NativeMotionAndRejectedScale() {
    Motion f;
    const auto originalKeys = f.keys;
    const auto originalMorphKeys = f.morphKeys;
    const auto originalBone = f.bone;
    std::vector<unsigned char> originalRecord(sizeof(*f.model));
    std::memcpy(originalRecord.data(), f.model.get(), originalRecord.size());
    // Submit the same non-identity inputs accepted by the full-profile editor,
    // import and undo paths. They must not leak into either runtime or records.
    Check(!bone_scale::SetPose(f.raw(), 0, stretched), "scale pose edit is refused");
    bone_scale::SetKey(f.raw(), 1, stretched);
    bone_scale::SetKeyCurve(f.raw(), 1, delayed);
    bone_scale::CaptureUndo(f.raw());
    {
        bone_scale::Registration registration(stretched, ahead);
        bone_scale::ApplyRegistration(f.raw(), 0, false);
    }
    bone_scale::Snapshot imported;
    imported.pose[0] = stretched; imported.keys[1] = stretched; imported.curves[1] = ahead;
    Check(!bone_scale::Restore(f.raw(), imported), "nonempty affine import is refused");
    bone_scale::Undo(f.raw(), 0); bone_scale::Redo(f.raw(), 0);
    bone_scale::ApplyTransforms(f.raw()); bone_scale::RestoreTransforms(f.raw());
    Check(std::memcmp(f.model.get(), originalRecord.data(), originalRecord.size()) == 0,
          "scale edit/import/undo leave native model and undo descriptors unchanged");
    Check(std::memcmp(&f.bone, &originalBone, sizeof(f.bone)) == 0,
          "scale operations do not change rigid bone matrices");
    Check(bone_scale::Pose(f.raw(), 0) == bone_scale::Matrix{} &&
          bone_scale::Key(f.raw(), 1) == bone_scale::Matrix{} && !bone_scale::HasPose(f.raw()),
          "scale inputs cannot become effective pose or key state");
    const auto state = bone_scale::Capture(f.raw());
    Check(state.pose.empty() && state.keys.empty() && state.curves.empty() && bone_scale::Revision(f.raw()) == 0,
          "disabled authoring has no persisted or revision state");

    Check(bone_scale::kInterpBoneAll == 4 && bone_scale::kInterpCameraAll == 6,
          "original all-channel indices are bone 4 and camera 6");
    // Golden midpoint values are independent of the disabled scale evaluator.
    Check(Near(BoneEase(f.raw(), 0, 1, 0.5f), 0.7917), "native bone X retains non-linear ease");
    Check(Near(BoneEase(f.raw(), 1, 1, 0.5f), 0.1711), "native bone Y retains independent delayed ease");
    Check(Near(BoneEase(f.raw(), 2, 1, 0.5f), 0.5), "native bone Z retains linear ease");
    Check(Near(BoneEase(f.raw(), 3, 1, 0.5f), 0.5), "native rotation remains the fourth channel");
    SeekModelFrame(f.raw(), 5, 0);
    Check(Near(f.bone.trans[0], 7.917, 0.015) && Near(f.bone.trans[1], 3.422, 0.015) && Near(f.bone.trans[2], 15),
          "real model seek evaluates three native translation curves");
    Check(Near(f.bone.rotQuat[2], 0.38268343) && Near(f.bone.rotQuat[3], 0.92387953),
          "real model seek retains quaternion rotation interpolation");
    Check(Near(f.morph.value, 0.5), "real model seek retains original linear morph interpolation");
    SeekModelFrame(f.raw(), 10, 0);
    Check(Near(f.bone.trans[0], 10) && Near(f.bone.trans[1], 20) && Near(f.bone.trans[2], 30) && Near(f.morph.value, 0.8),
          "real seek reaches the exact native bone and morph endpoint");
    SeekModelFrame(f.raw(), 0, 0);
    Check(Near(f.bone.trans[0], 0) && Near(f.morph.value, 0.2), "backward seek restores native first keys");
    Check(std::memcmp(f.keys.data(), originalKeys.data(), sizeof(f.keys)) == 0 &&
          std::memcmp(f.morphKeys.data(), originalMorphKeys.data(), sizeof(f.morphKeys)) == 0,
          "rejected extensions and seek preserve every native key byte");
}

void CameraBezierWithoutDevice() {
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    std::array<mdl::CameraKey, 2> keys{};
    app->CameraKeys() = keys.data();
    for (int i = 0; i < 2; ++i) {
        keys[i].frame = i * 10; keys[i].parentModel = keys[i].parentBone = -1;
        for (int channel = 0; channel < 6; ++channel)
            for (int j = 0; j < 4; ++j) keys[i].interpolation[j][channel] = linear[j];
    }
    keys[0].next = 1; keys[0].fov = 30; keys[1].fov = 90;
    keys[0].distance = 10; keys[1].distance = 110;
    for (int axis = 0; axis < 3; ++axis) { keys[1].eye[axis] = 10.f * (axis + 1); keys[1].target[axis] = float(axis + 1); }
    for (int j = 0; j < 4; ++j) {
        keys[1].interpolation[j][0] = ahead[j];
        keys[1].interpolation[j][1] = delayed[j];
        keys[1].interpolation[j][4] = ahead[j];
        keys[1].interpolation[j][5] = delayed[j];
    }
    const auto originalKeys = keys;
    app->CurrentFrame() = 5;
    // ReloadModels evaluates CPU state before its device guard. Null HWND and
    // renderer ensure that the test creates no controls or D3D resources.
    ReloadModels(app.get());
    Check(Near(app->CameraPositionAxis(0), 7.917, 0.015) && Near(app->CameraPositionAxis(1), 3.422, 0.015) &&
          Near(app->CameraPositionAxis(2), 15), "camera seek retains three independent native Bezier axes");
    Check(Near(app->CameraRotation()[0], 0.5) && Near(app->CameraRotation()[1], 1) && Near(app->CameraRotation()[2], 1.5),
          "camera rotation channel still controls all three Euler components");
    Check(Near(app->CameraDistance(), 89.17, 0.15), "camera distance retains its fifth native curve");
    Check(Near(app->CameraFov(), 40.266, 0.09), "camera FOV retains its sixth native curve");
    app->CurrentFrame() = 10; ReloadModels(app.get());
    Check(Near(app->CameraDistance(), 110) && Near(app->CameraFov(), 90), "camera native endpoint survives disabled curve editor");
    Check(std::memcmp(keys.data(), originalKeys.data(), sizeof(keys)) == 0, "camera evaluation never rewrites native interpolation bytes");
}

struct Physics {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
    btSphereShape sphere{1};
    btBoxShape box{btVector3(1, 2, 3)};
    btRigidBody a{btRigidBody::btRigidBodyConstructionInfo(0, nullptr, &sphere)};
    btRigidBody b{btRigidBody::btRigidBodyConstructionInfo(2, nullptr, &box, btVector3(2, 3, 4))};
    btGeneric6DofSpringConstraint joint{a, b, btTransform::getIdentity(), btTransform::getIdentity(), true};
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> bones{};
    std::array<mdl::RigidRecord, 2> rigids{};
    mdl::JointRecord jointRecord{};
    unsigned char* raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    Physics() {
        model->boneTable = bones.data(); model->boneCount = 2;
        model->rigidTable = rigids.data(); model->rigidCount = 2;
        model->jointTable = &jointRecord; model->jointCount = 1;
        for (int i = 0; i < 2; ++i) { bones[i].parent = -1; Identity(bones[i].matInit); rigids[i].boneIndex = i; }
        rigids[0].body = &a; rigids[1].body = &b;
        jointRecord.radiusBound = 37.25f;
        auto pose = btTransform::getIdentity(); pose.setOrigin(btVector3(10, -4, 6));
        pose.setRotation(btQuaternion(btVector3(0, 1, 0), 0.4f)); b.setWorldTransform(pose);
        auto frameA = btTransform::getIdentity(), frameB = frameA;
        frameA.setOrigin(btVector3(1.25f, -2.5f, 3.75f)); frameB.setOrigin(btVector3(-4, 5, -6));
        frameA.setRotation(btQuaternion(btVector3(1, 0, 0), 0.35f));
        frameB.setRotation(btQuaternion(btVector3(0, 0, 1), -0.7f));
        joint.getFrameOffsetA() = frameA;
        joint.getFrameOffsetB() = frameB;
        joint.calculateTransforms();
        joint.setLinearLowerLimit(btVector3(-1, -2, -3)); joint.setLinearUpperLimit(btVector3(4, 5, 6));
        joint.setAngularLowerLimit(btVector3(-.1f, -.2f, -.3f)); joint.setAngularUpperLimit(btVector3(.4f, .5f, .6f));
        world.addRigidBody(&a); world.addRigidBody(&b);
    }
    ~Physics() { world.removeRigidBody(&b); world.removeRigidBody(&a); }
    bool SphereContact(float x) {
        btSphereShape probeShape{0.25f};
        btRigidBody probe{btRigidBody::btRigidBodyConstructionInfo(1, nullptr, &probeShape, btVector3(1, 1, 1))};
        auto pose = btTransform::getIdentity(); pose.setOrigin(btVector3(x, 0, 0)); probe.setWorldTransform(pose);
        world.addRigidBody(&probe); world.performDiscreteCollisionDetection();
        bool contact = false;
        for (int i = 0; i < dispatcher.getNumManifolds(); ++i) {
            const auto* manifold = dispatcher.getManifoldByIndexInternal(i);
            if ((manifold->getBody0() == &probe && manifold->getBody1() == &a) ||
                (manifold->getBody1() == &probe && manifold->getBody0() == &a))
                for (int j = 0; j < manifold->getNumContacts(); ++j)
                    contact |= manifold->getContactPoint(j).getDistance() < 0;
        }
        world.removeRigidBody(&probe);
        return contact;
    }
};

void OriginalPhysicsBaselines() {
    Physics f;
    const auto bodyPose = f.b.getWorldTransform();
    const auto originalA = f.joint.getFrameOffsetA(), originalB = f.joint.getFrameOffsetB();
    const auto lower = f.joint.getTranslationalLimitMotor()->m_lowerLimit;
    const auto upper = f.joint.getTranslationalLimitMotor()->m_upperLimit;
    const auto invInertia = f.b.getInvInertiaDiagLocal();
    Check(f.SphereContact(1.1f) && !f.SphereContact(2.f), "fixture collides inside the native sphere and misses outside");
    bone_scale::SetPose(f.raw(), 0, stretched); bone_scale::SetPose(f.raw(), 1, stretched);
    bone_scale::ApplyTransforms(f.raw()); physics_scale::Sync(f.raw(), true);
    physics_scale::Readback(f.raw()); physics_scale::FinishReadback(f.raw());
    Check(physics_scale::BaseShape(&f.a) == &f.sphere && physics_scale::BaseShape(&f.b) == &f.box,
          "collision/debug consumers receive the original owned Bullet shapes");
    Check(Near(f.box.getHalfExtentsWithMargin().x(), 1) && Near(f.box.getHalfExtentsWithMargin().y(), 2) &&
          Near(f.box.getHalfExtentsWithMargin().z(), 3), "native box dimensions are neither scaled nor flattened");
    Check(f.b.getWorldTransform() == bodyPose && f.b.getInvInertiaDiagLocal() == invInertia,
          "disabled scale sync preserves physical pose and inertia");
    btTransform a, b; physics_scale::JointBaseline(f.joint, a, b);
    Check(a == originalA && b == originalB, "joint baseline preserves both translated and rotated original frames");
    Check(f.joint.getTranslationalLimitMotor()->m_lowerLimit == lower && f.joint.getTranslationalLimitMotor()->m_upperLimit == upper &&
          Near(f.joint.getRotationalLimitMotor(2)->m_hiLimit, .6), "joint linear and angular limits are preserved");
    Check(Near(physics_scale::JointRadiusBaseline(f.raw(), 0), 37.25), "joint radius uses authored data rather than a constant stub");
    f.jointRecord.radiusBound = 12.5f;
    auto changed = originalA; changed.setOrigin(btVector3(9, 8, 7));
    f.joint.getFrameOffsetA() = changed;
    f.joint.getFrameOffsetB() = originalB;
    f.joint.calculateTransforms();
    physics_scale::JointBaseline(f.joint, a, b);
    Check(a == changed && b == originalB && Near(physics_scale::JointRadiusBaseline(f.raw(), 0), 12.5),
          "baseline queries read current native edits instead of retaining stale scale caches");
    Check(f.SphereContact(1.1f) && !f.SphereContact(2.f), "rejected scale cannot enlarge the real collision boundary");
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        std::puts("PHASE native-motion");
        NativeMotionAndRejectedScale();
        std::puts("PHASE camera-bezier");
        CameraBezierWithoutDevice();
        std::puts("PHASE original-physics");
        OriginalPhysicsBaselines();
    } catch (const std::exception& error) {
        ++failures; std::fprintf(stderr, "EXCEPTION %s\n", error.what());
    }
    std::printf("original_profile_state checks=%d failures=%d windows=0 devices=0\n", checks, failures);
    return failures ? 1 : 0;
}
