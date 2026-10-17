void CameraProjectionMode(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    const auto originalFov=app->CameraFov();const auto originalDistance=app->CameraDistance();const auto originalPerspective=app->CameraPerspective();
    const auto editorMode=app->CameraMode();const auto frame=app->CurrentFrame();
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::CameraKey> keys(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
    const auto button=GetDlgItem(app->MainWindow(),446);Check(button!=nullptr,"real camera perspective checkbox exists");
    const auto commit=[&]{SendMessageW(app->MainWindow(),WM_COMMAND,446,0);};
    const auto matrix=[&]{D3DMATRIX actual{};Check(SUCCEEDED(app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&actual)),"read device projection for actual perspective toggle");return actual;};
    const auto projection=[&]{plugins::Refresh(app);float expected[16]{};float aspect=app->Renderer()->aspectRatio;if(!(aspect>0))aspect=4.f/3.f;
        if(app->CameraPerspective()){expected[0]=-2.f/app->CameraDistance();expected[5]=aspect*expected[0];expected[10]=.0013f;expected[15]=1;}
        else Check(BuildCameraPerspectiveProjection(app,aspect,expected),"build current frame perspective expectation");
        const auto actual=matrix();const auto values=reinterpret_cast<const float*>(&actual);
        for(unsigned i=0;i<16;++i)if(!std::isfinite(values[i])||std::fabs(values[i]-expected[i])>1e-5f*(1+std::fabs(expected[i])))return false;
        return !app->cameraProjectionRefreshPending&&SendMessageW(button,BM_GETCHECK,0,0)==(app->CameraPerspective()?BST_UNCHECKED:BST_CHECKED);};
    for(std::uint8_t initial:{std::uint8_t(0),std::uint8_t(1)}) {
        app->CameraMode()=1;app->CameraFov()=45.125f;app->CameraDistance()=-45;app->CameraPerspective()=initial;ApplyFrameCameraProjection(app);
        const auto before=matrix();Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;
        SendMessageW(button,BM_SETCHECK,initial?BST_CHECKED:BST_UNCHECKED,0);commit();const auto after=matrix();
        Check(app->CameraPerspective()==initial&&!history.UndoCount()&&!app->SceneModified()&&!std::memcmp(&before,&after,sizeof(before))&&projection(),
            "pending real plugin transaction blocks perspective and restores checkbox without changing device matrix");
        Menu(app,L"Edit probe cancel");Menu(app,L"Edit probe once");commit();const auto target=initial?0:1;
        Check(app->CameraPerspective()==target&&app->CameraFov()==45.125f&&app->CameraDistance()==-45&&value()==1&&history.UndoCount()==2&&projection(),
            "actual perspective toggle joins plugin journal and installs correct current-state device matrix");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundCameraProjection(*app,target!=0,changed)==MMS_OK&&!changed&&history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),
            "same projection request preserves later plugin redo and dirty flags");
        Menu(app,L"Edit probe redo");
        Check(SaveSceneCopy(app,std::filesystem::absolute(initial?L"projection-perspective.pmm":L"projection-ortho.pmm").c_str())&&history.UndoCount()==3,
            "PMM saves projection kind and plugin state while retaining history");
        // Change unrelated authoring to ensure the mode record never replays an old matrix.
        app->CameraFov()=67.125f;app->CameraDistance()=-88.75f;
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==1&&app->CameraPerspective()==initial&&app->CameraFov()==67.125f&&app->CameraDistance()==-88.75f&&projection(),
            "plugin projection undo preserves later FOV and distance and rebuilds their current device matrix");
        Menu(app,L"Edit probe undo");app->CameraMode()=0;app->CurrentFrame()=frame+43;
        Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
        Check(value()==2&&app->CameraPerspective()==target&&projection()&&app->CurrentFrame()==frame+43&&app->CameraFov()==67.125f&&app->CameraDistance()==-88.75f&&
            model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&!std::memcmp(app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey)),
            "projection shared redo across editor modes and playheads retains unrelated authoring and camera keys");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==0&&app->CameraPerspective()==initial&&history.Reset()==MMS_OK,"projection probe restores plugin state and clears isolated case");
    }
    app->CameraMode()=1;app->CameraFov()=45.125f;app->CameraDistance()=-45;app->CameraPerspective()=0;ApplyFrameCameraProjection(app);
    for(float distance:{0.f,(std::numeric_limits<float>::denorm_min)()}) {
        const auto before=matrix();app->CameraDistance()=distance;commit();const auto after=matrix();
        Check(app->CameraPerspective()==1&&!app->cameraProjectionRefreshPending&&!std::memcmp(&before,&after,sizeof(before)),
            "orthographic toggle at zero or tiny distance keeps finite previous GPU matrix while recording mode");
        Menu(app,L"Edit probe undo");Check(app->CameraPerspective()==0&&projection(),"undo unsafe orthographic scale restores current perspective matrix");
        Check(history.Reset()==MMS_OK,"clear isolated guarded projection history");
    }
    app->CameraFov()=originalFov;app->CameraDistance()=originalDistance;app->CameraPerspective()=originalPerspective;
    app->CameraMode()=editorMode;app->CurrentFrame()=frame;app->cameraProjectionRefreshPending=true;plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual perspective toggle, plugin interleave, current-state GPU matrices, orthographic guards and PMM save");
}
