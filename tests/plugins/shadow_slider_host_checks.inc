void ShadowRangeSlider(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    const float original=app->ShadowDistance();const auto originalMode=app->SelfShadowMode();const auto editor=app->CameraMode();const auto frame=app->CurrentFrame();
    app->ShadowDistance()=.07512345f;app->SelfShadowMode()=1;const auto before=app->ShadowDistance();const float target=float((10000.0-4321)/100000.0),second=float((10000.0-8765)/100000.0);
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::SelfShadowKey> keys(app->ShadowKeys(),app->ShadowKeys()+mdl::KeyPoolCapacity(app->ShadowKeys()));
    const auto slider=GetDlgItem(app->MainWindow(),panel::kSelfShadowRangeSlider);Check(slider!=nullptr,"actual self-shadow range slider exists");
    const auto scroll=[&](int tick,unsigned code=TB_THUMBTRACK){SendMessageW(slider,TBM_SETPOS,TRUE,tick);SendMessageW(app->MainWindow(),WM_HSCROLL,MAKEWPARAM(code,0),reinterpret_cast<LPARAM>(slider));};
    const auto echo=[&](int tick){char text[32]{};GetDlgItemTextA(app->MainWindow(),561,text,sizeof text);return std::atoi(text)==tick&&SendMessageW(slider,TBM_GETPOS,0,0)==tick;};
    Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(4321);
    Check(app->ShadowDistance()==before&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1()&&echo(ShadowSliderPosition(app)),"pending real plugin transaction rejects self-shadow live slider and restores thumb");Menu(app,L"Edit probe cancel");
    Menu(app,L"Edit probe once");SetCapture(slider);Check(GetCapture()==slider,"actual self-shadow trackbar owns test capture");scroll(4321);ReleaseCapture();
    Check(value()==1&&history.UndoCount()==2&&app->ShadowDistance()==target&&echo(4321)&&app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow),"actual captured range slider joins plugin common history and activates shadow row");
    scroll(8765);Check(history.UndoCount()==3&&app->ShadowDistance()==second&&echo(8765),"next live shadow position is a separate undo step");
    app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(8765,TB_ENDTRACK);
    Check(history.UndoCount()==3&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"actual shadow end-track notification adds no duplicate step");
    Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(8765,TB_THUMBPOSITION);
    Check(history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"same actual shadow slider position retains later plugin redo");Menu(app,L"Edit probe redo");
    Check(SaveSceneCopy(app,std::filesystem::absolute(L"shadow-slider.pmm").c_str()),"PMM saves slider distance and committed plugin state");
    Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(app->ShadowDistance()==target&&value()==1,"plugin undo reverses the last live shadow step");
    Menu(app,L"Edit probe undo");Check(app->ShadowDistance()==before&&value()==1,"second slider undo restores original distance bit pattern");Menu(app,L"Edit probe undo");
    app->SelfShadowMode()=2;app->CameraMode()=!editor;app->CurrentFrame()=frame+67;
    Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
    Check(app->ShadowDistance()==second&&app->SelfShadowMode()==2&&value()==2&&echo(8765)&&app->CurrentFrame()==frame+67&&model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&
        !std::memcmp(app->ShadowKeys(),keys.data(),keys.size()*sizeof(mdl::SelfShadowKey)),"actual range slider redo preserves later mode playhead keys model pose and old ring across editor modes");
    for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");Check(value()==0&&app->ShadowDistance()==before&&history.Reset()==MMS_OK,"restore isolated shadow slider history");
    app->ShadowDistance()=original;app->SelfShadowMode()=originalMode;app->CameraMode()=editor;app->CurrentFrame()=frame;plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual self-shadow slider, own capture, granular live history, plugin interleave and PMM save");
}
