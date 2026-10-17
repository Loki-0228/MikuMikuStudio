void CameraTextPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto pose = [&] { return std::array<float, 7>{app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraDistance()}; };
    const auto original = pose(); const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    auto& model = *mdl::Mdl(app->ModelSlot(0)); const auto modelPosition = model.boneTable[0].trans[1]; const auto ring = model.undoState[0];
    app->CameraMode() = 1;
    for (unsigned field = 0; field < 7; ++field) {
        const std::array<float, 7> source{4, 5, 6, .2f, -.3f, .4f, -40};
        app->CameraPositionX() = source[0]; app->CameraPositionY() = source[1]; app->CameraPositionZ() = source[2];
        app->CameraPitch() = source[3]; app->CameraYaw() = source[4]; app->CameraRoll() = source[5]; app->CameraDistance() = source[6];
        const auto edit = GetDlgItem(app->MainWindow(), 544 + int(field)); Check(edit != nullptr, "real camera text field exists");
        const auto commit = [&](const char* text) {
            SetWindowTextA(edit, text); SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
            SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0);
        };
        Menu(app, L"Edit probe prepared"); app->SceneModified() = 0; commit("17.123456789");
        Check(pose() == source && !history.UndoCount() && !app->SceneModified(), "pending real plugin transaction rejects camera text loss-of-focus edit");
        Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once"); commit("17.123456789");
        auto target = source; const double parsed = std::strtod("17.123456789", nullptr);
        target[field] = static_cast<float>(field < 3 ? parsed : field == 6 ? -parsed :
            (field == 3 ? -parsed : parsed) * 3.141592025756836 / 180.0);
        Check(pose() == target && value() == 1 && history.UndoCount() == 2 && !SendMessageA(edit, EM_GETMODIFY, 0, 0),
            "real seven-field camera text commits one step with exact legacy precision");
        SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0);
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        Check(pose() == target && history.UndoCount() == 2, "repeated real focus and Enter events do not write rounded camera echo twice");
        Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
        for (const char* text : {"17.123456789", "NaN", "3 junk"}) {
            app->SceneModified() = app->RecordedFrameCountByte1() = 0; commit(text);
            Check(pose() == target && history.RedoCount() == 1 && !app->SceneModified() && !app->RecordedFrameCountByte1(),
                "real same-value or invalid camera text retains later plugin redo and project flags");
        }
        Menu(app, L"Edit probe redo");
        if (field == 6) Check(SaveSceneCopy(app, std::filesystem::absolute(L"camera-text.pmm").c_str()) && history.UndoCount() == 3,
            "PMM saves committed camera text and plugin state without clearing shared history");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(pose() == source && value() == 1, "real plugin undo restores exact pose preceding camera text edit");
        Menu(app, L"Edit probe undo"); app->CameraMode() = 0; app->CurrentFrame() = frame + 19;
        Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
        Check(pose() == target && value() == 2 && app->CurrentFrame() == frame + 19 &&
            model.boneTable[0].trans[1] == modelPosition && model.undoState[0] == ring &&
            !std::memcmp(app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey)),
            "camera text shared redo works in model mode without key model or playhead changes");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 0 && history.Reset() == MMS_OK, "camera text probe restores plugin state and clears isolated case");
        app->CameraMode() = 1; app->CurrentFrame() = frame;
    }
    app->CameraPositionX() = original[0]; app->CameraPositionY() = original[1]; app->CameraPositionZ() = original[2];
    app->CameraPitch() = original[3]; app->CameraYaw() = original[4]; app->CameraRoll() = original[5]; app->CameraDistance() = original[6];
    app->CameraMode() = mode; app->CurrentFrame() = frame;
}
