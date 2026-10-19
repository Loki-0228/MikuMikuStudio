#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include <btBulletDynamicsCommon.h>
#include <shellapi.h>
#include <shlobj.h>
#include "mikumikustudio/transparent_capture.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <memory>
#include <MinHook.h>
namespace mikumikustudio { void DispatchLetterHotkeys(MMDApp*,HWND); }
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
LRESULT CALLBACK Hidden(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) { ShowWindow(reinterpret_cast<HWND>(wp), SW_HIDE); return 1; }
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) { std::printf("Unexpected dialog: %s\n", text); std::exit(2); }
bool importingTestMotion = false;
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT flags) {
    if (importingTestMotion && flags == (MB_OKCANCEL | MB_ICONWARNING) &&
        std::wcsstr(text, L"Only matching bone names will be imported.")) return IDOK;
    std::fwprintf(stderr, L"Unexpected dialog: %s\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShowWindow = nullptr;
BOOL WINAPI KeepHidden(HWND window, int) { return originalShowWindow(window, SW_HIDE); }
#include "window_cursor_checks.hpp"
template<class T> void Put(std::ofstream& f, T value) { f.write(reinterpret_cast<char*>(&value), sizeof(value)); }
void Text(std::ofstream& f, const wchar_t* text) { Put(f, std::uint32_t(wcslen(text) * 2)); f.write(reinterpret_cast<const char*>(text), wcslen(text) * 2); }
void Fixture(const fs::path& path) {
    std::ofstream f(path, std::ios::binary); f.write("PMX ", 4); Put(f, 2.f);
    for (std::uint8_t v : {8,0,0,1,1,1,1,1,1}) Put(f,v);
    Text(f,L"Alpha triangle"); Text(f,L"Alpha triangle"); Text(f,L""); Text(f,L""); Put(f,3u);
    const float xy[][2] = {{-5,5},{5,5},{0,15}};
    for(int i=0;i<3;++i) {
        Put(f,xy[i][0]); Put(f,xy[i][1]); Put(f,0.f);
        Put(f,0.f); Put(f,0.f); Put(f,-1.f); Put(f,0.f); Put(f,0.f);
        Put(f,std::uint8_t(0)); Put(f,std::int8_t(0)); Put(f,1.f);
    }
    Put(f,3u); Put(f,std::uint8_t(0)); Put(f,std::uint8_t(1)); Put(f,std::uint8_t(2)); Put(f,0u);
    Put(f,1u); Text(f,L"Half transparent"); Text(f,L"Half transparent");
    for(float v:{1.f,0.f,0.f,.5f,0.f,0.f,0.f,1.f,.4f,.4f,.4f}) Put(f,v);
    Put(f,std::uint8_t(1)); for(int i=0;i<5;++i) Put(f,0.f);
    Put(f,std::int8_t(-1)); Put(f,std::int8_t(-1)); Put(f,std::uint8_t(0)); Put(f,std::uint8_t(1)); Put(f,std::uint8_t(0)); Text(f,L""); Put(f,3u);
    Put(f,1u); Text(f,L"root"); Text(f,L"root"); for(int i=0;i<3;++i) Put(f,0.f);
    Put(f,std::int8_t(-1)); Put(f,0u); Put(f,std::uint16_t(0x1a)); for(int i=0;i<3;++i) Put(f,0.f);
    Put(f,0u); Put(f,1u); Text(f,L"Root"); Text(f,L"Root"); Put(f,std::uint8_t(1)); Put(f,1u); Put(f,std::uint8_t(0)); Put(f,std::int8_t(0));
    Put(f,0u); Put(f,0u); Check(bool(f),"write alpha PMX fixture");
}
std::vector<unsigned char> Read(IDirect3DDevice9* device, IDirect3DSurface9* source) {
    D3DSURFACE_DESC desc{}; Check(source && SUCCEEDED(source->GetDesc(&desc)), "source target");
    ComPtr<IDirect3DSurface9> resolve, system;
    Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, nullptr)), "resolve surface");
    Check(SUCCEEDED(device->StretchRect(source, nullptr, resolve.Get(), nullptr, D3DTEXF_NONE)), "resolve alpha target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system, nullptr)), "readback surface");
    Check(SUCCEEDED(device->GetRenderTargetData(resolve.Get(), system.Get())), "GPU alpha readback");
    D3DLOCKED_RECT locked{}; Check(SUCCEEDED(system->LockRect(&locked,nullptr,D3DLOCK_READONLY)), "lock target");
    std::vector<unsigned char> pixels(std::size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y) std::memcpy(pixels.data()+std::size_t(y)*desc.Width*4,static_cast<unsigned char*>(locked.pBits)+std::size_t(y)*locked.Pitch,desc.Width*4);
    system->UnlockRect(); return pixels;
}
void Blocks(IDirect3DDevice9* device, IDirect3DSurface9* target) {
    ComPtr<IDirect3DSurface9> original; device->GetRenderTarget(0,&original);
    D3DVIEWPORT9 old{}; device->GetViewport(&old);
    Check(SUCCEEDED(device->SetRenderTarget(0,target)),"bind capture for pixel fixture");
    device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
    D3DVIEWPORT9 viewport{0,0,64,48,0,1}; device->SetViewport(&viewport);
    device->SetVertexShader(nullptr); device->SetPixelShader(nullptr); device->SetTexture(0,nullptr);
    device->SetRenderState(D3DRS_ZENABLE,FALSE); device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device->SetRenderState(D3DRS_LIGHTING,FALSE); device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE); device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD); device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    SetCoverageAlphaBlend(device);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1); device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1); device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    struct Vertex { float x,y,z,w; DWORD color; };
    Check(SUCCEEDED(device->BeginScene()),"begin pixel fixture");
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        const DWORD color = D3DCOLOR_ARGB(x ? 255 : 128,y ? 0 : 255,0,y ? 255 : 0);
        const float left=(x ? 40.f : 16.f)-.5f,right=(x ? 64.f : 40.f)-.5f,top=y*24.f-.5f,bottom=top+24;
        Vertex vertices[]={{left,top,0,1,color},{right,top,0,1,color},{left,bottom,0,1,color},{right,bottom,0,1,color}};
        Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex))),"draw alpha fixture");
    }
    device->EndScene(); device->SetRenderTarget(0,original.Get()); device->SetViewport(&old);
}
void CheckVideoPlayback(MMDApp* app, const fs::path& video, const fs::path& output) {
    app->state.selfShadowEnabled = 1; app->SelfShadowMode() = 2;
    auto* raw = app->ModelSlot(0);
    auto& model = *mdl::Mdl(raw);
    if (model.boneCount == 1) {
    auto& head = model.boneKeys[0];
    head.next = 1;
    auto& tail = model.boneKeys[1];
    tail = head; tail.previous = 0; tail.next = 0; tail.frame = 120; tail.allocated = 1;
    tail.position[0] = 12;
    model.maxFrame = 120;
    app->state.lastRegisteredFrame = 120;
    }
    auto* world = app->Physics()->world;
    Check(world != nullptr, "production physics world exists");
    btSphereShape shape(.1f);
    btVector3 inertia; shape.calculateLocalInertia(1, inertia);
    btRigidBody::btRigidBodyConstructionInfo info(1, nullptr, &shape, inertia);
    btRigidBody body(info); world->addRigidBody(&body);
    for (bool visible : {false, true}) {
        app->AviBackgroundEnabled() = 0;
        if (visible) {
            const auto path = video.wstring();
            const auto bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
            HGLOBAL drop = GlobalAlloc(GHND, bytes);
            auto* files = static_cast<DROPFILES*>(GlobalLock(drop));
            files->pFiles = sizeof(DROPFILES); files->fWide = TRUE;
            std::memcpy(reinterpret_cast<char*>(files) + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(drop);
            SendMessageW(app->MainWindow(), WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0);
            Check(app->AviBackgroundEnabled() == 1, "video imported through drop handler");
        }
        auto transform = btTransform::getIdentity(); transform.setOrigin(btVector3(0, 100, 0));
        body.setWorldTransform(transform); body.setLinearVelocity(btVector3(0,0,0)); body.activate(true);
        app->state.optflag[0] = 0; app->SelectedModelSlot() = 0;
        app->PlaybackPhysicsMode() = 2; app->FrameStepPlayback() = 0;
        app->PlaybackStartsAtCurrentFrame() = 0; app->PlaybackLoopEnabled() = 0;
        app->TimeNowLow() = 100000; app->TimeNowHigh() = 0; app->MilliToSec() = .001f;
        SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStartFrameEdit), L"0");
        SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStopFrameEdit), L"120");
        CommandDispatch(GetDlgItem(app->MainWindow(), panel::kPlayButton), MAKEWPARAM(408, BN_CLICKED));
        Check(app->PlaybackActive(), "start model playback with background");
        std::vector<float> firstPose(model.boneCount * 16);
        for (int i = 1; i <= 120; ++i) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                Check(message.message != WM_QUIT, "playback host remains alive");
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            app->TimeNowLow() = 100000 + i * 1000 / 60;
            app->DeltaTime() = 1.f / 60;
            FrameDriver(app);
            if (i == 5) {
                // The start command resets key cursors; matrices settle on the
                // next driver pass. Compare two evaluated poses within this run.
                for (unsigned b=0;b<model.boneCount;++b)
                    std::memcpy(firstPose.data()+16*b,model.boneTable[b].matInit,64);
            }
            if (i == 5 || i == 115) {
                std::printf("VIDEO_SAMPLE visible=%d i=%d frame=%.3f edit=%d\n",visible,i,
                    app->PlaybackCursorSeconds()*30,app->CurrentFrame());
                ComPtr<IDirect3DSurface9> surface;
                app->Renderer()->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);
                const auto file = output/(L"video-playback-" + std::to_wstring(visible) + L"-" + std::to_wstring(i) + L".png");
                Check(SUCCEEDED(d3dx::Get().saveSurfaceToFileW(file.c_str(), 3, surface.Get(), nullptr, nullptr)), "save playback display probe");
            }
        }
        std::printf("VIDEO_PLAYBACK visible=%d frame=%.3f bone_x=%.3f matrix_x=%.3f body_y=%.3f gate=%d\n",
            visible, app->PlaybackCursorSeconds()*30, model.boneTable[0].trans[0], model.boneTable[0].matInit[12],
            double(body.getWorldTransform().getOrigin().y()), app->state.messageSeen);
        void* vertexData = nullptr;
        auto* vb = static_cast<IDirect3DVertexBuffer9*>(model.vertexBuffer);
        Check(vb && SUCCEEDED(vb->Lock(0, 0, &vertexData, D3DLOCK_READONLY)), "read rendered model geometry");
        const float vertexX = static_cast<float*>(vertexData)[0]; vb->Unlock();
        std::printf("VIDEO_GEOMETRY visible=%d vertex_x=%.3f\n", visible, vertexX);
        unsigned changed = 0;
        for (unsigned b=0;b<model.boneCount;++b)
            changed += std::memcmp(firstPose.data()+16*b,model.boneTable[b].matInit,64)!=0;
        std::printf("VIDEO_POSE changed_bones=%u/%u\n",changed,model.boneCount);
        Check(changed>0, "model pose changes after video drop");
        if (model.boneCount == 1) {
            Check(vertexX > .5f, "rendered model geometry advances after video drop");
            Check(model.boneTable[0].trans[0] > 5, "model animation advances after video drop");
            Check(model.boneTable[0].matInit[12] > 5, "rendered bone matrix advances after video drop");
        }
        Check(body.getWorldTransform().getOrigin().y() < 90, "physics world advances after video drop");
        app->PlaybackActive() = 0; StopPlayback(app);
    }
    world->removeRigidBody(&body);
}

void CheckEmptyPlaybackRange(MMDApp* app) {
    const auto window=app->MainWindow();
    SetWindowTextW(GetDlgItem(window,panel::kPlayStartFrameEdit),L"");
    SetWindowTextW(GetDlgItem(window,panel::kPlayStopFrameEdit),L"");
    app->state.lastRegisteredFrame=120;
    app->PlaybackStartsAtCurrentFrame()=0;
    CommandDispatch(GetDlgItem(window,panel::kPlayButton),MAKEWPARAM(408,BN_CLICKED));
    Check(app->PlaybackActive()!=0,"blank default playback range starts playback");
    Check(app->PlaybackStartSeconds()==0 && app->PlaybackEndSeconds()==4,
        "blank range means frame zero through last registered frame");
    const auto pressP=[&]() {
        unsigned char keys[256]{};
        app->LetterHotkeys().Update(keys,true);
        keys['P']=0x80;app->LetterHotkeys().Update(keys,true);
        DispatchLetterHotkeys(app,GetDlgItem(window,panel::kPlayButton));
        keys['P']=0;app->LetterHotkeys().Update(keys,true);
    };
    pressP();Check(!app->PlaybackActive(),"P stops playback with a blank range");
    pressP();Check(app->PlaybackActive()!=0,"P starts playback with a blank range");
    app->PlaybackLoopEnabled()=1;
    app->TimeNowLow()=app->PlaybackClockAnchorLow()+5000;
    app->MilliToSec()=.001f;app->DeltaTime()=1.f/60;
    FrameDriver(app);
    Check(app->PlaybackActive()!=0 && app->PlaybackCursorSeconds()==0,
        "blank start frame loops back to zero");
    app->PlaybackLoopEnabled()=0;pressP();
    struct Range {const wchar_t* start;const wchar_t* end;double from,to;};
    for(const auto& range : {Range{L"0",L"0",0,4},Range{L"30",L"",1,4},
                            Range{L"",L"60",0,2},Range{L"  ",L"\t",0,4}}) {
        SetWindowTextW(GetDlgItem(window,panel::kPlayStartFrameEdit),range.start);
        SetWindowTextW(GetDlgItem(window,panel::kPlayStopFrameEdit),range.end);
        pressP();
        Check(app->PlaybackActive() && app->PlaybackStartSeconds()==range.from &&
            app->PlaybackEndSeconds()==range.to,"optional and explicit playback bounds");
        pressP();
    }
    for(const auto* invalid : {L"-1",L"abc",L"2147483647",L"999999999999999999999999999"}) {
        for(int id : {panel::kPlayStartFrameEdit,panel::kPlayStopFrameEdit}) {
            SetWindowTextW(GetDlgItem(window,panel::kPlayStartFrameEdit),L"");
            SetWindowTextW(GetDlgItem(window,panel::kPlayStopFrameEdit),L"");
            SetWindowTextW(GetDlgItem(window,id),invalid);pressP();
            Check(!app->PlaybackActive(),"invalid nonblank range still rejects playback");
        }
    }
    std::puts("PASS playback button, P shortcut, empty/explicit bounds, loop and invalid input");
}

int wmain(int argc,wchar_t** argv) {
    Check(argc==2 || argc==3 || argc==4 || argc==6,"fixture directory required"); std::setvbuf(stdout,nullptr,_IONBF,0);
    const fs::path directory=fs::absolute(argv[1]);
    const bool playbackOnly = argc>=4 && std::wcscmp(argv[2],L"--video-playback")==0;
    const bool backgroundOnly = argc==3 && std::wcscmp(argv[2],L"--background")==0;
    const bool mediaOnly = argc==3 && std::wcscmp(argv[2],L"--media-only")==0;
    const bool benchmark = argc==3 && !backgroundOnly && !mediaOnly;
    const auto initialized=MH_Initialize(); Check(initialized==MH_OK || initialized==MH_ERROR_ALREADY_INITIALIZED,"dialog guard init");
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(&UnexpectedA),nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(&UnexpectedW),nullptr);
    MH_EnableHook(reinterpret_cast<void*>(&MessageBoxA)); MH_EnableHook(reinterpret_cast<void*>(&MessageBoxW));
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(&KeepHidden),
        reinterpret_cast<void**>(&originalShowWindow)) == MH_OK, "hide recording window guard");
    Check(MH_EnableHook(reinterpret_cast<void*>(&ShowWindow)) == MH_OK, "enable hidden recording guard");
    auto hide=SetWindowsHookExW(WH_CBT,Hidden,nullptr,GetCurrentThreadId());
    std::wstring processorError; processor::Initialize(processorError);
    auto* app=new MMDApp; g_Block=app; app->state=MMDAppState{}; app->InitDefaults(); app->EnglishUI()=1;
    Check(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"hidden production host");
    Check(!IsWindowVisible(app->MainWindow()),"host stays hidden");
    std::printf("PLAYBACK_HOST mme=%d mmhack=%d\n",GetModuleHandleW(L"MMEffect.dll")!=nullptr,
        GetModuleHandleW(L"MMHack.dll")!=nullptr);
    if (playbackOnly) {
        const auto model = argc==6 ? fs::absolute(argv[4]) : directory/L"alpha-triangle.pmx";
        if (argc!=6) Fixture(model);
        LoadModelFile(app,model.c_str(),false);
        if(argc==6) {
            app->EnglishUI()=1;app->state.optflag[0]=0;app->SelectedModelSlot()=0;
            importingTestMotion=true;LoadVmdFile(argv[5]);importingTestMotion=false;
        }
        CheckEmptyPlaybackRange(app);
        CheckVideoPlayback(app, fs::absolute(argv[3]), directory);
        ShutdownCleanup(app); g_Block=nullptr; delete app; UnhookWindowsHookEx(hide);
        return 0;
    }
    if (!benchmark && !backgroundOnly && !mediaOnly) cursor_checks::Run(app);
    app->DeltaTime()=1.f/30; app->state.messageSeen=1; FrameDriver(app);
    wcscpy_s(app->AviBackgroundPath(),256,(benchmark ? fs::absolute(argv[2]) : directory/L"corners.mp4").c_str()); LoadAviFile(app);
    Check(app->AviBackgroundEnabled()==1 && media::HasVideo(app),"production MP4 import and texture refresh");
    if (benchmark) {
        app->PlaybackActive() = 1;
        for (int mode : {0, 1}) {
            app->state.optflag[0] = mode;
            double total = 0, maximum = 0;
            for (int i = 0; i < 600; ++i) {
                app->PlaybackCursorSeconds() = float(i / 60.0);
                const auto start = std::chrono::steady_clock::now();
                AviBgOverlayRefresh(app);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
                if (i >= 60) { total += ms; maximum = std::max(maximum, ms); }
            }
            std::printf("VIDEO_BENCH mode=%d frames=540 mean_ms=%.6f max_ms=%.6f\n", mode, total/540, maximum);
        }
        app->PlaybackActive() = 0;
        ShutdownCleanup(app); g_Block=nullptr; delete app; UnhookWindowsHookEx(hide);
        return 0;
    }
    for (bool playing : {false, true}) {
        app->PlaybackActive() = playing;
        CommandDispatch(0, MAKEWPARAM(216, 0));
        Check(!app->AviBackgroundEnabled() && media::HasVideo(app), "hiding MP4 preserves its reader");
        app->CurrentFrame() = 30;
        app->PlaybackCursorSeconds() = 1.1f;
        CommandDispatch(0, MAKEWPARAM(216, 0));
        Check(app->AviBackgroundEnabled() && media::HasVideo(app), "show MP4 again without reloading");
        const auto pixel = (8 * 64 + 8) * 4;
        bool current = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            // The normal frame driver must publish a completed seek even when
            // playback is stopped and there is no new mouse/keyboard message.
            FrameDriver(app);
            auto displayed = Read(app->Renderer()->device, app->AviBackgroundSurface());
            current = displayed[pixel+1] > 235 && displayed[pixel+2] < 20;
            if (!current) Sleep(5);
        } while (!current && std::chrono::steady_clock::now() < deadline);
        Check(current, "restored MP4 asynchronously displays the current green frame");
    }
    app->PlaybackActive() = 0;
    app->CurrentFrame() = 0;
    app->PlaybackCursorSeconds() = 0;
    auto* device=app->Renderer()->device;
    app->RecordingWindow() = app->MainWindow();
    AviBgOverlayRefresh(app);
    const auto exportFrame = Read(device, app->AviBackgroundSurface());
    Check(exportFrame[(8 * 64 + 8) * 4 + 2] > 235 && exportFrame[(8 * 64 + 8) * 4 + 1] < 20,
          "recording waits for the exact red frame instead of the stale green preview");
    app->RecordingWindow() = nullptr;
    // Keep the alpha-export assertion independent of the user's grid setting.
    app->GroundGridEnabled() = 0;
    app->AviOutputWidth()=64; app->AviOutputHeight()=48;
    std::wstring error; const auto output=directory/L"render-alpha.mov";
    Check(StartPngMovRecording(app->Recorder(),output.c_str(),64,48,30,error),"start production recorder");
    app->RecordingWindow()=CreateWindowW(L"STATIC",L"Hidden capture",WS_POPUP,0,0,64,48,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    app->state.messageSeen=1; RenderFrameScene(app);
    auto clear=Read(device,TransparentCaptureSurface(app));
    std::size_t opaque = 0, clearPixels = 0;
    for (std::size_t i=3;i<clear.size();i+=4) { opaque += clear[i]==255; clearPixels += clear[i]==0; }
    std::printf("Empty export alpha: clear=%zu opaque=%zu total=%zu\n",clearPixels,opaque,clear.size()/4);
    for(std::size_t i=3;i<clear.size();i+=4) Check(clear[i]==0,"transparent scene excludes loaded background");
    if (backgroundOnly) {
        AbortPngMovRecording(app->Recorder());
        ReleaseTransparentCapture(app);
        DestroyWindow(app->RecordingWindow()); app->RecordingWindow()=nullptr;
        ShutdownCleanup(app); g_Block=nullptr; delete app; UnhookWindowsHookEx(hide);
        std::puts("PASS MP4 toggle, asynchronous stopped preview, exact recording frame and transparent background exclusion");
        return 0;
    }
    const auto model=directory/L"alpha-triangle.pmx"; Fixture(model); LoadModelFile(app,model.c_str(),false);
    Check(app->ModelSlot(0)!=nullptr,"load semitransparent model");
    // Settle the newly loaded model through the same pose/physics frame that
    // the main loop runs before validating its rendered coverage.
    app->state.messageSeen=1; FrameDriver(app);
    app->state.messageSeen=1; RenderFrameScene(app);
    const auto triangle=Read(device,TransparentCaptureSurface(app));
    std::size_t empty=0,half=0;
    for(std::size_t i=3;i<triangle.size();i+=4) { empty+=triangle[i]==0; half+=triangle[i]>=126 && triangle[i]<=129; }
    std::printf("Model alpha: empty=%zu half=%zu total=%zu\n",empty,half,triangle.size()/4);
    Check(empty>100 && half>100,"production model retains half opacity and clear background");
    for(int frame=0;frame<30;++frame) {
        Blocks(device,TransparentCaptureSurface(app));
        Check(RecordingReadbackPass(app),"production GPU readback and PNG submission");
    }
    Check(FinishPngMovRecording(app->Recorder(),30,error),"finalize rendered PNG MOV");
    ReleaseTransparentCapture(app);
    DestroyWindow(app->RecordingWindow()); app->RecordingWindow()=nullptr;
    for (int fps : {24, 30, 60}) {
        const auto recording = directory / (L"timeline-" + std::to_wstring(fps) + L".mov");
        wcscpy_s(app->AviOutputPath(), 256, recording.c_str());
        app->Recorder()->selectedCodec = kNativePngMovCodec;
        app->AviRecordStartFrame() = 0; app->AviRecordEndFrame() = 29;
        app->AviRecordFps() = static_cast<float>(fps);
        app->AviIncludeWave() = 0; app->AviCloseAfterExport() = 0;
        StartAviRecordWindow(app);
        Check(IsPngMovRecording(app->Recorder()), "start through production export entry");
        for (int i = 0; i < 180 && IsPngMovRecording(app->Recorder()); ++i) {
            app->state.messageSeen = 1; FrameDriver(app);
        }
        Check(!IsPngMovRecording(app->Recorder()) && fs::exists(recording), "timeline recording completes and publishes MOV");
        Check(app->RecordingWindow() == nullptr, "recording window released");
        std::printf("PASS production timeline export %d fps\n", fps);
    }
    CheckEmptyPlaybackRange(app);
    CheckVideoPlayback(app, directory/L"corners.mp4", directory);
    Check(!IsWindowVisible(app->MainWindow()),"renderer remained hidden");
    ShutdownCleanup(app); g_Block=nullptr; delete app; UnhookWindowsHookEx(hide);
    std::puts("PASS hidden production video import, transparent background, PMX alpha, GPU readback and MOV output");
}
