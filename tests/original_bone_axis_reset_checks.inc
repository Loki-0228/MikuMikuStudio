// MAINTENANCE: focused mmd932 regression for docked and detached axis resets.
namespace {
struct OriginalBoneAxisFixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    unsigned char selection = 1, edited = 0;
    MMDApp* previous = g_Block;
    HWND owner = nullptr;

    explicit OriginalBoneAxisFixture(bool floating) {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->SelectedModel() = reinterpret_cast<unsigned char*>(model.get());
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        model->boneCount = 1;
        model->selectedBone = 0;
        model->boneTable = &bone;
        model->boneSelection = &selection;
        model->bonePhysicsState = &edited;
        bone.parent = -1;
        bone.rotQuat[3] = 1.0f;

        app->MainWindow() = MakeOwner();
        owner = app->MainWindow();
        if (floating) app->FloatingWindow() = owner = MakeOwner();
        g_Block = app.get();
        for (int id = panel::kReadoutPosXEdit; id <= panel::kReadoutRotZEdit; ++id) {
            const HWND field = CreateWindowExA(0, "EDIT", "", WS_CHILD, 0, 0, 80, 20,
                owner, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Require(field != nullptr, "create a real hidden native readout");
            app->OriginalEditProc() = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(field, GWLP_WNDPROC));
            InstallControlSubclass(app.get(), field, id);
        }
    }
    ~OriginalBoneAxisFixture() {
        if (app->FloatingWindow()) DestroyWindow(app->FloatingWindow());
        DestroyWindow(app->MainWindow());
        for (auto& ring : model->undoRings)
            for (auto& slot : ring.slots)
                undo_storage::Delete(slot.bonePose);
        g_Block = previous;
    }
    static HWND MakeOwner() {
        const HWND window = CreateWindowExA(0, "STATIC", "mmd932 axis reset test", WS_OVERLAPPED,
            0, 0, 240, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "create a test-owned hidden panel");
        return window;
    }
    void Reset(bool rotation, int axis) {
        const auto id = static_cast<unsigned short>((rotation ? 0x21C : 0x219) + axis);
        if (app->FloatingWindow()) DispatchSeparateWindowCommand(app.get(), id);
        else CmdControl500(app.get(), owner, id, BN_CLICKED);
    }
    bool ResetPose(bool rotation, int axis) const {
        const float position[3] = {axis == 0 && !rotation ? 0.f : 2.f,
                                   axis == 1 && !rotation ? 0.f : 3.f,
                                   axis == 2 && !rotation ? 0.f : 4.f};
        return !std::memcmp(bone.trans, position, sizeof position) &&
            bone.rotQuat[0] == 0 && bone.rotQuat[1] == 0 &&
            bone.rotQuat[2] == 0 && bone.rotQuat[3] == 1;
    }
};

int OriginalBoneAxisResetChecks() {
    wchar_t desktop[128]{};
    DWORD desktopBytes = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
        desktop, sizeof desktop, &desktopBytes) && !_wcsicmp(desktop, L"Default"),
        "axis reset fixture runs on the inherited Default desktop");
    Require(d3dx::Get().Load(), "load real D3DX math for the original axis reset");
    for (bool floating : {false, true}) for (bool rotation : {false, true}) for (int axis = 0; axis < 3; ++axis) {
        OriginalBoneAxisFixture f(floating);
        std::printf("AXIS floating=%d rotation=%d axis=%d\n", floating, rotation, axis);
        f.bone.trans[0] = 2; f.bone.trans[1] = 3; f.bone.trans[2] = 4;
        if (rotation) {
            f.bone.rotQuat[axis] = 0.6f;
            f.bone.rotQuat[3] = 0.8f;
        }
        PostViewRefresh(f.app.get());
        const HWND field = GetDlgItem(f.owner, (rotation ? panel::kReadoutRotXEdit : panel::kReadoutPosXEdit) + axis);
        SetFocus(field);
        Require(GetFocus() == field, "target readout has real keyboard focus");
        Require(SetWindowTextA(field, "99.25") != FALSE, "write stale text to the actual edit");
        SendMessageA(field, EM_SETMODIFY, TRUE, 0);
        Require(SendMessageA(field, EM_GETMODIFY, 0, 0) != 0, "test draft has its native modify flag set");

        const auto revision = f.app->SceneRevision();
        f.Reset(rotation, axis);
        Check(f.ResetPose(rotation, axis) && f.edited && f.model->undoState[0] == 1,
            "selected axis is reset and original physics edit flag and undo record are published");
        Check(f.app->SceneModified() && f.app->SceneRevision() == revision + 1,
            "selected bone reset marks the scene modified exactly once");
        Check(GetFocus() == field && !SendMessageA(field, EM_GETMODIFY, 0, 0) &&
            Caption(field) == (rotation ? L"0.0" : L"0.00"),
            "reset replaces the pending draft while its readout still owns focus");
        SetFocus(f.owner);
        Check(GetFocus() == f.owner && f.ResetPose(rotation, axis),
            "real focus loss cannot reapply the stale position or rotation draft");

        f.app->SceneModified() = 0;
        f.Reset(rotation, axis);
        Check(f.app->SceneModified() && f.ResetPose(rotation, axis),
            "repeated selected-bone reset retains main's original dirty behavior");

        f.app->SceneModified() = 0;
        const auto cleanRevision = f.app->SceneRevision();
        const auto cursor = f.model->undoState[0];
        f.model->selectedBone = -1;
        f.Reset(rotation, axis);
        Check(!f.app->SceneModified() && f.app->SceneRevision() == cleanRevision &&
            f.model->undoState[0] == cursor && f.ResetPose(rotation, axis),
            "a missing selection changes neither the scene nor the model undo cursor");
        f.app->CameraMode() = 1;
        float& camera = rotation ? f.app->CameraRotation()[axis] : f.app->CameraPositionAxis(axis);
        camera = 1.f;
        f.Reset(rotation, axis);
        Check(camera == 0 && !f.app->SceneModified() && f.app->SceneRevision() == cleanRevision &&
            f.model->undoState[0] == cursor,
            "camera reset keeps original view-only behavior");
        AuditHidden("bone axis reset");
    }
    std::printf("RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
}
