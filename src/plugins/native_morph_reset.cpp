#include "native_morph_reset.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ui_control_readout.hpp"
#include <array>
#include <cstring>
#include <exception>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
std::uint32_t WeightBits(const mdl::MorphRecord& morph) noexcept {
    std::uint32_t value;std::memcpy(&value,&morph.value,sizeof(value));return value;
}
bool Ready(MMDApp& app,bool replay=false) noexcept {
    // The history command holds both locks while publishing an existing node.
    // Its private authorization cannot be borrowed by a new adjustment or an
    // arbitrary readout callback holding only ScopedUiInputGate.
    const bool ownReplay=replay&&NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&
        (!app.SceneMutationInProgress()||ownReplay)&&(!bake::UiLocked()||ownReplay)&&!SceneSaveBlocksEdits()&&!GetCapture()&&
        !app.FrameRangeDialog()&&!app.state.frameCopyDialog&&!app.RecordingWindow()&&
        !app.GravitySettingDialog()&&!app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&
        !app.state.accessoryEditDialogOpen&&!NativeModelOutlineDialogBlocks(app);
}
struct Source {
    std::array<char,20> name{};
    mdl::MorphPanel panel;
    std::uint8_t type;
    std::uint32_t bits;
    explicit Source(const mdl::MorphRecord& morph):panel(morph.panel),type(morph.type),bits(WeightBits(morph)) {
        std::memcpy(name.data(),morph.name,name.size());
    }
    bool Identity(const mdl::MorphRecord& morph) const noexcept {
        return panel==morph.panel&&type==morph.type&&!std::memcmp(name.data(),morph.name,name.size());
    }
};
struct Reset final:HistoryChange,std::enable_shared_from_this<Reset> {
    MMDApp& app;SharedHistory& history;const unsigned slot;unsigned char* const model;
    const MmsObjectRef identity;mdl::MorphRecord* const table;const unsigned count;
    const std::uint64_t pool,binding;const int frame;
    std::shared_ptr<RetainedSceneObject> owner;
    std::vector<Source> before;
    std::uint64_t generation=1;bool applied=false,published=false;
    Reset(MMDApp& a,SharedHistory& h,unsigned s):app(a),history(h),slot(s),model(a.ModelSlot(s)),
        identity(SceneObjectIdentity(model)),table(mdl::Mdl(model)->morphs),count(mdl::Mdl(model)->morphCount),
        pool(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),binding(BoundNativePoseGeneration(&a)),frame(a.CurrentFrame()),
        owner(RetainSceneObject(a,MMS_OBJECT_MODEL,s)) {
        before.reserve(count);for(unsigned i=0;i<count;++i)before.emplace_back(table[i]);
    }
    bool Present() const noexcept {
        if(app.ModelSlot(slot)!=model||!owner->Present()||!SceneObjectPresent(model,identity.id))return false;
        const auto& live=*mdl::Mdl(model);
        return live.morphs==table&&live.morphCount==count&&pool&&mdl::MorphKeyPoolIdentity(live.morphKeys)==pool;
    }
    bool IdentityCurrent(bool initial) const noexcept {
        if(!Present())return false;
        for(unsigned i=0;i<count;++i)if((initial||before[i].bits)&&!before[i].Identity(table[i]))return false;
        return true;
    }
    bool InitialCurrent() const noexcept {
        if(!IdentityCurrent(true)||app.CameraMode()||app.SelectedModelSlot()!=slot||app.CurrentFrame()!=frame||
            BoundNativePoseHistory(&app)!=&history||BoundNativePoseGeneration(&app)!=binding||
            SceneObjectIdentity(model).generation!=identity.generation)return false;
        for(unsigned i=0;i<count;++i)if(WeightBits(table[i])!=before[i].bits)return false;
        return true;
    }
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<Reset> source;const bool forward,initial;
        const std::uint64_t generation,binding,objectGeneration;const int frame;
        std::vector<std::uint32_t> expected;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes=0;bool consumed=false;
        Candidate(std::shared_ptr<Reset> value,bool direction):source(std::move(value)),forward(direction),initial(!source->published),
            generation(source->generation),binding(BoundNativePoseGeneration(&source->app)),
            objectGeneration(SceneObjectIdentity(source->model).generation),frame(source->app.CurrentFrame()) {
            expected.reserve(source->count);for(unsigned i=0;i<source->count;++i)expected.push_back(WeightBits(source->table[i]));
        }
        bool IsCurrent() const noexcept override {
            const auto& s=*source;
            if(consumed||s.generation!=generation||forward==s.applied||!s.IdentityCurrent(initial)||!Ready(s.app,!initial)||
                s.app.CurrentFrame()!=frame||BoundNativePoseHistory(&s.app)!=&s.history||BoundNativePoseGeneration(&s.app)!=binding||
                SceneObjectIdentity(s.model).generation!=objectGeneration||!s.owner->CoversBookkeeping(resources))return false;
            if(initial)return s.InitialCurrent();
            for(unsigned i=0;i<s.count;++i)if(s.before[i].bits&&WeightBits(s.table[i])!=expected[i])return false;
            return true;
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();
            for(unsigned i=0;i<source->count;++i)if(source->before[i].bits) {
                // No floating-point arithmetic, including for signed zero and
                // nonfinite payloads accepted by the original reset command.
                const std::uint32_t bits=forward?0:source->before[i].bits;
                std::memcpy(&source->table[i].value,&bits,sizeof(bits));
            }
            source->applied=forward;source->published=true;++source->generation;consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override {out=bytes;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();return Boundary([&]() -> MmsResult {
            if(forward==applied||!IdentityCurrent(!published)||(!published&&!InitialCurrent()))return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!Ready(app,published)||BoundNativePoseHistory(&app)!=&history)return MMS_E_BUSY;
            auto next=std::make_unique<Candidate>(shared_from_this(),forward);
            if(const auto result=owner->MeasureBookkeeping(next->resources))return result;
            if(!next->resources.Result().Complete())return MMS_E_NOT_READY;
            next->bytes=resource_budget::Collector::Sum(RetainedBytes(),next->resources.Result().totalBytes);
            if(!next->IsCurrent())return MMS_E_STALE;
            out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        constexpr std::uint64_t own=sizeof(*this)+4*sizeof(void*);
        const auto payload=std::uint64_t(before.capacity())*sizeof(Source);
        return payload>UINT64_MAX-own?UINT64_MAX:payload+own;
    }
    bool RequiresPhysicsReset() const noexcept override {return false;}
};
void ResetControls(MMDApp& app) noexcept {
    for(int lane=0;lane<4;++lane) {
        const auto edit=GetDlgItem(app.MainWindow(),panel::kMorphEdit0+lane*5);
        ui::SetReadoutTextIfChangedA(edit,"0.00000");
        // Reset is an explicit request to discard these four morph drafts.
        // Other numeric fields remain untouched, including their modify bits.
        if(edit&&SendMessageA(edit,EM_GETMODIFY,0,0))SendMessageA(edit,EM_SETMODIFY,FALSE,0);
        ui::SetTrackbarPositionIfChanged(GetDlgItem(app.MainWindow(),panel::kMorphSlider0+lane*5),0);
    }
}
}
MmsResult PlanMorphReset(MMDApp& app,unsigned slot,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();return Boundary([&]() -> MmsResult {
        auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_NOT_READY;
        if(const auto result=history->CheckNativeEdit())return result;
        if(!Ready(app)||app.CameraMode()||slot>=kModelSlotCount||app.SelectedModelSlot()!=slot||!app.ModelSlot(slot))return MMS_E_NOT_READY;
        const auto& model=*mdl::Mdl(app.ModelSlot(slot));
        if(!model.morphCount)return MMS_OK;
        if(!model.morphs||!mdl::MorphKeyPoolIdentity(model.morphKeys))return MMS_E_NOT_READY;
        bool changed=false;for(unsigned i=0;i<model.morphCount;++i)changed|=WeightBits(model.morphs[i])!=0;
        if(!changed)return MMS_OK;
        out=std::make_shared<Reset>(app,*history,slot);return MMS_OK;
    });
}
bool TryResetBoundMorphs(MMDApp* app) noexcept {
    if(!app)return false;auto* history=BoundNativePoseHistory(app);if(!history)return false;
    Boundary([&]() -> MmsResult {
        std::shared_ptr<HistoryChange> change;
        if(const auto result=PlanMorphReset(*app,app->SelectedModelSlot(),change))return result;
        const bool changed=bool(change);
        if(change) {
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if(const auto result=history->PrepareNative("Reset all morph weights",std::move(change),transaction,SharedHistory::NativeIntent::Adjustment))return result;
            if(const auto result=history->CommitNative(std::move(transaction)))return result;
        }
        // EnableWindow in the publication hints synchronously sends WM_ENABLE.
        // Block native commands before the very first hint, as well as during
        // every caption/slider echo, including a data-no-op reset.
        bake::ScopedUiInputGate gate;
        if(changed)PublishBoundPoseEdit(*app,*history,false);
        ResetControls(*app);return MMS_OK;
    });return true;
}
}
#endif
