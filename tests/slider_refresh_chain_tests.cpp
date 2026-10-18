// Real hidden Win32 controls and production refresh entries. Does not assess
// visible thumb flicker; it detects redundant host messages in that chain.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/ui_language.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/storage_store.hpp"
#endif
#include <CommCtrl.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace refresh_allocation {
thread_local bool reject = false, observe = false;
thread_local unsigned long long attempts = 0;
void* Allocate(std::size_t size) {
    if (observe) ++attempts;
    if (reject) throw std::bad_alloc();
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t size) { return refresh_allocation::Allocate(size); }
void* operator new[](std::size_t size) { return refresh_allocation::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace mikudancestudio;
namespace {
unsigned checks = 0;
void Check(bool okay, const char* label) {
    ++checks;
    if (!okay) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
}
struct Spy {
    HWND window = nullptr;
    unsigned setText = 0, replaceText = 0, textSelection = 0;
    unsigned resetCombo = 0, selectCombo = 0, setCheck = 0;
    unsigned trackPositions = 0, redrawTrackPositions = 0;
    LPARAM lastTrackPosition = 0;
    int failComboAddAfter = -1;
    bool failNextComboItemData = false;
    MMDApp* reenterApp = nullptr;
    UINT reenterMessage = 0;
    unsigned reentries = 0;
    bool destroyOnReset = false;
    bool replaceOnTextRead = false;
    LRESULT nestedDelete = 0, nestedInsert = 0;
    mdl::BoneRecord* renameOnTextRead = nullptr;
    bool Watch(HWND control) {
        if (!control || !SetWindowSubclass(control, Observe,
                reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
            return false;
        window = control;
        return true;
    }
    void Clear() {
        setText = replaceText = textSelection = resetCombo = selectCombo = setCheck = 0;
        trackPositions = redrawTrackPositions = 0; lastTrackPosition = 0;
    }
    bool NoWrites() const {
        return !setText && !replaceText && !textSelection && !resetCombo && !selectCombo && !setCheck && !trackPositions;
    }
    ~Spy() { if (IsWindow(window)) RemoveWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this)); }
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                     UINT_PTR, DWORD_PTR pointer) {
        auto& self = *reinterpret_cast<Spy*>(pointer);
        if (message == CB_GETLBTEXT && wp == 1 && self.renameOnTextRead) {
            strcpy_s(self.renameOnTextRead->name, "source_edited");
            self.renameOnTextRead = nullptr;
        }
        if (message == CB_GETLBTEXT && self.replaceOnTextRead) {
            self.replaceOnTextRead = false;
            wchar_t longer[512];
            for (auto& character : longer) character = L'x';
            longer[511] = L'\0';
            self.nestedDelete = SendMessageW(window, CB_DELETESTRING, wp, 0);
            self.nestedInsert = SendMessageW(window, CB_INSERTSTRING, wp, reinterpret_cast<LPARAM>(longer));
        }
        if (message == self.reenterMessage && self.reenterApp) {
            auto* app = self.reenterApp;
            self.reenterApp = nullptr;
            ++self.reentries;
            SyncAccessoryEditPanel(app);
        }
        if (message == CB_RESETCONTENT && self.destroyOnReset) {
            self.destroyOnReset = false;
            ++self.resetCombo;
            DestroyWindow(window);
            return CB_ERR;
        }
        if (message == WM_SETTEXT) ++self.setText;
        else if (message == EM_REPLACESEL) ++self.replaceText;
        else if (message == EM_SETSEL) ++self.textSelection;
        else if (message == CB_RESETCONTENT) ++self.resetCombo;
        else if (message == CB_SETCURSEL) ++self.selectCombo;
        else if (message == BM_SETCHECK) ++self.setCheck;
        else if (message == TBM_SETPOS) {
            ++self.trackPositions;
            if (wp) ++self.redrawTrackPositions;
            self.lastTrackPosition = lp;
        }
        else if (message == CB_ADDSTRING && self.failComboAddAfter >= 0) {
            if (self.failComboAddAfter-- == 0) return CB_ERRSPACE;
        } else if (message == CB_SETITEMDATA && self.failNextComboItemData) {
            self.failNextComboItemData = false;
            return CB_ERR;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
};
bool HasText(HWND control, const char* expected) {
    char actual[128]{};
    return GetWindowTextA(control, actual, sizeof(actual)) == int(std::strlen(expected)) &&
        !std::strcmp(actual, expected);
}
bool ComboText(HWND combo, int row, const wchar_t* expected) {
    wchar_t actual[128]{};
    const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, row, 0);
    return length >= 0 && length < 128 &&
        SendMessageW(combo, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(actual)) == length &&
        std::wstring(actual) == expected;
}
struct Fixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> first = std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<mdl::ModelRecord> second = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 4> firstBones{}, secondBones{};
    mdl::AccessoryRecord accessory{};
    HWND window = nullptr;
    std::array<Spy, 7> poseSpies;
    std::array<Spy, 8> accessorySpies;
    std::array<Spy, 3> checkSpies;
    Spy parentSpy, boneSpy;
    unsigned char oldPreference = gModelInfoLanguagePreference;
    Fixture() {
        gModelInfoLanguagePreference = kModelInfoLanguageAuto;
        app->state = MMDAppState{};
        window = CreateWindowExW(0, L"STATIC", L"refresh chain", WS_OVERLAPPED,
            0, 0, 640, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create hidden native refresh window");
        app->state.hwnd = window;
        app->state.optflag[0] = 1;
        app->EnglishUI() = kUiJapanese;
        app->CameraParentModel() = -1;
        app->CameraPositionX() = 1.234567f;
        app->CameraPositionY() = -2.345678f;
        app->CameraPositionZ() = 3.456789f;
        app->CameraDistance() = -12.5f;
        app->CurrentFrame() = 17;
        for (int index = 0; index < 7; ++index) {
            const HWND edit = Add(L"EDIT", 0x220 + index, WS_BORDER);
            Check(poseSpies[index].Watch(edit), "observe native pose readout");
        }
        for (int index = 0; index < 8; ++index) {
            const HWND edit = Add(L"EDIT", 478 + index, WS_BORDER);
            Check(accessorySpies[index].Watch(edit), "observe native accessory readout");
        }
        const int ids[] = {panel::kAccessoryVisibleCheckbox,
            panel::kAccessoryAddBlendCheckbox, panel::kAccessoryShadowCheckbox};
        for (int index = 0; index < 3; ++index)
            Check(checkSpies[index].Watch(Add(L"BUTTON", ids[index], BS_AUTOCHECKBOX)),
                "observe native accessory checkbox");
        const HWND parent = Add(L"COMBOBOX", panel::kMainComboGround, CBS_DROPDOWNLIST);
        const wchar_t* labels[] = {L"Ground", L"unused1", L"first", L"unused3", L"second"};
        for (const auto* label : labels)
            Check(SendMessageW(parent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)) >= 0,
                "populate real parent combo in display order");
        Check(parentSpy.Watch(parent) && boneSpy.Watch(Add(L"COMBOBOX", panel::kAttachBoneCombo,
            CBS_DROPDOWNLIST)), "observe actual accessory combos");
        first->boneCount = second->boneCount = 4;
        first->boneTable = firstBones.data(); second->boneTable = secondBones.data();
        first->comboSelIndex = 2; second->comboSelIndex = 4;
        for (unsigned bone = 0; bone < 4; ++bone) {
            sprintf_s(firstBones[bone].name, "first%u", bone);
            sprintf_s(firstBones[bone].nameEn, "english%u", bone);
            sprintf_s(secondBones[bone].name, "second%u", bone);
            firstBones[bone].type = secondBones[bone].type = mdl::BoneType::InertTip;
        }
        firstBones[0].type = secondBones[0].type = mdl::BoneType::RotateMove;
        firstBones[2].type = secondBones[2].type = mdl::BoneType::FixedAxis;
        app->ModelSlot(5) = reinterpret_cast<unsigned char*>(first.get());
        app->ModelSlot(8) = reinterpret_cast<unsigned char*>(second.get());
        accessory.parentModel = -1; accessory.parentBone = 2;
        accessory.visible = accessory.shadowEnabled = 1;
        accessory.scale = 1.234567f; accessory.opacity = .456789f;
        accessory.position[0] = .1234567f; accessory.rotation[1] = .3456789f;
        app->AccessorySlot(0) = &accessory;
        app->state.selectedObjectSlot = 0;
    }
    ~Fixture() {
        DestroyWindow(window);
        gModelInfoLanguagePreference = oldPreference;
    }
    HWND Add(const wchar_t* type, int id, DWORD style) {
        const HWND child = CreateWindowExW(0, type, L"", WS_CHILD | style,
            0, 0, 160, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        Check(child != nullptr, "create real native child control");
        return child;
    }
    HWND Item(int id) const { return GetDlgItem(window, id); }
    void Clear() {
        for (auto& spy : poseSpies) spy.Clear();
        for (auto& spy : accessorySpies) spy.Clear();
        for (auto& spy : checkSpies) spy.Clear();
        parentSpy.Clear(); boneSpy.Clear();
    }
    bool NoWrites() const {
        for (const auto& spy : poseSpies) if (!spy.NoWrites()) return false;
        for (const auto& spy : accessorySpies) if (!spy.NoWrites()) return false;
        for (const auto& spy : checkSpies) if (!spy.NoWrites()) return false;
        return parentSpy.NoWrites() && boneSpy.NoWrites();
    }
};
void OrdinaryPose() {
    Fixture f;
    PostViewRefresh(f.app.get());
    Check(HasText(f.Item(0x220), "1.23") && HasText(f.Item(0x221), "-2.35") &&
        HasText(f.Item(0x226), "12.50"), "pose refresh retains original numerical formatting");
    const float precise = f.app->CameraPositionX();
    SendMessageW(f.Item(0x220), EM_SETMODIFY, TRUE, 0);
    SendMessageW(f.Item(0x220), EM_SETSEL, 1, 3);
    f.Clear();
    for (unsigned repeat = 0; repeat < 10; ++repeat) PostViewRefresh(f.app.get());
    DWORD begin = 0, end = 0;
    SendMessageW(f.Item(0x220), EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
    Check(f.NoWrites() && begin == 1 && end == 3 && SendMessageW(f.Item(0x220), EM_GETMODIFY, 0, 0),
        "same pose readouts send no text writes and preserve edit selection and modify state");
    Check(f.app->CameraPositionX() == precise && f.app->CurrentFrame() == 17 && !f.app->SceneModified(),
        "readout refresh preserves source precision, playhead and dirty flag");
    f.app->CameraPositionY() = -4.25f; f.Clear(); PostViewRefresh(f.app.get());
    Check(f.poseSpies[1].setText == 1 && HasText(f.Item(0x221), "-4.25"),
        "changed pose lane updates actual native edit once");
    for (unsigned index = 0; index < 7; ++index) if (index != 1)
        Check(f.poseSpies[index].NoWrites(), "changed pose lane leaves unchanged siblings alone");
    const HWND draft = f.Item(0x220);
    SetFocus(draft); Check(GetFocus() == draft, "focus real pose edit for ordinary draft check");
    SetWindowTextA(draft, "12."); SendMessageW(draft, EM_SETMODIFY, TRUE, 0);
    f.app->CameraPositionX() = 9.f; f.Clear(); PostViewRefresh(f.app.get());
    Check(HasText(draft, "12.") && f.poseSpies[0].NoWrites(),
        "ordinary focused unfinished pose draft survives refresh");
    SetFocus(f.window); SendMessageW(draft, EM_SETMODIFY, FALSE, 0);
    f.Clear(); PostViewRefresh(f.app.get());
    Check(HasText(draft, "9.00") && f.poseSpies[0].setText == 1,
        "ordinary committed pose readout still refreshes after draft ends");
}
void ShadowRangeReadout() {
    Fixture f;
    const HWND slider = f.Add(TRACKBAR_CLASSW, panel::kSelfShadowRangeSlider, 0);
    const HWND edit = f.Add(L"EDIT", panel::kSelfShadowRangeEdit, WS_BORDER);
    const int checkIds[] = {panel::kEditOffCheckbox, panel::kEditMode1Checkbox, panel::kEditMode2Checkbox};
    std::array<Spy, 3> checkSpies;
    for (unsigned index = 0; index < checkSpies.size(); ++index)
        Check(checkSpies[index].Watch(f.Add(L"BUTTON", checkIds[index], BS_AUTOCHECKBOX)),
            "observe real shadow mode checkbox");
    Spy sliderSpy, editSpy;
    Check(sliderSpy.Watch(slider) && editSpy.Watch(edit), "observe real native self-shadow slider and number");
    SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 9999));
    SendMessageW(slider, TBM_SETPOS, TRUE, 5000);
    f.app->SelfShadowMode() = 2;
    struct Sample { float author; int displayed; const char* text; };
    const Sample samples[] = {{-.125f, 9999, "22500"}, {.25f, 0, "-15000"},
        {.0625f, 3750, "3750"}, {.03125f, 6875, "6875"}};
    for (const auto& sample : samples) {
        f.app->ShadowDistance() = sample.author;
        sliderSpy.Clear(); editSpy.Clear();
        for (auto& spy : checkSpies) spy.Clear();
        SyncSelfShadowControls(f.app.get());
        Check(SendMessageW(slider, TBM_GETPOS, 0, 0) == sample.displayed &&
            sliderSpy.trackPositions == 1 && sliderSpy.redrawTrackPositions == 1 &&
            sliderSpy.lastTrackPosition == sample.displayed && HasText(edit, sample.text),
            "changed shadow range updates native clamped thumb once and retains original number readout");
        Check(f.app->ShadowDistance() == sample.author && f.app->SelfShadowMode() == 2 &&
            !f.app->SceneModified() && f.app->CurrentFrame() == 17,
            "shadow readout clamp preserves exact author value, mode, dirty state and playhead");
        sliderSpy.Clear(); editSpy.Clear();
        for (auto& spy : checkSpies) spy.Clear();
        for (unsigned repeat = 0; repeat < 10; ++repeat) SyncSelfShadowControls(f.app.get());
        bool checksQuiet = true;
        for (const auto& spy : checkSpies) checksQuiet = checksQuiet && spy.NoWrites();
        Check(sliderSpy.NoWrites() && editSpy.NoWrites() && checksQuiet &&
            SendMessageW(slider, TBM_GETPOS, 0, 0) == sample.displayed && f.app->ShadowDistance() == sample.author,
            "repeated out-of-range or legal shadow readout sends no thumb, text or checkbox writes");
    }
}
void AccessoryParentsAndContents() {
    Fixture f;
    SyncAccessoryEditPanel(f.app.get());
    const auto before = f.accessory;
    Check(SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 0 &&
        SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCOUNT, 0, 0) == 0,
        "negative native ground parent maps to combo row zero and empty bones");
    f.Clear(); for (unsigned repeat = 0; repeat < 10; ++repeat) SyncAccessoryEditPanel(f.app.get());
    Check(f.NoWrites() && !std::memcmp(&before, &f.accessory, sizeof(before)),
        "stable ground accessory refresh does not reset combo, checkbox, selection or edit");
    f.accessory.parentModel = 5; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    const HWND parent = f.Item(panel::kMainComboGround), bones = f.Item(panel::kAttachBoneCombo);
    Check(SendMessageW(parent, CB_GETCURSEL, 0, 0) == 2 && f.parentSpy.selectCombo == 1 &&
        f.boneSpy.resetCombo == 1 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == 1 &&
        SendMessageW(bones, CB_GETCOUNT, 0, 0) == 2 &&
        SendMessageW(bones, CB_GETITEMDATA, 1, 0) == 2 && ComboText(bones, 1, L"first2"),
        "actual parent change resolves slot5 to row2 and selects fixed-axis bone by item identity");
    f.Clear(); for (unsigned repeat = 0; repeat < 10; ++repeat) SyncAccessoryEditPanel(f.app.get());
    Check(f.NoWrites(), "native parent slot different from combo row does not repeatedly rebuild bones");
    std::strcpy(f.firstBones[2].name, "renamed"); f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && !f.parentSpy.selectCombo && ComboText(bones, 1, L"renamed"),
        "same parent row with renamed bone refreshes actual contents once");
    f.Clear(); SyncAccessoryEditPanel(f.app.get()); Check(f.NoWrites(), "renamed list stabilizes on next refresh");
    f.app->EnglishUI() = kUiEnglish; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(bones, 1, L"english2"),
        "same parent row with changed display language refreshes actual names");
    f.app->EnglishUI() = kUiJapanese;
    f.firstBones[1].type = mdl::BoneType::Move; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && SendMessageW(bones, CB_GETCOUNT, 0, 0) == 3 &&
        SendMessageW(bones, CB_GETCURSEL, 0, 0) == 2 && SendMessageW(bones, CB_GETITEMDATA, 2, 0) == 2,
        "bone visibility change rebuilds list without confusing bone index and row");
    SendMessageW(bones, CB_SETITEMDATA, 0, 99); f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && SendMessageW(bones, CB_GETITEMDATA, 0, 0) == 0,
        "same labels with stale native bone identity are repaired");
    f.second->comboSelIndex = 2;
    f.app->ModelSlot(5) = reinterpret_cast<unsigned char*>(f.second.get());
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && !f.parentSpy.selectCombo && ComboText(bones, 1, L"second2"),
        "replacement model in same slot and display row refreshes its bone list");
    f.second->comboSelIndex = 4; f.accessory.parentModel = 8; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && f.parentSpy.selectCombo == 1 &&
        SendMessageW(parent, CB_GETCURSEL, 0, 0) == 4, "different actual parent updates row even with identical bone labels");
    f.accessory.parentBone = 0; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(!f.boneSpy.resetCombo && f.boneSpy.selectCombo == 1 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == 0,
        "only parent bone change selects one row without rebuilding list");
    f.accessory.visible = 0; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.checkSpies[0].setCheck == 1 && !f.checkSpies[1].setCheck && !f.checkSpies[2].setCheck &&
        SendMessageW(f.Item(panel::kAccessoryVisibleCheckbox), BM_GETCHECK, 0, 0) == BST_UNCHECKED,
        "real checkbox state change updates only changed native checkbox");
    f.accessory.parentModel = -1; f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && f.parentSpy.selectCombo == 1 &&
        SendMessageW(bones, CB_GETCOUNT, 0, 0) == 0 && SendMessageW(parent, CB_GETCURSEL, 0, 0) == 0,
        "detach clears previous model bones once and selects ground row");
    f.Clear(); SyncAccessoryEditPanel(f.app.get()); Check(f.NoWrites(), "detached accessory remains stable after real parent change");
    Check(f.app->CurrentFrame() == 17 && !f.app->SceneModified() && f.accessory.scale == 1.234567f &&
        f.accessory.position[0] == .1234567f && f.accessory.opacity == .456789f,
        "accessory panel refresh preserves native precision, playhead and dirty state");
}
void StableParent(Fixture& f) {
    strcpy_s(f.firstBones[0].name, "stable_parent_zero");
    strcpy_s(f.firstBones[2].name, "stable_parent_two");
    f.accessory.parentModel = 5;
    SyncAccessoryEditPanel(f.app.get());
    Check(SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 2 &&
        SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCURSEL, 0, 0) == 1 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"stable_parent_two"),
        "prepare a stable model parent with heap-sized real bone labels");
}
void SpoilAccessoryReadout(Fixture& f) {
    SetWindowTextA(f.Item(478), "999.0");
    SendMessageW(f.Item(478), EM_SETMODIFY, FALSE, 0);
    SendMessageW(f.Item(panel::kAccessoryVisibleCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
    f.Clear();
}
void CheckRestoredReadout(Fixture& f) {
    Check(HasText(f.Item(478), "0.1235") &&
        SendMessageW(f.Item(panel::kAccessoryVisibleCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED,
        "failed label preparation still restores committed accessory number and checkbox");
    Check(!f.parentSpy.selectCombo && !f.boneSpy.resetCombo && !f.boneSpy.selectCombo &&
        SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 2 &&
        SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCURSEL, 0, 0) == 1 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"stable_parent_two"),
        "stable-parent allocation failure preserves existing list and both combo selections");
}
void AllocationFailureReadout(bool slotEqualsRow) {
    Fixture f; StableParent(f);
    if (slotEqualsRow) {
        f.app->ModelSlot(2) = reinterpret_cast<unsigned char*>(f.first.get());
        f.accessory.parentModel = 2;
        SyncAccessoryEditPanel(f.app.get());
    }
    SpoilAccessoryReadout(f);
    const auto before = f.accessory;
    bool escaped = false;
    refresh_allocation::reject = true;
    try { SyncAccessoryEditPanel(f.app.get()); } catch (...) { escaped = true; }
    refresh_allocation::reject = false;
    Check(!escaped, "actual stable-parent SyncAccessoryEditPanel does not escape allocation failure");
    CheckRestoredReadout(f);
    Check(!std::memcmp(&before, &f.accessory, sizeof(before)) && !f.app->SceneModified() && f.app->CurrentFrame() == 17,
        "allocation-failed readout preserves authoring, dirty state and playhead");
    f.accessory.parentModel = 8; f.Clear();
    refresh_allocation::reject = true;
    try { SyncAccessoryEditPanel(f.app.get()); } catch (...) { escaped = true; }
    refresh_allocation::reject = false;
    Check(!escaped && !f.parentSpy.selectCombo && !f.boneSpy.resetCombo && !f.boneSpy.selectCombo &&
        SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 2 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"stable_parent_two"),
        "new-parent C++ preparation failure publishes no combo or list mutation");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.parentSpy.selectCombo == 1 && f.boneSpy.resetCombo == 1 &&
        SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 4 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"second2"),
        "next successful refresh retries privately prepared parent list");
}
void NativeListPublicationFailure() {
    Fixture f; StableParent(f);
    const HWND parent = f.Item(panel::kMainComboGround), bones = f.Item(panel::kAttachBoneCombo);
    f.accessory.parentModel = 8; SpoilAccessoryReadout(f);
    f.boneSpy.failComboAddAfter = 1;
    SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && !f.boneSpy.selectCombo && !f.parentSpy.selectCombo &&
        SendMessageW(parent, CB_GETCURSEL, 0, 0) == 2 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == -1,
        "native CB_ADDSTRING error stops parent and bone selection after partial publication");
    Check(HasText(f.Item(478), "0.1235") &&
        SendMessageW(f.Item(panel::kAccessoryVisibleCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED,
        "native list failure still restores other accessory readouts");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && f.parentSpy.selectCombo == 1 &&
        SendMessageW(parent, CB_GETCURSEL, 0, 0) == 4 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == 1 &&
        ComboText(bones, 1, L"second2"), "next refresh retries native partial-list failure and selects correct identity");
    f.accessory.parentModel = 5; f.Clear(); f.boneSpy.failNextComboItemData = true;
    SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && !f.boneSpy.selectCombo && !f.parentSpy.selectCombo &&
        SendMessageW(parent, CB_GETCURSEL, 0, 0) == 4 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == -1,
        "native CB_SETITEMDATA error also stops parent and bone selection");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(SendMessageW(parent, CB_GETCURSEL, 0, 0) == 2 && SendMessageW(bones, CB_GETCURSEL, 0, 0) == 1 &&
        ComboText(bones, 1, L"stable_parent_two"), "next refresh repairs item-data publication failure");
}
void CachedSourceAndNativeMutations() {
    Fixture f; StableParent(f);
    const HWND combo = f.Item(panel::kAttachBoneCombo);
    const auto before = f.accessory;
    wchar_t nativeName[32] = L"wide native before";
    wchar_t englishName[32] = L"wide english before";
    f.firstBones[2].jpText = nativeName;
    f.firstBones[2].enText = englishName;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 1, nativeName),
        "wide name source replaces legacy label without a model revision");
    wcscpy_s(nativeName, L"wide native after");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 1, nativeName),
        "in-place wide name edit invalidates the stable cached list");
    f.app->EnglishUI() = kUiEnglish;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 1, englishName),
        "cached wide labels follow the actual requested display language");
    wcscpy_s(englishName, L"wide english after");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 1, englishName),
        "in-place English wide source change is observed with identical source pointers");
    SendMessageW(combo, CB_DELETESTRING, 0, 0);
    Check(SendMessageW(combo, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(L"foreign text")) == 0 &&
        SendMessageW(combo, CB_SETITEMDATA, 0, 0) != CB_ERR,
        "replace a native cached row without changing the final row count");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 0, L"english0") &&
        SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1,
        "external same-count row replacement cannot retain a stale cached label or selection");
    SendMessageW(combo, CB_SETITEMDATA, 1, 0);
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && SendMessageW(combo, CB_GETITEMDATA, 1, 0) == 2,
        "external cached item identity mutation is repaired");
    const auto oldTranslation = gModelNameAutoTranslation;
    gModelNameAutoTranslation = !oldTranslation;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(!f.boneSpy.resetCombo && ComboText(combo, 1, englishName),
        "translation preference changes revalidate unchanged authoritative labels without rewriting them");
    gModelNameAutoTranslation = oldTranslation;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(!f.boneSpy.resetCombo && ComboText(combo, 1, englishName),
        "restoring translation preference also preserves an identical displayed list");
    f.Clear(); refresh_allocation::attempts = 0; refresh_allocation::observe = true;
    for (unsigned i = 0; i < 10; ++i) SyncAccessoryEditPanel(f.app.get());
    refresh_allocation::observe = false;
    Check(!refresh_allocation::attempts && f.NoWrites() &&
        !std::memcmp(&before, &f.accessory, sizeof(before)) && !f.app->SceneModified(),
        "unchanged cached wide sources allocate no C++ memory and preserve native authoring");
}
void CachedListReentryAndLifetime() {
    Fixture f; StableParent(f);
    const HWND combo = f.Item(panel::kAttachBoneCombo);
    SendMessageW(combo, CB_SETITEMDATA, 0, 0);
    f.boneSpy.replaceOnTextRead = true;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.nestedDelete == CB_ERR && f.boneSpy.nestedInsert == CB_ERR &&
        f.boneSpy.resetCombo == 1 && ComboText(combo, 1, L"stable_parent_two") &&
        SendMessageW(combo, CB_GETCOUNT, 0, 0) == 2,
        "bounded native text read rejects synchronous row growth then retries complete publication");
    SendMessageW(combo, CB_SETITEMDATA, 0, 0);
    f.boneSpy.renameOnTextRead = &f.firstBones[0];
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && ComboText(combo, 0, L"source_edited") &&
        ComboText(combo, 1, L"stable_parent_two"),
        "source change during native verification cannot publish an already stale readiness result");
    f.accessory.parentModel = 8;
    f.boneSpy.reenterApp = f.app.get(); f.boneSpy.reenterMessage = CB_ADDSTRING;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.reentries == 1 && f.boneSpy.resetCombo == 1 &&
        SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 4 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"second2"),
        "synchronous native list reentry never publishes or selects a partially constructed list");
    f.accessory.parentModel = 5; f.boneSpy.destroyOnReset = true;
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(!f.Item(panel::kAttachBoneCombo) && !f.parentSpy.selectCombo && !f.boneSpy.selectCombo &&
        SendMessageW(f.Item(panel::kMainComboGround), CB_GETCURSEL, 0, 0) == 4,
        "synchronous combo destruction cancels publication and retains the previous parent selection");
    Check(f.boneSpy.Watch(f.Add(L"COMBOBOX", panel::kAttachBoneCombo, CBS_DROPDOWNLIST)),
        "create a replacement native window after retiring its per-window cache");
    f.Clear(); SyncAccessoryEditPanel(f.app.get());
    Check(f.boneSpy.resetCombo == 1 && f.parentSpy.selectCombo == 1 &&
        ComboText(f.Item(panel::kAttachBoneCombo), 1, L"stable_parent_two") &&
        SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCURSEL, 0, 0) == 1,
        "replacement window receives a fresh complete list and source identity");
}
void LargeStableParentTiming() {
    Fixture f;
    std::vector<mdl::BoneRecord> bones(4000);
    for (unsigned index = 0; index < bones.size(); ++index) {
        sprintf_s(bones[index].name, "benchmark_bone%04u", index);
        bones[index].type = mdl::BoneType::RotateMove;
    }
    f.first->boneTable = bones.data(); f.first->boneCount = static_cast<std::uint32_t>(bones.size());
    f.accessory.parentModel = 5; f.accessory.parentBone = 3999;
    SyncAccessoryEditPanel(f.app.get());
    Check(SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCOUNT, 0, 0) == 4000 &&
        SendMessageW(f.Item(panel::kAttachBoneCombo), CB_GETCURSEL, 0, 0) == 3999,
        "4000-bone timing fixture has complete actual native list and correct selected identity");
    for (unsigned warmup = 0; warmup < 3; ++warmup) SyncAccessoryEditPanel(f.app.get());
    f.Clear(); LARGE_INTEGER frequency{}, begin{}, end{};
    Check(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 && QueryPerformanceCounter(&begin),
        "query high-resolution timer for stable large-parent report");
    refresh_allocation::attempts = 0; refresh_allocation::observe = true;
    for (unsigned repeat = 0; repeat < 100; ++repeat) SyncAccessoryEditPanel(f.app.get());
    refresh_allocation::observe = false;
    QueryPerformanceCounter(&end);
    const auto allocations = refresh_allocation::attempts;
    Check(allocations == 0, "warm stable 4000-bone refresh performs no C++ allocation");
    Check(f.NoWrites() && !f.app->SceneModified() && f.app->CurrentFrame() == 17,
        "4000-bone stable refresh sends no list, checkbox, selection or edit mutation");
    const double milliseconds = double(end.QuadPart - begin.QuadPart) * 1000.0 / double(frequency.QuadPart);
    std::printf("MEASURE stable accessory parent: bones=4000 refreshes=100 total_ms=%.3f per_refresh_ms=%.6f cpp_allocation_attempts=%llu (no machine-specific performance gate)\n",
        milliseconds, milliseconds / 100.0, allocations);
}
#if !MMD_ORIGINAL_PROFILE
void BoundAllocationFailureReadout() {
    Fixture f; plugins::StorageStore storage; plugins::SharedHistory history(storage);
    plugins::NativePoseTestBinding binding(*f.app, history);
    StableParent(f); SpoilAccessoryReadout(f);
    const auto before = f.accessory;
    refresh_allocation::reject = true;
    plugins::RefreshBoundGlobalReadout(*f.app);
    refresh_allocation::reject = false;
    CheckRestoredReadout(f);
    Check(!std::memcmp(&before, &f.accessory, sizeof(before)) && !history.UndoCount() && !history.RedoCount() &&
        !f.app->SceneModified() && f.app->CurrentFrame() == 17,
        "bound rejected-edit readout restores native controls without authoring, journal or dirty writes");
}
void BoundDrafts() {
    Fixture f; plugins::StorageStore storage; plugins::SharedHistory history(storage);
    plugins::NativePoseTestBinding binding(*f.app, history);
    PostViewRefresh(f.app.get()); SyncAccessoryEditPanel(f.app.get());
    const HWND pose = f.Item(0x220), accessory = f.Item(478);
    SetWindowTextA(pose, "pose draft."); SendMessageW(pose, EM_SETMODIFY, TRUE, 0);
    SetWindowTextA(accessory, "accessory draft."); SendMessageW(accessory, EM_SETMODIFY, TRUE, 0);
    SetFocus(f.window); f.app->CameraPositionX() = 7.f; f.accessory.position[0] = 8.f; f.Clear();
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        PostViewRefresh(f.app.get()); SyncAccessoryEditPanel(f.app.get());
    }
    Check(f.NoWrites() && HasText(pose, "pose draft.") && HasText(accessory, "accessory draft.") &&
        SendMessageW(pose, EM_GETMODIFY, 0, 0) && SendMessageW(accessory, EM_GETMODIFY, 0, 0),
        "bound delayed refresh preserves unfocused unregistered pose and accessory drafts");
    SendMessageW(pose, EM_SETMODIFY, FALSE, 0); SendMessageW(accessory, EM_SETMODIFY, FALSE, 0);
    f.Clear(); PostViewRefresh(f.app.get()); SyncAccessoryEditPanel(f.app.get());
    Check(HasText(pose, "7.00") && HasText(accessory, "8.0000") && f.poseSpies[0].setText == 1 &&
        f.accessorySpies[0].replaceText == 1 && !SendMessageW(accessory, EM_GETMODIFY, 0, 0),
        "bound committed readouts refresh once and host accessory writes are not new drafts");
    Check(!history.UndoCount() && !history.RedoCount() && !f.app->SceneModified() && f.app->CurrentFrame() == 17,
        "bound refresh writes no common history, project dirty state or playhead");
}
#endif
}
int main() {
    // ICC_STANDARD_CLASSES requires common-controls v6; these fixtures have
    // no app manifest and also run against the original profile's v5 loader.
    // EDIT/BUTTON/COMBOBOX are supplied by USER32. Initialize the same v5-safe
    // class group as the retained native readout control regression.
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    Check(InitCommonControlsEx(&controls) != FALSE, "initialize native common controls");
    OrdinaryPose(); ShadowRangeReadout(); AccessoryParentsAndContents();
    AllocationFailureReadout(false); AllocationFailureReadout(true);
    NativeListPublicationFailure(); CachedSourceAndNativeMutations();
    CachedListReentryAndLifetime(); LargeStableParentTiming();
#if !MMD_ORIGINAL_PROFILE
    BoundDrafts(); BoundAllocationFailureReadout();
#endif
    std::printf("PASS production slider refresh chain: %u checks (visual flicker not assessed)\n", checks);
    return 0;
}
