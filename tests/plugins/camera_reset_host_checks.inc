// Included inside edit_history_host_checks. Uses actual command 302 and DLL.
void CameraRotationReset(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const std::array<float, 7> original{app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraDistance()};
    const auto parentModel = app->CameraParentModel(), parentBone = app->CameraParentBone();
    const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    const auto fov = app->CameraFov(); const auto perspective = app->CameraPerspective();
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    const auto modelPosition = model.boneTable[0].trans[1]; const auto ring = model.undoState[0];
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    const std::array<float, 3> source{.25f, -.75f, .5f};
    app->CameraMode() = 1; app->CameraParentModel() = 0; app->CameraParentBone() = 0;
    std::memcpy(app->CameraRotation(), source.data(), sizeof(source));
    const auto rotation = [&] { return std::array<float, 3>{app->CameraPitch(), app->CameraYaw(), app->CameraRoll()}; };
    const auto retained = [&] {
        return app->CameraPositionX() == original[0] && app->CameraPositionY() == original[1] &&
            app->CameraPositionZ() == original[2] && app->CameraDistance() == original[6] && app->CameraFov() == fov &&
            app->CameraPerspective() == perspective && app->CameraParentModel() == 0 && app->CameraParentBone() == 0 &&
            mdl::KeyPoolCapacity(app->CameraKeys()) == keys.size() &&
            !std::memcmp(app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey)) &&
            model.undoState[0] == ring && model.boneTable[0].trans[1] == modelPosition;
    };
    Menu(app, L"Edit probe prepared"); app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(value() == 0 && !history.UndoCount() && rotation() == source && retained() && !app->SceneModified(),
        "pending real plugin transaction refuses camera reset without legacy pose changes");
    Menu(app, L"Edit probe cancel");
    Menu(app, L"Edit probe once"); SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(value() == 1 && history.UndoCount() == 2 && rotation() == std::array<float, 3>{} && retained(),
        "actual camera rotation reset adds one shared step and preserves camera binding keys and other pose values");
    Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
    app->SceneModified() = app->RecordedFrameCountByte1() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(history.UndoCount() == 2 && history.RedoCount() == 1 && !app->SceneModified() && !app->RecordedFrameCountByte1(),
        "real zero camera reset preserves later plugin redo without dirtying the project");
    Menu(app, L"Edit probe redo");
    Check(SaveSceneCopy(app, std::filesystem::absolute(L"camera-reset.pmm").c_str()) && history.UndoCount() == 3 && retained(),
        "PMM saves committed camera reset with plugin state and keeps its common history");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 1 && rotation() == source && retained(), "real plugin undo restores exact previous camera rotation");
    SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
    app->CameraMode() = 0; app->CurrentFrame() = frame + 11;
    Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
    Check(value() == 2 && history.UndoCount() == 3 && rotation() == std::array<float, 3>{} && retained() && app->CurrentFrame() == frame + 11,
        "camera reset follows shared redo in model mode without seeking or changing model pose");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 0 && rotation() == source && retained() && history.Reset() == MMS_OK,
        "camera reset probe restores plugin state and clears isolated history");
    app->CameraPositionX() = original[0]; app->CameraPositionY() = original[1]; app->CameraPositionZ() = original[2];
    app->CameraPitch() = original[3]; app->CameraYaw() = original[4]; app->CameraRoll() = original[5]; app->CameraDistance() = original[6];
    app->CameraParentModel() = parentModel; app->CameraParentBone() = parentBone;
    app->CameraMode() = mode; app->CurrentFrame() = frame;
}
