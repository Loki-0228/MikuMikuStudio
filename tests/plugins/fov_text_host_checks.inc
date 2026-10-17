void FovTextPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto original = app->CameraFov(); const auto perspective = app->CameraPerspective();
    const auto distance = app->CameraDistance(); const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    const auto edit = GetDlgItem(app->MainWindow(), panel::kFovEdit); Check(edit != nullptr, "real FOV edit exists");
    const auto commit = [&](const char* text) { SetWindowTextA(edit, text); SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
        SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); };
    const auto projection = [&] {
        plugins::Refresh(app); float expected[16]{};
        float aspect = app->Renderer()->aspectRatio; if (!(aspect > 0)) aspect = 4.0f / 3.0f;
        if (app->CameraPerspective()) { expected[0] = -2.0f / app->CameraDistance(); expected[5] = aspect * expected[0]; expected[10] = .0013f; expected[15] = 1; }
        else Check(BuildCameraPerspectiveProjection(app, aspect, expected), "build expected live FOV projection");
        D3DMATRIX actual{}; Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION, &actual)), "read real device projection");
        const auto* values = reinterpret_cast<const float*>(&actual);
        for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(values[i]) || std::fabs(values[i] - expected[i]) > 1e-5f * (1 + std::fabs(expected[i]))) return false;
        return !app->cameraProjectionRefreshPending;
    };
    for (unsigned orthographic = 0; orthographic < 2; ++orthographic) {
        app->CameraMode() = 1; app->CameraFov() = 45.125f; app->CameraDistance() = -45; app->CameraPerspective() = orthographic;
        ApplyFrameCameraProjection(app);
        Menu(app, L"Edit probe prepared"); app->SceneModified() = 0; commit("67.875");
        Check(app->CameraFov() == 45.125f && !history.UndoCount() && !app->SceneModified(), "pending real plugin transaction blocks FOV text");
        Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once"); commit("67.875");
        Check(value() == 1 && history.UndoCount() == 2 && app->CameraFov() == 67 && projection() &&
            SendDlgItemMessageW(app->MainWindow(), panel::kFovSlider, TBM_GETPOS, 0, 0) == 67,
            "real FOV text commits one common step and updates correct perspective or orthographic device matrix");
        SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        Check(app->CameraFov() == 67 && history.UndoCount() == 2, "repeated FOV focus and Enter do not add history");
        Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
        for (const char* text : {"67.875", "NaN", "3 junk"}) {
            app->SceneModified() = app->RecordedFrameCountByte1() = 0; commit(text);
            Check(app->CameraFov() == 67 && history.RedoCount() == 1 && !app->SceneModified(), "same integer or invalid FOV text retains plugin redo");
        }
        Menu(app, L"Edit probe redo");
        Check(SaveSceneCopy(app, std::filesystem::absolute(orthographic ? L"fov-ortho.pmm" : L"fov-perspective.pmm").c_str()) && history.UndoCount() == 3,
            "PMM saves committed FOV and plugin edits without clearing common history");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 1 && app->CameraFov() == 45.125f && projection(), "plugin undo restores precise FOV and correct device projection after safe refresh");
        Menu(app, L"Edit probe undo"); app->CameraMode() = 0; app->CurrentFrame() = frame + 29;
        Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
        Check(value() == 2 && app->CameraFov() == 67 && projection() && app->CurrentFrame() == frame + 29,
            "FOV common redo works across editor mode and playhead without replacing projection kind");
        if (orthographic) {
            D3DMATRIX before{}, after{}; app->Renderer()->device->GetTransform(D3DTS_PROJECTION, &before);
            app->CameraDistance() = 0; commit("68"); app->Renderer()->device->GetTransform(D3DTS_PROJECTION, &after);
            Check(app->CameraFov() == 68 && !std::memcmp(&before, &after, sizeof(before)) && !app->cameraProjectionRefreshPending,
                "FOV commit at zero orthographic distance leaves device matrix finite instead of installing infinities");
            Menu(app, L"Edit probe undo");
            app->CameraDistance() = (std::numeric_limits<float>::denorm_min)(); commit("69");
            app->Renderer()->device->GetTransform(D3DTS_PROJECTION, &after);
            Check(app->CameraFov() == 69 && !std::memcmp(&before, &after, sizeof(before)),
                "FOV commit at tiny orthographic distance avoids overflow and preserves finite device matrix");
            Menu(app, L"Edit probe undo"); app->CameraDistance() = -45;
        }
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 0 && app->CameraFov() == 45.125f && history.Reset() == MMS_OK, "FOV probe restores plugin data and clears isolated history");
    }
    app->CameraFov() = original; app->CameraPerspective() = perspective; app->CameraDistance() = distance;
    app->CameraMode() = mode; app->CurrentFrame() = frame; app->cameraProjectionRefreshPending = false; ApplyFrameCameraProjection(app);
}
