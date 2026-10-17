void GridDisplayCommands(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto original=app->GroundGridEnabled();const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const auto fov=app->CameraFov();const auto distance=app->CameraDistance();const auto accessory=app->AccessorySlot(0);const auto scale=accessory->scale;
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
    const auto checkbox=GetDlgItem(app->MainWindow(),557);Check(checkbox!=nullptr,"real coordinate axis checkbox exists");
    const auto echo=[&]{plugins::Refresh(app);const bool enabled=app->GroundGridEnabled()!=0;
        return bool(GetMenuState(GetMenu(app->MainWindow()),215,MF_BYCOMMAND)&MF_CHECKED)==enabled&&SendMessageW(checkbox,BM_GETCHECK,0,0)==(enabled?BST_CHECKED:BST_UNCHECKED);};
    const auto commit=[&](unsigned route){if(route==2)DispatchSeparateWindowCommand(app,557);else SendMessageW(app->MainWindow(),WM_COMMAND,route?557:215,0);};
    for(unsigned route=0;route<3;++route){const auto before=app->GroundGridEnabled();const auto target=before?0:1;
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;
        SendMessageW(checkbox,BM_SETCHECK,before?BST_UNCHECKED:BST_CHECKED,0);CheckMenuItem(GetMenu(app->MainWindow()),215,before?MF_UNCHECKED:MF_CHECKED);
        commit(route);Check(app->GroundGridEnabled()==before&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1()&&echo(),"pending real plugin transaction rejects menu main and floating-dispatch grid toggle and restores readout");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit(route);
        Check(app->GroundGridEnabled()==target&&value()==1&&history.UndoCount()==2&&echo(),"actual coordinate display writer joins plugin common history and synchronizes menu and checkbox");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundGridDisplay(*app,target!=0,changed)==MMS_OK&&!changed&&history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"same coordinate display value retains later plugin redo and dirty state");
        Menu(app,L"Edit probe redo");const auto saved=std::filesystem::absolute(std::wstring(L"grid-display-")+std::to_wstring(route)+L".pmm");
        Check(SaveSceneCopy(app,saved.c_str())&&history.UndoCount()==3,"PMM saves grid display and plugin state without discarding history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(app->GroundGridEnabled()==before&&value()==1&&echo(),"plugin undo restores original grid flag and deferred menu checkbox readout");
        Menu(app,L"Edit probe undo");app->CameraMode()=!mode;app->CurrentFrame()=frame+53;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(app->GroundGridEnabled()==target&&value()==2&&echo()&&app->CurrentFrame()==frame+53&&app->CameraFov()==fov&&app->CameraDistance()==distance&&
            model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&app->AccessorySlot(0)==accessory&&accessory->scale==scale&&
            !std::memcmp(app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),"grid shared redo across modes and playheads preserves model accessory pose and camera keys");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(app->GroundGridEnabled()==before&&value()==0&&history.Reset()==MMS_OK,"grid entry case restores state and clears isolated history");
        app->CameraMode()=mode;app->CurrentFrame()=frame;
    }
    Check(app->GroundGridEnabled()==original,"all coordinate display cases restore original project state");
    std::puts("PASS actual coordinate menu and main checkbox plus floating dispatcher, plugin interleave, safe readout and PMM save");
}
