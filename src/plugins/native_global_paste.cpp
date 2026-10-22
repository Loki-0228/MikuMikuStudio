#include "native_global_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_registration.hpp"
#include "scene_resource_budget.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <array>
#include <algorithm>
#include <stdexcept>
namespace mikumikustudio::plugins {
namespace {
template<class K> bool CopySource(const K* input,std::uint32_t count,std::size_t limit,std::vector<K>& out) {
    if(count>limit||(count&&!input))return false;
    if(count)out.assign(input,input+count);return true;
}
struct GlobalPaste final:HistoryChange,std::enable_shared_from_this<GlobalPaste> {
    MMDApp& app;GlobalKeySelection selection;int playhead;
    std::uint32_t beforeLast,afterLast;std::uint64_t generation=1;bool published=false;
    std::vector<std::shared_ptr<GlobalPastePart>> parts;
    explicit GlobalPaste(MMDApp& a):app(a),selection(a),playhead(a.CurrentFrame()),beforeLast(a.LastRegisteredFrame()),afterLast(beforeLast){}
    struct Candidate final:PreparedHistoryChange {
        std::shared_ptr<GlobalPaste> source;bool forward,consumed=false;std::uint64_t generation;
        int frame;std::unique_ptr<GlobalKeySelection::Candidate> selection;
        std::vector<std::unique_ptr<PreparedGlobalPastePart>> parts;
        std::uint64_t bytes = 0;
        Candidate(std::shared_ptr<GlobalPaste> s,bool f):source(std::move(s)),forward(f),generation(source->generation),frame(source->app.CurrentFrame()){}
        bool IsCurrent() const noexcept override {
            if(consumed||source->generation!=generation||source->app.CurrentFrame()!=frame||
                source->app.LastRegisteredFrame()!=(forward?source->beforeLast:source->afterLast)||!selection->IsCurrent())return false;
            for(const auto& part:parts)if(!part->IsCurrent())return false;return true;
        }
        void Publish() noexcept override {
            if(!IsCurrent())std::terminate();
            // No participant may inspect another's shared state after this
            // point. Growth first, then restore the one selection snapshot.
            for(auto& part:parts)part->PublishOwned();
            selection->Publish();
            for(auto& part:parts)part->PublishSelection();
            source->app.LastRegisteredFrame()=forward?source->afterLast:source->beforeLast;
            source->published=true;++source->generation;consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override { out=bytes;return true; }
    };
    MmsResult PrepareWithIntent(bool forward,SharedHistory* journal,bool record,std::unique_ptr<PreparedHistoryChange>& out) {
        out.reset();if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        if((!published&&(!forward||app.CurrentFrame()!=playhead))||app.LastRegisteredFrame()!=(forward?beforeLast:afterLast))return MMS_E_STALE;
        return Boundary([&]()->MmsResult {
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            try{candidate->selection=selection.Prepare(forward,!published);}
            catch(const std::invalid_argument&){return MMS_E_STALE;}
            candidate->bytes=RetainedBytes();
            for(auto& part:parts){std::unique_ptr<PreparedGlobalPastePart> prepared;
                const auto result=journal&&!published?part->PrepareInitial(*journal,record,prepared):part->Prepare(forward,prepared);
                if(result)return result;
                std::uint64_t retained=0;
                if(prepared&&prepared->TargetRetainedBytes(retained)) {
                    const auto original=part->RetainedBytes();
                    if(retained<original)return MMS_E_FAILED;
                    candidate->bytes=resource_budget::Collector::Sum(candidate->bytes,retained-original);
                }
                if(!prepared)return MMS_E_NOT_READY;candidate->parts.push_back(std::move(prepared));}
            if(!candidate->IsCurrent())return MMS_E_STALE;out=std::move(candidate);return MMS_OK;
        });
    }
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{return PrepareWithIntent(forward,nullptr,true,out);}
    MmsResult PrepareInitial(SharedHistory& journal,bool record,std::unique_ptr<PreparedHistoryChange>& out)override{return PrepareWithIntent(true,&journal,record,out);}
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        const auto add=[&](std::uint64_t n){bytes=n>UINT64_MAX-bytes?UINT64_MAX:bytes+n;};
        add(parts.capacity()*sizeof(parts[0]));add(selection.RetainedBytes());
        for(const auto& part:parts)add(part->RetainedBytes());return bytes;
    }
};
}
MmsResult PlanGlobalKeyPaste(MMDApp& app,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult {
        const auto counts=app.ClipboardCounts();GlobalPasteSource source;source.frameOffset=unsigned(app.CurrentFrame());
        if(!CopySource(app.CameraClipboard(),counts.cameras,mdl::KeyPoolLimit<mdl::CameraKey>(),source.cameras)||
            !CopySource(app.LightClipboard(),counts.lights,mdl::KeyPoolLimit<mdl::LightKey>(),source.lights)||
            !CopySource(app.ShadowClipboard(),counts.shadows,mdl::KeyPoolLimit<mdl::SelfShadowKey>(),source.shadows)||
            !CopySource(app.GravityClipboard(),counts.gravity,mdl::KeyPoolLimit<mdl::GravityKey>(),source.gravity)||
            !CopySource(app.AccessoryClipboard(),counts.accessories,mdl::KeyPoolLimit<mdl::AccessoryKey>(),source.accessories))return MMS_E_ARGUMENT;
        return PlanGlobalKeyPaste(app,source,out);
    });
}
MmsResult PlanGlobalKeyPaste(MMDApp& app,const GlobalPasteSource& source,std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if(app.PlaybackActive()||app.FrameStepPlayback()||app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
    if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum||source.frameOffset>unsigned(frame_limits::maximum)||
        source.cameras.size()>mdl::KeyPoolLimit<mdl::CameraKey>()||source.lights.size()>mdl::KeyPoolLimit<mdl::LightKey>()||
        source.shadows.size()>mdl::KeyPoolLimit<mdl::SelfShadowKey>()||source.gravity.size()>mdl::KeyPoolLimit<mdl::GravityKey>()||
        source.accessories.size()>mdl::KeyPoolLimit<mdl::AccessoryKey>())return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult {
        if(source.cameras.empty()&&source.lights.empty()&&source.shadows.empty()&&source.gravity.empty()&&source.accessories.empty())return MMS_OK;
        std::array<bool,255> accessorySlots{};
        for(const auto& key:source.accessories){
            if(key.slot>=accessorySlots.size()||!app.AccessorySlot(key.slot))return MMS_E_NOT_READY;
            accessorySlots[key.slot]=true;
        }
        auto change=std::make_shared<GlobalPaste>(app);bool changed=false;
        const auto add=[&](auto plan)->MmsResult {GlobalPasteFamilyResult result;
            if(auto error=plan(result))return error;
            if(result.part){change->parts.push_back(std::move(result.part));changed|=result.dataChanged;
                change->afterLast=(std::max)(change->afterLast,result.maximumInsertedFrame);}
            return MMS_OK;
        };
        if(auto result=add([&](auto& r){return PlanCameraKeyPastePart(app,source,r);}))return result;
        if(auto result=add([&](auto& r){return PlanLightKeyPastePart(app,source,r);}))return result;
        if(auto result=add([&](auto& r){return PlanShadowKeyPastePart(app,source,r);}))return result;
        if(auto result=add([&](auto& r){return PlanGravityKeyPastePart(app,source,r);}))return result;
        for(unsigned slot=0;slot<accessorySlots.size();++slot)if(accessorySlots[slot])
            if(auto result=add([&](auto& r){return PlanAccessoryKeyPastePart(app,slot,source,r);}))return result;
        if(changed)out=std::move(change);return MMS_OK;
    });
}
}
#endif
