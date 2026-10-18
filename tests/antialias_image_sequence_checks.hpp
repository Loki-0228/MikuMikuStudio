// Count production scene submissions and encoded files separately. Extra
// temporal samples must not enter playback, physics, or the image encoder.
struct TemporalSequenceWrite {
    int frame;
    double time;
    unsigned renders,samples,recorded;
};
struct TemporalSequenceProbe {
    unsigned renders=0,lastWrittenRender=0,physics=0,catchups=0;
    bool insideRender=false,rejectMotion=false;
    std::vector<TemporalSequenceWrite> writes;
};
TemporalSequenceProbe* temporalSequenceProbe=nullptr;
decltype(&RenderFrameScene) temporalOriginalRender=nullptr;
decltype(&PhysicsFrame) temporalOriginalPhysics=nullptr;
decltype(&PlaybackCatchup) temporalOriginalCatchup=nullptr;
decltype(&seq::WriteFrame) temporalOriginalWrite=nullptr;
decltype(&antialias::motion::Resolve) temporalOriginalMotion=nullptr;
decltype(&runtime_log::Write) temporalOriginalLog=nullptr;
void ObserveTemporalLog(const char* format,...) noexcept {
    char message[2048]{};va_list args;va_start(args,format);
    std::vsnprintf(message,sizeof(message),format,args);va_end(args);
    if(std::strstr(message,"TAA motion"))std::printf("TRACE %s\n",message);
    temporalOriginalLog("%s",message);
}
void ObserveTemporalScene(MMDApp* app) {
    auto* probe=temporalSequenceProbe;
    if(!probe || !app->RecordingWindow()) {temporalOriginalRender(app);return;}
    ++probe->renders;probe->insideRender=true;
    const int frame=app->CurrentFrame(),recorded=app->state.recordedFrameCount;
    const double time=app->PlaybackCursorSeconds();
    const auto before=antialias::AccumulatedSamples(app);
    temporalOriginalRender(app);
    const auto stats=antialias::motion::GetStats(app);
    std::printf("TRACE temporal-render pass=%u frame=%d time=%.6f recorded=%d samples=%u->%u required=%u initializing=%d motion_draws=%u matched=%u captures=%u hits=%u\n",
        probe->renders,frame,time,recorded,before,antialias::AccumulatedSamples(app),antialias::RequiredSamples(app),
        antialias::Initializing(app),stats.draws,stats.matchedDraws,stats.vertexCaptures,stats.vertexCacheHits);
    Check(app->CurrentFrame()==frame && app->PlaybackCursorSeconds()==time &&
        app->state.recordedFrameCount==recorded,"temporal render samples keep time and output position unchanged");
    probe->insideRender=false;
}
void ObserveTemporalPhysics(MMDApp* app,unsigned char selected) {
    if(auto* probe=temporalSequenceProbe) {
        Check(!probe->insideRender,"temporal sub-samples do not step physics");++probe->physics;
    }
    temporalOriginalPhysics(app,selected);
}
void ObserveTemporalCatchup(MMDApp* app,unsigned char selected) {
    if(auto* probe=temporalSequenceProbe) {
        Check(!probe->insideRender,"temporal sub-samples do not advance playback");++probe->catchups;
    }
    temporalOriginalCatchup(app,selected);
}
bool ObserveTemporalWrite(MMDApp* app,IDirect3DSurface9* surface,std::wstring& error) {
    auto* probe=temporalSequenceProbe;
    TemporalSequenceWrite sample{};
    if(probe) {
        sample={app->CurrentFrame(),app->PlaybackCursorSeconds(),probe->renders-probe->lastWrittenRender,
            antialias::AccumulatedSamples(app),unsigned(app->state.recordedFrameCount)};
        D3DSURFACE_DESC desc{};IDirect3DDevice9* device=nullptr;IDirect3DSurface9* pixels=nullptr;
        Check(SUCCEEDED(surface->GetDesc(&desc)) && SUCCEEDED(surface->GetDevice(&device)),"read temporal output surface");
        Check((desc.Format==D3DFMT_A8R8G8B8 || desc.Format==D3DFMT_X8R8G8B8) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&pixels,nullptr)) &&
            SUCCEEDED(device->GetRenderTargetData(surface,pixels)),"read back temporal fixture pixels");
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(pixels->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"map temporal fixture pixels");
        unsigned visible=0;
        for(unsigned y=0;y<desc.Height;++y) {
            const auto* row=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+y*lock.Pitch);
            for(unsigned x=0;x<desc.Width;++x)visible+=desc.Format==D3DFMT_A8R8G8B8?
                (row[x]>>24)!=0:(row[x]&0xffffffu)!=0xffffffu;
        }
        pixels->UnlockRect();pixels->Release();device->Release();
        Check(visible>4,"temporal image contains visible fixture geometry");
    }
    const bool result=temporalOriginalWrite(app,surface,error);
    if(probe && result){probe->lastWrittenRender=probe->renders;probe->writes.push_back(sample);}
    return result;
}
IDirect3DTexture9* RejectTemporalSequenceMotion(MMDApp* app,IDirect3DDevice9* device) {
    auto* texture=temporalOriginalMotion(app,device);
    if(temporalSequenceProbe)std::printf("TRACE temporal-motion texture=%d forced_reject=%d\n",texture!=nullptr,temporalSequenceProbe->rejectMotion);
    return temporalSequenceProbe && temporalSequenceProbe->rejectMotion?nullptr:texture;
}
void CheckTemporalImageSequence(MMDApp* app) {
    wchar_t modelPath[32768]{};
    const auto modelLength=GetEnvironmentVariableW(L"MMS_AA_SEQUENCE_MODEL",modelPath,std::size(modelPath));
    if(!modelLength) {
        std::puts("SKIP temporal image sequence checks: set MMS_AA_SEQUENCE_MODEL to a real PMX fixture");
        return;
    }
    Check(modelLength<std::size(modelPath) && fs::is_regular_file(modelPath),"temporal PMX fixture exists");
    const auto previousSettings=app->antialiasing;
    ResetAppState(app);
    app->CurrentFrame()=0;app->PlaybackCursorSeconds()=0;
    LoadModelFile(app,modelPath,false);
    Check(app->ModelSlot(0)!=nullptr,"load real temporal export model");
    app->CameraPositionY()=10;app->CameraDistance()=-45;app->CameraYaw()=.3f;app->CameraPitch()=.1f;
    // Keep the requested camera in frame zero as recording evaluates the track.
    app->CameraFov()=30;app->CameraPerspective()=0;
    if(auto* camera=app->CameraKeys()) {
        camera[0].distance=-45;camera[0].eye[1]=10;camera[0].fov=30;camera[0].perspective=0;
        camera[0].target[0]=.1f;camera[0].target[1]=.3f;camera[0].parentModel=-1;
    }
    app->GroundGridEnabled()=false;app->GroundShadowEnabled()=false;
    Check(MH_CreateHook(reinterpret_cast<void*>(&RenderFrameScene),reinterpret_cast<void*>(&ObserveTemporalScene),reinterpret_cast<void**>(&temporalOriginalRender))==MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&PhysicsFrame),reinterpret_cast<void*>(&ObserveTemporalPhysics),reinterpret_cast<void**>(&temporalOriginalPhysics))==MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&PlaybackCatchup),reinterpret_cast<void*>(&ObserveTemporalCatchup),reinterpret_cast<void**>(&temporalOriginalCatchup))==MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&seq::WriteFrame),reinterpret_cast<void*>(&ObserveTemporalWrite),reinterpret_cast<void**>(&temporalOriginalWrite))==MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&antialias::motion::Resolve),reinterpret_cast<void*>(&RejectTemporalSequenceMotion),reinterpret_cast<void**>(&temporalOriginalMotion))==MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&runtime_log::Write),reinterpret_cast<void*>(&ObserveTemporalLog),reinterpret_cast<void**>(&temporalOriginalLog))==MH_OK,
        "observe production temporal render and export boundaries");
    void* hooks[]={reinterpret_cast<void*>(&RenderFrameScene),reinterpret_cast<void*>(&PhysicsFrame),
        reinterpret_cast<void*>(&PlaybackCatchup),reinterpret_cast<void*>(&seq::WriteFrame),
        reinterpret_cast<void*>(&antialias::motion::Resolve),reinterpret_cast<void*>(&runtime_log::Write)};
    for(auto* hook:hooks)Check(MH_EnableHook(hook)==MH_OK,"enable temporal export observation");
    const auto select=[&](antialias::Algorithm algorithm,bool preview=true) {
        std::wstring error;
        if(!app->antialiasing.enabled)Check(antialias::Toggle(app,error),"enable AA for temporal output checks");
        Check(antialias::Select(app,algorithm,error),"select real output antialiasing algorithm");
        if(!preview)return;
        const auto deadline=GetTickCount64()+10000;
        do {app->state.messageSeen=1;RenderFrameScene(app);Sleep(1);}
        while(antialias::Initializing(app) && GetTickCount64()<deadline);
        Check(!antialias::Initializing(app),"output antialiasing shaders become ready");
        for(unsigned i=0;i<antialias::RequiredSamples(app);++i){app->state.messageSeen=1;RenderFrameScene(app);}
    };
    const auto run=[&](const wchar_t* name,int first,int last,unsigned firstSamples,bool rejectMotion=false) {
        seq::Options options;options.directory=(output/name).wstring();fs::create_directories(options.directory);
        options.width=64;options.height=48;options.start=first;options.end=last;options.fps=30;options.format=2;
        // A stale playhead cannot substitute for initializing the requested
        // first frame, including two consecutive exports without a preview.
        app->CurrentFrame()=37;app->PlaybackCursorSeconds()=37.0f/30.0f;
        TemporalSequenceProbe probe;probe.rejectMotion=rejectMotion;temporalSequenceProbe=&probe;
        Start(app,options);
        unsigned steps=0;
        while(seq::Active(app) && steps++<20){app->state.messageSeen=1;FrameDriver(app);}
        temporalSequenceProbe=nullptr;
        Check(!seq::Active(app) && !app->RecordingWindow(),"temporal image export completes normally");
        const auto expected=unsigned(seq::FrameCount(options));
        std::printf("TRACE temporal-export mode=%u range=%d..%d writes=%zu renders=%u expected_renders=%u physics=%u catchups=%u steps=%u\n",
            unsigned(app->antialiasing.algorithm),first,last,probe.writes.size(),probe.renders,firstSamples+expected-1,probe.physics,probe.catchups,steps);
        Check(probe.writes.size()==expected,"temporal samples produce exactly the requested image count");
        Check(probe.renders==firstSamples+expected-1,"only the first output frame fills missing temporal samples");
        Check(probe.physics==steps && probe.catchups==steps,"physics and playback advance once per output driver step");
        unsigned files=0;
        for(const auto& entry:fs::directory_iterator(options.directory)) {
            Check(entry.path().extension()==L".png","temporal export produces no extra image or temporary files");++files;
        }
        Check(files==expected,"temporal warm-up does not write extra files");
        for(unsigned i=0;i<expected;++i) {
            const auto& sample=probe.writes[i];
            // Recording evaluates animation from PlaybackCursorSeconds; the
            // stopped editing playhead (CurrentFrame) may stay at the start.
            if(i==0)Check(sample.frame==first,"first output initializes the requested editing frame");
            Check(std::abs(sample.time-double(first+int(i))/30.0)<1e-5,
                "encoded temporal frames keep the requested animation timestamp");
            Check(sample.recorded==i,"temporal samples do not advance the output frame counter");
            Check(sample.renders==(i==0?firstSamples:1u),"later sequence images render once using shared history");
            if(firstSamples>1)Check(sample.samples>=firstSamples,"first image contains all required temporal samples");
            Check(fs::file_size(seq::FramePath(options,i+1))>0,"temporal output image is complete");
        }
        std::printf("PASS temporal-sequence mode=%u range=%d..%d images=%u renders=%u first_samples=%u physics=%u catchups=%u reject_motion=%d\n",
            unsigned(app->antialiasing.algorithm),first,last,expected,probe.renders,firstSamples,probe.physics,probe.catchups,rejectMotion);
    };
    select(antialias::Algorithm::SmaaT2xEnhanced,false);
    run(L"temporal-t2x-cold-zero",0,0,2);
    select(antialias::Algorithm::SmaaT2xStandard);
    run(L"temporal-t2x-zero",0,0,2);
    run(L"temporal-t2x-zero-again",0,0,2);
    run(L"temporal-t2x-sequence",0,2,2);
    select(antialias::Algorithm::SmaaStandard);
    run(L"temporal-1x-sequence",0,2,1);
    select(antialias::Algorithm::Taa);
    run(L"temporal-taa-zero",0,0,8);
    select(antialias::Algorithm::SmaaT2xStandard);
    run(L"temporal-motion-unavailable",0,0,1,true);
    for(auto* hook:hooks){MH_DisableHook(hook);MH_RemoveHook(hook);}
    std::wstring error;
    Check(antialias::Select(app,previousSettings.algorithm,error),"restore pre-test antialiasing mode");
    if(app->antialiasing.enabled!=previousSettings.enabled)Check(antialias::Toggle(app,error),"restore pre-test AA enable state");
}
