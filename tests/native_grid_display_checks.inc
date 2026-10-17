namespace {
struct GridDisplayFixture : AccessoryRegistrationFixture {
    HWND floating=nullptr;HMENU menu=nullptr;
    GridDisplayFixture(){menu=CreateMenu();Check(menu&&AppendMenuW(menu,MF_STRING,215,L"coordinate axes")&&SetMenu(app->MainWindow(),menu),"create coordinate display menu");
        floating=CreateWindowExA(0,"STATIC","grid display",WS_OVERLAPPED,0,0,240,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(floating!=nullptr,"create hidden floating grid fixture");app->FloatingWindow()=floating;
        for(HWND owner:{app->MainWindow(),floating})Check(CreateWindowExA(0,"BUTTON","grid",WS_CHILD|BS_AUTOCHECKBOX,0,0,80,20,owner,
            reinterpret_cast<HMENU>(INT_PTR(557)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"create coordinate display checkbox");}
    ~GridDisplayFixture(){history.Reset();history.Collect();app->FloatingWindow()=nullptr;DestroyWindow(floating);SetMenu(app->MainWindow(),nullptr);DestroyMenu(menu);}
    void Toggle(unsigned route){if(route==0)CommandDispatch(nullptr,MAKEWPARAM(215,0));else if(route==1)CmdControl500(app.get(),app->MainWindow(),557,0);else DispatchSeparateWindowCommand(app.get(),557);}
    bool Echo(){const bool on=app->GroundGridEnabled()!=0;if(bool(GetMenuState(menu,215,MF_BYCOMMAND)&MF_CHECKED)!=on)return false;
        for(HWND owner:{app->MainWindow(),floating})if(SendDlgItemMessageW(owner,557,BM_GETCHECK,0,0)!=(on?BST_CHECKED:BST_UNCHECKED))return false;return true;}
};
void GridDisplayRoutingChecks(){
    for(unsigned route=0;route<3;++route)for(std::uint8_t initial:{std::uint8_t(0),std::uint8_t(1),std::uint8_t(7)}){
        GridDisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->GroundGridEnabled()=initial;
        const auto target=initial?0:1;const auto pose=NumericCameraPose(*f.app);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto accessory=f.accessory;const auto ring=PoseRingDigest(f.first);f.Toggle(route);
        Check(f.app->GroundGridEnabled()==target&&f.history.UndoCount()==1&&f.app->SceneModified()&&f.Echo(),"menu main and floating grid commands publish one common state and synchronize both controls");
        Check(f.SelectionUnchanged()&&NumericCameraPose(*f.app)==pose&&GlobalRegistrationSame(f.app->CameraKeys(),keys)&&
            !std::memcmp(&f.accessory,&accessory,sizeof(accessory))&&PoseRingDigest(f.first)==ring&&!f.app->cameraProjectionRefreshPending,
            "coordinate display edit preserves tracks authoring key pools model ring and projection state");
        f.app->CameraMode()=1;f.app->CurrentFrame()=41;f.app->SelectedModelSlot()=1;
        Check(f.history.History(false)==MMS_OK&&f.app->GroundGridEnabled()==initial&&f.app->CurrentFrame()==41,"grid undo restores exact original byte independent of mode model and frame");
        plugins::RefreshBoundGlobalReadout(*f.app);Check(f.Echo(),"history readout restores grid menu and both checkboxes");
        Check(f.history.History(true)==MMS_OK&&f.app->GroundGridEnabled()==target,"grid redo restores target state");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundGridDisplay(*f.app,target!=0,changed)==MMS_OK&&!changed&&f.history.UndoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"same grid value does not dirty or append history");
        f.Toggle((route+1)%3);Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK,"different grid entry shares chronological history");
        Check(plugins::ApplyBoundGridDisplay(*f.app,target!=0,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1,"grid no-op retains later redo branch");
    }
}
void GridDisplayFailureChecks(){
    for(bool initial:{false,true}){GridDisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->GroundGridEnabled()=initial;
        plugins::TryToggleBoundGridDisplay(f.app.get());Check(f.history.History(false)==MMS_OK,"seed grid display redo");
        const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit){SendDlgItemMessageW(f.app->MainWindow(),557,BM_SETCHECK,initial?BST_UNCHECKED:BST_CHECKED,0);
            SendDlgItemMessageW(f.floating,557,BM_SETCHECK,initial?BST_UNCHECKED:BST_CHECKED,0);CheckMenuItem(f.menu,215,initial?MF_UNCHECKED:MF_CHECKED);
            failAfter=limit;plugins::TryToggleBoundGridDisplay(f.app.get());failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(f.app->GroundGridEnabled()==initial&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.SelectionUnchanged()&&
                NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&f.Echo(),"grid preparation failure restores pre-toggled controls and preserves data pose selection dirty and redo");
            Check(f.history.Enter()==MMS_OK,"grid failure releases journal gate");f.history.Leave();}
        Check(succeeded&&failures>=5,"grid allocation sweep reaches successful publication");}
}
void GridDisplayGuardAndIdentityChecks(){
    for(unsigned route=0;route<3;++route){GridDisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->GroundGridEnabled()=1;
        f.app->PlaybackActive()=1;f.Toggle(route);f.app->PlaybackActive()=0;
        Check(f.app->GroundGridEnabled()==1&&!f.history.UndoCount()&&f.Echo(),"all grid entries reject playback without legacy toggle");
        Check(f.history.Enter()==MMS_OK,"hold journal for grid entry");f.Toggle(route);f.history.Leave();
        Check(f.app->GroundGridEnabled()==1&&!f.history.UndoCount(),"all grid entries reject busy journal");}
    GridDisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->GroundGridEnabled()=0;f.Toggle(1);
    auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(12);std::memcpy(grown,f.app->CameraKeys(),mdl::KeyPoolCapacity(f.app->CameraKeys())*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK&&f.app->GroundGridEnabled()==1,"grid state survives identity preserving scene camera pool growth");
    mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(12);
    Check(f.history.History(false)==MMS_E_STALE&&f.app->GroundGridEnabled()==1&&f.history.UndoCount()==1,"replaced scene root rejects old grid history without cursor change");
}
}
void NativeGridDisplayChecks(){GridDisplayRoutingChecks();GridDisplayFailureChecks();GridDisplayGuardAndIdentityChecks();}
