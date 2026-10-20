// Kept in the real-MME host so every capture uses the production scene path.
#include "antialias_temporal_checks.hpp"
#include "antialias_motion_probe.hpp"
#include "antialias_stability_checks.hpp"
using AntialiasCheckFormat=HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,D3DFORMAT,DWORD,D3DRESOURCETYPE,D3DFORMAT);
AntialiasCheckFormat originalAntialiasCheckFormat=nullptr;
unsigned rejectedAntialiasFloatQueries=0;
bool rejectAntialiasVertexTexture=false;
bool RequireOriginalCoverageForTest(){return false;}
HRESULT WINAPI RejectAntialiasFloatTarget(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,D3DFORMAT adapterFormat,
    DWORD usage,D3DRESOURCETYPE resource,D3DFORMAT format) {
    if(format==D3DFMT_A32B32G32R32F &&
       (usage&(rejectAntialiasVertexTexture?D3DUSAGE_QUERY_VERTEXTEXTURE:D3DUSAGE_RENDERTARGET))) {
        ++rejectedAntialiasFloatQueries;return D3DERR_NOTAVAILABLE;
    }
    return originalAntialiasCheckFormat(d3d,adapter,type,adapterFormat,usage,resource,format);
}
void CheckAntialiasScene(MMDApp* app, const wchar_t* directory) {
    if(std::getenv("MMS_AA_STABILITY")) { CheckAntialiasStability(app,directory);return; }
    Check(antialias::Settings{}.outlineMode==antialias::OutlineMode::FxaaAdaptive,
        "new AA settings default to FXAA outline softening without additional MSAA");
    CheckTemporalResolveShader();
    captureDirectory = directory;
    std::filesystem::create_directories(captureDirectory);
    auto* r = app->Renderer();
    // Freeze this saved pose so algorithm comparisons do not include hair
    // physics evolving while the test traverses the menu.
    app->PlaybackPhysicsMode()=0;
    std::printf("PMM camera=%g,%g,%g distance=%g rotation=%g,%g,%g render=%dx%d viewport=%ld,%ld,%ld,%ld\n",
        app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ(), app->CameraDistance(),
        app->CameraPitch(), app->CameraYaw(), app->CameraRoll(), app->RenderWidth(), app->RenderHeight(),
        app->ViewportRect().left, app->ViewportRect().top, app->ViewportRect().right, app->ViewportRect().bottom);
    auto** vtable = *reinterpret_cast<void***>(r->device);
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
        MH_EnableHook(vtable[17]) == MH_OK, "capture saved PMM view");
    const auto capture=[&](const char* name,int frames=4) {
        const auto until=GetTickCount64()+10000;
        const bool wasEnabled=app->antialiasing.enabled;
        while(antialias::Initializing(app) && GetTickCount64()<until) {
            app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);Sleep(1);
        }
        Check(!antialias::Initializing(app) && app->antialiasing.enabled==wasEnabled,
            "requested AA is ready before comparison capture");
        const auto frameStart=GetTickCount64();
        for (int frame=0;frame<frames;++frame) {
            captureRequested=frame==frames-1;
            app->DeltaTime()=1.f/30;app->state.messageSeen=10;FrameDriver(app);
        }
        const auto frameMs=GetTickCount64()-frameStart;
        Check(!captureRequested && !capturedPixels.empty(), "saved PMM pixels captured");
        std::ofstream file(captureDirectory/(std::string(name)+".bgra"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
        std::ofstream size(captureDirectory/(std::string(name)+".txt"));size<<captureWidth<<" "<<captureHeight;
        std::printf("AA CAPTURE %s mode=%u enabled=%u samples=%u frames=%d ms=%llu\n",name,unsigned(app->antialiasing.algorithm),
            app->antialiasing.enabled,unsigned(r->presentParameters.MultiSampleType),frames,frameMs);
    };
    const auto select=[&](antialias::Algorithm mode) {
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kFirstCommand+unsigned(mode),0);
        Check(app->antialiasing.algorithm==mode,"menu selects actual algorithm");
    };
    select(antialias::Algorithm::MmdOriginal);
    if(!app->antialiasing.enabled)CommandDispatch(nullptr,MAKEWPARAM(277,0));
    capture("original-aa");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("disabled");
    const auto disabled=capturedPixels;
    select(antialias::Algorithm::Fxaa);
    Check(app->antialiasing.enabled,"algorithm selection enables the main AA item");
    capture("fxaa");
    Check(disabled!=capturedPixels,"FXAA changes actual rendered edge pixels");
    auto* formatQuery=(*reinterpret_cast<void***>(r->d3d9))[10];
    Check(MH_CreateHook(formatQuery,&RejectAntialiasFloatTarget,reinterpret_cast<void**>(&originalAntialiasCheckFormat))==MH_OK &&
        MH_EnableHook(formatQuery)==MH_OK,"simulate unavailable standard D3D9 floating-point target");
    std::wstring capabilityError;
    Check(!antialias::Supported(app,antialias::Algorithm::Taa) &&
        !antialias::Select(app,antialias::Algorithm::Taa,capabilityError) && !capabilityError.empty(),
        "TAA rejects a device without floating-point render targets");
    Check(!antialias::Supported(app,antialias::Algorithm::SmaaT2xStandard) &&
        !antialias::Supported(app,antialias::Algorithm::SmaaT2xEnhanced) &&
        antialias::Supported(app,antialias::Algorithm::SmaaStandard) && antialias::Supported(app,antialias::Algorithm::Smaa),
        "SMAA T2x requires standard motion targets while both 1x presets remain available");
    Check(rejectedAntialiasFloatQueries>0 && app->antialiasing.algorithm==antialias::Algorithm::Fxaa && app->antialiasing.enabled,
        "unsupported temporal hardware preserves the working spatial AA mode");
    MH_DisableHook(formatQuery);MH_RemoveHook(formatQuery);
    Check(PostDeviceReset(app),"reset FXAA device");capture("fxaa-reset");
    select(antialias::Algorithm::Taa);capture("taa",18);
    std::printf("TAA samples=%u\n",antialias::AccumulatedSamples(app));
    Check(antialias::AccumulatedSamples(app)>=8,"TAA accumulates real jittered static frames");
    app->CameraYaw()+=.02f;capture("taa-camera-moved",1);
    Check(antialias::AccumulatedSamples(app)>=9,"camera motion reprojects temporal history instead of disabling TAA");
    const auto movingStart=antialias::AccumulatedSamples(app);
    for(int frame=0;frame<12;++frame) {
        app->CameraYaw()+=.004f;capture("taa-camera-playing",1);
    }
    Check(antialias::AccumulatedSamples(app)>=movingStart+12,"continuous camera motion retains temporal samples");
    const auto cameraMotion=antialias::motion::GetStats(app);
    const auto cameraPixels=ReadAntialiasMotionField(app).MaximumPixels();
    std::printf("TAA camera draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",cameraMotion.draws,cameraMotion.matchedDraws,
        cameraPixels,cameraMotion.vertexCaptures,cameraMotion.vertexCacheHits);
    Check(cameraMotion.matchedDraws>0 && cameraPixels>.01f,
        "camera motion produces real geometry velocity for history reprojection");
    app->CameraYaw()-=.068f;
    const auto* record=mdl::Mdl(app->SelectedModel());
    auto* bones=mdl::Bones(app->SelectedModel());
    Check(bones && record->boneCount,"animated TAA fixture has a skinned bone");
    std::vector<float> savedBones(std::size_t(record->boneCount)*16);
    for(unsigned bone=0;bone<record->boneCount;++bone)
        std::memcpy(savedBones.data()+std::size_t(bone)*16,bones[bone].matInit,16*sizeof(float));
    const auto savedFrame=app->CurrentFrame();const auto savedPlayback=app->PlaybackActive();
    const auto playbackStart=antialias::AccumulatedSamples(app);
    std::printf("TAA playback begin frame=%d samples=%u required=%u outline_mode=%u previous_playback=%d viewport_tool=%u\n",
        savedFrame,playbackStart,antialias::RequiredSamples(app),unsigned(app->antialiasing.outlineMode),int(savedPlayback),app->UsesViewportTool()?1u:0u);
    const auto beforePlayback=capturedPixels;
    app->PlaybackActive()=1;
    for(int frame=0;frame<12;++frame) {
        // Exercise the production playback rendering branch with deterministic
        // bone deformation; wall-clock animation updates cannot hide a reset.
        for(unsigned bone=0;bone<record->boneCount;++bone) {
            std::memcpy(bones[bone].matInit,savedBones.data()+std::size_t(bone)*16,16*sizeof(float));
            bones[bone].matInit[12]+=(frame+1)*.02f*(1+bone%3);
        }
        app->CurrentFrame()=savedFrame+frame+1;
        app->state.messageSeen=10;RenderFrameScene(app);
        captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
        Check(!captureRequested,"animated production frame reaches presentation");
        const auto motion=antialias::motion::GetStats(app);
        const auto outline=antialias::outline_multisample::GetStats(app);
        std::printf("TAA playback step=%d frame=%d samples=%u required=%u motion=%u/%u captures=%u hits=%u gpu=%u readbacks=%u coverage=%u opaque_skips=%u outline_samples=%u outlines=%u composites=%u failed=%u fallback=%s\n",
            frame,app->CurrentFrame(),antialias::AccumulatedSamples(app),antialias::RequiredSamples(app),motion.matchedDraws,motion.draws,
            motion.vertexCaptures,motion.vertexCacheHits,motion.gpuVertexCaptures,motion.cpuVertexReadbacks,motion.coverageReplays,motion.opaqueCoverageSkips,
            outline.sampleCount,outline.outlineDraws,outline.composites,outline.frameFailed?1u:0u,outline.fallbackReason.c_str());
    }
    Check(antialias::AccumulatedSamples(app)>=playbackStart+12,
        "playback with changing skinned positions keeps temporal accumulation active");
    const auto boneMotion=antialias::motion::GetStats(app);
    const auto bonePixels=ReadAntialiasMotionField(app).MaximumPixels();
    std::printf("TAA deformed draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",boneMotion.draws,boneMotion.matchedDraws,
        bonePixels,boneMotion.vertexCaptures,boneMotion.vertexCacheHits);
    Check(boneMotion.matchedDraws>0 && bonePixels>.01f,
        "deformed skinned vertices produce real motion vectors during playback");
    Check(capturedPixels!=beforePlayback,"animated TAA test changes actual rendered pixels");
    for(unsigned bone=0;bone<record->boneCount;++bone)
        std::memcpy(bones[bone].matInit,savedBones.data()+std::size_t(bone)*16,16*sizeof(float));
    app->CurrentFrame()=savedFrame;app->PlaybackActive()=savedPlayback;
    // Compare both implementations on the same geometry and camera delta.
    // The capability hook also exercises old AMD/Intel devices without VTF.
    select(antialias::Algorithm::Fxaa);capture("gpu-parity-reset",1);
    select(antialias::Algorithm::Taa);capture("gpu-parity-warm",3);
    app->CameraYaw()+=.02f;capture("gpu-parity-motion",1);
    const auto gpuField=ReadAntialiasMotionField(app);
    const auto gpuStats=antialias::motion::GetStats(app);
    std::printf("GPU MOTION captures=%u cache_hits=%u cpu_readbacks=%u coverage=%u skipped=%u\n",
        gpuStats.gpuVertexCaptures,gpuStats.vertexCacheHits,gpuStats.cpuVertexReadbacks,gpuStats.coverageReplays,gpuStats.opaqueCoverageSkips);
    Check(gpuStats.gpuVertexCaptures==0||gpuStats.cpuVertexReadbacks==0,
        "GPU temporal path never calls CPU vertex readback");
    app->CameraYaw()-=.02f;
    select(antialias::Algorithm::Fxaa);capture("cpu-parity-reset",1);
    rejectAntialiasVertexTexture=true;
    Check(MH_CreateHook(formatQuery,&RejectAntialiasFloatTarget,reinterpret_cast<void**>(&originalAntialiasCheckFormat))==MH_OK&&
        MH_EnableHook(formatQuery)==MH_OK,"simulate unavailable vertex texture sampling without rejecting temporal targets");
    const auto coverageQuery=reinterpret_cast<void*>(&deformed_mesh_capture::OpaqueCoverage);
    Check(MH_CreateHook(coverageQuery,&RequireOriginalCoverageForTest,nullptr)==MH_OK&&MH_EnableHook(coverageQuery)==MH_OK,
        "reference path replays coverage for every native material");
    select(antialias::Algorithm::Taa);capture("cpu-parity-warm",3);
    app->CameraYaw()+=.02f;capture("cpu-parity-motion",1);
    const auto cpuField=ReadAntialiasMotionField(app);
    const auto cpuStats=antialias::motion::GetStats(app);
    Check(cpuStats.gpuVertexCaptures==0&&cpuStats.matchedDraws>0&&antialias::AccumulatedSamples(app)>=4,
        "no-VTF adapter preserves correct temporal accumulation through compatible capture");
    Check(cpuStats.opaqueCoverageSkips==0&&cpuStats.coverageReplays==cpuStats.draws,
        "CPU reference keeps all original coverage draws");
    unsigned compared=0,different=0,coverageUnion=0,coverageDifference=0;float largest=0;
    Check(cpuField.width==gpuField.width&&cpuField.height==gpuField.height,"GPU and CPU motion fields have matching dimensions");
    if(cpuField.pixels.size()==gpuField.pixels.size())for(std::size_t i=0;i<cpuField.pixels.size();++i) {
        const auto& a=gpuField.pixels[i];const auto& b=cpuField.pixels[i];
        const bool validA=a[2]>0&&a[3]>0,validB=b[2]>0&&b[3]>0;
        coverageUnion+=validA||validB;coverageDifference+=validA!=validB;
        if(validA&&validB) {
            ++compared;float difference=0;
            for(unsigned k=0;k<4;++k)difference=std::max(difference,std::abs(a[k]-b[k])/std::max(1.f,std::abs(b[k])));
            largest=std::max(largest,difference);different+=difference>1e-4f;
        }
    }
    std::printf("GPU PARITY compared=%u different=%u max_relative_error=%g\n",compared,different,largest);
    std::printf("GPU COVERAGE union=%u mismatches=%u\n",coverageUnion,coverageDifference);
    Check(coverageUnion>100&&coverageDifference*1000u<=coverageUnion,
        "optimized motion coverage matches complete reference coverage at 99.9 percent of valid pixels");
    Check(compared>100&&different*1000u<=compared,"GPU history velocities and depths agree with CPU reference at 99.9 percent of valid pixels");
    app->CameraYaw()-=.02f;MH_DisableHook(formatQuery);MH_RemoveHook(formatQuery);rejectAntialiasVertexTexture=false;
    MH_DisableHook(coverageQuery);MH_RemoveHook(coverageQuery);
    select(antialias::Algorithm::Msaa2);capture("msaa2");
    Check(r->presentParameters.MultiSampleType==D3DMULTISAMPLE_2_SAMPLES,"MSAA 2 uses 2 samples");
    select(antialias::Algorithm::Msaa4);capture("msaa4");
    Check(r->presentParameters.MultiSampleType==D3DMULTISAMPLE_4_SAMPLES,"MSAA 4 uses 4 samples");
    select(antialias::Algorithm::Msaa8);capture("msaa8");
    select(antialias::Algorithm::Smaa);capture("smaa");
    Check(disabled!=capturedPixels,"SMAA changes actual rendered edge pixels");
    Check(PostDeviceReset(app),"reset SMAA device");capture("smaa-reset");
    std::wstring smaaError;
    Check(viewport_render_scale::Configure(app,false,150,smaaError),"combine SMAA with viewport scaling");capture("smaa-scale150");
    Check(viewport_render_scale::Configure(app,true,100,smaaError),"restore viewport scale after SMAA");
    select(antialias::Algorithm::SmaaStandard);capture("smaa-standard");
    for(const auto mode:{antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
        select(mode);
        const auto before=app->CurrentFrame();app->CurrentFrame()=0;
        capture(mode==antialias::Algorithm::SmaaT2xStandard?"smaa-t2x-standard":"smaa-t2x-enhanced",2);
        Check(app->CurrentFrame()==0 && antialias::RequiredSamples(app)==2 && antialias::AccumulatedSamples(app)>=2,
            "SMAA T2x accumulates both actual submissions in a static frame-zero scene");
        app->CameraYaw()+=.004f;capture("smaa-t2x-camera",1);
        const auto moving=antialias::motion::GetStats(app);
        const auto movingPixels=ReadAntialiasMotionField(app).MaximumPixels();
        std::printf("SMAA T2x mode=%u draws=%u matched=%u motion_px=%g captures=%u cache_hits=%u\n",
            unsigned(mode),moving.draws,moving.matchedDraws,movingPixels,moving.vertexCaptures,moving.vertexCacheHits);
        Check(moving.matchedDraws>0 && movingPixels>.01f && antialias::AccumulatedSamples(app)>=3,
            "SMAA T2x retains both phases while reprojecting real camera motion");
        app->CameraYaw()-=.004f;app->CurrentFrame()=before;
    }
    select(antialias::Algorithm::Fxaa);
    Check(app->antialiasing.lastSmaaAlgorithm==antialias::Algorithm::SmaaT2xEnhanced,
        "switching to FXAA preserves the last selected SMAA variant");
    SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kSmaaCommand,0);
    Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
        "former SMAA parent command cannot apply a remembered variant");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("fxaa-disabled");
    Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa && !app->antialiasing.enabled,
        "main item toggles without forgetting algorithm");
    CommandDispatch(nullptr,MAKEWPARAM(277,0));capture("fxaa-reenabled");
    std::wstring error;
    Check(viewport_render_scale::Configure(app,false,150,error),"combine AA with viewport scaling");capture("fxaa-scale150");
    Check(viewport_render_scale::Configure(app,true,100,error),"restore viewport scale");
    // Exercise the production recording branch without starting a codec or
    // touching a user output file. It ends before editor overlays are drawn.
    app->RecordingWindow()=CreateWindowW(L"STATIC",L"Hidden AA export",WS_POPUP,0,0,640,480,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(app->RecordingWindow()!=nullptr,"hidden recording surface created");
    Check(EnsureCaptureRenderTarget(app),"initialize the production recording target before AA begins");
    Microsoft::WRL::ComPtr<IDirect3DSurface9> recordingTarget;
    r->device->GetRenderTarget(0,&recordingTarget);
    app->state.messageSeen=10;RenderFrameScene(app);
    captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
    Check(!captureRequested,"FXAA completes recording frame before presentation");
    Microsoft::WRL::ComPtr<IDirect3DSurface9> restored;
    r->device->GetRenderTarget(0,&restored);
    Check(restored.Get()==recordingTarget.Get(),"recording restores production target ownership");
    restored.Reset();recordingTarget.Reset();
    select(antialias::Algorithm::Smaa);
    Check(EnsureCaptureRenderTarget(app),"restore capture target after the SMAA device reset");
    r->device->GetRenderTarget(0,&recordingTarget);
    app->state.messageSeen=10;RenderFrameScene(app);
    captureRequested=true;r->device->Present(nullptr,nullptr,nullptr,nullptr);
    Check(!captureRequested,"SMAA completes recording frame before presentation");
    restored.Reset();r->device->GetRenderTarget(0,&restored);
    Check(restored.Get()==recordingTarget.Get(),"SMAA recording restores production target ownership");
    restored.Reset();recordingTarget.Reset();
    select(antialias::Algorithm::Taa);
    const auto recordingFrame=app->CurrentFrame();
    for(int frame=0;frame<10;++frame) {
        app->CurrentFrame()=recordingFrame+frame;
        app->state.messageSeen=10;RenderFrameScene(app);
    }
    Check(antialias::AccumulatedSamples(app)>=8,"recording continues temporal accumulation across changing frames");
    app->CurrentFrame()=recordingFrame;
    DestroyWindow(app->RecordingWindow());app->RecordingWindow()=nullptr;
    // Exercise both shared outline strategies with each post-process family.
    // Require real native geometry rather than treating an empty/fallback
    // outline layer as successful MME compatibility.
    unsigned eligibleOutlineModels=0,eligibleOutlineMaterials=0;
    for(int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot)) {
        const auto& state=*mdl::Mdl(model);
        if(state.loadComplete==0 || state.displayState!=0 || state.edgeScale<=0 || state.postLoadFlag2!=0)continue;
        auto* materials=mdl::Materials(model);if(!materials)continue;
        ++eligibleOutlineModels;
        for(unsigned material=0;material<state.materialCount;++material)
            eligibleOutlineMaterials+=materials[material].faceVertexCount>0 && materials[material].doubleSided!=0;
    }
    std::printf("OUTLINE fixture eligible_models=%u eligible_materials=%u playback=%d viewport_tool=%u effect_suppressed=%d\n",
        eligibleOutlineModels,eligibleOutlineMaterials,int(app->PlaybackActive()),app->UsesViewportTool()?1u:0u,int(app->ModelNonDisplayMode()));
    Check(eligibleOutlineModels>0 && eligibleOutlineMaterials>0,
        "shared-outline fixture contains visible loaded native models with enabled outline material draws");
    Check(app->PlaybackActive()!=0 || app->UsesViewportTool(),
        "shared-outline fixture opens the renderer's camera/playback outline gate");
    Check(r->postProcessEnabled==0 || app->ModelNonDisplayMode()==0,
        "shared-outline fixture does not suppress native outlines in the effect-rendering path");
    std::ofstream strategyTable(captureDirectory/L"outline-strategies.tsv");
    Check(strategyTable.good(),"create shared-outline execution evidence");
    strategyTable<<"outline_mode\talgorithm\twarmup_frames\taccumulated_samples\trequired_samples\toutline_samples\tdepth_replays\toutline_draws\tcomposites\tobserved_native\tframe_failed\tfallback\tmotion_draws\tmotion_matched\tmotion_pixels\n";
    for(const auto outline:{antialias::OutlineMode::FxaaAdaptive,antialias::OutlineMode::Multisample}) {
        std::wstring outlineError;
        const auto beforeAlgorithm=app->antialiasing.algorithm;
        const bool beforeEnabled=app->antialiasing.enabled;
        Check(antialias::SelectOutlineMode(app,outline,outlineError),"select shared outline strategy through the production API");
        Check(app->antialiasing.outlineMode==outline && app->antialiasing.algorithm==beforeAlgorithm &&
            app->antialiasing.enabled==beforeEnabled,"outline strategy does not replace the selected scene AA mode");
        for(const auto algorithm:{antialias::Algorithm::Fxaa,antialias::Algorithm::Taa,
                                 antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                                 antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
            select(algorithm);
            const bool temporal=algorithm==antialias::Algorithm::Taa || algorithm==antialias::Algorithm::SmaaT2xStandard ||
                algorithm==antialias::Algorithm::SmaaT2xEnhanced;
            const unsigned expectedSamples=algorithm==antialias::Algorithm::Taa?8:temporal?2:1;
            const int warmupFrames=algorithm==antialias::Algorithm::Taa?10:temporal?4:3;
            const auto name=std::string(outline==antialias::OutlineMode::FxaaAdaptive?"outline-fxaa-adaptive-aa":"outline-msaa-coverage-aa")+
                std::to_string(unsigned(algorithm));
            capture(name.c_str(),warmupFrames);
            const auto warmedSamples=antialias::AccumulatedSamples(app);
            const auto savedYaw=app->CameraYaw();
            if(temporal) {
                // Both observers must keep working when geometry/camera history
                // is used. A static image alone would miss silent motion fallback.
                app->CameraYaw()=savedYaw+.004f;
                capture(name.c_str(),1);
            }
            const auto native=antialias::outline_multisample::GetStats(app);
            const auto motion=antialias::motion::GetStats(app);
            const auto accumulated=antialias::AccumulatedSamples(app),required=antialias::RequiredSamples(app);
            float motionPixels=0;
            if(temporal)motionPixels=ReadAntialiasMotionField(app).MaximumPixels();
            strategyTable<<unsigned(outline)<<'\t'<<unsigned(algorithm)<<'\t'<<warmupFrames<<'\t'<<accumulated<<'\t'<<required<<'\t'
                <<native.sampleCount<<'\t'<<native.depthReplays<<'\t'<<native.outlineDraws<<'\t'<<native.composites<<'\t'
                <<native.observedNativeDevice<<'\t'<<native.frameFailed<<'\t'<<native.fallbackReason<<'\t'
                <<motion.draws<<'\t'<<motion.matchedDraws<<'\t'<<motionPixels<<'\n';
            strategyTable.flush();
            std::printf("OUTLINE mode=%u algorithm=%u accumulated=%u required=%u samples=%u depth=%u draws=%u composites=%u native=%u failed=%u fallback=%s motion=%u/%u motion_px=%g\n",
                unsigned(outline),unsigned(algorithm),accumulated,required,native.sampleCount,native.depthReplays,native.outlineDraws,
                native.composites,native.observedNativeDevice?1u:0u,native.frameFailed?1u:0u,native.fallbackReason.c_str(),
                motion.matchedDraws,motion.draws,motionPixels);
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==algorithm && app->antialiasing.outlineMode==outline,
                "actual shared-outline frame preserves both requested selections and AA activation");
            Check(required==expectedSamples,"shared-outline rendering does not silently replace the requested temporal algorithm with spatial fallback");
            if(temporal) {
                Check(warmedSamples>=unsigned(warmupFrames) && accumulated>=warmedSamples+1,
                    "TAA and SMAA T2x retain actual history across a camera-moving frame with either outline strategy");
                Check(motion.draws>0 && motion.matchedDraws>0 && motionPixels>.01f,
                    "shared outline and temporal observers both preserve matched geometry and real camera velocity");
            }
            if(outline==antialias::OutlineMode::Multisample) {
                Check(native.sampleCount>=4 && native.depthReplays>0 && native.outlineDraws>0 && native.composites==1,
                    "MSAA outline strategy really replays body depth, draws multisampled hulls and composites exactly once");
                Check(native.observedNativeDevice && !native.observing && !native.frameFailed && native.fallbackReason.empty() &&
                    !antialias::outline_multisample::Active(app) && !antialias::outline_multisample::Failed(app) &&
                    antialias::outline_multisample::GetTexture(app)!=nullptr,
                    "MSAA outline frame completes on the native device with a published layer and no fallback");
            } else {
                Check(native.sampleCount==0 && native.depthReplays==0 && native.outlineDraws==0 && native.composites==0 &&
                    !native.observedNativeDevice && !native.observing && !native.frameFailed && native.fallbackReason.empty() &&
                    !antialias::outline_multisample::Active(app) && antialias::outline_multisample::GetTexture(app)==nullptr,
                    "pure FXAA outline strategy never allocates or executes an independent MSAA outline pass");
            }
            app->CameraYaw()=savedYaw;
        }
    }
    {
        std::wstring outlineError;
        Check(antialias::SelectOutlineMode(app,antialias::OutlineMode::FxaaAdaptive,outlineError),
            "restore pure FXAA outline preference before menu and restart checks");
    }
    select(antialias::Algorithm::MmdOriginal);
    select(antialias::Algorithm::Fxaa);
    for(int language=0;language<3;++language) {
        app->EnglishUI()=language;RefreshMenuLanguage(app);
        SendMessageW(app->MainWindow(),WM_ENTERMENULOOP,0,0);
        HMENU bar=GetMenu(app->MainWindow()),parent=nullptr,popup=nullptr;int index=-1;
        for(int i=0;i<GetMenuItemCount(bar);++i) {
            auto child=GetSubMenu(bar,i);
            for(int j=0;j<GetMenuItemCount(child);++j) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STATE;
                GetMenuItemInfoW(child,j,TRUE,&item);
                if(item.wID==277){parent=child;popup=item.hSubMenu;index=j;}
            }
        }
        Check(popup && GetMenuItemCount(popup)==7,
            "AA flyout has the algorithm choices, separator and independent outline processing submenu");
        wchar_t offLabel[32]{};GetMenuStringW(popup,antialias::kOffCommand,offLabel,32,MF_BYCOMMAND);
        Check(std::wcscmp(offLabel,language==2?L"关闭":language==1?L"Off":L"オフ")==0,"Off is localized in every UI language");
        const auto commandPosition=[&](unsigned command) {
            for(int i=0;i<GetMenuItemCount(popup);++i) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID;
                if(GetMenuItemInfoW(popup,i,TRUE,&item) && item.wID==command)return i;
            }
            return -1;
        };
        const int smaaPosition=commandPosition(antialias::kSmaaCommand);
        const int outlinePosition=commandPosition(antialias::kOutlineCommand);
        auto smaaPopup=GetSubMenu(popup,smaaPosition);
        Check(smaaPopup && GetMenuItemCount(smaaPopup)==4,"four SMAA variants are available in the right-hand submenu");
        auto outlinePopup=GetSubMenu(popup,outlinePosition);
        Check(outlinePopup && GetMenuItemCount(outlinePopup)==2,"outline processing offers exactly two independent strategies");
        wchar_t outlineLabel[96]{},fxaaOutlineLabel[96]{},msaaOutlineLabel[96]{};
        GetMenuStringW(popup,antialias::kOutlineCommand,outlineLabel,96,MF_BYCOMMAND);
        GetMenuStringW(outlinePopup,antialias::kOutlineFxaaCommand,fxaaOutlineLabel,96,MF_BYCOMMAND);
        GetMenuStringW(outlinePopup,antialias::kOutlineMsaaCommand,msaaOutlineLabel,96,MF_BYCOMMAND);
        Check(std::wcscmp(outlineLabel,language==2?L"描边处理":language==1?L"Outline processing":L"輪郭線処理")==0 &&
            std::wcscmp(fxaaOutlineLabel,language==2?L"FXAA 自适应柔化":language==1?L"FXAA adaptive softening":L"FXAA 適応平滑化")==0 &&
            std::wcscmp(msaaOutlineLabel,language==2?L"MSAA 覆盖＋自适应柔化":language==1?L"MSAA coverage + adaptive softening":L"MSAA カバレッジ＋適応平滑化")==0,
            "outline processing and both strategies are localized in every UI language");
        const auto checkOutlineSelection=[&]() {
            // RefreshMenu replaces descendant menus after each selection.
            const auto child=GetSubMenu(popup,outlinePosition);
            const auto& settings=app->antialiasing;
            const bool editable=settings.enabled && settings.algorithm>=antialias::Algorithm::Fxaa &&
                settings.algorithm<antialias::Algorithm::Count;
            Check(((GetMenuState(popup,antialias::kOutlineCommand,MF_BYCOMMAND)&(MF_DISABLED|MF_GRAYED))==0)==editable,
                "outline strategy submenu is enabled only for active post-process AA");
            Check(GetMenuDefaultItem(child,FALSE,0)==UINT(-1),"outline submenu has no implicit default action");
            for(const auto mode:{antialias::OutlineMode::FxaaAdaptive,antialias::OutlineMode::Multisample}) {
                const unsigned command=mode==antialias::OutlineMode::FxaaAdaptive?
                    antialias::kOutlineFxaaCommand:antialias::kOutlineMsaaCommand;
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_STATE|MIIM_FTYPE;
                Check(GetMenuItemInfoW(child,command,FALSE,&item)!=FALSE,"outline strategy item exists");
                Check((item.fType&MFT_RADIOCHECK)!=0 && ((item.fState&MFS_CHECKED)!=0)==(settings.outlineMode==mode),
                    "exactly the remembered outline strategy has a radio check even while disabled");
                Check(((item.fState&(MFS_DISABLED|MFS_GRAYED))==0)==editable,
                    "outline child availability follows post-process AA activation");
            }
        };
        const auto checkSmaaSelection=[&](antialias::Algorithm active) {
            // Selecting a variant refreshes the native submenu, so do not keep
            // the old handle between command dispatches.
            const auto child=GetSubMenu(popup,smaaPosition);
            const bool enabled=app->antialiasing.enabled;
            Check(((GetMenuState(parent,277,MF_BYCOMMAND)&MF_CHECKED)!=0)==enabled,"main item mirrors enabled state");
            Check(((GetMenuState(popup,antialias::kOffCommand,MF_BYCOMMAND)&MF_CHECKED)!=0)==!enabled,
                "Off is marked exactly when the main item is disabled");
            const bool smaaActive=enabled && active>=antialias::Algorithm::Smaa && active<antialias::Algorithm::Count;
            Check(((GetMenuState(popup,antialias::kSmaaCommand,MF_BYCOMMAND)&MF_CHECKED)!=0)==smaaActive,
                "SMAA parent is marked only when a SMAA variant is selected");
            Check(GetMenuDefaultItem(child,FALSE,0)==UINT(-1),"SMAA submenu has no default choice");
            for(const auto variant:{antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                                   antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced}) {
                Check(((GetMenuState(child,antialias::kFirstCommand+unsigned(variant),MF_BYCOMMAND)&MF_CHECKED)!=0)==(enabled && active==variant),
                    "SMAA child marks the current algorithm instead of the remembered variant");
            }
            for(const auto other:{antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa,antialias::Algorithm::Taa}) {
                Check(((GetMenuState(popup,antialias::kFirstCommand+unsigned(other),MF_BYCOMMAND)&MF_CHECKED)!=0)==(enabled && active==other),
                    "the algorithm submenu marks only the selected family");
            }
            checkOutlineSelection();
        };
        checkSmaaSelection(antialias::Algorithm::Fxaa);
        for(const auto mode:{antialias::OutlineMode::FxaaAdaptive,antialias::OutlineMode::Multisample}) {
            const auto command=mode==antialias::OutlineMode::FxaaAdaptive?
                antialias::kOutlineFxaaCommand:antialias::kOutlineMsaaCommand;
            SendMessageW(app->MainWindow(),WM_COMMAND,command,0);
            Check(app->antialiasing.outlineMode==mode && app->antialiasing.enabled &&
                app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
                "choosing an outline strategy preserves the selected AA algorithm and activation");
            checkOutlineSelection();
        }
        Check((GetMenuState(popup,antialias::kFirstCommand+unsigned(antialias::Algorithm::Fxaa),MF_BYCOMMAND)&MF_CHECKED)!=0,
            "selected algorithm has radio check");
        SendMessageW(app->MainWindow(),WM_MENUSELECT,MAKEWPARAM(smaaPosition,MF_POPUP),reinterpret_cast<LPARAM>(popup));
        MSG smaaEnter{};smaaEnter.message=WM_KEYDOWN;smaaEnter.wParam=VK_RETURN;
        Check(antialias::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&smaaEnter))==0,
            "Enter on the SMAA label is left to native submenu navigation");
        MSG smaaPending{};
        Check(!PeekMessageW(&smaaPending,app->MainWindow(),WM_COMMAND,WM_COMMAND,PM_REMOVE),
            "opening the SMAA submenu does not post a selection command");
        Check(app->antialiasing.algorithm==antialias::Algorithm::Fxaa,
            "opening the SMAA submenu preserves the current algorithm");
        SendMessageW(app->MainWindow(),WM_MENUSELECT,MAKEWPARAM(outlinePosition,MF_POPUP),reinterpret_cast<LPARAM>(popup));
        Check(antialias::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&smaaEnter))==0,
            "Enter on the outline label is left to native submenu navigation");
        Check(!PeekMessageW(&smaaPending,app->MainWindow(),WM_COMMAND,WM_COMMAND,PM_REMOVE),
            "opening the outline submenu posts neither an AA toggle nor a strategy command");
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineCommand,0);
        Check(app->antialiasing.enabled && app->antialiasing.algorithm==antialias::Algorithm::Fxaa &&
            app->antialiasing.outlineMode==antialias::OutlineMode::Multisample,
            "a stale outline parent command changes neither activation, algorithm nor strategy");
        for(const auto active:{antialias::Algorithm::SmaaStandard,antialias::Algorithm::Smaa,
                               antialias::Algorithm::SmaaT2xStandard,antialias::Algorithm::SmaaT2xEnhanced,
                               antialias::Algorithm::Taa,antialias::Algorithm::MmdOriginal,antialias::Algorithm::Fxaa}) {
            select(active);checkSmaaSelection(active);
            Check(app->antialiasing.outlineMode==antialias::OutlineMode::Multisample,
                "changing the AA algorithm retains the independent outline strategy");
            CommandDispatch(nullptr,MAKEWPARAM(277,0));
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active,"main Off remembers the selected algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active &&
                app->antialiasing.outlineMode==antialias::OutlineMode::Multisample,
                "a disabled outline command cannot enable AA or change its remembered strategy");
            CommandDispatch(nullptr,MAKEWPARAM(277,0));
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==active,"main On restores the selected algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOffCommand,0);
            Check(!app->antialiasing.enabled && app->antialiasing.algorithm==active,"submenu Off disables without forgetting the algorithm");
            checkSmaaSelection(active);
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOffCommand,0);
            Check(!app->antialiasing.enabled,"selecting Off twice remains disabled");
            select(active);
            Check(app->antialiasing.enabled,"selecting an algorithm turns AA back on");
            checkSmaaSelection(active);
        }
        for(const auto active:{antialias::Algorithm::MmdOriginal,antialias::Algorithm::Msaa2,
                               antialias::Algorithm::Msaa4,antialias::Algorithm::Msaa8}) {
            select(active);checkOutlineSelection();
            SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
            Check(app->antialiasing.enabled && app->antialiasing.algorithm==active &&
                app->antialiasing.outlineMode==antialias::OutlineMode::Multisample,
                "original and explicit MSAA ignore unavailable outline strategy commands");
        }
        select(antialias::Algorithm::Fxaa);
        SendMessageW(app->MainWindow(),WM_COMMAND,antialias::kOutlineFxaaCommand,0);
        checkOutlineSelection();
        SendMessageW(app->MainWindow(),WM_MENUSELECT,MAKEWPARAM(index,MF_POPUP),reinterpret_cast<LPARAM>(parent));
        MSG enter{};enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        Check(antialias::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&enter))==1,"Enter on parent keeps toggle action");
        MSG pending{};
        Check(PeekMessageW(&pending,app->MainWindow(),WM_COMMAND,WM_COMMAND,PM_REMOVE) && LOWORD(pending.wParam)==277,
            "split menu posts original AA toggle command");
        SendMessageW(app->MainWindow(),WM_EXITMENULOOP,0,0);
        Check(GetSubMenu(parent,index)==nullptr,"flyout detached on exit without changing original item");
        DispatchMessageW(&pending);Check(!app->antialiasing.enabled,"keyboard activates toggle");
        CommandDispatch(nullptr,MAKEWPARAM(277,0));
    }
    if(std::getenv("MMS_SURFACE_MISSING_DX12")) {
        const auto status=skinning::GetStatus();
        Check(blockedSurfaceDx12>0 && status.gpuUpdates==0 && status.cpuUpdates>0,
            "all AA algorithms work after actual missing-DX12 fallback");
    }
    MH_DisableHook(vtable[17]);
}
