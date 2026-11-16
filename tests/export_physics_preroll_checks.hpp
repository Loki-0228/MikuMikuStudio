// Compare a pre-rolled export with a continuous export using a real Bullet body.
struct PrerollProbe {
    btRigidBody* body;
    int start;
    int physicsStart;
    unsigned writes = 0;
    btVector3 position{}, velocity{};
    int sampleFrame = 10;
};
PrerollProbe* prerollProbe = nullptr;
decltype(&seq::WriteFrame) prerollWrite = nullptr;
bool ObservePrerollWrite(MMDApp* app, IDirect3DSurface9* surface, std::wstring& error) {
    auto& p = *prerollProbe;
    const double expected = double(p.start) / 30 + double(p.writes) / app->AviRecordFps();
    Check(std::abs(app->PlaybackCursorSeconds() - expected) < 1e-6,
        "pre-roll does not change output timestamps or number the warm-up frames");
    Check(!app->RecordingPhysicsPrerollActive() &&
        std::abs(app->PlaybackPhysicsClock().origin - double(p.physicsStart) / 30) < 1e-9,
        "first image is written only after physics reaches the output start");
    if (std::abs(expected - double(p.sampleFrame) / 30) < 1e-6) {
        p.position = p.body->getWorldTransform().getOrigin();
        p.velocity = p.body->getLinearVelocity();
    }
    const bool result = prerollWrite(app, surface, error);
    if (result) ++p.writes;
    return result;
}
void ExportPhysicsPrerollTests(MMDApp* app) {
    {
        negative_cursor_checks::Fixture f;
        f.Key(-1, 10);
        mdl::GravityKey zero{};
        f.app->GravityKeys() = &zero;
        zero.acceleration = 10; zero.direction[1] = -1;
        f.app->GravityMagnitude() = 20; f.app->GravityDirection()[1] = -1;
        Check(negative_frames::RegisterGlobal(f.app.get(), negative_frames::Global::Gravity, -1),
            "register negative gravity key for subframe warm-up");
        Check(recording_state::ResolvePhysicsStart(f.app.get(), 0) == -1 &&
            recording_state::ResolvePhysicsStart(f.app.get(), -60) == -60,
            "project keys move zero earlier without shortening explicitly requested warm-up");
        InitModelTrackCursors(f.Raw(), -1.0 / 30, 0);
        for (double frame : {-.75, -.5, -.25, 0.}) {
            f.app->GravityTrackActive() = 1; f.app->GravityTrackCursor() = 0;
            f.app->PlaybackCursorSeconds() = frame / 30;
            AdvancePlaybackPhysicsTracks(f.app.get());
            Check(std::abs(f.bone.trans[0] + 10 * frame) < 1e-5 &&
                std::abs(f.morph.value + .1 * frame) < 1e-5 &&
                std::abs(f.app->GravityMagnitude() - (10 - 10 * frame)) < 1e-5,
                "negative warm-up samples fractional bone, morph and gravity keys and crosses zero safely");
        }
        f.app->GravityKeys() = nullptr;
    }
    auto* world = app->Physics()->world;
    Check(world != nullptr, "pre-roll fixture has a real physics world");
    const auto mode = app->PlaybackPhysicsMode();
    app->PlaybackPhysicsMode() = 1;
    btSphereShape shape(.5f);
    btVector3 inertia; shape.calculateLocalInertia(1, inertia);
    btRigidBody::btRigidBodyConstructionInfo info(1, nullptr, &shape, inertia);
    btRigidBody body(info);
    world->addRigidBody(&body, 1, 0);
    const auto resetBody = [&] {
        btTransform transform; transform.setIdentity(); transform.setOrigin(btVector3(0, 100, 0));
        body.setCenterOfMassTransform(transform); body.setInterpolationWorldTransform(transform);
        body.setLinearVelocity(btVector3(0,0,0)); body.setAngularVelocity(btVector3(0,0,0));
        body.clearForces(); body.activate(true);
    };
    Check(MH_CreateHook(reinterpret_cast<void*>(&seq::WriteFrame), &ObservePrerollWrite,
        reinterpret_cast<void**>(&prerollWrite)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&seq::WriteFrame)) == MH_OK, "observe actual pre-rolled image writes");
    for (float fps : {30.f, 60.f}) {
        btVector3 continuousPosition, continuousVelocity;
        for (int variant = 0; variant < 3; ++variant) {
            resetBody();
            seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48;
            o.start = variant == 0 ? 0 : 10; o.end = 10; o.fps = fps;
            o.physicsStartEnabled = variant == 1; o.physicsStart = 0;
            o.prefix = L"preroll_" + std::to_wstring(int(fps)) + L"_" + std::to_wstring(variant) + L"_";
            PrerollProbe p{&body, o.start, o.physicsStartEnabled ? 0 : o.start};
            prerollProbe = &p; Start(app, o);
            unsigned pumps = 0;
            while (seq::Active(app) && pumps++ < 200) {
                app->MessageSeen() = 1; coordinator_fixture::Drive(app);
            }
            Check(!seq::Active(app) && p.writes == seq::FrameCount(o), "pre-roll preserves exact output image count");
            if (variant == 0) { continuousPosition = p.position; continuousVelocity = p.velocity; }
            else if (variant == 1) {
                Check((p.position - continuousPosition).length() < 1e-4f &&
                    (p.velocity - continuousVelocity).length() < 1e-4f,
                    "first pre-rolled image has the same real rigid-body state as continuous output");
            } else {
                Check((p.position - continuousPosition).length() > .01f,
                    "unchecked option retains the previous start-at-output physics behavior");
            }
        }
        // With constant input, -30..10 must match the same 40-frame duration
        // at 0..40. Only the requested 0..10 images are written after warm-up.
        for (int variant = 0; variant < 2; ++variant) {
            resetBody();
            seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48;
            o.start = 0; o.end = variant == 0 ? 40 : 10; o.fps = fps;
            o.physicsStartEnabled = variant == 1; o.physicsStart = -30;
            o.prefix = L"negative_preroll_" + std::to_wstring(int(fps)) + L"_" + std::to_wstring(variant) + L"_";
            PrerollProbe p{&body, o.start, variant == 0 ? 0 : -30}; p.sampleFrame = o.end;
            prerollProbe = &p; Start(app, o);
            for (unsigned pumps = 0; seq::Active(app) && pumps < 300; ++pumps) {
                app->MessageSeen() = 1; coordinator_fixture::Drive(app);
            }
            Check(!seq::Active(app) && p.writes == seq::FrameCount(o), "negative warm-up adds no output images");
            if (variant == 0) { continuousPosition = p.position; continuousVelocity = p.velocity; }
            else Check((p.position - continuousPosition).length() < 1e-4f &&
                (p.velocity - continuousVelocity).length() < 1e-4f && p.position.y() < 99,
                "negative physics frames really advance the body and match continuous simulation");
        }
    }
    resetBody();
    seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48;
    o.start = o.end = 0; o.physicsStartEnabled = true; o.physicsStart = -100000; o.prefix = L"preroll_cancel_";
    PrerollProbe cancelled{&body, o.start, o.physicsStart}; prerollProbe = &cancelled;
    app->CurrentFrame() = 7; Start(app, o);
    app->MessageSeen() = 1; coordinator_fixture::Drive(app);
    Check(app->RecordingPhysicsPrerollActive() && cancelled.writes == 0,
        "long pre-roll yields to the message loop without writing any output");
    FinishAviRecord(app);
    Check(!seq::Active(app) && !app->RecordingPhysicsPrerollActive() && !app->RecordingWindow() &&
        !app->FrameStepPlayback() && !app->PlaybackActive() && app->AviRecordStartFrame() == o.start,
        "cancelling pre-roll clears recording state without changing the output range");
    resetBody(); o.start = o.end = 10; o.physicsStart = 0; o.prefix = L"preroll_retry_";
    Check(negative_frames::RegisterGlobal(app, negative_frames::Global::Gravity, -30),
        "register a negative project key before requesting physics from zero");
    PrerollProbe retried{&body, o.start, -30}; prerollProbe = &retried; Start(app, o);
    for (unsigned pumps = 0; seq::Active(app) && pumps < 100; ++pumps) {
        app->MessageSeen() = 1; coordinator_fixture::Drive(app);
    }
    Check(!seq::Active(app) && retried.writes == 1, "a cancelled pre-roll can be retried as a fresh export");
    negative_frames::Reset(app);
    prerollProbe = nullptr;
    MH_DisableHook(reinterpret_cast<void*>(&seq::WriteFrame)); MH_RemoveHook(reinterpret_cast<void*>(&seq::WriteFrame));
    world->removeRigidBody(&body); app->PlaybackPhysicsMode() = mode;
    std::puts("PASS export physics pre-roll: continuous body state, output count, disabled behavior, cancellation");
}
