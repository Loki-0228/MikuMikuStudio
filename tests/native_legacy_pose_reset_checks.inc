// MAINTENANCE: regression-keep. Enhanced unbound CmdControl450(495), native
// 30-slot undo/redo, real hidden EDITs and ordinary plugin Initialize(false).
// Does not test the original profile, a full application launch or rendering.
// Requires a marked isolated installation; never binds a common history except
// the explicitly named synchronous takeover counterexample.
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "../src/plugins/history_runtime.hpp"
#include <filesystem>
#include <fstream>
namespace {
void LegacyPoseResetTrace(const char* stage) {
    std::printf("RUN legacy reset %s\n", stage);
    std::fflush(stdout);
}
void LegacyPoseResetInstallation() {
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "legacy reset uses inherited Default desktop only");
    wchar_t module[32768]{}; const auto length = GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
    Check(length && length < std::size(module), "locate legacy-reset test executable installation");
    const auto root = std::filesystem::path(module).parent_path();
    std::ifstream marker(root / L"legacy-pose-reset-test-installation.txt", std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
    Check(content == "MMS legacy pose reset test installation v1\n", "legacy runtime test requires its exact independent-installation marker");
    const auto plugins = root / L"PLUGINS";
    Check(!std::filesystem::exists(plugins) || (std::filesystem::is_directory(plugins) && std::filesystem::is_empty(plugins)),
        "ordinary plugin initialization cannot load unrelated binaries from this isolated test installation");
}
void LegacyPoseResetUnbound(PoseCommandFixture& f) {
    Check(plugins::BoundNativePoseHistory(f.app.get()) == nullptr && !f.history.UndoCount() && !f.history.RedoCount(),
        "legacy reset fixture remains outside common-history migration binding");
}
void LegacyPoseResetCurve(PoseCommandFixture& f) {
    bone_curve::Tracks tracks;
    for (int axis = 0; axis < 9; ++axis) {
        animation_curve::Key first, last; last.frame = 20;
        first.tangent = last.tangent = animation_curve::Tangent::Free;
        first.value = last.value = axis >= 6 ? 1 : 0;
        if (!axis) { last.value = 13; first.outSlope = .2; last.inSlope = .1; first.outWeight = .2; last.inWeight = .4; }
        tracks[axis].keys = {first, last};
    }
    std::string error;
    Check(bone_curve::Write(f.first.raw(), 1, tracks, error), "seed real free curves for native reset side-state preservation");
}
bool LegacyPoseResetZero(const Fixture& f, int bone) {
    const float zero[3]{}, identity[4]{0, 0, 0, 1};
    return !std::memcmp(f.bones[bone].trans, zero, sizeof zero) && !std::memcmp(f.bones[bone].rotQuat, identity, sizeof identity);
}
// The genuine legacy numeric tail may repaint the timeline and sweep its row
// tables. Supply owned CPU/GDI resources instead of suppressing that tail.
struct LegacyPoseResetCpuPanel {
    PoseCommandFixture& f;
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::array<std::uint32_t, 2> boneRows{};
    std::array<std::uint32_t, 1> morphRows{};
    D3DRenderer* previousRenderer;
    std::uint32_t* previousBoneRows; std::uint32_t* previousMorphRows;
    HDC previousDc, dc = nullptr;
    HBITMAP previousBitmap, previousSpare, bitmap = nullptr, spare = nullptr, icons = nullptr;
    void* previousIcons;
    HGDIOBJ previousObject = nullptr;
    int previousWidth;
    explicit LegacyPoseResetCpuPanel(PoseCommandFixture& fixture) : f(fixture),
        previousRenderer(f.app->Renderer()), previousBoneRows(f.first.m->boneKeyIndices), previousMorphRows(f.first.m->morphKeyIndices),
        previousDc(f.app->PanelDC()), previousBitmap(f.app->PanelBitmap()), previousSpare(f.app->PanelSpareBitmap()),
        previousIcons(f.app->state.bmpRes101), previousWidth(f.app->SidebarWidth()) {
        std::memset(renderer.get(), 0, sizeof(*renderer));
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = 240; info.bmiHeader.biHeight = -160;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr); void* pixels = nullptr; void* sparePixels = nullptr; void* iconPixels = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        spare = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &sparePixels, nullptr, 0);
        icons = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &iconPixels, nullptr, 0);
        Check(dc && bitmap && spare && icons && pixels && sparePixels && iconPixels, "allocate actual CPU memory DC and owned panel spare and icon bitmaps");
        std::memset(pixels, 0, 240 * 160 * 4); std::memset(sparePixels, 0, 240 * 160 * 4); std::memset(iconPixels, 0, 240 * 160 * 4);
        previousObject = SelectObject(dc, bitmap);
        renderer->screenWidth = 240; renderer->screenHeight = 160; renderer->viewScale = 1;
        f.app->Renderer() = renderer.get(); f.app->PanelDC() = dc; f.app->PanelBitmap() = bitmap;
        f.app->PanelSpareBitmap() = spare; f.app->state.bmpRes101 = icons; f.app->SidebarWidth() = 210;
        f.first.m->boneKeyIndices = boneRows.data(); f.first.m->morphKeyIndices = morphRows.data();
        Check(!renderer->device && !renderer->d3d9, "numeric focus host has a real zeroed renderer wrapper and no GPU device or fake COM object");
    }
    ~LegacyPoseResetCpuPanel() {
        f.first.m->boneKeyIndices = previousBoneRows; f.first.m->morphKeyIndices = previousMorphRows;
        f.app->Renderer() = previousRenderer; f.app->PanelDC() = previousDc; f.app->PanelBitmap() = previousBitmap;
        f.app->PanelSpareBitmap() = previousSpare; f.app->state.bmpRes101 = previousIcons; f.app->SidebarWidth() = previousWidth;
        SelectObject(dc, previousObject); DeleteObject(bitmap); DeleteObject(spare); DeleteObject(icons); DeleteDC(dc);
    }
};
void LegacyPoseResetSuccessChecks() {
    Check(d3dx::Get().Load(), "legacy reset success and counterexamples use real D3DX math");
    for (bool floating : {false, true}) for (int topology = 0; topology < 3; ++topology) for (int focus : {0, 4, 8}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, floating); LegacyPoseResetUnbound(f);
        LegacyPoseResetCurve(f); PoseResetDraftSeed(f, true, topology);
        if (topology == 2) f.first.m->undoState[0] = f.first.m->undoState[1] = 29;
        const int target = f.first.m->selectedBone, other = 1 - target;
        const auto before = f.first.bones[target], untouched = f.first.bones[other]; const auto dirty = f.first.dirty;
        const auto selection = f.first.selected; const auto second = f.second.Digest(); const auto secondRing = PoseRingDigest(f.second);
        const auto scales = bone_scale::PoseValues(f.first.raw()); const auto curves = bone_curve::AuthoringState::Capture(f.first.raw());
        const auto cursor = f.first.m->undoState[0];
        panel.Drafts(focus); f.Command(495); panel.Expected(topology == 1);
        Check(LegacyPoseResetZero(f.first, target) && bone_scale::Identity(bone_scale::Pose(f.first.raw(), target)) &&
            f.first.dirty[target] == 1 && f.first.m->undoState[0] == (cursor + 1) % 30 &&
            f.first.m->undoState[1] == f.first.m->undoState[0] && PoseResetDraftAngles(*f.app) == std::array<float, 3>{0, 0, 0},
            "unbound root child and non-first parentless reset have local zero pose and exactly one native ring record");
        Check(!std::memcmp(&untouched, &f.first.bones[other], sizeof untouched) && f.first.dirty[other] == dirty[other] &&
            f.first.selected == selection && f.app->CurrentFrame() == 37 && !f.app->PhysicsResetPending() && f.SelectionUnchanged() &&
            f.second.Digest() == second && PoseRingDigest(f.second) == secondRing && !bone_scale::TransformsApplied(f.first.raw()) &&
            curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())),
            "unbound reset preserves unselected authored data curves other model frame selection and unapplied transforms");
        const auto reset = f.first.Digest(), ring = PoseRingDigest(f.first); panel.LoseFocus(focus);
        Check(reset == f.first.Digest() && ring == PoseRingDigest(f.first), "actual legacy focus loss cannot reapply a cancelled 495 draft or create another ring step");
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) &&
            !std::memcmp(before.trans, f.first.bones[target].trans, sizeof before.trans) &&
            !std::memcmp(before.rotQuat, f.first.bones[target].rotQuat, sizeof before.rotQuat) && f.first.dirty[target] == dirty[target] &&
            bone_scale::PoseValues(f.first.raw()) == scales && curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())) && f.app->CurrentFrame() == 37,
            "actual native undo restores reset translation quaternion flag scale and free curves without seeking");
        Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && LegacyPoseResetZero(f.first, target) &&
            bone_scale::Identity(bone_scale::Pose(f.first.raw(), target)) && curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())),
            "actual native redo restores the reset and retained free curves"); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetNoopChecks() {
    for (bool floating : {false, true}) for (int focus = 0; focus < 9; ++focus) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, true);
        f.Command(495); Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()), "create native redo before an unbound data no-op");
        auto& bone = f.first.bones[1]; std::fill(std::begin(bone.trans), std::end(bone.trans), 0.f);
        bone.rotQuat[0] = bone.rotQuat[1] = bone.rotQuat[2] = 0; bone.rotQuat[3] = 1;
        Check(bone_scale::SetPose(f.first.raw(), 1, bone_scale::Matrix{}), "make current authored target an actual no-op without consuming redo"); f.first.dirty[1] = 0;
        panel.Drafts(focus); const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto scales = bone_scale::PoseValues(f.first.raw());
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        failAfter = 0; f.Command(495); failAfter = -1; panel.Expected(true);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && scales == bone_scale::PoseValues(f.first.raw()) &&
            !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.first.dirty[1],
            "registered-identity legacy no-op allocates nothing and retains native redo dirty-only flag and complete authored state");
        panel.LoseFocus(focus); Check(before == f.first.Digest() && ring == PoseRingDigest(f.first), "all nine clean legacy fields stay read-only on focus loss after reset");
        Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()), "legacy no-op retained a usable native redo record"); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetSelectionAndRefreshChecks() {
    for (bool floating : {false, true}) for (bool empty : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, false);
        f.first.selected[0] = empty ? 0 : 1; f.first.selected[1] = 0; panel.Drafts(3);
        const auto current = f.first.bones[1]; const auto degrees = PoseResetDraftAngles(*f.app); const auto cursor = f.first.m->undoState[0];
        f.Command(495);
        Check(panel.DraftsPreserved() && degrees == PoseResetDraftAngles(*f.app) && !std::memcmp(&current, &f.first.bones[1], sizeof current) &&
            f.first.m->undoState[0] == (empty ? cursor : (cursor + 1) % 30),
            "unbound empty selection or displayed-unselected bone keeps every pending draft and derived angle"); LegacyPoseResetUnbound(f);
    }
    for (bool floating : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, true); panel.Drafts(0);
        EnableWindow(panel.Field(8), FALSE); Check(!IsWindowEnabled(panel.Field(8)), "disable nonfocused scale EDIT as full-refresh sentinel");
        f.Command(495); panel.Expected(true);
        Check(!IsWindowEnabled(panel.Field(8)), "unbound 495 local caption publication never runs full readout that reenables scale controls"); panel.LoseFocus(0);
    }
}
void LegacyPoseResetAllocationChecks() {
    for (bool floating : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); LegacyPoseResetCurve(f); PoseResetDraftSeed(f, true); panel.Drafts(4);
        const unsigned next = (f.first.m->undoState[0] + 1) % 30; auto& reused = f.first.m->undoRings[0].slots[next];
        reused.operation = 2; reused.dirty = 2; reused.frame = 19;
        reused.bonePose = static_cast<mdl::BonePoseSnapshot*>(undo_storage::Allocate(2 * sizeof(mdl::BonePoseSnapshot)));
        reused.auxiliaryPose = undo_storage::Allocate(sizeof(mdl::BoneKeySnapshot));
        std::memset(reused.bonePose, 0, 2 * sizeof(mdl::BonePoseSnapshot)); std::memset(reused.auxiliaryPose, 0, sizeof(mdl::BoneKeySnapshot));
        reused.bonePose[0].boneIndex = 0; reused.bonePose[1].boneIndex = 1; reused.bonePose[1].position[0] = 42;
        reused.bonePose[0].rotation[3] = reused.bonePose[1].rotation[3] = 1;
        auto* key = static_cast<mdl::BoneKeySnapshot*>(reused.auxiliaryPose); key->keyIndex = 3; key->key.position[0] = 43;
        f.first.m->undoState[1] = next; f.first.m->redoDirty = 1;
        EnableWindow(f.Edit(panel::kRedoButton), TRUE);
        const auto* oldPose = reused.bonePose; const auto* oldAux = reused.auxiliaryPose;
        const std::array<mdl::BonePoseSnapshot, 2> poseBytes{oldPose[0], oldPose[1]}; const auto keyBytes = *key;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto scales = bone_scale::PoseValues(f.first.raw());
        const auto curves = bone_curve::AuthoringState::Capture(f.first.raw()); const auto degrees = PoseResetDraftAngles(*f.app);
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = f.app->PhysicsResetPending() = 0;
        bool succeeded = false; unsigned failures = 0;
        for (long limit = 0; limit < 512; ++limit) {
            failAfter = limit; f.Command(495); failAfter = -1;
            if (f.first.m->undoState[0] == next) { succeeded = true; break; }
            ++failures; undo_storage::Allocation allocation{};
            Check(reused.bonePose == oldPose && reused.auxiliaryPose == oldAux && undo_storage::Find(oldPose, allocation) &&
                allocation.bytes == sizeof poseBytes && undo_storage::Find(oldAux, allocation) && allocation.bytes == sizeof keyBytes,
                "failed reset preparation retains both live reused-slot allocations in the ownership registry before reading them");
            Check(!std::memcmp(oldPose, poseBytes.data(), sizeof poseBytes) && !std::memcmp(oldAux, &keyBytes, sizeof keyBytes) &&
                before == f.first.Digest() && ring == PoseRingDigest(f.first) && scales == bone_scale::PoseValues(f.first.raw()) &&
                curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())) && panel.DraftsPreserved() &&
                degrees == PoseResetDraftAngles(*f.app) && !IsWindowEnabled(f.Edit(panel::kUndoButton)) && IsWindowEnabled(f.Edit(panel::kRedoButton)) &&
                !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending(),
                "each allocation failure keeps ring pointers metadata payloads authoring scales free curves drafts and derived cache unchanged");
        }
        Check(succeeded && failures > 3 && reused.operation == 1 && reused.bonePose != oldPose,
            "allocation sweep reaches capture and side-state preparation then publishes a replacement native record");
        undo_storage::Allocation released{};
        Check(!reused.auxiliaryPose && !undo_storage::Find(oldPose, released) && !undo_storage::Find(oldAux, released),
            "successful reused-slot replacement clears obsolete auxiliary data and releases both old owned buffers");
        panel.Expected(true); panel.LoseFocus(4);
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && scales == bone_scale::PoseValues(f.first.raw()) &&
            curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())), "first successful reset after failures remains natively undoable with complete side state");
        Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && LegacyPoseResetZero(f.first, 1), "replacement of reused record remains redoable"); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetGuardChecks() {
    for (bool floating : {false, true}) for (bool noop : {false, true}) for (int reason = 0; reason < 7; ++reason) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, !noop); panel.Drafts(2);
        std::unique_ptr<ScopedSceneMutation> mutation; std::unique_ptr<bake::ScopedUiInputGate> input;
        if (reason == 0) f.app->PlaybackActive() = 1;
        if (reason == 1) f.app->FrameStepPlayback() = 1;
        if (reason == 2) f.app->state.recordPlaybackStartPending = 1;
        if (reason == 3) mutation = std::make_unique<ScopedSceneMutation>(*f.app);
        if (reason == 4) input = std::make_unique<bake::ScopedUiInputGate>();
        if (reason == 5) { Check(GetCapture() == nullptr, "no external capture before legacy reset guard test"); SetCapture(panel.owner); Check(GetCapture() == panel.owner, "capture only test-owned panel"); }
        if (reason == 6) f.app->FrameRangeDialog() = panel.owner;
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); const auto scales = bone_scale::PoseValues(f.first.raw()); const auto degrees = PoseResetDraftAngles(*f.app);
        f.Command(495);
        Check(before == f.first.Digest() && ring == PoseRingDigest(f.first) && scales == bone_scale::PoseValues(f.first.raw()) &&
            degrees == PoseResetDraftAngles(*f.app) && panel.DraftsPreserved(), "unbound changed and no-op reset honor playback mutation input capture and modal guards without discarding drafts");
        panel.Clear(); f.app->PlaybackActive() = f.app->FrameStepPlayback() = f.app->state.recordPlaybackStartPending = 0;
        if (reason == 5) { ReleaseCapture(); Check(GetCapture() == nullptr, "release only test-owned legacy capture"); }
        if (reason == 6) f.app->FrameRangeDialog() = nullptr; LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetIdentityChecks() {
    for (bool floating : {false, true}) for (bool noop : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, !noop); panel.Drafts(0);
        MmsObjectRef old{}; Check(plugins::FindSceneObjectIdentity(f.first.raw(), old), "fixture begins with real registered object identity");
        plugins::ForgetSceneObject(f.first.raw()); MmsObjectRef missing{};
        Check(!plugins::FindSceneObjectIdentity(f.first.raw(), missing), "remove identity through production object lifecycle API");
        const auto before = f.first.Digest(); const auto scales = bone_scale::PoseValues(f.first.raw());
        failAfter = 0; f.Command(495); failAfter = -1;
        Check(before == f.first.Digest() && scales == bone_scale::PoseValues(f.first.raw()) && panel.DraftsPreserved() &&
            !plugins::FindSceneObjectIdentity(f.first.raw(), missing), "failed first identity registration changes neither authored data nor drafts");
        f.Command(495); panel.Expected(true); MmsObjectRef live{};
        Check(plugins::FindSceneObjectIdentity(f.first.raw(), live) && live.generation != old.generation && plugins::SceneObjectPresent(f.first.raw(), live.id),
            "ordinary first reset establishes a new real stable identity before publishing readouts"); panel.LoseFocus(0); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetTextFailureChecks() {
    struct Fault {
        bool reject = true; unsigned writes = 0;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Fault*>(data);
            if (message == WM_SETTEXT && self.reject) { ++self.writes; return FALSE; }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (int focus : {0, 4, 8}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, true); panel.Drafts(focus); Fault fault;
        Check(SetWindowSubclass(panel.Field(focus), Fault::Proc, 2291, reinterpret_cast<DWORD_PTR>(&fault)) != FALSE, "reject real legacy reset caption write");
        f.Command(495); const auto after = f.first.Digest(), ring = PoseRingDigest(f.first);
        Check(fault.writes && LegacyPoseResetZero(f.first, 1) && PoseResetDraftPanel::Text(panel.Field(focus), "99.25") &&
            !PoseResetDraftPanel::Modified(panel.Field(focus)), "legacy reset cancels stale draft authorization even when its caption write fails");
        panel.LoseFocus(focus); Check(after == f.first.Digest() && ring == PoseRingDigest(f.first), "actual unbound focus loss cannot revive rejected reset caption or create a native record");
        fault.reject = false; f.Command(495); panel.Expected(true); Check(ring == PoseRingDigest(f.first), "legacy no-op repairs failed caption without changing history");
        RemoveWindowSubclass(panel.Field(focus), Fault::Proc, 2291); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetReentryChecks() {
    struct Reentry {
        PoseCommandFixture& f; HWND replacement; unsigned action; bool armed = true, fired = false;
        std::unique_ptr<plugins::NativePoseTestBinding> takeover;
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
            auto& self = *reinterpret_cast<Reentry*>(data);
            if (self.armed && message == EM_SETMODIFY && !wp) {
                self.armed = false; self.fired = true; const auto result = DefSubclassProc(window, message, wp, lp);
                if (self.action == 0) self.f.app->SelectedModelSlot() = 1;
                if (self.action == 1) self.f.app->FloatingWindow() = self.replacement;
                if (self.action == 2) self.f.first.selected[1] = 0;
                if (self.action == 3) self.takeover = std::make_unique<plugins::NativePoseTestBinding>(*self.f.app, self.f.history);
                if (self.action == 4) { plugins::ForgetSceneObject(self.f.first.raw()); plugins::SceneObjectIdentity(self.f.first.raw()); }
                self.f.app->BoneRotationEditDegreesX() = 71; self.f.app->BoneRotationEditDegreesY() = 72; self.f.app->BoneRotationEditDegreesZ() = 73;
                PoseResetDraftPanel::Pending(window, "new context draft"); return result;
            }
            return DefSubclassProc(window, message, wp, lp);
        }
    };
    for (bool floating : {false, true}) for (unsigned action = 0; action < 5; ++action) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, false); panel.Drafts(0);
        HWND replacement = CreateWindowExA(0, "STATIC", "owned legacy replacement", WS_OVERLAPPED, 0, 0, 300, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(replacement != nullptr, "create only owned replacement window for legacy readout reentry");
        for (int id : PoseResetDraftPanel::ids) {
            HWND edit = CreateWindowExA(0, "EDIT", "replacement pending", WS_CHILD, 0, 0, 80, 20, replacement, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Check(edit != nullptr, "create replacement real EDIT identities"); SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
        }
        Reentry reentry{f, replacement, action}; Check(SetWindowSubclass(panel.Field(0), Reentry::Proc, 2292, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE, "install actual synchronous legacy readout transition");
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first); f.Command(495);
        Check(reentry.fired && before == f.first.Digest() && ring == PoseRingDigest(f.first) && PoseResetDraftAngles(*f.app) == std::array<float, 3>{71, 72, 73} &&
            PoseResetDraftPanel::Text(panel.Field(0), "new context draft") && PoseResetDraftPanel::Modified(panel.Field(0)),
            "unbound publication stops after actual model owner selection common-binding takeover or stable identity transition");
        for (int i = 1; i < 9; ++i) Check(PoseResetDraftPanel::Text(panel.Field(i), "99.25") && PoseResetDraftPanel::Modified(panel.Field(i)), "stale unbound publication cannot clear later old-owner drafts");
        for (int id : PoseResetDraftPanel::ids) { HWND edit = GetDlgItem(replacement, id); Check(PoseResetDraftPanel::Text(edit, "replacement pending") && PoseResetDraftPanel::Modified(edit), "unbound publication cannot edit replacement owner drafts"); SendMessageA(edit, EM_SETMODIFY, FALSE, 0); }
        RemoveWindowSubclass(panel.Field(0), Reentry::Proc, 2292); panel.Clear(); reentry.takeover.reset();
        f.app->SelectedModelSlot() = 0; f.first.selected[1] = 1; f.app->FloatingWindow() = floating ? panel.owner : nullptr; DestroyWindow(replacement); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetEditedFocusChecks() {
    Check(d3dx::Get().Load(), "legacy edited rotation and scale regression uses real D3DX math");
    for (bool floating : {false, true}) for (int index : {0, 3, 6}) {
        PoseCommandFixture f; LegacyPoseResetCpuPanel cpu(f); PoseResetDraftPanel panel(f, floating);
        f.first.m->selectedBone = 0; f.first.selected[0] = 1; f.first.selected[1] = 0;
        f.first.bones[0].trans[0] = 1.234567f;
        f.app->BoneRotationEditDegreesX() = 0; f.app->BoneRotationEditDegreesY() = 0; f.app->BoneRotationEditDegreesZ() = 0;
        SetWindowTextA(panel.Field(0), "1.23"); panel.Clear();
        const auto clean = f.first.Digest(), ring = PoseRingDigest(f.first);
        for (int repeat = 0; repeat < 2; ++repeat) for (int field = 0; field < 9; ++field) {
            SetFocus(panel.Field(field)); Check(GetFocus() == panel.Field(field), "clean legacy field takes actual focus"); panel.LoseFocus(field);
        }
        Check(clean == f.first.Digest() && ring == PoseRingDigest(f.first) && f.first.bones[0].trans[0] == 1.234567f,
            "repeated clean legacy focus changes preserve full author precision and native ring");
        SetFocus(panel.Field(index)); Check(GetFocus() == panel.Field(index), "edited legacy numeric field takes actual focus");
        PoseResetDraftPanel::Pending(panel.Field(index), index == 6 ? "2" : "12.5");
        panel.LoseFocus(index);
        const bool changed = index == 0 ? f.first.bones[0].trans[0] == 12.5f : index == 3 ? f.first.bones[0].rotQuat[0] != 0 : !bone_scale::Identity(bone_scale::Pose(f.first.raw(), 0));
        Check(changed && PoseRingDigest(f.first) != ring, "actually modified unbound position rotation and scale still commit through real focus loss"); LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetOrdinaryRuntimeChecks() {
    for (bool floating : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); PoseResetDraftSeed(f, true);
        struct Runtime { ~Runtime() { plugins::Shutdown(); } } runtime;
        Check(plugins::NativeHistory(f.app.get()) == nullptr, "ordinary initialization starts without a leaked runtime");
        plugins::Initialize(f.app.get()); auto* history = plugins::NativeHistory(f.app.get());
        Check(history != nullptr && plugins::BoundNativePoseHistory(f.app.get()) == nullptr, "actual ordinary Initialize(false) succeeds while leaving native migration unbound");
        panel.Drafts(0); const auto before = f.first.bones[1]; const auto scale = bone_scale::Pose(f.first.raw(), 1); const auto cursor = f.first.m->undoState[0];
        f.Command(495); panel.Expected(true);
        Check(LegacyPoseResetZero(f.first, 1) && f.first.m->undoState[0] == (cursor + 1) % 30 && !history->UndoCount() && !history->RedoCount(),
            "ordinary initialized runtime routes actual 495 to one native ring step without common-history records"); panel.LoseFocus(0);
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && !std::memcmp(before.trans, f.first.bones[1].trans, sizeof before.trans) &&
            bone_scale::Pose(f.first.raw(), 1) == scale && TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && LegacyPoseResetZero(f.first, 1),
            "ordinary initialized runtime supports actual native undo and redo for 495");
        plugins::Shutdown(); Check(plugins::NativeHistory(f.app.get()) == nullptr && plugins::BoundNativePoseHistory(f.app.get()) == nullptr, "ordinary runtime shuts down between isolated cases");
        LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetAbsentStorageChecks() {
    for (bool floating : {false, true}) for (int target : {0, 1}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating);
        f.first.m->selectedBone = target; f.first.selected[0] = target == 0; f.first.selected[1] = target == 1;
        // No Seed/SetPose/curve writer: genuinely absent state must exercise
        // first side-state preparation, not merely an empty live pose map.
        f.first.bones[1].parent = -1; f.app->CurrentFrame() = 37;
        f.first.bones[target].trans[0] = 2.125f; f.first.bones[target].trans[2] = -4.75f;
        f.first.bones[target].rotQuat[2] = .6f; f.first.bones[target].rotQuat[3] = .8f;
        f.app->BoneRotationEditDegreesX() = 18; f.app->BoneRotationEditDegreesY() = 27; f.app->BoneRotationEditDegreesZ() = 36;
        panel.Drafts(0); bone_scale::Forget(f.first.raw());
        Check(!bone_scale::HasPoseStorage(f.first.raw()), "cold legacy reset begins with genuinely absent scale storage");
        const auto bone = f.first.bones[target]; const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
        const auto degrees = PoseResetDraftAngles(*f.app); const unsigned next = (f.first.m->undoState[0] + 1) % 30;
        bool succeeded = false; unsigned failures = 0;
        // A first scale State owns current + 30 undo + 30 redo Snapshots,
        // each with three std::maps. MSVC allocates 183 empty-map sentinels
        // before the other capture/node allocations: 128 cannot reach success.
        // This only widens fault-injection coverage, not a production budget.
        for (long limit = 0; limit < 512; ++limit) {
            failAfter = limit; f.Command(495); failAfter = -1;
            if (f.first.m->undoState[0] == next) { succeeded = true; break; }
            ++failures;
            Check(!bone_scale::HasPoseStorage(f.first.raw()) && before == f.first.Digest() && ring == PoseRingDigest(f.first) &&
                degrees == PoseResetDraftAngles(*f.app) && panel.DraftsPreserved(),
                "first-capture allocation failure leaves scale storage absent and retains ring pose and every draft");
        }
        const bool zero = LegacyPoseResetZero(f.first, target), unitScale = bone_scale::Identity(bone_scale::Pose(f.first.raw(), target));
        if (!succeeded || failures <= 3 || !zero || !unitScale) {
            const auto& value = f.first.bones[target];
            std::printf("DIAG cold reset success=%d failures=%u target=%d floating=%d cursor=%u next=%u pos=[%.9g,%.9g,%.9g] quat=[%.9g,%.9g,%.9g,%.9g] scaleUnit=%d storage=%d camera=%u playback=%u frameStep=%u pending=%u mutation=%d uiLock=%d saving=%d capture=%p range=%p runtime=%p binding=%p\n",
                int(succeeded), failures, target, int(floating), unsigned(f.first.m->undoState[0]), next,
                double(value.trans[0]), double(value.trans[1]), double(value.trans[2]), double(value.rotQuat[0]), double(value.rotQuat[1]), double(value.rotQuat[2]), double(value.rotQuat[3]),
                int(unitScale), int(bone_scale::HasPoseStorage(f.first.raw())), unsigned(f.app->CameraMode()), unsigned(f.app->PlaybackActive()), unsigned(f.app->FrameStepPlayback()),
                unsigned(f.app->state.recordPlaybackStartPending), int(f.app->SceneMutationInProgress()), int(bake::UiLocked()), int(SceneSaveBlocksEdits()),
                static_cast<void*>(GetCapture()), static_cast<void*>(f.app->FrameRangeDialog()), static_cast<void*>(plugins::NativeHistory(f.app.get())), static_cast<void*>(plugins::BoundNativePoseHistory(f.app.get())));
            std::fflush(stdout);
        }
        Check(succeeded, "cold-storage reset eventually publishes a native record within the fixed allocation sweep");
        Check(failures > 3, "cold-storage reset exercises staged-node allocation failures before success");
        Check(zero, "cold-storage reset publishes the exact zero position and identity quaternion");
        Check(unitScale, "cold-storage reset publishes unit scale");
        std::printf("PASS cold-storage allocation sweep floating=%d target=%d failures=%u\n", int(floating), target, failures);
        std::fflush(stdout);
        panel.Expected(false); const auto reset = f.first.Digest(); panel.LoseFocus(0);
        Check(reset == f.first.Digest(), "cold-storage reset draft stays cancelled on actual focus loss");
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && !std::memcmp(bone.trans, f.first.bones[target].trans, sizeof bone.trans) &&
            !std::memcmp(bone.rotQuat, f.first.bones[target].rotQuat, sizeof bone.rotQuat) && !f.first.dirty[target] &&
            bone_scale::Identity(bone_scale::Pose(f.first.raw(), target)), "native undo restores cold-storage original author pose and unit scale");
        Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && LegacyPoseResetZero(f.first, target), "native redo works for first-ever scale capture");
        LegacyPoseResetUnbound(f);
    }
    for (bool floating : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating); f.first.selected[0] = f.first.selected[1] = 1;
        f.first.m->selectedBone = 0; panel.Drafts(8); bone_scale::Forget(f.first.raw());
        const auto before = f.first.Digest(), ring = PoseRingDigest(f.first);
        Check(!bone_scale::HasPoseStorage(f.first.raw()), "all-zero no-op begins without scale storage");
        failAfter = 0; f.Command(495); failAfter = -1; panel.Expected(false);
        Check(!bone_scale::HasPoseStorage(f.first.raw()) && before == f.first.Digest() && ring == PoseRingDigest(f.first) &&
            !f.first.dirty[0] && !f.first.dirty[1], "all-zero selected legacy no-op cancels drafts without allocating scale storage or native history");
        panel.LoseFocus(8); Check(before == f.first.Digest() && !bone_scale::HasPoseStorage(f.first.raw()), "clean no-op focus loss keeps scale storage absent");
        LegacyPoseResetUnbound(f);
    }
}
void LegacyPoseResetMixedSelectionChecks() {
    for (bool floating : {false, true}) for (int changed : {0, 1}) for (bool showChanged : {false, true}) {
        PoseCommandFixture f; PoseResetDraftPanel panel(f, floating);
        f.first.selected[0] = f.first.selected[1] = 1; const int noop = 1 - changed;
        f.first.m->selectedBone = showChanged ? changed : noop; f.first.bones[1].parent = -1;
        f.first.bones[changed].trans[0] = 2.5f; f.first.bones[changed].trans[2] = -6.25f;
        f.first.bones[changed].rotQuat[2] = .6f; f.first.bones[changed].rotQuat[3] = .8f;
        Check(bone_scale::SetPose(f.first.raw(), changed, CommandScale()), "seed only changed member of shared selection");
        const auto original = f.first.bones[changed], untouched = f.first.bones[noop]; const auto scales = bone_scale::PoseValues(f.first.raw());
        const auto cursor = f.first.m->undoState[0]; panel.Drafts(4); f.Command(495); panel.Expected(false);
        Check(LegacyPoseResetZero(f.first, changed) && LegacyPoseResetZero(f.first, noop) && f.first.dirty[changed] == 1 && !f.first.dirty[noop] &&
            !std::memcmp(&untouched, &f.first.bones[noop], sizeof untouched) && f.first.m->undoState[0] == (cursor + 1) % 30 &&
            f.first.selected[0] && f.first.selected[1], "mixed selected reset records once and marks only changed bone while clearing selected changed-or-no-op displayed drafts");
        const auto after = f.first.Digest(); panel.LoseFocus(4); Check(after == f.first.Digest(), "mixed selected reset cannot reapply discarded draft on focus loss");
        Check(TryUndoModelEdit(f.first.raw(), &f.app->CurrentFrame()) &&
            !std::memcmp(original.trans, f.first.bones[changed].trans, sizeof original.trans) &&
            !std::memcmp(original.rotQuat, f.first.bones[changed].rotQuat, sizeof original.rotQuat) &&
            bone_scale::PoseValues(f.first.raw()) == scales && !f.first.dirty[changed] && !f.first.dirty[noop],
            "one native undo restores mixed-selection changed pose scale and both flags");
        Check(TryRedoModelEdit(f.first.raw(), &f.app->CurrentFrame()) && LegacyPoseResetZero(f.first, changed) &&
            f.first.dirty[changed] == 1 && !f.first.dirty[noop], "mixed-selection redo does not mark its unchanged selected bone dirty");
        LegacyPoseResetUnbound(f);
    }
}
}
void NativeLegacyPoseResetChecks() {
    const auto run = [](const char* name, void (*test)()) {
        LegacyPoseResetTrace(name); test();
    };
    run("Installation", LegacyPoseResetInstallation);
    run("SuccessChecks", LegacyPoseResetSuccessChecks);
    run("NoopChecks", LegacyPoseResetNoopChecks);
    run("SelectionAndRefreshChecks", LegacyPoseResetSelectionAndRefreshChecks);
    run("AllocationChecks", LegacyPoseResetAllocationChecks);
    run("GuardChecks", LegacyPoseResetGuardChecks);
    run("IdentityChecks", LegacyPoseResetIdentityChecks);
    run("TextFailureChecks", LegacyPoseResetTextFailureChecks);
    run("ReentryChecks", LegacyPoseResetReentryChecks);
    run("EditedFocusChecks", LegacyPoseResetEditedFocusChecks);
    run("OrdinaryRuntimeChecks", LegacyPoseResetOrdinaryRuntimeChecks);
    run("AbsentStorageChecks", LegacyPoseResetAbsentStorageChecks);
    run("MixedSelectionChecks", LegacyPoseResetMixedSelectionChecks);
}
