#include "native_accessory_parent_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "history_runtime.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/accessory_layout.hpp"
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
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <CommCtrl.h>
#include <array>
#include <climits>
#include <cstring>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
constexpr wchar_t kCaptureOwner[] = L"MMS.Native.AccessoryParentCapture.V1";
struct CaptureAdmission {
    inline static thread_local CaptureAdmission* active = nullptr;
    MMDApp& app; const HWND main, combo, list, capture;
    CaptureAdmission* previous; HANDLE previousOwner = nullptr; bool tagged = false;
    CaptureAdmission(MMDApp& a, HWND c, HWND l)
        : app(a), main(a.MainWindow()), combo(c), list(l), capture(GetCapture()), previous(active) {
        if (capture && (capture == combo || capture == list)) {
            previousOwner = GetPropW(combo, kCaptureOwner);
            if (!SetPropW(combo, kCaptureOwner, reinterpret_cast<HANDLE>(this))) throw std::bad_alloc();
            tagged = true;
        }
        active = this;
    }
    ~CaptureAdmission() {
        if (tagged && GetPropW(combo, kCaptureOwner) == reinterpret_cast<HANDLE>(this)) {
            if (previousOwner) SetPropW(combo, kCaptureOwner, previousOwner); else RemovePropW(combo, kCaptureOwner);
        }
        active = previous;
    }
    bool Allows(HWND current) const noexcept {
        return tagged && current == capture && (current == combo || current == list) && app.MainWindow() == main &&
            GetPropW(combo, kCaptureOwner) == reinterpret_cast<HANDLE>(const_cast<CaptureAdmission*>(this));
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
bool Listed(const mdl::BoneRecord& bone) noexcept {
    return bone.type < mdl::BoneType::InertTip || bone.type == mdl::BoneType::FixedAxis;
}
struct Attachment {
    std::int32_t model, bone;
    bool operator==(const Attachment& other) const noexcept { return model == other.model && bone == other.bone; }
};
Attachment Read(const mdl::AccessoryRecord& object) noexcept { return {object.parentModel, object.parentBone}; }
using RecordBytes = std::array<unsigned char, sizeof(mdl::AccessoryRecord)>;
RecordBytes Bytes(const mdl::AccessoryRecord& object) noexcept {
    RecordBytes out; std::memcpy(out.data(), &object, sizeof(object)); return out;
}
struct Parent {
    int slot = -1; unsigned char* raw = nullptr; mdl::BoneRecord* bones = nullptr; std::uint32_t count = 0;
    MmsObjectRef identity{}; std::shared_ptr<RetainedSceneObject> owner;
    bool Current(MMDApp& app) const noexcept {
        if (slot < 0 || slot >= kModelSlotCount || app.ModelSlot(slot) != raw || !owner || !owner->Present() ||
            !SceneObjectPresent(raw, identity.id)) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (int(i) != slot && app.ModelSlot(i) == raw) return false;
        return mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == count;
    }
    std::uint64_t Generation() const noexcept {
        MmsObjectRef value{}; return FindSceneObjectIdentity(raw, value) ? value.generation : 0;
    }
    MmsResult Capture(MMDApp& app, int value) {
        if (value < 0 || value >= kModelSlotCount || !app.ModelSlot(value)) return MMS_E_ARGUMENT;
        slot = value; raw = app.ModelSlot(slot); bones = mdl::Mdl(raw)->boneTable; count = mdl::Mdl(raw)->boneCount;
        if (!bones || !count) return MMS_E_ARGUMENT;
        owner = RetainSceneObject(app, MMS_OBJECT_MODEL, unsigned(slot));
        if (!FindSceneObjectIdentity(raw, identity)) return MMS_E_STALE;
        return Current(app) ? MMS_OK : MMS_E_STALE;
    }
};
struct Change final : HistoryChange, std::enable_shared_from_this<Change> {
    MMDApp& app; SharedHistory* const history;
    const unsigned slot; mdl::AccessoryRecord* const object;
    const std::uint64_t scene, pool, binding, initialObject;
    const int frame; const HWND capture;
    const Attachment before; Attachment after;
    const RecordBytes initialBytes;
    std::shared_ptr<RetainedSceneObject> owner; MmsObjectRef identity{};
    std::array<Parent, 2> parents; std::array<std::uint64_t, 2> initialParents{};
    unsigned parentCount = 0; std::uint64_t initialRevision = 0, serial = 1;
    bool applied = false, published = false, boneOnly = false;
    explicit Change(MMDApp& a)
        : app(a), history(BoundNativePoseHistory(&a)), slot(a.SelectedAccessorySlot()), object(a.AccessorySlot(slot)),
          scene(a.SceneGeneration()), pool(mdl::KeyPoolIdentity(a.AccessoryKeys(slot))), binding(BoundNativePoseGeneration(&a)),
          initialObject(SceneObjectIdentity(object).generation), frame(a.CurrentFrame()), capture(GetCapture()),
          before(Read(*object)), after(before), initialBytes(Bytes(*object)), owner(RetainSceneObject(a, MMS_OBJECT_ACCESSORY, slot)) {
        if (!FindSceneObjectIdentity(object, identity)) throw std::invalid_argument("accessory attachment identity");
    }
    std::uint64_t Generation() const noexcept {
        MmsObjectRef value{}; return FindSceneObjectIdentity(object, value) ? value.generation : 0;
    }
    MmsResult AddParent(Attachment value) {
        if (value.model == -1) return MMS_OK; // Detached bone bytes have no referent.
        if (value.model < 0 || value.bone < 0) return MMS_E_ARGUMENT;
        for (unsigned i = 0; i < parentCount; ++i) if (parents[i].slot == value.model)
            return unsigned(value.bone) < parents[i].count ? MMS_OK : MMS_E_ARGUMENT;
        if (parentCount == parents.size()) return MMS_E_FAILED;
        if (auto r = parents[parentCount].Capture(app, value.model)) return r;
        if (unsigned(value.bone) >= parents[parentCount].count) return MMS_E_ARGUMENT;
        initialParents[parentCount] = parents[parentCount].Generation(); ++parentCount; return MMS_OK;
    }
    bool SourcesCurrent() const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || !pool ||
            app.AccessorySlot(slot) != object || !owner || !owner->Present() || !SceneObjectPresent(object, identity.id) ||
            mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) != pool || !(Read(*object) == (applied ? after : before))) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (i != slot && app.AccessorySlot(i) == object) return false;
        for (unsigned i = 0; i < parentCount; ++i) if (!parents[i].Current(app)) return false;
        if (boneOnly) {
            const auto* parent = mdl::Mdl(app.ModelSlot(after.model));
            if (!parent || unsigned(after.bone) >= parent->boneCount || !Listed(parent->boneTable[after.bone])) return false;
        }
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (published) return true;
        std::uint64_t revision = 0;
        if (!SourcesCurrent() || BoundNativePoseGeneration(&app) != binding || Generation() != initialObject ||
            app.CurrentFrame() != frame || app.SelectedAccessorySlot() != slot || GetCapture() != capture ||
            Bytes(*object) != initialBytes || history->Revision(revision) != MMS_OK || revision != initialRevision) return false;
        for (unsigned i = 0; i < parentCount; ++i) if (parents[i].Generation() != initialParents[i]) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<Change> change; const bool forward;
        const std::uint64_t serial, binding, object; const int frame; const HWND capture;
        const RecordBytes expected; std::array<std::uint64_t, 2> parents{};
        std::uint64_t revision = 0, bytes = 0; mutable resource_budget::Collector resources;
        bool consumed = false;
        Candidate(std::shared_ptr<Change> c, bool direction)
            : change(std::move(c)), forward(direction), serial(change->serial), binding(BoundNativePoseGeneration(&change->app)),
              object(change->Generation()), frame(change->app.CurrentFrame()), capture(GetCapture()), expected(Bytes(*change->object)) {
            for (unsigned i = 0; i < change->parentCount; ++i) parents[i] = change->parents[i].Generation();
        }
        bool IsCurrent() const noexcept override {
            const auto& c = *change; std::uint64_t now = 0;
            if (consumed || serial != c.serial || forward == c.applied || !c.SourcesCurrent() || !c.InitialCurrent() ||
                !Ready(c.app, true) || binding != BoundNativePoseGeneration(&c.app) || object != c.Generation() ||
                frame != c.app.CurrentFrame() || capture != GetCapture() || expected != Bytes(*c.object) ||
                c.history->Revision(now) != MMS_OK || now != revision || !resources.Result().Complete() ||
                !c.owner->CoversBookkeeping(resources)) return false;
            for (unsigned i = 0; i < c.parentCount; ++i)
                if (parents[i] != c.parents[i].Generation() || !c.parents[i].owner->CoversBookkeeping(resources)) return false;
            return true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& c = *change; const auto& target = forward ? c.after : c.before;
            c.object->parentModel = target.model; c.object->parentBone = target.bone;
            c.applied = forward; c.published = true; ++c.serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!SourcesCurrent() || !InitialCurrent() || forward == applied) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (auto r = history->Revision(candidate->revision)) return r;
            if (auto r = owner->MeasureBookkeeping(candidate->resources)) return r;
            for (unsigned i = 0; i < parentCount; ++i)
                if (auto r = parents[i].owner->MeasureBookkeeping(candidate->resources)) return r;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RetainedBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult MakeChange(MMDApp& app, bool model, int value, std::shared_ptr<Change>& out) {
    auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
    if (auto r = history->CheckNativeNavigation()) return r;
    if (auto r = history->CheckNativeEdit()) return r;
    if (!Ready(app)) return MMS_E_BUSY;
    const auto slot = unsigned(app.SelectedAccessorySlot());
    if (slot >= kModelSlotCount || !app.AccessorySlot(slot) || !mdl::KeyPoolIdentity(app.AccessoryKeys(slot))) return MMS_E_NOT_READY;
    auto change = std::make_shared<Change>(app);
    change->boneOnly = !model;
    if (auto r = history->Revision(change->initialRevision)) return r;
    if (auto r = change->AddParent(change->before)) return r;
    if (model) {
        if (value < -1 || value >= kModelSlotCount) return MMS_E_ARGUMENT;
        change->after.model = value;
        if (value >= 0) change->after.bone = 0;
    } else {
        if (change->before.model < 0 || value < 0) return MMS_E_ARGUMENT;
        change->after.bone = value;
    }
    if (auto r = change->AddParent(change->after)) return r;
    if (!model) {
        const auto* parent = mdl::Mdl(app.ModelSlot(change->after.model));
        if (!Listed(parent->boneTable[value])) return MMS_E_ARGUMENT;
    }
    if (!change->InitialCurrent()) return MMS_E_STALE;
    out = std::move(change); return MMS_OK;
}

// Stack watches survive synchronous destruction of their own HWND. They also
// distinguish a native same-handle replacement from the original control.
struct ComboWatch {
    HWND window; bool alive = true; std::uint64_t mutations = 0, selections = 0;
    static LRESULT CALLBACK Observe(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<ComboWatch*>(data);
        if (m == WM_NCDESTROY) { self.alive = false; RemoveWindowSubclass(h, Observe, id); }
        if (m == CB_RESETCONTENT || m == CB_ADDSTRING || m == CB_INSERTSTRING || m == CB_DELETESTRING || m == CB_SETITEMDATA)
            ++self.mutations;
        if (m == CB_SETCURSEL) ++self.selections;
        return DefSubclassProc(h, m, w, l);
    }
    explicit ComboWatch(HWND h) : window(h) {
        if (!h || !IsWindow(h) || !SetWindowSubclass(h, Observe, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
            throw std::bad_alloc();
    }
    ~ComboWatch() { if (alive) RemoveWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this)); }
    bool Current() const noexcept {
        DWORD_PTR value = 0;
        return alive && GetWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this), &value) && value == reinterpret_cast<DWORD_PTR>(this);
    }
};
using Projection = model_order::Projection<kModelSlotCount>;
Projection Order(MMDApp& app) noexcept {
    return model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
        const auto* object = app.ModelSlot(int(slot)); return object ? int(mdl::Mdl(object)->comboSelIndex) : -1;
    });
}
struct Input {
    MMDApp& app; SharedHistory* const history; const HWND main, models, bones;
    const unsigned slot; mdl::AccessoryRecord* const accessory; const MmsObjectRef identity;
    const std::uint64_t scene, binding, pool; const int frame, language;
    const Attachment attachment; const Projection order;
    ComboWatch modelWatch, boneWatch;
    struct Source { unsigned char* raw = nullptr; MmsObjectRef id{}; mdl::BoneRecord* bones = nullptr; std::uint32_t count = 0; int rank = 0; };
    std::array<Source, kModelSlotCount> sources{}; std::uint64_t revision = 0;
    explicit Input(MMDApp& a)
        : app(a), history(BoundNativePoseHistory(&a)), main(a.MainWindow()), models(GetDlgItem(main, panel::kMainComboGround)),
          bones(GetDlgItem(main, panel::kAttachBoneCombo)), slot(a.SelectedAccessorySlot()), accessory(slot < kModelSlotCount ? a.AccessorySlot(slot) : nullptr),
          identity(accessory ? SceneObjectIdentity(accessory) : MmsObjectRef{}), scene(a.SceneGeneration()),
          binding(BoundNativePoseGeneration(&a)), pool(slot < kModelSlotCount ? mdl::KeyPoolIdentity(a.AccessoryKeys(slot)) : 0), frame(a.CurrentFrame()),
          language(a.EnglishUI()), attachment(accessory ? Read(*accessory) : Attachment{-1, 0}), order(Order(a)),
          modelWatch(models), boneWatch(bones) {
        if (!history || history->Revision(revision) != MMS_OK) throw MMS_E_NOT_READY;
        for (unsigned i = 0; i < sources.size(); ++i) if (auto* raw = a.ModelSlot(i)) {
            auto& s = sources[i]; s.raw = raw; s.id = SceneObjectIdentity(raw); const auto& record = *mdl::Mdl(raw);
            s.bones = record.boneTable; s.count = record.boneCount; s.rank = record.comboSelIndex;
            for (unsigned j = 0; j < i; ++j) if (sources[j].raw == raw) throw MMS_E_STALE;
        }
    }
    bool IdentityCurrent() const noexcept {
        MmsObjectRef current{}; std::uint64_t now = 0;
        if (!history || !accessory || !pool || BoundNativePoseHistory(&app) != history || BoundNativePoseGeneration(&app) != binding ||
            app.SceneGeneration() != scene || app.MainWindow() != main || !IsWindow(main) || app.SelectedAccessorySlot() != slot ||
            app.AccessorySlot(slot) != accessory || !FindSceneObjectIdentity(accessory, current) || current.generation != identity.generation ||
            current.id.lo != identity.id.lo || current.id.hi != identity.id.hi || mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) != pool ||
            app.CurrentFrame() != frame || app.EnglishUI() != language || GetDlgItem(main, panel::kMainComboGround) != models ||
            GetDlgItem(main, panel::kAttachBoneCombo) != bones || !modelWatch.Current() || !boneWatch.Current() ||
            history->Revision(now) != MMS_OK || now != revision) return false;
        for (unsigned i = 0; i < sources.size(); ++i) {
            const auto& s = sources[i]; if (app.ModelSlot(i) != s.raw) return false;
            if (!s.raw) continue;
            if (!FindSceneObjectIdentity(s.raw, current) || current.generation != s.id.generation || current.id.lo != s.id.id.lo || current.id.hi != s.id.id.hi ||
                mdl::Mdl(s.raw)->boneTable != s.bones || mdl::Mdl(s.raw)->boneCount != s.count || mdl::Mdl(s.raw)->comboSelIndex != s.rank) return false;
        }
        return true;
    }
    bool Current() const noexcept {
        return IdentityCurrent() && Read(*accessory) == attachment && !modelWatch.mutations && !modelWatch.selections &&
            !boneWatch.mutations && !boneWatch.selections;
    }
    LRESULT Send(HWND target, UINT message, WPARAM w = 0, LPARAM l = 0) const {
        if (!Current()) throw MMS_E_STALE;
        const auto result = SendMessageW(target, message, w, l);
        if (!Current()) throw MMS_E_STALE;
        return result;
    }
    int ModelRow() const {
        const auto count = Send(models, CB_GETCOUNT);
        if (count != LRESULT(order.count + 1)) return -1;
        std::array<LRESULT, kModelSlotCount + 1> items{};
        for (LRESULT row = 0; row < count; ++row) items[std::size_t(row)] = Send(models, CB_GETITEMDATA, row);
        if (!order.count) { if (items[0] != -1 && items[0] != 0) return -1; }
        else if (model_order::Resolve(order, 1, count, [&](std::size_t row) noexcept { return items[row]; }) < 0) return -1;
        const auto row = Send(models, CB_GETCURSEL);
        if (row < 0 || row >= count) return -1;
        return int(row);
    }
};
template<class F> MmsResult ResultBoundary(F&& f) noexcept {
    return Boundary([&]() -> MmsResult { try { return f(); } catch (MmsResult r) { return r; } });
}

struct Row { std::wstring text; int bone; };
bool RestoreFast(Input& input) noexcept {
    // Accepted input has not changed either list yet. Budget/OOM rejection can
    // restore the two selections using only stack storage and native messages.
    return ResultBoundary([&]() -> MmsResult {
        if (!input.Current()) return MMS_E_STALE;
        const auto count = input.Send(input.models, CB_GETCOUNT);
        if (count < 1 || count > kModelSlotCount + 1) return MMS_E_STALE;
        std::array<LRESULT, kModelSlotCount + 1> mapping{};
        for (LRESULT i = 0; i < count; ++i) mapping[std::size_t(i)] = input.Send(input.models, CB_GETITEMDATA, i);
        const auto row = input.attachment.model < 0 ? 0 : model_order::FindRow(input.order, unsigned(input.attachment.model), count,
            [&](std::size_t i) noexcept { return mapping[i]; });
        if (row < 0) return MMS_E_STALE;
        const auto bones = input.Send(input.bones, CB_GETCOUNT); LRESULT selected = -1;
        for (LRESULT i = 0; i < bones; ++i) if (input.Send(input.bones, CB_GETITEMDATA, i) == input.attachment.bone) selected = i;
        const auto identity = [&]() noexcept { return input.IdentityCurrent() && Read(*input.accessory) == input.attachment &&
            !input.modelWatch.mutations && !input.boneWatch.mutations; };
        bake::ScopedUiInputGate gate;
        const auto modelSelections = input.modelWatch.selections, boneSelections = input.boneWatch.selections;
        if (!identity()) return MMS_E_STALE;
        SendMessageW(input.models, CB_SETCURSEL, row, 0);
        if (!identity() || input.modelWatch.selections != modelSelections + 1 || input.boneWatch.selections != boneSelections) return MMS_E_STALE;
        SendMessageW(input.bones, CB_SETCURSEL, selected, 0);
        return identity() && input.modelWatch.selections == modelSelections + 1 && input.boneWatch.selections == boneSelections + 1 ? MMS_OK : MMS_E_STALE;
    }) == MMS_OK;
}
// A readout never enters the edit gate and never reads or replaces EDIT text.
// Prepare all labels before clearing the old list. Revalidate after every
// native message, including subclass callbacks and synchronous destruction.
bool Readout(MMDApp& app, bool firstRow = false) noexcept {
    return ResultBoundary([&]() -> MmsResult {
        Input input(app); if (!input.Current()) return MMS_E_STALE;
        const auto& binding = input.attachment;
        const mdl::ModelRecord* model = nullptr;
        if (binding.model >= 0 && binding.model < kModelSlotCount && input.sources[binding.model].raw)
            model = mdl::Mdl(input.sources[binding.model].raw);
        const auto count = input.Send(input.models, CB_GETCOUNT);
        std::array<LRESULT, kModelSlotCount + 1> mapping{};
        if (count < 1 || count > kModelSlotCount + 1) return MMS_E_STALE;
        for (LRESULT i = 0; i < count; ++i) mapping[std::size_t(i)] = input.Send(input.models, CB_GETITEMDATA, i);
        const auto parentRow = binding.model < 0 ? 0 : model_order::FindRow(input.order, unsigned(binding.model), count,
            [&](std::size_t i) noexcept { return mapping[i]; });
        std::vector<Row> rows;
        if (model && model->boneTable) for (std::uint32_t i = 0; i < model->boneCount; ++i)
            if (Listed(model->boneTable[i])) rows.push_back({text_encoding::BoneLabel(model->boneTable[i], UiUsesEnglishNames(input.language)), int(i)});
        if (!input.Current()) return MMS_E_STALE;
        bake::ScopedUiInputGate gate;
        auto current = [&]() noexcept { return input.IdentityCurrent() && Read(*input.accessory) == binding; };
        const auto write = [&](HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) -> LRESULT {
            if (!current()) throw MMS_E_STALE;
            const auto models = input.modelWatch.mutations, bones = input.boneWatch.mutations;
            const auto modelSelections = input.modelWatch.selections, boneSelections = input.boneWatch.selections;
            const auto result = SendMessageW(window, message, w, l);
            const bool mutation = message == CB_RESETCONTENT || message == CB_ADDSTRING || message == CB_SETITEMDATA;
            if (!current() || input.modelWatch.mutations != models || input.boneWatch.mutations != bones + (mutation ? 1 : 0) ||
                input.modelWatch.selections != modelSelections + (message == CB_SETCURSEL && window == input.models ? 1 : 0) ||
                input.boneWatch.selections != boneSelections + (message == CB_SETCURSEL && window == input.bones ? 1 : 0)) throw MMS_E_STALE;
            return result;
        };
        // The current lists may be stale after a rejection. Rebuild only this
        // bounded pair; native number drafts, focus and timeline stay intact.
        if (write(input.models, CB_SETCURSEL, parentRow) == CB_ERR && parentRow >= 0) return MMS_E_FAILED;
        write(input.bones, CB_RESETCONTENT);
        int selected = -1;
        for (const auto& row : rows) {
            const auto at = write(input.bones, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.text.c_str()));
            if (at < 0 || write(input.bones, CB_SETITEMDATA, at, row.bone) == CB_ERR) return MMS_E_MEMORY;
            if (row.bone == binding.bone) selected = int(at);
        }
        if (firstRow && !rows.empty()) selected = 0;
        write(input.bones, CB_SETCURSEL, selected);
        return MMS_OK;
    }) == MMS_OK;
}
void Accepted(MMDApp& app, SharedHistory& history, bool changed, bool activate, bool firstRow) noexcept {
    bake::ScopedUiInputGate inputGate;
    const auto binding = BoundNativePoseGeneration(&app), scene = app.SceneGeneration();
    const auto slot = unsigned(app.SelectedAccessorySlot()); if (slot >= kModelSlotCount) return;
    auto* object = app.AccessorySlot(slot);
    MmsObjectRef identity{}; if (!object || !FindSceneObjectIdentity(object, identity)) return;
    const auto value = Read(*object); const auto main = app.MainWindow();
    const auto current = [&]() noexcept {
        return BoundNativePoseHistory(&app) == &history && BoundNativePoseGeneration(&app) == binding &&
            app.SceneGeneration() == scene && app.MainWindow() == main && app.SelectedAccessorySlot() == slot &&
            app.AccessorySlot(slot) == object && SceneObjectPresent(object, identity.id) && Read(*object) == value;
    };
    bool selection = false;
    if (activate && !object->rowSelected) {
        app.ClearGlobalTimelineTrackSelection();
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (auto* accessory = app.AccessorySlot(i)) accessory->rowSelected = 0;
        object->rowSelected = 1; selection = true;
    }
    if (changed) PublishBoundPoseEdit(app, history, false);
    if (!current()) return;
    if (!Readout(app, firstRow)) RequestBoundHistoryUiRefresh(&app);
    if (!current()) return;
    if (selection && app.CameraMode() && app.Renderer() && app.PanelDC()) {
        PostLanguageSweep(&app); if (!current()) return;
    }
    if (main) InvalidateRect(main, nullptr, FALSE);
    updates::RequestViewport(&app, false);
}
MmsResult Apply(MMDApp& app, bool model, int value, bool& changed, const Input* input = nullptr) {
    changed = false;
    std::shared_ptr<Change> change; if (auto r = MakeChange(app, model, value, change)) return r;
    if (input && !input->Current()) return MMS_E_STALE;
    if (!(change->before == change->after)) {
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto r = change->history->PrepareNative(model ? "Accessory parent model" : "Accessory parent bone", change, transaction,
                                                   SharedHistory::NativeIntent::Adjustment)) return r;
        if (input && !input->Current()) return MMS_E_STALE;
        if (auto r = change->history->CommitNative(std::move(transaction))) return r;
        changed = true;
    }
    // Native attach activates this accessory even when the value was unchanged;
    // native detach only clears the list. Neither action creates a key.
    Accepted(app, *change->history, changed, !model || value >= 0, model && value >= 0);
    return MMS_OK;
}
bool Select(MMDApp* app, HWND combo, bool model) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool restored = false;
    const auto result = ResultBoundary([&]() -> MmsResult {
        Input input(*app);
        const auto operation = ResultBoundary([&]() -> MmsResult {
        if (!input.Current() || combo != (model ? input.models : input.bones)) return MMS_E_ARGUMENT;
        const int row = input.ModelRow(); if (row < 0) return MMS_E_STALE;
        int value = -1;
        if (model) {
            if (row) { value = int(input.Send(input.models, CB_GETITEMDATA, row)); if (value < 0 || value >= kModelSlotCount) return MMS_E_STALE; }
        } else {
            if (input.attachment.model < 0 || input.attachment.model >= kModelSlotCount || !input.sources[input.attachment.model].raw) return MMS_E_ARGUMENT;
            const auto mapped = row ? input.Send(input.models, CB_GETITEMDATA, row) : -1;
            if (mapped != input.attachment.model) return MMS_E_STALE;
            const auto& source = input.sources[input.attachment.model];
            if (!source.bones) return MMS_E_ARGUMENT;
            const auto count = input.Send(input.bones, CB_GETCOUNT); LRESULT listed = 0;
            for (std::uint32_t i = 0; i < source.count; ++i) if (Listed(source.bones[i])) ++listed;
            if (listed != count) return MMS_E_STALE;
            std::vector<unsigned char> seen(source.count);
            for (LRESULT i = 0; i < count; ++i) {
                const auto value = input.Send(input.bones, CB_GETITEMDATA, i);
                if (value < 0 || std::uint64_t(value) >= source.count || !Listed(source.bones[value]) || seen[std::size_t(value)]++) return MMS_E_STALE;
            }
            const auto selected = input.Send(input.bones, CB_GETCURSEL); if (selected < 0 || selected >= count) return MMS_E_ARGUMENT;
            const auto bone = input.Send(input.bones, CB_GETITEMDATA, selected);
            if (bone < 0 || bone > INT32_MAX || std::uint64_t(bone) >= source.count) return MMS_E_ARGUMENT;
            value = int(bone);
        }
        COMBOBOXINFO info{sizeof(info)};
        if (!GetComboBoxInfo(combo, &info) || info.hwndCombo != combo || !input.Current()) return MMS_E_STALE;
        CaptureAdmission admission(*app, combo, info.hwndList);
        bool changed = false; if (auto r = Apply(*app, model, value, changed, &input)) return r;
        // The accepted publication changes the journal revision, so only test
        // native window identity here, never the old source revision.
        if (input.modelWatch.Current() && input.boneWatch.Current() && app->MainWindow() == input.main &&
            GetDlgItem(input.main, panel::kMainComboGround) == input.models && GetDlgItem(input.main, panel::kAttachBoneCombo) == input.bones) {
            bake::ScopedUiInputGate gate; SetFocus(input.main);
        }
        return MMS_OK;
        });
        if (operation) restored = RestoreFast(input);
        return operation;
    });
    if (result) {
        runtime_log::Write("controlled accessory parent %s refused: %d", model ? "model" : "bone", int(result));
        if (!restored) RefreshBoundAccessoryParentReadout(*app);
    }
    return true;
}
}
MmsResult PlanAccessoryParentModel(MMDApp& app, std::int32_t value, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult { std::shared_ptr<Change> change; if (auto r = MakeChange(app, true, value, change)) return r;
        if (!(change->before == change->after)) out = std::move(change); return MMS_OK; });
}
MmsResult PlanAccessoryParentBone(MMDApp& app, std::int32_t value, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult { std::shared_ptr<Change> change; if (auto r = MakeChange(app, false, value, change)) return r;
        if (!(change->before == change->after)) out = std::move(change); return MMS_OK; });
}
MmsResult ApplyBoundAccessoryParentModel(MMDApp& app, std::int32_t value, bool& changed) noexcept {
    changed = false; return Boundary([&] { return Apply(app, true, value, changed); });
}
MmsResult ApplyBoundAccessoryParentBone(MMDApp& app, std::int32_t value, bool& changed) noexcept {
    changed = false; return Boundary([&] { return Apply(app, false, value, changed); });
}
bool TrySelectBoundAccessoryParentModel(MMDApp* app, HWND combo) noexcept { return Select(app, combo, true); }
bool TrySelectBoundAccessoryParentBone(MMDApp* app, HWND combo) noexcept { return Select(app, combo, false); }
void RefreshBoundAccessoryParentReadout(MMDApp& app) noexcept { if (BoundNativePoseHistory(&app)) Readout(app); }
}
#endif
