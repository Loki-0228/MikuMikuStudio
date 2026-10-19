// Included by the hidden native/MME host. Timing excludes captures and resize.
#include "mikumikustudio/build_profile.hpp"
#include "viewport_scale_target_checks.hpp"
#include "viewport_scale_gpu_checks.hpp"
namespace scale_cache_verify {
using Update = void(*)(MMDApp*,unsigned char*,const float*);
Update original=nullptr;
unsigned checked=0,hits=0;
std::vector<unsigned char> Read(void* resource) {
    auto* buffer=static_cast<IDirect3DVertexBuffer9*>(resource);
    D3DVERTEXBUFFER_DESC desc{};void* data=nullptr;
    Check(buffer && SUCCEEDED(buffer->GetDesc(&desc)) &&
        SUCCEEDED(buffer->Lock(0,0,&data,D3DLOCK_READONLY)),"read model buffer for cache parity");
    std::vector<unsigned char> result(desc.Size);std::memcpy(result.data(),data,desc.Size);
    buffer->Unlock();return result;
}
void Observe(MMDApp* app,unsigned char* model,const float* world) {
    const auto before=model_vertex_cache::GetStats();
    original(app,model,world);
    const auto after=model_vertex_cache::GetStats();
    hits+=unsigned(after.hits-before.hits+after.outlineHits-before.outlineHits);
    const auto& m=*mdl::Mdl(model);
    const auto body=Read(m.vertexBuffer),edge=Read(m.vertexBuffer2);
    model_vertex_cache::Invalidate(model);
    original(app,model,world);
    Check(body==Read(m.vertexBuffer) && edge==Read(m.vertexBuffer2),
        "real scene cached body and outline equal a fresh update byte for byte");
    ++checked;
}
}
void CheckViewportScalePerformance(MMDApp* app, const wchar_t* output) {
    using Microsoft::WRL::ComPtr;
    const auto clock=[] {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
        return double(now.QuadPart)/frequency.QuadPart;
    };
    const std::filesystem::path directory(output);
    std::filesystem::create_directories(directory);
    const bool capture = std::getenv("MMS_SCALE_CAPTURE") != nullptr;
    const bool projectSettings = std::getenv("MMS_SCALE_PROJECT_SETTINGS") != nullptr;
    const bool gpuTiming = std::getenv("MMS_SCALE_GPU_TIMING") != nullptr;
    if (std::getenv("MMS_SCALE_NO_VERTEX_CACHE")) model_vertex_cache::SetEnabledForTests(false);
    const unsigned gpuSamples = std::getenv("MMS_SCALE_GPU_SAMPLES") ?
        unsigned(std::clamp(std::atoi(std::getenv("MMS_SCALE_GPU_SAMPLES")),4,64)) : 16;
    const double measuredSeconds = std::getenv("MMS_SCALE_SECONDS") ?
        std::clamp(std::atof(std::getenv("MMS_SCALE_SECONDS")), 1.5, 30.) : 1.5;
    if (const auto* accessory = _wgetenv(L"MMS_SCALE_ACCESSORY")) {
        LoadAccessoryFile(accessory);
        Check(app->AccessorySlot(0)!=nullptr,"load scale-test postprocess accessory");
    }
    if (const auto* mapping = _wgetenv(L"MMS_SCALE_EMM")) NotifyEffectFileOpen(mapping);
    // Isolate CPU outline reuse from automatic adapter calibration between runs.
    if (std::getenv("MMS_SCALE_CPU_SKINNING")) skinning::SetPreference({skinning::Mode::Cpu});
    std::printf("SCALE_SKINNING preference=%u original_profile=%d\n",
        unsigned(skinning::GetPreference().mode),int(build_profile::original));
    std::ofstream report(directory/"performance.tsv");
    report<<"enhanced\tadaptive\tblock\tpercent\tframes\tfps\tshadow_size\tfiltered_size\tscene_ms\tpresent_ms\n";
    for (int slot=0;slot<kModelSlotCount;++slot) if(auto* model=app->ModelSlot(slot)) {
        const auto& record=*mdl::Mdl(model);
        const auto& uv=mdl::UvMorphCounts(model).byFamily;
        unsigned weights[5]{};
        if(record.physicsMode==2 && record.pmxVertices)for(unsigned i=0;i<record.vertexCount;++i) {
            const unsigned type=unsigned(record.pmxVertices[i].weightType);
            if(type<5)++weights[type];
        }
        std::printf("SCALE_MODEL slot=%d vertices=%u bones=%u morphs=%u materials=%u base_positions=%u base_uv=%d,%d,%d,%d,%d\n",
            slot,record.vertexCount,record.boneCount,record.morphCount,record.materialCount,
            mdl::BaseVertexMorphCount(model),uv[0],uv[1],uv[2],uv[3],uv[4]);
        std::printf("SCALE_WEIGHTS slot=%d bdef1=%u bdef2=%u bdef4=%u sdef=%u qdef=%u\n",slot,
            weights[0],weights[1],weights[2],weights[3],weights[4]);
    }
    app->FpsLimit()=1000;app->PlaybackPhysicsMode()=0;app->GroundGridEnabled()=0;
    app->EditMode()=ViewportEditMode::Camera;
    std::wstring error;
    if (!projectSettings) {
        app->state.selfShadowEnabled=1;app->SelfShadowMode()=1;app->ShadowResolutionLimit()=4096;
        Check(antialias::Select(app,antialias::Algorithm::MmdOriginal,error),"fixed AA for scale throughput");
    }
    const bool savedEnhanced=app->ShadowFilterEnhanced(),savedAdaptive=app->AdaptiveShadowResolution();
    auto* device=app->Renderer()->device;
    void* presentAddress=nullptr;
    if(capture) {
        presentAddress=(*reinterpret_cast<void***>(device))[17];
        Check(MH_CreateHook(presentAddress,&ObservePresent,reinterpret_cast<void**>(&originalPresent))==MH_OK &&
            MH_EnableHook(presentAddress)==MH_OK,"capture scale comparison outside timing");
    }
    ComPtr<IDirect3DSurface9> back;
    ComPtr<IDirect3DDevice9> native;
    device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);back->GetDevice(&native);back.Reset();
    ComPtr<IDirect3D9> d3d;D3DDEVICE_CREATION_PARAMETERS creation{};D3DADAPTER_IDENTIFIER9 adapter{};
    Check(SUCCEEDED(native->GetDirect3D(&d3d)) && SUCCEEDED(native->GetCreationParameters(&creation)) &&
        SUCCEEDED(d3d->GetAdapterIdentifier(creation.AdapterOrdinal,0,&adapter)),"identify benchmark adapter");
    std::printf("SCALE_ADAPTER name=%s vendor=%04lx device=%04lx type=%u behavior=%08lx\n",
        adapter.Description,adapter.VendorId,adapter.DeviceId,unsigned(creation.DeviceType),creation.BehaviorFlags);
    if(gpuTiming)scale_gpu::Initialize(native.Get(),directory,gpuSamples);
    ComPtr<IDirect3DQuery9> drain;
    Check(SUCCEEDED(native->CreateQuery(D3DQUERYTYPE_EVENT,&drain)),"GPU completion query");
    const auto flush=[&] {
        Check(SUCCEEDED(drain->Issue(D3DISSUE_END)),"submit GPU completion");
        const auto start=clock();HRESULT hr=S_FALSE;
        while((hr=drain->GetData(nullptr,0,D3DGETDATA_FLUSH))==S_FALSE && clock()-start<10)Sleep(0);
        Check(hr==S_OK,"finish queued GPU work before timing");
    };
    double sceneTime=0,presentTime=0;
    bool timing=false;
    unsigned presented=0;
    const bool cameraMotion=std::getenv("MMS_SCALE_CAMERA_MOTION")!=nullptr;
    model_vertex_cache::SetOutlineEnabledForTests(std::getenv("MMS_SCALE_NO_OUTLINE_CACHE")==nullptr);
    const float originalDistance=app->CameraDistance(),originalYaw=app->CameraYaw();
    unsigned motionFrame=0;
    const auto frame=[&] {
        app->DeltaTime()=0;app->state.messageSeen=10;
        if(cameraMotion) {
            const float phase=float(motionFrame++%120)*.05235987756f;
            app->CameraDistance()=originalDistance+std::sin(phase);
            app->CameraYaw()=originalYaw+.03f*std::sin(phase);
        }
        const double begin=timing?clock():0;
        RenderFrameScene(app);
        scale_gpu::Rendered();
        const double rendered=timing?clock():0;
        const RECT rect=FramePresentationRect(app);
        const double gpuPresentStart=gpuTiming?clock():0;
        const HRESULT hr=device->Present(&rect,&rect,app->MainWindow(),nullptr);
        if(timing && SUCCEEDED(hr))++presented;
        if(gpuTiming)scale_gpu::presentMs=(clock()-gpuPresentStart)*1000;
        if(timing){sceneTime+=rendered-begin;presentTime+=clock()-rendered;}
        frame_rate::Presented(app,SUCCEEDED(hr));
        if(FAILED(hr))Check(false,"benchmark frame presented");
    };
    for(bool enhanced:{false,true})for(bool adaptive:{false,true}) {
        if(projectSettings ? (enhanced!=savedEnhanced || adaptive!=savedAdaptive) :
           (std::getenv("MMS_SCALE_QUICK") && (enhanced || adaptive)))continue;
        app->ShadowFilterEnhanced()=enhanced;app->AdaptiveShadowResolution()=adaptive;
        unsigned fullShadow=0,fullFiltered=0;
        for(int block=0;block<4;++block) {
            motionFrame=0;
            const int percent=!build_profile::original && (block==1||block==2)?25:100;
            Check(viewport_render_scale::Configure(app,false,percent,error),"configure scale throughput");
            // A ray effect's first frame can compile shaders for longer than
            // the time budget. Still complete enough frames to warm the cache.
            unsigned warmFrames=0;const double warm=clock();
            do{frame();++warmFrames;}while(warmFrames<8 || clock()-warm<.5);
            std::printf("SCALE_WARM block=%d frames=%u seconds=%.3f\n",block,warmFrames,clock()-warm);
            flush();
            if (std::getenv("MMS_SCALE_VERIFY_VERTEX_CACHE")) {
                const auto before=scale_cache_verify::hits;
                Check(MH_CreateHook(&UpdateModelVertexBuffers,&scale_cache_verify::Observe,
                    reinterpret_cast<void**>(&scale_cache_verify::original))==MH_OK &&
                    MH_EnableHook(&UpdateModelVertexBuffers)==MH_OK,"observe real model cache parity outside timing");
                frame();frame();
                MH_RemoveHook(&UpdateModelVertexBuffers);
                Check(scale_cache_verify::hits>before,"real scene exercises reused model buffers");
                std::printf("SCALE_CACHE_PARITY block=%d percent=%d checked=%u hits=%u\n",block,percent,
                    scale_cache_verify::checked,scale_cache_verify::hits);
                flush();
            }
            if (std::getenv("MMS_SCALE_TRACE_TARGETS")) {
                scale_targets::Install(native.Get());
                scale_targets::rows.clear();scale_targets::observing=true;
                frame();
                scale_targets::observing=false;scale_targets::Remove(native.Get());
                scale_targets::Report(percent);flush();
            }
            if(gpuTiming) {
                scale_gpu::Install(device,native.Get(),false);
                for(unsigned sample=0;sample<gpuSamples*4;++sample) {
                    scale_gpu::BeginFrame(false);frame();scale_gpu::ReportCpu(block,percent,sample);
                }
                scale_gpu::Remove(device,native.Get(),false);
                flush();
                scale_gpu::Install(device,native.Get(),scale_gpu::detailed);
                unsigned accepted=0;
                for(unsigned sample=0;sample<gpuSamples;++sample) {
                    if(!scale_gpu::detailed)scale_gpu::SwapSnapshot(sample);
                    scale_gpu::BeginFrame();frame();
                    if(scale_gpu::detailed) {
                        if(scale_gpu::Report(block,percent,sample))++accepted;
                    } else scale_gpu::SwapSnapshot(sample);
                }
                // Coarse samples keep normal CPU/GPU overlap. Read all query
                // results only after the batch, not after every frame.
                if(!scale_gpu::detailed) {
                    flush();
                    for(unsigned sample=0;sample<gpuSamples;++sample) {
                        scale_gpu::SwapSnapshot(sample);
                        if(scale_gpu::Report(block,percent,sample))++accepted;
                        scale_gpu::SwapSnapshot(sample);
                    }
                }
                scale_gpu::Remove(device,native.Get(),scale_gpu::detailed);
                Check(accepted>=gpuSamples*3/4,"valid GPU timing samples");
                std::printf("SCALE_GPU_SAMPLES block=%d percent=%d accepted=%u requested=%u\n",block,percent,accepted,gpuSamples);
                // Query collection serializes diagnostic frames. Warm up the
                // ordinary pipeline again before measuring uninstrumented FPS.
                const double warmed=clock();do{frame();}while(clock()-warmed<.5);
                flush();
            }
            frame_rate::Reset(app);
#ifdef MIKUMIKUSTUDIO_PERF
            perf::Reset();
#endif
            const auto cacheBefore=model_vertex_cache::GetStats();
            const auto watchBefore=effect_file_watch::GetStats();
            sceneTime=presentTime=0;presented=0;timing=true;
            unsigned frames=0;const double start=clock();
            do {frame();++frames;}while(clock()-start<measuredSeconds);
            timing=false;
            flush();const double elapsed=clock()-start;
            const auto cacheAfter=model_vertex_cache::GetStats();
            const auto watchAfter=effect_file_watch::GetStats();
            std::printf("SCALE_FILE_WATCH block=%d reads=%llu hits=%llu notifications=%llu files=%u directories=%u\n",
                block, static_cast<unsigned long long>(watchAfter.reads-watchBefore.reads),
                static_cast<unsigned long long>(watchAfter.hits-watchBefore.hits),
                static_cast<unsigned long long>(watchAfter.notifications-watchBefore.notifications),
                watchAfter.files, watchAfter.directories);
            std::printf("SCALE_CACHE block=%d percent=%d hits=%llu misses=%llu stores=%llu bytes=%zu outline_hits=%llu cpu_bytes=%zu\n",block,percent,
                static_cast<unsigned long long>(cacheAfter.hits-cacheBefore.hits),
                static_cast<unsigned long long>(cacheAfter.misses-cacheBefore.misses),
                static_cast<unsigned long long>(cacheAfter.stores-cacheBefore.stores),cacheAfter.bytes,
                static_cast<unsigned long long>(cacheAfter.outlineHits-cacheBefore.outlineHits),cacheAfter.cpuBytes);
#ifdef MIKUMIKUSTUDIO_PERF
            perf::Report(frames);
#endif
            Check(presented==frames && (build_profile::original || app->frameRate.presentedFrames==frames),
                "throughput counts completed scene presents");
            D3DSURFACE_DESC coarse{},filtered{};
            auto* renderer=app->Renderer();
            Check(renderer->hdrTexture && SUCCEEDED(renderer->hdrTexture->GetLevelDesc(0,&coarse)),"actual shadow dimensions");
            auto* visible=shadow_filter::VisibleTexture(app);
            Check(visible && SUCCEEDED(visible->GetLevelDesc(0,&filtered)),"actual shared shadow dimensions");
            if(block==0){fullShadow=coarse.Width;fullFiltered=filtered.Width;}
            if(std::getenv("MMS_SCALE_EXPECT_SHADOW_BUDGET")) {
                if(percent==25) {
                    Check(coarse.Width<=std::max(512u,fullShadow/(adaptive?2:4)),"low preview reduces shadow rasterization");
                    Check(filtered.Width<=std::max(512u,fullFiltered/(adaptive?2:4)),"low preview reduces enhanced depth filtering");
                } else if(block==3) {
                    Check(coarse.Width==fullShadow && filtered.Width==fullFiltered,"100 percent restores shadow quality");
                }
            }
            std::printf("SCALE_PERF enhanced=%d adaptive=%d block=%d percent=%d fps=%.3f shadow=%u filtered=%u scene_ms=%.3f present_ms=%.3f\n",
                enhanced,adaptive,block,percent,frames/elapsed,coarse.Width,filtered.Width,sceneTime*1000/frames,presentTime*1000/frames);
            report<<enhanced<<'\t'<<adaptive<<'\t'<<block<<'\t'<<percent<<'\t'<<frames<<'\t'
                <<frames/elapsed<<'\t'<<coarse.Width<<'\t'<<filtered.Width<<'\t'<<sceneTime*1000/frames<<'\t'<<presentTime*1000/frames<<'\n';report.flush();
            if(capture) {
                captureRequested=true;frame();
                Check(!captureRequested && !capturedPixels.empty(),"capture complete scale comparison frame");
                const auto name="frame-"+std::to_string(int(enhanced))+"-"+std::to_string(int(adaptive))+"-"+std::to_string(block);
                std::ofstream pixels(directory/(name+".bgra"),std::ios::binary);
                pixels.write(reinterpret_cast<const char*>(capturedPixels.data()),capturedPixels.size()*4);
                std::ofstream dimensions(directory/(name+".txt"));dimensions<<captureWidth<<' '<<captureHeight;
            }
        }
    }
    if(presentAddress)MH_RemoveHook(presentAddress);
    if(gpuTiming)scale_gpu::Shutdown();
    app->CameraDistance()=originalDistance;app->CameraYaw()=originalYaw;
}
