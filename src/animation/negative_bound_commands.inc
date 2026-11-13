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
bool TryBoundNegativeCommand(MMDApp* app,unsigned id) noexcept {
    if(!app || (id!=421 && id!=423))return false;
    auto* history=plugins::BoundNativePoseHistory(app);if(!history)return false;
    // The bound delete route owns both signs, including an empty selection.
    // Refusal cannot fall through to the old positive or negative history.
    if(id==423)return plugins::TryDeleteBoundKeys(app,false);
    try {
        auto found=states.find(app);
        State* state=found==states.end()?nullptr:&found->second;
        if(id==421 && (!state || state->clipboard.empty()) && app->CurrentFrame()>=0)return false;
        // Once this route owns a command, rejection must not fall through to
        // native registrars or private rings. No live drag is cancelled here.
        if(!NegativeCommandReady(*app,*history,state))return true;
        {
            // Native clipboard admission stays detached. Failed paste does not
            // replace the saved clipboard or temporarily write live poses.
            std::vector<Key> imported;
            const auto* source=state?&state->clipboard:nullptr;
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
        if(app->MainWindow()){
            if(app->Renderer() && app->PanelDC())PanelPaint(app);else InvalidateRect(app->MainWindow(),nullptr,FALSE);
            RefreshControls(app);curve_editor::NotifyOwnedChanged(app->MainWindow());SetFocus(app->MainWindow());
        }
        return true;
    } catch(...) {
        // In particular, bad_alloc must never escape the Win32 command route
        // or activate a second implementation after preparation failed.
        return true;
    }
}
#endif
