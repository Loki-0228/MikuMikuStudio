// Included in accessory_feedback after the real modal driver.
#if !MMD_ORIGINAL_PROFILE
void CheckAccessoryTimelineOrder(MMDApp* app) {
    Check(app->CameraMode()!=0&&app->DisplayObjectListScrollPosition()==0,"settings timeline fixture shows all accessory rows");
    int row=0;
    for(int order=0;order<255;++order)
        for(int slot=0;slot<255;++slot)
            if(const auto* accessory=app->AccessorySlot(slot);accessory&&accessory->order==order) {
                Check(app->state.jointLineMap[row++]==slot,"cached timeline row follows the current accessory order immediately");
            }
    Check(row==3&&app->state.jointLineMap[row]==-1,"timeline row mapping terminates after the three fixture accessories");
}
std::vector<COLORREF> AccessoryTimelineLabels(MMDApp* app) {
    Check(app->PanelDC()!=nullptr,"read the actual cached timeline label bitmap");
    GdiFlush();
    std::vector<COLORREF> pixels;
    pixels.reserve(79*42);
    for(int y=72;y<114;++y)for(int x=12;x<91;++x) {
        const auto value=GetPixel(app->PanelDC(),x,y);
        Check(value!=CLR_INVALID,"read accessory timeline label pixel");pixels.push_back(value);
    }
    return pixels;
}
void CheckBoundAccessorySettings(MMDApp* app, bool explicitOnly) {
    plugins::StorageStore storage; plugins::SharedHistory history(storage);
    Check(history.SetMode(explicitOnly ? plugins::SharedHistory::RecordingMode::ExplicitOnly :
        plugins::SharedHistory::RecordingMode::AllOperations)==MMS_OK,"select settings history mode");
    plugins::NativePoseTestBinding binding(*app,history);
    const auto name=text_encoding::Display(app->AccessorySlot(2)->name);
    const auto scope=GetDlgItem(app->MainWindow(),panel::kRegisterScopeCombo);
    SendMessageW(scope,CB_SETCURSEL,6,0);
    const auto position=app->AccessorySlot(1)->position[0];
    const auto frame=app->CurrentFrame();
    app->PhysicsResetPending()=0;
    CheckAccessoryTimelineOrder(app);
    const auto originalLabels=AccessoryTimelineLabels(app);
    Settings(app,1,2,0,false,MoveAction::Drag,0,L"重命名あ");Selected(app,1,2);
    CheckAccessoryTimelineOrder(app);
    const auto changedLabels=AccessoryTimelineLabels(app);
    Check(changedLabels!=originalLabels,"confirmed order and Unicode rename repaint the cached timeline labels immediately");
    Check(history.UndoCount()==1&&history.RedoCount()==0&&app->AccessoryRenderSplitOrder()==1&&
        app->AccessorySlot(2)->order==0&&text_encoding::Display(app->AccessorySlot(2)->name)==L"重命名あ",
        "actual bound dialog publishes order, Unicode name and split in one explicit history entry");
    Check(!app->AccessoryOrderArray()&&SendMessageW(scope,CB_GETCURSEL,0,0)==4&&app->CurrentFrame()==frame&&
        app->AccessorySlot(1)->position[0]==position,"controlled settings close scratch and preserve frame, pose and independent scope");
    Check(!app->PhysicsResetPending()&&!history.LastPublicationRequiresPhysicsReset(),"confirmed metadata does not invalidate simulation");
    app->CurrentFrame()=frame+37;
    Check(plugins::RunBoundObjectHistory(*app,history,false)==MMS_OK,"undo actual settings through shared wrapper");Selected(app,1,1);
    CheckAccessoryTimelineOrder(app);
    Check(AccessoryTimelineLabels(app)==originalLabels,"undo restores the original cached timeline labels without a later scroll");
    Check(!app->PhysicsResetPending(),"metadata undo preserves simulation state");
    Check(app->AccessorySlot(2)->order==2&&text_encoding::Display(app->AccessorySlot(2)->name)==name&&
        SendMessageW(scope,CB_GETCURSEL,0,0)==6&&app->CurrentFrame()==frame+37,
        "undo restores names and list geometry at a different playhead without changing scope identity");
    app->SceneModified()=0;
    Settings(app);Selected(app,1,1);
    Check(history.UndoCount()==0&&history.RedoCount()==1&&app->SceneModified()==0,
        "unchanged bound dialog preserves redo and dirty state");
    Settings(app,0,2,0,true,MoveAction::Drag,0,L"discarded rename");Selected(app,1,1);
    Check(history.UndoCount()==0&&history.RedoCount()==1&&app->SceneModified()==0&&
        app->AccessorySlot(2)->order==2&&text_encoding::Display(app->AccessorySlot(2)->name)==name,
        "cancel discards all three settings without clearing redo");
    Check(plugins::RunBoundObjectHistory(*app,history,true)==MMS_OK,"redo actual combined settings");Selected(app,1,2);
    CheckAccessoryTimelineOrder(app);
    Check(AccessoryTimelineLabels(app)==changedLabels,"redo restores renamed timeline labels without a later scroll");
    Check(!app->PhysicsResetPending(),"metadata redo preserves simulation state");
    Check(app->AccessorySlot(2)->order==0&&text_encoding::Display(app->AccessorySlot(2)->name)==L"重命名あ"&&
        app->AccessoryRenderSplitOrder()==1,"redo restores exact controlled settings");
    Check(plugins::RunBoundObjectHistory(*app,history,false,true)==MMS_OK&&plugins::NativeObjectUiPending(app),
        "deferred plugin-style metadata undo marks unchanged object lists for refresh");
    Check(plugins::RefreshBoundObjectUi(*app,history)&&!plugins::NativeObjectUiPending(app),"deferred metadata readout is retryable");
    Selected(app,1,1);
    CheckAccessoryTimelineOrder(app);
    Check(AccessoryTimelineLabels(app)==originalLabels,"deferred metadata refresh also rebuilds timeline labels and row mapping");
    Check(plugins::RunBoundObjectHistory(*app,history,true)==MMS_OK,"restore target before PMM round trip");
    Check(app->CurrentFrame()==frame+37&&app->AccessorySlot(1)->position[0]==position,"all settings history is pose-independent");
    Check(!app->PhysicsResetPending(),"deferred metadata refresh preserves simulation state");
    Check(history.Reset()==MMS_OK,"release settings history before scene reset");history.Collect();
    std::printf("PASS accessory combined history explicitOnly=%d\n",explicitOnly);
}
#endif
