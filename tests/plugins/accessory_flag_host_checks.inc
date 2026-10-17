void AccessoryFlagParameters(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const int ids[]={476,477,486};auto* object=app->AccessorySlot(0);Check(object!=nullptr,"real accessory for flags exists");
    const auto flags=[&]{return std::array<std::uint8_t,3>{object->visible,object->additiveBlend,object->shadowEnabled};};
    const auto original=flags();const auto frame=app->CurrentFrame();const auto mode=app->CameraMode();const auto selected=app->SelectedAccessorySlot();
    const auto pose=std::array<float,8>{object->position[0],object->position[1],object->position[2],object->rotation[0],object->rotation[1],object->rotation[2],object->scale,object->opacity};
    const std::vector<mdl::AccessoryKey> keys(app->AccessoryKeys(0),app->AccessoryKeys(0)+mdl::KeyPoolCapacity(app->AccessoryKeys(0)));
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    app->SelectedAccessorySlot()=0;
    const auto echo=[&]{plugins::Refresh(app);const auto source=flags();
        for(unsigned i=0;i<3;++i)if(SendDlgItemMessageW(app->MainWindow(),ids[i],BM_GETCHECK,0,0)!=(source[i]?BST_CHECKED:BST_UNCHECKED))return false;return true;};
    for(unsigned field=0;field<3;++field) {
        const auto before=flags();auto target=before;target[field]=before[field]?0:1;
        const auto commit=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,ids[field],0);};
        object->rowSelected=0;app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;
        SendDlgItemMessageW(app->MainWindow(),ids[field],BM_SETCHECK,before[field]?BST_UNCHECKED:BST_CHECKED,0);commit();
        Check(flags()==before&&!history.UndoCount()&&!app->SceneModified()&&echo()&&!object->rowSelected&&app->GlobalTrackSelected(GlobalTimelineTrack::Light),
            "pending real plugin edit rejects accessory toggle and restores checkbox without changing selection");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit();
        Check(flags()==target&&history.UndoCount()==2&&value()==1&&echo(),"actual accessory flag joins plugin history and echoes checkbox");
        Check(field==1?(!object->rowSelected&&app->GlobalTrackSelected(GlobalTimelineTrack::Light)):
            (object->rowSelected&&!app->GlobalTrackSelected(GlobalTimelineTrack::Light)),"real additive checkbox preserves selection while display and shadow activate accessory");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");
        app->SceneModified()=app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundAccessoryFlag(*app,0,field,target[field]!=0,changed)==MMS_OK&&!changed&&history.RedoCount()==1&&
            !app->SceneModified()&&!app->RecordedFrameCountByte1(),"internal same-value flag set preserves later real plugin redo");
        Menu(app,L"Edit probe redo");
        const auto saved=std::filesystem::absolute(std::wstring(L"accessory-flag-")+std::to_wstring(field)+L".pmm");
        Check(SaveSceneCopy(app,saved.c_str())&&history.UndoCount()==3,"PMM saves accessory flag and plugin state without discarding history");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(flags()==before&&value()==1&&echo(),"plugin undo restores accessory flag and deferred checkbox");
        Menu(app,L"Edit probe undo");app->CameraMode()=!mode;app->CurrentFrame()=frame+41;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(flags()==target&&value()==2&&echo()&&app->CurrentFrame()==frame+41&&model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&
            !std::memcmp(app->AccessoryKeys(0),keys.data(),keys.size()*sizeof(mdl::AccessoryKey)),"flag redo works across modes and frames and preserves keys and model pose");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(flags()==before&&value()==0&&history.Reset()==MMS_OK,"accessory flag probe restores and clears isolated case");
        app->CameraMode()=mode;app->CurrentFrame()=frame;
    }
    const auto identity=plugins::SceneObjectIdentity(object);const auto pool=app->AccessoryKeys(0);const auto mesh=object->mesh;
    for(int id:ids)SendMessageW(app->MainWindow(),WM_COMMAND,id,0);
    const auto target=flags();Check(history.UndoCount()==3,"three flag commands precede controlled accessory deletion");
    SendDlgItemMessageW(app->MainWindow(),471,CB_SETCURSEL,0,0);
    {DeleteConfirmation confirmation(app,false);SendMessageW(app->MainWindow(),WM_COMMAND,473,0);
        Check(confirmation.allowed==1&&confirmation.seen==1,"flag lifetime scenario permits actual accessory deletion confirmation");}
    Check(!app->AccessorySlot(0)&&history.UndoCount()==4,"controlled delete follows accessory flag history");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);const auto restored=plugins::SceneObjectIdentity(object);
    Check(app->AccessorySlot(0)==object&&app->AccessoryKeys(0)==pool&&object->mesh==mesh&&flags()==target&&
        plugins::Same(identity.id,restored.id)&&identity.generation!=restored.generation,"controlled restore keeps flag values resources and stable ID with new generation");
    for(int i=0;i<3;++i)Menu(app,L"Edit probe undo");
    Check(flags()==original&&history.RedoCount()==4,"all older flag records undo after object restoration");
    for(int i=0;i<4;++i)Menu(app,L"Edit probe redo");FlushObjectUi(app);
    Check(!app->AccessorySlot(0)&&history.UndoCount()==4,"flag redo and later delete retain chronological order");
    Menu(app,L"Edit probe undo");FlushObjectUi(app);for(int i=0;i<3;++i)Menu(app,L"Edit probe undo");
    Check(flags()==original&&history.Reset()==MMS_OK,"restore accessory and clear flag deletion history");
    const auto currentPose=std::array<float,8>{object->position[0],object->position[1],object->position[2],object->rotation[0],object->rotation[1],object->rotation[2],object->scale,object->opacity};
    Check(currentPose==pose,"all real accessory flag operations preserve numeric authoring");
    app->SelectedAccessorySlot()=selected;app->CameraMode()=mode;app->CurrentFrame()=frame;plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual accessory flags, selection differences, plugin interleave, retained deletion and PMM save");
}
