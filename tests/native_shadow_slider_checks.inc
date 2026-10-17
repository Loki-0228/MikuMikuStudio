namespace {
struct ShadowSliderFixture : ShadowParameterFixture {
    ShadowSliderFixture(){SyncSelfShadowControls(app.get());}
    HWND Slider(){return GetDlgItem(app->MainWindow(),panel::kSelfShadowRangeSlider);}
    void Scroll(int tick,unsigned code=TB_THUMBTRACK){g_Block=app.get();SendMessageW(Slider(),TBM_SETPOS,TRUE,tick);HandleHScroll(reinterpret_cast<LPARAM>(Slider()),MAKEWPARAM(code,0));}
};
void ShadowSliderParityChecks(){
    for(int tick:{0,1,2,9,10,53,100,1234,4321,5000,6789,9998,9999}){
        ShadowSliderFixture controlled,oracle;for(auto* f:{&controlled,&oracle})f->app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)=1;
        oracle.Scroll(tick);g_Block=controlled.app.get();plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);controlled.Scroll(tick);
        Check(!std::memcmp(&controlled.app->ShadowDistance(),&oracle.app->ShadowDistance(),sizeof(float))&&controlled.Echo(1,tick)&&oracle.Echo(1,tick),"self-shadow slider distance bits and readout match actual unbound WM_HSCROLL at range boundaries");
        Check(controlled.history.UndoCount()==1&&controlled.app->SceneModified(),"self-shadow slider actual author change creates one common step");
        Check(controlled.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&controlled.accessory.rowSelected==1,"already active shadow row preserves mixed row selection on slider edit");
        for(unsigned code:{unsigned(TB_LINEUP),unsigned(TB_LINEDOWN),unsigned(TB_THUMBPOSITION),unsigned(TB_ENDTRACK)}){
            controlled.app->SceneModified()=controlled.app->RecordedFrameCountByte1()=0;controlled.Scroll(tick,code);
            Check(controlled.history.UndoCount()==1&&!controlled.app->SceneModified()&&!controlled.app->RecordedFrameCountByte1(),"duplicate line thumb-position and end-track notifications do not create shadow history");}
    }
}
void ShadowSliderHistoryChecks(){
    ShadowSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const float before=f.app->ShadowDistance();const auto ring=PoseRingDigest(f.first);
    const auto marks=AccessoryRegistrationMarks(*f.app);const auto accessory=f.accessory;const auto pose=NumericCameraPose(*f.app);
    f.Scroll(4321);const auto after=f.app->ShadowDistance();
    Check(f.history.UndoCount()==1&&f.Echo(1,4321)&&f.app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)&&!f.app->GlobalTrackSelected(GlobalTimelineTrack::Light),"successful range drag activates previously unselected shadow track");
    f.Scroll(8765);Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==after,"two effective drag positions have separate chronological history");
    f.app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)=0;f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)=1;f.accessory.rowSelected=1;
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Scroll(4321,TB_ENDTRACK);
    Check(f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&!f.app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)&&f.accessory.rowSelected==1,"same final drag position preserves redo dirty and current selection");
    f.app->SelfShadowMode()=2;f.app->CurrentFrame()=47;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
    Check(f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==2&&f.app->CurrentFrame()==47,"drag undo restores exact original float while preserving later mode and frame");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.app->ShadowDistance()==float((10000.0-8765)/100000.0)&&f.app->SelfShadowMode()==2,"self-shadow drag redo owns author value without evaluating key data");
    // Direct history publication intentionally leaves readout to the safe UI boundary.
    plugins::RefreshBoundGlobalReadout(*f.app);Check(f.Echo(2,8765),"safe shadow readout follows final common redo");
    Check(PoseRingDigest(f.first)==ring&&AccessoryRegistrationMarks(*f.app)==marks&&AccessoryRegistrationPoseEqual(f.accessory,accessory)&&NumericCameraPose(*f.app)==pose,"slider common history preserves model rings keys accessory transform and camera pose");
}
void ShadowSliderNotificationChecks(){
    // Each effective keyboard/page/thumb position must take the same path as a
    // live drag, even while shadow rendering is disabled (mode zero).
    for(int mode=0;mode<3;++mode){
        ShadowSliderFixture controlled,oracle;
        // The renderer-free legacy oracle must already own this track:
        // RefreshRequest(-3) otherwise enters its full UI language sweep.
        // Track activation itself is covered separately by HistoryChecks.
        for(auto* f:{&controlled,&oracle})f->app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)=1;
        controlled.app->SelfShadowMode()=oracle.app->SelfShadowMode()=mode;
        SyncSelfShadowControls(controlled.app.get());SyncSelfShadowControls(oracle.app.get());
        const auto capacity=mdl::KeyPoolCapacity(controlled.app->ShadowKeys());
        const std::vector<mdl::SelfShadowKey> keys(controlled.app->ShadowKeys(),controlled.app->ShadowKeys()+capacity);
        const float original=controlled.app->ShadowDistance();
        const unsigned codes[]={TB_LINEUP,TB_LINEDOWN,TB_PAGEUP,TB_PAGEDOWN,TB_TOP,TB_BOTTOM,TB_THUMBPOSITION,TB_THUMBTRACK};
        unsigned steps=0;
        for(unsigned code:codes){
            const int tick=++steps*1000;
            oracle.Scroll(tick,code);
            {plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);
                controlled.app->SceneModified()=controlled.app->RecordedFrameCountByte1()=0;controlled.Scroll(tick,code);}
            Check(controlled.history.UndoCount()==steps&&controlled.app->SceneModified()&&controlled.app->RecordedFrameCountByte1(),
                "each effective line page endpoint or thumb notification publishes one dirty common step");
            Check(!std::memcmp(&controlled.app->ShadowDistance(),&oracle.app->ShadowDistance(),sizeof(float))&&controlled.Echo(mode,tick),
                "effective non-drag notifications retain legacy conversion and immediate readout in every shadow mode");
        }
        plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);
        for(unsigned i=0;i<steps;++i)Check(controlled.history.History(false)==MMS_OK,"undo every effective shadow notification");
        Check(controlled.app->ShadowDistance()==original&&controlled.app->SelfShadowMode()==mode,"notification sequence undo restores original float and shadow mode");
        for(unsigned i=0;i<steps;++i)Check(controlled.history.History(true)==MMS_OK,"redo every effective shadow notification");
        plugins::RefreshBoundGlobalReadout(*controlled.app);
        Check(controlled.Echo(mode,8000)&&!std::memcmp(controlled.app->ShadowKeys(),keys.data(),keys.size()*sizeof(mdl::SelfShadowKey)),
            "notification sequence redo restores readout without registering or changing shadow keys");
    }
}
void ShadowSliderFailureChecks(){
    for(int mode=0;mode<3;++mode){ShadowSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->SelfShadowMode()=mode;f.Scroll(8765);Check(f.history.History(false)==MMS_OK,"seed shadow slider failure redo");
        f.app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow)=0;f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)=1;f.accessory.rowSelected=1;
        const auto before=f.app->ShadowDistance();const auto marks=AccessoryRegistrationMarks(*f.app);const auto ring=PoseRingDigest(f.first);const auto pose=NumericCameraPose(*f.app);const auto track=f.app->state.globalTrackSelected[2];
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool success=false;unsigned failures=0;PoseAxisMessageProbe messages(f);
        for(long limit=0;limit<128;++limit){SendMessageW(f.Slider(),TBM_SETPOS,TRUE,4321);failAfter=limit;HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_THUMBTRACK);failAfter=-1;
            if(f.history.UndoCount()){success=true;break;}++failures;
            Check(f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==mode&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
                f.app->state.globalTrackSelected[2]==track&&f.SelectionUnchanged()&&f.Echo(mode,ShadowSliderPosition(f.app.get()))&&PoseRingDigest(f.first)==ring&&AccessoryRegistrationMarks(*f.app)==marks&&NumericCameraPose(*f.app)==pose,
                "self-shadow slider allocation failure retains distance mode redo dirty selection and keys and restores moved thumb");
            Check(f.history.Enter()==MMS_OK,"failed shadow slider releases journal gate");f.history.Leave();}
        Check(success&&failures>=5&&!messages.observedBusy,"shadow slider allocation sweep reaches success with notification outside publication");
        Check(f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==before,"shadow slider stays exactly undoable after failures");
    }
}
void ShadowSliderGuardChecks(){
    ShadowSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const float before=f.app->ShadowDistance();
    for(int gate=0;gate<8;++gate){if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->GroundShadowColorDialog()=reinterpret_cast<HWND>(INT_PTR(1));if(gate==4)f.app->state.accessoryEditDialogOpen=true;
        if(gate==5)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));if(gate==6)Check(f.history.Enter()==MMS_OK,"hold journal for range slider");
        if(gate==7){SetCapture(f.Field());Check(GetCapture()==f.Field(),"capture unrelated range edit");}
        f.Scroll(4321);Check(f.app->ShadowDistance()==before&&!f.history.UndoCount()&&f.Echo(1,ShadowSliderPosition(f.app.get())),"range slider refuses playback recording other dialog busy and unrelated capture without legacy fallback");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;f.app->GroundShadowColorDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;f.app->FrameRangeDialog()=nullptr;
        if(gate==6)f.history.Leave();if(gate==7)ReleaseCapture();}
    SetCapture(f.Slider());Check(GetCapture()==f.Slider(),"capture own self-shadow slider");f.Scroll(4321);bool changed=true;
    Check(f.history.UndoCount()==1&&f.Echo(1,4321),"own self-shadow capture permits live history commit");
    Check(plugins::ApplyBoundShadowDistance(*f.app,.02f,changed)!=MMS_OK&&!changed&&f.history.UndoCount()==1,"ordinary distance API cannot reuse slider capture exemption");ReleaseCapture();
    const auto after=f.app->ShadowDistance();Check(plugins::TryCommitBoundShadowSlider(f.app.get(),f.Field())&&f.app->ShadowDistance()==after&&f.history.UndoCount()==1,"wrong source HWND is consumed without range edit");
    const auto size=mdl::KeyPoolCapacity(f.app->ShadowKeys());auto* grown=mdl::KeyPoolCreate<mdl::SelfShadowKey>(size+4);std::memcpy(grown,f.app->ShadowKeys(),size*sizeof(mdl::SelfShadowKey));mdl::KeyPoolCommitGrowth(f.app->ShadowKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==before&&f.history.History(true)==MMS_OK,"range slider history survives shadow key pool growth");
    mdl::KeyPoolDestroy(f.app->ShadowKeys());f.app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(size+4);
    Check(f.history.History(false)==MMS_E_STALE&&f.app->ShadowDistance()==after&&f.history.UndoCount()==1,"replaced shadow pool refuses slider history without moving cursor");
}
}
void NativeShadowSliderChecks(){ShadowSliderParityChecks();ShadowSliderHistoryChecks();ShadowSliderNotificationChecks();ShadowSliderFailureChecks();ShadowSliderGuardChecks();}
