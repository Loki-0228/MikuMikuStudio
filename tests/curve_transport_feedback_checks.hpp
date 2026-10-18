void CheckCurveTransportFeedback(MMDApp*app,const std::filesystem::path& output){
    const HWND main=app->MainWindow();
    app->state.optflag[0]=1;app->CameraParentModel()=-1;
    for(int frame:{0,100}){
        app->CameraPositionX()=float(frame+3);app->CameraPositionY()=12;app->CameraPositionZ()=-5;
        app->CameraPitch()=.1f;app->CameraYaw()=.2f;app->CameraRoll()=0;app->CameraDistance()=-45;
        RegisterCameraState(app,frame);
    }
    for(unsigned language=0;language<3;++language){
        app->EnglishUI()=static_cast<unsigned char>(language);app->CurrentFrame()=0;RefreshAfterFrameApply(app);
        curve_host::Open(app,false);
        HWND editor=nullptr;
        std::pair<HWND,HWND*> ownerAndEditor{main,&editor};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM data)->BOOL{
            auto*pair=reinterpret_cast<std::pair<HWND,HWND*>*>(data);
            if(curve_editor::OwnsWindow(pair->first,window)){*pair->second=window;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&ownerAndEditor));
        Check(editor!=nullptr,"find hidden production curve editor");
        const HWND canvas=GetDlgItem(editor,1020);
        SetupFrameWorldTransform(app);D3DMATRIX previousWorld{};
        app->Renderer()->device->GetTransform(D3DTS_WORLD,&previousWorld);
        for(int wanted:{12,36,80}){
            const auto p=SendMessageW(editor,WM_APP+301,wanted,0);
            ValidateRect(main,nullptr);
            SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(LOWORD(p),12));
            SendMessageW(canvas,WM_LBUTTONUP,0,MAKELPARAM(LOWORD(p),12));
            const int frame=app->CurrentFrame();
            Check(std::abs(frame-wanted)<=1,"curve ruler seeks the real app frame");
            Check(std::fabs(app->CameraPositionX()-(frame+3))<.05,"real camera responds to curve ruler seek");
            wchar_t text[32]{};GetDlgItemTextW(main,panel::kCurrentFrameEdit,text,32);
            Check(_wtoi(text)==frame,"native current-frame field follows curve ruler seek");
            Check(app->state.timelineStartFrame<=frame&&frame-app->state.timelineStartFrame<=(app->SidebarWidth()-84)/26,
                "native timeline scrolls to include the selected frame");
            SetupFrameWorldTransform(app);
            D3DMATRIX view{};app->Renderer()->device->GetTransform(D3DTS_VIEW,&view);
            Check(std::isfinite(view._41)&&std::isfinite(view._42)&&std::isfinite(view._43),"rendered camera transform is finite after scrubbing");
            D3DMATRIX world{};app->Renderer()->device->GetTransform(D3DTS_WORLD,&world);
            Check(std::fabs(world._41-previousWorld._41)+std::fabs(world._42-previousWorld._42)+std::fabs(world._43-previousWorld._43)>.01,
                "viewport camera/world transform changes with the curve ruler");
            previousWorld=world;
        }
        const int frame=app->CurrentFrame();
        SendMessageW(canvas,WM_KEYDOWN,VK_INSERT,0);
        bool found=false;for(unsigned i=0,n=0;n<mdl::KeyPoolCapacity(app->CameraKeys());++n){
            found=found||int(app->CameraKeys()[i].frame)==frame;
            if(!app->CameraKeys()[i].next)break;i=app->CameraKeys()[i].next;
        }
        Check(found,"Insert shortcut registers the native camera key");
        curve_editor::CloseOwned(main);
    }
    std::filesystem::create_directories(output);
    std::vector<curve_editor::Channel> before,after;std::wstring error;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,before,error),"read camera before full PMM save");
    const auto saved=output/L"curve-transport.pmm";
    Check(SaveSceneCopy(app,saved.c_str()),"save real PMM with inserted camera authoring");
    wcscpy_s(app->EnvFileName(),256,saved.c_str());LoadSceneFile();
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,after,error),"read inserted camera from real PMM");
    for(double frame:{.25,12.25,36.5,79.75,90.5})
        Check(std::fabs(before[0].sample(before[0],frame).value-after[0].sample(after[0],frame).value)<1e-10,
            "complete PMM save/load keeps subdivided camera timing");
    std::puts("PASS curve ruler seeks real camera, native frame field, timeline and viewport in three languages");
}
