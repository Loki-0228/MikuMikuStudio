// Hidden Win32 controls exercise the production refresh helper. No renderer,
// application window procedure, model file, device, or message loop is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/scene_history_ui.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace mikumikustudio;
namespace history_ui = mikumikustudio::scene_history_ui;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
constexpr std::array<int, 7> comboIds{{436, 471, 449, 474, 450, 475, 434}};
constexpr wchar_t modelAName[] = L"模型A_髪_\U0001f600";
constexpr wchar_t modelBName[] = L"模型B_洛天依";
constexpr wchar_t accessoryAName[] = L"附件A_髪_\U0001f600";
constexpr wchar_t accessoryBName[] = L"附件B_中文";
struct Observer {
    unsigned commands = 0, resets = 0, inserts = 0, dataWrites = 0, selectionWrites = 0;
    int failInsertControl = 0, failInsertCountdown = 0, failReadControl = 0;
    LRESULT failure = CB_ERRSPACE;
    unsigned Writes() const { return resets + inserts + dataWrites + selectionWrites; }
    void Clear() { commands = resets = inserts = dataWrites = selectionWrites = 0; }
};
LRESULT CALLBACK ObserveOwner(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    if (message == WM_COMMAND) ++reinterpret_cast<Observer*>(data)->commands;
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK ObserveCombo(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& observer = *reinterpret_cast<Observer*>(data);
    if (message == CB_RESETCONTENT) ++observer.resets;
    if (message == CB_INSERTSTRING) {
        ++observer.inserts;
        if (GetDlgCtrlID(window) == observer.failInsertControl && observer.failInsertCountdown > 0 &&
            --observer.failInsertCountdown == 0) return observer.failure;
    }
    if (message == CB_SETITEMDATA) ++observer.dataWrites;
    if (message == CB_SETCURSEL) ++observer.selectionWrites;
    if (message == CB_GETCOUNT && GetDlgCtrlID(window) == observer.failReadControl) {
        observer.failReadControl = 0;
        return CB_ERR;
    }
    return DefSubclassProc(window, message, wp, lp);
}
std::vector<unsigned char> Bytes(const void* object, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(object);
    return {bytes, bytes + size};
}
struct Fixture {
    // Large legacy records always live on the heap, including snapshot copies.
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<mdl::ModelRecord> modelA = std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<mdl::ModelRecord> modelB = std::make_unique<mdl::ModelRecord>();
    std::unique_ptr<mdl::AccessoryRecord> accessoryA = std::make_unique<mdl::AccessoryRecord>();
    std::unique_ptr<mdl::AccessoryRecord> accessoryB = std::make_unique<mdl::AccessoryRecord>();
    std::array<mdl::BoneRecord, 4> bonesA{}, bonesB{};
    Observer observed;
    HWND window = nullptr;

    Fixture() {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        window = CreateWindowExW(0, L"STATIC", L"MMS scene history UI test", WS_OVERLAPPED,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create hidden owner");
        Check(SetWindowSubclass(window, ObserveOwner, 1, reinterpret_cast<DWORD_PTR>(&observed)) != FALSE,
            "observe owner notifications");
        for (int id : comboIds) {
            const auto combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
                0, 0, 240, 120, window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Check(combo != nullptr && IsWindowUnicode(combo), "create hidden Unicode combo");
            Check(SetWindowSubclass(combo, ObserveCombo, 1, reinterpret_cast<DWORD_PTR>(&observed)) != FALSE,
                "observe combo writes and injected failures");
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"untouched scope"));
            SendMessageW(combo, CB_SETITEMDATA, 0, 77);
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
        }
        for (int id : {panel::kUndoButton, panel::kRedoButton})
            Check(CreateWindowExW(0, L"BUTTON", L"", WS_CHILD, 0, 0, 40, 20, window,
                reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr) != nullptr, "create history button");
        app->Hwnd() = window;
        app->EnglishUI() = kUiChinese;
        app->ModelSlot(2) = Raw(modelA.get());app->ModelSlot(9) = Raw(modelB.get());
        app->AccessorySlot(3) = accessoryA.get();app->AccessorySlot(8) = accessoryB.get();
        modelA->comboSelIndex = 2;modelB->comboSelIndex = 1;
        modelA->comboSelIndex2 = 1;modelB->comboSelIndex2 = 2; // Render order is independent.
        modelA->pmxTextBuffers[0] = const_cast<wchar_t*>(modelAName);
        modelB->pmxTextBuffers[0] = const_cast<wchar_t*>(modelBName);
        modelA->pmxTextBuffers[1] = const_cast<wchar_t*>(L"Model A");
        modelB->pmxTextBuffers[1] = const_cast<wchar_t*>(L"Model B");
        modelA->boneCount = modelB->boneCount = 4;
        modelA->boneTable = bonesA.data();modelB->boneTable = bonesB.data();
        for (auto* bones : {&bonesA, &bonesB}) {
            (*bones)[0].jpText = (*bones)[2].jpText = const_cast<wchar_t*>(L"同名_髪");
            (*bones)[1].type = mdl::BoneType::InertTip;
            (*bones)[2].type = mdl::BoneType::FixedAxis;
            (*bones)[3].type = mdl::BoneType::CoRotate;
            for (unsigned i = 0; i < bones->size(); ++i) {
                (*bones)[i].position[0] = 1.25f + i;
                (*bones)[i].trans[1] = 3.5f + i;
                (*bones)[i].rotQuat[2] = 0.125f * i;
                (*bones)[i].matWorld[12] = -10.0f - i;
                (*bones)[i].selState = int(i % 2);
            }
        }
        for (auto* model : {modelA.get(), modelB.get()}) {
            model->lightDir[0] = 7.25f;model->lightDir[1] = -3.5f;model->lightDir[2] = 15.0f;
            model->legIkXOffset = 2.75f;
            for (auto& selected : model->selectedMorphs) selected = 19;
        }
        accessoryA->order = 1;accessoryB->order = 0;
        Check(text_encoding::EncodeDisplay(accessoryA->name, sizeof accessoryA->name, accessoryAName), "encode accessory A name");
        Check(text_encoding::EncodeDisplay(accessoryB->name, sizeof accessoryB->name, accessoryBName), "encode accessory B name");
        for (auto* accessory : {accessoryA.get(), accessoryB.get()}) {
            accessory->parentModel = 9;accessory->parentBone = 2;
            accessory->position[0] = 21.0f;accessory->position[1] = -4.5f;
            accessory->rotation[1] = 0.75f;accessory->scale = 1.625f;
            accessory->opacity = 0.25f;accessory->rowSelected = 1;
        }
        app->SetSelectedModelSlot(2);app->SelectedAccessorySlot() = 3;
        app->CameraMode() = 0;app->MainModelComboSelection() = 2;
        app->EditMode() = ViewportEditMode::Scale;
        app->CameraParentModel() = 9;app->CameraParentBone() = 2;
        app->CameraDistance() = -123.0f;app->ViewOffsetX() = 17.0f;app->ViewOffsetY() = -9.0f;
        for (int i = 0; i < 3; ++i) {
            app->CameraPositionAxis(i) = 7.5f + i;
            app->CameraRotation()[i] = 0.2f + i;
            app->LightDirection()[i] = 0.3f + i;
        }
        app->CurrentFrame() = 137;app->SceneModified() = 1;app->PhysicsResetPending() = 0;
        app->GlobalTrackSelected(GlobalTimelineTrack::Camera) = 1;
        app->GlobalTrackSelected(GlobalTimelineTrack::Light) = 1;
        app->GlobalTrackSelected(GlobalTimelineTrack::Gravity) = 1;
        observed.Clear();
    }
    ~Fixture() { if (window) DestroyWindow(window); }
    static unsigned char* Raw(mdl::ModelRecord* model) { return reinterpret_cast<unsigned char*>(model); }
    HWND Control(int id) const { return GetDlgItem(window, id); }
    LRESULT Count(int id) const { return SendMessageW(Control(id), CB_GETCOUNT, 0, 0); }
    LRESULT Selected(int id) const { return SendMessageW(Control(id), CB_GETCURSEL, 0, 0); }
    LRESULT Data(int id, int row) const { return SendMessageW(Control(id), CB_GETITEMDATA, row, 0); }
    std::wstring Text(int id, int row) const {
        const auto length = SendMessageW(Control(id), CB_GETLBTEXTLEN, row, 0);
        Check(length >= 0, "read combo text length");
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        Check(SendMessageW(Control(id), CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(text.data())) == length, "read combo text");
        text.resize(static_cast<std::size_t>(length));return text;
    }
    void CheckPmmCounts() const {
        int models = 0, accessories = 0;
        for (int slot = 0; slot < kModelSlotCount; ++slot) models += app->ModelSlot(slot) != nullptr;
        for (int slot = 0; slot < 255; ++slot) accessories += app->AccessorySlot(slot) != nullptr;
        Check(Count(436) - 1 == models && Count(471) == accessories, "PMM header counts match occupied native slots");
        for (int slot = 0; slot < 255; ++slot)
            if (const auto* accessory = app->AccessorySlot(slot))
                Check(Text(471, accessory->order) == text_encoding::Display(accessory->name), "PMM accessory labels match native serialized order");
    }
};
struct AuthoringSnapshot {
    std::unique_ptr<MMDAppState> state = std::make_unique<MMDAppState>();
    std::vector<unsigned char> modelA, modelB, accessoryA, accessoryB, bonesA, bonesB;
    explicit AuthoringSnapshot(const Fixture& f)
        : modelA(Bytes(f.modelA.get(), sizeof(*f.modelA))), modelB(Bytes(f.modelB.get(), sizeof(*f.modelB))),
          accessoryA(Bytes(f.accessoryA.get(), sizeof(*f.accessoryA))), accessoryB(Bytes(f.accessoryB.get(), sizeof(*f.accessoryB))),
          bonesA(Bytes(f.bonesA.data(), sizeof f.bonesA)), bonesB(Bytes(f.bonesB.data(), sizeof f.bonesB)) {
        std::memcpy(state.get(), &f.app->state, sizeof(*state));
    }
    void Verify(const Fixture& f, bool allowSelection = true) {
        if (allowSelection) {
            state->slotIdx = f.app->state.slotIdx;
            state->selectedObjectSlot = f.app->state.selectedObjectSlot;
            state->mainModelComboSelection = f.app->state.mainModelComboSelection;
            state->optflag[0] = f.app->state.optflag[0];
            state->editMode = f.app->state.editMode;
        }
        Check(std::memcmp(state.get(), &f.app->state, sizeof(*state)) == 0,
            "refresh preserves all application data except documented selection and mode fields");
        Check(modelA == Bytes(f.modelA.get(), sizeof(*f.modelA)) && modelB == Bytes(f.modelB.get(), sizeof(*f.modelB)),
            "refresh does not alter model state or independent render order");
        Check(accessoryA == Bytes(f.accessoryA.get(), sizeof(*f.accessoryA)) && accessoryB == Bytes(f.accessoryB.get(), sizeof(*f.accessoryB)),
            "refresh does not alter accessory pose, bindings, or timeline row flags");
        Check(bonesA == Bytes(f.bonesA.data(), sizeof f.bonesA) && bonesB == Bytes(f.bonesB.data(), sizeof f.bonesB),
            "refresh preserves live bone poses and selection bytes");
        Check(f.observed.commands == 0, "refresh emits no WM_COMMAND notifications");
    }
};
struct UiSnapshot {
    std::array<std::vector<std::wstring>, comboIds.size()> text;
    std::array<std::vector<LRESULT>, comboIds.size()> data;
    std::array<LRESULT, comboIds.size()> selected{};
    bool undo, redo;
    explicit UiSnapshot(const Fixture& f)
        : undo(IsWindowEnabled(f.Control(400)) != FALSE), redo(IsWindowEnabled(f.Control(401)) != FALSE) {
        for (std::size_t i = 0; i < comboIds.size(); ++i) {
            const auto id = comboIds[i];selected[i] = f.Selected(id);
            for (int row = 0; row < f.Count(id); ++row) {
                text[i].push_back(f.Text(id, row));data[i].push_back(f.Data(id, row));
            }
        }
    }
    void Verify(const Fixture& f) const {
        const UiSnapshot current(f);
        Check(text == current.text && data == current.data && selected == current.selected && undo == current.undo && redo == current.redo,
            "rejected refresh leaves every combo and history button unchanged");
    }
};
void Refresh(Fixture& f, const history_ui::Selection& selection, bool undo = true, bool redo = false) {
    Check(history_ui::RefreshObjectLists(*f.app, selection, undo, redo) == history_ui::Result::Ok, "refresh succeeds");
}
void OrderingAndParents() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);AuthoringSnapshot before(f);
    Refresh(f, selection);before.Verify(f);
    for (int id : {436, 449, 474}) {
        Check(f.Count(id) == 3 && f.Text(id, 1) == modelBName && f.Text(id, 2) == modelAName,
            "model combos use native order rather than slot or render order and preserve Unicode");
    }
    Check(f.Selected(436) == 2 && f.Selected(471) == 1 && f.Text(471, 0) == accessoryBName && f.Text(471, 1) == accessoryAName,
        "object selections follow their pointers and accessory order");
    Check(f.Selected(449) == 1 && f.Selected(474) == 1, "parent model slot 9 resolves to combo row 1");
    for (int id : {450, 475})
        Check(f.Count(id) == 2 && f.Data(id, 0) == 0 && f.Data(id, 1) == 2 && f.Selected(id) == 1 && f.Text(id, 0) == f.Text(id, 1),
            "parent bone itemdata preserves logical index across filtered and duplicate names");
    Check(f.Count(434) == 1 && f.Text(434, 0) == L"untouched scope" && f.Data(434, 0) == 77,
        "model-mode scope remains caller owned");
    Check(IsWindowEnabled(f.Control(400)) && !IsWindowEnabled(f.Control(401)), "history buttons use supplied availability");
    f.CheckPmmCounts();f.observed.Clear();AuthoringSnapshot repeated(f);
    Refresh(f, selection);repeated.Verify(f, false);
    Check(f.observed.Writes() == 0, "identical refresh performs no combo rebuild or selection write");
}
void DeletionFallbacks() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);Refresh(f, selection);
    // The caller has already published removal and adjusted native bindings.
    // The fixture owners remain alive, like retained history resources.
    f.app->ModelSlot(2) = nullptr;f.app->AccessorySlot(3) = nullptr;
    AuthoringSnapshot before(f);f.observed.Clear();Refresh(f, selection, true, true);before.Verify(f);
    Check(f.app->CameraMode() == 1 && f.app->MainModelComboSelection() == 0 && f.Selected(436) == 0 &&
        f.app->EditMode() == ViewportEditMode::None, "deleted selected model falls back to camera without pose evaluation");
    Check(f.app->SelectedAccessorySlot() == 8 && f.Selected(471) == 0, "deleted selected accessory falls back to first native order");
    Check(f.Count(434) == 5 && IsWindowEnabled(f.Control(400)) && IsWindowEnabled(f.Control(401)),
        "camera fallback builds global scopes and keeps shared undo and redo available");
    f.CheckPmmCounts();
}
void PreserveSelectionAfterEarlierRemoval() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);Refresh(f, selection);
    f.app->ModelSlot(9) = nullptr;f.modelA->comboSelIndex = 1;
    f.app->CameraParentModel() = -1;f.accessoryA->parentModel = -1;
    f.app->AccessorySlot(8) = nullptr;f.accessoryA->order = 0;
    AuthoringSnapshot before(f);f.observed.Clear();Refresh(f, selection);before.Verify(f);
    Check(f.app->SelectedModelSlot() == 2 && f.app->SelectedAccessorySlot() == 3 && f.Selected(436) == 1 && f.Selected(471) == 0,
        "earlier row removal preserves selected objects while their row numbers change");
    Check(f.app->CameraMode() == 0 && f.app->EditMode() == ViewportEditMode::Scale, "unchanged model selection retains edit mode");
    Check(f.Selected(449) == 0 && f.Selected(474) == 0 && f.Count(450) == 0 && f.Count(475) == 0,
        "removed parents clear parent controls without writing native bindings");
    f.CheckPmmCounts();
}
void CameraScopesAndLanguage() {
    Fixture f;f.app->CameraMode() = 1;auto selection = history_ui::CaptureSelection(*f.app);selection.globalScope = 3;
    AuthoringSnapshot before(f);Refresh(f, selection);before.Verify(f);
    const wchar_t* labels[] = {L"相机", L"照明", L"自阴影", L"重力"};
    for (int row = 0; row < 4; ++row) Check(f.Text(434, row) == labels[row], "global scope uses the existing Chinese UI dictionary");
    Check(f.Selected(434) == 3 && f.Text(434, 4) == accessoryBName && f.Text(434, 5) == accessoryAName,
        "camera scopes append accessories in native order");
    Check(f.Text(436, 0) == L"相机/照明/配件" && f.Text(449, 0) == L"无" && f.Text(474, 0) == L"地面", "object and parent headers are localized");
    SendMessageW(f.Control(434), CB_SETCURSEL, 5, 0);selection = history_ui::CaptureSelection(*f.app);
    Check(selection.scopeAccessory == f.accessoryA.get(), "capture scope accessory by pointer");
    f.app->AccessorySlot(8) = nullptr;f.accessoryA->order = 0;
    AuthoringSnapshot reordered(f);f.observed.Clear();Refresh(f, selection);reordered.Verify(f);
    Check(f.Selected(434) == 4, "scope follows accessory when an earlier row disappears");
    f.app->AccessorySlot(3) = nullptr;AuthoringSnapshot removed(f);Refresh(f, selection);removed.Verify(f);
    Check(f.Selected(434) == 0 && f.Count(434) == 4, "deleted scope accessory falls back to camera");
    for (unsigned char language : {kUiEnglish, kUiJapanese}) {
        f.app->EnglishUI() = language;AuthoringSnapshot translated(f);Refresh(f, selection);translated.Verify(f);
        Check(f.Text(434, 0) == (language == kUiEnglish ? L"camera" : L"カメラ"), "global scope supports English and Japanese");
    }
}
void EmptyScene() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);
    f.app->ModelSlot(2) = f.app->ModelSlot(9) = nullptr;
    f.app->AccessorySlot(3) = f.app->AccessorySlot(8) = nullptr;
    f.app->CameraParentModel() = -1;
    AuthoringSnapshot before(f);Refresh(f, selection, true, true);before.Verify(f);
    Check(f.Count(436) == 1 && f.Count(471) == 0 && f.Selected(471) == -1 && f.Count(434) == 4,
        "empty scene retains camera header and no accessory placeholder");
    Check(f.Count(449) == 1 && f.Count(474) == 1 && f.Count(450) == 0 && f.Count(475) == 0,
        "empty scene clears both parent bone lists");
    Check(f.app->CameraMode() == 1 && f.app->SelectedModelSlot() == 0 && f.app->SelectedAccessorySlot() == 0 &&
        IsWindowEnabled(f.Control(400)) && IsWindowEnabled(f.Control(401)), "empty scene permits common history buttons and safe slot values");
    f.CheckPmmCounts();
}
void InvalidNativeData() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);Refresh(f, selection);
    const auto rejected = [&](history_ui::Result expected) {
        AuthoringSnapshot before(f);const UiSnapshot ui(f);f.observed.Clear();
        Check(history_ui::RefreshObjectLists(*f.app, selection, false, true) == expected, "invalid native data returns an explicit error");
        before.Verify(f, false);ui.Verify(f);
        Check(f.observed.Writes() == 0, "invalid order and bone data are rejected before control writes");
    };
    for (unsigned char order : {0, 1, 3}) { f.modelA->comboSelIndex = order;rejected(history_ui::Result::InvalidOrder); }
    f.modelA->comboSelIndex = 2;
    for (unsigned char order : {0, 2}) { f.accessoryA->order = order;rejected(history_ui::Result::InvalidOrder); }
    f.accessoryA->order = 1;
    f.modelB->boneTable = nullptr;rejected(history_ui::Result::InvalidData);f.modelB->boneTable = f.bonesB.data();
}
void ControlFailureAndRetry() {
    for (const LRESULT failure : {LRESULT(CB_ERRSPACE), LRESULT(CB_ERR)}) {
        Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);AuthoringSnapshot before(f);
        f.observed.failInsertControl = 471;f.observed.failInsertCountdown = 2;f.observed.failure = failure;
        Check(history_ui::RefreshObjectLists(*f.app, selection, true, false) == history_ui::Result::ControlFailure,
            "failed Win32 insert is reported");
        Check(f.Count(436) == 3 && f.Count(471) == 1, "failure may leave partial controls and is not claimed to be atomic");
        before.Verify(f);Refresh(f, selection);before.Verify(f);f.CheckPmmCounts();
        Check(f.Selected(436) == 2 && f.Selected(471) == 1 && f.Text(471, 0) == accessoryBName && f.Text(471, 1) == accessoryAName,
            "retry rebuilds complete controls from native state");
    }
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);AuthoringSnapshot before(f);const UiSnapshot ui(f);
    f.observed.failReadControl = 450;
    Check(history_ui::RefreshObjectLists(*f.app, selection, true, false) == history_ui::Result::ControlFailure,
        "failed Win32 preflight read is reported");
    before.Verify(f, false);ui.Verify(f);Check(f.observed.Writes() == 0, "preflight read failure does not begin control writes");
    Refresh(f, selection);before.Verify(f);f.CheckPmmCounts();
}
void SlotReuseDoesNotReselectReplacement() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);
    auto model = std::make_unique<mdl::ModelRecord>();auto accessory = std::make_unique<mdl::AccessoryRecord>();
    model->comboSelIndex = 2;model->pmxTextBuffers[0] = const_cast<wchar_t*>(L"replacement");
    accessory->order = 1;accessory->parentModel = -1;
    f.app->ModelSlot(2) = Fixture::Raw(model.get());f.app->AccessorySlot(3) = accessory.get();
    const auto replacementModel = Bytes(model.get(), sizeof(*model)), replacementAccessory = Bytes(accessory.get(), sizeof(*accessory));
    AuthoringSnapshot before(f);Refresh(f, selection);before.Verify(f);
    Check(f.app->CameraMode() == 1 && f.Selected(436) == 0 && f.app->SelectedAccessorySlot() == 8 && f.Selected(471) == 0,
        "same-slot replacements cannot impersonate the captured selected objects");
    Check(replacementModel == Bytes(model.get(), sizeof(*model)) && replacementAccessory == Bytes(accessory.get(), sizeof(*accessory)),
        "fallback does not modify replacement objects");
    f.CheckPmmCounts();
}
void ThreadAndControlGuards() {
    Fixture f;const auto selection = history_ui::CaptureSelection(*f.app);AuthoringSnapshot before(f);const UiSnapshot ui(f);
    history_ui::Result result = history_ui::Result::Ok;
    std::thread worker([&] { result = history_ui::RefreshObjectLists(*f.app, selection, true, false); });worker.join();
    Check(result == history_ui::Result::WrongThread, "worker thread cannot refresh owner controls");
    before.Verify(f, false);ui.Verify(f);Check(f.observed.Writes() == 0, "wrong-thread call sends no combo writes");
    DestroyWindow(f.Control(450));f.observed.Clear();
    Check(history_ui::RefreshObjectLists(*f.app, selection, true, false) == history_ui::Result::MissingControl,
        "missing parent control prevents refresh");
    before.Verify(f, false);Check(f.observed.Writes() == 0, "missing-control rejection precedes all control writes");
}
} // namespace

int main() {
    // Match the repository's Win7-compatible hidden physics/editor fixtures.
    // ICC_STANDARD_CLASSES requires the v6 activation context, which this
    // renderer-free console test does not request. The controls themselves are
    // USER32 classes; every actual SetWindowSubclass call is checked below.
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&controls) != FALSE, "initialize Win32 control subclass support");
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);SetModelNameAutoTranslation(false);
    OrderingAndParents();DeletionFallbacks();PreserveSelectionAfterEarlierRemoval();CameraScopesAndLanguage();
    EmptyScene();InvalidNativeData();ControlFailureAndRetry();SlotReuseDoesNotReselectReplacement();ThreadAndControlGuards();
    std::printf("scene history UI checks=%u passed\n", checks);
    return 0;
}
