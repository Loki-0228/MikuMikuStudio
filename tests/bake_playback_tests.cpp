// Hidden production-host diagnostic: <fixture.pmx> <keyframes|physics>.
// Simulates 60 Hz over a dense 0..500 bake and the sparse remainder to 3845.
// Timings are per rendered frame, including an explicit GPU completion wait;
// they are not wall-clock playback FPS. No user window or pointer is moved.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <MinHook.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <vector>
#include <cmath>

using namespace mikumikustudio;
using Clock = std::chrono::steady_clock;
void Check(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); }
}
LRESULT CALLBACK Hidden(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) { std::printf("Unexpected dialog: %s\n", text); std::exit(2); }
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) { std::fwprintf(stderr,L"Unexpected dialog: %s\n",text); std::exit(2); }
decltype(&ShowWindow) originalShowWindow;
BOOL WINAPI KeepHidden(HWND window, int) { return originalShowWindow(window, SW_HIDE); }
struct Counter { double ms=0; unsigned calls=0; } counters[4];
template<class F, class... Args> void Timed(int id, F f, Args... args) {
    auto t=Clock::now(); f(args...);
    counters[id].ms+=std::chrono::duration<double,std::milli>(Clock::now()-t).count();
    ++counters[id].calls;
}
decltype(&AdvanceModelKeyframes) advanceOriginal;
decltype(&PlaybackCatchup) catchupOriginal;
decltype(&PhysicsFrame) physicsOriginal;
decltype(&RenderFrameScene) renderOriginal;
void Advance(unsigned char* m,double seconds,int physics) { Timed(0,advanceOriginal,m,seconds,physics); }
void Catchup(MMDApp* a,unsigned char s) { Timed(1,catchupOriginal,a,s); }
void Physics(MMDApp* a,unsigned char s) { Timed(2,physicsOriginal,a,s); }
void Render(MMDApp* a) { Timed(3,renderOriginal,a); }
template<class F> void Hook(F original,F replacement,F* trampoline) {
    Check(MH_CreateHook(reinterpret_cast<void*>(original),reinterpret_cast<void*>(replacement),
        reinterpret_cast<void**>(trampoline))==MH_OK,"install profiling/hidden-window hook");
    Check(MH_EnableHook(reinterpret_cast<void*>(original))==MH_OK,"enable hook");
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
        Check(msg.message!=WM_QUIT,"host is alive");
        if(msg.message==WM_PAINT) ValidateRect(msg.hwnd,nullptr);
        else { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}
void Profile(MMDApp* app,const char* stage,int start,int mode,bool physicsBaked=false) {
    auto* device=app->Renderer()->device;
    Microsoft::WRL::ComPtr<IDirect3DQuery9> visible;
    Check(SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION,&visible)),"GPU visibility query");
    unsigned long long visibleSamples=0;
    app->state.optflag[0]=static_cast<unsigned char>(mode);
    app->FpsLimit()=60; app->MilliToSec()=.001f;
    app->PlaybackStartsAtCurrentFrame()=0;
    app->TimeNowLow()=100000; app->TimeNowHigh()=0;
    SetWindowTextW(GetDlgItem(app->MainWindow(),panel::kPlayStartFrameEdit),std::to_wstring(start).c_str());
    SetWindowTextW(GetDlgItem(app->MainWindow(),panel::kPlayStopFrameEdit),L"3845");
    CommandDispatch(GetDlgItem(app->MainWindow(),panel::kPlayButton),MAKEWPARAM(408,BN_CLICKED));
    Check(app->PlaybackActive(),"start through production playback command");
    double total=0,gpu=0,pump=0,boundary=0,boundaryMax=0; unsigned samples=0,boundarySamples=0;
    int firstPhysicsFlag=-1,lastPhysicsFlag=-1,probeBone=-1;
    const auto& model=*mdl::Mdl(app->SelectedModel());
    for(unsigned i=0;i<model.rigidCount&&probeBone<0;++i) {
        const auto& rigid=model.rigidTable[i];
        if(!rigid.staticFlag&&rigid.body&&rigid.boneIndex>=0&&unsigned(rigid.boneIndex)<model.boneCount)
            probeBone=rigid.boneIndex;
    }
    std::vector<double> durations;
    unsigned long long poseHash=14695981039346656037ull;
    for(int i=0;i<1000;++i) {
        if(i==30) for(auto& c:counters)c={};
        auto begin=Clock::now(); Pump(); auto pumped=Clock::now();
        app->TimeNowLow()=100000+static_cast<DWORD>(i*1000/60);
        app->DeltaTime()=1.f/60; app->state.messageSeen=10;
        // Bake completion and model/camera panel changes can resize the host's
        // viewport. Keep drawing area and aspect equal across every comparison.
        app->ViewportRect()={0,0,960,540};
        app->Renderer()->aspectRatio=960.f/540;
        D3DVIEWPORT9 viewport{0,0,960,540,0,1};
        Check(SUCCEEDED(device->SetViewport(&viewport)),"fixed comparison viewport");
        Check(SUCCEEDED(visible->Issue(D3DISSUE_BEGIN)),"begin GPU visibility query");
        FrameDriver(app);
        D3DVIEWPORT9 actual{}; device->GetViewport(&actual);
        Check(actual.Width==960&&actual.Height==540,"viewport unchanged by playback");
        const auto submitted=Clock::now();
        Check(SUCCEEDED(visible->Issue(D3DISSUE_END)),"issue GPU completion fence");
        HRESULT status; DWORD pixels=0;
        while((status=visible->GetData(&pixels,sizeof(pixels),D3DGETDATA_FLUSH))==S_FALSE) {
            Check(Clock::now()-submitted<std::chrono::seconds(10),"GPU query completes within timeout");
            SwitchToThread();
        }
        Check(status==S_OK,"GPU finished without device loss");
        const auto done=Clock::now();
        if(i>=30) {
            Check(pixels>0,"frame contains GPU rasterized geometry"); visibleSamples+=pixels;
            const double elapsed=std::chrono::duration<double,std::milli>(done-begin).count();
            const float frame=app->PlaybackCursorSeconds()*30;
            if(frame>=495&&frame<=505) { boundary+=elapsed; boundaryMax=std::max(boundaryMax,elapsed); ++boundarySamples; }
            const auto& bone=mdl::Mdl(app->SelectedModel())->boneTable[probeBone>=0?probeBone:0];
            for(float v:bone.trans) Check(std::isfinite(v),"playback pose is finite");
            lastPhysicsFlag=bone.physicsDisabled;
            if(firstPhysicsFlag<0) firstPhysicsFlag=lastPhysicsFlag;
            durations.push_back(elapsed); total+=elapsed;
            gpu+=std::chrono::duration<double,std::milli>(done-submitted).count();
            pump+=std::chrono::duration<double,std::milli>(pumped-begin).count(); ++samples;
            // Outside the measured interval: detect changed playback poses in
            // paired builds without including hashing in frame timings.
            for(unsigned b=0;b<model.boneCount;++b) {
                const auto* bytes=reinterpret_cast<const unsigned char*>(model.boneTable[b].matInit);
                for(unsigned j=0;j<sizeof(model.boneTable[b].matInit);++j) {poseHash^=bytes[j];poseHash*=1099511628211ull;}
            }
        }
        Check(!IsWindowVisible(app->MainWindow()),"host remains hidden");
        if(i<30) Sleep(2);
    }
    const float last=app->PlaybackCursorSeconds()*30;
    Check(last>start+499&&last<start+500,"playback advanced across the requested interval");
    std::sort(durations.begin(),durations.end());
    std::printf("PROFILE stage=%s mode=%d range=%d..%.2f frames=%u total_ms=%.6f p95_ms=%.6f keys_ms=%.6f catchup_ms=%.6f physics_ms=%.6f render_ms=%.6f gpu_wait_ms=%.6f messages_ms=%.6f key_calls=%u gpu_visible_samples=%llu pose_hash=%016llx\n",
        stage,mode,start,last,samples,total/samples,durations[durations.size()*95/100],
        counters[0].ms/samples,counters[1].ms/samples,counters[2].ms/samples,counters[3].ms/samples,
        gpu/samples,pump/samples,counters[0].calls,visibleSamples,poseHash);
    if(boundarySamples) std::printf("BOUNDARY stage=%s mode=%d frames=495..505 mean_ms=%.6f max_ms=%.6f samples=%u physics_disabled=%d->%d\n",
        stage,mode,boundary/boundarySamples,boundaryMax,boundarySamples,firstPhysicsFlag,lastPhysicsFlag);
    if(physicsBaked&&probeBone>=0&&start==250&&app->PlaybackPhysicsMode()==2)
        Check(firstPhysicsFlag==1&&lastPhysicsFlag==0,"physical simulation resumes outside the baked interval");
    Check(SUCCEEDED(device->TestCooperativeLevel()),"render device remained available");
    app->PlaybackActive()=0; StopPlayback(app); Pump();
}
int wmain(int argc,wchar_t** argv) {
    Check(argc>=3&&argc<=5,"fixture PMX, bake type, optional playback physics mode and scale required"); std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto source=std::filesystem::absolute(argv[1]);
    const bool physics=std::wcscmp(argv[2],L"physics")==0;
    Check(physics||std::wcscmp(argv[2],L"keyframes")==0,"known bake type");
    Check(MH_Initialize()==MH_OK,"initialize hidden host guards");
    Hook(&ShowWindow,&KeepHidden,&originalShowWindow);
    decltype(&MessageBoxA) boxA; decltype(&MessageBoxW) boxW;
    Hook(&MessageBoxA,&UnexpectedA,&boxA); Hook(&MessageBoxW,&UnexpectedW,&boxW);
    auto hide=SetWindowsHookExW(WH_CBT,Hidden,nullptr,GetCurrentThreadId());
    auto app=std::make_unique<MMDApp>(); g_Block=app.get(); app->state=MMDAppState{}; app->InitDefaults(); app->EnglishUI()=1;
    Check(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize production host");
    Check(!GetModuleHandleW(L"MMEffect.dll")&&!GetModuleHandleW(L"MMV.dll"),"native diagnostic has no third-party renderer loaded");
    LoadModelFile(app.get(),source.c_str(),false);
    app->SelectedModelSlot()=0;
    auto* raw=app->SelectedModel(); Check(raw!=nullptr,"load diagnostic fixture"); auto& m=*mdl::Mdl(raw);
    // Sparse original tracks, keeping a genuine endpoint after the bake range.
    for(unsigned b=0;b<m.boneCount;++b) {
        auto& head=m.boneKeys[b]; head.next=b+m.boneCount;
        auto& tail=m.boneKeys[head.next]; tail=head; tail.previous=b; tail.next=0; tail.frame=3845; tail.allocated=1;
        tail.position[0]=2.f;
    }
    m.maxFrame=3845; app->state.lastRegisteredFrame=3845;
    // Mode 1 forces physics even on disabled bone keys. Mode 2 respects the
    // per-key physics flags that a physics bake writes over its frame range.
    app->PlaybackPhysicsMode()=argc>=4?_wtoi(argv[3]):(physics?2:0);
    Check(app->PlaybackPhysicsMode()>=0&&app->PlaybackPhysicsMode()<=2,"valid playback physics mode");
    if(argc==5) {
        Check(std::wcscmp(argv[4],L"scale")==0,"known scale diagnostic");
        bone_scale::Matrix scale;scale.v[0]=1.1f;
        bone_scale::SetKey(raw,0,scale);
    }
    EnableD3D12Native(false);
    app->DeltaTime()=1.f/60; app->state.messageSeen=10; FrameDriver(app.get()); Pump();
    std::printf("SCENE bones=%u rigid=%u joints=%u vertices=%u physics=%d\n",m.boneCount,m.rigidCount,m.jointCount,m.vertexCount,physics);
    Hook(&AdvanceModelKeyframes,&Advance,&advanceOriginal); Hook(&PlaybackCatchup,&Catchup,&catchupOriginal);
    Hook(&PhysicsFrame,&Physics,&physicsOriginal); Hook(&RenderFrameScene,&Render,&renderOriginal);
    const int modeCount=std::getenv("MMS_BAKE_PROFILE_ONE_MODE")?1:2;
    for(int mode=0;mode<modeCount;++mode) for(int start:{0,501}) Profile(app.get(),"original",start,mode);
    bake::Request request; request.firstFrame=0; request.lastFrame=500;
    request.physics=physics; request.keyframes=!physics; request.settings=bake::BulletDefaults();
    auto result=bake::Compute(*app,request);
    std::printf("BAKE error=%d bones=%d keys=%zu steps=%llu scales=%zu\n",int(result.error),result.boneCount,result.changes.size(),result.simulationSteps,result.scales.size());
    Check(result.error==bake::Error::None&&!result.changes.empty(),"compute genuine 0..500 bake");
    Check(bake::Commit(raw,result,0),"commit genuine baked tracks");
    for(int mode=0;mode<modeCount;++mode) for(int start:{501,0,250}) Profile(app.get(),"baked",start,mode,physics);
    ShutdownCleanup(app.get()); g_Block=nullptr; UnhookWindowsHookEx(hide);
    std::puts("PASS production bake and dense/sparse playback diagnostics");
}
