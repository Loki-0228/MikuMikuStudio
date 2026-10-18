// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Registration must preserve the per-bone frame-zero heads even when another
// key pool (or a freshly loaded model) left the shared search cursor at zero.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/keyboard_input.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/frame_scope.hpp"

namespace mikudancestudio {
bool PrepareFramePaste(MMDApp*);
void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DispatchLetterHotkeys(MMDApp*, HWND);
void StepFrame(MMDApp*, bool);
void RefreshAfterFrameApply(MMDApp*);
void JumpNextKeyframe(MMDApp*);
void JumpPrevKeyframe(MMDApp*);
// Test seam (frame_modes.cpp): drive one viewport box-select step with the
// caller's input instead of the thread key state and the foreground window.
// step 0 press edge, 1 held move, 2 release edge.
void DriveViewportBoxSelect(MMDApp*, int step, int x, int y);
}

namespace {
using namespace mikudancestudio;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

struct Fixture {
    static constexpr int kBones = 4;
    static constexpr int kShoulder = 2;
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, kBones> bones{};
    std::array<unsigned char, kBones> selection{};
    std::array<unsigned char, kBones> physics{};
    std::vector<mdl::BoneKey> keys{mdl::kBoneKeyCapacity};
    std::vector<mdl::MorphKey> morphKeys{mdl::kMorphKeyCapacity};
    std::vector<mdl::DisplayKey> displayKeys{mdl::kDisplayKeyCapacity};

    Fixture() {
        model->boneCount = kBones;
        model->boneTable = bones.data();
        model->boneSelection = selection.data();
        model->bonePhysicsState = physics.data();
        model->boneKeys = keys.data();
        model->morphKeys = morphKeys.data();
        model->displayKeys = displayKeys.data();
        model->morphCount = 1;
        for (int i = 0; i < kBones; ++i) {
            bones[i].rotQuat[3] = keys[i].rotation[3] = 1.f;
            bones[i].slotIndex = -1;
        }
        Select(kShoulder);
    }

    ~Fixture() {
        for (auto& ring : model->undoRings) {
            for (auto& slot : ring.slots) {
                ::operator delete(slot.bonePose);
                ::operator delete(slot.auxiliaryPose);
            }
        }
    }

    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }

    void Select(int bone) {
        selection.fill(0);
        selection[bone] = 1;
        model->selectedBone = bone;
    }

    void Register(int frame) { RegisterSelectedBoneKeys(Raw(), frame, 0); }

    // Validate sorted, disjoint tracks as well as reverse links. A cycle fails
    // this bounded walk instead of hanging the regression itself.
    void CheckTracks() const {
        std::vector<bool> visited(keys.size());
        for (int bone = 0; bone < kBones; ++bone) {
            Check(keys[bone].frame == 0, "registration preserves every frame-zero head");
            int current = bone;
            visited[current] = true;
            while (keys[current].next != 0) {
                const auto next = keys[current].next;
                Check(next >= kBones && next < keys.size(), "links stay outside bone heads and inside pool");
                Check(!visited[next], "bone tracks are acyclic and never share a key");
                Check(keys[next].previous == current, "reverse link points to predecessor");
                Check(keys[next].frame > keys[current].frame, "track frames strictly increase");
                visited[next] = true;
                current = static_cast<int>(next);
            }
        }
    }
};

void PhysicsSelectionAndLocalizedScopes() {
    Fixture f;
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SelectedModelSlot() = 0;
    app->EnglishUI() = 2;
    app->CurrentFrame() = 5;
    auto* previous = g_Block;
    g_Block = app.get();
    HWND window = CreateWindowExW(0, L"STATIC", L"physics selection regression",
        WS_OVERLAPPED, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    app->Hwnd() = f.model->hwnd = window;
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
        0, 0, 200, 200, window, reinterpret_cast<HMENU>(434), nullptr, nullptr);
    for (int id : {panel::kRangeStartEdit, panel::kRangeStopEdit})
        Check(CreateWindowExW(0, L"EDIT", L"5", WS_CHILD, 0, 0, 50, 20, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr) != nullptr,
            "create range controls");
    Check(window && combo, "create frame scope controls");
    f.Select(2); f.Register(5); f.Register(10);
    const auto onKey = f.keys[2].next;
    const auto offKey = f.keys[onKey].next;
    f.keys[offKey].physicsDisabled = 1;
    f.bones[2].hasRigidBody = 1;
    f.morphKeys[0].next = 1; f.morphKeys[1].frame = 5;
    f.displayKeys[0].next = 1; f.displayKeys[1].frame = 5;
    unsigned char ikSentinel = 37;
    f.displayKeys[1].ikStates = &ikSentinel;
    f.keys[0].allocated = f.morphKeys[1].allocated = f.displayKeys[1].allocated = 1;
    for (int repeat = 0; repeat < 3; ++repeat)
        CmdViewMenu(app.get(), window, 274, 0);
    Check(f.keys[2].allocated && f.keys[onKey].allocated && !f.keys[offKey].allocated,
          "physics menu selects frame zero and enabled later keys only");
    Check(!f.keys[0].allocated && !f.morphKeys[1].allocated && !f.displayKeys[1].allocated,
          "physics menu clears unrelated bone, morph and display selections");
    Check(f.displayKeys[1].ikStates == &ikSentinel && ikSentinel == 37,
          "clearing display selection preserves x64 pointer fields");
    f.CheckTracks();

    mdl::FrameGroup morphGroup{};
    strcpy_s(morphGroup.name, "face"); morphGroup.selected = 1;
    f.model->displayFrames = &morphGroup; f.model->facialFrameCount = 1;
    PostLoadInit(f.Raw());
    struct ScopeCase { FrameScope scope; const wchar_t* caption; bool bone, morph, display; };
    for (const auto& test : {
        ScopeCase{FrameScope::AllFrames, L"全部帧", true, true, true},
        ScopeCase{FrameScope::AllBones, L"全部骨骼", true, false, false},
        ScopeCase{FrameScope::Display, L"显示·IK·外亲", false, false, true},
        ScopeCase{FrameScope::SelectedBones, L"选中骨骼", true, false, false},
        ScopeCase{FrameScope::SelectedMorphs, L"选中表情", false, true, false},
        ScopeCase{FrameScope::AllMorphs, L"全部表情", false, true, false}}) {
        int row = -1;
        const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i)
            if (SendMessageW(combo, CB_GETITEMDATA, i, 0) == static_cast<LPARAM>(test.scope)) row = i;
        Check(row >= 0, "scope keeps a semantic identity after Chinese population");
        wchar_t caption[128]{};
        SendMessageW(combo, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(caption));
        Check(std::wcscmp(caption, test.caption) == 0, "scope has its Chinese caption");
        SendMessageW(combo, CB_SETCURSEL, row, 0);
        for (auto& key : f.keys) key.allocated = 0;
        for (auto& key : f.morphKeys) key.allocated = 0;
        for (auto& key : f.displayKeys) key.allocated = 0;
        CmdControl400(app.get(), window, 415, BN_CLICKED);
        Check(bool(f.keys[onKey].allocated) == test.bone &&
              bool(f.morphKeys[1].allocated) == test.morph &&
              bool(f.displayKeys[1].allocated) == test.display,
              "Chinese scope selects the intended tracks through the actual command");
        Check(!f.keys[0].allocated && !f.keys[offKey].allocated,
              "range selection excludes frame zero and frames beyond the chosen range");
    }
    app->ModelSlot(0) = nullptr;
    CmdViewMenu(app.get(), window, 274, 0);
    DestroyWindow(window);
    g_Block = previous;
}

void FreshModelRegistration() {
    Fixture f;
    f.bones[Fixture::kShoulder].rotQuat[2] = .25f;
    f.physics[Fixture::kShoulder] = 1;
    Check(f.model->searchCursor == 0, "fresh-model cursor starts at zero");
    f.Register(10);
    f.CheckTracks();
    const auto shoulderKey = f.keys[Fixture::kShoulder].next;
    Check(shoulderKey >= Fixture::kBones, "right shoulder receives its own key");
    Check(f.keys[shoulderKey].frame == 10 && f.keys[shoulderKey].rotation[2] == .25f,
          "registered key captures the selected shoulder pose");
    Check(f.keys[0].next == 0 && f.keys[0].rotation[2] == 0.f,
          "operation center receives no shoulder key or pose");
    Check(f.model->selectedBone == Fixture::kShoulder && f.selection[Fixture::kShoulder],
          "registration preserves bone selection");
    Check(f.physics[Fixture::kShoulder] == 0 && f.model->maxFrame == 10,
          "registration clears pose dirty flag and updates maximum frame");
    f.bones[Fixture::kShoulder].rotQuat[2] = .5f;
    f.Register(10);
    Check(f.keys[Fixture::kShoulder].next == shoulderKey && f.keys[shoulderKey].next == 0 &&
          f.keys[shoulderKey].rotation[2] == .5f, "same-frame registration overwrites without duplicate keys");
    f.Register(20);
    f.Register(15);
    f.Register(0);
    f.CheckTracks();
    Check(f.keys[Fixture::kShoulder].rotation[2] == .5f &&
          f.keys[f.keys[shoulderKey].next].frame == 15, "frame zero and insertion preserve the shoulder track");
}

void SharedCursorAndUndo() {
    Fixture f;
    // Display/morph registration can leave the shared cursor inside the bone
    // heads, including the selected head itself (which used to self-link).
    ResetDisplayKeyCursor(f.Raw());
    f.Register(5);
    ResetMorphKeyCursor(f.Raw());
    f.Register(10);
    f.model->searchCursor = Fixture::kShoulder;
    f.Register(15);
    f.CheckTracks();

    f.Select(3);
    f.selection[Fixture::kShoulder] = 1;
    f.Register(20);
    f.CheckTracks();
    Check(f.keys[f.keys[3].next].frame == 20, "multi-selection registers each selected bone");
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->CurrentFrame() = 20;
    f.model->morphCount = 0;
    CmdControl400(app.get(), nullptr, 400, 0);
    f.CheckTracks();
    Check(f.keys[3].next == 0, "undo button removes the newly registered track key");
    Check(app->CurrentFrame() == 20, "undo writes to the frame variable, not its numeric address");
    CmdControl400(app.get(), nullptr, 401, 0);
    f.CheckTracks();
    Check(f.keys[f.keys[3].next].frame == 20, "redo button restores the correct track");
    CmdControl400(app.get(), nullptr, 400, 0);
    f.model->searchCursor = static_cast<int>(mdl::kBoneKeyCapacity);
    f.Register(25);
    f.CheckTracks();
    Check(f.keys[f.keys[3].next].frame == 25, "new edit after undo reuses free bone slots");
}

// Runtime pool capacity: when the free-slot scan reaches the end of the pool,
// registration grows the pool instead of reporting the overflow, and the track
// stays linked inside the new block.
void PoolGrowthRegistration() {
    std::puts("bone_registration_tests: pool growth registration start");
    auto model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> bones{};
    std::array<unsigned char, 2> selection{}, physics{};
    // The bone-key registrar clears the marks of all three key arrays, so the
    // morph and display pools have to exist even though this case only touches
    // bone keys.
    std::vector<mdl::MorphKey> morphKeys(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> displayKeys(mdl::kDisplayKeyCapacity);
    for (auto& bone : bones) bone.rotQuat[3] = 1.f;
    model->boneCount = 2;
    model->boneTable = bones.data();
    model->boneSelection = selection.data();
    model->bonePhysicsState = physics.data();
    model->morphKeys = morphKeys.data();
    model->displayKeys = displayKeys.data();
    model->boneKeys = mdl::BoneKeyPoolCreate(16);
    model->searchCursor = 2;
    unsigned char* raw = reinterpret_cast<unsigned char*>(model.get());
    Check(mdl::BoneKeyCapacity(*model) == 16, "a created pool reports its own capacity");

    // Occupy every non-head slot, so the next registration has to run past the
    // end of the block.
    for (std::size_t slot = 2; slot < 16; ++slot) {
        model->boneKeys[slot].frame = 100 + static_cast<unsigned>(slot);
        model->boneKeys[slot].allocated = 1;
    }
    mdl::BoneKey* const before = model->boneKeys;
    bones[0].trans[1] = 5.f;
    selection[0] = 1;
    model->selectedBone = 0;
    RegisterSelectedBoneKeys(raw, 7, 0);
    std::puts("bone_registration_tests: pool growth registration returned");
    Check(model->boneKeys != before, "registration past the pool end grows the pool");
    Check(mdl::BoneKeyCapacity(*model) > 16, "the grown pool reports its new capacity");
    if (mdl::BoneKeyCapacity(*model) <= 16) {
        std::puts("bone_registration_tests: growth failed, stopping");
        mdl::BoneKeyPoolDestroy(model->boneKeys);
        return;
    }

    Check(model->boneKeys[0].next == 16, "the new key is linked after the bone head");
    Check(model->boneKeys[16].frame == 7 && model->boneKeys[16].previous == 0 &&
              model->boneKeys[16].allocated == 1,
          "the new key lands in the grown block with its reverse link");
    Check(model->boneKeys[16].position[1] == 5.f,
          "the new key captures the registered pose");
    Check(model->boneKeys[15].frame == 115,
          "growth copies the records the pool already held");
    Check(model->boneKeys[16].next == 0, "the new key terminates its track");

    bone_scale::Forget(raw);
    for (auto& ring : model->undoRings)
        for (auto& slot : ring.slots) {
            std::free(slot.bonePose);
            std::free(slot.auxiliaryPose);
        }
    mdl::BoneKeyPoolDestroy(model->boneKeys);
    std::puts("bone_registration_tests: runtime pool capacity growth");
}

void SelectedMorphFrameNavigation() {
    Fixture f;
    f.selection.fill(0);
    f.model->morphCount = 0; // Only the timeline mapping participates in this fixture.
    std::array<mdl::FrameGroup, 3> rows{};
    f.model->displayFrames = rows.data();
    f.model->facialFrameCount = 3;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    for (int row = 0; row < 3; ++row) {
        rows[row].targetIndex = static_cast<std::uint16_t>(row);
        f.morphKeys[row].next = row + 3;
        f.morphKeys[row + 3].previous = row;
        f.morphKeys[row + 3].frame = 10 + row * 10;
    }
    for (int selected : {1, 2}) {
        for (auto& row : rows) row.selected = 0;
        rows[selected].selected = 1;
        app->CurrentFrame() = 1;
        JumpNextKeyframe(app.get());
        Check(app->CurrentFrame() == 10 + selected * 10,
              "next-key navigation reaches the second and third selected morph rows");
        app->CurrentFrame() = 40;
        JumpPrevKeyframe(app.get());
        Check(app->CurrentFrame() == 10 + selected * 10,
              "previous-key navigation uses the complete FrameGroup stride");
    }
}

void FootIkRegistrationAndShortcuts() {
    Fixture f;
    // Keep existing all-parent / upper-body tracks, then select only foot IK.
    // Use the registration command (including timeline repaint) and the real
    // letter dispatcher so this cannot pass merely by bypassing the UI path.
    for (int bone : {0, 1}) {
        f.Select(bone);
        f.Register(8);
        f.Register(24);
    }
    f.Select(3);
    f.model->morphCount = 0;
    std::array<std::uint32_t, Fixture::kBones> rows{0, 1, 2, 3};
    f.model->boneKeyIndices = rows.data();
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SelectedModelSlot() = 0;
    app->SidebarWidth() = 604;
    app->CurrentFrame() = 12;
    HWND window = CreateWindowExW(0, L"STATIC", L"bone shortcut regression",
        WS_OVERLAPPED, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden shortcut-test window");
    HWND button = CreateWindowExW(0, L"BUTTON", L"register", WS_CHILD,
        0, 0, 80, 20, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kRegisterFrameButton)),
        GetModuleHandleW(nullptr), nullptr);
    Check(button != nullptr, "create registration button for shortcut focus");
    app->Hwnd() = window;
    f.model->hwnd = window;
    CmdControl500(app.get(), window, panel::kRegisterFrameButton, BN_CLICKED);
    f.CheckTracks();
    const auto footKey = f.keys[3].next;
    Check(footKey >= Fixture::kBones && f.keys[footKey].frame == 12,
          "register button writes the selected foot IK track");
    Check(app->state.rowHitBone[3 + 200 * 12] == footKey &&
          app->state.rowHitBone[200 * 12] == 0 && app->state.rowHitBone[1 + 200 * 12] == 0,
          "timeline shows foot IK key without all-parent or upper-body keys");
    f.Register(24);
    const auto footLater = f.keys[footKey].next;
    const auto rootEarlier = f.keys[0].next;
    const auto rootLater = f.keys[rootEarlier].next;
    const auto upperEarlier = f.keys[1].next;
    const auto upperLater = f.keys[upperEarlier].next;

    auto press = [&](unsigned char key) {
        unsigned char keyboard[256]{};
        app->LetterHotkeys().Update(keyboard, true);
        keyboard[key] = 0x80;
        app->LetterHotkeys().Update(keyboard, true);
        DispatchLetterHotkeys(app.get(), button);
        f.CheckTracks();
    };
    press('I');
    Check(f.keys[footKey].frame == 13 && f.keys[footLater].frame == 25 &&
          f.keys[rootLater].frame == 25 && f.keys[upperLater].frame == 25,
          "I pushes current and later frames across selected and unselected bone tracks");
    Check(app->state.rowHitBone[3 + 200 * 12] == 0 &&
          app->state.rowHitBone[3 + 200 * 13] == footKey,
          "I repaints the inserted empty frame and shifted foot IK key");
    press('K');
    Check(f.keys[footKey].frame == 12 && f.keys[footLater].frame == 24 &&
          f.keys[rootLater].frame == 24 && f.keys[upperLater].frame == 24,
          "K on an empty frame pulls all later bone keys back one frame");
    press('K');
    Check(f.keys[3].next == footLater && f.keys[footKey].frame == 0 &&
          f.keys[footLater].frame == 23 && f.keys[rootLater].frame == 23 &&
          f.keys[upperLater].frame == 23, "K removes the current key and pulls later frames forward");
    Check(f.keys[rootEarlier].frame == 8 && f.keys[upperEarlier].frame == 8 &&
          f.keys[2].next == 0, "I/K preserve earlier keys and never create keys on unrelated bones");
    Check(f.selection[3] && !f.selection[0] && !f.selection[1] && !f.selection[2],
          "foot IK remains the only selected bone");
    DestroyWindow(window);
}

LRESULT CALLBACK GestureWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED: case WM_CANCELMODE: case WM_COMMAND:
        return MainWndProc(hwnd, msg, wp, lp);
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void SelectedBoneMovement() {
    Fixture f;
    f.model->morphCount = 0;
    std::array<std::uint32_t, Fixture::kBones> rows{};
    f.model->boneKeyIndices = rows.data();
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    renderer->screenWidth = renderer->screenHeight = 800;
    renderer->viewScale = 1.f;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SidebarWidth() = 604;
    app->ViewportRect() = RECT{604, 30, 800, 550};
    app->RenderWidth() = app->RenderHeight() = 800;
    app->state.selectedClipW = 10.f;
    app->state.coordinateSystem = 1;
    for (int i = 0; i < Fixture::kBones; ++i) {
        f.bones[i].type = mdl::BoneType::Move;
        f.bones[i].parent = -1;
        f.bones[i].flags = mdl::kBoneFlagVisible;
        f.bones[i].selState = 650 + i * 20;
        f.bones[i].selState2 = 100;
        for (int n = 0; n < 16; ++n)
            f.bones[i].matInit[n] = f.bones[i].matWorld[n] = n % 5 == 0 ? 1.f : 0.f;
    }
    auto* previousApp = g_Block;
    g_Block = app.get();
    HWND window = CreateWindowExW(0, L"STATIC", L"bone movement",
        WS_OVERLAPPED, 0, 0, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden bone movement window");
    app->Hwnd() = f.model->hwnd = window;
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GestureWndProc));
    HWND edit = CreateWindowExA(0, "EDIT", "", WS_CHILD,
        0, 0, 80, 20, window, reinterpret_cast<HMENU>(panel::kReadoutPosXEdit),
        GetModuleHandleW(nullptr), nullptr);
    Check(edit != nullptr, "create native bone coordinate edit");
    app->OriginalEditProc() = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(edit, GWLP_WNDPROC));
    InstallControlSubclass(app.get(), edit, panel::kReadoutPosXEdit);
    SetWindowTextA(edit, "12.5");
    SendMessageA(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(window), 0);
    Check(f.bones[2].trans[0] == 12.5f && f.physics[2],
          "leaving a coordinate field commits that field to the selected bone");
    SetFocus(edit);
    Check(GetFocus() == edit, "coordinate edit receives thread focus");
    SetWindowTextA(edit, "13.5");
    const auto undoBeforeEnter = f.model->undoState[0];
    SendMessageA(edit, WM_KEYDOWN, VK_RETURN, 0);
    Check(f.bones[2].trans[0] == 13.5f && GetFocus() == window &&
          f.model->undoState[0] == undoBeforeEnter + 1,
          "Enter commits once, adds one undo entry and returns focus to the editor");

    // Changing frames must clear only pending pose edits. Keep the same
    // selected bone through typed seeks, step buttons and timeline clicks.
    for (int frame : {0, 5, 10}) {
        app->CurrentFrame() = frame;
        f.physics[2] = 1;
        RefreshAfterFrameApply(app.get());
        Check(f.selection[2] && f.model->selectedBone == 2 && !f.physics[2],
              "frame apply clears dirty pose flags without clearing bone selection");
        StepFrame(app.get(), true);
        StepFrame(app.get(), false);
        Check(app->CurrentFrame() == frame && f.selection[2] && f.model->selectedBone == 2,
              "frame step buttons preserve the active bone");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(106 + frame * 13, 150));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(106 + frame * 13, 150));
        Check(app->CurrentFrame() == frame && f.selection[2],
              "timeline header seek keeps the same selection as typed and button seeks");
        app->PreviousMouseY() = 120;
        app->MouseY() = 100;
        app->InteractionDragMode() = ViewportDragMode::PhysicsAxisX;
        BoneEditModes(app.get());
        Check(std::fabs(f.bones[2].trans[0] - 1.f) < .0001f && f.physics[2],
              "selected bone remains movable on empty nonzero frames");
        app->InteractionDragMode() = ViewportDragMode::None;
    }

    // Deselecting the active bone must leave the remaining highlighted bone
    // editable. Previously viewport Shift-click set selectedBone to -1.
    f.selection[3] = 1;
    f.model->selectedBone = 3;
    PostLanguageSweep(app.get());
    Check(f.model->selectedBone == 3, "refresh preserves a still-selected active bone");
    app->EditMode() = ViewportEditMode::Bone;
    app->MouseX() = f.bones[3].selState;
    app->MouseY() = f.bones[3].selState2;
    HandleViewportBonePress(app.get(), true);
    Check(!f.selection[3] && f.selection[2] && f.model->selectedBone == 2,
          "Shift deselection keeps the remaining highlighted bone as the edit target");
    // The direct press above starts a Shift box gesture. End it before the
    // following movement and name-list clicks, as a real mouse-up would.
    SendMessageW(window, WM_LBUTTONUP, MK_SHIFT,
                 MAKELPARAM(app->MouseX(), app->MouseY()));

    // Both drag paths use the same selection, without touching other bones.
    f.bones[2].trans[0] = 0;
    app->PreviousMouseX() = 700;
    app->MouseX() = 720;
    app->PreviousMouseY() = 120;
    app->MouseY() = 100;
    app->ShiftModifierState() = app->CtrlModifierState() = 0;
    app->InteractionDragMode() = ViewportDragMode::BoneMoveScreenPlane;
    BoneEditModes(app.get());
    Check(f.bones[2].trans[0] > 0 && f.bones[2].trans[1] > 0,
          "screen-plane handle moves the remaining selected bone");
    const float before = f.bones[2].trans[0];
    app->InteractionDragMode() = ViewportDragMode::PhysicsAxisX;
    BoneEditModes(app.get());
    Check(std::fabs(f.bones[2].trans[0] - before - 1.f) < .0001f,
          "axis control moves the remaining selected bone");
    SetWindowTextA(edit, "8.25");
    SendMessageA(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(window), 0);
    Check(f.bones[2].trans[0] == 8.25f && f.bones[3].trans[0] == 0,
          "coordinate edit follows the remaining selection and leaves the old target unchanged");

    f.selection[3] = 1;
    f.model->selectedBone = f.model->displayRootBone = 3;
    PostLanguageSweep(app.get());
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(50, 167));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(50, 167));
    Check(f.model->selectedBone == 2 && f.selection[2] && !f.selection[3],
          "Shift deselection in the name list also updates the active edit target");

    // Box/group selection can replace the bitmap without setting an active
    // bone. Its shared panel refresh must establish one before editing.
    f.selection.fill(0);
    f.selection[1] = 1;
    PostLanguageSweep(app.get());
    Check(f.model->selectedBone == 1, "replacement selection rejects a stale active bone");
    f.model->selectedBone = -1;
    PostLanguageSweep(app.get());
    Check(f.model->selectedBone == 1, "box/group selection establishes an active bone");
    f.selection.fill(0);
    PostLanguageSweep(app.get());
    Check(f.model->selectedBone == -1, "empty selection has no editable bone");
    DestroyWindow(window);
    g_Block = previousApp;
}

void TimelineSelectionAndRegisterClicks() {
    Fixture f;
    f.model->morphCount = 0;
    std::array<std::uint32_t, Fixture::kBones> rows{0, 1, 2, 3};
    f.model->boneKeyIndices = rows.data();
    std::array<unsigned, Fixture::kBones> first{}, last{};
    for (int bone = 0; bone < Fixture::kBones; ++bone) {
        f.Select(bone);
        f.Register(10);
        f.Register(20);
        first[bone] = f.keys[bone].next;
        last[bone] = f.keys[first[bone]].next;
    }
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SidebarWidth() = 604;
    auto* previousApp = g_Block;
    g_Block = app.get();
    HWND window = CreateWindowExW(0, L"STATIC", L"timeline gestures",
        WS_OVERLAPPED, 0, 0, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden gesture window");
    app->Hwnd() = f.model->hwnd = window;
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GestureWndProc));
    HWND button = CreateWindowExW(0, L"BUTTON", L"register", WS_CHILD,
        366, 700, 60, 28, window, reinterpret_cast<HMENU>(500), GetModuleHandleW(nullptr), nullptr);
    Check(button != nullptr, "create native registration button");
    auto send = [&](UINT msg, int x, int y) {
        SendMessageW(window, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(x, y));
    };
    auto clear = [&]() {
        for (auto& key : f.keys) key.allocated = 0;
        PanelPaint(app.get());
    };
    auto checkBox = [&]() {
        for (int bone = 1; bone < Fixture::kBones; ++bone)
            Check(f.keys[first[bone]].allocated && f.keys[last[bone]].allocated,
                  "box selects every enclosed key across multiple rows and columns");
        Check(!f.keys[first[0]].allocated && !f.keys[last[0]].allocated,
              "box excludes the root row above it");
    };
    // Include all frame-zero heads in a mixed selection and drag both ways.
    // Heads must remain at zero even when the operation center is not bone 0.
    f.model->displayRootBone = 3;
    rows = {1, 2, 3, 0};
    for (int bone = 0; bone < Fixture::kBones; ++bone) {
        f.keys[bone].allocated = 1;
        f.keys[first[bone]].allocated = 1;
    }
    PanelPaint(app.get());
    send(WM_LBUTTONDOWN, 106, 167);
    send(WM_MOUSEMOVE, 171, 167);
    f.CheckTracks();
    for (int bone = 0; bone < Fixture::kBones; ++bone)
        Check(f.keys[first[bone]].frame == 15, "drag moves sparse keys but keeps frame-zero heads");
    send(WM_MOUSEMOVE, -1000, 167);
    f.CheckTracks();
    for (int bone = 0; bone < Fixture::kBones; ++bone)
        Check(f.keys[first[bone]].frame == 1, "dragging left cannot move a later key over frame zero");
    send(WM_MOUSEMOVE, 106, 167);
    send(WM_LBUTTONUP, 106, 167);
    f.CheckTracks();
    // A scene saved after an older bad edit can already contain a displaced
    // head. A new drag must restore the zero anchor without discarding pose.
    f.keys[3].frame = 5;
    f.keys[3].position[0] = 12.5f;
    PanelPaint(app.get());
    send(WM_LBUTTONDOWN, 236, 181);
    send(WM_MOUSEMOVE, 301, 181);
    f.CheckTracks();
    Check(f.keys[3].position[0] == 12.5f && app->state.rowHitBone[0] == 3,
          "drag anchors a displaced operation-center head at zero without losing its pose");
    send(WM_MOUSEMOVE, 236, 181);
    send(WM_LBUTTONUP, 236, 181);
    f.CheckTracks();
    f.model->displayRootBone = 0;
    rows = {0, 1, 2, 3};
    clear();
    send(WM_LBUTTONDOWN, 165, 178);
    send(WM_MOUSEMOVE, 385, 217);
    send(WM_LBUTTONUP, 385, 217);
    checkBox();
    clear();
    send(WM_LBUTTONDOWN, 385, 217);
    // No intervening mouse-move: release must contribute its final position.
    send(WM_LBUTTONUP, 165, 178);
    checkBox();

    rows[1] = rows[2] = rows[3] = static_cast<std::uint32_t>(-2);
    clear();
    send(WM_LBUTTONDOWN, 165, 178);
    send(WM_MOUSEMOVE, 385, 217);
    PanelPaint(app.get());  // unrelated repaint while holding the box
    checkBox();
    send(WM_LBUTTONUP, 385, 217);
    checkBox();
    clear();
    send(WM_LBUTTONDOWN, 236, 195); // collapsed group, frame 10
    send(WM_LBUTTONUP, 236, 195);
    for (int bone = 1; bone < Fixture::kBones; ++bone)
        Check(f.keys[first[bone]].allocated && !f.keys[last[bone]].allocated,
              "click on a collapsed group uses frame/row coordinates");

    clear();
    send(WM_LBUTTONDOWN, 165, 178);
    send(WM_MOUSEMOVE, 385, 217);
    SetCapture(button); // no mouse-up to the main window
    Check(!app->SelectionBoxDragging(), "capture loss ends timeline selection");
    ReleaseCapture();
    f.Select(3);
    app->CurrentFrame() = 30;
    SendMessageW(button, BM_CLICK, 0, 0);
    const auto registered = f.keys[last[3]].next;
    Check(registered && f.keys[registered].frame == 30 && f.keys[registered].allocated,
          "one native register click after interrupted drag registers and highlights the selected bone");
    f.CheckTracks();
    Check(!f.keys[last[0]].next && !f.keys[last[1]].next && !f.keys[last[2]].next,
          "native register click leaves unselected bone tracks unchanged");
    send(WM_LBUTTONDOWN, 165, 178);
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    Check(!app->SelectionBoxDragging() && GetCapture() != window,
          "cancel mode ends drag and releases capture");

    // A click must not depend on a render frame falling between down and up.
    // Exercise both the name list and viewport, then the native button's
    // complete mouse message path, without calling the registration helper.
    app->ViewportRect() = RECT{604, 30, 800, 550};
    app->EditMode() = ViewportEditMode::Bone;
    f.model->displayRootBone = 3;
    f.bones[3].flags = mdl::kBoneFlagVisible;
    f.bones[3].selState = 650;
    f.bones[3].selState2 = 100;
    for (int frame = 31; frame <= 36; ++frame) {
        f.selection.fill(0);
        f.model->selectedBone = -1;
        PostLanguageSweep(app.get());
        app->CurrentFrame() = frame;
        const bool viewport = (frame % 2) == 0;
        send(WM_LBUTTONDOWN, viewport ? 650 : 50, viewport ? 100 : 167);
        send(WM_LBUTTONUP, viewport ? 650 : 50, viewport ? 100 : 167);
        Check(f.selection[3] && f.model->selectedBone == 3,
              "fast list and viewport clicks select the bone before the next render frame");
        SendMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
        SendMessageW(button, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
        unsigned key = 3;
        while (f.keys[key].next) key = f.keys[key].next;
        Check(f.keys[key].frame == frame && f.keys[key].allocated,
              "one native mouse click registers the freshly selected bone");
    }
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(650, 100));
    send(WM_LBUTTONUP, 650, 100);
    Check(!f.selection[3], "Shift viewport click toggles selection exactly once");

    std::vector<mdl::CameraKey> camera(mdl::kTimelineKeyCapacity);
    std::vector<mdl::LightKey> light(mdl::kTimelineKeyCapacity);
    std::vector<mdl::SelfShadowKey> shadow(mdl::kTimelineKeyCapacity);
    std::vector<mdl::GravityKey> gravity(mdl::kTimelineKeyCapacity);
    std::vector<mdl::AccessoryKey> accessory0(mdl::kTimelineKeyCapacity);
    std::vector<mdl::AccessoryKey> accessory1(mdl::kTimelineKeyCapacity);
    app->CameraKeys() = camera.data();
    app->LightKeys() = light.data();
    app->ShadowKeys() = shadow.data();
    app->GravityKeys() = gravity.data();
    app->AccessoryKeys(7) = accessory0.data();
    app->AccessoryKeys(19) = accessory1.data();
    for (auto* keys : {accessory0.data(), accessory1.data()}) {
        keys[0].next = 1;
        keys[1].frame = 10;
        keys[1].next = 2;
        keys[2].frame = 20;
        keys[2].previous = 1;
    }
    app->state.optflag[0] = 1;
    std::fill(std::begin(app->state.jointLineMap), std::end(app->state.jointLineMap), -1);
    app->state.jointLineMap[0] = 7;
    app->state.jointLineMap[1] = 19;
    PanelPaint(app.get());
    send(WM_LBUTTONDOWN, 165, 218);
    send(WM_MOUSEMOVE, 385, 248);
    Check(accessory0[1].selected && accessory0[2].selected &&
          accessory1[1].selected && accessory1[2].selected,
          "accessory box selects multiple tracks and frames");
    send(WM_MOUSEMOVE, 260, 248);
    Check(accessory0[1].selected && !accessory0[2].selected &&
          accessory1[1].selected && !accessory1[2].selected,
          "shrinking the box clears keys on their actual accessory slots");
    send(WM_LBUTTONUP, 260, 248);
    send(WM_LBUTTONDOWN, 165, 218);
    send(WM_MOUSEMOVE, -100, -100);
    send(WM_MOUSEMOVE, 32000, 32000);
    send(WM_LBUTTONUP, 32000, 32000);
    Check(!app->SelectionBoxDragging(), "captured drags outside the window stay within hit-map bounds");
    DestroyWindow(window);
    g_Block = previousApp;
}

// ---------------------------------------------------------------------------
// Main-viewport bone picking versus viewport box selection.
//
// The reported defect: with plain click selection on, clicking a bone in the
// main viewport selected nothing while box selection of the same bone worked.
// The click path (HandleViewportBonePress -> PickBoneAtCursor) gated every
// non-RotateMove bone out while a physics menu state was active and never
// consulted hasRigidBody, so ordinary bones that own no rigid body - Move,
// Ik, UnderIk controls and IK chain bones - lost their click under the
// default physics state.  The painted markers (sprite_overlay.cpp) and the
// box path (UpdateBoneBoxSelection) both keep those bones selectable and
// only lock bones that really are driven by a rigid body:
// mode 1 ("anytime") locks every hasRigidBody bone, mode 2 ("on/off") one
// while physicsDisabled == 0.  These cases drive the real
// WM_LBUTTONDOWN/WM_LBUTTONUP messages through MainWndProc in a hidden
// window.
// ---------------------------------------------------------------------------
struct PickFixture {
    static constexpr int kBones = 7;
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, kBones> bones{};
    std::array<unsigned char, kBones> selection{};
    std::array<unsigned char, kBones> physics{};
    std::vector<mdl::BoneKey> keys{mdl::kBoneKeyCapacity};
    std::vector<mdl::MorphKey> morphKeys{mdl::kMorphKeyCapacity};
    std::vector<mdl::DisplayKey> displayKeys{mdl::kDisplayKeyCapacity};
    std::array<std::uint32_t, kBones> rows{};

    PickFixture() {
        model->boneCount = kBones;
        model->boneTable = bones.data();
        model->boneSelection = selection.data();
        model->bonePhysicsState = physics.data();
        model->boneKeys = keys.data();
        model->morphKeys = morphKeys.data();
        model->displayKeys = displayKeys.data();
        model->boneKeyIndices = rows.data();
        model->morphCount = 0;
        model->selectedBone = -1;
        for (int i = 0; i < kBones; ++i) {
            bones[i].rotQuat[3] = 1.f;
            bones[i].slotIndex = -1;
            bones[i].parent = -1;
            bones[i].type = mdl::BoneType::Move;
            bones[i].flags = mdl::kBoneFlagVisible;
            bones[i].selState = 620 + i * 25;
            bones[i].selState2 = 100;
            for (int n = 0; n < 16; ++n)
                bones[i].matInit[n] = bones[i].matWorld[n] = n % 5 == 0 ? 1.f : 0.f;
        }
        bones[0].type = mdl::BoneType::RotateMove;   // control: always answered
        bones[1].type = mdl::BoneType::Move;         // ordinary, no rigid body
        bones[2].type = mdl::BoneType::Ik;
        bones[3].type = mdl::BoneType::UnderIk;
        bones[4].type = mdl::BoneType::Move;         // driven by a rigid body
        bones[4].hasRigidBody = 1;
        bones[5].type = mdl::BoneType::Move;         // rigid body, physics off
        bones[5].hasRigidBody = 1;
        bones[5].physicsDisabled = 1;
        bones[6].type = mdl::BoneType::Move;         // not drawn: no visible flag
        bones[6].flags = 0;
    }

    ~PickFixture() {
        for (auto& ring : model->undoRings) {
            for (auto& slot : ring.slots) {
                ::operator delete(slot.bonePose);
                ::operator delete(slot.auxiliaryPose);
            }
        }
    }

    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
};

void ViewportClickSelectionPhysicsLock() {
    PickFixture f;
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    renderer->screenWidth = renderer->screenHeight = 800;
    renderer->viewScale = 1.f;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SetSelectedModelSlot(0);
    app->SidebarWidth() = 604;
    app->ViewportRect() = RECT{604, 30, 800, 550};
    app->RenderWidth() = app->RenderHeight() = 800;
    app->EditMode() = ViewportEditMode::Bone;
    auto* previousApp = g_Block;
    g_Block = app.get();
    HWND window = CreateWindowExW(0, L"STATIC", L"viewport pick",
        WS_OVERLAPPED, 0, 0, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden viewport pick window");
    app->Hwnd() = f.model->hwnd = window;
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GestureWndProc));

    // keepSelection: the Shift multi-select cases need the selection to
    // survive across clicks - a plain call starts each case from scratch.
    const auto click = [&](int bone, WPARAM modifiers, bool keepSelection = false) {
        if (!keepSelection) {
            f.selection.fill(0);
            f.model->selectedBone = -1;
            PostLanguageSweep(app.get());
        }
        const int x = f.bones[bone].selState;
        const int y = f.bones[bone].selState2;
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | modifiers, MAKELPARAM(x, y));
        SendMessageW(window, WM_LBUTTONUP, modifiers, MAKELPARAM(x, y));
    };
    const auto picked = [&](int bone) {
        return f.selection[bone] != 0 && f.model->selectedBone == bone;
    };

    // An ordinary bone without a rigid body answers a real click in every
    // physics menu state.  Bones 1..3 are Move/Ik/UnderIk; before the fix
    // every one of them lost the click as soon as a physics mode was active.
    for (int mode : {0, 1, 2, 3}) {
        app->PlaybackPhysicsMode() = mode;
        for (int bone : {0, 1, 2, 3}) {
            click(bone, 0);
            char message[128];
            std::snprintf(message, sizeof(message),
                          "a click selects an ordinary bone without a rigid body "
                          "in every physics mode (mode %d, bone %d)", mode, bone);
            Check(picked(bone), message);
        }
    }

    // The lock stays where the marker pass puts it: only bones really driven
    // by a rigid body, mode 1 always, mode 2 while their physics is on.
    const auto checkLocked = [&](int mode, int bone, bool locked) {
        app->PlaybackPhysicsMode() = mode;
        click(bone, 0);
        char message[128];
        std::snprintf(message, sizeof(message),
                      locked ? "a rigid-body driven bone keeps its physics lock "
                               "(mode %d, bone %d)"
                             : "a rigid-body bone without physics ownership stays "
                               "clickable (mode %d, bone %d)",
                      mode, bone);
        if (locked)
            Check(!f.selection[bone] && f.model->selectedBone == -1, message);
        else
            Check(picked(bone), message);
    };
    checkLocked(1, 4, true);
    checkLocked(2, 4, true);
    checkLocked(0, 4, false);
    checkLocked(3, 4, false);
    checkLocked(1, 5, true);
    checkLocked(2, 5, false);
    checkLocked(0, 5, false);

    // The draw gate belongs to the pick too: a bone without the visible flag
    // paints no marker and takes no click.
    app->PlaybackPhysicsMode() = 2;
    click(6, 0);
    Check(!f.selection[6] && f.model->selectedBone == -1,
          "a bone without the visible flag takes no click");

    // Shift keeps the additive toggle and playback keeps its no-pick rule.
    // The selection bitmap carries "still selected"; selectedBone carries the
    // single edit target, so the two are asserted separately.
    click(1, 0);
    Check(picked(1), "the plain click selects one bone");
    click(2, MK_SHIFT, true);
    Check(f.selection[1] != 0 && f.selection[2] != 0 && f.model->selectedBone == 2,
          "Shift adds to the selection and moves the edit target");
    click(2, MK_SHIFT, true);
    Check(f.selection[1] != 0 && f.selection[2] == 0 && f.model->selectedBone == 1,
          "Shift deselects one bone and keeps the remaining edit target");
    app->state.playbackActive = 1;
    click(3, 0);
    Check(!f.selection[3], "playback keeps its no-pick rule");
    app->state.playbackActive = 0;

    DestroyWindow(window);
    g_Block = previousApp;
}

LRESULT CALLBACK PickCycleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_CAPTURECHANGED:
        return g_Block->FloatingWindow() == hwnd ? MicWndProc(hwnd, msg, wp, lp)
                                                 : MainWndProc(hwnd, msg, wp, lp);
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void ViewportOverlappingBoneCycle() {
    for (const bool pmx : {false, true}) for (const bool floating : {false, true}) {
        std::printf("overlapping bone cycle: %s %s viewport\n", pmx ? "PMX" : "PMD",
                    floating ? "floating" : "main");
        PickFixture f;
        f.model->physicsMode = pmx ? 2 : 0;
        auto renderer = std::make_unique<D3DRenderer>();
        std::memset(renderer.get(), 0, sizeof(*renderer));
        renderer->screenWidth = renderer->screenHeight = 800;
        renderer->viewScale = 1.f;
        auto app = std::make_unique<MMDApp>();
        app->state = MMDAppState{};
        app->Renderer() = renderer.get();
        app->ModelSlot(0) = f.Raw();
        app->SetSelectedModelSlot(0);
        app->SidebarWidth() = 604;
        app->ViewportRect() = RECT{604, 30, 800, 550};
        app->RenderWidth() = app->RenderHeight() = 800;
        app->EditMode() = ViewportEditMode::Bone;
        app->PlaybackPhysicsMode() = 2;
        for (auto& bone : f.bones) { bone.selState = 750; bone.selState2 = 200; }
        f.bones[0].selState = f.bones[1].selState = 650;
        f.bones[0].selState2 = f.bones[1].selState2 = 100;
        // The PMX loader maps flag 0x04 (translation enabled) to type 1;
        // type 0 supplies the overlapping rotation marker from the report.
        f.bones[0].flags = mdl::kBoneFlagVisible | 0x02;
        f.bones[1].flags = mdl::kBoneFlagVisible | 0x06;
        auto* previousApp = g_Block;
        g_Block = app.get();
        HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"overlap pick regression",
            WS_OVERLAPPED, 0, 0, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create hidden overlap pick window");
        app->Hwnd() = f.model->hwnd = window;
        if (floating) app->FloatingWindow() = window;
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PickCycleWndProc));
        const auto click = [&](UINT message = WM_LBUTTONDOWN, WPARAM modifiers = 0, int x = 650) {
            SendMessageW(window, message, MK_LBUTTON | modifiers, MAKELPARAM(x, 100));
            SendMessageW(window, WM_LBUTTONUP, modifiers, MAKELPARAM(x, 100));
        };
        const auto expectOne = [&](int bone, const char* reason) {
            bool matches = f.model->selectedBone == bone;
            for (int i = 0; i < PickFixture::kBones; ++i)
                matches &= (f.selection[i] != 0) == (i == bone);
            if (!matches)
                std::fprintf(stderr, "expected bone %d, active %d; mouse %d,%d; selection %d,%d,%d; mode %d; box %d\n",
                    bone, f.model->selectedBone, app->MouseX(), app->MouseY(),
                    f.selection[0], f.selection[1], f.selection[2],
                    static_cast<int>(app->EditMode()), app->BoneBoxSelectionActive());
            Check(matches, reason);
        };
        click(); expectOne(0, "first overlap click selects the first bone in model order");
        click(); expectOne(1, "second click reaches the overlapping translate/rotate bone");
        click(); expectOne(0, "next click wraps to the rotation bone");
        click(WM_LBUTTONDBLCLK); expectOne(1, "rapid second click advances once through WM_LBUTTONDBLCLK");
        click(); expectOne(0, "click after double-click wraps once");

        f.bones[2].selState = 650; f.bones[2].selState2 = 100;
        click(); expectOne(1, "three-bone overlap reaches the middle bone");
        click(); expectOne(2, "three-bone overlap reaches its final bone");
        click(); expectOne(0, "three-bone overlap wraps to its first bone");
        f.bones[1].flags = 0;
        click(); expectOne(2, "cycle skips hidden markers");
        f.bones[2].hasRigidBody = 1;
        click(); expectOne(0, "cycle skips bones locked by physics");
        f.bones[1].flags = mdl::kBoneFlagVisible | 0x06;
        f.bones[2].physicsDisabled = 1;
        click(); expectOne(1, "cycle keeps ordinary bones under physics on/off");
        click(); expectOne(2, "disabled physics lets the rigid-body bone rejoin the cycle");

        f.bones[0].selState = 700;
        f.bones[2].selState = 700;
        click(); expectOne(1, "single remaining hit is selected after wrap");
        click(); expectOne(1, "repeated clicks keep a single hit selected");
        click(WM_LBUTTONDOWN, 0, 680); expectOne(1, "empty click preserves selection");
        f.bones[0].selState = 658;
        click(); expectOne(1, "eight-pixel hit boundary remains exclusive");
        f.bones[0].selState = 657;
        click(); expectOne(0, "seven-pixel neighbour participates in wraparound");
        f.bones[0].selState = 650;
        f.model->selectedBone = PickFixture::kBones + 5;
        click(); expectOne(0, "stale active index wraps without indexing the bone array");

        // Shift retains the original toggle-all-hits behavior, independent of
        // ordinary-click cycling, including a still-selected active edit target.
        click(WM_LBUTTONDOWN, MK_SHIFT);
        expectOne(1, "Shift toggles both overlapping hits and retains the selected target");
        click(WM_LBUTTONDOWN, MK_SHIFT);
        expectOne(0, "second Shift click restores the prior selection");
        app->PlaybackActive() = 1;
        click(); expectOne(0, "playback still disables picking");
        app->PlaybackActive() = 0;
        Check(!IsWindowVisible(window), "overlap regression window remains hidden");
        app->FloatingWindow() = nullptr;
        DestroyWindow(window);
        g_Block = previousApp;
    }
}

void ViewportBoxSelectionStillCoversOrdinaryBones() {
    PickFixture f;
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    renderer->screenWidth = renderer->screenHeight = 800;
    renderer->viewScale = 1.f;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    app->Renderer() = renderer.get();
    app->ModelSlot(0) = f.Raw();
    app->SetSelectedModelSlot(0);
    app->SidebarWidth() = 604;
    app->ViewportRect() = RECT{604, 30, 800, 550};
    app->RenderWidth() = app->RenderHeight() = 800;
    app->EditMode() = ViewportEditMode::BoneBox;
    auto* previousApp = g_Block;
    g_Block = app.get();
    HWND window = CreateWindowExW(0, L"STATIC", L"viewport box select",
        WS_OVERLAPPED, 0, 0, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden viewport box select window");
    app->Hwnd() = f.model->hwnd = window;

    // The viewport bone box lives in the frame driver's poll chain
    // (BeginOrEndViewportToolDrag's operation-0 press/release branches and
    // UpdateBoneBoxSelection).  A hidden window cannot drive that poll: it
    // reads the thread key state and the foreground window, so faking those
    // would inherit the user's real Shift/Ctrl state and flip the box to
    // additive.  DriveViewportBoxSelect is the test seam that runs the same
    // chain with the caller's input, so every case here is deterministic and
    // the desktop is never touched.  Shift is set per case on the same state
    // the poll would write, which is what UpdateBoneBoxSelection reads.
    const auto box = [&](int step, int x, int y) {
        DriveViewportBoxSelect(app.get(), step, x, y);
    };

    for (int mode : {0, 1, 2, 3}) {
        app->PlaybackPhysicsMode() = mode;
        app->ShiftModifierState() = 0;
        f.selection.fill(0);
        f.model->selectedBone = -1;
        PostLanguageSweep(app.get());
        box(0, 615, 60);
        Check(app->BoneBoxSelectionActive() == 1,
              "the poll starts the viewport box on the press edge");
        box(1, 790, 140);
        for (int bone : {0, 1, 2, 3})
            Check(f.selection[bone] != 0,
                  "box selection still covers ordinary bones in every physics mode");
        // Shrinking the box drops the bones that leave it.
        box(1, 685, 140);
        Check(f.selection[0] != 0 && f.selection[3] == 0 && f.selection[6] == 0,
              "shrinking the box clears the bones outside it");
        box(2, 685, 140);
        Check(app->BoneBoxSelectionActive() == 0,
              "the release edge ends the viewport box");
    }

    // Shift keeps the box additive: shrinking it then keeps the bones that
    // already joined the selection even after they leave the box.
    app->PlaybackPhysicsMode() = 2;
    f.selection.fill(0);
    f.model->selectedBone = -1;
    PostLanguageSweep(app.get());
    box(0, 615, 60);
    box(1, 790, 140);
    app->ShiftModifierState() = 3;
    box(1, 685, 140);
    Check(f.selection[0] != 0 && f.selection[3] != 0 && f.selection[6] != 0,
          "Shift keeps the viewport box additive when it shrinks");
    app->ShiftModifierState() = 0;
    box(1, 685, 140);
    Check(f.selection[0] != 0 && f.selection[3] == 0 && f.selection[6] == 0,
          "dropping Shift lets the shrunk box clear the bones outside it");
    box(2, 685, 140);

    DestroyWindow(window);
    g_Block = previousApp;
}
}  // namespace

namespace {
void DisplayFramePaste() {
    Fixture f;
    std::array<mdl::BoneOrderEntry, 3> order{};
    order[0].boneIndex = -1;  // The model-wide external-parent sentinel.
    order[1].boneIndex = 2;
    order[2].boneIndex = 3;
    f.model->boneOrderCount = static_cast<std::uint32_t>(order.size());
    f.model->boneOrderTable = order.data();
    std::strcpy(f.bones[2].name, "left");
    std::strcpy(f.bones[3].name, "right");
    SYSTEM_INFO system{};GetSystemInfo(&system);
    const std::size_t page = system.dwPageSize;
    Check(sizeof f.bones <= page, "guarded bone fixture fits one page");
    auto* guarded = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_NOACCESS));
    Check(guarded && VirtualAlloc(guarded + page, page, MEM_COMMIT, PAGE_READWRITE), "allocate guarded bone table");
    std::memcpy(guarded + page, f.bones.data(), sizeof f.bones);
    f.model->boneTable = reinterpret_cast<mdl::BoneRecord*>(guarded + page);
    std::vector<std::array<mdl::BoneReference, 3>> states(mdl::kDisplayKeyCapacity);
    for (std::size_t i = 0; i < states.size(); ++i)
        f.displayKeys[i].selectorStates = states[i].data();

    std::array<mdl::SelectorClipboardState, 3> clipboard{};
    std::strcpy(clipboard[0].boneName, "root");
    std::strcpy(clipboard[1].boneName, "right");
    std::strcpy(clipboard[2].boneName, "left");
    clipboard[0].modelIndex = -1;
    clipboard[1].modelIndex = 7; clipboard[1].boneIndex = 9;
    clipboard[2].modelIndex = 4; clipboard[2].boneIndex = 6;
    const auto paste = [&](int frame, unsigned char visible) {
        ResetDisplayKeyCursor(f.Raw());
        Check(RegisterDisplayKeyFromRecord(f.Raw(), 0, visible, 0, nullptr,
                  static_cast<int>(clipboard.size()),
                  reinterpret_cast<const unsigned char*>(clipboard.data()), frame),
              "paste display frame into an empty timeline position");
        int key = 0;
        while (f.displayKeys[key].frame < static_cast<unsigned>(frame)) {
            key = static_cast<int>(f.displayKeys[key].next);
            Check(key > 0 && key < static_cast<int>(f.displayKeys.size()), "find pasted display frame");
        }
        Check(f.displayKeys[key].frame == static_cast<unsigned>(frame) &&
              f.displayKeys[key].visible == visible, "paste retains frame and visibility");
        Check(states[key][0].modelIndex == clipboard[0].modelIndex &&
              states[key][0].boneIndex == clipboard[0].boneIndex,
              "paste restores model-wide parent without indexing bone minus one");
        Check(states[key][1].modelIndex == 4 && states[key][1].boneIndex == 6 &&
              states[key][2].modelIndex == 7 && states[key][2].boneIndex == 9,
              "named bone parents are matched independently of clipboard order");
    };
    paste(10, 1);  // Append into an empty frame.
    clipboard[0].modelIndex = 2; clipboard[0].boneIndex = 5;
    paste(10, 0);  // Overwrite the frame.
    paste(5, 1);   // Insert before it.
    std::memset(clipboard[0].boneName, 0, sizeof clipboard[0].boneName);
    paste(7, 1);   // Frame-range edits encode the root with an empty name.
    Check(f.displayKeys[0].next != 0 && f.model->maxFrame == 10,
          "display-frame insertion keeps the frame-zero head and maximum frame");
    VirtualFree(guarded, 0, MEM_RELEASE);
}

void PasteDestinationNames() {
    Fixture f;
    auto app=std::make_unique<MMDApp>();
    app->state=MMDAppState{}; app->ModelSlot(0)=f.Raw();
    std::strcpy(f.bones[0].name,"target");
    std::strcpy(f.bones[1].name,"helper");
    std::strcpy(f.bones[2].name,"helper");
    mdl::BoneClipboardRecord key{}; std::strcpy(key.name,"target");
    app->BoneClipboard()=&key; app->ClipboardCounts().bones=1;
    Check(PrepareFramePaste(app.get()),"unrelated duplicate bone names do not block a valid paste");
    std::strcpy(key.name,"helper");
    Check(!PrepareFramePaste(app.get()),"an ambiguous copied bone is still rejected before editing");
    std::array<mdl::MorphRecord,3> morphs{};
    f.model->morphs=morphs.data();f.model->morphCount=3;
    std::strcpy(morphs[0].name,"face");std::strcpy(morphs[1].name,"unused");std::strcpy(morphs[2].name,"unused");
    mdl::MorphClipboardRecord expression{};std::strcpy(expression.name,"face");
    app->MorphClipboard()=&expression;app->ClipboardCounts().bones=0;app->ClipboardCounts().morphs=1;
    Check(PrepareFramePaste(app.get()),"unrelated duplicate morph names do not block expression paste");
    std::strcpy(expression.name,"unused");
    Check(!PrepareFramePaste(app.get()),"an ambiguous copied morph is still rejected");
    app->ClipboardCounts().morphs=0;app->MorphClipboard()=nullptr;
    app->ClipboardCounts().bones=0; app->ClipboardCounts().displays=1;
    mdl::DisplayClipboardRecord display{}; app->DisplayClipboard()=&display;
    Check(PrepareFramePaste(app.get()),"display-only paste does not require all bone names to be unique");
    app->ClipboardCounts().displays=0; app->ClipboardCounts().bones=1;
    std::strcpy(key.name,"target"); key.frame=UINT32_MAX;
    Check(!PrepareFramePaste(app.get()),"frame overflow still rejects the paste");
    app->BoneClipboard()=nullptr; app->DisplayClipboard()=nullptr;
}

int RunDisplayFramePaste() {
    __try {
        DisplayFramePaste();
        PasteDestinationNames();
        std::puts("Display frame paste regressions passed");
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::printf("FAIL: display frame paste raised exception 0x%08lX\n", GetExceptionCode());
        return 1;
    }
}
}

#include "review_editing_checks.hpp"
#include "curve_compatibility_review_checks.hpp"

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--review-editing") == 0) {ReviewEditingChecks();return 0;}
    if (argc > 1 && std::strcmp(argv[1], "--review-curves") == 0) {ReviewCurveEditingChecks();return 0;}
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--pick-cycle") == 0) {
        ViewportOverlappingBoneCycle();
        std::puts("Overlapping bone pick regressions passed");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "display-paste") == 0)
        return RunDisplayFramePaste();
    if (RunDisplayFramePaste() != 0) return 1;
    std::puts("bone_registration_tests: fresh model / right shoulder");
    FreshModelRegistration();
    std::puts("bone_registration_tests: physics selection menu / Chinese frame scopes");
    PhysicsSelectionAndLocalizedScopes();
    std::puts("bone_registration_tests: shared cursor / multiple bones / undo and redo");
    SharedCursorAndUndo();
    PoolGrowthRegistration();
    SelectedMorphFrameNavigation();
    std::puts("bone_registration_tests: foot IK register button / I and K shortcuts");
    FootIkRegistrationAndShortcuts();
    std::puts("bone_registration_tests: box gestures / collapsed groups / native register click");
    TimelineSelectionAndRegisterClicks();
    std::puts("bone_registration_tests: selection consistency / movement / coordinate commit");
    SelectedBoneMovement();
    std::puts("bone_registration_tests: viewport click pick physics lock");
    ViewportClickSelectionPhysicsLock();
    std::puts("bone_registration_tests: overlapping bone click cycle");
    ViewportOverlappingBoneCycle();
    std::puts("bone_registration_tests: viewport box select still covers ordinary bones");
    ViewportBoxSelectionStillCoversOrdinaryBones();
    std::puts("Bone registration regressions passed");
}
