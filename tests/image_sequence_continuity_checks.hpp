// Included by image_sequence_tests.cpp; exercise production tracks and output.
void SceneRangeTests(MMDApp* host) {
    auto fixture = std::make_unique<MMDApp>();
    std::memset(&fixture->state, 0, sizeof(fixture->state)); fixture->InitDefaults();
    auto* app = fixture.get(); app->MainWindow() = host->MainWindow();
    const auto stop = Read(host->MainWindow(), panel::kPlayStopFrameEdit);
    SetDlgItemInt(app->MainWindow(), panel::kPlayStopFrameEdit, 5, FALSE);
    app->LastRegisteredFrame() = 99999;
    Check(seq::SceneEndFrame(app) == 0, "empty scene ignores stale global and playback range values");
    Check(seq::DefaultEndFrame(app) == 5, "video dialog still honors an explicit playback range");
    const auto checkTrack = [&](auto*& keys, unsigned frame, const char* label) {
        using Key = std::remove_pointer_t<std::decay_t<decltype(keys)>>;
        keys = mdl::KeyPoolCreate<Key>(3);
        keys[0].next = 1; keys[1].frame = frame;
        keys[2].frame = 999999; // Unlinked/deleted keys are not scene content.
        Check(seq::SceneEndFrame(app) == frame, label);
        keys[0].next = 0;
        Check(seq::SceneEndFrame(app) == 0, "removing the terminal key shrinks the detected range");
        mdl::KeyPoolDestroy(keys);
    };
    checkTrack(app->CameraKeys(), 180, "detect camera-only scene");
    checkTrack(app->LightKeys(), 240, "detect light-only scene");
    checkTrack(app->ShadowKeys(), 300, "detect shadow-only scene");
    checkTrack(app->GravityKeys(), 360, "detect gravity-only scene");
    auto accessory = std::make_unique<mdl::AccessoryRecord>();
    app->AccessorySlot(17) = accessory.get();
    checkTrack(app->AccessoryKeys(17), 420, "detect accessory track outside the selected slot");
    app->AccessorySlot(17) = nullptr;
    auto model = std::make_unique<mdl::ModelRecord>();
    app->ModelSlot(9) = reinterpret_cast<unsigned char*>(model.get());
    model->maxFrame = 999999; // This cached high-water mark must not win.
    model->boneCount = 1; model->boneKeys = mdl::BoneKeyPoolCreate(3);
    model->boneKeys[0].next = 1; model->boneKeys[1].frame = 600;
    Check(seq::SceneEndFrame(app) == 600, "detect bone motion in an unselected model");
    model->boneKeys[0].next = 0;
    mdl::BoneKeyPoolDestroy(model->boneKeys);
    model->morphCount = 1; model->morphKeys = mdl::MorphKeyPoolCreate(24002);
    model->morphKeys[0].next = 24001; model->morphKeys[24001].frame = 720;
    Check(seq::SceneEndFrame(app) == 720, "detect morph keys in a grown pool");
    mdl::MorphKeyPoolDestroy(model->morphKeys);
    checkTrack(model->displayKeys, 840, "detect display and IK motion");
    app->ModelSlot(9) = nullptr;
    WaveAudioContext audio{}; app->Audio() = &audio;
    audio.dataSize = 1001; audio.format.nAvgBytesPerSec = 300;
    Check(seq::SceneEndFrame(app) == 101, "audio duration rounds up to the final timeline frame");
    audio.format.nAvgBytesPerSec = 0;
    Check(seq::SceneEndFrame(app) == 0, "invalid audio rate cannot extend the range");
    audio.dataSize = INT_MAX; audio.format.nAvgBytesPerSec = 1;
    Check(seq::SceneEndFrame(app) == unsigned(frame_limits::maximum), "long audio is clamped before integer conversion");
    app->Audio() = nullptr; app->MainWindow() = nullptr;
    SetDlgItemTextW(host->MainWindow(), panel::kPlayStopFrameEdit, stop.c_str());
}

struct ContinuityProbe {
    seq::Options options;
    unsigned written = 0;
    float previousEye = 0;
};
ContinuityProbe* continuity = nullptr;
decltype(&seq::WriteFrame) continuityWrite = nullptr;
bool ObserveContinuousWrite(MMDApp* app, IDirect3DSurface9* surface, std::wstring& error) {
    auto& p = *continuity;
    const double expected = double(p.options.start) / 30.0 + double(p.written) / p.options.fps;
    Check(app->PlaybackActive() && app->FrameStepPlayback(), "one continuous recording session remains active for every image");
    Check(std::abs(app->PlaybackCursorSeconds() - expected) < 1e-6 &&
        std::abs(ExpGetFrameTime() - expected) < 1e-5, "effect time follows the output frame count, not the wall clock");
    Check(std::abs(app->DeltaTime() - 1.0f / p.options.fps) < 1e-6,
        "effect elapsed time follows output FPS regardless of save latency");
    const float eye = app->CameraPositionX();
    Check(eye >= 2 && eye < 12 && (p.written == 0 || eye > p.previousEye),
        "camera keys advance continuously instead of using the free view");
    if (p.written == 0 && p.options.start == 0)
        Check(std::abs(eye - 2) < 1e-6 && std::abs(app->CameraDistance() + 45) < 1e-6,
            "frame zero uses the registered camera before the first image");
    D3DMATRIX world{};
    Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_WORLD, &world)) &&
        std::abs(world._41 + app->CameraPositionX()) < 1e-5f &&
        std::abs(world._42 + app->CameraPositionY()) < 1e-5f,
        "image sequence renders the sampled camera target in either editor mode");
    p.previousEye = eye;
    const bool result = continuityWrite(app, surface, error);
    if (result) ++p.written;
    return result;
}
void CameraAndContinuityTests(MMDApp* app) {
    wchar_t modelPath[32768]{};
    if (GetEnvironmentVariableW(L"MMS_MENU_SEQUENCE_MODEL", modelPath, std::size(modelPath)))
        LoadModelFile(app, modelPath, false);
    auto* keys = app->CameraKeys();
    const auto zero = keys[0], next = keys[1];
    keys[0].next = 1; keys[0].eye[0] = 2; keys[0].eye[1] = 10;
    keys[0].distance = -45; keys[0].fov = 30; keys[0].parentModel = -1;
    std::memset(keys[0].target, 0, sizeof keys[0].target);
    keys[1] = keys[0]; keys[1].frame = 30; keys[1].previous = 0; keys[1].next = 0;
    keys[1].eye[0] = 12;
    for (int channel = 0; channel < 6; ++channel) {
        keys[1].interpolation[0][channel] = keys[1].interpolation[1][channel] = 20;
        keys[1].interpolation[2][channel] = keys[1].interpolation[3][channel] = 107;
    }
    Check(MH_CreateHook(reinterpret_cast<void*>(&seq::WriteFrame), &ObserveContinuousWrite,
        reinterpret_cast<void**>(&continuityWrite)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&seq::WriteFrame)) == MH_OK,
        "observe real sequence output time and camera");
    const auto mode = app->CameraMode();
    for (int cameraMode : {0, 1}) for (int start : {0, 10}) for (float fps : {30.f, 60.f}) {
        if (!cameraMode && !app->SelectedModel()) continue;
        app->CameraMode() = cameraMode; app->CurrentFrame() = start;
        app->CameraPositionX() = 99; app->CameraDistance() = -5;
        app->ViewOffsetX() = 8; app->ViewOffsetY() = 7;
        const auto editorView = [&] {
            return std::array<float,10>{app->CameraPositionX(),app->CameraPositionY(),app->CameraPositionZ(),
                app->CameraPitch(),app->CameraYaw(),app->CameraRoll(),app->ViewOffsetX(),app->ViewOffsetY(),
                app->CameraDistance(),app->CameraFov()};
        };
        const auto beforeView = editorView();
        const auto perspective = app->CameraPerspective();
        ContinuityProbe p;
        p.options.directory = output.wstring(); p.options.width = 64; p.options.height = 48;
        p.options.start = start; p.options.end = start + 2; p.options.fps = fps;
        p.options.prefix = L"continuous_" + std::to_wstring(cameraMode) + L"_" +
            std::to_wstring(start) + L"_" + std::to_wstring(int(fps)) + L"_";
        continuity = &p; Start(app, p.options);
        unsigned steps = 0;
        while (seq::Active(app) && steps++ < 30) {
            // Saving a frame can take longer than the output frame interval.
            const float wallDelta = steps % 2 ? .2f : .003f;
            app->DeltaTime() = wallDelta; app->state.messageSeen = 1; FrameDriver(app);
            Check(app->DeltaTime() == wallDelta, "export restores the caller's wall-clock delta");
            Sleep(steps % 2 ? 2 : 55);
        }
        Check(!seq::Active(app) && p.written == seq::FrameCount(p.options),
            "continuous output writes the complete range without skipped frames");
        Check(app->CameraMode() == cameraMode, "export preserves the editor's selected mode");
        if (!cameraMode && !app->state.followCameraEnabled)
            Check(editorView() == beforeView && app->CameraPerspective() == perspective,
                "image sequence restores the untracked model editing viewpoint");
    }
    continuity = nullptr;
    MH_DisableHook(reinterpret_cast<void*>(&seq::WriteFrame)); MH_RemoveHook(reinterpret_cast<void*>(&seq::WriteFrame));
    app->CameraMode() = mode; keys[0] = zero; keys[1] = next;
    ResetAppState(app);
}
