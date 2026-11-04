// Included inside negative_frames' private namespace after clipboard helpers.
#if !MMD_ORIGINAL_PROFILE
bool NegativeCommandReady(MMDApp& app,plugins::SharedHistory& history,const State* state) noexcept {
    return !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.SceneMutationInProgress() && !bake::UiLocked() && !SceneSaveBlocksEdits() && !GetCapture() &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !plugins::NativeModelOutlineDialogBlocks(app) &&
        (!state || state->dragging.empty()) && history.CheckNativeEdit()==MMS_OK;
}
bool CommitNegativeParticipant(MMDApp& app,plugins::SharedHistory& history,
    std::shared_ptr<plugins::HistoryChange> change,const char* label) {
    if(!change)return true;
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    if(history.PrepareNative(label,std::move(change),transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)!=MMS_OK ||
        history.CommitNative(std::move(transaction))!=MMS_OK)return false;
    plugins::PublishBoundPoseEdit(app,history,false);
    return true;
}
bool TryBoundNegativeCommand(MMDApp* app,unsigned id) noexcept {
    if(!app || (id!=421 && id!=423))return false;
    auto* history=plugins::BoundNativePoseHistory(app);if(!history)return false;
    try {
        auto found=states.find(app);
        State* state=found==states.end()?nullptr:&found->second;
        const bool selected=state && std::any_of(state->keys.begin(),state->keys.end(),
            [&](const Key& k){return k.selected && InActiveTimeline(*app,k) && Current(*app,k);});
        if(id==423 && !selected)return false;
        if(id==421 && (!state || state->clipboard.empty()) && app->CurrentFrame()>=0)return false;
        // Once this route owns a command, rejection must not fall through to
        // native registrars or private rings. No live drag is cancelled here.
        if(!NegativeCommandReady(*app,*history,state))return true;
        if(id==423) {
            bool positiveSelected=false;NativeSelectionFirst(app,&positiveSelected);
            // Mixed deletion awaits a single aggregate with the positive plan.
            // Keep BOTH halves unchanged rather than consuming only one half.
            if(positiveSelected)return true;
            std::shared_ptr<plugins::HistoryChange> change;
            if(PlanSelectedDeletion(*app,change)!=MMS_OK)return true;
            if(!CommitNegativeParticipant(*app,*history,std::move(change),"Delete negative keys"))return true;
        } else {
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
