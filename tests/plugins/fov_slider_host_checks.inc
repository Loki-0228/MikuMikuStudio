namespace fov_slider_host_checks {
void Run(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto original=app->CameraFov();const auto originalMode=app->CameraPerspective();
    const auto originalDistance=app->CameraDistance();const auto editor=app->CameraMode();const auto frame=app->CurrentFrame();
    const auto slider=GetDlgItem(app->MainWindow(),panel::kFovSlider);Check(slider!=nullptr,"real FOV trackbar exists");
    const auto scroll=[&](int tick,unsigned code=TB_THUMBTRACK){
        SendMessageW(slider,TBM_SETPOS,TRUE,tick);
        SendMessageW(app->MainWindow(),WM_HSCROLL,MAKEWPARAM(code,0),reinterpret_cast<LPARAM>(slider));
    };
    const auto echo=[&](int tick){char text[32]{};GetDlgItemTextA(app->MainWindow(),panel::kFovEdit,text,sizeof(text));
        return std::atoi(text)==tick&&SendMessageW(slider,TBM_GETPOS,0,0)==tick;};
    const auto projection=[&]{
        plugins::Refresh(app);float expected[16]{};float aspect=app->Renderer()->aspectRatio;if(!(aspect>0))aspect=4.f/3.f;
        if(app->CameraPerspective()){expected[0]=-2.f/app->CameraDistance();expected[5]=aspect*expected[0];expected[10]=.0013f;expected[15]=1;}
        else Check(BuildCameraPerspectiveProjection(app,aspect,expected),"build expected FOV slider perspective matrix");
        D3DMATRIX actual{};Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&actual)),"read actual FOV slider device matrix");
        const auto* data=reinterpret_cast<const float*>(&actual);
        for(unsigned i=0;i<16;++i)if(!std::isfinite(data[i])||std::fabs(data[i]-expected[i])>1e-5f*(1+std::fabs(expected[i])))return false;
        return !app->cameraProjectionRefreshPending;
    };
    for(unsigned mode=0;mode<2;++mode){
        app->CameraFov()=45.125f;app->CameraPerspective()=mode;app->CameraDistance()=-45;
        plugins::RefreshBoundGlobalReadout(*app);
        const auto count=mdl::KeyPoolCapacity(app->CameraKeys());
        const std::vector<mdl::CameraKey> keys(app->CameraKeys(),app->CameraKeys()+count);
        auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(67);
        Check(app->CameraFov()==45.125f&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1()&&
            SendMessageW(slider,TBM_GETPOS,0,0)==45,"pending plugin transaction rejects FOV slider and restores moved thumb");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");
        SetCapture(slider);Check(GetCapture()==slider,"actual FOV slider owns live capture");scroll(67);ReleaseCapture();
        Check(value()==1&&history.UndoCount()==2&&app->CameraFov()==67&&echo(67)&&projection(),
            "actual captured FOV slider joins plugin history and updates the real device projection");
        scroll(91);Check(history.UndoCount()==3&&app->CameraFov()==91&&echo(91)&&projection(),"second live FOV position creates another common step");
        app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(91,TB_ENDTRACK);
        Check(history.UndoCount()==3&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"actual FOV end-track does not duplicate history");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=0;scroll(91,TB_THUMBPOSITION);
        Check(history.RedoCount()==1&&!app->SceneModified(),"same actual FOV position retains plugin redo");Menu(app,L"Edit probe redo");
        Check(SaveSceneCopy(app,std::filesystem::absolute(mode?L"fov-slider-ortho.pmm":L"fov-slider-perspective.pmm").c_str()),
            "PMM saves FOV slider author value and committed plugin state");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(app->CameraFov()==67&&value()==1&&projection(),"interleaved FOV undo updates real device projection");
        Menu(app,L"Edit probe undo");Check(app->CameraFov()==45.125f&&value()==1,"FOV undo restores exact original fractional value");Menu(app,L"Edit probe undo");
        app->CameraMode()=!editor;app->CurrentFrame()=frame+53;app->CameraPerspective()=!mode;
        Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
        Check(value()==2&&app->CameraFov()==91&&app->CameraPerspective()==!mode&&app->CurrentFrame()==frame+53&&echo(91)&&projection()&&
            model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&!std::memcmp(app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),
            "FOV redo preserves later projection kind playhead keys and model pose across editor modes");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(value()==0&&app->CameraFov()==45.125f&&history.Reset()==MMS_OK,"restore isolated FOV slider common history");
        app->CameraMode()=editor;app->CurrentFrame()=frame;
    }
    app->CameraFov()=original;app->CameraPerspective()=originalMode;app->CameraDistance()=originalDistance;
    app->CameraMode()=editor;app->CurrentFrame()=frame;app->cameraProjectionRefreshPending=false;
    ApplyFrameCameraProjection(app);plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual FOV slider, captured live common history, device projection, plugin interleave and PMM save");
}
}
