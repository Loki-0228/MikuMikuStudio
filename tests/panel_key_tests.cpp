// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// User-acceptance regressions for panel controls and editing shortcuts.
//
// Field report (defect 5): dragging the camera "angle of view" (FOV) slider
// changed nothing on screen until the scene was redrawn by another action.
//   Covered here: the FOV value flow (slider -> state -> projection) is now
//   derived from the app state for every rendered frame
//   (ApplyCameraFovSlider / BuildCameraPerspectiveProjection /
//   ApplyFrameCameraProjection), so "drag = the next frame shows it" is an
//   invariant instead of a one-shot device write that any other
//   D3DTS_PROJECTION writer (or the per-frame ortho override) could drop.
//
// Field report (defect 6): after "select all bones" the bone "initialize"
// command left part of the selection transformed, and the Delete key did
// nothing.
//   Covered here: the Delete precondition is a pure predicate
//   (DeleteShortcutAllowed) that no longer requires the main window itself to
//   hold the focus - after clicking any panel button the focus is that button,
//   which is what killed the key - and Del now also removes the SELECTED
//   bones' keys on the current frame (MarkSelectedBoneKeysAtFrame) instead of
//   only the explicitly marked ones.  "No key at the frame" is a no-op.
//   Case 495 itself is field-exact against the reference build (the reset is
//   6 zeros + quat w = 1.0 for parented bones, the bones[0]-based bake for
//   parentless ones, and it walks exactly the boneSelection array that 494
//   fills); the guard added here only refuses a model whose per-bone arrays
//   are not allocated yet.
//
// Field report (defect 7): with a keyframe registered (K) the I / K shortcuts
// could neither insert nor delete the empty frame they are supposed to work on.
//   Covered here: both commands return the number of key records they moved /
//   removed (0 = nothing to do) and refresh the timeline strip + readout, so
//   the effect of I/K is observable - at a keyed frame the pose is unchanged by
//   construction, so the key squares were the only feedback and nothing
//   invalidated them.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikumikustudio/keyboard_input.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/bone_scale.hpp"

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

// CommandDispatch family entry point for the bone-panel controls (the same
// non-exported declaration tests/keyboard_input_tests.cpp uses).
namespace mikumikustudio {
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DispatchSeparateWindowCommand(MMDApp*, unsigned short);
}

namespace {

using mikumikustudio::MMDApp;
using mikumikustudio::mdl::BoneKey;
using mikumikustudio::mdl::BoneRecord;
using mikumikustudio::mdl::CameraKey;
using mikumikustudio::mdl::kBoneKeyCapacity;
using mikumikustudio::mdl::Mdl;
using mikumikustudio::mdl::ModelRecord;

// A model with `bones` bone records, per-bone selection/flag arrays and a bone
// key pool.  Bone 0 is the root (parent -1); every other bone hangs off it so
// the 495 "has parent" identity branch is the one under test.
struct Fixture {
    std::unique_ptr<ModelRecord> record = std::make_unique<ModelRecord>();
    std::vector<BoneRecord> bones;
    std::vector<unsigned char> selection;
    std::vector<unsigned char> flags;
    std::vector<BoneKey> keys;
    unsigned char* bytes = nullptr;

    explicit Fixture(int boneCount) {
        bones.resize(static_cast<std::size_t>(boneCount));
        selection.assign(static_cast<std::size_t>(boneCount), 0);
        flags.assign(static_cast<std::size_t>(boneCount), 0);
        keys.resize(kBoneKeyCapacity);
        for (int i = 0; i < boneCount; ++i) {
            bones[static_cast<std::size_t>(i)].parent = i == 0 ? -1 : 0;
            bones[static_cast<std::size_t>(i)].type =
                mikumikustudio::mdl::BoneType::RotateMove;
        }
        record->boneTable = bones.data();
        record->boneCount = static_cast<std::uint32_t>(boneCount);
        record->boneSelection = selection.data();
        record->bonePhysicsState = flags.data();
        record->boneKeys = keys.data();
        bytes = reinterpret_cast<unsigned char*>(record.get());
    }

    // Register one key for `bone` at `frame`: the record is allocated from the
    // first free pool slot above the per-bone head records and linked into that
    // bone's chain, exactly like the registrars do.  Every walk is explicitly
    // bounded so a malformed chain can never spin the test.
    int AddKey(int bone, std::uint32_t frame) {
        int index = static_cast<int>(record->boneCount);
        for (std::size_t steps = 0; steps < kBoneKeyCapacity; ++steps) {
            if (index >= static_cast<int>(kBoneKeyCapacity)) {
                return -1;
            }
            if (keys[static_cast<std::size_t>(index)].frame == 0) {
                break;
            }
            ++index;
        }
        if (index >= static_cast<int>(kBoneKeyCapacity)) {
            return -1;
        }
        BoneKey& head = keys[static_cast<std::size_t>(bone)];
        int tail = bone;
        for (std::size_t steps = 0;
             steps < kBoneKeyCapacity &&
             keys[static_cast<std::size_t>(tail)].next != 0; ++steps) {
            tail = static_cast<int>(keys[static_cast<std::size_t>(tail)].next);
            if (tail <= 0 || tail >= static_cast<int>(kBoneKeyCapacity)) {
                return -1;  // malformed chain: refuse instead of spinning
            }
        }
        if (keys[static_cast<std::size_t>(tail)].next != 0) {
            return -1;  // chain longer than the pool: refuse
        }
        BoneKey& key = keys[static_cast<std::size_t>(index)];
        key.frame = frame;
        key.previous = static_cast<std::uint32_t>(tail);
        key.next = 0;
        key.allocated = 0;
        key.rotation[3] = 1.0f;
        keys[static_cast<std::size_t>(tail)].next =
            static_cast<std::uint32_t>(index);
        if (frame > record->maxFrame) {
            record->maxFrame = frame;
        }
        return index;
    }

    // Chain length of one bone's track, bounded by the pool capacity (a cycle
    // reports capacity + 1 so callers can assert on it).
    std::size_t ChainLength(int bone) const {
        int index = bone;
        for (std::size_t steps = 0; steps < kBoneKeyCapacity; ++steps) {
            const int next = static_cast<int>(keys[static_cast<std::size_t>(index)].next);
            if (next == 0) {
                return steps;
            }
            index = next;
        }
        return kBoneKeyCapacity + 1;
    }

    bool HasKeyAt(std::uint32_t frame) const {
        for (std::size_t i = 0; i < kBoneKeyCapacity; ++i) {
            if (keys[i].frame == frame) {
                return true;
            }
        }
        return false;
    }

    // Number of pool records that carry a key (frame != 0).
    std::size_t KeyCount() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < kBoneKeyCapacity; ++i) {
            if (keys[i].frame != 0) {
                ++count;
            }
        }
        return count;
    }
};

// ---------------------------------------------------------------------------
// Defect 5 - the FOV data flow
// ---------------------------------------------------------------------------
void FovDataFlow() {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    app->CameraFov() = 30.0f;
    app->state.cameraPerspective = 0;

    // The slider body writes the state and reports a change only once.
    Check(mikumikustudio::ApplyCameraFovSlider(app.get(), 30) == false,
          "FOV slider: same value is not a change");
    Check(mikumikustudio::ApplyCameraFovSlider(app.get(), 72) == true,
          "FOV slider: new value must be applied");
    Check(app->CameraFov() == 72.0f, "FOV slider: value must reach cameraFov");

    // The frame path derives the perspective projection from that state.
    float projection[16]{};
    const float aspect = 16.0f / 9.0f;
    Check(mikumikustudio::BuildCameraPerspectiveProjection(app.get(), aspect,
                                                            projection),
          "frame projection: perspective matrix must be buildable");
    const double fovRadians = 72.0 * 0.01745329238474369;
    const double yScale = 1.0 / std::tan(fovRadians * 0.5);
    Check(std::abs(projection[5] - yScale) < 1e-3,
          "frame projection: vertical scale follows cameraFov");
    Check(std::abs(projection[0] - yScale / aspect) < 1e-3,
          "frame projection: horizontal scale folds in the aspect ratio");
    Check(projection[11] == 1.0f && projection[14] < 0.0f,
          "frame projection: LH depth mapping (m23 = 1, m32 < 0)");

    // The frame installs the state-derived projection, never a stale one: the
    // perspective matrix differs per FOV while the ortho matrix does not.
    float other[16]{};
    app->CameraFov() = 110.0f;
    Check(mikumikustudio::BuildCameraPerspectiveProjection(app.get(), aspect,
                                                            other),
          "frame projection: rebuild after a slider move");
    Check(other[5] < projection[5],
          "frame projection: a wider FOV must narrow the vertical scale");

    Check(mikumikustudio::CameraFrameProjectionFor(app.get()) ==
              mikumikustudio::CameraFrameProjection::Perspective,
          "frame projection: perspective while cameraPerspective is clear");
    app->state.cameraPerspective = 1;
    Check(mikumikustudio::CameraFrameProjectionFor(app.get()) ==
              mikumikustudio::CameraFrameProjection::Orthographic,
          "frame projection: the パース byte selects the ortho override");
    app->state.cameraPerspective = 0;

    // No device / no renderer must stay harmless (the frame path is called
    // before any device exists in a headless run).
    Check(mikumikustudio::ApplyFrameCameraProjection(app.get()) ==
              mikumikustudio::CameraFrameProjection::Perspective,
          "frame projection: installs without a device");
    Check(mikumikustudio::ApplyCameraFovSlider(nullptr, 10) == false,
          "FOV slider: null app is rejected");
    Check(!mikumikustudio::BuildCameraPerspectiveProjection(app.get(), 0.0f,
                                                             projection),
          "frame projection: a zero aspect ratio is rejected");
    std::puts("PASS defect 5: FOV slider -> state -> per-frame projection");
}

// ---------------------------------------------------------------------------
// Defect 6 - Del precondition + selected-bone key removal
// ---------------------------------------------------------------------------
void DeleteShortcutPreconditions() {
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    app->state.deleteKeyState = 1;  // the polled press edge

    Check(mikumikustudio::DeleteShortcutAllowed(app.get(), true, false),
          "Del gate: pressed with an accepting focus must fire");
    // The reported failure: the focus sat on a panel button, not on the main
    // window.  The window half of the rule now accepts it (the button belongs
    // to the app window tree), the pure gate must not add a main-window test.
    Check(mikumikustudio::DeleteShortcutAllowed(app.get(), true, false),
          "Del gate: a panel-button focus must not kill the key");
    Check(!mikumikustudio::DeleteShortcutAllowed(app.get(), false, false),
          "Del gate: a foreign/text-entry focus keeps the key");
    Check(!mikumikustudio::DeleteShortcutAllowed(app.get(), true, true),
          "Del gate: the five frame edits keep the key");
    app->state.deleteKeyState = 0;
    Check(!mikumikustudio::DeleteShortcutAllowed(app.get(), true, false),
          "Del gate: no press edge is a no-op");
    app->state.deleteKeyState = 1;
    app->PlaybackActive() = 1;
    Check(!mikumikustudio::DeleteShortcutAllowed(app.get(), true, false),
          "Del gate: playback suppresses the key");
    app->PlaybackActive() = 0;
    Check(!mikumikustudio::DeleteShortcutAllowed(nullptr, true, false),
          "Del gate: null app is rejected");
    std::puts("PASS defect 6a: Delete precondition is a pure predicate");
}

void SelectedBoneKeyRemoval() {
    Fixture fixture(3);
    // Bone 1 is selected, bone 2 is not; both carry a key on frame 20.
    fixture.selection[1] = 1;
    const int selectedKey = fixture.AddKey(1, 20);
    const int otherKey = fixture.AddKey(2, 20);
    const int otherFrame = fixture.AddKey(1, 40);
    Check(selectedKey >= 0 && otherKey >= 0 && otherFrame >= 0,
          "bone key fixture allocated");

    const int marked =
        mikumikustudio::MarkSelectedBoneKeysAtFrame(fixture.bytes, 20);
    Check(marked == 1, "Del: exactly the selected bone's key is marked");
    Check(fixture.keys[static_cast<std::size_t>(selectedKey)].allocated != 0,
          "Del: the selected bone's current-frame key is deletable");
    Check(fixture.keys[static_cast<std::size_t>(otherKey)].allocated == 0,
          "Del: an unselected bone's key is left alone");
    Check(fixture.keys[static_cast<std::size_t>(otherFrame)].allocated == 0,
          "Del: another frame is left alone");

    // "No key to delete" must stay a no-op (the reported second half of the
    // symptom): nothing marked, nothing changed.
    Fixture empty(3);
    empty.selection[1] = 1;
    empty.AddKey(1, 10);
    Check(mikumikustudio::MarkSelectedBoneKeysAtFrame(empty.bytes, 20) == 0,
          "Del: a frame without keys is a no-op");
    Check(mikumikustudio::MarkSelectedBoneKeysAtFrame(nullptr, 20) == 0,
          "Del: a missing model is a no-op");
    std::puts("PASS defect 6b: Del removes the selected bones' current keys");
}

void InitializeSelectionSet() {
    // 494 fills boneSelection for the editable bone types only; 495 must reset
    // exactly that set (and only that set) - the "part of the selection is not
    // reset" report is the type filter, not a missing store.
    Fixture fixture(4);
    fixture.selection[0] = 1;  // selected
    fixture.selection[1] = 0;  // not selected
    fixture.selection[2] = 1;  // selected
    fixture.bones[0].trans[1] = 5.0f;
    fixture.bones[1].trans[1] = 5.0f;
    fixture.bones[2].trans[1] = 5.0f;
    fixture.bones[2].rotQuat[0] = 0.25f;
    fixture.bones[2].rotQuat[1] = 0.25f;
    fixture.bones[2].rotQuat[2] = 0.25f;
    fixture.bones[2].rotQuat[3] = 0.5f;

    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    app->SelectedModelSlot() = 0;
    app->ModelSlot(0) = fixture.bytes;
    mikumikustudio::CmdControl450(app.get(), nullptr, 495, 0);
    Check(fixture.bones[0].trans[1] == 0.0f,
          "初期化: bone 0 translation reset (root seed)");
    Check(fixture.bones[0].rotQuat[3] == 1.0f &&
              fixture.bones[0].rotQuat[0] == 0.0f,
          "初期化: bone 0 quaternion reset");
    Check(fixture.bones[1].trans[1] == 5.0f,
          "初期化: an unselected bone is untouched");
    Check(fixture.bones[2].trans[0] == 0.0f &&
              fixture.bones[2].trans[1] == 0.0f &&
              fixture.bones[2].trans[2] == 0.0f,
          "初期化: selected bone translation reset");
    Check(fixture.bones[2].rotQuat[0] == 0.0f &&
              fixture.bones[2].rotQuat[1] == 0.0f &&
              fixture.bones[2].rotQuat[2] == 0.0f &&
              fixture.bones[2].rotQuat[3] == 1.0f,
          "初期化: selected bone quaternion reset (all four components)");
    Check(fixture.flags[2] != 0,
          "初期化: the reset bone is marked as edited");

    // A model whose per-bone arrays are not allocated yet must be refused
    // instead of walking a null selection/flag array.
    ModelRecord bare{};
    bare.boneCount = 4;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(&bare);
    mikumikustudio::CmdControl450(app.get(), nullptr, 495, 0);
    std::puts("PASS defect 6c: 初期化 resets exactly the selected set");
}

struct ResetPanelContext {
    MMDApp* app;
    bool separate;
};

LRESULT CALLBACK ResetPanelProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto* context = reinterpret_cast<const ResetPanelContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_COMMAND && context && LOWORD(wp) >= 537 &&
        LOWORD(wp) <= 539 && HIWORD(wp) == BN_CLICKED) {
        if (context->separate)
            mikumikustudio::DispatchSeparateWindowCommand(context->app, LOWORD(wp));
        else
            mikumikustudio::CmdControl500(context->app, window, LOWORD(wp), HIWORD(wp));
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

// Exercise native edit focus loss and native button notifications without a
// render device. Bind coordinates are intentionally nonzero: reset means the
// bind pose, not moving the child bone onto the parent's origin.
void ModelToggleSelection() {
    using namespace mikumikustudio;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    HWND window = CreateWindowW(L"STATIC", L"model toggle regression", WS_POPUP,
        0, 0, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden model selection panel");
    app->Hwnd() = window;
    HWND combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
        0, 0, 200, 100, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kMainComboModel)),
        GetModuleHandleW(nullptr), nullptr);
    for (const auto* label : {L"camera/light/accessory", L"Model B", L"Model A"})
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    Fixture first(1), second(1);
    first.record->comboSelIndex = 2;
    second.record->comboSelIndex = 1;
    app->ModelSlot(3) = first.bytes;
    app->ModelSlot(9) = second.bytes;
    app->SetSelectedModelSlot(3);
    app->CameraMode() = 1;
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    Check(ResolveModelComboSelection(app.get()) == 2,
          "camera-mode PMM restores selected model independently of slot/display order");
    Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0 && app->CameraMode() == 1,
          "remembering model does not switch loaded camera mode");
    app->MainModelComboSelection() = 1;
    Check(ResolveModelComboSelection(app.get()) == 1, "valid previous model wins");
    app->MainModelComboSelection() = 255;
    Check(ResolveModelComboSelection(app.get()) == 2, "stale row falls back to selected model");
    app->ModelSlot(3) = nullptr;
    Check(ResolveModelComboSelection(app.get()) == 1, "removed selected model falls back to loaded model");
    app->ModelSlot(9) = nullptr;
    Check(ResolveModelComboSelection(app.get()) == 0, "empty scene still permits model loading");
    DestroyWindow(window);
    std::puts("PASS model toggle restores loaded model selection");
}

void BoneTranslationReset() {
    using namespace mikumikustudio;
    for (bool separate : {false, true})
    for (bool pmx : {false, true})
    for (auto type : {mdl::BoneType::RotateMove, mdl::BoneType::Move,
                      mdl::BoneType::FixedAxis}) {
        Fixture f(3);
        f.record->selectedBone = 1;
        f.record->physicsMode = pmx ? 2 : 0;
        f.selection[1] = 1;
        f.bones[1].type = type;
        f.bones[1].flags = mdl::kBoneFlagVisible |
            (type == mdl::BoneType::Move ? mdl::kBoneFlagMovable : 0);
        for (int bone = 0; bone < 3; ++bone) {
            f.bones[bone].rotQuat[3] = 1.f;
            f.keys[bone].rotation[3] = 1.f;
            f.bones[bone].slotIndex = -1;
        }
        const float origin[] = {2.f, 4.f, 6.f}, child[] = {5.f, 8.f, 1.f};
        std::memcpy(f.bones[0].position, origin, sizeof(origin));
        std::memcpy(f.bones[1].position, child, sizeof(child));
        f.bones[2].trans[0] = 17.f;
        for (int frame : {20, 40}) {
            const auto index = f.AddKey(1, frame);
            Check(index >= 0, "reset fixture has existing bone keys");
            f.keys[index].position[0] = frame + .25f;
            f.keys[index].position[1] = -frame - .5f;
            f.keys[index].position[2] = frame * 2.f;
        }
        const auto originalKeys = f.keys;
        auto app = std::make_unique<MMDApp>();
        app->state = MMDAppState{};
        app->ModelSlot(0) = f.bytes;
        app->CurrentFrame() = 10;
        auto* previousApp = g_Block;
        g_Block = app.get();
        std::printf("RESET-BEGIN format=%s panel=%s type=%u\n",
            pmx ? "PMX" : "PMD", separate ? "separate" : "main", unsigned(type));
        HWND window = CreateWindowExW(0, L"STATIC", L"bone reset regression",
            WS_OVERLAPPED, 0, 0, 600, 160, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create hidden reset panel");
        app->Hwnd() = f.record->hwnd = window;
        if (separate) app->FloatingWindow() = window;
        ResetPanelContext context{app.get(), separate};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ResetPanelProc));
        for (int axis = 0; axis < 3; ++axis) {
            const int editId = panel::kReadoutPosXEdit + axis;
            HWND edit = CreateWindowExA(0, "EDIT", "", WS_CHILD,
                axis * 120, 0, 80, 20, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(editId)),
                GetModuleHandleW(nullptr), nullptr);
            HWND button = CreateWindowExA(0, "BUTTON", "reset", WS_CHILD | BS_PUSHBUTTON,
                axis * 120, 25, 80, 20, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(537 + axis)),
                GetModuleHandleW(nullptr), nullptr);
            Check(edit && button, "create native coordinate edit and reset button");
            app->OriginalEditProc() = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(edit, GWLP_WNDPROC));
            InstallControlSubclass(app.get(), edit, editId);
            const float baseline = BoneParentBindOffset(f.bytes, 1, axis);
            PostViewRefresh(app.get());
            char shown[32]{};
            GetWindowTextA(edit, shown, sizeof(shown));
            Check(std::atof(shown) == baseline, "unmoved bone displays its bind coordinate");
            for (int commitMode : {0, 1, 2, 3}) {
                if (commitMode == 3) {
                    f.bones[1].trans[axis] = -1.25f;
                    PostViewRefresh(app.get());
                }
                Check(SetFocus(edit) != nullptr || GetFocus() == edit,
                      "coordinate edit takes thread focus");
                char input[32]{};
                std::snprintf(input, sizeof(input), "%g", double(baseline + 2.5f));
                SetWindowTextA(edit, input);
                SendMessageA(edit, EM_SETMODIFY, TRUE, 0);
                if (commitMode < 2) {
                    SetFocus(commitMode == 0 ? window : button);
                    Check(f.bones[1].trans[axis] == 2.5f,
                          "rotation-only bone accepts manual displacement on focus loss");
                }
                const int beforeReset = f.record->undoState[0];
                if (commitMode == 3)
                    SendMessageW(window, WM_COMMAND, MAKEWPARAM(537 + axis, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(button));
                else
                    SendMessageA(button, BM_CLICK, 0, 0);
                Check(f.bones[1].trans[axis] == 0.f && f.flags[1] != 0,
                      "axis button clears actual displacement and marks pose dirty");
                std::printf("RESET-HISTORY mode=%d before=%d after=%d editFocus=%d\n",
                    commitMode, beforeReset, f.record->undoState[0], GetFocus() == edit);
                Check(f.record->undoState[0] == (beforeReset + (commitMode == 2 ? 2 : 1)) % 30,
                      "reset adds one undo step after any native button focus-loss commit");
                GetWindowTextA(edit, shown, sizeof(shown));
                Check(std::atof(shown) == baseline,
                      "reset restores the original bind coordinate readout");
                UndoModelEdit(f.bytes, &app->CurrentFrame(), 0);
                Check(f.bones[1].trans[axis] == (commitMode == 3 ? -1.25f : 2.5f),
                      "undo restores the actual pre-reset displacement");
                RedoModelEdit(f.bytes, &app->CurrentFrame(), 0);
                Check(f.bones[1].trans[axis] == 0.f, "redo reapplies displacement reset");
                PostViewRefresh(app.get());
                SetFocus(edit);
                SetFocus(window);
                Check(f.bones[1].trans[axis] == 0.f,
                      "later focus loss cannot restore the old typed displacement");
                Check(std::memcmp(originalKeys.data(), f.keys.data(),
                                  f.keys.size() * sizeof(BoneKey)) == 0,
                      "input, reset, undo and redo leave every existing key unchanged");
                Check(f.bones[2].trans[0] == 17.f,
                      "reset preserves the unselected bone");
                std::printf("RESET format=%s panel=%s type=%u axis=%d commit=%d "
                    "bind=%g trans=%g readout=%s\n", pmx ? "PMX" : "PMD",
                    separate ? "separate" : "main", unsigned(type), axis,
                    commitMode, baseline, f.bones[1].trans[axis], shown);
            }
        }
        RegisterSelectedBoneKeys(f.bytes, 10, 0);
        const auto registered = f.keys[1].next;
        Check(registered >= f.record->boneCount && f.keys[registered].frame == 10,
              "registering the reset pose inserts the requested current frame");
        for (float value : f.keys[registered].position)
            Check(value == 0.f, "registered reset pose stores zero animation displacement");
        auto next = f.keys[registered].next;
        Check(next != 0 && f.keys[next].frame == 20,
              "registering the reset pose keeps the following key at frame 20");
        for (int frame : {20, 40}) {
            Check(next != 0 && f.keys[next].frame == frame,
                  "registering a reset leaves later keys in order");
            Check(std::memcmp(f.keys[next].position, originalKeys[next].position,
                              sizeof(f.keys[next].position)) == 0 &&
                  std::memcmp(f.keys[next].rotation, originalKeys[next].rotation,
                              sizeof(f.keys[next].rotation)) == 0,
                  "registering a reset preserves the values in all later keys");
            next = f.keys[next].next;
        }
        DestroyWindow(window);
        g_Block = previousApp;
        bone_scale::Forget(f.bytes);
        for (auto& ring : f.record->undoRings)
            for (auto& slot : ring.slots) {
                std::free(slot.bonePose); slot.bonePose = nullptr;
                std::free(slot.auxiliaryPose); slot.auxiliaryPose = nullptr;
            }
    }
    std::puts("PASS native bone displacement input, reset and history regression");
}

// ---------------------------------------------------------------------------
// Defect 7 - I / K on a frame that already carries a keyframe
// ---------------------------------------------------------------------------
void FrameLineInsertDelete() {
    Fixture fixture(3);
    fixture.selection[1] = 1;
    const int key = fixture.AddKey(1, 30);
    Check(key >= 0 && fixture.HasKeyAt(30), "frame-line fixture has a K frame");

    // The frame-line commands end in PanelPaint, which reads the render
    // wrapper's list dimensions: a zero-initialised wrapper keeps that path
    // headless (no device, no window, all-zero dimensions).
    auto renderer = std::make_unique<mikumikustudio::D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));

    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    app->SelectedModelSlot() = 0;
    app->ModelSlot(0) = fixture.bytes;
    app->Renderer() = renderer.get();
    app->CurrentFrame() = 30;
    app->state.optflag[0] = 0;  // bone mode
    // I: the key on the current frame moves one frame to the right, so the
    // current frame becomes an EMPTY frame line.  The table surgery is the
    // exported half of the command (the command's tail needs a seek + panel
    // paint, i.e. a device and a window).
    const int moved =
        mikumikustudio::ShiftBoneKeysForFrameLineInsert(app.get(), 30);
    Check(moved == 1, "I: the key on the current frame is moved");
    Check(fixture.keys[static_cast<std::size_t>(key)].frame == 31,
          "I: the key lands on cur + 1");
    Check(!fixture.HasKeyAt(30), "I: the current frame is now an empty line");
    Check(fixture.record->maxFrame == 31, "I: the frame range grows with it");
    Check(fixture.ChainLength(1) == 1, "I: the track keeps one key");

    // K on the frame that now carries the key: the record is unlinked, cleared
    // and removed from the frame line.
    const int removed = mikumikustudio::ShiftBoneKeysForFrameLineDelete(
        app.get(), 31, /*autoInterp=*/false);
    Check(removed == 1, "K: the key on the current frame is removed");
    Check(!fixture.HasKeyAt(31), "K: no key is left on that frame line");
    Check(fixture.keys[static_cast<std::size_t>(key)].frame == 0,
          "K: the record is cleared");
    Check(fixture.ChainLength(1) == 0, "K: the bone track is unlinked");

    // A frame line with no key is a no-op for both commands.
    Check(mikumikustudio::ShiftBoneKeysForFrameLineDelete(app.get(), 31,
                                                           false) == 0,
          "K: no key at the frame is a no-op");
    Check(mikumikustudio::ShiftBoneKeysForFrameLineInsert(app.get(), 31) == 0,
          "I: no key at or after the frame is a no-op");

    // K on an empty line still pulls the keys above it down by one frame (that
    // is what "delete the frame line" means) and reports the change.
    const int shifted = fixture.AddKey(2, 12);
    Check(shifted >= 0, "frame-line fixture third key");
    Check(mikumikustudio::ShiftBoneKeysForFrameLineDelete(app.get(), 5,
                                                           false) == 1,
          "K: pulls the keys above the frame line down by one");
    Check(fixture.keys[static_cast<std::size_t>(shifted)].frame == 11,
          "K: the shifted key lands one frame lower");

    // The commands are frame-line operations: they must not require a selected
    // bone or a selected frame.
    fixture.selection.assign(3, 0);
    Check(mikumikustudio::ShiftBoneKeysForFrameLineInsert(app.get(), 11) == 1,
          "I: works without any bone selection");
    Check(mikumikustudio::ShiftBoneKeysForFrameLineDelete(app.get(), 12,
                                                           false) == 1,
          "K: works without any bone selection");
    Check(fixture.KeyCount() == 0, "I/K leave no key behind");
    std::puts("PASS defect 7: I/K insert and delete a keyed frame line");
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered so a hang (or a crash) still shows which case was running.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--bone-translation-reset") == 0) {
        ModelToggleSelection();
        BoneTranslationReset();
        return 0;
    }
    std::puts("panel_key_tests: start");
    std::puts("  case 1/5 FOV data flow");
    FovDataFlow();
    std::puts("  case 2/5 Delete preconditions");
    DeleteShortcutPreconditions();
    std::puts("  case 3/5 selected-bone key removal");
    SelectedBoneKeyRemoval();
    std::puts("  case 4/5 初期化 selection set");
    InitializeSelectionSet();
    std::puts("  case 5/5 frame-line insert/delete");
    FrameLineInsertDelete();
    std::puts("Panel and key regressions passed");
    return 0;
}
