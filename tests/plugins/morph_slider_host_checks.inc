namespace morph_slider_host_checks {
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    auto& model=*mdl::Mdl(app->ModelSlot(0));Check(model.morphs&&model.morphCount,"actual model has morph slider targets");
    const unsigned index=model.morphCount>1?1:0;const auto physics=model.physicsMode;
    std::vector<float> original;for(unsigned i=0;i<model.morphCount;++i)original.push_back(model.morphs[i].value);
    std::array<int,4> selectors{};std::memcpy(selectors.data(),model.selectedMorphs,sizeof(selectors));
    const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();
    const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::MorphKey> keys(model.morphKeys,model.morphKeys+mdl::MorphKeyCapacity(model.morphKeys));
    app->CameraMode()=0;app->SelectedModelSlot()=0;if(!index)model.physicsMode=2;
    for(unsigned lane=0;lane<4;++lane) {
        model.selectedMorphs[lane]=int(index);model.morphs[index].value=.12345678f;
        const auto slider=GetDlgItem(app->MainWindow(),505+int(lane)*5);Check(slider!=nullptr,"actual morph slider lane exists");
        slider_readout_probe::Spy sliderMessages,editMessages;
        Check(sliderMessages.Watch(slider)&&editMessages.Watch(GetDlgItem(app->MainWindow(),506+int(lane)*5)),"observe actual morph native control echoes");
        const auto scroll=[&](int tick,unsigned code=TB_THUMBTRACK){SendMessageW(slider,TBM_SETPOS,TRUE,tick);
            sliderMessages.Clear();editMessages.Clear(); // Exclude the simulated user thumb movement itself.
            SendMessageW(app->MainWindow(),WM_HSCROLL,MAKEWPARAM(code,0),reinterpret_cast<LPARAM>(slider));};
        plugins::RefreshBoundMorphReadout(*app);
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(53);
        Check(model.morphs[index].value==.12345678f&&!history.UndoCount()&&!app->SceneModified()&&
            SendMessageW(slider,TBM_GETPOS,0,0)==12,"pending plugin edit rejects actual morph slider and restores thumb");
        Check(sliderMessages.counts.positions==1&&sliderMessages.counts.redrawPositions==1,"actual pending morph rejection restores the moved native thumb exactly once");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");SetCapture(slider);Check(GetCapture()==slider,"actual morph slider owns capture");scroll(53);ReleaseCapture();
        Check(value()==1&&history.UndoCount()==2&&model.morphs[index].value==.53f&&SendMessageW(slider,TBM_GETPOS,0,0)==53,
            "actual captured morph slider joins shared history and preserves float-boundary tick");
        Check(!sliderMessages.counts.positions,"actual successful morph publication does not echo the already moved native thumb");
        sliderMessages.Clear();editMessages.Clear();
        plugins::RefreshBoundMorphReadout(*app);SendMessageW(app->MainWindow(),WM_HSCROLL,TB_ENDTRACK,reinterpret_cast<LPARAM>(slider));
        Check(history.UndoCount()==2&&model.morphs[index].value==.53f&&SendMessageW(slider,TBM_GETPOS,0,0)==53,
            "morph safe refresh and native end-track do not author a drifted tick");
        Check(sliderMessages.counts.NoEcho()&&editMessages.counts.NoEcho(),"actual unchanged morph refresh and ENDTRACK send no redundant position, text or selection messages");
        scroll(75);Check(history.UndoCount()==3&&model.morphs[index].value==.75f,"next actual morph position creates one additional live node");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=0;scroll(75,TB_ENDTRACK);
        Check(history.RedoCount()==1&&!app->SceneModified(),"same actual morph tick retains later plugin redo");Menu(app,L"Edit probe redo");
        if(lane==3)Check(SaveSceneCopy(app,std::filesystem::absolute(L"morph-slider.pmm").c_str()),"save actual morph slider weight and plugin state");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(model.morphs[index].value==.53f&&value()==1,"undo latest morph slider tick in plugin order");
        Menu(app,L"Edit probe undo");Check(model.morphs[index].value==.12345678f,"morph slider undo restores exact prior weight");Menu(app,L"Edit probe undo");
        app->CameraMode()=1;app->CurrentFrame()=frame+61;model.selectedMorphs[lane]=int((index+1)%model.morphCount);
        for(int i=0;i<4;++i)Menu(app,L"Edit probe redo");
        Check(value()==2&&model.morphs[index].value==.75f&&app->CurrentFrame()==frame+61&&model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&
            !std::memcmp(model.morphKeys,keys.data(),keys.size()*sizeof(mdl::MorphKey)),"morph slider redo resolves original target after selector mode and frame changes without rewriting keys");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(value()==0&&model.morphs[index].value==.12345678f&&history.Reset()==MMS_OK,"restore isolated actual morph slider history");
        app->CameraMode()=0;app->CurrentFrame()=frame;
    }
    for(unsigned i=0;i<model.morphCount;++i)model.morphs[i].value=original[i];
    std::memcpy(model.selectedMorphs,selectors.data(),sizeof(selectors));model.physicsMode=physics;
    app->CameraMode()=mode;app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;plugins::RefreshBoundMorphReadout(*app);
    std::puts("PASS actual four morph sliders, own capture, tick stability, plugin mixed history and PMM save");
}
}
