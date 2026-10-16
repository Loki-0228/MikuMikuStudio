// Include after the common history/pose fixtures. These tests drive the real
// 437/473 handlers and their actual confirmation boxes in a hidden window.
#include "../src/plugins/native_object_commands.hpp"
#include "mikudancestudio/scene_history_ui.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_message.hpp"
#include "mikudancestudio/plugin_storage.hpp"
#include <commctrl.h>
namespace mikudancestudio {
void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void DeleteModel(unsigned char*, int);
void DeleteAccessory(mdl::AccessoryRecord*, int);
}
namespace {
struct ObjectCommandFixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    plugins::StorageStore storage;
    plugins::SharedHistory history;
    MMDApp* previous = g_Block;
    unsigned commands = 0;
    int failInsert = 0;
    int failCountReads = 0;
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<ObjectCommandFixture*>(data);
        if (message == WM_COMMAND) ++self.commands;
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK InsertFault(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<ObjectCommandFixture*>(data);
        if (message == CB_GETCOUNT && GetDlgCtrlID(window) == 436 && self.failCountReads > 0 && --self.failCountReads == 0)
            return CB_ERR;
        if (message == CB_INSERTSTRING && self.failInsert == GetDlgCtrlID(window)) {
            self.failInsert = 0; return CB_ERRSPACE;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    explicit ObjectCommandFixture(uint64_t budget = 256ull * 1024 * 1024)
        : history(storage, [this] {
            if (plugins::NativeObjectUiPending(app.get())) return MMS_E_BUSY;
            if ((plugins::NativeObjectOperationActive(app.get()) || app->SceneMutationInProgress()) &&
                !plugins::NativeObjectPublicationAuthorized(app.get())) return MMS_E_BUSY;
            return MMS_OK;
        }, 128, budget) {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->EnglishUI() = 2; app->WindowLayoutReady() = 1; app->CameraParentModel() = -1;
        app->MainWindow() = CreateWindowExW(0, L"STATIC", L"object history command test", WS_OVERLAPPED,
            0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(app->MainWindow() != nullptr, "object command hidden main window created");
        Check(SetWindowSubclass(app->MainWindow(), Observe, 5, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
            "object command owner observes reentrant messages");
        for (int id = 400; id <= 566; ++id) {
            const bool combo = id == 429 || id == 434 || id == 436 || id == 443 || id == 449 ||
                id == 450 || id == 471 || id == 474 || id == 475 || id == 504 || id == 509 || id == 514 || id == 519;
            const auto control = CreateWindowExW(0, combo ? L"COMBOBOX" : (id >= 478 && id <= 485) ? L"EDIT" : L"BUTTON",
                L"", WS_CHILD | (combo ? CBS_DROPDOWNLIST : 0), 0, 0, 80, 100,
                app->MainWindow(), reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            Check(control != nullptr, "object command hidden panel control created");
            if (combo) Check(SetWindowSubclass(control, InsertFault, 5, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
                "object command insertion failure hook installed");
        }
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto* model = static_cast<mdl::ModelRecord*>(std::calloc(1, sizeof(mdl::ModelRecord)));
            Check(model != nullptr, "object command model allocation");
            model->comboSelIndex = model->comboSelIndex2 = slot + 1; model->selectedBone = -1;
            model->boneKeys = mdl::BoneKeyPoolCreate(4); model->morphKeys = mdl::MorphKeyPoolCreate(4);
            model->displayKeys = mdl::KeyPoolCreate<mdl::DisplayKey>(4);
            std::strcpy(model->name, slot ? "model-B" : "model-A");
            model->lightDir[0] = 11.f + slot; model->morphKeys[0].value = .25f + slot;
            app->ModelSlot(slot) = reinterpret_cast<unsigned char*>(model);
            plugins::SceneObjectIdentity(model);
            auto* accessory = new mdl::AccessoryRecord{};
            accessory->order = std::uint8_t(slot); accessory->parentModel = -1;
            accessory->position[0] = 17.f + slot; accessory->rotation[2] = .75f;
            accessory->scale = 1.75f; accessory->opacity = .5f; accessory->rowSelected = 1;
            Check(text_encoding::EncodeDisplay(accessory->name, sizeof accessory->name, slot ? L"附件乙" : L"附件甲"),
                "object command Unicode accessory name");
            app->AccessorySlot(slot) = accessory;
            app->AccessoryKeys(slot) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
            app->AccessoryKeys(slot)[0].parentModel = -1;
            app->AccessoryKeys(slot)[0].scale = 1.75f; app->AccessoryKeys(slot)[0].opacity = .5f;
            app->AccessoryKeys(slot)[0].position[0] = 3.f + slot;
            plugins::SceneObjectIdentity(accessory);
        }
        app->CurrentFrame() = 137; app->SetSelectedModelSlot(0); app->CameraMode() = 0;
        app->SelectedAccessorySlot() = 0; app->AccessoryRenderSplitOrder() = 2;
        app->GlobalTrackSelected(GlobalTimelineTrack::Light) = 1;
        app->CameraPositionX() = 27.f; app->CameraDistance() = -81.f;
        g_Block = app.get();
        Check(scene_history_ui::RefreshObjectLists(*app, scene_history_ui::CaptureSelection(*app), false, false) ==
            scene_history_ui::Result::Ok, "object command initial lists built from native order");
        commands = 0;
    }
    ~ObjectCommandFixture() {
        plugins::ForgetBoundObjectUi(app.get());
        Check(history.Reset() == MMS_OK, "object command journal reset before scene resources"); history.Collect();
        for (unsigned slot = 0; slot < 3; ++slot) {
            if (auto* model = app->ModelSlot(slot)) { DeleteModel(model, 1); app->ModelSlot(slot) = nullptr; }
            if (auto* accessory = app->AccessorySlot(slot)) { DeleteAccessory(accessory, 1); app->AccessorySlot(slot) = nullptr; }
            mdl::KeyPoolDestroy(app->AccessoryKeys(slot));
        }
        curve_host::Reset(app.get()); DestroyWindow(app->MainWindow()); app->MainWindow() = nullptr; g_Block = previous;
    }
    void Command(bool model) {
        if (model) CmdControl400(app.get(), app->MainWindow(), 437, 0);
        else CmdControl450(app.get(), app->MainWindow(), 473, 0);
    }
    LRESULT Count(int id) const { return SendDlgItemMessageW(app->MainWindow(), id, CB_GETCOUNT, 0, 0); }
    bool Unmoved() const {
        return app->CurrentFrame() == 137 && app->CameraPositionX() == 27.f && app->CameraDistance() == -81.f &&
            app->GlobalTrackSelected(GlobalTimelineTrack::Light) == 1 &&
            app->AccessorySlot(1)->position[0] == 18.f && app->AccessorySlot(1)->rotation[2] == .75f &&
            app->AccessorySlot(1)->scale == 1.75f && app->AccessorySlot(1)->rowSelected == 1 &&
            mdl::Mdl(app->ModelSlot(1))->lightDir[0] == 12.f && mdl::Mdl(app->ModelSlot(1))->morphKeys[0].value == 1.25f;
    }
};
struct ObjectModalProbe {
    ObjectCommandFixture& fixture;
    HHOOK hook = nullptr;
    int answer = IDOK, seen = 0;
    bool stale = false, reenter = false, close = false, model = true;
    long allocationFailure = -1;
    static thread_local ObjectModalProbe* current;
    static LRESULT CALLBACK Inspect(int code, WPARAM wp, LPARAM lp) {
        if (code == HCBT_ACTIVATE && current) {
            auto& self = *current; const auto dialog = reinterpret_cast<HWND>(wp);
            wchar_t name[32]{}; GetClassNameW(dialog, name, 32);
            if (std::wcscmp(name, L"#32770") == 0) {
                ++self.seen; ShowWindow(dialog, SW_HIDE);
                wchar_t body[1024]{}; GetDlgItemTextW(dialog, 0xffff, body, 1024);
                const auto language = self.fixture.app->EnglishUI();
                const auto expected = language == 0 ? L"元に戻せます" : language == 1 ? L"can undo" : L"可以撤销";
                Check(std::wcsstr(body, expected) != nullptr, "controlled deletion dialog states it can be undone in the selected language");
                if (!self.model) Check(std::wcsstr(body, L"附件甲") != nullptr, "actual accessory confirmation retains Unicode name");
                Check(plugins::NativeObjectOperationActive(self.fixture.app.get()) &&
                    !plugins::NativeObjectPublicationAuthorized(self.fixture.app.get()),
                    "confirmation runs under operation guard without publication permission");
                if (self.reenter) {
                    const auto count = self.fixture.commands;
                    SendMessageW(self.fixture.app->MainWindow(), WM_COMMAND, 437, 0);
                    Check(self.fixture.commands == count, "queued/direct native command cannot reenter confirmation");
                    Check(plugins::TryDeleteBoundAccessory(self.fixture.app.get(), self.fixture.app->MainWindow()),
                        "direct migrated deletion is consumed during modal reentry");
                    Check(self.fixture.history.History(false) == MMS_E_BUSY,
                        "common journal rejects modal-time direct history operation");
                }
                if (self.close) SendMessageW(self.fixture.app->MainWindow(), WM_CLOSE, 0, 0);
                if (self.stale) {
                    const void* object = self.model ? static_cast<void*>(self.fixture.app->ModelSlot(0)) :
                        static_cast<void*>(self.fixture.app->AccessorySlot(0));
                    plugins::ForgetSceneObject(object); plugins::SceneObjectIdentity(object);
                }
                PostMessageW(dialog, WM_COMMAND, self.answer, 0);
                if (self.allocationFailure >= 0) failAfter = self.allocationFailure;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    explicit ObjectModalProbe(ObjectCommandFixture& value, bool forModel) : fixture(value), model(forModel) {
        Check(current == nullptr, "no nested test confirmation hook"); current = this;
        hook = SetWindowsHookExW(WH_CBT, Inspect, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "actual confirmation hook installed");
    }
    ~ObjectModalProbe() { failAfter = -1; UnhookWindowsHookEx(hook); current = nullptr; }
};
thread_local ObjectModalProbe* ObjectModalProbe::current = nullptr;

void ObjectCommandRoundTripChecks() {
    for (bool model : {true, false}) {
        std::printf("  object roundtrip model=%u\n", unsigned(model)); std::fflush(stdout);
        ObjectCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        const void* original = model ? static_cast<void*>(f.app->ModelSlot(0)) : static_cast<void*>(f.app->AccessorySlot(0));
        const auto identity = plugins::SceneObjectIdentity(original);
        auto* keys = f.app->AccessoryKeys(0);
        { ObjectModalProbe prompt(f, model); prompt.reenter = true; f.Command(model);
          Check(prompt.seen == 1, "real 437/473 handler displays exactly one confirmation"); }
        Check(f.history.UndoCount() == 1 && f.history.RedoCount() == 0 && f.app->SceneModified() == 1 &&
            (model ? f.app->ModelSlot(0) == nullptr : f.app->AccessorySlot(0) == nullptr),
            "real deletion publishes one measured common-history node");
        Check(f.Unmoved() && f.Count(436) == (model ? 2 : 3) && f.Count(471) == (model ? 2 : 1),
            "object deletion rebuilds PMM counts without seeking or touching surviving author data");
        Check(model ? (f.app->CameraMode() && f.app->MainModelComboSelection() == 0) :
            f.app->SelectedAccessorySlot() == 1, "deleted selected object receives safe selection fallback");
        Check(!plugins::NativeObjectOperationActive(f.app.get()) && !plugins::NativeObjectUiPending(f.app.get()) &&
            !f.app->SceneMutationInProgress() && IsWindowEnabled(f.app->MainWindow()), "operation releases all guards after UI refresh");
        CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
        Check(f.history.RedoCount() == 1 && (model ? static_cast<void*>(f.app->ModelSlot(0)) : static_cast<void*>(f.app->AccessorySlot(0))) == original &&
            plugins::SceneObjectIdentity(original).id.lo == identity.id.lo && f.Count(436) == 3 && f.Count(471) == 2 && f.Unmoved(),
            "actual undo button restores the same retained object, lists, and authoring data without disk access");
        if (!model) Check(f.app->AccessoryKeys(0) == keys && f.app->AccessoryKeys(0)[0].position[0] == 3.f &&
            f.app->AccessoryRenderSplitOrder() == 2, "accessory undo keeps original pool and ordering boundary");
        CmdControl400(f.app.get(), f.app->MainWindow(), 401, 0);
        Check(f.history.UndoCount() == 1 && (model ? !f.app->ModelSlot(0) : !f.app->AccessorySlot(0)) && f.Unmoved(),
            "actual redo button removes retained object without re-evaluating scene");
    }
}
void ObjectCommandRejectionChecks() {
    for (bool model : {true, false}) for (unsigned language = 0; language < 3; ++language) {
        std::printf("  object cancel model=%u language=%u\n", unsigned(model), language); std::fflush(stdout);
        ObjectCommandFixture f; f.app->EnglishUI() = std::uint8_t(language);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        { ObjectModalProbe prompt(f, model); prompt.answer = IDCANCEL; prompt.close = true; f.Command(model);
          Check(prompt.seen == 1, "cancel still shows the real localized confirmation"); }
        Check(f.app->ModelSlot(0) && f.app->AccessorySlot(0) && f.history.UndoCount() == 0 &&
            f.app->SceneModified() == 0 && f.app->RecordedFrameCountByte1() == 0 && f.Unmoved(),
            "cancel leaves object, history, dirty, frame and author pose unchanged");
        MSG close{}; Check(PeekMessageW(&close, f.app->MainWindow(), WM_CLOSE, WM_CLOSE, PM_REMOVE) != FALSE,
            "close requested inside confirmation is deferred instead of lost");
        std::puts("  object cancel complete"); std::fflush(stdout);
    }
    for (int rejection = 0; rejection < 7; ++rejection) {
        std::printf("  object rejection=%d\n", rejection); std::fflush(stdout);
        ObjectCommandFixture f(rejection == 0 ? 1 : 256ull * 1024 * 1024);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        wchar_t unknown[] = L"unregistered resource capacity";
        if (rejection == 1) mdl::Mdl(f.app->ModelSlot(0))->pmxTextBuffers[0] = unknown;
        if (rejection == 3) f.app->FrameRangeDialog() = f.app->MainWindow();
        if (rejection == 4) mdl::Mdl(f.app->ModelSlot(1))->comboSelIndex = 1;
        if (rejection == 5) f.app->state.accessoryEditDialogOpen = 1;
        if (rejection == 6) f.app->state.edgeThicknessDialog = f.app->MainWindow();
        { ObjectModalProbe prompt(f, true); prompt.stale = rejection == 2; f.Command(true);
          Check(prompt.seen == (rejection < 3 ? 1 : 0), "busy/invalid-order rejection precedes confirmation"); }
        Check(f.app->ModelSlot(0) && f.history.UndoCount() == 0 && f.app->SceneModified() == 0 &&
            !plugins::NativeObjectOperationActive(f.app.get()) && !plugins::NativeObjectUiPending(f.app.get()),
            "budget, missing measurement, identity change, busy editor or invalid order never falls through to legacy deletion");
        mdl::Mdl(f.app->ModelSlot(0))->pmxTextBuffers[0] = nullptr;
        mdl::Mdl(f.app->ModelSlot(1))->comboSelIndex = 2; f.app->FrameRangeDialog() = nullptr;
        f.app->state.accessoryEditDialogOpen = 0; f.app->state.edgeThicknessDialog = nullptr;
    }
    { ObjectCommandFixture f;
      std::puts("  object operation allocation failure"); std::fflush(stdout);
      Check(!plugins::TryDeleteBoundModel(f.app.get(), f.app->MainWindow()) &&
          !plugins::TryDeleteBoundAccessory(f.app.get(), f.app->MainWindow()), "unbound host preserves both legacy deletion routes");
      plugins::NativePoseTestBinding binding(*f.app, f.history);
      failAfter = 0; f.Command(true); failAfter = -1;
      Check(f.app->ModelSlot(0) && !f.app->SceneModified() && f.history.UndoCount() == 0 &&
          !plugins::NativeObjectOperationActive(f.app.get()), "operation allocation failure releases guard and leaves scene unchanged"); }
    for (bool model : {true, false}) {
        std::printf("  object post-confirmation allocation failure model=%u\n", unsigned(model)); std::fflush(stdout);
        ObjectCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        { ObjectModalProbe prompt(f, model); prompt.allocationFailure = 0; f.Command(model); failAfter = -1;
          Check(prompt.seen == 1, "allocation failure is injected after a real accepted confirmation"); }
        Check(f.app->ModelSlot(0) && f.app->AccessorySlot(0) && f.history.UndoCount() == 0 &&
            !f.app->SceneModified() && !f.app->SceneMutationInProgress() && !plugins::NativeObjectOperationActive(f.app.get()) && f.Unmoved(),
            "post-confirmation allocation failure leaves object/history/author state and unlocks native UI");
    }
}
void ObjectCommandUiRetryChecks() {
  for (bool failBeforeControlWrites : {false, true}) {
    std::printf("  object UI retry early=%u\n", unsigned(failBeforeControlWrites)); std::fflush(stdout);
    ObjectCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    if (failBeforeControlWrites) f.failCountReads = 2;
    else f.failInsert = 436;
    { ObjectModalProbe prompt(f, true); f.Command(true); }
    Check(!f.app->ModelSlot(0) && f.app->CameraMode() && f.history.UndoCount() == 1 && plugins::NativeObjectUiPending(f.app.get()),
        "post-publication Win32 failure keeps committed deletion and marks UI for retry");
    Check(f.history.History(false) == MMS_E_BUSY, "partial object lists block subsequent history publication");
    bool refusedSave = false;
    try { plugins::ProjectSave save(f.app.get()); } catch (const std::runtime_error&) { refusedSave = true; }
    Check(refusedSave, "partial object lists cannot be serialized into PMM");
    Check(plugins::RefreshBoundObjectUi(*f.app, f.history) && !plugins::NativeObjectUiPending(f.app.get()) &&
        f.Count(436) == 2 && f.Count(471) == 2 && f.Unmoved(), "UI retry rebuilds from native data without repeating deletion or seeking");
    CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
    Check(f.app->ModelSlot(0) && f.Count(436) == 3 && f.history.RedoCount() == 1,
        "history remains usable after a failed control update and retry");
  }
}
void ObjectCommandDeferredScopeChecks() {
    for (bool deferred : {false, true}) {
        std::printf("  object scope deferred=%u\n", unsigned(deferred)); std::fflush(stdout);
        ObjectCommandFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        auto* third = new mdl::AccessoryRecord{}; third->parentModel = -1; third->order = 2;
        third->scale = third->opacity = 1; std::strcpy(third->name, "accessory-C");
        f.app->AccessorySlot(2) = third; f.app->AccessoryKeys(2) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
        f.app->AccessoryKeys(2)[0].parentModel = -1;
        f.app->CameraMode() = 1; f.app->SelectedAccessorySlot() = 1;
        Check(scene_history_ui::RefreshObjectLists(*f.app, scene_history_ui::CaptureSelection(*f.app), false, false) ==
            scene_history_ui::Result::Ok, "three-accessory camera scope list created");
        SendDlgItemMessageW(f.app->MainWindow(), 434, CB_SETCURSEL, 5, 0); // B
        SendDlgItemMessageW(f.app->MainWindow(), 471, CB_SETCURSEL, 0, 0); // Delete A, keep selected B by identity.
        if (!deferred) f.failInsert = 471;
        { ObjectModalProbe prompt(f, false); f.Command(false); }
        if (deferred) {
            CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
            Check(f.app->AccessorySlot(0) != nullptr, "restore A before deferred plugin-style redo");
            SendDlgItemMessageW(f.app->MainWindow(), 434, CB_SETCURSEL, 5, 0); // B in A/B/C.
            Check(plugins::RunBoundObjectHistory(*f.app, f.history, true, true) == MMS_OK,
                "plugin-style history publication queues the object UI update");
        }
        Check(!f.app->AccessorySlot(0) && plugins::NativeObjectUiPending(f.app.get()) && f.app->AccessorySlot(1)->order == 0,
            "queued object refresh sees B at new native order while controls still refer to old rows");
        Check(plugins::RunBoundObjectHistory(*f.app, f.history, false, true) == MMS_E_BUSY &&
            plugins::RunBoundObjectHistory(*f.app, f.history, true, true) == MMS_E_BUSY,
            "plugin history requests are refused while object selection/list publication is pending");
        CmdControl400(f.app.get(), f.app->MainWindow(), 400, 0);
        Check(!f.app->AccessorySlot(0) && f.history.UndoCount() == 1,
            "native history retry also preserves the pending object publication and its selection token");
        Check(plugins::RefreshBoundObjectUi(*f.app, f.history), "deferred or failed refresh resolves saved stable selection IDs");
        Check(SendDlgItemMessageW(f.app->MainWindow(), 434, CB_GETCURSEL, 0, 0) == 4 &&
            SendDlgItemMessageW(f.app->MainWindow(), 434, CB_GETITEMDATA, 4, 0) == 1 && f.app->SelectedAccessorySlot() == 1,
            "deleting A preserves registration scope B instead of reinterpreting its old row as C");
    }
}
}
void NativeObjectCommandChecks() {
    std::puts("RUN NativeObjectCommandRoundTripChecks"); ObjectCommandRoundTripChecks();
    std::puts("RUN NativeObjectCommandRejectionChecks"); ObjectCommandRejectionChecks();
    std::puts("RUN NativeObjectCommandUiRetryChecks"); ObjectCommandUiRetryChecks();
    std::puts("RUN NativeObjectCommandDeferredScopeChecks"); ObjectCommandDeferredScopeChecks();
}
