namespace {
void CameraAxisCommand(MMDApp& app, unsigned field, bool floating) {
    const auto id = static_cast<unsigned short>(537 + field);
    if (floating) DispatchSeparateWindowCommand(&app, id);
    else CmdControl500(&app, app.MainWindow(), id, BN_CLICKED);
}
void CameraAxisRoutingChecks() {
    for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) {
        GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        const auto before = NumericCameraPose(*f.app); auto target = before; target[field] = 0;
        const auto keys = GlobalRegistrationKeys(f.app->CameraKeys()); const auto frame = f.app->CurrentFrame();
        CameraAxisCommand(*f.app, field, floating);
        Check(f.history.UndoCount() == 1 && NumericCameraPose(*f.app) == target &&
            GlobalRegistrationSame(f.app->CameraKeys(), keys) && f.app->CurrentFrame() == frame && f.app->SceneModified(),
            "main and floating camera axis and distance commands publish one common step without key or frame changes");
        Check(bool(f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)) == !(floating && field == 6),
            "successful camera field reset preserves original main and floating track activation differences");
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        const auto digest = f.Digest(); CameraAxisCommand(*f.app, field, floating);
        Check(f.Digest() == digest && f.history.UndoCount() == 1, "camera field already zero is a complete no-op");
        f.app->CurrentFrame() = 32;
        Check(f.history.History(false) == MMS_OK && NumericCameraPose(*f.app) == before && f.app->CurrentFrame() == 32,
            "camera field undo restores the exact original value at another playhead");
        // The reset field is zero again without using the journal. A no-op
        // must retain the redo branch rather than add an authored flag step.
        if (field < 3) f.app->CameraPositionAxis(field) = 0;
        else if (field < 6) f.app->CameraRotation()[field - 3] = 0;
        else f.app->CameraDistance() = 0;
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        const auto noop = f.Digest(); CameraAxisCommand(*f.app, field, floating);
        Check(f.Digest() == noop && f.history.RedoCount() == 1 && !f.history.UndoCount(),
            "zero camera component after undo preserves redo and selection");
        Check(f.history.History(true) == MMS_OK && NumericCameraPose(*f.app) == target,
            "camera component redo restores only its own field");
    }
}
void CameraAxisFailureChecks() {
    for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) {
        GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        CmdViewMenu(f.app.get(), nullptr, 302, 0); Check(f.history.History(false) == MMS_OK, "seed camera axis redo");
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        const auto digest = f.Digest(); unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 128; ++limit) {
            failAfter = limit; CameraAxisCommand(*f.app, field, floating); failAfter = -1;
            if (f.history.UndoCount()) { succeeded = true; break; }
            ++failures;
            Check(f.Digest() == digest && f.history.RedoCount() == 1,
                "camera axis allocation failure preserves all pose keys row selection dirty state and redo");
            Check(f.history.Enter() == MMS_OK, "failed camera axis route releases its journal reservation"); f.history.Leave();
        }
        Check(succeeded && failures >= 5 && !f.history.RedoCount(), "camera field preparation sweep reaches successful atomic publication");
    }
    GlobalRegistrationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto before = f.Digest();
    Check(plugins::TryResetBoundCameraField(f.app.get(), 7) && f.Digest() == before && !f.history.UndoCount(),
        "invalid camera component is consumed before old writes");
    for (int gate = 0; gate < 4; ++gate) {
        if (gate == 0) f.app->PlaybackActive() = 1;
        if (gate == 1) f.app->FrameStepPlayback() = 1;
        if (gate == 2) f.app->state.recordPlaybackStartPending = true;
        if (gate == 3) Check(f.history.Enter() == MMS_OK, "reserve journal before camera axis rejection");
        const auto digest = f.Digest();
        for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) CameraAxisCommand(*f.app, field, floating);
        Check(f.Digest() == digest && !f.history.UndoCount(), "camera field routes reject playback recording and busy state without fallthrough");
        if (gate == 3) f.history.Leave();
        f.app->PlaybackActive() = f.app->FrameStepPlayback() = 0; f.app->state.recordPlaybackStartPending = false;
    }
}
}
void NativeCameraAxisChecks() { CameraAxisRoutingChecks(); CameraAxisFailureChecks(); }
