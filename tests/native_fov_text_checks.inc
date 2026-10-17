namespace {
struct FovTextFixture : PoseNumericFixture {
    FovTextFixture() {
        app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4); app->CameraFov() = 45.125f;
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES}; InitCommonControlsEx(&common);
        Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, 0, 0, 80, 20, app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(panel::kFovEdit)), GetModuleHandleW(nullptr), nullptr) != nullptr, "create hidden FOV edit");
        const auto slider = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD, 0, 0, 80, 20, app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(panel::kFovSlider)), GetModuleHandleW(nullptr), nullptr);
        Check(slider != nullptr, "create hidden FOV slider"); SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(1, 125));
    }
    ~FovTextFixture() { history.Reset(); history.Collect(); mdl::KeyPoolDestroy(app->CameraKeys()); }
    HWND Field() { return GetDlgItem(app->MainWindow(), panel::kFovEdit); }
    void Type(const char* text) { SetWindowTextA(Field(), text); SendMessageA(Field(), EM_SETMODIFY, TRUE, 0); }
    void Commit(const char* text) { Type(text); CommitEditControl(app.get(), Field()); }
    void Lose() { EditSubclassProc(Field(), WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); }
};
void FovTextHistoryChecks() {
    FovTextFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto pose = NumericCameraPose(*f.app); const auto ring = PoseRingDigest(f.first);
    f.Type("67.875"); f.Lose();
    Check(f.app->CameraFov() == 67 && f.history.UndoCount() == 1 && f.app->cameraProjectionRefreshPending &&
        SendDlgItemMessageW(f.app->MainWindow(), panel::kFovSlider, TBM_GETPOS, 0, 0) == 67,
        "FOV focus commit keeps integer legacy conversion and queues projection after atomic publication");
    f.Lose(); CommitEditControl(f.app.get(), f.Field());
    Check(f.app->CameraFov() == 67 && f.history.UndoCount() == 1 && NumericCameraPose(*f.app) == pose && PoseRingDigest(f.first) == ring,
        "unmodified FOV echo adds no history and preserves camera pose and old ring");
    f.app->CurrentFrame() = 31;
    Check(f.history.History(false) == MMS_OK && f.app->CameraFov() == 45.125f && f.app->CurrentFrame() == 31,
        "FOV undo restores precise prior float without seeking");
    for (const char* invalid : {"NaN", "inf", "", "3 junk", "1e999"}) {
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; f.Commit(invalid);
        Check(f.app->CameraFov() == 45.125f && f.history.RedoCount() == 1 && !f.app->SceneModified(), "invalid FOV input preserves authored value and redo");
    }
    Check(f.history.History(true) == MMS_OK && f.app->CameraFov() == 67, "FOV redo survives rejected inputs");
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; f.Commit("67.875");
    Check(f.history.UndoCount() == 1 && !f.app->SceneModified(), "same integer FOV input is no-op");
    f.Commit("200"); Check(f.app->CameraFov() == 200 && SendDlgItemMessageW(f.app->MainWindow(), panel::kFovSlider, TBM_GETPOS, 0, 0) == 125,
        "FOV keeps legacy authored value outside slider range while slider display clamps");
    auto* grown = mdl::KeyPoolCreate<mdl::CameraKey>(8); mdl::KeyPoolCommitGrowth(f.app->CameraKeys(), grown);
    Check(f.history.History(false) == MMS_OK && f.app->CameraFov() == 67, "FOV history survives identity-preserving camera pool growth");
    auto* replacement = mdl::KeyPoolCreate<mdl::CameraKey>(8); mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = replacement;
    Check(f.history.History(false) == MMS_E_STALE && f.app->CameraFov() == 67 && f.history.UndoCount() == 1,
        "FOV history rejects replaced scene identity without moving cursor");
}
void FovTextFailureChecks() {
    FovTextFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit("60"); Check(f.history.History(false) == MMS_OK, "seed FOV redo");
    f.app->cameraProjectionRefreshPending = false; f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    unsigned failures = 0; bool succeeded = false;
    for (long limit = 0; limit < 128; ++limit) {
        f.Type("67"); failAfter = limit; CommitEditControl(f.app.get(), f.Field()); failAfter = -1;
        if (f.history.UndoCount()) { succeeded = true; break; }
        ++failures;
        Check(f.app->CameraFov() == 45.125f && f.history.RedoCount() == 1 && !f.app->cameraProjectionRefreshPending &&
            !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "FOV preparation failure preserves authoring redo and projection request state");
        Check(f.history.Enter() == MMS_OK, "FOV failure releases journal gate"); f.history.Leave();
    }
    Check(succeeded && failures >= 5 && f.app->cameraProjectionRefreshPending, "FOV allocation sweep reaches successful atomic publication");
}
}
void NativeFovTextChecks() { FovTextHistoryChecks(); FovTextFailureChecks(); }
