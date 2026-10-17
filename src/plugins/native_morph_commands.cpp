#include "native_morph_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace mikudancestudio::plugins {
namespace {
bool Ready(MMDApp& app) noexcept {
    return !app.CameraMode() && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.SceneMutationInProgress() &&
        !bake::UiLocked() && !SceneSaveInProgress() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
// Non-owning scalar history follows the same chronological object lifetime
// rules as accessory scalars. Controlled import/delete nodes retain resources.
struct MorphValueChange final : HistoryChange, std::enable_shared_from_this<MorphValueChange> {
    MMDApp& app;
    const unsigned slot, index;
    unsigned char* const model;
    const MmsId id;
    mdl::MorphRecord* const table;
    const unsigned count;
    const std::uint64_t pool;
    const mdl::MorphPanel panel;
    const std::uint8_t type;
    std::array<char,20> name{};
    const float before, after;
    std::uint64_t generation=1;
    bool applied=false, published=false;
    MorphValueChange(MMDApp& a,unsigned s,unsigned i,float value) : app(a),slot(s),index(i),model(a.ModelSlot(s)),
        id(SceneObjectIdentity(model).id),table(mdl::Mdl(model)->morphs),count(mdl::Mdl(model)->morphCount),
        pool(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),panel(table[i].panel),type(table[i].type),before(table[i].value),after(value) {
        std::memcpy(name.data(),table[i].name,name.size());
    }
    bool Current() const noexcept {
        if(app.ModelSlot(slot)!=model || !SceneObjectPresent(model,id))return false;
        const auto& live=*mdl::Mdl(model);
        return live.morphs==table && live.morphCount==count && pool && mdl::MorphKeyPoolIdentity(live.morphKeys)==pool &&
            table[index].panel==panel && table[index].type==type && !std::memcmp(table[index].name,name.data(),name.size());
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<MorphValueChange> source;
        const bool forward;
        const std::uint64_t generation,objectGeneration;
        const int frame;
        const float expected;
        bool consumed=false;
        Candidate(std::shared_ptr<MorphValueChange> value,bool direction) : source(std::move(value)),forward(direction),
            generation(source->generation),objectGeneration(SceneObjectIdentity(source->model).generation),
            frame(source->app.CurrentFrame()),expected(source->table[source->index].value) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->generation==generation && forward!=source->applied && source->Current() &&
                SceneObjectIdentity(source->model).generation==objectGeneration && source->app.CurrentFrame()==frame && source->table[source->index].value==expected;
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();
            source->table[source->index].value=forward?source->after:source->before;
            source->published=true;source->applied=forward;++source->generation;consumed=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();return Boundary([&]() -> MmsResult {
            if(!Current() || forward==applied || (!published && table[index].value!=before))return MMS_E_STALE;
            if(generation==UINT64_MAX)return MMS_E_NOT_READY;
            if(!std::isfinite(table[index].value))return MMS_E_ARGUMENT;
            out=std::make_unique<Candidate>(shared_from_this(),forward);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {return sizeof(*this)+4*sizeof(void*);}
};
}
MmsResult ApplyBoundMorphValue(MMDApp& app,unsigned slot,unsigned morph,float value,bool& changed) noexcept {
    changed=false;return Boundary([&]() -> MmsResult {
        auto* history=BoundNativePoseHistory(&app);
        if(!history || !Ready(app))return MMS_E_NOT_READY;
        if(slot>=kModelSlotCount || !std::isfinite(value))return MMS_E_ARGUMENT;
        if(const auto result=history->CheckNativeEdit())return result;
        auto* raw=app.ModelSlot(slot);if(!raw)return MMS_E_NOT_READY;
        const auto& model=*mdl::Mdl(raw);
        if(!model.morphs || morph>=model.morphCount)return MMS_E_ARGUMENT;
        if(!mdl::MorphKeyPoolIdentity(model.morphKeys))return MMS_E_NOT_READY;
        if(!std::isfinite(model.morphs[morph].value))return MMS_E_ARGUMENT;
        if(model.morphs[morph].value==value)return MMS_OK;
        auto change=std::make_shared<MorphValueChange>(app,slot,morph,value);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if(const auto result=history->PrepareNative("Edit morph weight",std::move(change),transaction))return result;
        if(const auto result=history->CommitNative(std::move(transaction)))return result;
        changed=true;
        Boundary([&]() -> MmsResult {PublishBoundPoseEdit(app,*history,false);RefreshBoundMorphReadout(app);return MMS_OK;});
        return MMS_OK;
    });
}
void RefreshBoundMorphReadout(MMDApp& app) noexcept {
    if(!app.MainWindow() || app.CameraMode() || app.SelectedModelSlot()>=kModelSlotCount)return;
    const auto* model=mdl::Mdl(app.SelectedModel());if(!model || !model->morphs)return;
    for(unsigned lane=0;lane<4;++lane) {
        const int index=model->selectedMorphs[lane];const int editId=506+int(lane)*5;
        const auto edit=GetDlgItem(app.MainWindow(),editId);
        // Another command can refresh while the user is still typing here.
        if(!edit || SendMessageA(edit,EM_GETMODIFY,0,0) || index<0 || unsigned(index)>=model->morphCount)continue;
        const float value=model->morphs[index].value;if(!std::isfinite(value))continue;
        char text[64]{},previous[64]{};std::snprintf(text,sizeof(text),"%5.4f",double(value));GetWindowTextA(edit,previous,sizeof(previous));
        if(std::strcmp(text,previous))SetWindowTextA(edit,text);
        const auto position=static_cast<int>((std::clamp)(double(value)*100.0,double((std::numeric_limits<int>::min)()),double((std::numeric_limits<int>::max)())));
        SendDlgItemMessageW(app.MainWindow(),editId-1,TBM_SETPOS,TRUE,position);
    }
}
}
#endif
