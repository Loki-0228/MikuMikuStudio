// Included inside negative_frames, with access to its private authored rows.
#if !MMD_ORIGINAL_PROFILE
namespace {
namespace copy_source_detail {
static_assert(kModelSlotCount<=255,"negative copy reference capacity");
bool SameIdentity(const MmsObjectRef& a,const MmsObjectRef& b) noexcept {
    return a.id.lo==b.id.lo && a.id.hi==b.id.hi && a.generation==b.generation;
}
void Model(const MMDApp& app,CopySource& source,int slot,bool tracks=false) noexcept {
    if(slot<0 || slot>=kModelSlotCount){source.referencesAvailable=false;return;}
    auto& ref=source.models[slot];
    ref.tracks|=tracks;
    if(ref.referenced)return;
    ref.referenced=true;ref.slot=slot;ref.pointer=app.ModelSlot(slot);
    ref.available=ref.pointer && plugins::FindSceneObjectIdentity(ref.pointer,ref.identity);
    if(!ref.available){source.referencesAvailable=false;return;}
    const auto& model=*static_cast<const mdl::ModelRecord*>(ref.pointer);
    ref.bones=model.boneTable;ref.boneCount=model.boneCount;
    ref.morphs=model.morphs;ref.morphCount=model.morphCount;
    ref.ikChains=model.ikChains;ref.ikCount=model.ikChainCount;
    ref.selectors=model.boneOrderTable;ref.selectorCount=model.boneOrderCount;
}
void Parent(const MMDApp& app,CopySource& source,double slot) noexcept {
    if(slot==-1)return;
    if(!std::isfinite(slot) || slot<0 || slot>=kModelSlotCount || std::floor(slot)!=slot) {
        source.referencesAvailable=false;return;
    }
    Model(app,source,int(slot));
}
void Accessory(const MMDApp& app,CopySource& source,int slot) noexcept {
    auto& ref=source.accessories[slot];
    if(ref.referenced)return;
    ref.referenced=true;ref.slot=slot;ref.pointer=app.AccessorySlot(slot);
    ref.available=ref.pointer && plugins::FindSceneObjectIdentity(ref.pointer,ref.identity);
    if(!ref.available)source.referencesAvailable=false;
}
void Name(CopySourceKey& key,const mdl::ModelRecord& model,int bone) noexcept {
    if(bone<0 || std::uint32_t(bone)>=model.boneCount || !model.boneTable) {
        key.trackAvailable=false;return;
    }
    std::copy_n(model.boneTable[bone].name,key.name.size(),key.name.begin());
    for(int i=0;i<bone;++i)
        if(!std::strncmp(model.boneTable[i].name,key.name.data(),key.name.size()))++key.occurrence;
}
void ModelTrack(CopySource& source,CopySourceKey& key) noexcept {
    const auto& ref=source.models[source.modelSlot];
    key.trackAvailable=ref.available && key.model==ref.pointer;
    if(!key.trackAvailable)return;
    const auto& model=*static_cast<const mdl::ModelRecord*>(ref.pointer);
    key.trackAvailable=key.identity==(key.morph ? ref.morphs : ref.bones);
    if(!key.trackAvailable)return;
    if(key.target>=0) {
        if(!key.morph){Name(key,model,key.target);return;}
        if(!model.morphs || std::uint32_t(key.target)>=model.morphCount){key.trackAvailable=false;return;}
        std::copy_n(model.morphs[key.target].name,key.name.size(),key.name.begin());
        for(int i=0;i<key.target;++i)
            if(!std::strncmp(model.morphs[i].name,key.name.data(),key.name.size()))++key.occurrence;
    } else if(key.morph) {
        key.trackAvailable=false;
    } else if(key.target<=-1000000) {
        const auto selector=-1000000-std::int64_t(key.target);
        if(!model.boneOrderTable || selector>=model.boneOrderCount){key.trackAvailable=false;return;}
        key.mappedBone=static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable)[selector].boneIndex;
        // The first order entry is the whole model, not a bone track.
        if(selector!=0 || key.mappedBone!=-1)Name(key,model,key.mappedBone);
    } else if(key.target<-1) {
        const auto ik=-2-std::int64_t(key.target);
        if(!model.ikChains || ik>=model.ikChainCount){key.trackAvailable=false;return;}
        key.mappedBone=model.ikChains[ik].boneIndex;
        Name(key,model,key.mappedBone);
    }
}
void References(const MMDApp& app,CopySource& source,CopySourceKey& key) noexcept {
    if(key.model) {
        // Parent tokens belong to the raw row, even when its source track or
        // identity has disappeared and no track name can safely be read.
        if(!key.morph && key.target<=-1000000)Parent(app,source,key.values[0]);
        ModelTrack(source,key);
    }
    else if(key.target>=1000 && key.target<1255) {
        const auto slot=key.target-1000;
        Accessory(app,source,slot);
        const auto& ref=source.accessories[slot];
        key.trackAvailable=!key.morph && ref.available && key.identity==ref.pointer;
        Parent(app,source,key.values[10]);
    } else {
        key.trackAvailable=!key.morph && key.identity==&app && key.target>=2 && key.target<=5;
        if(key.target==2)Parent(app,source,key.values[9]);
    }
    if(!key.trackAvailable)source.referencesAvailable=false;
}
bool ObjectCurrent(const void* pointer,const CopyObjectReference& ref) noexcept {
    MmsObjectRef identity{};
    return ref.available && pointer && pointer==ref.pointer &&
        plugins::FindSceneObjectIdentity(pointer,identity) && SameIdentity(identity,ref.identity);
}
}
}

bool HasSelectedCopyRows(MMDApp& app) noexcept {
    const auto found=states.find(&app);
    return found!=states.end() && std::any_of(found->second.keys.begin(),found->second.keys.end(),
        [&](const Key& key){return key.selected && InActiveTimeline(app,key);});
}
MmsResult CaptureCopySource(MMDApp& app,std::shared_ptr<const CopySource>& out) noexcept {
    out.reset();
    try {
        const bool global=app.CameraMode()!=0;
        const auto slot=app.SelectedModelSlot();
        const auto* model=global || slot>=kModelSlotCount ? nullptr : app.ModelSlot(slot);
        if(!global && !model)return MMS_E_NOT_READY;
        auto source=std::make_shared<CopySource>();
        source->owner=&app;source->mode=global?CopySourceMode::Global:CopySourceMode::Model;
        source->modelSlot=global?-1:int(slot);source->sceneGeneration=app.SceneGeneration();
        if(!global)copy_source_detail::Model(app,*source,int(slot),true);
        const auto found=states.find(&app);
        if(found!=states.end()) {
            const auto& state=found->second;source->hadState=true;source->lifetime=state.lifetime;
            const auto selected=[&](const Key& key){return key.selected && (global?!key.model:key.model==model);};
            const auto count=std::count_if(state.keys.begin(),state.keys.end(),selected);
            source->keys.reserve(std::size_t(count));
            for(const auto& key:state.keys)if(selected(key)) {
                CopySourceKey copied;
                copied.model=key.model;copied.identity=key.identity;copied.target=key.target;copied.frame=key.frame;
                copied.morph=key.morph;copied.selected=key.selected;copied.values=key.values;
                copied.clipboardOffset=key.clipboardOffset;
                copied.author=key.author;
                copy_source_detail::References(app,*source,copied);
                if(source->keys.empty() || copied.frame<source->first)source->first=copied.frame;
                source->keys.push_back(copied);
            }
        }
        out=std::move(source);return MMS_OK;
    } catch(const std::bad_alloc&) {
        return MMS_E_MEMORY;
    } catch(...) {
        return MMS_E_FAILED;
    }
}

bool CopySourceCurrent(const MMDApp& app,const CopySource& source) noexcept {
    if(source.owner!=&app || source.sceneGeneration!=app.SceneGeneration() || !source.referencesAvailable)return false;
    if(source.hadState) {
        const auto found=states.find(&app);
        if(found==states.end() || source.lifetime.expired() || source.lifetime.lock()!=found->second.lifetime)return false;
    }
    for(const auto& ref:source.accessories)if(ref.referenced) {
        if(ref.slot<0 || ref.slot>=255 || !copy_source_detail::ObjectCurrent(app.AccessorySlot(ref.slot),ref))return false;
    }
    for(const auto& ref:source.models)if(ref.referenced) {
        if(ref.slot<0 || ref.slot>=kModelSlotCount || !copy_source_detail::ObjectCurrent(app.ModelSlot(ref.slot),ref))return false;
        const auto& model=*static_cast<const mdl::ModelRecord*>(ref.pointer);
        if(model.boneTable!=ref.bones || model.boneCount!=ref.boneCount)return false;
        if(ref.tracks && (model.morphs!=ref.morphs || model.morphCount!=ref.morphCount ||
            model.ikChains!=ref.ikChains || model.ikChainCount!=ref.ikCount ||
            model.boneOrderTable!=ref.selectors || model.boneOrderCount!=ref.selectorCount))return false;
    }
    // Validate only mappings actually read for selected source rows. Name and
    // value edits remain independent; moving an IK/selector to another bone
    // changes the meaning of its saved source index and is a stale dependency.
    for(const auto& key:source.keys)if(key.model && !key.morph && key.target<-1) {
        if(source.modelSlot<0 || source.modelSlot>=kModelSlotCount)return false;
        const auto& ref=source.models[source.modelSlot];
        if(!ref.referenced || !ref.tracks || !key.trackAvailable)return false;
        const auto& model=*static_cast<const mdl::ModelRecord*>(ref.pointer);
        if(key.target<=-1000000) {
            const auto selector=-1000000-std::int64_t(key.target);
            if(!model.boneOrderTable || selector>=model.boneOrderCount ||
                static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable)[selector].boneIndex!=key.mappedBone)return false;
        } else {
            const auto ik=-2-std::int64_t(key.target);
            if(!model.ikChains || ik>=model.ikChainCount || model.ikChains[ik].boneIndex!=key.mappedBone)return false;
        }
    }
    return true;
}
#endif
