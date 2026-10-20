#include "native_camera_parent_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_camera_follow_ui.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "../window/accessory_bone_combo_cache.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <climits>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
constexpr wchar_t kCaptureOwner[]=L"MMS.Native.CameraParentCapture.V1";
struct CaptureAdmission {
    inline static thread_local CaptureAdmission* active = nullptr;
    MMDApp& app; const HWND main, combo, list, capture;
    CaptureAdmission* previous;
    HANDLE previousOwner=nullptr; bool tagged=false;
    CaptureAdmission(MMDApp& value, HWND control, HWND popup)
        : app(value), main(value.MainWindow()), combo(control), list(popup), capture(GetCapture()), previous(active) {
        if(capture&&(capture==combo||capture==list)){
            previousOwner=GetPropW(combo,kCaptureOwner);
            if(!SetPropW(combo,kCaptureOwner,reinterpret_cast<HANDLE>(this)))throw std::bad_alloc();
            tagged=true;
        }
        active = this;
    }
    ~CaptureAdmission() {
        if(tagged&&GetPropW(combo,kCaptureOwner)==reinterpret_cast<HANDLE>(this)){
            if(previousOwner)SetPropW(combo,kCaptureOwner,previousOwner);else RemovePropW(combo,kCaptureOwner);
        }
        active = previous;
    }
    bool Allows(HWND current) const noexcept {
        // The trusted entry validated GetComboBoxInfo once. Do not send a
        // combo query from the final history publication check. Destroying
        // the window removes this token, including same-HWND recreation.
        return tagged&&current&&current==capture&&(current==combo||current==list)&&app.MainWindow()==main&&
            GetPropW(combo,kCaptureOwner)==reinterpret_cast<HANDLE>(const_cast<CaptureAdmission*>(this));
    }
};
bool Ready(MMDApp& app, bool replay = false) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    const auto capture = GetCapture();
    const bool ownCapture = CaptureAdmission::active && &CaptureAdmission::active->app == &app && CaptureAdmission::active->Allows(capture);
    return (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) &&
        (!NativeObjectOperationActive(&app) || own) && !NativeObjectUiPending(&app) &&
        !SceneSaveBlocksEdits() && (!capture || ownCapture) && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.RecordingWindow() && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app);
}
bool Selectable(const mdl::BoneRecord& bone) noexcept {
    return bone.type < mdl::BoneType::InertTip || bone.type == mdl::BoneType::FixedAxis;
}
// All remembered pointers are tested through the current scene membership
// before dereferencing the model or table. Runtime generation is deliberately
// absent here: a restored retained owner remains a valid long-term source.
struct ParentSource {
    MMDApp& app;
    SharedHistory* const history;
    const std::uint64_t scene, pool;
    const int slot;
    unsigned char* const raw;
    mdl::BoneRecord* const bones;
    const std::uint32_t count;
    std::shared_ptr<RetainedSceneObject> owner;
    MmsObjectRef identity{};
    explicit ParentSource(MMDApp& value, bool retain = true)
        : app(value), history(BoundNativePoseHistory(&value)), scene(value.SceneGeneration()),
          pool(mdl::KeyPoolIdentity(value.CameraKeys())), slot(value.CameraParentModel()), raw(value.ModelSlot(slot)),
          bones(mdl::Mdl(raw)->boneTable), count(mdl::Mdl(raw)->boneCount),
          owner(retain ? RetainSceneObject(value, MMS_OBJECT_MODEL, unsigned(slot)) : nullptr) {
        if (!FindSceneObjectIdentity(raw, identity)) identity = SceneObjectIdentity(raw);
    }
    bool Current() const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || !pool ||
            mdl::KeyPoolIdentity(app.CameraKeys()) != pool || app.CameraParentModel() != slot ||
            slot < 0 || slot >= kModelSlotCount || app.ModelSlot(slot) != raw ||
            (owner && !owner->Present()) || !SceneObjectPresent(raw, identity.id)) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (int(i) != slot && app.ModelSlot(i) == raw) return false;
        return mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == count;
    }
    std::uint64_t Generation() const noexcept { MmsObjectRef current{}; return FindSceneObjectIdentity(raw,current) ? current.generation : 0; }
};
bool HasParent(MMDApp& app) noexcept {
    const int slot = app.CameraParentModel();
    return slot >= 0 && slot < kModelSlotCount && app.ModelSlot(slot) &&
        mdl::Mdl(app.ModelSlot(slot))->boneTable && mdl::Mdl(app.ModelSlot(slot))->boneCount;
}
struct ParentChange final : HistoryChange, std::enable_shared_from_this<ParentChange> {
    ParentSource source;
    const std::int32_t before, after;
    const std::uint64_t initialBinding, initialObject;
    std::uint64_t initialRevision = 0, serial = 1;
    bool applied = false, published = false;
    ParentChange(MMDApp& app, std::int32_t value)
        : source(app), before(app.CameraParentBone()), after(value), initialBinding(BoundNativePoseGeneration(&app)),
          initialObject(source.Generation()) {}
    bool InitialCurrent() const noexcept {
        if (published) return true;
        std::uint64_t revision = 0;
        return source.Current() && BoundNativePoseGeneration(&source.app) == initialBinding &&
            source.Generation() == initialObject && source.history->Revision(revision) == MMS_OK &&
            revision == initialRevision && source.app.CameraParentBone() == before;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ParentChange> change;
        const bool forward;
        const std::uint64_t serial, binding, object;
        const HWND capture;
        std::uint64_t revision = 0;
        const int frame;
        const std::int32_t expected;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<ParentChange> value, bool direction)
            : change(std::move(value)), forward(direction), serial(change->serial),
              binding(BoundNativePoseGeneration(&change->source.app)), object(change->source.Generation()), capture(GetCapture()),
              frame(change->source.app.CurrentFrame()), expected(change->source.app.CameraParentBone()) {}
        bool IsCurrent() const noexcept override {
            const auto& c = *change; const auto& s = c.source; std::uint64_t currentRevision = 0;
            return !consumed && serial == c.serial && forward != c.applied && s.Current() && c.InitialCurrent() &&
                Ready(s.app, true) && binding == BoundNativePoseGeneration(&s.app) && object == s.Generation() &&
                frame == s.app.CurrentFrame() && GetCapture() == capture && expected == s.app.CameraParentBone() &&
                s.history->Revision(currentRevision) == MMS_OK && currentRevision == revision &&
                s.owner->CoversBookkeeping(resources) && resources.Result().Complete();
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& c = *change;
            c.source.app.CameraParentBone() = forward ? c.after : c.before;
            c.applied = forward; c.published = true; ++c.serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!source.Current() || !InitialCurrent() || forward == applied) return MMS_E_STALE;
            if (!Ready(source.app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (auto r = source.history->Revision(candidate->revision)) return r;
            if (auto r = source.owner->MeasureBookkeeping(candidate->resources)) return r;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RetainedBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

struct Controls {
    ParentSource& source;
    const HWND main, models, bones;
    const std::uint64_t binding, object;
    const int language, value;
    explicit Controls(ParentSource& value)
        : source(value), main(value.app.MainWindow()), models(GetDlgItem(main, panel::kMainComboNormal)),
          bones(GetDlgItem(main, panel::kBoneRegisterCombo)), binding(BoundNativePoseGeneration(&value.app)),
          object(value.Generation()), language(value.app.EnglishUI()), value(value.app.CameraParentBone()) {}
    bool CurrentIdentity() const noexcept {
        return source.Current() && BoundNativePoseGeneration(&source.app) == binding && source.Generation() == object &&
            source.app.EnglishUI() == language && source.app.MainWindow() == main && IsWindow(main) &&
            models && bones && GetDlgItem(main, panel::kMainComboNormal) == models &&
            GetDlgItem(main, panel::kBoneRegisterCombo) == bones;
    }
    bool Current() const noexcept { return CurrentIdentity() && source.app.CameraParentBone() == value; }
    LRESULT Send(HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) const {
        if (!Current()) throw MMS_E_STALE;
        const auto result = SendMessageW(window, message, w, l);
        if (!Current()) throw MMS_E_STALE;
        return result;
    }
};
using Projection = model_order::Projection<kModelSlotCount>;
Projection Order(MMDApp& app) noexcept {
    return model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
        const auto* raw = app.ModelSlot(int(slot)); return raw ? int(mdl::Mdl(raw)->comboSelIndex) : -1;
    });
}
int ModelRow(const Controls& controls, const Projection& order) {
    // Resolve uses noexcept item access; native calls must be revalidated by
    // the outer function, never throw through its noexcept projection helper.
    const auto count = controls.Send(controls.models, CB_GETCOUNT);
    std::array<LRESULT, kModelSlotCount + 1> items{};
    if (count < 1 || count > kModelSlotCount + 1) return -1;
    for (LRESULT row = 0; row < count; ++row) items[std::size_t(row)] = controls.Send(controls.models, CB_GETITEMDATA, row);
    return model_order::FindRow(order, unsigned(controls.source.slot), count,
        [&](std::size_t row) noexcept { return items[row]; });
}
struct Row { std::wstring text; int value; };
std::vector<Row> BoneRows(const Controls& controls) {
    std::vector<Row> rows;
    for (std::uint32_t i = 0; i < controls.source.count; ++i) if (Selectable(controls.source.bones[i]))
        rows.push_back({text_encoding::BoneLabel(controls.source.bones[i], UiUsesEnglishNames(controls.language)), int(i)});
    return rows;
}
bool VerifyBoneList(const Controls& controls, const std::vector<Row>& rows, const accessory_bone_combo::Cache& cache) {
    if (controls.Send(controls.bones, CB_GETCOUNT) != LRESULT(rows.size())) return false;
    std::vector<bool> used(rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto value = controls.Send(controls.bones, CB_GETITEMDATA, row);
        const auto found = std::lower_bound(rows.begin(), rows.end(), value, [](const Row& item,LRESULT id){ return item.value < id; });
        if (found == rows.end() || found->value != value) return false;
        const auto index = std::size_t(found - rows.begin()); if (used[index]) return false; used[index] = true;
        accessory_bone_combo::ReadGuard read(cache);
        const auto length = controls.Send(controls.bones, CB_GETLBTEXTLEN, row);
        if (length < 0 || std::size_t(length) != found->text.size()) return false;
        std::wstring text(std::size_t(length) + 1, L'\0');
        if (controls.Send(controls.bones, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(text.data())) != length ||
            !std::equal(found->text.begin(), found->text.end(), text.begin())) return false;
    }
    return true;
}
bool BoneList(const Controls& controls) {
    auto cache=accessory_bone_combo::Find(controls.bones,true);
    if(!controls.Current()||!cache||cache->busy)return false;
    const auto* model=mdl::Mdl(controls.source.raw);const bool english=UiUsesEnglishNames(controls.language);
    if(cache->valid&&cache->source.Matches(model,english))return controls.Current();
    accessory_bone_combo::Guard guard(cache);accessory_bone_combo::Source source;
    const bool captured=accessory_bone_combo::Source::Capture(model,english,source);
    const auto mutation=cache->mutations;const auto rows=BoneRows(controls);
    if(!VerifyBoneList(controls,rows,cache)||!cache->alive||cache->mutations!=mutation||!controls.Current())return false;
    if(captured){if(!source.Matches(model,english))return false;cache->Remember(std::move(source),mutation);}
    return true;
}
bool SelectByData(const Controls& controls, HWND combo, int value) {
    const auto cache=accessory_bone_combo::Find(combo);
    if(cache&&cache->valid){
        const auto selected=controls.Send(combo,CB_GETCURSEL);
        if(selected>=0&&controls.Send(combo,CB_GETITEMDATA,selected)==value)return true;
    }
    const auto count = controls.Send(combo, CB_GETCOUNT); LRESULT found = -1;
    for (LRESULT row = 0; row < count; ++row) if (controls.Send(combo, CB_GETITEMDATA, row) == value) {
        if (found >= 0) return false; found = row;
    }
    if (controls.Send(combo, CB_GETCURSEL) != found) {
        const auto result = controls.Send(combo, CB_SETCURSEL, found);
        if (found >= 0 && result != found) return false;
    }
    return controls.Send(combo, CB_GETCURSEL) == found;
}
bool Rebuild(const Controls& controls, HWND combo, const std::vector<Row>& rows) {
    controls.Send(combo, CB_RESETCONTENT);
    if (controls.Send(combo, CB_GETCOUNT) != 0) return false;
    for (const auto& row : rows) {
        const auto index = controls.Send(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.text.c_str()));
        if (index < 0 || controls.Send(combo, CB_SETITEMDATA, index, row.value) == CB_ERR) return false;
    }
    return controls.Send(combo, CB_GETCOUNT) == LRESULT(rows.size());
}
thread_local MMDApp* reading = nullptr;
LRESULT CALLBACK RepairProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
    if (message == WM_NCDESTROY) { KillTimer(window, id); RemoveWindowSubclass(window, RepairProc, id); }
    if (message == WM_TIMER && w == id) {
        KillTimer(window, id);
        auto* app = reinterpret_cast<MMDApp*>(data);
        if (app && BoundNativePoseHistory(app) && GetDlgItem(app->MainWindow(), panel::kBoneRegisterCombo) == window)
            RefreshBoundCameraParentBoneReadout(*app);
        return 0;
    }
    return DefSubclassProc(window, message, w, l);
}
void Retry(MMDApp& app) noexcept {
    const auto combo = GetDlgItem(app.MainWindow(), panel::kBoneRegisterCombo);
    const auto id = reinterpret_cast<UINT_PTR>(&RepairProc);
    if (combo && SetWindowSubclass(combo, RepairProc, id, reinterpret_cast<DWORD_PTR>(&app))) SetTimer(combo, id, 50, nullptr);
}
// No allocations or label generation: a budget/OOM refusal can still restore
// a healthy list's exact canonical row. Corrupt lists are left for repair.
void RestoreFast(MMDApp& app) noexcept {
    if (!HasParent(app)) return;
    const int slot = app.CameraParentModel(); const auto* raw = app.ModelSlot(slot);
    const auto* bones = mdl::Mdl(raw)->boneTable; const auto boneCount = mdl::Mdl(raw)->boneCount;
    const auto scene = app.SceneGeneration(), pool = mdl::KeyPoolIdentity(app.CameraKeys()), binding = BoundNativePoseGeneration(&app);
    MmsObjectRef identity{}; if(!FindSceneObjectIdentity(raw,identity))return;
    const auto main = app.MainWindow(), combo = GetDlgItem(main, panel::kBoneRegisterCombo);
    const int value = app.CameraParentBone(); LRESULT found = -1;
    const auto current = [&]() noexcept {
        MmsObjectRef live{};
        return app.SceneGeneration() == scene && BoundNativePoseGeneration(&app) == binding &&
            mdl::KeyPoolIdentity(app.CameraKeys()) == pool && app.CameraParentModel() == slot && app.ModelSlot(slot) == raw &&
            FindSceneObjectIdentity(raw,live)&&live.id.lo==identity.id.lo&&live.id.hi==identity.id.hi&&live.generation==identity.generation&&
            mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == boneCount && app.CameraParentBone() == value &&
            main == app.MainWindow() && GetDlgItem(main, panel::kBoneRegisterCombo) == combo;
    };
    const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT row = 0; row < count && current(); ++row) {
        const auto data = SendMessageW(combo, CB_GETITEMDATA, row, 0); if (!current()) return;
        if (data == value) { if (found >= 0) return; found = row; }
    }
    if (current()) SendMessageW(combo, CB_SETCURSEL, found, 0);
}
void Accepted(MMDApp& app, SharedHistory& history, bool changed) {
    bake::ScopedUiInputGate input;
    const auto main=app.MainWindow();const auto binding=BoundNativePoseGeneration(&app),scene=app.SceneGeneration();
    const auto* journal=&history;
    const bool undo=history.UndoCount()!=0,redo=history.RedoCount()!=0;
    const auto current=[&]() noexcept {return BoundNativePoseHistory(&app)==journal&&BoundNativePoseGeneration(&app)==binding&&app.SceneGeneration()==scene&&app.MainWindow()==main&&(!main||IsWindow(main));};
    // The old RefreshRequest(-1) only enters an unselected camera row. Keep
    // that selection effect explicit and never run a model-mode pose sweep.
    const bool selectionChanged=!app.GlobalTrackSelected(GlobalTimelineTrack::Camera);
    if (selectionChanged) {
        app.SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.AccessorySlot(slot)) object->rowSelected = 0;
    }
    if (changed) {
        app.SceneModified() = 1; app.RecordedFrameCountByte1() = 1;
        EnableWindow(GetDlgItem(main, panel::kUndoButton),undo);if(!current())return;
        EnableWindow(GetDlgItem(main, panel::kRedoButton),redo);if(!current())return;
    }
    RefreshBoundCameraParentBoneReadout(app);
    if(!current())return;
    // WM_PAINT only blits this bitmap. Refresh its camera-track highlight
    // after acceptance, while drafts and the input gate remain protected.
    // The model-mode sweep can change selectedBone and is never used here.
    if(selectionChanged&&app.CameraMode()&&app.Renderer()&&app.PanelDC()) {
        PostLanguageSweep(&app);if(!current())return;
    }
    if(main)InvalidateRect(main, nullptr, FALSE);
    if(!current())return;
    updates::RequestViewport(&app, false);
}
}
MmsResult PlanCameraParentBone(MMDApp& app, std::int32_t value, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (auto r = history->CheckNativeNavigation()) return r;
        if (auto r = history->CheckNativeEdit()) return r;
        if (!Ready(app)) return MMS_E_BUSY;
        if (!HasParent(app) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        const auto* model = mdl::Mdl(app.ModelSlot(app.CameraParentModel()));
        if (value < 0 || std::uint32_t(value) >= model->boneCount || !Selectable(model->boneTable[value])) return MMS_E_ARGUMENT;
        auto change = std::make_shared<ParentChange>(app, value);
        if (auto r = history->Revision(change->initialRevision)) return r;
        if (!change->InitialCurrent()) return MMS_E_STALE;
        if (change->before == change->after) return MMS_OK;
        out = std::move(change); return MMS_OK;
    });
}
MmsResult ApplyBoundCameraParentBone(MMDApp& app, std::int32_t value, bool& changed) noexcept {
    changed = false; return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if(!HasParent(app)||!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
        // Pin the original target before draft collection sends any native
        // reads. A callback may retire this source without changing binding.
        // No-op actions need the same guard before their selection effect.
        ParentSource original(app);
        const auto binding=BoundNativePoseGeneration(&app);
        const auto object=original.Generation();const auto frame=app.CurrentFrame(),before=app.CameraParentBone();
        const auto type=value>=0&&std::uint32_t(value)<original.count?original.bones[value].type:mdl::BoneType::InertTip;
        std::uint64_t revision=0;if(auto r=history->Revision(revision))return r;
        const auto current=[&]() noexcept {
            std::uint64_t now=0;
            return BoundNativePoseHistory(&app)==history&&BoundNativePoseGeneration(&app)==binding&&original.Current()&&
                original.Generation()==object&&app.CurrentFrame()==frame&&app.CameraParentBone()==before&&
                value>=0&&std::uint32_t(value)<original.count&&original.bones[value].type==type&&
                history->Revision(now)==MMS_OK&&now==revision;
        };
        std::shared_ptr<HistoryChange> change;if(auto r=PlanCameraParentBone(app,value,change))return r;
        if(!current())return MMS_E_STALE;
        ScopedFollowReadoutDrafts drafts(app);
        if(!current())return MMS_E_STALE;
        if (change) {
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative("Camera parent bone", change, transaction, SharedHistory::NativeIntent::Adjustment)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
            changed = true;
        }
        Boundary([&]() -> MmsResult { Accepted(app, *history, changed); return MMS_OK; });
        return MMS_OK;
    });
}
void RefreshBoundCameraParentBoneReadout(MMDApp& app) noexcept {
    if (!BoundNativePoseHistory(&app) || reading == &app) return;
    bake::ScopedUiInputGate input;
    struct Scope { MMDApp* previous; explicit Scope(MMDApp& a) : previous(reading) { reading = &a; } ~Scope() { reading = previous; } } scope(app);
    const auto result = Boundary([&]() -> MmsResult {
        if (!HasParent(app)) {
            const auto main=app.MainWindow(),combo=GetDlgItem(main,panel::kBoneRegisterCombo),models=GetDlgItem(main,panel::kMainComboNormal);
            if(!combo)return MMS_OK;
            const auto binding=BoundNativePoseGeneration(&app),scene=app.SceneGeneration(),pool=mdl::KeyPoolIdentity(app.CameraKeys());
            const auto parent=app.CameraParentModel(),bone=app.CameraParentBone();
            const auto current=[&]() noexcept{return BoundNativePoseHistory(&app)&&BoundNativePoseGeneration(&app)==binding&&app.SceneGeneration()==scene&&
                mdl::KeyPoolIdentity(app.CameraKeys())==pool&&app.MainWindow()==main&&GetDlgItem(main,panel::kBoneRegisterCombo)==combo&&
                GetDlgItem(main,panel::kMainComboNormal)==models&&app.CameraParentModel()==parent&&app.CameraParentBone()==bone&&!HasParent(app);};
            const auto count=SendMessageW(combo,CB_GETCOUNT,0,0);if(!current())return MMS_E_STALE;
            if(count>0){SendMessageW(combo,CB_RESETCONTENT,0,0);if(!current())return MMS_E_STALE;}
            if(SendMessageW(combo,CB_GETCOUNT,0,0)!=0||!current())return MMS_E_NOT_READY;
            if(models){const LRESULT row=parent<0?0:-1;
                const auto selected=SendMessageW(models,CB_GETCURSEL,0,0);if(!current())return MMS_E_STALE;
                if(selected!=row){SendMessageW(models,CB_SETCURSEL,row,0);if(!current())return MMS_E_STALE;}
                if(SendMessageW(models,CB_GETCURSEL,0,0)!=row||!current())return MMS_E_NOT_READY;}
            KillTimer(combo,reinterpret_cast<UINT_PTR>(&RepairProc));return MMS_OK;
        }
        ParentSource source(app,false); Controls controls(source); if (!controls.Current()) return MMS_E_STALE;
        const auto order = Order(app);
        auto row = ModelRow(controls, order);
        if (row < 1) {
            std::vector<Row> models{{UiUsesEnglishNames(app.EnglishUI()) ? L"non" : L"\x306a\x3057", -1}};
            for (std::size_t i = 1; i <= order.count; ++i) {
                const auto slot = order.rows[i] - 1;
                models.push_back({text_encoding::ModelLabel(*mdl::Mdl(app.ModelSlot(slot)), UiUsesEnglishNames(app.EnglishUI())), slot});
            }
            if (!controls.Current() || !Rebuild(controls, controls.models, models)) return MMS_E_NOT_READY;
            row = ModelRow(controls, order); if (row < 1) return MMS_E_NOT_READY;
        }
        if (controls.Send(controls.models, CB_GETCURSEL) != row && controls.Send(controls.models, CB_SETCURSEL, row) != row) return MMS_E_NOT_READY;
        if (!BoneList(controls)) {
            if(!controls.Current())return MMS_E_STALE;
            const auto rows=BoneRows(controls);
            if(!Rebuild(controls, controls.bones, rows)||!BoneList(controls))return MMS_E_NOT_READY;
        }
        if (!SelectByData(controls, controls.bones, controls.value)) return MMS_E_NOT_READY;
        KillTimer(controls.bones, reinterpret_cast<UINT_PTR>(&RepairProc));
        return MMS_OK;
    });
    if (result) { RestoreFast(app); Retry(app); }
}
bool TrySelectBoundCameraParentBone(MMDApp* app, HWND combo) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    const auto result = Boundary([&]() -> MmsResult {
        if (!HasParent(*app)) return MMS_E_NOT_READY;
        ParentSource source(*app); Controls controls(source);
        const auto original = app->CameraParentBone(); const int frame = app->CurrentFrame();
        std::uint64_t revision = 0; if (auto r = source.history->Revision(revision)) return r;
        if (!controls.Current() || combo != controls.bones) return MMS_E_ARGUMENT;
        const auto order = Order(*app); const int parent = ModelRow(controls, order);
        if (parent < 1 || controls.Send(controls.models, CB_GETCURSEL) != parent) return MMS_E_STALE;
        if (!BoneList(controls)) return MMS_E_STALE;
        const auto row = controls.Send(combo, CB_GETCURSEL); if (row < 0) return MMS_E_ARGUMENT;
        const auto value = controls.Send(combo, CB_GETITEMDATA, row);
        if (value < 0 || value > INT32_MAX || std::uint64_t(value) >= source.count || !Selectable(source.bones[value])) return MMS_E_ARGUMENT;
        COMBOBOXINFO info{sizeof(info)};
        if (!GetComboBoxInfo(combo, &info) || !controls.Current() || info.hwndCombo != combo) return MMS_E_STALE;
        CaptureAdmission admission(*app, combo, info.hwndList);
        ScopedFollowReadoutDrafts drafts(*app);
        std::uint64_t currentRevision = 0;
        if (!controls.Current() || app->CameraParentBone() != original || app->CurrentFrame() != frame ||
            source.history->Revision(currentRevision) != MMS_OK || currentRevision != revision) return MMS_E_STALE;
        bool changed = false; if (auto r = ApplyBoundCameraParentBone(*app, std::int32_t(value), changed)) return r;
        if (controls.CurrentIdentity()) { bake::ScopedUiInputGate input; SetFocus(controls.main); }
        return MMS_OK;
    });
    if (result) { runtime_log::Write("controlled camera parent bone refused: %d", int(result)); RefreshBoundCameraParentBoneReadout(*app); }
    return true;
}
}
#endif
