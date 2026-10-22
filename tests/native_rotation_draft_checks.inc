// Actual bound rotation commands, native EDIT focus loss and guarded readout.
// Include after PositionDraftPanel and the physics readout checks.
#include "mikumikustudio/bone_rotation_readout.hpp"
namespace mikumikustudio { void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t); }
namespace {
// Independent copy of the pre-extraction PostViewRefresh calculation. Do not
// replace this oracle with the new production helper.
std::array<float, 3> RotationDraftOldDegrees(const d3dx::D3DXMATRIXF& m) {
    float rz = float(std::atan2(double(m.m[0][1]), double(m.m[1][1])));
    float rx = float(std::asin((std::max)(-1., (std::min)(1., -double(m.m[2][1])))));
    float ry = float(std::atan2(double(m.m[2][0]), double(m.m[2][2])));
    const float c = float(std::cos(double(rx)));
    if (std::fabs(double(c)) < 1e-6f) { rz = 0; ry = std::atan2(-m.m[0][2], m.m[0][0]); }
    if (std::fabs(double(rx)) < 1e-6f) rx = 0;
    if (std::fabs(double(ry)) < 1e-6f) ry = 0;
    if (std::fabs(double(rz)) < 1e-6f) rz = 0;
    return {float(double(rx) / 3.141592f * 180.f),
        float(-double(ry) / 3.141592f * 180.f), float(180.f * (-double(rz) / 3.141592f))};
}
std::array<float, 3> RotationDraftQuaternionDegrees(const float* quaternion) {
    d3dx::D3DXMATRIXF matrix{};
    d3dx::Get().matrixRotationQuaternion(&matrix, quaternion);
    return RotationDraftOldDegrees(matrix);
}
void RotationDraftHelperChecks() {
    Check(d3dx::Get().Load(), "rotation draft regression requires real D3DX math");
    std::vector<d3dx::D3DXMATRIXF> samples(2);
    for (int i = 0; i < 4; ++i) samples[1].m[i][i] = 1;
    for (float angle : {.00000001f, .25f, -.75f, 1.5707963f, -1.5707963f, 1.570790f, -1.570790f}) {
        d3dx::D3DXMATRIXF x{}, y{}, z{}, value{};
        d3dx::Get().rotX(&x, angle); d3dx::Get().rotY(&y, .3f); d3dx::Get().rotZ(&z, -.2f);
        d3dx::Get().multiply(&value, &z, &x); d3dx::Get().multiply(&value, &value, &y);
        samples.push_back(x); samples.push_back(value);
    }
    for (float outside : {-1.0001f, 1.0001f}) { auto m = samples[1]; m.m[2][1] = outside; samples.push_back(m); }
    for (const auto& matrix : samples) {
        const auto expected = RotationDraftOldDegrees(matrix);
        const auto actual = ui::BoneRotationDegreesFromMatrix(matrix);
        Check(std::isfinite(actual[0]) && std::isfinite(actual[1]) && std::isfinite(actual[2]) &&
            !std::memcmp(actual.data(), expected.data(), sizeof actual),
            "extracted readout preserves old float results for zero identity mixed small pole and clamped matrices");
    }
}
struct RotationDraftPanel {
    PositionDraftPanel base;
    static constexpr int scales[3] = {panel::kReadoutScaleXEdit, panel::kReadoutScaleYEdit, panel::kReadoutScaleZEdit};
    RotationDraftPanel(PoseNumericFixture& f, bool floating) : base(f, floating) {
        PhysicsRotationFields(base);
        for (int axis = 0; axis < 3; ++axis) {
            InstallControlSubclass(f.app.get(), Field(axis), panel::kReadoutRotXEdit + axis);
            if (floating) Check(CreateWindowExA(0, "EDIT", "1", WS_CHILD, axis * 90, 48, 80, 20, base.owner,
                reinterpret_cast<HMENU>(INT_PTR(scales[axis])), GetModuleHandleW(nullptr), nullptr) != nullptr,
                "create real separate scale field for unrelated draft preservation");
        }
    }
    ~RotationDraftPanel() { Clear(); }
    HWND Field(int axis) const { return GetDlgItem(base.owner, panel::kReadoutRotXEdit + axis); }
    static bool Modified(HWND field) { return SendMessageA(field, EM_GETMODIFY, 0, 0) != 0; }
    static bool Text(HWND field, const char* expected) {
        char value[96]{}; GetWindowTextA(field, value, sizeof value); return !std::strcmp(value, expected);
    }
    static void Pending(HWND field, const char* text) { SetWindowTextA(field, text); SendMessageA(field, EM_SETMODIFY, TRUE, 0); }
    void Draft(int axis, const char* text) {
        SetFocus(Field(axis)); Check(GetFocus() == Field(axis), "rotation draft has actual native keyboard focus");
        Pending(Field(axis), text);
    }
    void LoseFocus(int axis) {
        SetFocus(base.owner);
        Check(GetFocus() == base.owner && GetFocus() != Field(axis), "rotation draft actually loses native focus to its owned panel");
    }
    void Clear() {
        for (int axis = 0; axis < 3; ++axis) for (HWND field : {Field(axis), base.Field(axis), GetDlgItem(base.owner, scales[axis])})
            SendMessageA(field, EM_SETMODIFY, FALSE, 0);
    }
    void Unrelated() {
        Pending(base.Field(0), "position pending"); Pending(GetDlgItem(base.owner, scales[0]), "scale pending");
    }
    bool UnrelatedPreserved() const {
        return Text(base.Field(0), "position pending") && Modified(base.Field(0)) &&
            Text(GetDlgItem(base.owner, scales[0]), "scale pending") && Modified(GetDlgItem(base.owner, scales[0]));
    }
    void Command(bool menu, int axis = 0) {
        if (menu) CmdViewMenu(base.fixture.app.get(), base.fixture.app->MainWindow(), 302, 0);
        else PoseAxisCommand(base.fixture, AxisKind::Rotation, axis, base.floating);
    }
};
void RotationDraftSeed(PoseNumericFixture& f) {
    PoseAxisDegrees(*f.app); f.app->CurrentFrame() = 37;
    f.first.bones[0].rotQuat[2] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    Check(bone_scale::SetPose(f.first.raw(), 0, Scale(2)) && !bone_scale::TransformsApplied(f.first.raw()),
        "unapplied nonidentity scale keeps ordinary full pose readout guarded during this regression");
}
void RotationDraftCaption(RotationDraftPanel& panel, int axis, float expected) {
    if (std::fabs(double(expected)) < 1e-7f) expected = 0;
    char text[64]{}; sprintf_s(text, sizeof text, "%1.1f", double(expected));
    Check(RotationDraftPanel::Text(panel.Field(axis), text) && !RotationDraftPanel::Modified(panel.Field(axis)),
        "successful reset replaces the requested rotation draft and clears its commit authorization");
}
std::array<float, 4> RotationDraftNumericOracle(std::array<float, 3> degrees, int axis, float value) {
    degrees[axis] = value; constexpr double pi = 3.141592025756836;
    d3dx::D3DXMATRIXF target{}, temporary{}; std::array<float, 4> quaternion{};
    auto& api = d3dx::Get();
    api.rotZ(&target, float(-degrees[2] * pi / 180.)); api.rotX(&temporary, float(degrees[0] * pi / 180.));
    api.multiply(&target, &target, &temporary); api.rotY(&temporary, float(-degrees[1] * pi / 180.));
    api.multiply(&target, &target, &temporary); api.quatFromMatrix(quaternion.data(), &target); return quaternion;
}
void RotationDraftSuccessChecks(bool menu) {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool floating : {false, true})
        for (int axis = 0; axis < (menu ? 1 : 3); ++axis) {
            PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); RotationDraftPanel panel(f, floating);
            Check(f.history.SetMode(mode) == MMS_OK, "configure real rotation draft recording policy"); RotationDraftSeed(f);
            const std::vector<mdl::BoneKey> keys(f.first.m->boneKeys, f.first.m->boneKeys + mdl::BoneKeyCapacity(f.first.m->boneKeys));
            const auto ring = PoseRingDigest(f.first), second = f.second.Digest();
            const auto original = f.first.bones[0]; const auto other = f.first.bones[1];
            for (int i = 0; i < 3; ++i) RotationDraftPanel::Pending(panel.Field(i), "rotation pending");
            panel.Unrelated(); panel.Draft(axis, "91.25"); panel.Command(menu, axis);
            const auto expected = RotationDraftQuaternionDegrees(f.first.bones[0].rotQuat);
            const auto actual = PoseAxisReadDegrees(*f.app);
            Check(actual == expected,
                "actual command synchronizes all derived rotation angles even when ordinary PostViewRefresh is guarded");
            for (int i = 0; i < 3; ++i) {
                if (menu || i == axis) RotationDraftCaption(panel, i, expected[i]);
                else Check(RotationDraftPanel::Text(panel.Field(i), "rotation pending") && RotationDraftPanel::Modified(panel.Field(i)),
                    "axis reset preserves the other uncommitted rotation drafts");
            }
            const auto count = mode == Mode::AllOperations ? 1u : 0u;
            Check(panel.UnrelatedPreserved() && f.history.UndoCount() == count && f.first.dirty[0] == 1 &&
                !bone_scale::TransformsApplied(f.first.raw()) && bone_scale::Pose(f.first.raw(), 0) == Scale(2) &&
                !std::memcmp(original.trans, f.first.bones[0].trans, sizeof original.trans) &&
                !std::memcmp(&other, &f.first.bones[1], sizeof other) && f.second.Digest() == second &&
                PoseRingDigest(f.first) == ring && f.app->CurrentFrame() == 37 && f.SelectionUnchanged() &&
                mdl::BoneKeyCapacity(f.first.m->boneKeys) == keys.size() &&
                !std::memcmp(keys.data(), f.first.m->boneKeys, keys.size() * sizeof(mdl::BoneKey)),
                "rotation draft completion preserves unrelated drafts authoring keys scale frame selection and legacy history");
            const auto after = f.first.Digest(); std::uint64_t revision = 0, later = 0; f.history.Revision(revision);
            panel.LoseFocus(axis); f.history.Revision(later);
            Check(after == f.first.Digest() && later == revision && f.history.UndoCount() == count,
                "real focus loss cannot commit the discarded rotation draft");
            if (!menu) {
                const int next = (axis + 1) % 3; const auto wanted = RotationDraftNumericOracle(expected, next, 12.5f);
                panel.Draft(next, "12.5"); panel.LoseFocus(next);
                Check(!std::memcmp(wanted.data(), f.first.bones[0].rotQuat, sizeof wanted),
                    "editing another real rotation field uses synchronized angles and cannot restore the reset axis from stale cache");
            }
            panel.Clear();
            if (mode == Mode::AllOperations) {
                if (!menu) Check(f.history.History(false) == MMS_OK, "undo subsequent rotation edit before reset undo");
                Check(f.history.History(false) == MMS_OK && !std::memcmp(original.rotQuat, f.first.bones[0].rotQuat, sizeof original.rotQuat),
                    "rotation draft publication retains exact common undo");
                Check(f.history.History(true) == MMS_OK, "rotation draft publication retains common redo");
            }
        }
}
void RotationDraftNoopChecks(bool menu) {
    for (bool floating : {false, true}) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); RotationDraftPanel panel(f, floating);
        f.first.bones[0].trans[0] = 4; PoseAxisCommand(f, AxisKind::Position, 0, floating);
        Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "retain an actual redo branch before rotation no-op");
        RotationDraftSeed(f); if (menu) { f.first.bones[0].rotQuat[2] = 0; f.first.bones[0].rotQuat[3] = 1; }
        else f.app->BoneRotationEditDegreesX() = 0;
        const auto cached = PoseAxisReadDegrees(*f.app); const auto before = f.first.Digest();
        for (int i = 0; i < 3; ++i) RotationDraftPanel::Pending(panel.Field(i), "rotation pending");
        panel.Unrelated(); panel.Draft(0, "77.5");
        std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; panel.Command(menu); f.history.Revision(after);
        const auto degrees = PoseAxisReadDegrees(*f.app);
        Check(f.first.Digest() == before && revision == after && f.history.RedoCount() == 1 && !f.history.UndoCount() &&
            !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && panel.UnrelatedPreserved(),
            "rotation no-op cancels drafts without changing authored pose dirty flags revision or redo");
        for (int i = 0; i < 3; ++i) {
            if (menu || !i) { RotationDraftCaption(panel, i, 0); Check(degrees[i] == 0, "requested no-op angle cache is zero"); }
            else Check(degrees[i] == cached[i] && RotationDraftPanel::Text(panel.Field(i), "rotation pending") && RotationDraftPanel::Modified(panel.Field(i)),
                "zero-axis no-op leaves other cached angles and drafts untouched");
        }
        panel.LoseFocus(0); Check(f.first.Digest() == before && f.history.RedoCount() == 1, "no-op focus loss retains pose and redo");
    }
}
void RotationDraftFailureChecks(bool menu) {
    for (bool floating : {false, true}) for (unsigned reason = 0; reason < 4; ++reason) for (bool noop : {false, true}) {
        if (noop && reason < 2) continue; // Allocation/budget-free no-ops are intentionally admitted.
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); RotationDraftPanel panel(f, floating);
        f.first.bones[0].trans[0] = 4; PoseAxisCommand(f, AxisKind::Position, 0, floating);
        Check(f.history.History(false) == MMS_OK && f.history.RedoCount() == 1, "seed genuine redo before rotation failure gates");
        RotationDraftSeed(f);
        if (noop) { if (menu) { f.first.bones[0].rotQuat[2] = 0; f.first.bones[0].rotQuat[3] = 1; }
            else f.app->BoneRotationEditDegreesX() = 0; }
        for (int i = 0; i < 3; ++i) RotationDraftPanel::Pending(panel.Field(i), "rotation pending");
        panel.Unrelated(); panel.Draft(0, "88.75");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if (reason == 0) Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations, 128, 1) == MMS_OK, "limit rotation journal capture budget");
        if (reason == 2) Check(f.history.Enter() == MMS_OK, "hold real journal busy gate before rotation reset");
        if (reason == 3) {
            f.app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(4); f.app->CameraPitch() = .25f;
            f.app->CameraDistance() = -45; f.app->CameraFov() = 45; f.app->CameraParentModel() = -1;
            std::shared_ptr<plugins::HistoryChange> change;
            Check(plugins::PlanCameraRotationReset(*f.app, change) == MMS_OK && change &&
                f.history.PrepareNative("reserved rotation regression", std::move(change), transaction) == MMS_OK && transaction,
                "prepare an actual native candidate before rotation reset");
        }
        const auto before = f.first.Digest(); const auto cached = PoseAxisReadDegrees(*f.app);
        const auto redo = f.history.RedoCount(); std::uint64_t revision = 0, after = 0; f.history.Revision(revision);
        if (reason == 1) failAfter = 0;
        panel.Command(menu); failAfter = -1; f.history.Revision(after);
        Check(before == f.first.Digest() && cached == PoseAxisReadDegrees(*f.app) && !f.history.UndoCount() &&
            redo == f.history.RedoCount() && revision == after &&
            RotationDraftPanel::Text(panel.Field(0), "88.75") && RotationDraftPanel::Modified(panel.Field(0)) && panel.UnrelatedPreserved(),
            "failed rotation reset and busy no-op preserve authored pose derived angles and pending drafts");
        for (int i = 1; i < 3; ++i) Check(RotationDraftPanel::Text(panel.Field(i), "rotation pending") && RotationDraftPanel::Modified(panel.Field(i)),
            "failure does not clear later rotation fields");
        if (reason == 2) f.history.Leave();
        transaction.reset(); panel.Clear();
        if (reason == 3) { f.history.Reset(); f.history.Collect(); mdl::KeyPoolDestroy(f.app->CameraKeys()); f.app->CameraKeys() = nullptr; }
    }
}
void RotationDraftTextFailureChecks(bool menu) {
    struct Fault {
        bool reject = true; unsigned writes = 0;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& f = *reinterpret_cast<Fault*>(data);
            if (message == WM_SETTEXT && f.reject) { ++f.writes; return FALSE; }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); RotationDraftPanel panel(f, floating);
        RotationDraftSeed(f); panel.Draft(0, "77.25"); Fault fault;
        Check(SetWindowSubclass(panel.Field(0), Fault::Proc, 2091, reinterpret_cast<DWORD_PTR>(&fault)) != FALSE, "install actual rotation caption failure");
        panel.Command(menu); const auto authored = f.first.Digest();
        Check(fault.writes && f.history.UndoCount() == 1 && RotationDraftPanel::Text(panel.Field(0), "77.25") &&
            !RotationDraftPanel::Modified(panel.Field(0)), "failed caption write still revokes stale draft after successful authoring");
        panel.LoseFocus(0); Check(f.first.Digest() == authored && f.history.UndoCount() == 1, "caption failure cannot revive draft on actual focus loss");
        fault.reject = false; panel.Command(menu);
        RotationDraftCaption(panel, 0, PoseAxisReadDegrees(*f.app)[0]);
        Check(f.history.UndoCount() == 1, "a no-op retry repairs caption without a second authored change");
        RemoveWindowSubclass(panel.Field(0), Fault::Proc, 2091);
    }
}
void RotationDraftReentryChecks(bool menu) {
    struct Reentry {
        PoseNumericFixture& fixture; unsigned action; HWND replacement; bool armed = true, fired = false;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Reentry*>(data);
            if (self.armed && message == EM_SETMODIFY && !wp) {
                self.armed = false; self.fired = true;
                const auto result = DefSubclassProc(window, message, wp, lp);
                if (self.action == 0) self.fixture.app->SelectedModelSlot() = 1;
                if (self.action == 1) { plugins::NativePoseTestBinding nested(*self.fixture.app, self.fixture.history); }
                if (self.action == 2) self.fixture.app->FloatingWindow() = self.replacement;
                self.fixture.app->BoneRotationEditDegreesX() = 71;
                self.fixture.app->BoneRotationEditDegreesY() = 72;
                self.fixture.app->BoneRotationEditDegreesZ() = 73;
                RotationDraftPanel::Pending(window, "new context draft");
                return result;
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (unsigned action = 0; action < 3; ++action) {
        PoseNumericFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history); RotationDraftPanel panel(f, floating);
        RotationDraftSeed(f); f.app->BoneRotationEditDegreesX() = 0;
        if (menu) { f.first.bones[0].rotQuat[2] = 0; f.first.bones[0].rotQuat[3] = 1; }
        for (int i = 0; i < 3; ++i) RotationDraftPanel::Pending(panel.Field(i), "rotation pending");
        panel.Draft(0, "91.5");
        HWND replacement = CreateWindowExA(0, "STATIC", "owned replacement rotation panel", WS_OVERLAPPED,
            0, 0, 300, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(replacement != nullptr, "create owned replacement panel for synchronous owner change");
        for (int i = 0; i < 3; ++i) {
            HWND edit = CreateWindowExA(0, "EDIT", "replacement pending", WS_CHILD, i * 90, 0, 80, 20, replacement,
                reinterpret_cast<HMENU>(INT_PTR(panel::kReadoutRotXEdit + i)), GetModuleHandleW(nullptr), nullptr);
            Check(edit != nullptr, "replacement owner has actual rotation EDIT identities");
            SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
        }
        Reentry reentry{f, action, replacement};
        Check(SetWindowSubclass(panel.Field(0), Reentry::Proc, 2092, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE, "install real EDIT synchronous context transition");
        const auto before = f.first.Digest(); panel.Command(menu);
        Check(reentry.fired && RotationDraftPanel::Text(panel.Field(0), "new context draft") && RotationDraftPanel::Modified(panel.Field(0)) &&
            PoseAxisReadDegrees(*f.app) == std::array<float, 3>{71, 72, 73} && f.first.Digest() == before && !f.history.UndoCount(),
            "rotation publication stops after actual message changes selected object binding generation or readout owner");
        for (int i = 1; i < 3; ++i) Check(RotationDraftPanel::Text(panel.Field(i), "rotation pending") && RotationDraftPanel::Modified(panel.Field(i)),
            "reentrant rotation publication cannot clear remaining old-owner drafts");
        for (int i = 0; i < 3; ++i) {
            HWND edit = GetDlgItem(replacement, panel::kReadoutRotXEdit + i);
            Check(RotationDraftPanel::Text(edit, "replacement pending") && RotationDraftPanel::Modified(edit),
                "old publication cannot write or clear replacement owner fields");
            SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
        }
        RemoveWindowSubclass(panel.Field(0), Reentry::Proc, 2092);
        panel.Clear(); f.app->SelectedModelSlot() = 0; f.app->FloatingWindow() = floating ? panel.base.owner : nullptr;
        DestroyWindow(replacement);
    }
}
void NativeAxisRotationDraftChecks() {
    RotationDraftHelperChecks(); RotationDraftSuccessChecks(false); RotationDraftNoopChecks(false);
    RotationDraftFailureChecks(false); RotationDraftTextFailureChecks(false); RotationDraftReentryChecks(false);
}
void NativeMenuRotationDraftChecks() {
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "rotation-reset fixture uses only inherited Default desktop");
    Check(d3dx::Get().Load(), "menu rotation draft regression requires real D3DX math");
    RotationDraftSuccessChecks(true); RotationDraftNoopChecks(true); RotationDraftFailureChecks(true);
    RotationDraftTextFailureChecks(true); RotationDraftReentryChecks(true);
}
}
