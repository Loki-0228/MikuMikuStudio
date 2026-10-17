void CameraNumericPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto pose = [&] { return std::array<float, 7>{app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraDistance()}; };
    const auto original = pose(); const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    const auto parent = app->CameraParentModel(), bone = app->CameraParentBone();
    const auto fov = app->CameraFov(); const auto perspective = app->CameraPerspective();
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    auto& model = *mdl::Mdl(app->ModelSlot(0)); const auto ring = model.undoState[0]; const auto modelPosition = model.boneTable[0].trans[1];
    app->CameraMode() = 1; app->CameraParentModel() = 0; app->CameraParentBone() = 0;
    const auto retained = [&] { return app->CameraParentModel() == 0 && app->CameraParentBone() == 0 &&
        app->CameraFov() == fov && app->CameraPerspective() == perspective && model.undoState[0] == ring &&
        model.boneTable[0].trans[1] == modelPosition && mdl::KeyPoolCapacity(app->CameraKeys()) == keys.size() &&
        !std::memcmp(app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey)); };
    Menu(app, L"Edit probe prepared");
    { BoneNumericDialog driver(app, BoneNumericDialog::Noop); SendMessageW(app->MainWindow(), WM_COMMAND, 300, 0);
      Check(!driver.seen && !history.UndoCount() && pose() == original, "pending plugin edit prevents camera numeric modal entry"); }
    Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once");
    { BoneNumericDialog driver(app, BoneNumericDialog::Accept); driver.Run(); }
    Check(value() == 1 && history.UndoCount() == 2 && app->CameraPositionX() == 12.5f && app->CameraDistance() == -60 &&
        app->CameraPitch() == original[3] && app->CameraYaw() == original[4] && retained(),
        "real camera numeric dialog edits seven-field pose as one shared step and retains keys binding and untouched radians");
    const auto target = pose(); Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
    for (auto action : {BoneNumericDialog::Noop, BoneNumericDialog::Cancel, BoneNumericDialog::Invalid, BoneNumericDialog::Recursive}) {
        app->SceneModified() = app->RecordedFrameCountByte1() = 0;
        { BoneNumericDialog driver(app, action); driver.Run(); }
        Check(pose() == target && history.UndoCount() == 2 && history.RedoCount() == 1 &&
            !app->SceneModified() && !app->RecordedFrameCountByte1(), "camera no-op cancel invalid and recursive input preserve exact pose and plugin redo");
    }
    { BoneNumericDialog driver(app, BoneNumericDialog::ChangeFrame); driver.Run(); }
    Check(pose() == target && history.RedoCount() == 1 && app->CurrentFrame() == frame + 1,
        "camera numeric modal rejects changed playhead without applying stale coordinates"); app->CurrentFrame() = frame;
    { BoneNumericDialog driver(app, BoneNumericDialog::PluginPrepared); driver.Run(); }
    Check(pose() == target && history.RedoCount() == 1 && value() == 1, "plugin transaction opened during camera input blocks native apply");
    Menu(app, L"Edit probe cancel");
    { BoneNumericDialog driver(app, BoneNumericDialog::PluginEdit); driver.Run(); }
    Check(pose() == target && value() == 2 && history.UndoCount() == 3 && retained(),
        "camera input becomes stale after a real plugin commit and preserves that plugin edit");
    Check(SaveSceneCopy(app, std::filesystem::absolute(L"camera-numeric.pmm").c_str()) && history.UndoCount() == 3,
        "PMM saves committed camera numeric edit and plugin state without clearing common history");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(pose() == original && value() == 1 && retained(), "real plugin undo restores exact pose preceding camera numeric dialog");
    Menu(app, L"Edit probe undo"); app->CameraMode() = 0; app->CurrentFrame() = frame + 13;
    Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
    Check(pose() == target && value() == 2 && retained() && app->CurrentFrame() == frame + 13,
        "camera numeric shared redo works in model mode at a different frame without touching model authoring");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 0 && history.Reset() == MMS_OK && pose() == original, "camera numeric probe restores setup and clears isolated history");
    app->CameraMode() = mode; app->CurrentFrame() = frame; app->CameraParentModel() = parent; app->CameraParentBone() = bone;
}
