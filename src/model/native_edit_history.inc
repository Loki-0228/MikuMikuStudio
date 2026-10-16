// Included inside mikudancestudio, after the legacy snapshot helpers. The
// original-profile build retains its original undo implementation below.
#if !MMD_ORIGINAL_PROFILE
namespace native_history {
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
    bool redo=false,published=false,refreshSeek=false,restorePoseAfterSeek=false,frameChanged=false;
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
           m.undoState[0]!=cursor||m.undoState[1]!=head||m.undoDirty!=undoDirty||m.redoDirty!=redoDirty||
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
    m.maxFrame=p.newMax;m.searchCursor=p.newSearch;m.undoState[0]=p.nextCursor;frame=p.newFrame;
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
