void LightTextPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto pose = [&] { return std::array<float, 6>{app->LightColor()[0], app->LightColor()[1], app->LightColor()[2],
        app->LightDirection()[0], app->LightDirection()[1], app->LightDirection()[2]}; };
    const auto original = pose(); const auto originalLight = app->SceneLight(); const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    const std::vector<mdl::LightKey> keys(app->LightKeys(), app->LightKeys() + mdl::KeyPoolCapacity(app->LightKeys()));
    auto& model = *mdl::Mdl(app->ModelSlot(0)); const auto ring = model.undoState[0]; const auto bone = model.boneTable[0].trans[1];
    const auto coherent = [&] {
        // Plugin undo defers HWND/device notifications until its callback has
        // returned. Flush that production boundary before inspecting the GPU.
        plugins::Refresh(app);
        D3DLIGHT9 device{}; Check(SUCCEEDED(app->Renderer()->device->GetLight(0, &device)), "read actual device light after publication");
        const auto& cpu = app->SceneLight();
        return cpu.Ambient.r == app->LightColor()[0] && cpu.Ambient.g == app->LightColor()[1] && cpu.Ambient.b == app->LightColor()[2] &&
            cpu.Specular.r == app->LightColor()[0] && cpu.Specular.g == app->LightColor()[1] && cpu.Specular.b == app->LightColor()[2] &&
            cpu.Direction.x == app->LightDirection()[0] && cpu.Direction.y == app->LightDirection()[1] && cpu.Direction.z == app->LightDirection()[2] &&
            !std::memcmp(&device.Ambient, &cpu.Ambient, sizeof(cpu.Ambient)) && !std::memcmp(&device.Specular, &cpu.Specular, sizeof(cpu.Specular)) &&
            !std::memcmp(&device.Direction, &cpu.Direction, sizeof(cpu.Direction));
    };
    for (unsigned field = 0; field < 6; ++field) {
        const std::array<float, 6> source{.25f, .35f, .45f, .3f, .4f, .5f};
        for (unsigned i = 0; i < 3; ++i) { app->LightColor()[i] = source[i]; app->LightDirection()[i] = source[i + 3]; }
        app->ApplyTimelineLightState();
        const auto edit = GetDlgItem(app->MainWindow(), 461 + int(field)); Check(edit != nullptr, "real light text control exists");
        const auto commit = [&](const char* text) { SetWindowTextA(edit, text); SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
            SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); };
        const auto text = field < 3 ? "128.375" : ".1234567";
        Menu(app, L"Edit probe prepared"); app->SceneModified() = 0; commit(text);
        Check(pose() == source && !history.UndoCount() && !app->SceneModified(), "pending real plugin edit blocks light text without scalar changes");
        Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once"); commit(text);
        auto target = source; target[field] = field < 3 ? .5f : .123456f;
        Check(value() == 1 && pose() == target && history.UndoCount() == 2 && coherent(),
            "actual light text joins common history and synchronizes scalar CPU mirror and device without key sampling");
        Check(SendDlgItemMessageW(app->MainWindow(), 455 + int(field), TBM_GETPOS, 0, 0) == (field < 3 ? 128 : 12),
            "actual paired light slider follows committed text");
        SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        Check(pose() == target && history.UndoCount() == 2, "real repeated focus and Enter retain precise unregistered light value");
        Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
        for (const char* input : {text, "NaN", "3 junk"}) {
            app->SceneModified() = app->RecordedFrameCountByte1() = 0; commit(input);
            Check(pose() == target && history.RedoCount() == 1 && !app->SceneModified() && !app->RecordedFrameCountByte1(),
                "real same-value and invalid light text preserve plugin redo");
        }
        Menu(app, L"Edit probe redo");
        if (field == 5) Check(SaveSceneCopy(app, std::filesystem::absolute(L"light-text.pmm").c_str()) && history.UndoCount() == 3,
            "PMM saves committed light text and plugin state without clearing shared history");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 1 && pose() == source && coherent(), "plugin undo restores prior light authoring and CPU and GPU mirrors");
        Menu(app, L"Edit probe undo"); app->CameraMode() = 1; app->CurrentFrame() = frame + 23;
        Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
        Check(value() == 2 && pose() == target && coherent() && app->CurrentFrame() == frame + 23 &&
            model.boneTable[0].trans[1] == bone && model.undoState[0] == ring &&
            !std::memcmp(app->LightKeys(), keys.data(), keys.size() * sizeof(mdl::LightKey)),
            "light text shared redo preserves keys model pose and current playhead");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 0 && history.Reset() == MMS_OK, "light text probe restores plugin data and clears isolated case");
        app->CameraMode() = mode; app->CurrentFrame() = frame;
    }
    for (unsigned i = 0; i < 3; ++i) { app->LightColor()[i] = original[i]; app->LightDirection()[i] = original[i + 3]; }
    app->SceneLight() = originalLight; app->Renderer()->device->SetLight(0, &app->SceneLight());
}
