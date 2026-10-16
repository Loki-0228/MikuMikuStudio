// Included inside mikudancestudio, after the legacy snapshot helpers. The
// original-profile build retains its original undo implementation below.
#if !MMD_ORIGINAL_PROFILE
namespace native_history {
void MeasureResources(const mdl::ModelRecord& model, plugins::resource_budget::Collector& out) {
    using C = plugins::resource_budget::Collector;
    for (const auto& ring : model.undoRings) for (const auto& entry : ring.slots) {
        // Pose operations can keep an all-bones allocation after compacting
        // selected entries. Charging all bones is a conservative upper bound.
        if (entry.bonePose) out.Array(entry.bonePose, (std::max)(std::uint64_t(model.boneCount),
            entry.dirty > 0 ? std::uint64_t(entry.dirty) : 0));
        if (!entry.auxiliaryPose) continue;
        if (entry.operation == 6) { sequence_scatter::MeasureSnapshot(entry.auxiliaryPose, out); continue; }
        if (entry.dirty < 0) { out.MissingResource(plugins::resource_budget::UndoCapacity); continue; }
        std::uint64_t bytes = C::Product(std::uint64_t(entry.dirty), sizeof(mdl::BoneKeySnapshot));
        if (entry.operation == 7 && entry.reserved0 >= 0) {
            const auto offset = C::Product(C::Product(std::uint64_t(entry.reserved0), 3), sizeof(mdl::BoneKeySnapshot));
            if (offset > SIZE_MAX || std::uint64_t(entry.reserved0) > mdl::BoneKeyCapacity(model)) {
                out.MissingResource(plugins::resource_budget::UndoCapacity); continue;
            }
            DeleteAuxHeader h{};
            std::memcpy(&h, static_cast<const unsigned char*>(entry.auxiliaryPose) + std::size_t(offset), sizeof(h));
            const auto displayStride = C::Sum(sizeof(mdl::DisplayKey), C::Sum(h.ikCount, C::Product(h.selectorCount, sizeof(mdl::BoneReference))));
            bytes = C::Sum(offset, C::Sum(sizeof(h), C::Sum(C::Product(h.morphCapacity, sizeof(mdl::MorphKey)), C::Product(h.displayCapacity, displayStride))));
        } else if (entry.operation != 5) {
            // Operations 2/4 reserve selectedCount*3 but retain only dirty.
            // That reservation count was never stored. Old unrelated aux
            // buffers can also survive a later pose-only operation.
            out.MissingResource(plugins::resource_budget::UndoCapacity);
        }
        out.Add(entry.auxiliaryPose, 1, bytes);
    }
}
namespace {
using Bytes = std::vector<unsigned char>;
using Buffer = std::unique_ptr<void, decltype(&std::free)>;
Buffer Allocate(std::size_t bytes) {
    Buffer result(bytes ? ::operator new(bytes) : nullptr, &std::free);
    if(bytes)std::memset(result.get(),0,bytes);
    return result;
}
bool SameRecord(const mdl::UndoRecord& a, const mdl::UndoRecord& b) noexcept {
    return a.operation==b.operation && a.dirty==b.dirty && a.reserved0==b.reserved0 &&
        a.frame==b.frame && a.bonePose==b.bonePose && a.auxiliaryPose==b.auxiliaryPose && a.reserved1==b.reserved1;
}
struct Step {
    unsigned ring=0;
    mdl::UndoRecord undo{}, redo{}, replacement{};
    Buffer pose{nullptr,&std::free}, aux{nullptr,&std::free};
    Bytes sourcePose, sourceAux;
};
struct KeyChange { mdl::BoneKey before, after; };
struct Pose {
    float position[3], rotation[4];
    unsigned char dirty, physics;
};
Pose ReadPose(const mdl::ModelRecord& m, std::size_t i) noexcept {
    Pose p{};
    std::memcpy(p.position,m.boneTable[i].trans,sizeof(p.position));
    std::memcpy(p.rotation,m.boneTable[i].rotQuat,sizeof(p.rotation));
    p.dirty=m.bonePhysicsState?m.bonePhysicsState[i]:0;p.physics=m.boneTable[i].physicsDisabled;
    return p;
}
bool SamePose(const Pose& a,const Pose& b) noexcept {
    return !std::memcmp(a.position,b.position,sizeof(a.position)) &&
        !std::memcmp(a.rotation,b.rotation,sizeof(a.rotation)) && a.dirty==b.dirty && a.physics==b.physics;
}
void CopyBytes(Bytes& out,const void* source,std::size_t size) {
    if(size&&!source)throw std::runtime_error("Missing history snapshot");
    out.resize(size);if(size)std::memcpy(out.data(),source,size);
}
bool SameBytes(const Bytes& bytes,const void* source) noexcept {
    return bytes.empty()||(source&&!std::memcmp(bytes.data(),source,bytes.size()));
}
}

struct ModelEdit::Impl {
    unsigned char* model=nullptr;
    const void *bones=nullptr,*keys=nullptr,*morphs=nullptr,*displays=nullptr,*dirty=nullptr;
    std::size_t boneCount=0,keyCapacity=0,morphCapacity=0,displayCapacity=0,ikCount=0,selectorCount=0;
    unsigned cursor=0,head=0,nextCursor=0,oldMax=0,newMax=0;
    int oldSearch=0,newSearch=0,oldFrame=0,newFrame=0;
    unsigned char undoDirty=0,redoDirty=0;
    bool redo=false,published=false,refreshSeek=false,restorePoseAfterSeek=false,frameChanged=false,ownedPose=false;
    std::vector<Step> steps;
    std::vector<Pose> beforePose,afterPose;
    std::map<int,KeyChange> keyChanges;
    std::vector<mdl::MorphKey> beforeMorph,afterMorph;
    Bytes beforeDisplay,afterDisplay;
    std::unique_ptr<SideState> scale,curves,morphLayers;

    void EnsurePose() {
        if(!beforePose.empty()||!boneCount)return;
        const auto& m=*mdl::Mdl(model);
        if(!m.boneTable)throw std::runtime_error("Missing history bones");
        beforePose.reserve(boneCount);
        for(std::size_t i=0;i<boneCount;++i)beforePose.push_back(ReadPose(m,i));
        afterPose=beforePose;
    }
    void EnsureMorph() {
        if(!beforeMorph.empty())return;
        const auto& m=*mdl::Mdl(model);
        if(!m.morphKeys||!morphCapacity)throw std::runtime_error("Missing history morph keys");
        beforeMorph.assign(m.morphKeys,m.morphKeys+morphCapacity);afterMorph=beforeMorph;
    }
    void EnsureDisplay() {
        if(!beforeDisplay.empty())return;
        const auto& m=*mdl::Mdl(model);
        if(!m.displayKeys||!displayCapacity)throw std::runtime_error("Missing history display keys");
        DeleteAuxHeader layout{0,displayCapacity,ikCount,selectorCount};
        beforeDisplay.resize(DeleteAuxSize(layout));CaptureDeleteAux(m,layout,beforeDisplay.data());afterDisplay=beforeDisplay;
    }
    void Poses(Step& step,const mdl::UndoRecord& source,int type) {
        const std::size_t count=type==1||type==3?static_cast<std::size_t>(source.dirty):boneCount;
        if(count>boneCount)throw std::runtime_error("Invalid history pose count");
        // Key-only redo does not use the pose snapshot, except type 5's live
        // physics switch. Native callers evaluate the restored keys afterward.
        if(redo&&type!=1&&type!=3&&type!=5)return;
        EnsurePose();CopyBytes(step.sourcePose,source.bonePose,count*sizeof(mdl::BonePoseSnapshot));
        if(!redo)step.pose=Allocate(count*sizeof(mdl::BonePoseSnapshot));
        for(std::size_t i=0;i<count;++i){
            mdl::BonePoseSnapshot target;std::memcpy(&target,step.sourcePose.data()+i*sizeof(target),sizeof(target));
            if(target.boneIndex<0||std::size_t(target.boneIndex)>=boneCount)throw std::runtime_error("Invalid history bone index");
            auto& p=afterPose[target.boneIndex];
            if(!redo){auto& saved=static_cast<mdl::BonePoseSnapshot*>(step.pose.get())[i];
                saved.boneIndex=target.boneIndex;std::memcpy(saved.position,p.position,sizeof(p.position));
                std::memcpy(saved.rotation,p.rotation,sizeof(p.rotation));saved.physicsDisabled=p.dirty;saved.reserved[0]=p.physics;
            }
            if(!redo||type==1||type==3){std::memcpy(p.position,target.position,sizeof(p.position));
                std::memcpy(p.rotation,target.rotation,sizeof(p.rotation));p.dirty=target.physicsDisabled;}
            if(type==5)p.physics=target.reserved[0];
        }
    }
    void Aux(Step& step,const mdl::UndoRecord& source,int type) {
        if(source.dirty<0||std::size_t(source.dirty)>keyCapacity||(!keys&&source.dirty))throw std::runtime_error("Invalid history key count");
        std::size_t size=std::size_t(source.dirty)*sizeof(mdl::BoneKeySnapshot);
        DeleteAuxHeader layout{};
        if(type==7){
            if(source.reserved0<0||std::size_t(source.reserved0)>keyCapacity||std::size_t(source.dirty)>std::size_t(source.reserved0)*3||!source.auxiliaryPose)
                throw std::runtime_error("Invalid deletion history");
            std::memcpy(&layout,static_cast<const unsigned char*>(source.auxiliaryPose)+DeleteAuxOffset(source),sizeof(layout));
            if(layout.morphCapacity>morphCapacity||layout.displayCapacity>displayCapacity||layout.ikCount!=ikCount||layout.selectorCount!=selectorCount)
                throw std::runtime_error("Stale deletion history");
            size=DeleteAuxOffset(source)+DeleteAuxSize(layout);
        }
        CopyBytes(step.sourceAux,source.auxiliaryPose,size);
        if(!redo)step.aux=Allocate(size);
        for(int i=0;i<source.dirty;++i){
            mdl::BoneKeySnapshot target;std::memcpy(&target,step.sourceAux.data()+std::size_t(i)*sizeof(target),sizeof(target));
            if(target.keyIndex<0||std::size_t(target.keyIndex)>=keyCapacity)throw std::runtime_error("Invalid history key index");
            const auto key=mdl::Mdl(model)->boneKeys[target.keyIndex];
            auto& change=keyChanges.try_emplace(target.keyIndex,KeyChange{key,key}).first->second;
            if(!redo)static_cast<mdl::BoneKeySnapshot*>(step.aux.get())[i]={target.keyIndex,change.after};
            change.after=target.key;
        }
        if(type!=7)return;
        const auto offset=DeleteAuxOffset(source);
        const auto* from=step.sourceAux.data()+offset+sizeof(layout);
        auto* capture=redo?nullptr:static_cast<unsigned char*>(step.aux.get())+offset;
        if(capture){std::memcpy(capture,&layout,sizeof(layout));capture+=sizeof(layout);}
        const auto morphBytes=layout.morphCapacity*sizeof(mdl::MorphKey);
        if(morphBytes){EnsureMorph();if(capture)std::memcpy(capture,afterMorph.data(),morphBytes);std::memcpy(afterMorph.data(),from,morphBytes);}
        from+=morphBytes;if(capture)capture+=morphBytes;
        if(layout.displayCapacity){
            EnsureDisplay();const auto displayBytes=DeleteAuxSize(layout)-sizeof(layout)-morphBytes;
            if(capture)std::memcpy(capture,afterDisplay.data()+sizeof(layout),displayBytes);
            std::memcpy(afterDisplay.data()+sizeof(layout),from,displayBytes);
        }
    }
    bool Current(int frame) const noexcept {
        if(!model||published||frame!=oldFrame)return false;
        const auto& m=*mdl::Mdl(model);
        if(m.boneTable!=bones||m.boneKeys!=keys||m.morphKeys!=morphs||m.displayKeys!=displays||m.bonePhysicsState!=dirty||
           m.boneCount!=boneCount||m.ikChainCount!=ikCount||m.boneOrderCount!=selectorCount||
           (!ownedPose&&(m.undoState[0]!=cursor||m.undoState[1]!=head||m.undoDirty!=undoDirty||m.redoDirty!=redoDirty))||
           m.maxFrame!=oldMax||m.searchCursor!=oldSearch||mdl::BoneKeyCapacity(m)!=keyCapacity||
           mdl::MorphKeyCapacity(m.morphKeys)!=morphCapacity||mdl::KeyPoolCapacity(m.displayKeys)!=displayCapacity)return false;
        for(std::size_t i=0;i<beforePose.size();++i)if(!SamePose(beforePose[i],ReadPose(m,i)))return false;
        for(const auto& item:keyChanges)if(std::memcmp(&item.second.before,&m.boneKeys[item.first],sizeof(mdl::BoneKey)))return false;
        if(!beforeMorph.empty()&&std::memcmp(beforeMorph.data(),m.morphKeys,beforeMorph.size()*sizeof(mdl::MorphKey)))return false;
        if(!beforeDisplay.empty()){
            auto* data=beforeDisplay.data()+sizeof(DeleteAuxHeader);
            for(std::size_t i=0;i<displayCapacity;++i){const auto& key=m.displayKeys[i];
                if(std::memcmp(data,&key,sizeof(key)))return false;data+=sizeof(key);
                if(ikCount&&key.ikStates&&std::memcmp(data,key.ikStates,ikCount))return false;data+=ikCount;
                const auto bytes=selectorCount*sizeof(mdl::BoneReference);
                if(bytes&&key.selectorStates&&std::memcmp(data,key.selectorStates,bytes))return false;data+=bytes;
            }
        }
        for(const auto& step:steps){
            if(!SameRecord(m.undoRings[0].slots[step.ring],step.undo)||!SameRecord(m.undoRings[1].slots[step.ring],step.redo))return false;
            const auto& source=redo?step.redo:step.undo;
            if(!SameBytes(step.sourcePose,source.bonePose)||!SameBytes(step.sourceAux,source.auxiliaryPose))return false;
        }
        return (!scale||scale->IsCurrent())&&(!curves||curves->IsCurrent())&&(!morphLayers||morphLayers->IsCurrent());
    }
};

ModelEdit::ModelEdit()=default;
ModelEdit::~ModelEdit()=default;
ModelEdit::ModelEdit(ModelEdit&&) noexcept=default;
ModelEdit& ModelEdit::operator=(ModelEdit&&) noexcept=default;

namespace {
class OwnedScalePose final:public SideState {
    unsigned char* model_;
    std::uint64_t revision_;
    bone_scale::Values before_,target_;
public:
    OwnedScalePose(unsigned char* model,bone_scale::Values target)
        :model_(model),revision_(bone_scale::Revision(model)),before_(bone_scale::PoseValues(model)),target_(std::move(target)) {}
    bool IsCurrent() const noexcept override {
        return revision_==bone_scale::Revision(model_)&&(target_.empty()||bone_scale::HasPoseStorage(model_))&&bone_scale::PoseValuesEqual(model_,before_);
    }
    void Publish() noexcept override { bone_scale::ExchangePoseValues(model_,target_); }
};
enum PoseField : unsigned char { PositionX=1,PositionY=2,PositionZ=4,Rotation=8,Dirty=16 };
unsigned char ChangedPoseFields(const Pose& before,const Pose& after) noexcept {
    unsigned char fields=0;
    for(unsigned axis=0;axis<3;++axis)
        if(std::memcmp(&before.position[axis],&after.position[axis],sizeof(float)))fields|=static_cast<unsigned char>(1u<<axis);
    if(std::memcmp(before.rotation,after.rotation,sizeof(before.rotation)))fields|=Rotation;
    if(before.dirty!=after.dirty)fields|=Dirty;
    return fields;
}
struct OwnedPoseChange { std::size_t index; unsigned char fields; Pose before,after; };
struct OwnedScaleChange {
    int index;
    bool beforePresent,afterPresent;
    bone_scale::Matrix before,after;
};
void WritePose(mdl::ModelRecord& model,std::size_t index,const Pose& pose) noexcept {
    std::memcpy(model.boneTable[index].trans,pose.position,sizeof(pose.position));
    std::memcpy(model.boneTable[index].rotQuat,pose.rotation,sizeof(pose.rotation));
    if(model.bonePhysicsState)model.bonePhysicsState[index]=pose.dirty;
}
std::uint64_t PoseBytes(std::uint64_t total,std::uint64_t count,std::uint64_t stride) noexcept {
    if(stride&&count>(UINT64_MAX-total)/stride)return UINT64_MAX;
    return total+count*stride;
}
}
struct OwnedPoseEdit::Impl {
    unsigned char* model=nullptr;
    const void* bones=nullptr;
    std::size_t boneCount=0;
    std::vector<OwnedPoseChange> changes;
    std::vector<OwnedScaleChange> scaleChanges;
};
struct PoseEditCapture::Impl {
    unsigned char* model=nullptr;
    const void* bones=nullptr;
    const void* dirty=nullptr;
    std::vector<Pose> before;
    bone_scale::Values scale;
    std::unique_ptr<OwnedPoseEdit> staged;
    std::uint64_t maximumBytes=0;
    bool active=true,finished=false;
    bool SameModel() const noexcept {
        if(!active||!model)return false;
        const auto& live=*mdl::Mdl(model);
        return live.boneTable==bones&&live.boneCount==before.size()&&live.bonePhysicsState==dirty;
    }
};
OwnedPoseEdit::OwnedPoseEdit()=default;
OwnedPoseEdit::~OwnedPoseEdit()=default;
OwnedPoseEdit::OwnedPoseEdit(OwnedPoseEdit&&) noexcept=default;
OwnedPoseEdit& OwnedPoseEdit::operator=(OwnedPoseEdit&&) noexcept=default;
PoseEditCapture::PoseEditCapture()=default;
PoseEditCapture::~PoseEditCapture()=default;
PoseEditCapture::PoseEditCapture(PoseEditCapture&&) noexcept=default;
PoseEditCapture& PoseEditCapture::operator=(PoseEditCapture&&) noexcept=default;
Result PoseEditCapture::Begin(unsigned char* model) noexcept {
    if(!model||(impl_&&impl_->active))return Result::Invalid;
    try {
        const auto& live=*mdl::Mdl(model);
        if(live.boneCount&&!live.boneTable)return Result::Invalid;
        auto next=std::make_unique<Impl>();next->model=model;next->bones=live.boneTable;next->dirty=live.bonePhysicsState;
        next->before.reserve(live.boneCount);
        for(std::size_t i=0;i<live.boneCount;++i)next->before.push_back(ReadPose(live,i));
        next->scale=bone_scale::PoseValues(model);
        for(const auto& value:next->scale)if(value.first<0||std::size_t(value.first)>=live.boneCount)return Result::Invalid;
        next->staged=std::make_unique<OwnedPoseEdit>();next->staged->impl_=std::make_unique<OwnedPoseEdit::Impl>();
        auto& record=*next->staged->impl_;record.model=model;record.bones=live.boneTable;record.boneCount=live.boneCount;
        record.changes.reserve(live.boneCount);record.scaleChanges.reserve(live.boneCount);
        next->maximumBytes=next->staged->RetainedBytes();impl_=std::move(next);return Result::Ready;
    }catch(const std::bad_alloc&){return Result::Memory;}catch(...){return Result::Invalid;}
}
std::uint64_t PoseEditCapture::MaximumRetainedBytes() const noexcept { return impl_&&impl_->active?impl_->maximumBytes:0; }
Result PoseEditCapture::Finish(int operation,std::unique_ptr<OwnedPoseEdit>& out) noexcept {
    out.reset();
    if(operation!=1||!impl_||impl_->finished||!impl_->SameModel())return Result::Invalid;
    try {
        const auto& live=*mdl::Mdl(impl_->model);
        auto& record=*impl_->staged->impl_;record.changes.clear();record.scaleChanges.clear();
        for(std::size_t i=0;i<record.boneCount;++i){const auto after=ReadPose(live,i);const auto& before=impl_->before[i];
            // Switching the physical bone mode is type 5, not a pose edit.
            if(before.physics!=after.physics)return Result::Invalid;
            const auto fields=ChangedPoseFields(before,after);
            if(fields)record.changes.push_back({i,fields,before,after});
        }
        const auto afterScale=bone_scale::PoseValues(impl_->model);
        for(const auto& value:afterScale)if(value.first<0||std::size_t(value.first)>=record.boneCount)return Result::Invalid;
        auto before=impl_->scale.cbegin(),after=afterScale.cbegin();
        while(before!=impl_->scale.end()||after!=afterScale.end()) {
            if(after==afterScale.end()||(before!=impl_->scale.end()&&before->first<after->first)) {
                record.scaleChanges.push_back({before->first,true,false,before->second,{}});++before;
            } else if(before==impl_->scale.end()||after->first<before->first) {
                record.scaleChanges.push_back({after->first,false,true,{},after->second});++after;
            } else {
                if(before->second!=after->second)record.scaleChanges.push_back({before->first,true,true,before->second,after->second});
                ++before;++after;
            }
        }
        if(record.changes.empty()&&record.scaleChanges.empty()){impl_->finished=true;return Result::Empty;}
        out=std::move(impl_->staged);impl_->finished=true;return Result::Ready;
    }catch(const std::bad_alloc&){return Result::Memory;}catch(...){return Result::Invalid;}
}
bool PoseEditCapture::Rollback() noexcept {
    if(!impl_||!impl_->SameModel()||(!impl_->scale.empty()&&!bone_scale::HasPoseStorage(impl_->model)))return false;
    auto& live=*mdl::Mdl(impl_->model);
    for(std::size_t i=0;i<impl_->before.size();++i)
        if(live.boneTable[i].physicsDisabled!=impl_->before[i].physics)return false;
    if(!bone_scale::ExchangePoseValues(impl_->model,impl_->scale))return false;
    for(std::size_t i=0;i<impl_->before.size();++i)WritePose(live,i,impl_->before[i]);
    impl_->active=false;return true;
}
std::uint64_t OwnedPoseEdit::RetainedBytes() const noexcept {
    std::uint64_t bytes=sizeof(*this);if(!impl_)return bytes;
    bytes=PoseBytes(bytes,1,sizeof(Impl));bytes=PoseBytes(bytes,impl_->changes.capacity(),sizeof(OwnedPoseChange));
    return PoseBytes(bytes,impl_->scaleChanges.capacity(),sizeof(OwnedScaleChange));
}
Result OwnedPoseEdit::Prepare(unsigned char* model,std::int32_t frame,bool forward,ModelEdit& out) const noexcept {
    out.impl_.reset();
    if(!impl_||!model||impl_->model!=model)return Result::Invalid;
    try {
        const auto& live=*mdl::Mdl(model);
        if(live.boneTable!=impl_->bones||live.boneCount!=impl_->boneCount)return Result::Invalid;
        auto targetScale=bone_scale::PoseValues(model);
        for(const auto& change:impl_->scaleChanges) {
            if(forward?change.afterPresent:change.beforePresent)targetScale[change.index]=forward?change.after:change.before;
            else targetScale.erase(change.index);
        }
        if(!targetScale.empty()&&!bone_scale::HasPoseStorage(model))return Result::Invalid;
        auto next=std::make_unique<ModelEdit::Impl>();next->model=model;next->ownedPose=true;next->redo=forward;
        next->bones=live.boneTable;next->keys=live.boneKeys;next->morphs=live.morphKeys;next->displays=live.displayKeys;next->dirty=live.bonePhysicsState;
        next->boneCount=live.boneCount;next->keyCapacity=mdl::BoneKeyCapacity(live);next->morphCapacity=mdl::MorphKeyCapacity(live.morphKeys);
        next->displayCapacity=mdl::KeyPoolCapacity(live.displayKeys);next->ikCount=live.ikChainCount;next->selectorCount=live.boneOrderCount;
        next->oldFrame=next->newFrame=frame;next->oldMax=next->newMax=live.maxFrame;next->oldSearch=next->newSearch=live.searchCursor;
        next->EnsurePose();
        for(const auto& change:impl_->changes){auto& target=next->afterPose[change.index];
            const auto& value=forward?change.after:change.before;
            for(unsigned axis=0;axis<3;++axis)if(change.fields&(1u<<axis))target.position[axis]=value.position[axis];
            if(change.fields&Rotation)std::memcpy(target.rotation,value.rotation,sizeof(target.rotation));
            if(change.fields&Dirty)target.dirty=value.dirty;
        }
        next->scale=std::make_unique<OwnedScalePose>(model,std::move(targetScale));
        out.impl_=std::move(next);return Result::Ready;
    }catch(const std::bad_alloc&){return Result::Memory;}catch(...){return Result::Invalid;}
}
Result ModelEdit::Prepare(unsigned char* model,std::int32_t frame,bool redo) noexcept {
    impl_.reset();if(!model)return Result::Invalid;
    try {
        auto p=std::make_unique<Impl>();const auto& m=*mdl::Mdl(model);p->model=model;p->redo=redo;
        p->cursor=m.undoState[0];p->head=m.undoState[1];if(p->cursor>=30||p->head>=30)return Result::Invalid;
        p->bones=m.boneTable;p->keys=m.boneKeys;p->morphs=m.morphKeys;p->displays=m.displayKeys;p->dirty=m.bonePhysicsState;
        p->boneCount=m.boneCount;p->keyCapacity=mdl::BoneKeyCapacity(m);p->morphCapacity=mdl::MorphKeyCapacity(m.morphKeys);
        p->displayCapacity=mdl::KeyPoolCapacity(m.displayKeys);p->ikCount=m.ikChainCount;p->selectorCount=m.boneOrderCount;
        p->oldFrame=p->newFrame=frame;p->oldMax=p->newMax=m.maxFrame;p->oldSearch=p->newSearch=m.searchCursor;
        p->undoDirty=m.undoDirty;p->redoDirty=m.redoDirty;
        std::vector<RingStep> sideSteps;
        unsigned ring=redo?(p->cursor+1)%30:p->cursor;
        for(unsigned count=0;;++count){
            if(count==30)return Result::Invalid;
            Step step;step.ring=ring;step.undo=m.undoRings[0].slots[ring];step.redo=m.undoRings[1].slots[ring];
            const auto& source=redo?step.redo:step.undo;const int type=source.operation;
            if(type==0)return p->steps.empty()?Result::Empty:Result::Invalid;
            if(type<1||type>7||source.dirty<0)return Result::Invalid;
            step.replacement=source;step.replacement.bonePose=nullptr;step.replacement.auxiliaryPose=nullptr;
            if(type==6){
                p->EnsureMorph();if(!sequence_scatter::PrepareHistory(model,source.auxiliaryPose,p->afterMorph,p->morphLayers,p->newMax,p->newSearch))return Result::Invalid;
                p->newFrame=int(source.frame);p->frameChanged=true;p->refreshSeek=true;
            } else {
                sideSteps.push_back({ring,redo});p->Poses(step,source,type);
                if(type!=1&&type!=3){
                    p->Aux(step,source,type);
                    step.replacement.operation=type==5||type==7?type:2;
                    if(type==5){step.replacement.reserved0=int(p->newMax);step.replacement.reserved1=p->newSearch;
                        p->newMax=unsigned(source.reserved0);p->newSearch=source.reserved1;}
                    p->newFrame=int(redo?step.undo.frame:source.frame);p->frameChanged=true;
                }else{
                    step.replacement.operation=1;
                    if(type==3){p->newFrame=int(source.frame);p->frameChanged=true;}
                }
                if(type==7&&!redo){p->refreshSeek=true;p->restorePoseAfterSeek=true;}
            }
            p->steps.push_back(std::move(step));p->nextCursor=redo?ring:(ring+29)%30;
            if(type==6)break;
            const bool chain=redo?m.undoRings[0].slots[ring].operation==4:m.undoRings[0].slots[p->nextCursor].operation==4;
            if(!chain)break;
            ring=redo?(ring+1)%30:p->nextCursor;
        }
        if(!sideSteps.empty()){p->scale=bone_scale::PrepareHistory(model,sideSteps);p->curves=bone_curve::PrepareHistory(model,sideSteps);}
        impl_=std::move(p);return Result::Ready;
    }catch(const std::bad_alloc&){return Result::Memory;}catch(...){return Result::Invalid;}
}
bool ModelEdit::IsCurrent(std::int32_t frame) const noexcept { return impl_&&impl_->Current(frame); }
bool ModelEdit::Publish(std::int32_t& frame) noexcept {
    if(!IsCurrent(frame))return false;
    auto& p=*impl_;auto& m=*mdl::Mdl(p.model);
    if(p.scale)p.scale->Publish();if(p.curves)p.curves->Publish();
    for(std::size_t i=0;i<p.afterPose.size();++i){const auto& pose=p.afterPose[i];
        std::memcpy(m.boneTable[i].trans,pose.position,sizeof(pose.position));std::memcpy(m.boneTable[i].rotQuat,pose.rotation,sizeof(pose.rotation));
        if(m.bonePhysicsState)m.bonePhysicsState[i]=pose.dirty;m.boneTable[i].physicsDisabled=pose.physics;
    }
    for(const auto& item:p.keyChanges)m.boneKeys[item.first]=item.second.after;
    if(!p.keyChanges.empty())mdl::BoneKeyVisitClear(m);
    if(!p.afterMorph.empty())std::memcpy(m.morphKeys,p.afterMorph.data(),p.afterMorph.size()*sizeof(mdl::MorphKey));
    if(!p.afterDisplay.empty())ApplyDeleteAux(m,p.afterDisplay.data());
    if(p.morphLayers)p.morphLayers->Publish();
    if(!p.redo)for(auto& step:p.steps)if(step.undo.operation!=6){auto& entry=m.undoRings[1].slots[step.ring];
        std::free(entry.bonePose);std::free(entry.auxiliaryPose);entry=step.replacement;
        entry.bonePose=static_cast<mdl::BonePoseSnapshot*>(step.pose.release());entry.auxiliaryPose=step.aux.release();
    }
    if(!p.ownedPose){m.maxFrame=p.newMax;m.searchCursor=p.newSearch;m.undoState[0]=p.nextCursor;frame=p.newFrame;}
    p.published=true;return true;
}
void ModelEdit::Refresh(int physicsMode) {
    if(!impl_||!impl_->published)return;
    auto& p=*impl_;auto& m=*mdl::Mdl(p.model);
    if(physicsMode>=2)for(std::size_t i=0;i<p.afterPose.size();++i)
        if(m.boneTable[i].hasRigidBody&&p.beforePose[i].physics!=p.afterPose[i].physics)NotifyBonePhysicsMode(p.model,int(i),p.afterPose[i].physics);
    if(p.frameChanged)SetFrameEdit(p.model,p.newFrame);
    if(p.refreshSeek){SeekModelFrame(p.model,p.newFrame,physicsMode);
        if(p.restorePoseAfterSeek)for(std::size_t i=0;i<p.afterPose.size();++i){const auto& pose=p.afterPose[i];
            std::memcpy(m.boneTable[i].trans,pose.position,sizeof(pose.position));std::memcpy(m.boneTable[i].rotQuat,pose.rotation,sizeof(pose.rotation));
            if(m.bonePhysicsState)m.bonePhysicsState[i]=pose.dirty;
        }
    }
}
}
#endif

bool TryUndoModelEdit(unsigned char* model,std::int32_t* frame,int physicsMode) {
#if MMD_ORIGINAL_PROFILE
    if(!model||!frame)return false;UndoModelEdit(model,frame,physicsMode);return true;
#else
    if(!frame)return false;native_history::ModelEdit edit;
    if(edit.Prepare(model,*frame,false)!=native_history::Result::Ready||!edit.Publish(*frame))return false;
    edit.Refresh(physicsMode);return true;
#endif
}
bool TryRedoModelEdit(unsigned char* model,std::int32_t* frame,int physicsMode) {
#if MMD_ORIGINAL_PROFILE
    if(!model||!frame)return false;RedoModelEdit(model,frame,physicsMode);return true;
#else
    if(!frame)return false;native_history::ModelEdit edit;
    if(edit.Prepare(model,*frame,true)!=native_history::Result::Ready||!edit.Publish(*frame))return false;
    edit.Refresh(physicsMode);return true;
#endif
}
