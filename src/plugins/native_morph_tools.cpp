#include "native_morph_tools.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace mikumikustudio::plugins {
namespace {
using Key=mdl::MorphKey;using Pool=std::vector<Key>;
bool Exclusive(MMDApp& app, unsigned slot, const Key* pool, std::size_t capacity) noexcept {
    struct Range { const void* data; std::size_t count, stride; };
    const auto separate = [](Range first, Range second) {
        if (!first.data || !first.count || !second.data || !second.count) return true;
        const auto a = reinterpret_cast<std::uintptr_t>(first.data), b = reinterpret_cast<std::uintptr_t>(second.data);
        return first.stride && second.stride && first.count <= (UINTPTR_MAX - a) / first.stride &&
            second.count <= (UINTPTR_MAX - b) / second.stride &&
            !(a < b + second.count * second.stride && b < a + first.count * first.stride);
    };
    if (!pool) return false;
    const auto& own = *mdl::Mdl(app.ModelSlot(slot));
    const Range native{pool, capacity, sizeof(Key)}, appState{&app.state, 1, sizeof app.state};
    const std::array<Range, 3> writes = {{{own.morphs, own.morphCount, sizeof(mdl::MorphRecord)},
        {own.morphKeyCursors, own.morphCount, sizeof(std::uint32_t)}, {own.morphTrackActive, own.morphCount, 1}}};
    if (!separate(native, appState)) return false;
    for (const auto& write : writes) if (!separate(write, appState)) return false;
    for (unsigned index = 0; index < kModelSlotCount; ++index) if (app.ModelSlot(index)) {
        const auto& record = *mdl::Mdl(app.ModelSlot(index));
        const std::array<Range, 16> protectedRanges = {{{&record, 1, sizeof record},
            {record.morphs, record.morphCount, sizeof(mdl::MorphRecord)},
            {record.boneTable, record.boneCount, sizeof(mdl::BoneRecord)},
            {record.boneKeys, mdl::BoneKeyCapacity(record.boneKeys), sizeof(mdl::BoneKey)},
            {record.displayKeys, mdl::KeyPoolCapacity(record.displayKeys), sizeof(mdl::DisplayKey)},
            {record.morphKeyCursors, record.morphCount, sizeof(std::uint32_t)}, {record.morphTrackActive, record.morphCount, 1},
            {record.boneKeyCursors, record.boneCount, sizeof(std::uint32_t)}, {record.boneTrackActive, record.boneCount, 1},
            {record.morphKeys, mdl::MorphKeyCapacity(record.morphKeys), sizeof(Key)},
            {record.boneSelection, record.boneCount, 1}, {record.bonePhysicsState, record.boneCount, 1},
            {record.boneKeyIndices, record.boneCount, sizeof(std::uint32_t)},
            {record.morphKeyIndices, record.morphCount, sizeof(std::uint32_t)},
            {record.ikChains, record.ikChainCount, sizeof(mdl::IkChain)},
            {record.boneOrderTable, record.boneOrderCount, sizeof(mdl::BoneOrderEntry)}}};
        for (unsigned range = 0; range < protectedRanges.size(); ++range) {
            if (!(index == slot && range == 9) && !separate(native, protectedRanges[range])) return false;
            for (unsigned write = 0; write < writes.size(); ++write)
                if (!(index == slot && range == (write == 0 ? 1 : write == 1 ? 5 : 6)) &&
                    !separate(writes[write], protectedRanges[range])) return false;
        }
        for (unsigned chain = 0; record.ikChains && chain < record.ikChainCount; ++chain) {
            const Range links{record.ikChains[chain].links, record.ikChains[chain].linkCount, sizeof(std::uint16_t)};
            if (!separate(native, links)) return false;
            for (const auto& write : writes) if (!separate(write, links)) return false;
        }
        for (std::size_t key = 0; record.displayKeys && key < mdl::KeyPoolCapacity(record.displayKeys); ++key) {
            const std::array<Range, 2> nested = {{{record.displayKeys[key].ikStates, record.ikChainCount, 1},
                {record.displayKeys[key].selectorStates, record.boneOrderCount, sizeof(mdl::BoneReference)}}};
            for (const auto& item : nested) {
                if (!separate(native, item)) return false;
                for (const auto& write : writes) if (!separate(write, item)) return false;
            }
        }
    }
    return true;
}
void Add(std::uint64_t& bytes,std::uint64_t count,std::uint64_t stride=1)noexcept{
    bytes=stride&&count>(UINT64_MAX-bytes)/stride?UINT64_MAX:bytes+count*stride;
}
bool Same(Key a,Key b,bool marks=true)noexcept{if(!marks)a.allocated=b.allocated=0;return !std::memcmp(&a,&b,sizeof a);}
bool SameFloat(float a,float b)noexcept{return !std::memcmp(&a,&b,sizeof a);}
bool Ready(MMDApp& app,bool replay=false)noexcept{
    const bool own=replay&&NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress()||own)&&(!bake::UiLocked()||own)&&!SceneSaveBlocksEdits()&&
        !NativeObjectUiPending(&app)&&!app.PlaybackActive()&&!app.FrameStepPlayback()&&!app.state.recordPlaybackStartPending&&
        !GetCapture()&&!app.RecordingWindow()&&!app.FrameRangeDialog()&&!app.state.frameCopyDialog&&!app.GravitySettingDialog()&&
        !app.GroundShadowColorDialog()&&!app.AccessoryOrderArray()&&!app.state.accessoryEditDialogOpen&&!app.state.edgeThicknessDialog;
}
std::vector<std::vector<unsigned>> Chains(const Pool& pool,unsigned count){
    if(!count||count>pool.size())throw std::invalid_argument("missing morph tool roots");
    std::vector<std::vector<unsigned>> chains(count);std::vector<unsigned char> seen(pool.size());
    for(unsigned morph=0;morph<count;++morph){unsigned slot=morph,previous=0,frame=0;
        for(;;){if(slot>=pool.size()||seen[slot]||(slot<count&&slot!=morph))throw std::invalid_argument("shared morph tool chain");
            const auto& key=pool[slot];if(key.next>=pool.size()||key.previous>=pool.size()||!std::isfinite(key.value)||
                key.frame>unsigned(frame_limits::maximum)||(slot==morph?key.frame!=0:key.previous!=previous||key.frame<=frame))
                throw std::invalid_argument("invalid morph tool key");
            seen[slot]=1;chains[morph].push_back(slot);if(!key.next)break;previous=slot;frame=key.frame;slot=key.next;}
    }
    for(std::size_t slot=count;slot<pool.size();++slot)if(!seen[slot]&&pool[slot].frame)throw std::invalid_argument("orphan morph tool key");
    return chains;
}
morph_curve::NativeKeyView View(const Pool& pool)noexcept{return {&pool,[](const void* context,std::uint32_t index)noexcept->const Key*{
    const auto& keys=*static_cast<const Pool*>(context);return index<keys.size()?&keys[index]:nullptr;},pool.size()};}
struct Rule{
    std::array<char,20> name{};mdl::MorphPanel panel;std::uint8_t type;float weight;std::uint32_t cursor;unsigned char active;
    bool Identity(const mdl::MorphRecord& morph)const noexcept{return panel==morph.panel&&type==morph.type&&!std::memcmp(name.data(),morph.name,name.size());}
};
struct Mark{unsigned morph,frame;unsigned char value;bool operator==(const Mark& b)const noexcept{return morph==b.morph&&frame==b.frame&&value==b.value;}};
std::vector<Mark> Marks(const Pool& pool,const std::vector<std::vector<unsigned>>& chains,const std::vector<unsigned>& affected){
    std::vector<Mark> marks;for(unsigned morph:affected)for(unsigned slot:chains[morph])marks.push_back({morph,pool[slot].frame,pool[slot].allocated});return marks;
}
}
struct NativeMorphToolsPlan::State final:HistoryChange,std::enable_shared_from_this<State>{
    MMDApp& app;SharedHistory& history;const NativeMorphTool tool;const unsigned slot;unsigned char* const model;
    const MmsObjectRef object;std::shared_ptr<RetainedSceneObject> owner;mdl::MorphRecord* const morphs;
    const unsigned count;std::uint32_t* const cursors;unsigned char* const active;const unsigned char physics;
    const std::uint64_t identity,binding,revision;const int initialFrame,search;
    const std::uint32_t maxBefore,lastBefore;const unsigned char phaseBefore;
    std::uint32_t maxAfter,lastAfter;unsigned char phaseAfter;std::uint64_t serial=1;
    Pool original;std::vector<Rule> rules;std::vector<unsigned> affected;
    morph_curve::AuthoredLayers initialLayers;std::unique_ptr<morph_curve::AuthoredReplacement> authoring;
    struct Patch{unsigned slot;Key before,after;};std::vector<Patch> patches;
    std::vector<Mark> beforeMarks,afterMarks;unsigned blinkCount=0;bool read=false,changed=false,published=false,applied=false,preserveSelection=false;
    State(MMDApp& a,SharedHistory& h,NativeMorphTool t,std::uint64_t r):app(a),history(h),tool(t),slot(a.SelectedModelSlot()),model(a.ModelSlot(slot)),
        object(SceneObjectIdentity(model)),owner(RetainSceneObject(a,MMS_OBJECT_MODEL,slot)),morphs(mdl::Mdl(model)->morphs),
        count(mdl::Mdl(model)->morphCount),cursors(mdl::Mdl(model)->morphKeyCursors),active(mdl::Mdl(model)->morphTrackActive),physics(mdl::Mdl(model)->physicsMode),
        identity(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),binding(BoundNativePoseGeneration(&a)),revision(r),initialFrame(a.CurrentFrame()),
        search(mdl::Mdl(model)->searchCursor),maxBefore(mdl::Mdl(model)->maxFrame),lastBefore(a.LastRegisteredFrame()),phaseBefore(a.state.blinkPhase),
        maxAfter(maxBefore),lastAfter(lastBefore),phaseAfter(phaseBefore),original(mdl::Mdl(model)->morphKeys,mdl::Mdl(model)->morphKeys+mdl::MorphKeyCapacity(mdl::Mdl(model)->morphKeys)){
        (void)Chains(original,count);rules.reserve(count);
        for(unsigned morph=0;morph<count;++morph){Rule rule{{},morphs[morph].panel,morphs[morph].type,morphs[morph].value,cursors?cursors[morph]:0,std::uint8_t(active?active[morph]:0)};
            if(!std::isfinite(rule.weight))throw std::invalid_argument("nonfinite morph tool pose");std::memcpy(rule.name.data(),morphs[morph].name,rule.name.size());rules.push_back(rule);
            if(t==NativeMorphTool::MouthShift&&rule.panel==mdl::MorphPanel::mouth)affected.push_back(morph);}
        if(t==NativeMorphTool::RandomBlink){static constexpr char name[]="\x82\xdc\x82\xce\x82\xbd\x82\xab";
            for(unsigned morph=0;morph<count;++morph)if(!std::memcmp(morphs[morph].name,name,9)){affected.push_back(morph);break;}}
        std::string error;if(!morph_curve::CaptureLayers(model,initialLayers,error))throw std::invalid_argument("invalid morph tool author snapshot");
    }
    bool Structure()const noexcept{
        if(BoundNativePoseHistory(&app)!=&history||app.ModelSlot(slot)!=model||!owner->Present()||!SceneObjectPresent(model,object.id))return false;
        const auto& record=*mdl::Mdl(model);if(record.morphs!=morphs||record.morphCount!=count||record.physicsMode!=physics||
            record.morphKeyCursors!=cursors||record.morphTrackActive!=active||mdl::MorphKeyPoolIdentity(record.morphKeys)!=identity||
            mdl::MorphKeyCapacity(record.morphKeys)<original.size()||!Exclusive(app,slot,record.morphKeys,mdl::MorphKeyCapacity(record.morphKeys)))return false;
        for(unsigned morph:affected)if(!rules[morph].Identity(morphs[morph]))return false;return true;
    }
    bool InitialCurrent()const noexcept{
        if(BoundNativePoseHistory(&app)!=&history||BoundNativePoseGeneration(&app)!=binding||!Structure()||app.CameraMode()||app.SelectedModelSlot()!=slot||
            app.CurrentFrame()!=initialFrame||SceneObjectIdentity(model).generation!=object.generation||mdl::Mdl(model)->searchCursor!=search||
            mdl::Mdl(model)->maxFrame!=maxBefore||app.LastRegisteredFrame()!=lastBefore||app.state.blinkPhase!=phaseBefore||
            !morph_curve::LayersUnchanged(model,initialLayers))return false;
        const auto* keys=mdl::Mdl(model)->morphKeys;if(mdl::MorphKeyCapacity(keys)!=original.size()||std::memcmp(keys,original.data(),original.size()*sizeof(Key)))return false;
        for(unsigned morph=0;morph<count;++morph)if(!rules[morph].Identity(morphs[morph])||!SameFloat(rules[morph].weight,morphs[morph].value)||
            (cursors&&cursors[morph]!=rules[morph].cursor)||(active&&active[morph]!=rules[morph].active))return false;
        std::uint64_t current=0;return history.Revision(current)==MMS_OK&&current==revision;
    }
    MmsResult ReadReady()const noexcept{
        // Check the binding before touching a journal captured before a modal.
        if(!InitialCurrent())return MMS_E_STALE;if(auto status=history.CheckNativeNavigation())return status;
        if(auto status=history.CheckNativeEdit())return status;return Ready(app)?MMS_OK:MMS_E_BUSY;
    }
    MmsResult Finish(Pool target,int shift,unsigned blinks,unsigned char phase){
        if(auto status=ReadReady())return status;
        const auto beforeChains=Chains(original,count),afterChains=Chains(target,count);
        std::vector<unsigned char> owned((std::max)(original.size(),target.size()));
        for(unsigned morph:affected){for(unsigned key:beforeChains[morph])owned[key]=1;for(unsigned key:afterChains[morph])owned[key]=1;}
        std::vector<Patch> next;bool different=false;
        for(std::size_t key=0;key<owned.size();++key)if(owned[key]){const Key before=key<original.size()?original[key]:Key{},after=key<target.size()?target[key]:Key{};
            next.push_back({unsigned(key),before,after});different|=!Same(before,after);}
        auto layers=std::make_unique<morph_curve::AuthoredReplacement>(morph_curve::AuthoredReplacement::ShiftNative(model,affected,shift));
        auto beforeSelection=Marks(original,beforeChains,affected),afterSelection=Marks(target,afterChains,affected);
        if(auto status=ReadReady())return status;
        maxAfter=maxBefore;for(unsigned morph:affected)for(unsigned key:afterChains[morph])
            if(key>=count&&(key>=original.size()||original[key].frame==0))maxAfter=(std::max)(maxAfter,target[key].frame);
        // Shift keeps the native high-water marker; only registration raises it.
        if(tool==NativeMorphTool::MouthShift)maxAfter=maxBefore;
        lastAfter=blinks?(std::max)(lastBefore,maxAfter):lastBefore;phaseAfter=phase;
        patches=std::move(next);authoring=std::move(layers);beforeMarks=std::move(beforeSelection);afterMarks=std::move(afterSelection);
        blinkCount=blinks;changed=different||authoring->Changed();read=true;return MMS_OK;
    }
    struct Candidate final:PreparedHistoryChange{
        std::shared_ptr<State> source;const bool forward,first;const std::uint64_t serial,binding,objectGeneration;const int frame,search;
        Key* const pool;const std::size_t capacity;Pool current,target;const std::uint32_t maximum,last;const unsigned char phase;
        std::vector<float> beforeWeights,afterWeights;std::vector<std::uint32_t> beforeCursors,afterCursors;std::vector<unsigned char> beforeActive,afterActive;
        morph_curve::AuthoredLayers sourceLayers;std::unique_ptr<morph_curve::AuthoredReplacement::Target> layers;
        std::unique_ptr<mdl::MorphKeyPoolGrowth> growth;mutable resource_budget::Collector resources;
        std::uint64_t bytes=0;bool consumed=false,preserveSelection=false;
        Candidate(std::shared_ptr<State> s,bool direction):source(std::move(s)),forward(direction),first(!source->published),serial(source->serial),
            binding(BoundNativePoseGeneration(&source->app)),objectGeneration(SceneObjectIdentity(source->model).generation),frame(source->app.CurrentFrame()),
            search(mdl::Mdl(source->model)->searchCursor),pool(mdl::Mdl(source->model)->morphKeys),capacity(mdl::MorphKeyCapacity(pool)),current(pool,pool+capacity),target(current),
            maximum(mdl::Mdl(source->model)->maxFrame),last(source->app.LastRegisteredFrame()),phase(source->app.state.blinkPhase){}
        bool IsCurrent()const noexcept override{
            const auto& s=*source;if(consumed||s.serial!=serial||forward==s.applied||first==s.published||BoundNativePoseGeneration(&s.app)!=binding||
                !s.Structure()||!Ready(s.app,!first)||SceneObjectIdentity(s.model).generation!=objectGeneration||s.app.CurrentFrame()!=frame||
                mdl::Mdl(s.model)->searchCursor!=search||mdl::Mdl(s.model)->maxFrame!=maximum||s.app.LastRegisteredFrame()!=last||s.app.state.blinkPhase!=phase||
                mdl::Mdl(s.model)->morphKeys!=pool||mdl::MorphKeyCapacity(pool)!=capacity||std::memcmp(pool,current.data(),capacity*sizeof(Key))||
                !growth||!growth->IsCurrent()||!layers||!layers->IsCurrent()||!morph_curve::LayersUnchanged(s.model,sourceLayers)||
                !s.owner->CoversBookkeeping(resources))return false;
            if(first&&!s.InitialCurrent())return false;
            for(std::size_t i=0;i<s.affected.size();++i){const auto morph=s.affected[i];if(!SameFloat(s.morphs[morph].value,beforeWeights[i])||
                (s.cursors&&s.cursors[morph]!=beforeCursors[i])||(s.active&&s.active[morph]!=beforeActive[i]))return false;}return true;
        }
        void Publish()noexcept override{
            if(!IsCurrent()||!growth->Publish())std::terminate();auto& s=*source;auto& record=*mdl::Mdl(s.model);
            for(const auto& patch:s.patches)record.morphKeys[patch.slot]=target[patch.slot];layers->Publish();
            for(std::size_t i=0;i<s.affected.size();++i){const auto morph=s.affected[i];s.morphs[morph].value=afterWeights[i];
                if(s.cursors)s.cursors[morph]=afterCursors[i];if(s.active)s.active[morph]=afterActive[i];}
            if(s.maxBefore!=s.maxAfter)record.maxFrame=forward?s.maxAfter:s.maxBefore;
            if(s.lastBefore!=s.lastAfter)s.app.LastRegisteredFrame()=forward?s.lastAfter:s.lastBefore;
            if(s.phaseBefore!=s.phaseAfter)s.app.state.blinkPhase=forward?s.phaseAfter:s.phaseBefore;
            s.published=true;s.applied=forward;s.preserveSelection=preserveSelection;++s.serial;consumed=true;if(!growth->Accept())std::terminate();
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{out=bytes;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{
        out.reset();return Boundary([&]()->MmsResult{
            if(!read||!changed||forward==applied||!Structure()||!authoring||!authoring->IsCurrent()||(!published&&!InitialCurrent()))return MMS_E_STALE;
            if(!Ready(app,published))return MMS_E_BUSY;if(serial==UINT64_MAX)return MMS_E_NOT_READY;
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
            const auto& record=*mdl::Mdl(model);
            if((maxBefore!=maxAfter&&record.maxFrame!=(forward?maxBefore:maxAfter))||(lastBefore!=lastAfter&&app.LastRegisteredFrame()!=(forward?lastBefore:lastAfter))||
                (phaseBefore!=phaseAfter&&app.state.blinkPhase!=(forward?phaseBefore:phaseAfter)))return MMS_E_STALE;
            auto next=std::make_unique<Candidate>(shared_from_this(),forward);const auto chains=Chains(next->current,count);auto marks=Marks(next->current,chains,affected);
            std::size_t needed=next->capacity;for(const auto& patch:patches){const auto live=patch.slot<next->capacity?next->pool[patch.slot]:Key{};
                if(!Same(live,forward?patch.before:patch.after,!published))return MMS_E_STALE;needed=(std::max)(needed,std::size_t(patch.slot)+1);}
            next->target.resize(needed);for(const auto& patch:patches)next->target[patch.slot]=forward?patch.after:patch.before;
            const auto targetChains=Chains(next->target,count);next->preserveSelection=published&&(preserveSelection||marks!=(forward?beforeMarks:afterMarks));
            if(next->preserveSelection)for(unsigned morph:affected)for(unsigned index:targetChains[morph]){auto& key=next->target[index];
                const auto at=std::lower_bound(marks.begin(),marks.end(),std::pair<unsigned,unsigned>{morph,key.frame},[](const Mark& mark,const auto& id){return std::pair<unsigned,unsigned>{mark.morph,mark.frame}<id;});
                key.allocated=at!=marks.end()&&at->morph==morph&&at->frame==key.frame?at->value:0;}
            if(!mdl::MorphKeyPoolGrowth::Prepare(*mdl::Mdl(model),needed,next->growth))return MMS_E_MEMORY;
            next->layers=authoring->Prepare(View(next->target),forward);std::string error;
            if(!morph_curve::CaptureLayers(model,next->sourceLayers,error))return MMS_E_ARGUMENT;
            for(unsigned morph:affected){const float weight=morphs[morph].value;float old=0,value=0;if(!std::isfinite(weight)||
                !morph_curve::SampleLayersTarget(next->sourceLayers,View(next->current),int(morph),next->frame,old,error)||
                !next->layers->Sample(int(morph),next->frame,value))return MMS_E_ARGUMENT;
                const bool clean=SameFloat(weight,old);next->beforeWeights.push_back(weight);next->afterWeights.push_back(clean?value:weight);
                unsigned index=morph;while(next->target[index].frame<unsigned(next->frame)&&next->target[index].next)index=next->target[index].next;
                const bool follow=clean&&!(next->first&&tool==NativeMorphTool::RandomBlink);
                if(cursors){next->beforeCursors.push_back(cursors[morph]);next->afterCursors.push_back(follow?index:cursors[morph]);}
                if(active){next->beforeActive.push_back(active[morph]);next->afterActive.push_back(follow?std::uint8_t(next->target[index].frame>=unsigned(next->frame)):active[morph]);}
            }
            if(auto status=owner->MeasureBookkeeping(next->resources))return status;if(!next->resources.Result().Complete())return MMS_E_NOT_READY;
            next->bytes=resource_budget::Collector::Sum(RetainedBytes(),next->resources.Result().totalBytes);
            if(next->growth->CandidateCapacity()>original.size())next->bytes=resource_budget::Collector::Sum(next->bytes,
                resource_budget::Collector::Product(next->growth->CandidateCapacity()-original.size(),sizeof(Key)));
            if(!next->IsCurrent())return MMS_E_STALE;out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);Add(bytes,original.capacity(),sizeof(Key));Add(bytes,rules.capacity(),sizeof(Rule));
        Add(bytes,affected.capacity(),sizeof(unsigned));Add(bytes,patches.capacity(),sizeof(Patch));Add(bytes,beforeMarks.capacity(),sizeof(Mark));Add(bytes,afterMarks.capacity(),sizeof(Mark));
        Add(bytes,initialLayers.RetainedBytes());if(authoring)Add(bytes,authoring->RetainedBytes());return bytes;
    }
    bool RequiresPhysicsReset()const noexcept override{return false;}
};
MmsResult NativeMorphToolsPlan::Begin(MMDApp& app,NativeMorphTool tool)noexcept{
    if(state_)return MMS_E_BUSY;if(tool!=NativeMorphTool::MouthShift&&tool!=NativeMorphTool::RandomBlink)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{
        auto* history=BoundNativePoseHistory(&app);if(!history)return MMS_E_NOT_READY;
        if(auto status=history->CheckNativeNavigation())return status;if(auto status=history->CheckNativeEdit())return status;
        if(!Ready(app)||app.CameraMode()||app.SelectedModelSlot()>=kModelSlotCount||!app.SelectedModel())return MMS_E_NOT_READY;
        if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        const auto& record=*mdl::Mdl(app.SelectedModel());if(!record.morphCount||!record.morphs||!mdl::MorphKeyPoolIdentity(record.morphKeys)||
            !Exclusive(app,app.SelectedModelSlot(),record.morphKeys,mdl::MorphKeyCapacity(record.morphKeys)))return MMS_E_NOT_READY;
        std::uint64_t revision=0;if(auto status=history->Revision(revision))return status;
        auto state=std::make_shared<State>(app,*history,tool,revision);if(tool==NativeMorphTool::RandomBlink&&state->affected.empty())return MMS_E_NOT_FOUND;
        state_=std::move(state);return MMS_OK;
    });
}
MmsResult NativeMorphToolsPlan::ReadShift(int shift)noexcept{
    if(!state_||state_->tool!=NativeMorphTool::MouthShift)return MMS_E_NOT_READY;
    return Boundary([&]()->MmsResult{auto& s=*state_;if(s.read)return MMS_E_CONFLICT;if(auto status=s.ReadReady())return status;
        if(s.affected.empty()){s.read=true;return MMS_OK;}Pool target=s.original;const auto chains=Chains(target,s.count);
        for(unsigned morph:s.affected){unsigned previous=morph;for(std::size_t i=1;i<chains[morph].size();++i){const auto index=chains[morph][i];auto& key=target[index];
            const auto frame=std::int64_t(key.frame)+shift;if(frame>frame_limits::maximum)return MMS_E_ARGUMENT;
            if(frame<=0){key.frame=key.next=key.previous=0;key.allocated=0;key.value=0;continue;}
            key.frame=unsigned(frame);target[previous].next=index;key.previous=previous;previous=index;}
            target[previous].next=0;}
        return s.Finish(std::move(target),shift,0,s.phaseBefore);
    });
}
MmsResult NativeMorphToolsPlan::ReadBlink(int start,int end,int(*random)())noexcept{
    if(!state_||state_->tool!=NativeMorphTool::RandomBlink)return MMS_E_NOT_READY;
    if(start<0||end<start||end>frame_limits::maximum)return MMS_E_ARGUMENT;
    return Boundary([&]()->MmsResult{auto& s=*state_;if(s.read)return MMS_E_CONFLICT;if(auto status=s.ReadReady())return status;
        if(start==end){s.read=true;return MMS_OK;}Pool target=s.original;const unsigned morph=s.affected.front();
        const auto maximumBytes=s.history.MaxBytes();
        std::int64_t frame=start;unsigned count=0,retries=0;unsigned char phase=s.phaseBefore;
        const auto put=[&](unsigned time,float value){if(morph==0&&s.physics!=2)return;unsigned current=morph;
            while(target[current].frame<time&&target[current].next)current=target[current].next;
            if(target[current].frame==time){target[current].value=value;target[current].allocated=1;return;}
            std::size_t fresh=s.count;while(fresh<target.size()&&target[fresh].frame)++fresh;
            if(fresh>=mdl::kMorphKeyCapacityLimit)throw std::bad_alloc();
            if(fresh==target.size()){if((std::uint64_t(fresh)+1)*sizeof(Key)>maximumBytes)throw std::bad_alloc();target.resize(fresh+1);}
            auto& key=target[fresh];if(target[current].frame<time){target[current].next=unsigned(fresh);key.previous=current;key.next=0;}
            else{const auto previous=target[current].previous;target[previous].next=unsigned(fresh);key.previous=previous;key.next=current;target[current].previous=unsigned(fresh);}
            key.frame=time;key.value=value;key.allocated=1;
        };
        while(frame<end){const int sample=random?random():std::rand();if(sample<0||sample>32767)return MMS_E_ARGUMENT;
            if(BoundNativePoseHistory(&s.app)!=&s.history||BoundNativePoseGeneration(&s.app)!=s.binding)return MMS_E_STALE;
            const int q=sample*230/32768;const auto signedPhase=static_cast<std::int8_t>(phase);bool insert=false;
            if(q<90&&signedPhase<=0){frame+=7;phase=3;insert=true;}
            else if(q>50){frame+=q;phase=std::uint8_t(int(signedPhase)-1);insert=true;}
            if(!insert){if(++retries>4096)return MMS_E_FAILED;continue;}retries=0;
            if(frame+6>frame_limits::maximum)return MMS_E_ARGUMENT;
            put(unsigned(frame),0);put(unsigned(frame+2),1);put(unsigned(frame+3),1);put(unsigned(frame+6),0);++count;
        }
        return s.Finish(std::move(target),0,count,phase);
    });
}
MmsResult NativeMorphToolsPlan::Take(std::shared_ptr<HistoryChange>& out)noexcept{
    if(out)return MMS_E_ARGUMENT;if(!state_||!state_->read)return MMS_E_NOT_READY;
    if(auto status=state_->ReadReady())return status;if(state_->changed)out=state_;return MMS_OK;
}
unsigned NativeMorphToolsPlan::BlinkCount()const noexcept{return state_?state_->blinkCount:0;}
}
#endif
