void ProjectedShadowFlags(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto flags=[&]{return std::array<std::uint8_t,3>{app->GroundGridEnabled(),app->GroundShadowEnabled(),app->ProjectedShadowBlendEnabled()};};
    const auto original=flags();const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();const auto video=app->state.aviBackgroundEnabled;
    const auto ambient=app->ProjectedShadowAmbientIntensity();const auto fov=app->CameraFov();const auto distance=app->CameraDistance();
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
    const auto echo=[&]{plugins::Refresh(app);const auto data=flags();const UINT ids[]={215,221,254};
        for(unsigned i=0;i<3;++i){const auto state=GetMenuState(GetMenu(app->MainWindow()),ids[i],MF_BYCOMMAND);if(state==UINT(-1)||bool(state&MF_CHECKED)!=(data[i]!=0))return false;}return true;};
    for(unsigned field=0;field<2;++field){const auto before=flags();auto target=before;target[field+1]=before[field+1]?0:1;const auto id=field?254:221;
        const auto commit=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,id,0);};
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;app->state.dialogFlags[8]=0;
        CheckMenuItem(GetMenu(app->MainWindow()),id,before[field+1]?MF_UNCHECKED:MF_CHECKED);commit();
        Check(flags()==before&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1()&&echo()&&app->state.dialogFlags[8]==(field?0:1),
            "pending real plugin transaction rejects shadow menu authoring and restores checkmark while preserving native input signal");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit();
        Check(flags()==target&&value()==1&&history.UndoCount()==2&&echo(),"actual ground shadow and transparency menus join common plugin history");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundProjectedShadowFlag(*app,field,target[field+1]!=0,changed)==MMS_OK&&!changed&&history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),
            "same projected shadow flag retains later plugin redo and dirty state");
        Menu(app,L"Edit probe redo");const auto saved=std::filesystem::absolute(field?L"ground-shadow-transparent.pmm":L"ground-shadow-visible.pmm");
        Check(SaveSceneCopy(app,saved.c_str())&&history.UndoCount()==3,"PMM saves projected shadow flag and plugin state without discarding common history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(flags()==before&&value()==1&&echo(),"plugin undo restores original projected shadow flag and deferred menu state");
        Menu(app,L"Edit probe undo");app->CameraMode()=!mode;app->CurrentFrame()=frame+59;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(flags()==target&&value()==2&&echo()&&app->CurrentFrame()==frame+59&&app->ProjectedShadowAmbientIntensity()==ambient&&app->state.aviBackgroundEnabled==video&&
            app->CameraFov()==fov&&app->CameraDistance()==distance&&model.boneTable[0].trans[1]==bone&&model.undoState[0]==ring&&
            !std::memcmp(app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),"shadow redo across modes and frames preserves concentration video camera pose model and keys");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(flags()==before&&value()==0&&history.Reset()==MMS_OK,"restore projected shadow case and clear isolated history");app->CameraMode()=mode;app->CurrentFrame()=frame;
    }
    Check(flags()==original,"projected shadow probes restore all project display flags");
    std::puts("PASS actual projected ground shadow and transparency menus, plugin interleave, safe readout and PMM save");
}
