// Compare the native production draw before/after coverage, with identical AA.
decltype(&antialias::DrawOutline) originalCoverage=nullptr;
bool legacyCoverage=false;
unsigned coverageCalls=0,coverageDraws=0;
bool ObserveCoverage(MMDApp* app,unsigned char* model,unsigned first,unsigned count,const float* color) {
    ++coverageCalls;
    if(legacyCoverage)return false;
    const bool drawn=originalCoverage(app,model,first,count,color);coverageDraws+=drawn;return drawn;
}
void CheckOutlineCoverage(MMDApp* app,const wchar_t* directory) {
    const auto helper=reinterpret_cast<void*>(&antialias::DrawOutline);
    Check(MH_CreateHook(helper,&ObserveCoverage,reinterpret_cast<void**>(&originalCoverage))==MH_OK &&
        MH_EnableHook(helper)==MH_OK,"observe native coverage draws");
    auto** vtable=*reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[17],&ObservePresent,reinterpret_cast<void**>(&originalPresent))==MH_OK &&
        MH_EnableHook(vtable[17])==MH_OK,"capture real outline frames");
    const std::filesystem::path output=directory;std::filesystem::create_directories(output);
    app->EditMode()=ViewportEditMode::None;app->PlaybackPhysicsMode()=0;
    app->GroundGridEnabled()=app->GroundShadowEnabled()=false;
    app->CameraPositionX()=app->ViewOffsetX()=app->ViewOffsetY()=app->CameraPositionZ()=0;
    app->CameraPositionY()=app->ViewOffsetY()=10;app->CameraDistance()=-45;
    app->CameraYaw()=.05f;app->CameraPitch()=app->CameraRoll()=0;
    std::wstring error;
    for(auto mode:{antialias::Algorithm::Fxaa,antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                  antialias::Algorithm::Taa,antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
        Check(antialias::Select(app,mode,error),"select outline comparison AA");
        if(!app->antialiasing.enabled)Check(antialias::Toggle(app,error),"enable comparison AA");
        const auto until=GetTickCount64()+30000;
        while(antialias::Initializing(app)&&GetTickCount64()<until) {
            app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);Sleep(1);
        }
        Check(!antialias::Initializing(app),"comparison AA ready");
        std::vector<std::uint32_t> before;
        for(bool legacy:{true,false}) {
            legacyCoverage=legacy;antialias::ResetHistory(app);coverageCalls=coverageDraws=0;
            for(unsigned i=0;i<8;++i) {
                captureRequested=i==7;app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);
            }
            Check(!captureRequested&&coverageCalls>0,"real model renders outline pass");
            Check(legacy?coverageDraws==0:coverageDraws>0,"coverage used only in corrected frames");
            const auto name=std::to_string(unsigned(mode))+(legacy?"-legacy":"-fixed");
            std::ofstream raw(output/(name+".bgra"),std::ios::binary);
            raw.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
            std::ofstream size(output/(name+".txt"));size<<captureWidth<<' '<<captureHeight;
            if(legacy)before=capturedPixels;
            else {
                unsigned changed=0;for(std::size_t i=0;i<before.size();++i)changed+=before[i]!=capturedPixels[i];
                std::printf("OUTLINE_COVERAGE mode=%u calls=%u draws=%u changed=%u samples=%u\n",
                    unsigned(mode),coverageCalls,coverageDraws,changed,antialias::AccumulatedSamples(app));
                Check(changed>0,"coverage reaches final model image");
                if(mode==antialias::Algorithm::Taa||mode==antialias::Algorithm::SmaaT2xStandard||mode==antialias::Algorithm::SmaaT2xEnhanced)
                    Check(antialias::AccumulatedSamples(app)>=8,"coverage shader retains temporal motion capture");
            }
        }
        if(mode==antialias::Algorithm::Fxaa && std::getenv("MMS_OUTLINE_BENCHMARK")) {
            const unsigned drawsPerFrame=coverageCalls/8;
            Microsoft::WRL::ComPtr<IDirect3DQuery9> fence;
            Check(SUCCEEDED(app->Renderer()->device->CreateQuery(D3DQUERYTYPE_EVENT,&fence)),"create timing fence");
            const auto finish=[&]{fence->Issue(D3DISSUE_END);while(fence->GetData(nullptr,0,D3DGETDATA_FLUSH)==S_FALSE)Sleep(0);};
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
            unsigned block=0;
            for(bool legacy:{true,false,false,true}) {
                legacyCoverage=legacy;
                for(unsigned i=0;i<8;++i){app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);}
                finish();LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
                coverageCalls=0;
                for(unsigned i=0;i<64;++i){app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);}
                finish();QueryPerformanceCounter(&end);
                Check(drawsPerFrame>0 && coverageCalls==64*drawsPerFrame,"timing block renders every model frame");
                std::printf("OUTLINE_TIMING block=%u legacy=%d frames=64 ms_per_frame=%.5f\n",++block,legacy,
                    double(end.QuadPart-start.QuadPart)*1000/frequency.QuadPart/64);
            }
        }
    }
    MH_DisableHook(helper);MH_DisableHook(vtable[17]);
    Check(!IsWindowVisible(app->MainWindow()),"outline comparison host stays hidden");
}

void CheckOutlineStability(MMDApp* app,const wchar_t* directory) {
    auto** vtable=*reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[17],&ObservePresent,reinterpret_cast<void**>(&originalPresent))==MH_OK &&
        MH_EnableHook(vtable[17])==MH_OK,"capture static AA sequence");
    const std::filesystem::path output=directory;std::filesystem::create_directories(output);
    app->EditMode()=ViewportEditMode::None;app->PlaybackPhysicsMode()=0;
    app->CameraMode()=1;app->CameraParentModel()=-1;
    app->GroundGridEnabled()=app->GroundShadowEnabled()=false;
    app->CameraPositionX()=app->ViewOffsetX()=app->ViewOffsetY()=app->CameraPositionZ()=0;
    app->CameraPositionY()=10;app->CameraDistance()=-45;
    app->CameraYaw()=.05f;app->CameraPitch()=app->CameraRoll()=0;
    std::wstring error;
    for(auto mode:{antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa,antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                  antialias::Algorithm::Taa,antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
        Check(antialias::Select(app,mode,error),"select static AA mode");
        if(!app->antialiasing.enabled)Check(antialias::Toggle(app,error),"enable static AA");
        const auto until=GetTickCount64()+30000;
        do {app->DeltaTime()=0;app->state.messageSeen=10;FrameDriver(app);Sleep(1);}
        while(antialias::Initializing(app)&&GetTickCount64()<until);
        if(mode==antialias::Algorithm::MmdOriginal) {
            const auto samples=app->Renderer()->presentParameters.MultiSampleType;
            Check(samples>=D3DMULTISAMPLE_2_SAMPLES,"real model baseline uses original hardware MSAA");
            std::printf("REFERENCE_MSAA samples=%u\n",unsigned(samples));
        }
        std::vector<std::uint32_t> previous;
        double total=0;unsigned worst=0,changed=0;
        for(unsigned i=0;i<48;++i) {
            captureRequested=true;app->DeltaTime()=0;app->state.messageSeen=10;FrameDriver(app);
            Check(!captureRequested,"capture static scene");
            if(i>=32) {
                for(std::size_t p=0;p<capturedPixels.size();++p) {
                    unsigned difference=0;
                    for(unsigned shift:{0u,8u,16u}) difference=std::max(difference,unsigned(std::abs(
                        int((capturedPixels[p]>>shift)&255)-int((previous[p]>>shift)&255))));
                    total+=difference;worst=std::max(worst,difference);changed+=difference>8;
                }
            }
            previous=capturedPixels;
            if(i==46||i==47) {
                const auto name=std::to_string(unsigned(mode))+"-"+std::to_string(i);
                std::ofstream raw(output/(name+".bgra"),std::ios::binary);
                raw.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
                std::ofstream size(output/(name+".txt"));size<<captureWidth<<' '<<captureHeight;
            }
        }
        std::printf("STATIC_AA mode=%u mean_delta=%.6f worst=%u changed_gt8=%u samples=%u\n",
            unsigned(mode),total/(16*capturedPixels.size()),worst,changed,antialias::AccumulatedSamples(app));
        if(mode==antialias::Algorithm::Taa||mode==antialias::Algorithm::SmaaT2xStandard||mode==antialias::Algorithm::SmaaT2xEnhanced) {
            const auto field=ReadAntialiasMotionField(app);
            unsigned valid=0,rejected=0;
            for(const auto& p:field.pixels)if(p[2]>0) {valid+=p[3]>0;rejected+=p[3]<=0;}
            std::printf("STATIC_MOTION mode=%u valid=%u rejected=%u max_pixels=%.7f\n",unsigned(mode),valid,rejected,field.MaximumPixels());
            Check(field.MaximumPixels()<.02f,"frozen geometry has no false motion above subpixel round-off");
            Check(valid>100 && std::uint64_t(changed)*100<=std::uint64_t(valid)*16,
                "large static fluctuations affect less than one percent of the model pixels per frame");
        } else if(mode!=antialias::Algorithm::MmdOriginal) {
            Check(changed==0 && worst==0,"spatial AA stays identical across a frozen sequence");
        }
        if(mode==antialias::Algorithm::Taa||mode==antialias::Algorithm::SmaaT2xStandard||mode==antialias::Algorithm::SmaaT2xEnhanced) {
            const float originalYaw=app->CameraYaw();
            const auto samples=antialias::AccumulatedSamples(app);
            float maxMotion=0;
            for(unsigned frame=0;frame<8;++frame) {
                app->CameraYaw()=originalYaw+(frame+1)*.004f;
                captureRequested=true;app->DeltaTime()=0;app->state.messageSeen=10;FrameDriver(app);
                Check(!captureRequested,"capture moving softened silhouette");
                maxMotion=std::max(maxMotion,ReadAntialiasMotionField(app).MaximumPixels());
            }
            Check(maxMotion>.01f && antialias::AccumulatedSamples(app)>=samples+8 && previous!=capturedPixels,
                "softened temporal outlines retain live camera motion and advancing history");
            std::printf("OUTLINE_MOTION mode=%u max_pixels=%.6f samples=%u\n",
                unsigned(mode),maxMotion,antialias::AccumulatedSamples(app));
            app->CameraYaw()=originalYaw;
        }
    }
    MH_DisableHook(vtable[17]);
}
