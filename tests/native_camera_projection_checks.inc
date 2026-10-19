namespace mikumikustudio { void CmdControl400(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
struct CameraProjectionFixture : FovTextFixture {
    CameraProjectionFixture(){app->CameraMode()=1;app->CameraDistance()=-45.125f;
        Check(CreateWindowExA(0,"BUTTON","perspective",WS_CHILD|BS_AUTOCHECKBOX,0,0,80,20,app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(446)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"camera perspective checkbox exists");}
    void Toggle(){CmdControl400(app.get(),app->MainWindow(),446,0);}
    bool Echo(){return SendDlgItemMessageW(app->MainWindow(),446,BM_GETCHECK,0,0)==(app->CameraPerspective()?BST_UNCHECKED:BST_CHECKED);}
};
void CameraProjectionRoutingChecks() {
    for(std::uint8_t initial:{std::uint8_t(0),std::uint8_t(1),std::uint8_t(7)}) {
        CameraProjectionFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->CameraPerspective()=initial;
        const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);const float fov=f.app->CameraFov();
        const auto count=mdl::KeyPoolCapacity(f.app->CameraKeys());const std::vector<mdl::CameraKey> keys(f.app->CameraKeys(),f.app->CameraKeys()+count);
        const auto target=initial?0:1;f.Toggle();
        Check(f.app->CameraPerspective()==target&&f.history.UndoCount()==1&&f.app->cameraProjectionRefreshPending&&f.Echo()&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera),"perspective native command toggles legacy byte and defers projection after one common edit");
        Check(NumericCameraPose(*f.app)==pose&&f.app->CameraFov()==fov&&PoseRingDigest(f.first)==ring&&
            !std::memcmp(f.app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),"projection toggle does not change numeric camera pose FOV keys or model ring");
        f.app->CurrentFrame()=37;f.app->CameraMode()=0;f.app->SelectedModelSlot()=1;f.app->CameraFov()=67.125f;f.app->CameraDistance()=-88.75f;
        Check(f.history.History(false)==MMS_OK&&f.app->CameraPerspective()==initial&&f.app->CameraFov()==67.125f&&f.app->CameraDistance()==-88.75f&&f.app->CurrentFrame()==37,
            "projection undo restores exact original mode byte and preserves later FOV distance mode and playhead");
        Check(f.history.History(true)==MMS_OK&&f.app->CameraPerspective()==target,"projection redo works after editor mode and model selection changes");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundCameraProjection(*f.app,target!=0,changed)==MMS_OK&&!changed&&f.history.UndoCount()==1&&!f.app->SceneModified(),"same projection is no-op without scene dirty");
        f.Commit("71.875");Check(f.app->CameraFov()==71&&f.history.UndoCount()==2,"FOV text shares projection journal record type");
        f.app->CameraPerspective()=7;
        Check(f.history.History(false)==MMS_OK&&f.app->CameraFov()==67.125f&&f.app->CameraPerspective()==7,"FOV undo preserves later projection mode despite shared record");
        Check(plugins::ApplyBoundCameraProjection(*f.app,true,changed)==MMS_OK&&changed,"explicit projection normalizes nonstandard byte as an actual edit");
        Check(f.history.History(false)==MMS_OK&&f.app->CameraPerspective()==7,"projection exact byte undo survives explicit normalization");
        // This is a same-value request only after the stored byte has reached 0/1.
        Check(f.history.History(true)==MMS_OK&&f.app->CameraPerspective()==1,"redo normalized projection");
        f.Commit("72");Check(f.history.History(false)==MMS_OK,"seed redo after projection");
        f.app->SceneModified()=0;Check(plugins::ApplyBoundCameraProjection(*f.app,true,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1&&!f.app->SceneModified(),"projection no-op retains later FOV redo");
    }
}
void CameraProjectionFailureChecks() {
    for(bool orthographic:{false,true}) {
        CameraProjectionFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->CameraPerspective()=orthographic?0:1;
        const auto before=f.app->CameraPerspective();const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);
        f.Toggle();Check(f.history.History(false)==MMS_OK,"seed camera projection redo");
        f.app->cameraProjectionRefreshPending=false;f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;
        unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit) {
            SendDlgItemMessageW(f.app->MainWindow(),446,BM_SETCHECK,before?BST_CHECKED:BST_UNCHECKED,0);
            failAfter=limit;f.Toggle();failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(f.app->CameraPerspective()==before&&NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&
                f.history.RedoCount()==1&&!f.app->cameraProjectionRefreshPending&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.SelectionUnchanged()&&f.Echo(),
                "projection allocation failure restores checkbox and retains authoring pending flag selection old ring dirty and redo");
            Check(f.history.Enter()==MMS_OK,"projection failure releases journal gate");f.history.Leave();
        }
        Check(succeeded&&failures>=5&&f.app->cameraProjectionRefreshPending,"projection allocation sweep reaches successful atomic publication");
    }
}
void CameraProjectionIdentityAndGuardChecks() {
    CameraProjectionFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    for(int guard=0;guard<3;++guard){
        if(guard==0)f.app->PlaybackActive()=1;
        if(guard==1)f.app->FrameStepPlayback()=1;
        if(guard==2)f.app->state.recordPlaybackStartPending=true;
        f.Toggle();Check(!f.history.UndoCount()&&f.app->CameraPerspective()==0&&f.Echo(),"playback stepping or pending recording blocks projection without legacy GPU path");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;
    }
    Check(f.history.Enter()==MMS_OK,"hold journal for projection busy check");f.Toggle();f.history.Leave();
    Check(!f.history.UndoCount()&&f.app->CameraPerspective()==0,"busy journal blocks projection");
    f.Toggle();auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(8);mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&f.app->CameraPerspective()==0&&f.history.History(true)==MMS_OK,"projection record survives identity preserving pool growth");
    auto* replacement=mdl::KeyPoolCreate<mdl::CameraKey>(8);mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=replacement;
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==1&&f.app->CameraPerspective()==1,"replaced camera scene rejects projection undo without moving cursor");
}
}
void NativeCameraProjectionChecks(){CameraProjectionRoutingChecks();CameraProjectionFailureChecks();CameraProjectionIdentityAndGuardChecks();}
