namespace {
struct LightSliderFixture : LightTextFixture {
    LightSliderFixture() {
        app->GlobalTrackSelected(GlobalTimelineTrack::Light)=1;
        for(unsigned i=0;i<6;++i) {
            SendMessageW(Slider(i),TBM_SETRANGE,FALSE,MAKELPARAM(i<3?0:-100,i<3?255:100));
            if(i<3)app->LightColor()[i]=.251234f+float(i)*.1f;
            else app->LightDirection()[i-3]=.312345f+float(i)*.1f;
        }
        app->ApplyTimelineLightState();plugins::RefreshBoundGlobalReadout(*app);
    }
    HWND Slider(unsigned field){return GetDlgItem(app->MainWindow(),455+int(field));}
    void Scroll(unsigned field,int tick,unsigned code=TB_THUMBTRACK){
        g_Block=app.get();SendMessageW(Slider(field),TBM_SETPOS,TRUE,tick);
        HandleHScroll(reinterpret_cast<LPARAM>(Slider(field)),MAKEWPARAM(code,0));
    }
    int Canonical(unsigned field,float value){return int(field<3?double(value)*256.0:double(value)*100.0);}
};
void LightSliderParityChecks(){
    for(unsigned field=0;field<6;++field)for(int tick:{-500,-100,-53,-1,0,1,53,99,100,255,500}){
        LightSliderFixture f,oracle;const auto before=LightTextPose(*f.app);
        // The renderer-free fixture can execute the legacy RGB branch. The
        // legacy direction branch calls a device unconditionally; compare its
        // documented arithmetic here and validate real GPU output in the host.
        if(field<3)oracle.Scroll(field,tick);
        else {
            SendMessageW(oracle.Slider(field),TBM_SETPOS,TRUE,tick);
            const auto pos=SendMessageW(oracle.Slider(field),TBM_GETPOS,0,0);
            oracle.app->LightDirection()[field-3]=float(double(pos)/100.0);
            char text[64]{};std::snprintf(text,sizeof(text),"%+3.1f",double(oracle.app->LightDirection()[field-3]));
            SetWindowTextA(oracle.Field(field),text);
        }
        const auto expected=LightTextPose(*oracle.app);
        plugins::NativePoseTestBinding binding(*f.app,f.history);f.Scroll(field,tick);
        Check(LightTextPose(*f.app)==expected&&LightTextMirror(*f.app)&&f.history.UndoCount()==1&&f.app->SceneModified(),
            "light slider preserves legacy range and double-to-float scalar conversion with coherent CPU light");
        Check(SendMessageW(f.Slider(field),TBM_GETPOS,0,0)==SendMessageW(oracle.Slider(field),TBM_GETPOS,0,0),
            "live light direction retains its original integer tick despite float roundtrip");
        char actual[64]{},legacy[64]{};GetWindowTextA(f.Field(field),actual,sizeof(actual));GetWindowTextA(oracle.Field(field),legacy,sizeof(legacy));
        Check(std::strcmp(actual,legacy)==0,"light slider readout retains legacy RGB integer and signed direction precision");
        for(unsigned code:{unsigned(TB_LINEUP),unsigned(TB_PAGEUP),unsigned(TB_THUMBPOSITION),unsigned(TB_ENDTRACK)}){
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Scroll(field,tick,code);
            Check(f.history.UndoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "equal light tick and end track do not duplicate history or dirty state");
        }
        Check(f.history.History(false)==MMS_OK&&LightTextPose(*f.app)==before&&LightTextMirror(*f.app),
            "light slider undo restores exact fractional values and derived mirror");
        Check(f.history.History(true)==MMS_OK&&LightTextPose(*f.app)==expected&&LightTextMirror(*f.app),
            "light slider redo restores all fields coherently");
    }
}
void LightSliderFailureChecks(){
    for(unsigned field=0;field<6;++field){
        LightSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.Scroll(field,field<3?123:-53);Check(f.history.History(false)==MMS_OK,"seed light slider redo for allocation sweep");
        const auto before=LightTextPose(*f.app);const auto mirror=f.app->SceneLight();const auto ring=PoseRingDigest(f.first);
        f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)=0;
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool success=false;unsigned failures=0;
        for(long limit=0;limit<128;++limit){
            g_Block=f.app.get();SendMessageW(f.Slider(field),TBM_SETPOS,TRUE,field<3?171:-67);
            failAfter=limit;HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(field)),TB_THUMBTRACK);failAfter=-1;
            if(f.history.UndoCount()){success=true;break;}++failures;
            Check(LightTextPose(*f.app)==before&&!std::memcmp(&mirror,&f.app->SceneLight(),sizeof(mirror))&&
                f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
                !f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&PoseRingDigest(f.first)==ring&&
                SendMessageW(f.Slider(field),TBM_GETPOS,0,0)==f.Canonical(field,before[field]),
                "light slider allocation failure restores moved thumb author floats mirror redo dirty flags and selection");
            Check(f.history.Enter()==MMS_OK,"light slider failure releases history gate");f.history.Leave();
        }
        Check(success&&failures>=5&&LightTextMirror(*f.app),"light slider failure sweep reaches coherent committed edit");
        Check(f.history.History(false)==MMS_OK&&LightTextPose(*f.app)==before,"light slider remains undoable after failed preparation");
    }
}
void LightSliderGuardChecks(){
    for(unsigned field=0;field<6;++field){
        LightSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=LightTextPose(*f.app);
        for(int gate=0;gate<8;++gate){
            if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;
            if(gate==2)f.app->state.recordPlaybackStartPending=true;
            if(gate==3)f.app->GroundShadowColorDialog()=reinterpret_cast<HWND>(INT_PTR(1));
            if(gate==4)f.app->state.accessoryEditDialogOpen=true;
            if(gate==5)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));
            if(gate==6)Check(f.history.Enter()==MMS_OK,"hold light common journal");
            if(gate==7){SetCapture(f.Slider((field+1)%6));Check(GetCapture()==f.Slider((field+1)%6),"capture different light slider");}
            f.Scroll(field,field<3?177:-53);
            Check(LightTextPose(*f.app)==before&&!f.history.UndoCount()&&
                SendMessageW(f.Slider(field),TBM_GETPOS,0,0)==f.Canonical(field,before[field]),
                "light slider rejects playback recording modal busy and another slider capture and restores thumb");
            f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;
            f.app->GroundShadowColorDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;f.app->FrameRangeDialog()=nullptr;
            if(gate==6)f.history.Leave();if(gate==7)ReleaseCapture();
        }
        SetCapture(f.Slider(field));Check(GetCapture()==f.Slider(field),"own light slider captures");f.Scroll(field,field<3?177:-53);
        Check(f.history.UndoCount()==1&&LightTextMirror(*f.app),"own light slider capture permits live common edit");
        bool changed=true;Check(plugins::ApplyBoundLightField(*f.app,field,.87f,changed)!=MMS_OK&&!changed,
            "ordinary light API cannot borrow source slider capture exception");ReleaseCapture();
        if(field>=3){
            Check(f.history.Enter()==MMS_OK,"hold light history for moved tick rejection");f.Scroll(field,71);f.history.Leave();
            g_Block=f.app.get();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(field)),TB_ENDTRACK);
            Check(f.history.UndoCount()==1&&f.app->LightDirection()[field-3]==-.53f&&SendMessageW(f.Slider(field),TBM_GETPOS,0,0)==-53,
                "rejected direction tick followed by ENDTRACK preserves exact original hundredth and common history");
        }
        f.Scroll(field,field<3?189:71);Check(f.history.History(false)==MMS_OK,"seed later light slider redo");
        f.app->SceneModified()=0;f.Scroll(field,field<3?177:-53,TB_ENDTRACK);
        Check(f.history.RedoCount()==1&&!f.app->SceneModified(),"equal light tick retains later redo");
        Check(plugins::TryCommitBoundLightSlider(f.app.get(),f.Field(field))&&f.history.UndoCount()==1,
            "wrong source HWND is consumed without changing light history");
    }
}
void LightSliderHistoryChecks(){
    LightSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=LightTextPose(*f.app);
    const auto capacity=mdl::KeyPoolCapacity(f.app->LightKeys());
    const std::vector<mdl::LightKey> keys(f.app->LightKeys(),f.app->LightKeys()+capacity);
    const auto ring=PoseRingDigest(f.first);
    for(unsigned field=0;field<6;++field)f.Scroll(field,field<3?137+int(field):-53+int(field));
    const auto after=LightTextPose(*f.app);Check(f.history.UndoCount()==6,"six light sliders share one ordered journal");
    auto* grown=mdl::KeyPoolCreate<mdl::LightKey>(capacity+4);std::memcpy(grown,f.app->LightKeys(),keys.size()*sizeof(mdl::LightKey));
    mdl::KeyPoolCommitGrowth(f.app->LightKeys(),grown);
    f.app->CameraMode()=0;f.app->CurrentFrame()=53;f.app->SelectedModelSlot()=1;
    for(unsigned i=0;i<6;++i)Check(f.history.History(false)==MMS_OK,"light slider undo survives pool growth and editor switch");
    Check(LightTextPose(*f.app)==before&&LightTextMirror(*f.app),"six light undos restore original author precision");
    for(unsigned i=0;i<6;++i)Check(f.history.History(true)==MMS_OK,"six light sliders redo");
    Check(LightTextPose(*f.app)==after&&LightTextMirror(*f.app)&&f.app->CurrentFrame()==53&&PoseRingDigest(f.first)==ring&&
        !std::memcmp(f.app->LightKeys(),keys.data(),keys.size()*sizeof(mdl::LightKey)),"light redo preserves playhead model history and authored keys");
    mdl::KeyPoolDestroy(f.app->LightKeys());f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(capacity+4);
    Check(f.history.History(false)==MMS_E_STALE&&LightTextPose(*f.app)==after&&f.history.UndoCount()==6,
        "light slider history refuses replaced scene identity without moving cursor");
}
}
void NativeLightSliderChecks(){LightSliderParityChecks();LightSliderFailureChecks();LightSliderGuardChecks();LightSliderHistoryChecks();}
