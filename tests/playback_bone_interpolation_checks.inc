// Exercise the complete playback/physics pose path at fractional timeline times.
void CheckBonePlaybackInterpolation() {
    for (int scenario : {0, 1, 2, 3}) {
        const bool transition = scenario == 1 || scenario == 2;
        const bool moving = scenario == 3;
        const bool attached = transition || moving;
        const bool eased = scenario == 2;
        for (int physicsMode : {0, 1, 2}) {
            if (attached && physicsMode != 2) continue;
            for (int outputFps : {60, 120, 240, 480}) {
                if (moving && outputFps <= 240) continue;
                btDefaultCollisionConfiguration config;
                btCollisionDispatcher dispatcher(&config);
                btDbvtBroadphase broadphase;
                btSequentialImpulseConstraintSolver solver;
                DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
                PhysicsScene scene{};
                scene.world = &world;
                auto app = std::make_unique<MMDApp>();
                app->state = MMDAppState{};
                auto model = std::make_unique<mdl::ModelRecord>();
                mdl::BoneRecord bone{};
                std::array<mdl::BoneKey, 7> keys{};
                mdl::DisplayKey display{};
                mdl::BoneOrderEntry order{};
                mdl::BoneReference attachment{-1, -1};
                order.linkedModel = order.linkedBone = -1;
                display.selectorStates = &attachment;
                unsigned cursor = 0;
                unsigned char active = 1;
                auto* raw = reinterpret_cast<unsigned char*>(model.get());
                model->boneCount = 1;
                model->boneTable = &bone;
                model->boneKeys = keys.data();
                model->boneKeyCursors = &cursor;
                model->boneTrackActive = &active;
                model->displayKeys = &display;
                model->boneOrderTable = &order;
                model->boneOrderCount = 1;
                model->centerBone = -1;
                model->scenePtr = &scene;
                model->physicsMode = 2;
                bone.parent = bone.tailIdx = bone.slotIndex = -1;
                bone.type = mdl::BoneType::RotateMove;
                bone.rotQuat[3] = bone.rotQuat2[3] = bone.ikBackup[6] = 1;
                btSphereShape shape(1);
                const btQuaternion startRotation(btVector3(0, 0, 1), .6f);
                btDefaultMotionState motion(btTransform(startRotation, btVector3(5, 0, 0)));
                btVector3 inertia;
                shape.calculateLocalInertia(1, inertia);
                btRigidBody body(btRigidBody::btRigidBodyConstructionInfo(1, &motion, &shape, inertia));
                mdl::RigidRecord rigid{};
                if (attached) {
                    world.addRigidBody(&body);
                    rigid.body = &body;
                    rigid.boneIndex = 0;
                    rigid.mode = 1;
                    for (int i = 0; i < 16; i += 5) rigid.invTransform[i] = 1;
                    model->rigidTable = &rigid;
                    model->rigidCount = 1;
                    bone.hasRigidBody = 1;
                    bone.ikBackup[0] = 5;
                    bone.ikBackup[5] = std::sin(.3f);
                    bone.ikBackup[6] = std::cos(.3f);
                }
                for (int i = 0; i < 16; i += 5)
                    bone.matInit[i] = bone.matWorld[i] = bone.matLocal[i] = 1;
                for (unsigned i = 0; i < keys.size(); ++i) {
                    auto& key = keys[i];
                    key.frame = i;
                    key.previous = i ? i - 1 : 0;
                    key.next = i + 1 < keys.size() ? i + 1 : 0;
                    key.position[0] = float(i);
                    key.rotation[2] = attached ? 0 : std::sin(float(i) * .1f);
                    key.rotation[3] = attached ? 1 : std::cos(float(i) * .1f);
                    key.physicsDisabled = transition && i > 0;
                    for (int c = 0; c < 4; ++c) {
                        key.interpolation[c] = key.interpolation[c + 4] = 20;
                        key.interpolation[c + 8] = key.interpolation[c + 12] = 107;
                        if (eased && i == 1) {
                            key.interpolation[c] = key.interpolation[c + 4] = 0;
                            key.interpolation[c + 8] = 127;
                            key.interpolation[c + 12] = 0;
                        }
                    }
                }
                display.visible = 1;
                app->Physics() = &scene;
                app->ModelSlot(0) = raw;
                app->PlaybackPhysicsMode() = physicsMode;
                app->PlaybackTiming().interpolate = true;
                app->PlaybackActive() = 1;
                app->FrameStepPlayback() = outputFps <= 240;
                app->AviRecordFps() = float(outputFps);
                app->AviRecordEndFrame() = 300;
                app->PlaybackEndSeconds() = 10;
                app->state.messageSeen = 1;
                app->PlaybackPhysicsClock().Reset(0);
                InitModelTrackCursors(raw, 0, physicsMode);
                PreparePlaybackPhysics(app.get(), 0);
                if (moving) {
                    // Playback initialization reseats the dynamic body on key 0.
                    Check(body.getWorldTransform().getOrigin().length2() < 1e-10f &&
                          std::abs(body.getWorldTransform().getRotation().w() - 1.f) < 1e-6f,
                          "dynamic playback starts from the initialized zero-frame pose");
                    body.setLinearVelocity(btVector3(3, 0, 0));
                }
                if (transition) {
                    PlaybackPoseAdvance(app.get(), 0);
                    PhysicsFrame(app.get(), 0);
                    Check(std::abs(bone.matInit[12] - 5) < 1e-5 &&
                          std::abs(bone.matInit[0] - std::cos(.6)) < 1e-5,
                          "the transition start keeps the simulated position and rotation");
                }
                double maxPositionError = 0, maxRotationError = 0;
                const int samples = outputFps / 5;
                for (int sample = 1; sample <= samples; ++sample) {
                    app->DeltaTime() = 1.f / outputFps;
                    app->TimeNowLow() = unsigned(std::lround(sample * 1000.0 / outputFps));
                    double frame = outputFps <= 240 ? sample * 30.0 / outputFps
                        : app->TimeNowLow() * .03;
                    const auto stepsBefore = app->PlaybackPhysicsClock().step;
                    const bool physicsDue = physicsMode && (outputFps <= 240
                        ? app->PlaybackPhysicsClock().Due(frame / 30, 60)
                        : app->PlaybackPhysicsClock().Completed(60) + 1e-12 < frame / 30);
                    const int collisionFlags = body.getCollisionFlags();
                    const auto bodyMode = rigid.mode;
                    const auto boneMode = bone.physicsDisabled;
                    const auto armedFrame = bone.rigidIdx;
                    const auto bodyPose = body.getWorldTransform();
                    const auto velocity = body.getLinearVelocity();
                    PlaybackCatchup(app.get(), 0);
                    PhysicsFrame(app.get(), 0);
                    if (transition && !physicsDue) {
                        Check(app->PlaybackPhysicsClock().step == stepsBefore &&
                              collisionFlags == body.getCollisionFlags() && bodyMode == rigid.mode &&
                              boneMode == bone.physicsDisabled && armedFrame == bone.rigidIdx &&
                              bodyPose == body.getWorldTransform() && velocity == body.getLinearVelocity(),
                              "fractional display samples do not step or change the rigid-body transition");
                    }
                    double blend = (std::min)(frame, 1.0);
                    if (eased && frame < 1) {
                        // Independent inverse of x(u)=3u^2-2u^3, y(u)=u^3.
                        double left = 0, right = 1;
                        for (int iteration = 0; iteration < 64; ++iteration) {
                            const double u = (left + right) * .5;
                            if (3 * u * u - 2 * u * u * u < frame) left = u;
                            else right = u;
                        }
                        const double u = (left + right) * .5;
                        blend = u * u * u;
                    }
                    maxPositionError = (std::max)(maxPositionError,
                        std::abs(double(bone.matInit[12]) - (moving ? frame * .1 :
                            transition && frame < 1 ? 5 - 4 * blend : frame)));
                    maxRotationError = (std::max)(maxRotationError,
                        std::abs(double(bone.matInit[0]) -
                            std::cos(moving ? 0 : transition ? .6 * (1 - blend) : frame * .2)));
                }
                std::printf("BONE_SUBFRAME scenario=%d physics=%d output=%d position_error=%.9g rotation_error=%.9g\n",
                            scenario, physicsMode, outputFps, maxPositionError, maxRotationError);
                // Native easing uses a 12-step float inversion of the curve.
                Check(maxPositionError < (eased ? .002 : 1e-5), "dense bone translation reaches every fractional render sample");
                Check(maxRotationError < (eased ? .0002 : 1e-5), "dense bone rotation reaches every fractional render sample");
                if (transition) {
                    keys.back().physicsDisabled = 0;
                    const auto collisionFlags = body.getCollisionFlags();
                    const auto bodyMode = rigid.mode;
                    const auto boneMode = bone.physicsDisabled;
                    SampleModelPlaybackVisual(raw, 6.0 / 30, physicsMode);
                    SampleModelPlaybackVisual(raw, 6.25 / 30, physicsMode);
                    Check(collisionFlags == body.getCollisionFlags() && bodyMode == rigid.mode &&
                          boneMode == bone.physicsDisabled && active != 0,
                          "exact and terminal display samples leave the next physics switch pending");
                    AdvanceModelKeyframes(raw, 6.25 / 30, physicsMode);
                    Check(bone.physicsDisabled == 0 && rigid.mode == 1 && !body.isKinematicObject(),
                          "the next authoritative physics sample still applies the terminal switch");
                }
                app->ModelSlot(0) = nullptr;
                app->Physics() = nullptr;
                if (attached) world.removeRigidBody(&body);
            }
        }
    }
}
