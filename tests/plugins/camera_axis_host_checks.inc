void CameraAxisPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto pose = [&] { return std::array<float, 7>{app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->CameraDistance()}; };
    const auto original = pose(); const auto mode = app->CameraMode(); const auto frame = app->CurrentFrame();
    std::array<unsigned char, 4> selection{}; std::memcpy(selection.data(), app->state.globalTrackSelected, sizeof(selection));
    const auto accessoryRow = app->AccessorySlot(0)->rowSelected;
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    auto& model = *mdl::Mdl(app->ModelSlot(0)); const auto modelPosition = model.boneTable[0].trans[1]; const auto ring = model.undoState[0];
    app->CameraMode() = 1;
    for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) {
        const std::array<float, 7> source{4, 5, 6, .2f, -.3f, .4f, -40};
        app->CameraPositionX() = source[0]; app->CameraPositionY() = source[1]; app->CameraPositionZ() = source[2];
        app->CameraPitch() = source[3]; app->CameraYaw() = source[4]; app->CameraRoll() = source[5]; app->CameraDistance() = source[6];
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light); app->AccessorySlot(0)->rowSelected = 1;
        const auto dispatch = [&] {
            if (floating) DispatchSeparateWindowCommand(app, static_cast<unsigned short>(537 + field));
            else SendMessageW(app->MainWindow(), WM_COMMAND, 537 + field, 0);
        };
        Menu(app, L"Edit probe prepared"); app->SceneModified() = 0; dispatch();
        Check(pose() == source && !history.UndoCount() && !app->SceneModified() &&
            !app->GlobalTrackSelected(GlobalTimelineTrack::Camera) && app->AccessorySlot(0)->rowSelected,
            "pending real plugin transaction blocks both camera field routes without authoring or selection changes");
        Menu(app, L"Edit probe cancel"); Menu(app, L"Edit probe once"); dispatch();
        auto target = source; target[field] = 0;
        Check(pose() == target && value() == 1 && history.UndoCount() == 2 &&
            bool(app->GlobalTrackSelected(GlobalTimelineTrack::Camera)) == !(floating && field == 6),
            "production main and floating dispatchers record each camera component and retain track activation rules");
        Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
        app->SceneModified() = app->RecordedFrameCountByte1() = 0; dispatch();
        Check(pose() == target && history.RedoCount() == 1 && !app->SceneModified() && !app->RecordedFrameCountByte1(),
            "already-zero production camera field preserves later plugin redo and dirty state");
        Menu(app, L"Edit probe redo");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 1 && pose() == source, "real plugin undo restores the exact camera field value");
        Menu(app, L"Edit probe undo"); app->CameraMode() = 0; app->CurrentFrame() = frame + 17;
        Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
        Check(pose() == target && value() == 2 && app->CurrentFrame() == frame + 17 &&
            model.boneTable[0].trans[1] == modelPosition && model.undoState[0] == ring &&
            !std::memcmp(app->CameraKeys(), keys.data(), keys.size() * sizeof(mdl::CameraKey)),
            "camera field common redo works in model mode without seeking or changing keys and model pose");
        if (field == 6 && floating)
            Check(SaveSceneCopy(app, std::filesystem::absolute(L"camera-axis.pmm").c_str()) && history.UndoCount() == 3,
                "PMM saves committed camera field and plugin edits without clearing history");
        Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
        Check(value() == 0 && history.Reset() == MMS_OK, "camera field probe restores plugin state and clears its isolated case");
        app->CameraMode() = 1; app->CurrentFrame() = frame;
    }
    app->CameraPositionX() = original[0]; app->CameraPositionY() = original[1]; app->CameraPositionZ() = original[2];
    app->CameraPitch() = original[3]; app->CameraYaw() = original[4]; app->CameraRoll() = original[5]; app->CameraDistance() = original[6];
    app->CameraMode() = mode; app->CurrentFrame() = frame;
    std::memcpy(app->state.globalTrackSelected, selection.data(), sizeof(selection)); app->AccessorySlot(0)->rowSelected = accessoryRow;
}
