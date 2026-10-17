namespace {
struct FovSliderFixture : FovTextFixture {
    FovSliderFixture() { app->GlobalTrackSelected(GlobalTimelineTrack::Camera)=1; SyncCameraFovControls(app.get()); }
    HWND Slider() { return GetDlgItem(app->MainWindow(),panel::kFovSlider); }
    void Scroll(int tick,unsigned code=TB_THUMBTRACK) {
        g_Block=app.get(); SendMessageW(Slider(),TBM_SETPOS,TRUE,tick);
        HandleHScroll(reinterpret_cast<LPARAM>(Slider()),MAKEWPARAM(code,0));
    }
    bool Echo(float value) {
        char text[32]{};GetWindowTextA(Field(),text,sizeof(text));
        return std::strtof(text,nullptr)==value && SendMessageW(Slider(),TBM_GETPOS,0,0)==
            std::lround(std::clamp(value,1.f,125.f));
    }
};
void FovSliderParityChecks() {
    for(int tick:{-100,0,1,2,44,45,67,90,124,125,200}) {
        FovSliderFixture controlled,oracle;
        const auto before=controlled.app->CameraFov();
        oracle.Scroll(tick);
        plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);
        controlled.Scroll(tick);
        const float expected=oracle.app->CameraFov();
        Check(!std::memcmp(&controlled.app->CameraFov(),&expected,sizeof(expected))&&controlled.Echo(expected),
            "FOV slider keeps legacy trackbar range clamping and authored float conversion");
        Check(controlled.history.UndoCount()==1&&controlled.app->SceneModified(),"effective FOV slider edit creates one dirty common step");
        for(unsigned code:{unsigned(TB_LINEUP),unsigned(TB_PAGEUP),unsigned(TB_THUMBPOSITION),unsigned(TB_ENDTRACK)}) {
            controlled.app->SceneModified()=controlled.app->RecordedFrameCountByte1()=0;controlled.Scroll(tick,code);
            Check(controlled.history.UndoCount()==1&&!controlled.app->SceneModified()&&!controlled.app->RecordedFrameCountByte1(),
                "same FOV slider position and end-track do not duplicate history or dirty state");
        }
        Check(controlled.history.History(false)==MMS_OK&&controlled.app->CameraFov()==before,"FOV slider undo restores exact fractional author value");
        Check(controlled.history.History(true)==MMS_OK&&controlled.app->CameraFov()==expected,"FOV slider redo restores authored integer");
    }
}
void FovSliderHistoryChecks() {
    for(std::uint8_t mode:{std::uint8_t(0),std::uint8_t(1),std::uint8_t(7)}) {
        FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->CameraPerspective()=mode;
        const auto original=f.app->CameraFov();const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);
        const auto count=mdl::KeyPoolCapacity(f.app->CameraKeys());
        const std::vector<mdl::CameraKey> keys(f.app->CameraKeys(),f.app->CameraKeys()+count);
        f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)=0;
        f.Scroll(67);f.Scroll(92);
        Check(f.history.UndoCount()==2&&f.app->CameraFov()==92&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera),
            "live FOV positions share the common journal and activate the camera track");
        f.app->CurrentFrame()=47;f.app->CameraMode()=0;f.app->SelectedModelSlot()=1;
        f.app->CameraPerspective()=mode?0:1;
        Check(f.history.History(false)==MMS_OK&&f.app->CameraFov()==67&&f.history.History(false)==MMS_OK&&f.app->CameraFov()==original,
            "FOV slider undo crosses model selection and editor mode without seeking");
        Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.app->CameraFov()==92&&
            f.app->CameraPerspective()==(mode?0:1)&&f.app->CurrentFrame()==47,
            "FOV slider redo preserves later projection mode and playhead");
        plugins::RefreshBoundGlobalReadout(*f.app);
        Check(f.Echo(92)&&NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&
            !std::memcmp(f.app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),
            "FOV history readout leaves camera pose keys and model ring unchanged");
        f.Commit("80");Check(f.history.History(false)==MMS_OK,"seed later text edit redo after slider");
        f.app->SceneModified()=0;f.Scroll(92,TB_ENDTRACK);
        Check(f.history.RedoCount()==1&&!f.app->SceneModified(),"same slider value retains later text edit redo");
        auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(count+4);
        std::memcpy(grown,f.app->CameraKeys(),count*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
        Check(f.history.History(false)==MMS_OK&&f.app->CameraFov()==67,"FOV slider history survives camera key pool growth");
        mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(count+4);
        Check(f.history.History(false)==MMS_E_STALE&&f.app->CameraFov()==67&&f.history.UndoCount()==1,
            "replaced camera pool refuses FOV history without moving journal cursor");
    }
}
void FovSliderFailureChecks() {
    FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.Scroll(67);Check(f.history.History(false)==MMS_OK,"seed FOV slider failure redo");
    f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)=0;
    const auto original=f.app->CameraFov();const auto marks=AccessoryRegistrationMarks(*f.app);
    const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);
    f.app->cameraProjectionRefreshPending=false;f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
    bool success=false;unsigned failures=0;PoseAxisMessageProbe messages(f);
    for(long limit=0;limit<128;++limit) {
        SendMessageW(f.Slider(),TBM_SETPOS,TRUE,91);failAfter=limit;
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_THUMBTRACK);failAfter=-1;
        if(f.history.UndoCount()){success=true;break;}++failures;
        Check(f.app->CameraFov()==original&&f.history.RedoCount()==1&&!f.app->cameraProjectionRefreshPending&&
            !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Echo(original)&&f.SelectionUnchanged()&&
            AccessoryRegistrationMarks(*f.app)==marks&&NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring,
            "FOV allocation failure restores moved thumb and retains author value redo dirty flags selection and pose");
        Check(f.history.Enter()==MMS_OK,"failed FOV slider preparation releases journal gate");f.history.Leave();
    }
    Check(success&&failures>=5&&!messages.observedBusy,"FOV preparation failure sweep reaches success with UI outside publication");
    Check(f.history.History(false)==MMS_OK&&f.app->CameraFov()==original,"FOV slider remains exactly undoable after allocation failures");
}
void FovSliderGuardChecks() {
    FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto original=f.app->CameraFov();
    for(int gate=0;gate<8;++gate) {
        if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;
        if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->GroundShadowColorDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==4)f.app->state.accessoryEditDialogOpen=true;
        if(gate==5)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==6)Check(f.history.Enter()==MMS_OK,"hold common journal for FOV slider");
        if(gate==7){SetCapture(f.Field());Check(GetCapture()==f.Field(),"capture unrelated FOV text field");}
        f.Scroll(88);
        Check(f.app->CameraFov()==original&&!f.history.UndoCount()&&f.Echo(original),
            "FOV slider rejects playback recording modal busy and unrelated capture without legacy fallback");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;
        f.app->GroundShadowColorDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;f.app->FrameRangeDialog()=nullptr;
        if(gate==6)f.history.Leave();if(gate==7)ReleaseCapture();
    }
    SetCapture(f.Slider());Check(GetCapture()==f.Slider(),"capture own FOV trackbar");f.Scroll(88);
    Check(f.history.UndoCount()==1&&f.Echo(88),"own FOV trackbar capture permits live common edit");
    bool changed=true;Check(plugins::ApplyBoundCameraFov(*f.app,99,changed)!=MMS_OK&&!changed,
        "ordinary FOV API cannot borrow live slider capture exception");
    Check(plugins::ApplyBoundCameraProjection(*f.app,true,changed)!=MMS_OK&&!changed,
        "perspective API cannot borrow live FOV capture exception");
    ReleaseCapture();
    Check(plugins::TryCommitBoundCameraFovSlider(f.app.get(),f.Field())&&f.history.UndoCount()==1&&f.app->CameraFov()==88,
        "wrong source HWND is consumed without changing FOV or common history");
}
}
void NativeFovSliderChecks(){FovSliderParityChecks();FovSliderHistoryChecks();FovSliderFailureChecks();FovSliderGuardChecks();}
