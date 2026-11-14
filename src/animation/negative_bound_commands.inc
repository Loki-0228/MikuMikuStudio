// Included inside negative_frames' private namespace after clipboard helpers.
#if !MMD_ORIGINAL_PROFILE
bool NegativeInteractionReady(MMDApp& app,const State* state,bool historyTraversal=false) noexcept {
    // The real undo route has already admitted input and holds its own scene
    // lock. Only that publication may pass these two guards; all other input,
    // save, drag and dialog exclusions still apply during replay.
    const bool publication=historyTraversal && plugins::NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        (publication || (!app.SceneMutationInProgress() && !bake::UiLocked())) && !SceneSaveBlocksEdits() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !plugins::NativeModelOutlineDialogBlocks(app) &&
        (!state || state->dragging.empty());
}
bool NegativeCommandReady(MMDApp& app,plugins::SharedHistory& history,const State* state) noexcept {
    return NegativeInteractionReady(app,state) && history.CheckNativeEdit()==MMS_OK;
}
bool DeletionSelectionPoolsReady(MMDApp& app) {
    // NativeSelectionFirst uses legacy capacity fallbacks for borrowed pools.
    // Admit every pool it will scan before reading any of those key arrays.
    if(!app.CameraMode()) {
        if(app.SelectedModelSlot()>=kModelSlotCount || !app.SelectedModel())return false;
        const auto& model=*mdl::Mdl(app.SelectedModel());
        return (!model.boneKeys || mdl::BoneKeyPoolIdentity(model.boneKeys)) &&
            (!model.morphKeys || mdl::MorphKeyPoolIdentity(model.morphKeys)) &&
            (!model.displayKeys || mdl::KeyPoolIdentity(model.displayKeys));
    }
    const auto managed=[](const auto* pool){return !pool || mdl::KeyPoolIdentity(pool)!=0;};
    if(!managed(app.CameraKeys()) || !managed(app.LightKeys()) ||
        !managed(app.ShadowKeys()) || !managed(app.GravityKeys()))return false;
    for(unsigned slot=0;slot<kModelSlotCount;++slot)
        if(!managed(app.AccessoryKeys(slot)))return false;
    return true;
}
int CopiedBoneOrMorphTarget(const mdl::ModelRecord& model,const CopySourceKey& source,bool morph) noexcept {
    const unsigned count=morph?model.morphCount:model.boneCount;
    if(morph?!model.morphs:!model.boneTable)return -1;
    auto occurrence=source.occurrence;
    for(unsigned index=0;index<count;++index) {
        const char* name=morph?model.morphs[index].name:model.boneTable[index].name;
        if(std::strncmp(name,source.name.data(),source.name.size()))continue;
        if(!occurrence)return int(index);
        --occurrence;
    }
    return -1;
}
bool CopiedNegativeTarget(MMDApp& app,const CopySourceKey& source,Key& key) noexcept {
    if(!source.trackAvailable || !Current(app,key))return false;
    if(!source.model)return app.CameraMode()!=0;
    auto* model=app.CameraMode() || app.SelectedModelSlot()>=kModelSlotCount ? nullptr : app.SelectedModel();
    if(!model)return false;
    if(model==source.model)return true;
    const auto& record=*mdl::Mdl(model);
    if(!record.boneTable)return false;
    int target=source.target;
    if(target>=0) {
        target=CopiedBoneOrMorphTarget(record,source,source.morph);
        if(target<0)return false;
    } else if(target<=-1000000) {
        const auto* order=mdl::BoneOrder(model);
        if(!order || !record.boneOrderCount)return false;
        if(target==-1000000 && source.mappedBone==-1) {
            if(order[0].boneIndex!=-1)return false;
        } else {
            const int bone=CopiedBoneOrMorphTarget(record,source,false);
            if(bone<0)return false;
            unsigned match=record.boneOrderCount;
            for(unsigned i=0;i<record.boneOrderCount;++i)if(order[i].boneIndex==bone) {
                if(match!=record.boneOrderCount)return false;
                match=i;
            }
            if(match==record.boneOrderCount || match>unsigned((std::numeric_limits<int>::max)()-1000000))return false;
            target=-1000000-int(match);
        }
    } else if(target<-1) {
        const int bone=CopiedBoneOrMorphTarget(record,source,false);
        if(bone<0 || !record.ikChains)return false;
        unsigned match=record.ikChainCount;
        for(unsigned i=0;i<record.ikChainCount;++i)if(record.ikChains[i].boneIndex==bone) {
            if(match!=record.ikChainCount)return false;
            match=i;
        }
        if(match==record.ikChainCount || match>=999998)return false;
        target=-2-int(match);
    }
    key.model=model;key.target=target;
    key.identity=source.morph?static_cast<const void*>(record.morphs):record.boneTable;
    return Current(app,key);
}
bool CopiedNegativeParent(MMDApp& app,const Key& key) noexcept {
    int modelLane=-1,boneLane=-1;
    if(key.model && key.target<=-1000000){modelLane=0;boneLane=1;}
    else if(!key.model && key.target==2){modelLane=9;boneLane=10;}
    else if(!key.model && key.target>=1000){modelLane=10;boneLane=11;}
    if(modelLane<0 || key.values[modelLane]==-1)return true;
    if(!Integer(key.values[modelLane],0,kModelSlotCount-1) ||
       !Integer(key.values[boneLane],0,frame_limits::maximum))return false;
    const auto* model=mdl::Mdl(app.ModelSlot(int(key.values[modelLane])));
    return model && model->boneTable && key.values[boneLane]<model->boneCount;
}
bool CopiedNegativeValue(const Key& key) noexcept {
    if(!key.model)return GlobalValid(key.values,key.target);
    if(key.target>=0)return Valid(key.values,key.morph);
    if(!std::all_of(key.values.begin(),key.values.end(),[](double value){return std::isfinite(value);}))return false;
    if(key.target<=-1000000)
        return Integer(key.values[0],-1,254) && Integer(key.values[1],-1,frame_limits::maximum);
    return Integer(key.values[0],0,1);
}
void RefreshNegativePaste(MMDApp& app) {
    if(!app.MainWindow())return;
    if(app.Renderer() && app.PanelDC())PanelPaint(&app);else InvalidateRect(app.MainWindow(),nullptr,FALSE);
    RefreshControls(&app);curve_editor::NotifyOwnedChanged(app.MainWindow());SetFocus(app.MainWindow());
}
void PasteCopiedNegativeKeys(MMDApp& app,plugins::SharedHistory& history,const State* state,
                            const plugins::FrameCopyBatch& batch) {
    // For a production negative-only receipt, never retry with the mutable
    // side clipboard. A different mode can have replaced or cleared it.
    if(!NegativeCommandReady(app,history,state) || !plugins::FrameCopyBatchCurrent(app,batch) || !state)return;
    const int origin=app.CurrentFrame();
    const auto first=batch.first;
    if(first<frame_limits::minimum || first>=0 || origin<frame_limits::minimum || origin>=0)return;
    const bool global=app.CameraMode()!=0;
    const auto slot=app.SelectedModelSlot();
    const auto* model=global || slot>=kModelSlotCount?nullptr:app.SelectedModel();
    MmsObjectRef identity{};
    if(!global && (!model || !plugins::FindSceneObjectIdentity(model,identity)))return;
    const auto generation=state->keysGeneration;
    const auto binding=plugins::BoundNativePoseGeneration(&app);
    const auto current=[&]{
        if(!plugins::FrameCopyBatchCurrent(app,batch) || app.CurrentFrame()!=origin || (app.CameraMode()!=0)!=global ||
           plugins::BoundNativePoseHistory(&app)!=&history || plugins::BoundNativePoseGeneration(&app)!=binding)return false;
        const auto found=states.find(&app);
        if(found==states.end() || &found->second!=state || found->second.keysGeneration!=generation)return false;
        if(global)return true;
        MmsObjectRef live{};
        return app.SelectedModelSlot()==slot && app.SelectedModel()==model && plugins::FindSceneObjectIdentity(model,live) &&
            live.id.lo==identity.id.lo && live.id.hi==identity.id.hi && live.generation==identity.generation;
    };
    // Bound the extra plan before cloning the live vector or allocating
    // relocated curves. The existing history candidate additionally charges
    // retained payloads, snapshots and prepared poses before publication.
    auto remaining=(std::min)(history.MaxBytes(),std::uint64_t(64)*1024*1024);
    const auto charge=[&](std::uint64_t bytes){if(bytes>remaining)return false;remaining-=bytes;return true;};
    if(!charge(detail::RetainedKeyBytes(state->keys)))return;
    auto next=state->keys;
    for(auto& key:next)if(InActiveTimeline(app,key) && Current(app,key))key.selected=false;
    std::set<std::tuple<int,int,bool>> destinations;
    for(const auto& source:batch.negative->keys) {
        std::int32_t frame=0;
        if(source.frame<first || source.frame>=0 ||
           !plugins::ResolveSignedPasteFrame(origin,std::int64_t(source.frame)-first,frame) || frame>=0)return;
        Key key{const_cast<unsigned char*>(source.model),source.identity,source.target,frame,source.morph,
            true,source.values,std::int64_t(source.frame)-first};
        if(!CopiedNegativeTarget(app,source,key) || !CopiedNegativeValue(key) || !CopiedNegativeParent(app,key) ||
           !destinations.emplace(key.target,key.frame,key.morph).second)return;
        if(!charge(sizeof(Key)+128) || !charge(authoring::RetainedBytes(source.author)))return;
        if(frame!=source.frame && source.author && source.author->Semantics()==authoring::Mode::Authored &&
           !charge(authoring::RetainedBytes(source.author)))return;
        key.author=authoring::TranslatePaste(source.author,source.frame,frame);
        const auto at=std::find_if(next.begin(),next.end(),[&](const Key& existing){
            return SameChannel(existing,key) && existing.frame==key.frame;
        });
        if(at==next.end())next.push_back(std::move(key));else *at=std::move(key);
    }
    if(SameKeys(state->keys,next)) {
        // A same-value paste can change selection without recording an edit.
        if(!current())return;
        for(std::size_t i=0;i<next.size();++i)states.find(&app)->second.keys[i].selected=next[i].selected;
    } else {
        auto change=std::make_shared<NegativeChange>(app,*state,std::move(next),nullptr,true);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if(history.PrepareNative("Paste negative keys",std::move(change),transaction) || !current() ||
           history.CommitNative(std::move(transaction)))return;
        plugins::PublishBoundPoseEdit(app,history);
    }
    RefreshNegativePaste(app);
}
bool TryBoundNegativeCommand(MMDApp* app,unsigned id,bool useCopyReceipt) noexcept {
    if(!app || (id!=421 && id!=423))return false;
    auto* history=plugins::BoundNativePoseHistory(app);if(!history)return false;
    // The bound delete route owns both signs, including an empty selection.
    // Refusal cannot fall through to the old positive or negative history.
    if(id==423)return plugins::TryDeleteBoundKeys(app,false);
    try {
        auto found=states.find(app);
        State* state=found==states.end()?nullptr:&found->second;
        bool nativeOnly=false;
        if(useCopyReceipt) {
            using Mode=frame_clipboard::CopyMode;
            using Status=frame_clipboard::CopyBatchStatus;
            std::shared_ptr<const plugins::FrameCopyBatch> batch;
            const auto mode=app->CameraMode()?Mode::Global:Mode::Model;
            const auto status=plugins::ReadFrameCopyBatch(*app,mode,batch);
            if(status==Status::Empty || status==Status::Failed)return true;
            if(batch && batch->negative) {
                nativeOnly=batch->negative->keys.empty();
                if(!batch->hasNative) {PasteCopiedNegativeKeys(*app,*history,state,*batch);return true;}
                // The existing mixed adapter still has its old limitations.
                // In particular, do not let another mode's side clipboard
                // substitute one portion of this batch after it was copied.
                if(!nativeOnly && (!plugins::FrameCopyBatchCurrent(*app,*batch) || !state || state->clipboard.empty() ||
                    std::any_of(state->clipboard.begin(),state->clipboard.end(),[&](const Key& key){
                        return (key.model==nullptr)!=(mode==Mode::Global);
                    })))return true;
            }
        }
        if((nativeOnly || !state || state->clipboard.empty()) && app->CurrentFrame()>=0)return false;
        // Once this route owns a command, rejection must not fall through to
        // native registrars or private rings. No live drag is cancelled here.
        if(!NegativeCommandReady(*app,*history,state))return true;
        {
            // Native clipboard admission stays detached. Failed paste does not
            // replace the saved clipboard or temporarily write live poses.
            std::vector<Key> imported;
            const auto* source=state && !nativeOnly?&state->clipboard:nullptr;
            if(!source || source->empty()) {
                imported=NativeClipboard(app);
                for(auto& k:imported)k.clipboardOffset=k.frame;
                source=&imported;
            }
            if(source->empty())return true;
            auto pasted=*source;
            for(auto& k:pasted) {
                if(!PasteTarget(app,k))return true;
                const auto offset=k.clipboardOffset;
                if(offset>std::int64_t(frame_limits::maximum)-app->CurrentFrame() ||
                    offset<std::int64_t(frame_limits::minimum)-app->CurrentFrame())return true;
                const auto frame=std::int64_t(app->CurrentFrame())+offset;
                // Positive targets need the matching detached native plan. The
                // complete aggregate remains pending; never register them live.
                if(frame>=0)return true;
                k.frame=int(frame);k.selected=true;
            }
            if(!state)state=&states.try_emplace(app).first->second;
            State pending;pending.keys=state->keys;
            for(auto& k:pending.keys)if(InActiveTimeline(*app,k))k.selected=false;
            for(const auto& k:pasted)Upsert(pending,k);
            if(!CommitRegistration(app,*state,std::move(pending.keys),nullptr,"Paste negative keys",true))return true;
        }
        RefreshNegativePaste(*app);
        return true;
    } catch(...) {
        // In particular, bad_alloc must never escape the Win32 command route
        // or activate a second implementation after preparation failed.
        return true;
    }
}
#endif
