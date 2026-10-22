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
using Attachment = AccessoryAttachmentValue;
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
struct ReceiptParent {
    int slot = -1; unsigned char* raw = nullptr; MmsId id{};
    mdl::BoneRecord* bones = nullptr; std::uint32_t count = 0;
    bool Current(MMDApp& app) const noexcept {
        if (slot < 0) return slot == -1;
        if (slot >= kModelSlotCount || app.ModelSlot(slot) != raw || !SceneObjectPresent(raw, id)) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (int(i) != slot && app.ModelSlot(i) == raw) return false;
        return mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == count;
    }
};
struct AttachmentCell {
    MMDApp* app = nullptr; mdl::AccessoryRecord* object = nullptr; MmsId id{};
    std::uint64_t scene = 0, pool = 0, epoch = 0, serial = 0;
    Attachment logical{}, actual{}; ReceiptParent logicalParent, actualParent;
    bool initialized = false;
};
struct AttachmentDomain final : NativeHistoryDomain {
    std::uint64_t epoch = 1;
    std::array<std::shared_ptr<AttachmentCell>, kModelSlotCount> active{};
    std::array<std::uint64_t, kModelSlotCount> publications{};
    void Reset() noexcept override {
        if (epoch != UINT64_MAX) ++epoch; // Exhausted epochs stay fail-closed.
        for (auto& cell : active) cell.reset();
        for (auto& serial : publications) if (serial != UINT64_MAX) ++serial;
    }
};
const unsigned char kAttachmentDomain = 0;
bool SameId(MmsId a, MmsId b) noexcept { return a.lo == b.lo && a.hi == b.hi; }
}

struct AccessoryAttachmentHistory::Impl {
    MMDApp& app; SharedHistory& history; const unsigned slot;
    mdl::AccessoryRecord* const object; const MmsId id;
    const std::uint64_t scene, pool, epoch, historyEpoch, initialSlotPublication;
    std::shared_ptr<AttachmentDomain> domain; std::shared_ptr<AttachmentCell> cell;
    std::shared_ptr<RetainedSceneObject> owner;
    // A node keeps its logical endpoints alive even when its actual endpoints
    // were an unrecorded draft on another model. Cells themselves own no model.
    std::array<Parent, 2> logicalParents;
    unsigned logicalParentCount = 0;
    Impl(MMDApp& a, SharedHistory& h, unsigned s, std::shared_ptr<AttachmentDomain> d, std::shared_ptr<AttachmentCell> c)
        : app(a), history(h), slot(s), object(a.AccessorySlot(s)), id(SceneObjectIdentity(object).id),
          scene(a.SceneGeneration()), pool(mdl::KeyPoolIdentity(a.AccessoryKeys(s))), epoch(d->epoch), historyEpoch(h.NativeEpoch()),
          initialSlotPublication(d->publications[s]),
          domain(std::move(d)), cell(std::move(c)), owner(RetainSceneObject(a, MMS_OBJECT_ACCESSORY, s)) {}
    bool Source() const noexcept {
        if ((BoundNativePoseHistory(&app) && BoundNativePoseHistory(&app) != &history) || history.NativeEpoch() != historyEpoch || app.SceneGeneration() != scene ||
            domain->epoch != epoch || epoch == UINT64_MAX ||
            (!cell->initialized && domain->publications[slot] != initialSlotPublication) ||
            app.AccessorySlot(slot) != object || !SceneObjectPresent(object, id) || !owner || !owner->Present() ||
            !pool || mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) != pool ||
            !history.CanPublishNativeDomain(&kAttachmentDomain, domain.get())) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (i != slot && app.AccessorySlot(i) == object) return false;
        const auto& active=domain->active[slot];
        if(cell->initialized&&active&&active!=cell&&active->app==&app&&active->object==object&&SameId(active->id,id)&&
            active->scene==scene&&active->pool==pool&&active->epoch==epoch)return false;
        for (unsigned i = 0; i < logicalParentCount; ++i) if (!logicalParents[i].Current(app)) return false;
        return true;
    }
    bool Receipt() const noexcept {
        return Source() && cell->initialized && cell->app == &app && cell->object == object && SameId(cell->id, id) &&
            cell->scene == scene && cell->pool == pool && cell->epoch == epoch && Read(*object) == cell->actual &&
            cell->actualParent.Current(app);
    }
    Attachment Logical() const noexcept {
        // A stale logical parent can no longer authorize a historical restore.
        // A new exact-source edit may start from its actual current attachment.
        return Receipt() && cell->logicalParent.Current(app) ? cell->logical : Read(*object);
    }
    bool Matches(Attachment expected) const noexcept {
        if (!Source()) return false;
        if (!cell->initialized) return Read(*object) == expected;
        // Check actual receipt first. A bare write which happens to equal an
        // old historical value is not evidence of a trusted publication.
        if (!Receipt()) return false;
        if (cell->logicalParent.Current(app)) return cell->logical == expected;
        return Read(*object) == expected;
    }
};
struct AccessoryAttachmentHistory::Prepared::State {
    std::shared_ptr<AccessoryAttachmentHistory::Impl> source;
    Attachment expected, target, logical, actual;
    const std::uint64_t cellSerial, slotSerial, objectGeneration, bindingGeneration;
    std::array<Parent, 4> parents; std::array<std::uint64_t, 4> generations{};
    unsigned parentCount = 0;
    ReceiptParent targetParent, logicalParent;
    Parent retainedLogical;
    bool addLogical = false;
    mutable resource_budget::Collector resources;
    bool consumed = false;
    State(std::shared_ptr<AccessoryAttachmentHistory::Impl> s, Attachment e, Attachment t, Attachment l)
        : source(std::move(s)), expected(e), target(t), logical(l), actual(Read(*source->object)),
          cellSerial(source->cell->serial), slotSerial(source->domain->publications[source->slot]),
          objectGeneration(SceneObjectIdentity(source->object).generation), bindingGeneration(BoundNativePoseGeneration(&source->app)) {}
    MmsResult ParentOf(Attachment value, ReceiptParent* output = nullptr) {
        if (value.model == -1) { if (output) *output = {}; return MMS_OK; }
        if (value.model < 0 || value.bone < 0) return MMS_E_ARGUMENT;
        Parent* selected = nullptr;
        for (unsigned i = 0; i < parentCount; ++i) if (parents[i].slot == value.model) selected = &parents[i];
        if (!selected) {
            if (parentCount == parents.size()) return MMS_E_FAILED;
            if (auto r = parents[parentCount].Capture(source->app, value.model)) return r;
            generations[parentCount] = parents[parentCount].Generation(); selected = &parents[parentCount++];
        }
        if (unsigned(value.bone) >= selected->count) return MMS_E_ARGUMENT;
        if (output) *output = {selected->slot, selected->raw, selected->identity.id, selected->bones, selected->count};
        return MMS_OK;
    }
    bool Current() const noexcept {
        const auto& s = *source; MmsObjectRef current{};
        if (consumed || !s.Matches(expected) || s.cell->serial != cellSerial || s.domain->publications[s.slot] != slotSerial ||
            cellSerial == UINT64_MAX || slotSerial == UINT64_MAX || !(Read(*s.object) == actual) ||
            BoundNativePoseGeneration(&s.app) != bindingGeneration ||
            !FindSceneObjectIdentity(s.object, current) || current.generation != objectGeneration ||
            !resources.Result().Complete() || !s.owner->CoversBookkeeping(resources)) return false;
        for (unsigned i = 0; i < parentCount; ++i)
            if (!parents[i].Current(s.app) || parents[i].Generation() != generations[i] || !parents[i].owner->CoversBookkeeping(resources)) return false;
        for (unsigned i = 0; i < s.logicalParentCount; ++i)
            if (!s.logicalParents[i].owner->CoversBookkeeping(resources)) return false;
        return true;
    }
};
AccessoryAttachmentHistory::Prepared::Prepared(std::unique_ptr<State> state) : state_(std::move(state)) {}
AccessoryAttachmentHistory::Prepared::~Prepared() = default;
bool AccessoryAttachmentHistory::Prepared::IsCurrent() const noexcept { return state_ && state_->Current(); }
std::uint64_t AccessoryAttachmentHistory::Prepared::MeasuredBytes() const noexcept {
    return state_ ? state_->resources.Result().totalBytes : 0;
}
void AccessoryAttachmentHistory::Prepared::Publish() noexcept {
    if (!IsCurrent()) std::terminate();
    auto& p = *state_; auto& s = *p.source;
    s.history.PublishNativeDomain(&kAttachmentDomain, s.domain);
    if (p.addLogical) s.logicalParents[s.logicalParentCount++] = std::move(p.retainedLogical);
    s.object->parentModel = p.target.model; s.object->parentBone = p.target.bone;
    s.cell->logical = p.logical; s.cell->actual = p.target;
    s.cell->logicalParent = p.logicalParent; s.cell->actualParent = p.targetParent;
    s.cell->initialized = true; ++s.cell->serial; ++s.domain->publications[s.slot];
    // Replacing the old metadata cell cannot run an object/COM destructor.
    // Historical nodes retain their own cells independently of this slot.
    s.domain->active[s.slot] = s.cell; p.consumed = true;
}
MmsResult AccessoryAttachmentHistory::Begin(MMDApp& app, SharedHistory& history, unsigned slot) noexcept {
    return Boundary([&]() -> MmsResult {
        if ((BoundNativePoseHistory(&app) && BoundNativePoseHistory(&app) != &history) || slot >= kModelSlotCount || !app.AccessorySlot(slot) ||
            !mdl::KeyPoolIdentity(app.AccessoryKeys(slot))) return MMS_E_NOT_READY;
        std::uint64_t revision = 0; if (auto r = history.Revision(revision)) return r;
        auto base = history.FindNativeDomain(&kAttachmentDomain);
        if (!base) base = history.PrepareNativeDomain(&kAttachmentDomain, std::make_shared<AttachmentDomain>());
        auto domain = std::static_pointer_cast<AttachmentDomain>(base);
        if (!domain) return MMS_E_NOT_READY;
        if (!history.CanPublishNativeDomain(&kAttachmentDomain, domain.get()) || domain->epoch == UINT64_MAX) return MMS_E_NOT_READY;
        const auto id = SceneObjectIdentity(app.AccessorySlot(slot)).id;
        auto cell = domain->active[slot];
        if (!cell || cell->app != &app || cell->object != app.AccessorySlot(slot) || !SameId(cell->id, id) ||
            cell->scene != app.SceneGeneration() || cell->pool != mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) || cell->epoch != domain->epoch) {
            cell = std::make_shared<AttachmentCell>(); cell->app = &app; cell->object = app.AccessorySlot(slot); cell->id = id;
            cell->scene = app.SceneGeneration(); cell->pool = mdl::KeyPoolIdentity(app.AccessoryKeys(slot)); cell->epoch = domain->epoch;
        }
        auto next = std::make_shared<Impl>(app, history, slot, std::move(domain), std::move(cell));
        if (!next->Source()) return MMS_E_STALE;
        const auto logical = next->Logical();
        if (logical.model >= 0) {
            if (auto r = next->logicalParents[0].Capture(app, logical.model)) return r;
            if (logical.bone < 0 || unsigned(logical.bone) >= next->logicalParents[0].count) return MMS_E_ARGUMENT;
            next->logicalParentCount = 1;
        }
        impl_ = std::move(next); return MMS_OK;
    });
}
AccessoryAttachmentValue AccessoryAttachmentHistory::Logical() const noexcept { return impl_ && impl_->Source() ? impl_->Logical() : Attachment{}; }
bool AccessoryAttachmentHistory::Matches(AccessoryAttachmentValue expected) const noexcept { return impl_ && impl_->Matches(expected); }
MmsResult AccessoryAttachmentHistory::Prepare(AccessoryAttachmentValue expected, AccessoryAttachmentValue target,
    AccessoryAttachmentValue logical, std::unique_ptr<Prepared>& output) const noexcept {
    return Boundary([&]() -> MmsResult {
        if (!impl_ || !impl_->Matches(expected)) return MMS_E_STALE;
        auto state = std::make_unique<Prepared::State>(impl_, expected, target, logical);
        if (auto r = state->ParentOf(state->actual)) return r;
        if (auto r = state->ParentOf(expected)) return r;
        if (auto r = state->ParentOf(target, &state->targetParent)) return r;
        if (auto r = state->ParentOf(logical, &state->logicalParent)) return r;
        bool retained = logical.model == -1;
        for (unsigned i = 0; i < impl_->logicalParentCount; ++i)
            if (impl_->logicalParents[i].slot == logical.model) retained = true;
        if (!retained) {
            if (impl_->logicalParentCount == impl_->logicalParents.size()) return MMS_E_STALE;
            if (auto r = state->retainedLogical.Capture(impl_->app, logical.model)) return r;
            state->addLogical = true;
        }
        state->resources.Add(impl_.get(), 1, sizeof(Impl) + 2 * sizeof(void*));
        state->resources.Add(impl_->domain.get(), 1, sizeof(AttachmentDomain) + 2 * sizeof(void*));
        state->resources.Add(impl_->cell.get(), 1, sizeof(AttachmentCell) + 2 * sizeof(void*));
        if (auto r = impl_->owner->MeasureBookkeeping(state->resources)) return r;
        for (unsigned i = 0; i < state->parentCount; ++i)
            if (auto r = state->parents[i].owner->MeasureBookkeeping(state->resources)) return r;
        for (unsigned i = 0; i < impl_->logicalParentCount; ++i)
            if (auto r = impl_->logicalParents[i].owner->MeasureBookkeeping(state->resources)) return r;
        if (!state->Current()) return MMS_E_STALE;
        auto next = std::unique_ptr<Prepared>(new Prepared(std::move(state))); output = std::move(next); return MMS_OK;
    });
}

AccessoryAttachmentEvaluation::AccessoryAttachmentEvaluation(MMDApp* app, unsigned slot) noexcept {
    if (!app || slot >= kModelSlotCount) return;
    app_ = app; slot_ = slot; scene_ = app->SceneGeneration(); object_ = app->AccessorySlot(slot);
    pool_ = mdl::KeyPoolIdentity(app->AccessoryKeys(slot)); bindingGeneration_ = BoundNativePoseGeneration(app);
    history_ = BoundNativePoseHistory(app);
    if (!history_) return; // Ordinary unbound evaluation keeps its old route.
    historyEpoch_ = history_->NativeEpoch();
    if (history_->Revision(revision_) != MMS_OK) { valid_ = false; return; }
    auto domain = history_->FindNativeDomain(&kAttachmentDomain);
    MmsObjectRef current{};
    if (!object_ || !FindSceneObjectIdentity(object_, current)) { valid_ = false; return; }
    id_ = current.id; objectGeneration_ = current.generation;
    if (!domain) return;
    auto& d = *static_cast<AttachmentDomain*>(domain.get());
    domain_ = domain; domainEpoch_ = d.epoch; slotSerial_ = d.publications[slot];
    auto* object = app->AccessorySlot(slot); const auto& cell = d.active[slot];
    if (!cell) return;
    cell_ = cell.get(); serial_ = cell->serial;
    // A deleted source can leave metadata in this slot. Observe its identity
    // and publication serial, but never turn evaluation of a new object/pool
    // into a publication on that old source's receipt.
    if (cell->app != app || cell->object != object || !SameId(cell->id, current.id) ||
        cell->scene != scene_ || cell->pool != pool_ || cell->epoch != d.epoch) return;
    valid_ = false;
    if (!object || !cell || !cell->initialized || cell->app != app || cell->object != object ||
        cell->scene != app->SceneGeneration() || cell->pool != mdl::KeyPoolIdentity(app->AccessoryKeys(slot)) ||
        cell->epoch != d.epoch || d.epoch == UINT64_MAX || cell->serial == UINT64_MAX || d.publications[slot] == UINT64_MAX ||
        !(Read(*object) == cell->actual) || !cell->actualParent.Current(*app) ||
        !FindSceneObjectIdentity(object, current) || !SameId(current.id, cell->id)) return;
    MmsObjectRef parent{};
    if (cell->actualParent.slot >= 0 && !FindSceneObjectIdentity(cell->actualParent.raw, parent)) return;
    for (unsigned i = 0; i < kModelSlotCount; ++i) if (i != slot && app->AccessorySlot(i) == object) return;
    cell_ = cell.get(); valid_ = true; observerOnly_ = false;
    serial_ = cell->serial; slotSerial_ = d.publications[slot]; objectGeneration_ = current.generation;
    bindingGeneration_ = BoundNativePoseGeneration(app); before_ = cell->actual;
    parentGeneration_ = parent.generation;
}
bool AccessoryAttachmentEvaluation::CanApply() const noexcept {
    if (!valid_) return false;
    if (!app_) return true;
    if (BoundNativePoseHistory(app_) != history_ || BoundNativePoseGeneration(app_) != bindingGeneration_) return false;
    if (!history_) return true;
    std::uint64_t revision = 0; MmsObjectRef object{};
    if (history_->NativeEpoch() != historyEpoch_ || history_->Revision(revision) != MMS_OK || revision != revision_ ||
        app_->SceneGeneration() != scene_ || app_->AccessorySlot(slot_) != object_ ||
        mdl::KeyPoolIdentity(app_->AccessoryKeys(slot_)) != pool_ ||
        !FindSceneObjectIdentity(object_, object) || !SameId(object.id, id_) || object.generation != objectGeneration_) return false;
    const auto current = history_->FindNativeDomain(&kAttachmentDomain);
    if (current.get() != domain_.get()) return false;
    if (!current) return true;
    const auto& d = *static_cast<AttachmentDomain*>(current.get()); const auto& cell = d.active[slot_];
    if (d.epoch != domainEpoch_ || d.publications[slot_] != slotSerial_ || cell.get() != cell_) return false;
    if (!cell) return true;
    if (cell->serial != serial_) return false;
    if (observerOnly_) return true;
    if (!(Read(*static_cast<mdl::AccessoryRecord*>(object_)) == before_) ||
        !(cell->actual == before_) || !cell->actualParent.Current(*app_)) return false;
    MmsObjectRef parent{};
    return cell->actualParent.slot < 0 || (FindSceneObjectIdentity(cell->actualParent.raw, parent) && parent.generation == parentGeneration_);
}
void AccessoryAttachmentEvaluation::Complete(AccessoryAttachmentValue computed) noexcept {
    if (!app_ || !valid_ || observerOnly_ || !cell_ || !domain_) return;
    auto* app = app_; app_ = nullptr; // A token is usable at most once.
    auto& d = *static_cast<AttachmentDomain*>(domain_.get());
    const auto& cell = d.active[slot_]; MmsObjectRef current{};
    if (BoundNativePoseHistory(app) != history_ || BoundNativePoseGeneration(app) != bindingGeneration_ ||
        history_->NativeEpoch() != historyEpoch_ || app->SceneGeneration() != scene_ || d.epoch != domainEpoch_ ||
        d.publications[slot_] != slotSerial_ || !cell || cell.get() != cell_ || cell->serial != serial_ ||
        app->AccessorySlot(slot_) != object_ || mdl::KeyPoolIdentity(app->AccessoryKeys(slot_)) != pool_ ||
        !FindSceneObjectIdentity(object_, current) || !SameId(current.id, id_) || current.generation != objectGeneration_ ||
        !(cell->actual == before_) || !cell->actualParent.Current(*app)) return;
    auto* object = static_cast<mdl::AccessoryRecord*>(object_);
    for (unsigned i = 0; i < kModelSlotCount; ++i) if (i != slot_ && app->AccessorySlot(i) == object) return;
    MmsObjectRef previousParent{};
    if (cell->actualParent.slot >= 0 && (!FindSceneObjectIdentity(cell->actualParent.raw, previousParent) ||
        previousParent.generation != parentGeneration_)) return;
    if (!(Read(*object) == computed) || computed == before_) return;
    ReceiptParent parent;
    if (computed.model != -1) {
        if (computed.model < 0 || computed.model >= kModelSlotCount || computed.bone < 0) return;
        auto* model = app->ModelSlot(computed.model); MmsObjectRef identity{};
        if (!model || !FindSceneObjectIdentity(model, identity)) return;
        const auto* record = mdl::Mdl(model);
        if (!record->boneTable || unsigned(computed.bone) >= record->boneCount) return;
        parent = {computed.model, model, identity.id, record->boneTable, record->boneCount};
        if (!parent.Current(*app)) return;
    }
    cell->actual = computed; cell->actualParent = parent; ++cell->serial; ++d.publications[slot_];
}

namespace {
bool SameParent(const ReceiptParent& a,const ReceiptParent& b) noexcept {
    return a.slot==b.slot&&a.raw==b.raw&&SameId(a.id,b.id)&&a.bones==b.bones&&a.count==b.count;
}
bool SameCellValue(const AttachmentCell& a,const AttachmentCell& b) noexcept {
    return a.app==b.app&&a.object==b.object&&SameId(a.id,b.id)&&a.scene==b.scene&&a.pool==b.pool&&a.epoch==b.epoch&&
        a.logical==b.logical&&a.actual==b.actual&&SameParent(a.logicalParent,b.logicalParent)&&
        SameParent(a.actualParent,b.actualParent)&&a.initialized==b.initialized;
}
MmsResult LightParent(MMDApp& app,Attachment value,ReceiptParent& out) noexcept {
    if(value.model==-1){out={};return MMS_OK;}
    if(value.model<0||value.model>=kModelSlotCount||value.bone<0)return MMS_E_ARGUMENT;
    auto* raw=app.ModelSlot(value.model);MmsObjectRef id{};
    if(!raw||!FindSceneObjectIdentity(raw,id))return MMS_E_STALE;
    const auto* model=mdl::Mdl(raw);
    if(!model->boneTable||unsigned(value.bone)>=model->boneCount)return MMS_E_ARGUMENT;
    out={value.model,raw,id.id,model->boneTable,model->boneCount};
    return out.Current(app)?MMS_OK:MMS_E_STALE;
}
}
struct AccessoryAttachmentRemoval::Impl {
    struct RemovedParent {ReceiptParent parent;std::weak_ptr<RetainedSceneObject> owner;};
    struct Item {
        unsigned slot=0;mdl::AccessoryRecord* object=nullptr;MmsId id{};std::uint64_t pool=0,initialPublication=0;
        mdl::AccessoryKey* originalKeys=nullptr;bool presence=false,cleared=false;
        Attachment initialRaw{},parkedActual{},savedActual{};
        Attachment presenceLogical{};ReceiptParent presenceLogicalParent;
        ReceiptParent savedParent;
        std::weak_ptr<RetainedSceneObject> objectOwner;
        std::shared_ptr<AttachmentCell> captured,before,after;
        AttachmentCell capturedValue{},beforeValue{},afterValue{};
    };
    MMDApp& app;SharedHistory& history;
    const std::uint64_t scene,historyEpoch,domainEpoch,binding;
    std::shared_ptr<AttachmentDomain> domain;
    std::vector<Item> items;std::vector<RemovedParent> removed;
    std::uint64_t generation=1,bytes=0,initialRevision=0;
    Impl(MMDApp& a,SharedHistory& h,std::shared_ptr<AttachmentDomain> d)
        :app(a),history(h),scene(a.SceneGeneration()),historyEpoch(h.NativeEpoch()),domainEpoch(d->epoch),
         binding(BoundNativePoseGeneration(&a)),domain(std::move(d)){}
    bool Source()const noexcept {
        std::uint64_t revision=0;
        return BoundNativePoseHistory(&app)==&history&&app.SceneGeneration()==scene&&
            (generation!=1||(BoundNativePoseGeneration(&app)==binding&&history.Revision(revision)==MMS_OK&&revision==initialRevision))&&
            history.NativeEpoch()==historyEpoch&&domain->epoch==domainEpoch&&domainEpoch!=UINT64_MAX&&generation!=UINT64_MAX&&
            history.CanPublishNativeDomain(&kAttachmentDomain,domain.get());
    }
    bool Removes(int slot)const noexcept {for(const auto& p:removed)if(p.parent.slot==slot)return true;return false;}
    static bool Own(const AttachmentCell& cell,const Item& item,const Impl& source)noexcept {
        return cell.initialized&&cell.app==&source.app&&cell.object==item.object&&SameId(cell.id,item.id)&&
            cell.scene==source.scene&&cell.pool==item.pool&&cell.epoch==source.domainEpoch;
    }
};
struct AccessoryAttachmentRemoval::Prepared::State {
    struct ParentStamp {
        ReceiptParent parent;std::shared_ptr<RetainedSceneObject> owner;std::uint64_t generation=0;bool present=false;
    };
    struct Row {
        std::shared_ptr<AttachmentCell> expected,target;
        std::shared_ptr<RetainedSceneObject> objectOwner;
        AttachmentCell expectedValue{},targetValue{};
        Attachment actual{},targetActual{};ReceiptParent actualParent;
        std::uint64_t publication=0,sourceSerial=0,targetSerial=0,objectGeneration=0;
        mdl::AccessoryKey* keys=nullptr;std::size_t capacity=0;bool sourcePresent=true,clear=false;
    };
    std::shared_ptr<Impl> source;bool forward,consumed=false;std::uint64_t generation,binding;
    std::vector<Row> rows;std::vector<ParentStamp> parents;
    std::uint64_t extraMetadata=0;
    mutable resource_budget::Collector resources;
    State(std::shared_ptr<Impl> s,bool f):source(std::move(s)),forward(f),generation(source->generation),binding(BoundNativePoseGeneration(&source->app)){}
    MmsResult ParentOf(Attachment value,ReceiptParent& out) {
        if(value.model==-1){out={};return MMS_OK;}
        if(value.model<0||value.model>=kModelSlotCount||value.bone<0)return MMS_E_ARGUMENT;
        auto& s=*source;
        for(const auto& stamp:parents)if(stamp.parent.slot==value.model){
            if(unsigned(value.bone)>=stamp.parent.count)return MMS_E_ARGUMENT;out=stamp.parent;return MMS_OK;}
        ParentStamp stamp;auto* raw=s.app.ModelSlot(value.model);MmsObjectRef id{};
        if(raw&&FindSceneObjectIdentity(raw,id)){
            if(auto r=LightParent(s.app,value,stamp.parent))return r;
            stamp.owner=RetainSceneObject(s.app,MMS_OBJECT_MODEL,unsigned(value.model));stamp.generation=id.generation;stamp.present=true;
        }else{
            const Impl::RemovedParent* retained=nullptr;
            for(const auto& p:s.removed)if(p.parent.slot==value.model)retained=&p;
            if(raw||!retained||(stamp.owner=retained->owner.lock())==nullptr||stamp.owner->Present()||
                !SameId(stamp.owner->Id(),retained->parent.id))return MMS_E_STALE;
            stamp.parent=retained->parent;
            // The outer retained object owns this absent raw pointer. We prove
            // its exact allocation/table, never make an absent slot present.
            const auto* model=mdl::Mdl(stamp.parent.raw);
            if(model->boneTable!=stamp.parent.bones||model->boneCount!=stamp.parent.count||
                value.bone<0||unsigned(value.bone)>=stamp.parent.count)return MMS_E_STALE;
        }
        if(auto r=stamp.owner->MeasureBookkeeping(resources))return r;
        out=stamp.parent;parents.push_back(std::move(stamp));return MMS_OK;
    }
    bool Current()const noexcept {
        const auto& s=*source;
        if(consumed||s.generation!=generation||!s.Source()||BoundNativePoseGeneration(&s.app)!=binding||
            rows.size()!=s.items.size()||!resources.Result().Complete())return false;
        for(const auto& p:parents){
            if(!p.owner||!SameId(p.owner->Id(),p.parent.id)||!p.owner->CoversBookkeeping(resources))return false;
            if(p.present){MmsObjectRef id{};if(!p.owner->Present()||!p.parent.Current(s.app)||
                !FindSceneObjectIdentity(p.parent.raw,id)||id.generation!=p.generation)return false;}
            else{if(p.owner->Present()||s.app.ModelSlot(p.parent.slot))return false;
                const auto* model=mdl::Mdl(p.parent.raw);
                if(model->boneTable!=p.parent.bones||model->boneCount!=p.parent.count)return false;}
        }
        for(std::size_t i=0;i<rows.size();++i){
            const auto& item=s.items[i];const auto& row=rows[i];MmsObjectRef id{};
            if(!row.objectOwner||!SameId(row.objectOwner->Id(),item.id)||!row.objectOwner->CoversBookkeeping(resources)||
                row.objectOwner->Present()!=row.sourcePresent||s.app.AccessoryKeys(item.slot)!=row.keys||mdl::KeyPoolCapacity(row.keys)!=row.capacity||
                s.domain->publications[item.slot]!=row.publication||row.publication==UINT64_MAX||
                s.domain->active[item.slot]!=row.expected)return false;
            if(s.generation==1&&row.publication!=item.initialPublication)return false;
            if(row.sourcePresent){
                if(s.app.AccessorySlot(item.slot)!=item.object||!FindSceneObjectIdentity(item.object,id)||!SameId(id.id,item.id)||
                    id.generation!=row.objectGeneration||mdl::KeyPoolIdentity(row.keys)!=item.pool)return false;
                for(unsigned slot=0;slot<kModelSlotCount;++slot)if(slot!=item.slot&&s.app.AccessorySlot(slot)==item.object)return false;
            }else if(s.app.AccessorySlot(item.slot)||mdl::KeyPoolIdentity(item.originalKeys)!=item.pool)return false;
            if(!(Read(*item.object)==row.actual))return false;
            if(row.expected&&(row.expected->serial!=row.sourceSerial||!SameCellValue(*row.expected,row.expectedValue)))return false;
            if(row.target&&(row.target->serial!=row.targetSerial||row.targetSerial==UINT64_MAX))return false;
        }
        return true;
    }
};
AccessoryAttachmentRemoval::Prepared::Prepared(std::unique_ptr<State> state):state_(std::move(state)){}
AccessoryAttachmentRemoval::Prepared::~Prepared()=default;
bool AccessoryAttachmentRemoval::Prepared::IsCurrent()const noexcept{return state_&&state_->Current();}
std::uint64_t AccessoryAttachmentRemoval::Prepared::MeasuredBytes()const noexcept {
    if(!state_)return 0;const auto base=state_->source->bytes;
    const auto resource=state_->resources.Result().totalBytes;
    if(state_->extraMetadata>UINT64_MAX-resource)return UINT64_MAX;
    const auto extra=state_->extraMetadata+resource;
    return extra>UINT64_MAX-base?UINT64_MAX:base+extra;
}
void AccessoryAttachmentRemoval::Prepared::Publish()noexcept {
    // All binding/presence candidates were checked against the complete old
    // state. Do not revalidate raw fields after one member has published.
    auto& p=*state_;auto& s=*p.source;if(p.consumed)std::terminate();
    s.history.PublishNativeDomain(&kAttachmentDomain,s.domain);
    for(std::size_t i=0;i<s.items.size();++i){auto& item=s.items[i];const auto& row=p.rows[i];
        if(row.target){const auto serial=row.target->serial;*row.target=row.targetValue;row.target->serial=serial+1;}
        s.domain->active[item.slot]=row.target;++s.domain->publications[item.slot];
        if(!item.presence){item.object->parentModel=row.targetActual.model;item.object->parentBone=row.targetActual.bone;}
        if(p.forward){
            // A previously unregistered imported accessory can acquire its
            // first canonical cell through later real native edits. Capture
            // that exact trusted cell when removing it, so restoring presence
            // does not fork away those older native history nodes.
            if(item.presence&&row.expected&&Impl::Own(*row.expected,item,s)&&row.expected!=item.before){
                item.before=row.expected;item.beforeValue=row.expectedValue;
            }
            item.cleared=row.clear;item.savedActual=row.actual;item.savedParent=row.actualParent;item.parkedActual=row.actual;
        }
    }
    s.bytes+=p.extraMetadata;++s.generation;p.consumed=true;
}
MmsResult AccessoryAttachmentRemoval::Capture(MMDApp& app,const std::vector<unsigned>& models,
    const std::vector<unsigned>& accessories)noexcept {
    return Boundary([&]()->MmsResult {
        auto* history=BoundNativePoseHistory(&app);if(!history){impl_.reset();return MMS_OK;}
        std::uint64_t revision=0;if(auto r=history->Revision(revision))return r;
        std::array<bool,kModelSlotCount> removedModels{},removedAccessories{};
        for(auto slot:models){if(slot>=removedModels.size())return MMS_E_ARGUMENT;removedModels[slot]=true;}
        for(auto slot:accessories){if(slot>=removedAccessories.size())return MMS_E_ARGUMENT;removedAccessories[slot]=true;}
        auto base=history->FindNativeDomain(&kAttachmentDomain);
        if(!base)base=history->PrepareNativeDomain(&kAttachmentDomain,std::make_shared<AttachmentDomain>());
        auto domain=std::static_pointer_cast<AttachmentDomain>(base);if(!domain)return MMS_E_NOT_READY;
        auto next=std::make_shared<Impl>(app,*history,domain);next->initialRevision=revision;
        for(auto slot:models){auto* raw=app.ModelSlot(slot);MmsObjectRef id{};
            if(!raw||!FindSceneObjectIdentity(raw,id))return MMS_E_STALE;
            const auto* model=mdl::Mdl(raw);auto owner=RetainSceneObject(app,MMS_OBJECT_MODEL,slot);
            next->removed.push_back({{int(slot),raw,id.id,model->boneTable,model->boneCount},owner});}
        for(unsigned slot=0;slot<kModelSlotCount;++slot){
            auto* object=app.AccessorySlot(slot);if(!object||(models.empty()&&!removedAccessories[slot]))continue;
            Impl::Item item;item.slot=slot;item.object=object;item.id=SceneObjectIdentity(object).id;item.pool=mdl::KeyPoolIdentity(app.AccessoryKeys(slot));
            item.originalKeys=app.AccessoryKeys(slot);item.initialPublication=domain->publications[slot];item.initialRaw=item.parkedActual=Read(*object);
            item.presence=removedAccessories[slot];item.captured=domain->active[slot];if(item.captured)item.capturedValue=*item.captured;
            if(!item.pool)return MMS_E_NOT_READY;
            const bool own=item.captured&&Impl::Own(*item.captured,item,*next);
            if(own&&(!(item.captured->actual==item.initialRaw)||!item.captured->actualParent.Current(app)||!item.captured->logicalParent.Current(app)))return MMS_E_STALE;
            auto owner=RetainSceneObject(app,MMS_OBJECT_ACCESSORY,slot);item.objectOwner=owner;
            if(item.presence){item.before=item.captured;item.beforeValue=item.capturedValue;
                item.presenceLogical=own?item.captured->logical:item.initialRaw;
                if(auto r=LightParent(app,item.presenceLogical,item.presenceLogicalParent))return r;}
            else{
                item.before=own?item.captured:std::make_shared<AttachmentCell>();
                if(!own){auto& cell=*item.before;cell.app=&app;cell.object=object;cell.id=item.id;cell.scene=next->scene;cell.pool=item.pool;cell.epoch=domain->epoch;
                    cell.logical=cell.actual=item.initialRaw;cell.initialized=true;
                    if(auto r=LightParent(app,cell.actual,cell.actualParent))return r;cell.logicalParent=cell.actualParent;}
                item.beforeValue=*item.before;item.after=std::make_shared<AttachmentCell>(*item.before);
                auto& after=*item.after;after.actual=next->Removes(item.initialRaw.model)?Attachment{-1,0}:item.initialRaw;
                after.logical=next->Removes(item.beforeValue.logical.model)?after.actual:item.beforeValue.logical;
                if(auto r=LightParent(app,after.actual,after.actualParent))return r;
                if(auto r=LightParent(app,after.logical,after.logicalParent))return r;
                item.afterValue=after;
            }
            next->items.push_back(std::move(item));
        }
        if(next->items.empty()){impl_.reset();return MMS_OK;}
        next->bytes=resource_budget::Collector::Sum(sizeof(Impl)+2*sizeof(void*),resource_budget::Collector::Sum(sizeof(AttachmentDomain)+2*sizeof(void*),
            resource_budget::Collector::Sum(resource_budget::Collector::Product(next->items.capacity(),sizeof(Impl::Item)),
                resource_budget::Collector::Product(next->removed.capacity(),sizeof(Impl::RemovedParent)+2*sizeof(void*)))));
        std::vector<const AttachmentCell*> cells;
        for(const auto& item:next->items)for(const auto* cell:{item.captured.get(),item.before.get(),item.after.get()})
            if(cell&&std::find(cells.begin(),cells.end(),cell)==cells.end()){cells.push_back(cell);next->bytes=resource_budget::Collector::Sum(next->bytes,sizeof(AttachmentCell)+2*sizeof(void*));}
        if(!next->Source())return MMS_E_STALE;impl_=std::move(next);return MMS_OK;
    });
}
bool AccessoryAttachmentRemoval::Affects(unsigned slot)const noexcept {
    if(impl_)for(const auto& item:impl_->items)if(item.slot==slot)return true;return false;
}
bool AccessoryAttachmentRemoval::OwnsPair(unsigned slot)const noexcept {
    if(impl_)for(const auto& item:impl_->items)if(item.slot==slot)return !item.presence;return false;
}
MmsResult AccessoryAttachmentRemoval::Prepare(bool forward,std::unique_ptr<Prepared>& out)const noexcept {
    return Boundary([&]()->MmsResult {
        if(!impl_){out.reset();return MMS_OK;}if(!impl_->Source())return MMS_E_STALE;
        auto state=std::make_unique<Prepared::State>(impl_,forward);state->rows.reserve(impl_->items.size());
        std::vector<const AttachmentCell*> newCells;
        for(const auto& item:impl_->items){
            Prepared::State::Row row;row.sourcePresent=!item.presence||forward;
            if((row.sourcePresent&&impl_->app.AccessorySlot(item.slot)!=item.object)||
                (!row.sourcePresent&&impl_->app.AccessorySlot(item.slot)))return MMS_E_STALE;
            row.objectOwner=row.sourcePresent?RetainSceneObject(impl_->app,MMS_OBJECT_ACCESSORY,item.slot):item.objectOwner.lock();
            if(!row.objectOwner||!SameId(row.objectOwner->Id(),item.id)||row.objectOwner->Present()!=row.sourcePresent)return MMS_E_STALE;
            row.keys=impl_->app.AccessoryKeys(item.slot);row.capacity=mdl::KeyPoolCapacity(row.keys);
            row.expected=forward?(impl_->generation==1?item.captured:item.before):item.after;
            if(item.presence&&forward&&impl_->generation!=1){
                const auto& active=impl_->domain->active[item.slot];
                if(active&&Impl::Own(*active,item,*impl_)&&active->logical==item.presenceLogical&&
                    SameParent(active->logicalParent,item.presenceLogicalParent))row.expected=active;
            }
            row.target=forward?item.after:item.before;row.publication=impl_->domain->publications[item.slot];
            if(row.expected){row.expectedValue=*row.expected;row.sourceSerial=row.expected->serial;}
            if(item.presence&&forward&&row.expected&&Impl::Own(*row.expected,item,*impl_)&&row.expected!=item.before){
                bool retained=false;for(const auto& known:impl_->items)if(row.expected==known.captured||row.expected==known.before||row.expected==known.after)retained=true;
                if(!retained&&std::find(newCells.begin(),newCells.end(),row.expected.get())==newCells.end()){
                    newCells.push_back(row.expected.get());state->extraMetadata=resource_budget::Collector::Sum(state->extraMetadata,sizeof(AttachmentCell)+2*sizeof(void*));
                }
            }
            if(row.target)row.targetSerial=row.target->serial;
            MmsObjectRef id{};if(row.sourcePresent){if(!FindSceneObjectIdentity(item.object,id))return MMS_E_STALE;row.objectGeneration=id.generation;}
            row.actual=Read(*item.object);
            const auto& logicalSource=forward?item.beforeValue:item.afterValue;
            const bool own=row.expected&&Impl::Own(*row.expected,item,*impl_);
            if(own){
                const auto logical=item.presence&&forward?item.presenceLogical:logicalSource.logical;
                const auto logicalParent=item.presence&&forward?item.presenceLogicalParent:logicalSource.logicalParent;
                if(!(row.expected->logical==logical)||!SameParent(row.expected->logicalParent,logicalParent)||
                    !(row.expected->actual==row.actual))return MMS_E_STALE;
                if(auto r=state->ParentOf(row.actual,row.actualParent))return r;
                if(!SameParent(row.actualParent,row.expected->actualParent))return MMS_E_STALE;
                ReceiptParent logicalProof;if(auto r=state->ParentOf(row.expected->logical,logicalProof))return r;
                if(!SameParent(logicalProof,row.expected->logicalParent))return MMS_E_STALE;
            }else{
                const auto expectedRaw=item.presence&&!forward?item.parkedActual:item.initialRaw;
                if(!(row.actual==expectedRaw))return MMS_E_STALE;
                if(row.expected&&!SameCellValue(*row.expected,item.capturedValue))return MMS_E_STALE;
                if(auto r=state->ParentOf(row.actual,row.actualParent))return r;
            }
            if(auto r=row.objectOwner->MeasureBookkeeping(state->resources))return r;
            row.clear=!item.presence&&forward&&impl_->Removes(row.actual.model);
            row.targetActual=item.presence?row.actual:(forward?(row.clear?Attachment{-1,0}:row.actual):(item.cleared?item.savedActual:row.actual));
            if(row.target){
                row.targetValue=forward?item.afterValue:item.beforeValue;
                if(Impl::Own(row.targetValue,item,*impl_)){
                    row.targetValue.actual=row.targetActual;
                    if(auto r=state->ParentOf(row.targetActual,row.targetValue.actualParent))return r;
                    if(auto r=state->ParentOf(row.targetValue.logical,row.targetValue.logicalParent))return r;
                }
            }
            state->rows.push_back(std::move(row));
        }
        resource_budget::Collector::Sum(impl_->bytes,state->extraMetadata);
        if(!state->Current())return MMS_E_STALE;out=std::unique_ptr<Prepared>(new Prepared(std::move(state)));return MMS_OK;
    });
}
std::uint64_t AccessoryAttachmentRemoval::RetainedBytes()const noexcept{return impl_?impl_->bytes:0;}

namespace {
struct Change final : HistoryChange, std::enable_shared_from_this<Change> {
    MMDApp& app; SharedHistory* const history;
    const unsigned slot; mdl::AccessoryRecord* const object;
    const std::uint64_t scene, pool, binding, initialObject;
    const int frame; const HWND capture;
    const Attachment before; Attachment after;
    AccessoryAttachmentHistory attachment;
    Attachment beforeLogical{}, afterLogical{};
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
            mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) != pool || !attachment.Matches(applied ? afterLogical : beforeLogical)) return false;
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
        const Attachment nextLogical;
        std::unique_ptr<AccessoryAttachmentHistory::Prepared> coordinated;
        std::uint64_t revision = 0, bytes = 0; mutable resource_budget::Collector resources;
        bool consumed = false;
        Candidate(std::shared_ptr<Change> c, bool direction, bool record)
            : change(std::move(c)), forward(direction), serial(change->serial), binding(BoundNativePoseGeneration(&change->app)),
              object(change->Generation()), frame(change->app.CurrentFrame()), capture(GetCapture()), expected(Bytes(*change->object)),
              nextLogical(direction ? (!change->published ? (record ? change->after : change->beforeLogical) : change->afterLogical) : change->beforeLogical) {
            for (unsigned i = 0; i < change->parentCount; ++i) parents[i] = change->parents[i].Generation();
        }
        bool IsCurrent() const noexcept override {
            const auto& c = *change; std::uint64_t now = 0;
            if (consumed || serial != c.serial || forward == c.applied || !c.SourcesCurrent() || !c.InitialCurrent() ||
                !Ready(c.app, true) || binding != BoundNativePoseGeneration(&c.app) || object != c.Generation() ||
                frame != c.app.CurrentFrame() || capture != GetCapture() || expected != Bytes(*c.object) ||
                c.history->Revision(now) != MMS_OK || now != revision || !resources.Result().Complete() ||
                !c.owner->CoversBookkeeping(resources) || !coordinated || !coordinated->IsCurrent()) return false;
            for (unsigned i = 0; i < c.parentCount; ++i)
                if (parents[i] != c.parents[i].Generation() || !c.parents[i].owner->CoversBookkeeping(resources)) return false;
            return true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& c = *change;
            coordinated->Publish();
            if (!c.published) c.afterLogical = nextLogical;
            c.applied = forward; c.published = true; ++c.serial; consumed = true;
        }
    };
    MmsResult PrepareWithIntent(bool forward, bool record, std::unique_ptr<PreparedHistoryChange>& out) {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!SourcesCurrent() || !InitialCurrent() || forward == applied) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward, record);
            if (auto r = attachment.Prepare(applied ? afterLogical : beforeLogical, forward ? after : before,
                                           candidate->nextLogical, candidate->coordinated)) return r;
            if (auto r = history->Revision(candidate->revision)) return r;
            if (auto r = owner->MeasureBookkeeping(candidate->resources)) return r;
            for (unsigned i = 0; i < parentCount; ++i)
                if (auto r = parents[i].owner->MeasureBookkeeping(candidate->resources)) return r;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RetainedBytes(), resource_budget::Collector::Sum(
                candidate->resources.Result().totalBytes, candidate->coordinated->MeasuredBytes()));
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        return PrepareWithIntent(forward, history->Mode() == SharedHistory::RecordingMode::AllOperations, out);
    }
    MmsResult PrepareInitial(SharedHistory& journal, bool record, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (&journal != history) return MMS_E_ARGUMENT;
        return PrepareWithIntent(true, record, out);
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
    if (auto r = change->attachment.Begin(app, *history, slot)) return r;
    change->beforeLogical = change->attachment.Logical();
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
