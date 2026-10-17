namespace {
struct CameraTextFixture : PoseNumericFixture {
    HWND base = nullptr, floating = nullptr;
    explicit CameraTextFixture(bool separate) {
        app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4);
        app->CameraMode() = 1; app->CameraParentModel() = -1;
        for (unsigned i = 0; i < 3; ++i) { app->CameraPositionAxis(i) = float(i + 4); app->CameraRotation()[i] = .2f + float(i) * .1f; }
        app->CameraDistance() = -40;
        base = app->MainWindow();
        if (separate) {
            floating = CreateWindowExA(0, "STATIC", "camera text", WS_OVERLAPPED, 0, 0, 360, 120,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(floating != nullptr, "create hidden floating camera text fixture"); base = floating; app->FloatingWindow() = floating;
        }
        for (int field = 0; field < 7; ++field) if (!GetDlgItem(base, 544 + field))
            Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, 0, 0, 80, 20, base,
                reinterpret_cast<HMENU>(INT_PTR(544 + field)), GetModuleHandleW(nullptr), nullptr) != nullptr, "create camera text field");
    }
    ~CameraTextFixture() {
        history.Reset(); history.Collect(); mdl::KeyPoolDestroy(app->CameraKeys());
        app->FloatingWindow() = nullptr; if (floating) DestroyWindow(floating);
    }
    HWND Field(unsigned field) { return GetDlgItem(base, 544 + int(field)); }
    void TypeField(unsigned field, const char* text) { SetWindowTextA(Field(field), text); SendMessageA(Field(field), EM_SETMODIFY, TRUE, 0); }
    void CommitField(unsigned field, const char* text) { TypeField(field, text); CommitEditControlTail(app.get(), Field(field)); }
    void LoseField(unsigned field) { EditSubclassProc(Field(field), WM_KILLFOCUS, reinterpret_cast<WPARAM>(base), 0); }
};
float CameraTextTarget(unsigned field, double value) {
    return static_cast<float>(field < 3 ? value : field == 6 ? -value : (field == 3 ? -value : value) * 3.141592025756836 / 180.0);
}
void CameraTextRoutingChecks() {
    for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) {
        CameraTextFixture f(floating); plugins::NativePoseTestBinding binding(*f.app, f.history);
        const auto before = NumericCameraPose(*f.app); auto after = before;
        after[field] = CameraTextTarget(field, std::strtod("13.37123456789", nullptr));
        const auto ring = PoseRingDigest(f.first);
        f.TypeField(field, "13.37123456789"); f.LoseField(field);
        Check(NumericCameraPose(*f.app) == after && f.history.UndoCount() == 1 &&
            !SendMessageA(f.Field(field), EM_GETMODIFY, 0, 0) && f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera),
            "main and floating camera text loss of focus commits exact legacy double conversion once");
        f.LoseField(field); CommitEditControlTail(f.app.get(), f.Field(field));
        Check(NumericCameraPose(*f.app) == after && f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring,
            "rounded unmodified camera echo cannot overwrite precision or add history");
        f.app->CurrentFrame() = 41;
        Check(f.history.History(false) == MMS_OK && NumericCameraPose(*f.app) == before && f.app->CurrentFrame() == 41,
            "camera text undo restores only its authored field without seeking");
        for (const char* invalid : {"NaN", "inf", "", "3 trailing", "1e999"}) {
            f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
            f.CommitField(field, invalid);
            Check(NumericCameraPose(*f.app) == before && f.history.RedoCount() == 1 &&
                !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "invalid camera text preserves original pose and redo");
        }
        Check(f.history.History(true) == MMS_OK && NumericCameraPose(*f.app) == after, "camera text redo remains valid after invalid input");
    }
}
void CameraTextFailureChecks() {
    for (bool floating : {false, true}) for (unsigned field = 0; field < 7; ++field) {
        CameraTextFixture f(floating); plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.CommitField(field, "10"); Check(f.history.History(false) == MMS_OK, "seed camera text redo branch");
        const auto before = NumericCameraPose(*f.app); const auto ring = PoseRingDigest(f.first);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 128; ++limit) {
            f.TypeField(field, "17.123456"); failAfter = limit; CommitEditControlTail(f.app.get(), f.Field(field)); failAfter = -1;
            if (f.history.UndoCount()) { succeeded = true; break; }
            ++failures;
            Check(NumericCameraPose(*f.app) == before && f.history.RedoCount() == 1 && PoseRingDigest(f.first) == ring &&
                !f.app->SceneModified() && !f.app->RecordedFrameCountByte1(), "camera text preparation failures preserve exact pose redo and native ring");
            Check(f.history.Enter() == MMS_OK, "camera text failure releases history gate"); f.history.Leave();
        }
        Check(succeeded && failures >= 5, "camera text allocation sweep reaches successful commit after rejected preparations");
    }
}
}
void NativeCameraTextChecks() { CameraTextRoutingChecks(); CameraTextFailureChecks(); }
