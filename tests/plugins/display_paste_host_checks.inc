namespace display_paste_host_checks {
struct Snapshot {
    std::vector<mdl::DisplayKey> keys;
    std::vector<std::vector<unsigned char>> ik;
    std::vector<std::vector<mdl::BoneReference>> parents;
    explicit Snapshot(const mdl::ModelRecord& model) {
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(model.displayKeys);++i) {
            keys.push_back(model.displayKeys[i]);keys.back().ikStates=nullptr;keys.back().selectorStates=nullptr;
            ik.emplace_back();parents.emplace_back();
            if(model.ikChainCount)ik.back().assign(model.displayKeys[i].ikStates,model.displayKeys[i].ikStates+model.ikChainCount);
            if(model.boneOrderCount)parents.back().assign(model.displayKeys[i].selectorStates,model.displayKeys[i].selectorStates+model.boneOrderCount);
        }
    }
    bool Matches(const mdl::ModelRecord& model)const {
        if(mdl::KeyPoolCapacity(model.displayKeys)<keys.size())return false;
        for(std::size_t i=0;i<keys.size();++i) {
            const auto& key=model.displayKeys[i];const auto& saved=keys[i];
            if(key.frame!=saved.frame||key.previous!=saved.previous||key.next!=saved.next||
                key.visible!=saved.visible||key.allocated!=saved.allocated||
                std::memcmp(key.reserved0,saved.reserved0,sizeof(key.reserved0))||
                std::memcmp(key.reserved1,saved.reserved1,sizeof(key.reserved1))||
                (!ik[i].empty()&&std::memcmp(ik[i].data(),model.displayKeys[i].ikStates,ik[i].size()))||
                (!parents[i].empty()&&std::memcmp(parents[i].data(),model.displayKeys[i].selectorStates,parents[i].size()*sizeof(mdl::BoneReference))))return false;
        }
        return true;
    }
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    auto& model=*mdl::Mdl(app->ModelSlot(0));
    auto* oldClipboard=app->DisplayClipboard();const auto counts=app->ClipboardCounts();
    const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();const auto mode=app->CameraMode();const auto recording=history.Mode();
    const auto ring=model.undoState[0],redoRing=model.undoState[1];
    const Snapshot original(model);const auto dirty=model.bonePhysicsState[0];
    const auto pose=model.boneTable[0].trans[1];
    mdl::DisplayClipboardRecord clipboard{};clipboard.visible=0;
    app->DisplayClipboard()=&clipboard;app->ClipboardCounts()={};app->ClipboardCounts().displays=1;
    app->CurrentFrame()=347;app->CameraMode()=0;app->SelectedModelSlot()=0;
    const auto command=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,421,0);};
    for(auto recordingMode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(history.SetMode(recordingMode)==MMS_OK,"configure actual display paste history mode");
        Menu(app,L"Edit probe prepared");command();
        Check(!history.UndoCount()&&original.Matches(model),"pending plugin transaction blocks actual display paste");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");command();
        Check(history.UndoCount()==2&&value()==1,"actual display paste joins common history as one explicit node");
        const Snapshot pasted(model);Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==1&&original.Matches(model),"actual display paste undo restores nested payloads between plugin commits");
        clipboard.visible=1;clipboard.ikCount=1;clipboard.ikStates=nullptr;
        app->CurrentFrame()=frame+113;app->SelectedModelSlot()=1;app->CameraMode()=1;
        Menu(app,L"Edit probe redo");
        Check(pasted.Matches(model)&&app->CurrentFrame()==frame+113&&value()==1,
            "actual display paste redo never reads now-invalid clipboard or changes the playhead");
        Menu(app,L"Edit probe redo");
        Check(value()==2&&model.undoState[0]==ring&&model.undoState[1]==redoRing&&
            model.bonePhysicsState[0]==dirty&&model.boneTable[0].trans[1]==pose,
            "actual display paste preserves old history and dirty authoring across plugin redo");
        for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==0&&history.Reset()==MMS_OK&&original.Matches(model),"actual display paste restores isolated common journal");
        clipboard.visible=0;clipboard.ikCount=0;app->CurrentFrame()=347;app->SelectedModelSlot()=0;app->CameraMode()=0;
    }
    Check(history.SetMode(recording)==MMS_OK,"restore actual display paste history mode");
    app->DisplayClipboard()=oldClipboard;app->ClipboardCounts()=counts;app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;app->CameraMode()=mode;
    std::puts("PASS actual display-only paste, history modes, deep payload and plugin interleaving");
}
}
