#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/recording_state.hpp"

#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace mikumikustudio;

namespace {

int failures = 0;

void Check(bool value, const char* label) {
    if (value) return;
    ++failures;
    std::printf("FAIL %s\n", label);
}

struct Result {
    btVector3 position;
    std::uint64_t steps;
    double frame;
};

Result Simulate(int playbackFps, int outputFps, bool recording, bool noise = false,
                bool followPlayback = false, bool pluginOwnsPhysics = false,
                bool trackedInput = false) {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config);
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    btSphereShape shape(1);
    btTransform initial;
    initial.setIdentity();
    initial.setOrigin(btVector3(0, 100, 0));
    btDefaultMotionState motion(initial);
    btVector3 inertia;
    shape.calculateLocalInertia(1, inertia);
    btRigidBody body(btRigidBody::btRigidBodyConstructionInfo(1, &motion, &shape, inertia));
    world.addRigidBody(&body);

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    PhysicsScene scene{};
    scene.world = &world;
    app->Physics() = &scene;
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::RigidRecord rigid{};
    rigid.mode = trackedInput ? 0 : 1;
    rigid.boneIndex = -1;
    rigid.position[1] = trackedInput ? 150.f : 100.f;
    rigid.body = &body;
    model->rigidTable = &rigid;
    model->rigidCount = 1;
    model->maxBoneLayer = -1;
    model->centerBone = -1;
    model->scenePtr = &scene;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());

    auto& timing = app->PlaybackTiming();
    timing.playbackFps = playbackFps;
    timing.physics.erp = .3f;
    timing.physics.cfm = .001f;
    timing.followPlaybackFps = followPlayback;
    app->PlaybackPhysicsClock().Reset(0);
    if (trackedInput) {
        // The motion state already carries the most recent live sensor input.
        body.setCollisionFlags(body.getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        body.setActivationState(DISABLE_DEACTIVATION);
        app->PlaybackPhysicsClock().initialPosePending = false;
    }
    app->PlaybackPhysicsMode() = 1;
    app->NativePhysicsOverridden() = pluginOwnsPhysics;
    app->PlaybackActive() = 1;
    app->GravityY() = -1;
    app->GravityMagnitude() = 9.8f;
    app->GravityNoiseEnabled() = noise;
    app->GravityNoise() = 1;
    app->state.messageSeen = 1;
    app->PlaybackEndSeconds() = 10;
    app->AviRecordEndFrame() = 300;
    app->AviRecordFps() = static_cast<float>(outputFps);
    app->FrameStepPlayback() = recording;
    const int samples = 60 * outputFps / playbackFps;
    for (int i = 1; i <= samples; ++i) {
        app->TimeNowLow() = static_cast<unsigned>(std::lround(i * 1000.0 / outputFps));
        app->DeltaTime() = 1.f / outputFps;
        PlaybackCatchup(app.get(), trackedInput ? 1 : 0);
        PhysicsFrame(app.get(), 0);
    }
    if (!pluginOwnsPhysics) {
        Check(world.getSolverInfo().m_erp == .3f && world.getSolverInfo().m_globalCfm == .001f,
              "live native physics receives configured ERP and CFM");
    }
    const Result result{body.getWorldTransform().getOrigin(),
        app->PlaybackPhysicsClock().step, app->PlaybackCursorSeconds() * 30};
    app->ModelSlot(0) = nullptr;
    app->Physics() = nullptr;
    world.removeRigidBody(&body);
    return result;
}

void CheckDisplayLimit() {
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->FpsLimit() = 60.f;
    app->PlaybackTiming().playbackFps = 24;
    Check(app->EffectiveDisplayFps() == 60.f, "fixed display cap is the default");
    app->PlaybackTiming().displayFollowsPlaybackFps = true;
    Check(app->EffectiveDisplayFps() == 24.f && app->FpsLimit() == 60.f,
          "display cap follows the playback rate without discarding the fixed setting");
    app->state.kinectCaptureActive = 1;
    app->FpsLimit() = 30.f;
    Check(app->EffectiveDisplayFps() == 30.f, "Kinect capture keeps its fixed sampling cap");
}

}  // namespace

int main() {
    CheckDisplayLimit();
    const auto baseline = Simulate(30, 30, true);
    Check(baseline.steps == 120 && baseline.position.y() < 99,
          "native physics advances at the independent 60 Hz rate");
    for (int playbackFps : {24, 30, 60, 120}) {
        for (int outputFps : {24, 30, 60, 120}) {
            if ((60 * outputFps) % playbackFps != 0) continue;
            for (bool recording : {false, true}) {
                const auto result = Simulate(playbackFps, outputFps, recording);
                Check(result.steps == baseline.steps, "fixed step count is independent of FPS");
                Check((result.position - baseline.position).length() < 1e-5f,
                      "native rigid body reaches the same position at the same authored frame");
                Check(std::abs(result.frame - 60) < 1e-4, "elapsed time advances the requested playback FPS");
            }
        }
    }
    const auto windy30 = Simulate(30, 30, true, true);
    const auto windy120 = Simulate(60, 120, false, true);
    Check((windy30.position - windy120.position).length() < 1e-5f,
          "wind is independent of render and playback rates");
    Check(Simulate(120, 120, true, false, true).steps == 240,
          "linked physics uses the playback FPS as its base rate");
    const auto overridden = Simulate(60, 60, true, false, false, true);
    Check(overridden.steps == 0 && overridden.position.y() == 100.f,
          "the host leaves plugin-owned physics untouched");
    const auto tracked = Simulate(60, 60, false, false, false, false, true);
    Check(tracked.steps == 120 && tracked.position.y() == 100.f,
          "fixed catch-up keeps the tracked model's live kinematic input");
    Check(recording_state::OutputFrameCount(0, 59, 60, 60) == 60 &&
          recording_state::OutputFrameCount(0, 59, 60, 30) == 120,
          "export count uses playback and output FPS independently");
    std::printf("playback timing failures=%d\n", failures);
    return failures ? 1 : 0;
}
