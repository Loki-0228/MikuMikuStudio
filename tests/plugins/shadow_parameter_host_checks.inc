void ShadowParameters(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto originalDistance=app->ShadowDistance();const auto originalMode=app->SelfShadowMode();
    const auto originalEditor=app->CameraMode();const auto frame=app->CurrentFrame();
    const std::vector<mdl::SelfShadowKey> keys(app->ShadowKeys(),app->ShadowKeys()+mdl::KeyPoolCapacity(app->ShadowKeys()));
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const auto accessory=app->AccessorySlot(0);const auto accessoryMesh=accessory->mesh;const auto accessoryScale=accessory->scale;
    const auto echo=[&]{
        plugins::Refresh(app);
        const auto range=int(std::lround(10000.0-double(app->ShadowDistance())*100000.0));
        char text[32]{};GetDlgItemTextA(app->MainWindow(),561,text,sizeof(text));
        if(std::atoi(text)!=range||SendDlgItemMessageW(app->MainWindow(),560,TBM_GETPOS,0,0)!=range)return false;
        for(int i=0;i<3;++i)if(SendDlgItemMessageW(app->MainWindow(),562+i,BM_GETCHECK,0,0)!=(i==app->SelfShadowMode()?BST_CHECKED:BST_UNCHECKED))return false;
        return true;
    };
    for(int field=0;field<4;++field) {
        const float before=.07512345f;const int beforeMode=field?field%3:1;
        app->ShadowDistance()=before;app->SelfShadowMode()=beforeMode;
        const auto edit=GetDlgItem(app->MainWindow(),561);Check(edit!=nullptr,"real self-shadow range edit exists");
        const auto commitText=[&](const char* text){SetWindowTextA(edit,text);SendMessageA(edit,EM_SETMODIFY,TRUE,0);
            SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);};
        const auto commit=[&]{if(!field)commitText("4321.875");else SendMessageW(app->MainWindow(),WM_COMMAND,561+field,0);};
        const float target=field?before:float((10000.0-4321)/100000.0);const int targetMode=field?field-1:beforeMode;
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;
        if(field)SendDlgItemMessageW(app->MainWindow(),561+field,BM_SETCHECK,BST_CHECKED,0);
        commit();
        Check(app->ShadowDistance()==before&&app->SelfShadowMode()==beforeMode&&!history.UndoCount()&&!app->SceneModified()&&echo(),
            "pending real plugin transaction rejects self-shadow text or mode and restores radio state");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit();
        Check(value()==1&&history.UndoCount()==2&&app->ShadowDistance()==target&&app->SelfShadowMode()==targetMode&&echo(),
            "actual self-shadow parameter joins common plugin history and echoes distance slider and radio state");
        if(!field) {
            SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
            Check(history.UndoCount()==2&&app->ShadowDistance()==target,"real shadow focus and Enter do not duplicate rounded readout");
        }
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");
        app->SceneModified()=app->RecordedFrameCountByte1()=0;commit();
        Check(history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"same shadow parameter preserves later plugin redo and dirty state");
        if(!field)for(const char* invalid:{"NaN","3 junk","inf"}) {
            commitText(invalid);Check(app->ShadowDistance()==target&&history.RedoCount()==1&&!app->SceneModified(),"real invalid shadow text preserves authoring and redo");
        }
        Menu(app,L"Edit probe redo");
        const auto saved=std::filesystem::absolute(std::wstring(L"shadow-parameter-")+std::to_wstring(field)+L".pmm");
        Check(SaveSceneCopy(app,saved.c_str())&&history.UndoCount()==3,"PMM saves shadow parameter and plugin data without discarding common history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==1&&app->ShadowDistance()==before&&app->SelfShadowMode()==beforeMode&&echo(),"plugin undo restores exact shadow parameter and deferred controls");
        Menu(app,L"Edit probe undo");app->CameraMode()=!originalEditor;app->CurrentFrame()=frame+31;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(value()==2&&app->ShadowDistance()==target&&app->SelfShadowMode()==targetMode&&echo()&&app->CurrentFrame()==frame+31&&
            model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&app->AccessorySlot(0)==accessory&&accessory->mesh==accessoryMesh&&accessory->scale==accessoryScale&&
            !std::memcmp(app->ShadowKeys(),keys.data(),keys.size()*sizeof(mdl::SelfShadowKey)),
            "shadow common redo works across modes and frames while preserving keys model and accessory");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==0&&app->ShadowDistance()==before&&app->SelfShadowMode()==beforeMode&&history.Reset()==MMS_OK,"shadow parameter probe restores data and clears isolated history");
        app->CameraMode()=originalEditor;app->CurrentFrame()=frame;
    }
    app->ShadowDistance()=originalDistance;app->SelfShadowMode()=originalMode;plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual self-shadow text and three modes, plugin interleave, deferred readout and PMM save");
}
