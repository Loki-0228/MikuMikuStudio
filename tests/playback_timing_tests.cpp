#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "../src/physics/playback_constraint.hpp"
#include <btBulletDynamicsCommon.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace mikumikustudio;
namespace mikumikustudio {
void PlaybackCatchup(MMDApp*, unsigned char);
void PhysicsFrame(MMDApp*, unsigned char);
void ResetPlaybackPhysics(MMDApp*);
}
namespace {
int failures = 0;
void Check(bool value, const char* label) {
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
struct Result { btVector3 position; std::uint64_t ticks; double frame; };

void CheckRecordingStartKey(std::uint32_t frame) {
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    mdl::DisplayKey keys[3]{};
    keys[0].next = 1;
    keys[1].frame = frame;
    keys[1].next = 2;
    keys[1].visible = 1;
    keys[2].frame = frame + 1;
    keys[2].previous = 1;
    mdl::DisplayKeys(raw) = keys;

    // The seconds round trip can put an integer start on either side of its key.
    const double start = double(frame) / 30.0;
    InitModelTrackCursors(raw, start, 0);
    AdvanceModelKeyframes(raw, start, 0);
    Check(model->displayKeyCursor == 1 && model->loadComplete,
          "the first recorded sample evaluates the exact nonzero start key");
    AdvanceModelKeyframes(raw, (frame + 0.5) / 30.0, 0);
    Check(model->displayKeyCursor == 2 && model->loadComplete,
          "a fractional sample stays between the start key and its successor");
}

Result Simulate(int playbackFps, int outputFps, bool recording, bool noise = false,
                bool jointed = false, float erp = .3f, float cfm = .001f,
                bool physicsEnabled = true) {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config);
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    btSphereShape shape(1);
    btTransform initial; initial.setIdentity(); initial.setOrigin(btVector3(0, 100, 0));
    btDefaultMotionState motion(initial);
    btVector3 inertia; shape.calculateLocalInertia(1, inertia);
    btRigidBody body(btRigidBody::btRigidBodyConstructionInfo(1, &motion, &shape, inertia));
    world.addRigidBody(&body);
    btTransform anchorPose; anchorPose.setIdentity(); anchorPose.setOrigin(btVector3(0, 100, 0));
    btDefaultMotionState anchorMotion(anchorPose);
    btRigidBody anchor(btRigidBody::btRigidBodyConstructionInfo(0, &anchorMotion, &shape));
    btTransform jointFrame; jointFrame.setIdentity();
    PlaybackSpringConstraint joint(anchor, body, jointFrame, jointFrame, true);
    joint.setLinearLowerLimit(btVector3(0, -1, 0));
    joint.setLinearUpperLimit(btVector3(0, 1, 0));
    if (jointed) { world.addRigidBody(&anchor); world.addConstraint(&joint, true); }
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    PhysicsScene scene{}; scene.world = &world;
    app->Physics() = &scene;
    app->PlaybackTiming().playbackFps = playbackFps;
    app->PlaybackTiming().physics.erp = erp;
    app->PlaybackTiming().physics.cfm = cfm;
    app->PlaybackPhysicsClock().Reset(0);
    app->PlaybackPhysicsMode() = physicsEnabled ? 1 : 0;
    app->PlaybackActive() = 1;
    app->GravityY() = -1;
    app->GravityMagnitude() = 9.8f;
    app->GravityNoiseEnabled() = noise;
    app->GravityNoise() = 1;
    app->state.messageSeen = 1;
    app->PlaybackEndSeconds() = 10;
    app->MilliToSec() = .001f;
    app->AviRecordEndFrame() = 300;
    app->AviRecordFps() = static_cast<float>(outputFps);
    app->FrameStepPlayback() = recording;
    ResetPlaybackPhysics(app.get());
    const int samples = 2 * outputFps * 30 / playbackFps;
    for (int i = 1; i <= samples; ++i) {
        app->TimeNowLow() = static_cast<unsigned>(std::lround(i * 1000.0 / outputFps));
        app->DeltaTime() = 1.f / outputFps;
        PlaybackCatchup(app.get(), 0);
        // The normal render tail may evaluate a visual pose, but must not
        // integrate or repair the world again at display cadence.
        PhysicsFrame(app.get(), 0);
    }
    if (physicsEnabled)
        Check(world.getSolverInfo().m_erp == erp && world.getSolverInfo().m_globalCfm == cfm,
              "live physics receives configured ERP and CFM");
    const Result result{body.getWorldTransform().getOrigin(),
        app->PlaybackPhysicsClock().ticks, app->PlaybackCursorSeconds() * 30};
    app->Physics() = nullptr;
    if (jointed) { world.removeConstraint(&joint); world.removeRigidBody(&anchor); }
    world.removeRigidBody(&body);
    return result;
}

std::array<float, 2> SampleDrivenBody(int displayFps, bool tracked) {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config);
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    btSphereShape shape(1);
    btTransform initial;
    initial.setIdentity();
    btDefaultMotionState motion(initial);
    btRigidBody body(btRigidBody::btRigidBodyConstructionInfo(0, &motion, &shape));
    body.setCollisionFlags(btCollisionObject::CF_KINEMATIC_OBJECT);
    body.setActivationState(DISABLE_DEACTIVATION);
    world.addRigidBody(&body);

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    PhysicsScene scene{};
    scene.world = &world;
    app->Physics() = &scene;
    app->PlaybackPhysicsMode() = 1;
    app->PlaybackActive() = 1;
    app->SelectedModelSlot() = 0;
    app->PlaybackEndSeconds() = 10;
    app->MilliToSec() = .001f;
    app->state.messageSeen = 1;

    auto model = std::make_unique<mdl::ModelRecord>();
    auto* raw = reinterpret_cast<unsigned char*>(model.get());
    mdl::BoneRecord bone{};
    bone.parent = bone.slotIndex = -1;
    bone.rotQuat[3] = bone.rotQuat2[3] = 1;
    mdl::BoneKey keys[2]{};
    keys[0].next = 1;
    keys[0].rotation[3] = keys[1].rotation[3] = 1;
    keys[0].allocated = keys[1].allocated = 1;
    keys[1].frame = 60;
    keys[1].position[0] = 12;
    mdl::DisplayKey display{};
    display.visible = 1;
    mdl::BoneOrderEntry externalParent{};
    externalParent.linkedModel = -1;
    unsigned char active{};
    std::uint32_t cursor{};
    mdl::RigidRecord rigid{};
    rigid.boneIndex = 0;
    rigid.staticFlag = 1;
    rigid.body = &body;
    model->boneCount = model->rigidCount = 1;
    model->boneTable = &bone;
    model->boneTrackActive = &active;
    model->boneKeyCursors = &cursor;
    model->rigidTable = &rigid;
    model->scenePtr = &scene;
    mdl::BoneKeys(raw) = keys;
    mdl::DisplayKeys(raw) = &display;
    mdl::BoneOrder(raw) = &externalParent;
    app->ModelSlot(0) = raw;

    InitModelTrackCursors(raw, 0, 1);
    app->PlaybackPhysicsClock().Reset(0);
    ResetPlaybackPhysics(app.get());
    if (tracked) {
        // The tracking pass feeds the motion state before the next fixed tick.
        initial.setOrigin(btVector3(25, 0, 0));
        motion.setWorldTransform(initial);
    }
    std::array<float, 2> samples{};
    for (int i = 1; i <= 2 * displayFps; ++i) {
        app->TimeNowLow() = static_cast<unsigned>(std::lround(i * 1000.0 / displayFps));
        app->DeltaTime() = 1.f / displayFps;
        PlaybackCatchup(app.get(), tracked ? 1 : 0);
        if (i % displayFps == 0)
            samples[i / displayFps - 1] = body.getWorldTransform().getOrigin().x();
        if (!tracked) PhysicsFrame(app.get(), 0);
    }
    app->ModelSlot(0) = nullptr;
    app->Physics() = nullptr;
    world.removeRigidBody(&body);
    return samples;
}
}

int main() {
    CheckRecordingStartKey(31);
    CheckRecordingStartKey(123);
    const auto baseline = Simulate(30, 30, true);
    Check(baseline.ticks == 120 && baseline.position.y() < 99, "fixed physics advances at independent 60 Hz");
    for (int playbackFps : {24, 30, 60, 120}) {
        for (int outputFps : {24, 30, 60, 120}) {
            // Select combinations ending at exactly the same timeline frame.
            if ((60 * outputFps) % playbackFps != 0) continue;
            for (bool recording : {false, true}) {
                const auto result = Simulate(playbackFps, outputFps, recording);
                Check(result.ticks == baseline.ticks, "physics tick count is independent of sampling and playback FPS");
                Check((result.position - baseline.position).length() < 1e-5f, "same timeline frame has identical physics");
                Check(std::abs(result.frame - 60) < 1e-4, "playback speed advances the requested timeline rate");
            }
        }
    }
    const auto windy30 = Simulate(30, 30, true, true);
    const auto windy120 = Simulate(60, 120, true, true);
    Check((windy30.position - windy120.position).length() < 1e-5f, "wind is deterministic across display and playback rates");
    const auto joint30 = Simulate(30, 30, true, false, true);
    const auto joint120 = Simulate(60, 120, true, false, true);
    Check((joint30.position - joint120.position).length() < 1e-5f,
          "joint solving is deterministic across output and playback rates");
    const auto softJoint = Simulate(30, 30, true, false, true, .1f, .1f);
    Check((joint30.position - softJoint.position).length() > 1e-4f,
          "ERP and CFM change the actual joint solution");
    Check(recording_state::OutputFrameCount(0, 59, 60, 60) == 60 &&
          recording_state::OutputFrameCount(0, 59, 60, 30) == 120,
          "export duration follows playback FPS independently of output sampling");
    const auto disabled = Simulate(60, 120, true, false, false, .3f, .001f, false);
    Check(disabled.ticks == 0 && disabled.position.y() == 100 &&
          std::abs(disabled.frame - 60) < 1e-4,
          "disabled physics skips integration while playback continues");
    const auto driven24 = SampleDrivenBody(24, false);
    const auto driven120 = SampleDrivenBody(120, false);
    Check(std::abs(driven24[0] - 6.f) < 1e-4f && std::abs(driven24[1] - 12.f) < 1e-4f,
          "native bone keys drive the actual kinematic body through the fixed steps");
    Check(driven24 == driven120,
          "bone-driven kinematic samples agree across display cadences");
    Check(SampleDrivenBody(60, true) == std::array<float, 2>{25.f, 25.f},
          "fixed catch-up preserves the selected model's pre-fed tracking transform");
    std::printf("playback timing failures=%d\n", failures);
    return failures ? 1 : 0;
}
