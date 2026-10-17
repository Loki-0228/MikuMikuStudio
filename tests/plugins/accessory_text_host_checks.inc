void AccessoryTextPose(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    auto* object=app->AccessorySlot(0);Check(object!=nullptr,"real accessory for numeric history exists");
    const auto pose=[&]{return std::array<float,8>{object->position[0],object->position[1],object->position[2],
        object->rotation[0],object->rotation[1],object->rotation[2],object->scale,object->opacity};};
    const auto original=pose();const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();const auto selected=app->SelectedAccessorySlot();
    app->SelectedAccessorySlot()=0;
    const std::vector<mdl::AccessoryKey> keys(app->AccessoryKeys(0),app->AccessoryKeys(0)+mdl::KeyPoolCapacity(app->AccessoryKeys(0)));
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    for(unsigned field=0;field<8;++field) {
        const auto source=pose();auto target=source;
        target[field]=field<3||field==6?12.3456f:field<6?float(12.3456/180.0*3.141592025756836):.75f;
        const auto edit=GetDlgItem(app->MainWindow(),478+int(field));Check(edit!=nullptr,"real accessory numeric control exists");
        const auto text=field==7?".75":"12.345678";
        const auto commit=[&](const char* input){SetWindowTextA(edit,input);SendMessageA(edit,EM_SETMODIFY,TRUE,0);
            SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);};
        Menu(app,L"Edit probe prepared");app->SceneModified()=0;commit(text);
        Check(pose()==source&&!history.UndoCount()&&!app->SceneModified(),"pending plugin edit blocks actual accessory text without legacy write");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit(text);
        Check(pose()==target&&value()==1&&history.UndoCount()==2,"actual accessory text joins plugin common history");
        SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
        Check(pose()==target&&history.UndoCount()==2,"real accessory Enter and focus echo preserve precise fields without extra history");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");
        for(const char* input:{text,"NaN","3 junk"}) {
            app->SceneModified()=app->RecordedFrameCountByte1()=0;commit(input);
            Check(pose()==target&&history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),
                "real no-op or invalid accessory input preserves later plugin redo and dirty flags");
        }
        Menu(app,L"Edit probe redo");
        if(field==7)Check(SaveSceneCopy(app,std::filesystem::absolute(L"accessory-text.pmm").c_str())&&history.UndoCount()==3,
            "PMM saves accessory fields and plugin state without clearing common history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(pose()==source&&value()==1,"plugin undo restores exact prior accessory fields");
        Menu(app,L"Edit probe undo");app->CameraMode()=1;app->CurrentFrame()=frame+23;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(pose()==target&&value()==2&&app->CurrentFrame()==frame+23&&model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&
            !std::memcmp(app->AccessoryKeys(0),keys.data(),keys.size()*sizeof(mdl::AccessoryKey)),
            "accessory shared redo preserves keys model pose old undo ring and playhead in another mode");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(pose()==source&&value()==0&&history.Reset()==MMS_OK,"accessory text probe restores and clears isolated case");
        app->CurrentFrame()=frame;app->CameraMode()=mode;
    }
    const auto identity=plugins::SceneObjectIdentity(object);const auto pool=app->AccessoryKeys(0);const auto mesh=object->mesh;
    const auto edit=GetDlgItem(app->MainWindow(),478);
    SetWindowTextA(edit,"12.345678");SendMessageA(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);
    Check(history.UndoCount()==1&&object->position[0]==12.3456f,"seed accessory text before controlled deletion");
    SendDlgItemMessageW(app->MainWindow(),471,CB_SETCURSEL,0,0);
    {DeleteConfirmation confirmation(app,false);SendMessageW(app->MainWindow(),WM_COMMAND,473,0);
        Check(confirmation.allowed==1&&confirmation.seen==1,"accessory scalar lifetime test confirms actual deletion");}
    Check(!app->AccessorySlot(0)&&history.UndoCount()==2,"controlled delete follows accessory scalar history");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);
    const auto restored=plugins::SceneObjectIdentity(object);
    Check(app->AccessorySlot(0)==object&&app->AccessoryKeys(0)==pool&&object->mesh==mesh&&
        plugins::Same(identity.id,restored.id)&&identity.generation!=restored.generation,"controlled restore retains resources and stable ID with renewed generation");
    Menu(app,L"Edit probe undo");
    Check(pose()==original&&history.RedoCount()==2,"older scalar undo resolves restored accessory despite new object generation");
    Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");FlushObjectUi(app);
    Check(!app->AccessorySlot(0)&&history.UndoCount()==2,"scalar redo followed by delete redo preserves ordering");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);Menu(app,L"Edit probe undo");
    Check(pose()==original&&history.Reset()==MMS_OK,"restore accessory and clear scalar/delete lifetime history");
    app->SelectedAccessorySlot()=selected;app->CameraMode()=mode;app->CurrentFrame()=frame;
    plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual accessory text, plugin interleave, controlled deletion restoration and stable identity");
}
