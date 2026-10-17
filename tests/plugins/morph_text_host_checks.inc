void MorphTextPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    auto* raw=app->ModelSlot(0);auto& model=*mdl::Mdl(raw);Check(model.morphs&&model.morphCount,"real model has morph data for text edit");
    const unsigned index=model.morphCount>1?1:0;
    std::vector<float> original;for(unsigned i=0;i<model.morphCount;++i)original.push_back(model.morphs[i].value);
    std::array<int,4> selectors{};std::memcpy(selectors.data(),model.selectedMorphs,sizeof(selectors));
    const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();
    const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::MorphKey> keys(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys));
    app->CameraMode()=0;app->SelectedModelSlot()=0;
    for(unsigned lane=0;lane<4;++lane){model.selectedMorphs[lane]=int(index);model.morphs[index].value=.12345678f;
        const auto edit=GetDlgItem(app->MainWindow(),506+int(lane)*5);Check(edit!=nullptr,"real morph text lane exists");
        const auto echo=[&]{plugins::Refresh(app);char actual[64]{},expected[64]{};
            GetWindowTextA(edit,actual,sizeof(actual));std::snprintf(expected,sizeof(expected),"%5.4f",double(model.morphs[index].value));
            return !std::strcmp(actual,expected)&&SendDlgItemMessageW(app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==int(double(model.morphs[index].value)*100.0);};
        const auto commit=[&](const char* text){SetWindowTextA(edit,text);SendMessageA(edit,EM_SETMODIFY,TRUE,0);
            SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);};
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;commit(".6543219");
        Check(model.morphs[index].value==.12345678f&&!history.UndoCount()&&!app->SceneModified(),"pending real plugin transaction blocks morph text without legacy weight write");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit(".6543219");
        Check(model.morphs[index].value==.654321f&&value()==1&&history.UndoCount()==2&&
            SendDlgItemMessageW(app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==65,"actual morph text joins plugin common history and synchronizes paired slider");
        SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
        Check(model.morphs[index].value==.654321f&&history.UndoCount()==2,"real morph focus and Enter preserve precise unregistered weight without duplicate history");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");
        for(const char* input:{".6543219","NaN","3 junk","1e100"}){app->SceneModified()=app->RecordedFrameCountByte1()=0;commit(input);
            Check(model.morphs[index].value==.654321f&&history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"real morph same-value invalid and overflow text retain later plugin redo");}
        Menu(app,L"Edit probe redo");const auto saved=std::filesystem::absolute(std::wstring(L"morph-text-")+std::to_wstring(lane)+L".pmm");
        Check(SaveSceneCopy(app,saved.c_str())&&history.UndoCount()==3,"PMM saves current morph weight and plugin edits without clearing history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(model.morphs[index].value==.12345678f&&value()==1&&echo(),"real plugin undo restores exact original morph float and deferred text and slider");
        Menu(app,L"Edit probe undo");app->CameraMode()=1;app->CurrentFrame()=frame+47;model.selectedMorphs[lane]=int((index+1)%model.morphCount);
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(model.morphs[index].value==.654321f&&value()==2&&app->CurrentFrame()==frame+47&&model.boneTable[0].trans[1]==bone&&model.undoState[0]==ring&&
            !std::memcmp(model.morphKeys,keys.data(),keys.size()*sizeof(mdl::MorphKey)),"morph redo resolves original channel after selector and mode change without seeking or rewriting keys");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(model.morphs[index].value==.12345678f&&value()==0&&history.Reset()==MMS_OK,"real morph case restores weight and clears isolated history");
        app->CameraMode()=0;app->CurrentFrame()=frame;
    }
    model.selectedMorphs[0]=int(index);
    {const auto edit=GetDlgItem(app->MainWindow(),506);SetWindowTextA(edit,"0.53");SendMessageA(edit,EM_SETMODIFY,TRUE,0);
        SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
        Check(model.morphs[index].value==.53f&&SendDlgItemMessageW(app->MainWindow(),505,TBM_GETPOS,0,0)==53,
            "actual initial morph text slider retains double conversion at rounding boundary");
        app->SceneModified()=0;SetWindowTextA(edit,"0.53");SendMessageA(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
        Check(history.UndoCount()==1&&!app->SceneModified()&&SendDlgItemMessageW(app->MainWindow(),505,TBM_GETPOS,0,0)==53,"actual same-weight morph text preserves initial double slider echo without another edit");
        Menu(app,L"Edit probe undo");Check(history.Reset()==MMS_OK,"restore morph slider rounding boundary scenario");}
    for(unsigned i=0;i<model.morphCount;++i)model.morphs[i].value=original[i];
    model.selectedMorphs[0]=int(index);const auto identity=plugins::SceneObjectIdentity(raw);const auto pool=model.morphKeys;const auto vertices=model.vertexBuffer;
    const auto edit=GetDlgItem(app->MainWindow(),506);SetWindowTextA(edit,".6543219");SendMessageA(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
    Check(history.UndoCount()==1&&model.morphs[index].value==.654321f,"seed morph weight before real model deletion");
    SendDlgItemMessageW(app->MainWindow(),436,CB_SETCURSEL,model.comboSelIndex,0);
    {DeleteConfirmation confirmation(app,true);SendMessageW(app->MainWindow(),WM_COMMAND,437,0);Check(confirmation.allowed==1&&confirmation.seen==1,"morph scalar lifetime test permits actual model deletion confirmation");}
    Check(!app->ModelSlot(0)&&history.UndoCount()==2,"real model deletion follows morph text history");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);const auto restored=plugins::SceneObjectIdentity(raw);
    Check(app->ModelSlot(0)==raw&&model.morphKeys==pool&&model.vertexBuffer==vertices&&plugins::Same(identity.id,restored.id)&&identity.generation!=restored.generation,
        "controlled model restore retains resources and stable identity while renewing generation");
    Menu(app,L"Edit probe undo");Check(model.morphs[index].value==original[index]&&history.RedoCount()==2,"morph scalar undo resolves model restored with new generation");
    Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");FlushObjectUi(app);Check(!app->ModelSlot(0),"morph scalar redo followed by deletion remains ordered");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);Menu(app,L"Edit probe undo");Check(model.morphs[index].value==original[index]&&history.Reset()==MMS_OK,"restore model and original morph weight and clear lifetime scenario");
    std::memcpy(model.selectedMorphs,selectors.data(),sizeof(selectors));app->CameraMode()=mode;app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;
    plugins::RefreshBoundMorphReadout(*app);
    std::puts("PASS actual four morph text lanes, plugin interleave, exact weight, retained model deletion and PMM save");
}
