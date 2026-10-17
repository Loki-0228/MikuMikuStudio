// Included inside edit_history_host_checks after Menu(). Drive real dialog
// controls on this test's thread, while preventing top-level visibility.
struct BoneNumericDialog {
    enum Action { Accept, Noop, Cancel, Invalid, ChangeFrame, PluginEdit, PluginPrepared, Recursive };
    MMDApp* app;
    Action action;
    HHOOK hide = nullptr, inspect = nullptr;
    unsigned seen = 0;
    static thread_local BoneNumericDialog* active;
    static LRESULT CALLBACK Hide(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
        if (message == WM_WINDOWPOSCHANGING) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lp);
            position->flags = (position->flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Hide, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK Creating(int code, WPARAM wp, LPARAM lp) {
        if (active && code == HCBT_CREATEWND) {
            auto* created = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
            if (!(created->style & WS_CHILD) && created->hwndParent == active->app->MainWindow()) {
                created->style &= ~WS_VISIBLE;
                SetWindowSubclass(reinterpret_cast<HWND>(wp), Hide, 91, 0);
            }
        }
        // No activation or user focus is needed to answer the real dialog.
        if (active && code == HCBT_ACTIVATE && GetWindow(reinterpret_cast<HWND>(wp), GW_OWNER) == active->app->MainWindow()) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK Initialized(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && active) {
            const auto* call = reinterpret_cast<CWPRETSTRUCT*>(lp);
            if (call->message == WM_INITDIALOG && GetWindow(call->hwnd, GW_OWNER) == active->app->MainWindow() &&
                GetDlgItem(call->hwnd, 637) && GetDlgItem(call->hwnd, 642) && !GetDlgItem(call->hwnd, 644)) {
                auto& driver = *active; ++driver.seen;
                Check(driver.seen == 1, "numeric entry refuses recursive modal dialogs");
                if (driver.action != Noop) {
                    const auto x = driver.action == Invalid ? L"NaN" :
                        driver.action == ChangeFrame || driver.action == PluginEdit || driver.action == PluginPrepared ? L"45.5" : L"12.5";
                    SetDlgItemTextW(call->hwnd, 637, x);
                    SetDlgItemTextW(call->hwnd, 642, L"90");
                }
                if (driver.action == ChangeFrame) ++driver.app->CurrentFrame();
                if (driver.action == PluginEdit) Menu(driver.app, L"Edit probe once");
                if (driver.action == PluginPrepared) Menu(driver.app, L"Edit probe prepared");
                if (driver.action == Recursive) {
                    // Even a nested camera-mode command must not overwrite
                    // the outer bone dialog's shared temp values.
                    driver.app->CameraMode() = 1;
                    SendMessageW(driver.app->MainWindow(), WM_COMMAND, 300, 0);
                    driver.app->CameraMode() = 0;
                }
                PostMessageW(call->hwnd, WM_COMMAND, driver.action == Cancel ? IDCANCEL : IDOK, 0);
                if (driver.action == Invalid) PostMessageW(call->hwnd, WM_COMMAND, IDCANCEL, 0);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    BoneNumericDialog(MMDApp* value, Action operation) : app(value), action(operation) {
        Check(!active, "no nested numeric test driver"); active = this;
        hide = SetWindowsHookExW(WH_CBT, Creating, nullptr, GetCurrentThreadId());
        inspect = SetWindowsHookExW(WH_CALLWNDPROCRET, Initialized, nullptr, GetCurrentThreadId());
        Check(hide && inspect, "install hidden real numeric dialog driver");
    }
    ~BoneNumericDialog() { UnhookWindowsHookEx(inspect); UnhookWindowsHookEx(hide); active = nullptr; }
    void Run() {
        SendMessageW(app->MainWindow(), WM_COMMAND, 300, 0);
        Check(seen == 1, "actual menu opens and finishes exactly one real bone numeric dialog");
    }
};
thread_local BoneNumericDialog* BoneNumericDialog::active = nullptr;
void NumericPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    auto& model = *mdl::Mdl(app->ModelSlot(0)); auto& bone = model.boneTable[0];
    std::array<float, 3> originalPosition{}; std::array<float, 4> originalRotation{};
    std::memcpy(originalPosition.data(), bone.trans, sizeof(originalPosition));
    std::memcpy(originalRotation.data(), bone.rotQuat, sizeof(originalRotation));
    const auto dirty = model.bonePhysicsState[0]; const auto ring = model.undoState[0];
    Menu(app, L"Edit probe prepared");
    { BoneNumericDialog driver(app, BoneNumericDialog::Noop);
      SendMessageW(app->MainWindow(), WM_COMMAND, 300, 0);
      Check(!driver.seen && !history.UndoCount() && model.undoState[0] == ring,
          "pending real plugin transaction rejects numeric entry before any modal dialog or old-ring write"); }
    Menu(app, L"Edit probe cancel");
    Menu(app, L"Edit probe once");
    { BoneNumericDialog driver(app, BoneNumericDialog::Accept); driver.Run(); }
    Check(value() == 1 && history.UndoCount() == 2 && bone.trans[0] == 12.5f &&
        bone.trans[1] == originalPosition[1] && model.undoState[0] == ring,
        "real numeric dialog records one native step after a plugin edit without legacy ring writes");
    Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
    for (auto action : {BoneNumericDialog::Noop, BoneNumericDialog::Cancel, BoneNumericDialog::Invalid}) {
        const auto before = bone; app->SceneModified() = app->RecordedFrameCountByte1() = 0;
        { BoneNumericDialog driver(app, action); driver.Run(); }
        Check(!std::memcmp(bone.trans, before.trans, sizeof(before.trans)) &&
            !std::memcmp(bone.rotQuat, before.rotQuat, sizeof(before.rotQuat)) &&
            history.UndoCount() == 2 && history.RedoCount() == 1 &&
            !app->SceneModified() && !app->RecordedFrameCountByte1(),
            "real no-op cancel and invalid numeric input preserve exact pose and plugin redo");
    }
    const auto frame = app->CurrentFrame();
    { BoneNumericDialog driver(app, BoneNumericDialog::ChangeFrame); driver.Run(); }
    Check(history.UndoCount() == 2 && history.RedoCount() == 1 && app->CurrentFrame() == frame + 1 && bone.trans[0] == 12.5f,
        "real modal frame change rejects old numeric input without moving the new playhead");
    app->CurrentFrame() = frame;
    { BoneNumericDialog driver(app, BoneNumericDialog::Recursive); driver.Run(); }
    Check(history.UndoCount() == 2 && history.RedoCount() == 1,
        "recursive camera numeric request cannot corrupt outer bone input or create history");
    { BoneNumericDialog driver(app, BoneNumericDialog::PluginPrepared); driver.Run(); }
    Check(value() == 1 && history.UndoCount() == 2 && history.RedoCount() == 1 && bone.trans[0] == 12.5f,
        "plugin transaction opened during numeric input blocks apply without native writes or redo loss");
    Menu(app, L"Edit probe cancel");
    { BoneNumericDialog driver(app, BoneNumericDialog::PluginEdit); driver.Run(); }
    Check(value() == 2 && history.UndoCount() == 3 && bone.trans[0] == 12.5f,
        "plugin commit during modal input retains its own edit and rejects the obsolete native session");
    Check(SaveSceneCopy(app, std::filesystem::absolute(L"numeric-pose.pmm").c_str()) && history.UndoCount() == 3,
        "real numeric pose and plugin state save without discarding shared history");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 1 && !std::memcmp(bone.trans, originalPosition.data(), sizeof(originalPosition)) &&
        !std::memcmp(bone.rotQuat, originalRotation.data(), sizeof(originalRotation)) && model.bonePhysicsState[0] == dirty,
        "plugin undo restores exact pose before real numeric dialog");
    Menu(app, L"Edit probe undo");
    app->CameraMode() = 1;
    Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0); Menu(app, L"Edit probe redo");
    Check(value() == 2 && bone.trans[0] == 12.5f && history.UndoCount() == 3 && model.undoState[0] == ring,
        "mixed native and plugin redo restores numeric pose even in camera mode");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 0 && history.Reset() == MMS_OK, "numeric dialog probe restores setup and clears its isolated journal");
    app->CameraMode() = 0;
}
