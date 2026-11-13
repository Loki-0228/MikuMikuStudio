// Hidden production painter baseline. Run only from a dedicated copied test
// installation containing the marker below. No desktop pixels are read.
#define NOMINMAX
#include "status_title_capture.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/status_title_checked_gdi.hpp"
#include "mikumikustudio/offscreen_gdi_surface.hpp"
#include <MinHook.h>
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
decltype(&ShowWindow) originalShow = nullptr;
BOOL WINAPI Hidden(HWND window, int command) {
    return originalShow(window, GetParent(window) ? command : SW_HIDE);
}
int WINAPI UnexpectedA(HWND, LPCSTR, LPCSTR, UINT) { std::fputs("unexpected dialog\n",stderr);std::exit(2); }
int WINAPI UnexpectedW(HWND, LPCWSTR, LPCWSTR, UINT) { std::fputs("unexpected dialog\n",stderr);std::exit(2); }
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Hook(void* target, void* replacement, void** original) {
    Require(MH_CreateHook(target,replacement,original)==MH_OK,"create hidden host hook");
    Require(MH_EnableHook(target)==MH_OK,"enable hidden host hook");
}
void VerifyInstallation(fs::path& root) {
    std::array<wchar_t,32768> path{};
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    Require(n && n<path.size(),"resolve fixture executable");
    root=fs::path(path.data()).parent_path();
    std::ifstream marker(root/L"status-title-test-installation.txt",std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(marker)),{});
    Require(contents=="MMS status title test installation v1\n","dedicated test installation marker");
    const auto plugins=root/L"PLUGINS";
    Require(!fs::exists(plugins) || (fs::is_directory(plugins) && fs::is_empty(plugins)),"no plugins in fixture");
    wchar_t desktop[256]{}; DWORD needed=0;
    const auto handle=GetThreadDesktop(GetCurrentThreadId());
    Require(handle && GetUserObjectInformationW(handle,UOI_NAME,desktop,sizeof desktop,&needed) &&
        _wcsicmp(desktop,L"Default")==0,"requires inherited Default desktop");
}
void Write(const fs::path& path,const status_title_capture::Image& image) {
    std::ofstream file(path,std::ios::binary|std::ios::out|std::ios::trunc);
    Require(bool(file),"open new baseline image");
    const std::uint32_t width=static_cast<std::uint32_t>(image.band.right-image.band.left);
    const std::uint32_t height=static_cast<std::uint32_t>(image.band.bottom-image.band.top);
    file.write(reinterpret_cast<const char*>(&width),sizeof width);
    file.write(reinterpret_cast<const char*>(&height),sizeof height);
    file.write(reinterpret_cast<const char*>(image.rgb.data()),static_cast<std::streamsize>(image.rgb.size()*sizeof(std::uint32_t)));
    Require(bool(file),"write baseline image");
}
std::uint64_t ThreadToken(void*) noexcept { return GetCurrentThreadId(); }
bool SameRect(const RECT& a, const RECT& b) {
    return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
}
bool ExactClip(HDC dc, const RECT& bounds, const RECT* hole = nullptr) {
    struct Region {
        HRGN value;
        ~Region() { if (value) DeleteObject(value); }
    } expected{CreateRectRgnIndirect(&bounds)}, actual{CreateRectRgn(0,0,0,0)};
    if (!expected.value || !actual.value) return false;
    if (hole) {
        Region excluded{CreateRectRgnIndirect(hole)};
        if (!excluded.value || CombineRgn(expected.value,expected.value,excluded.value,RGN_DIFF)==ERROR) return false;
    }
    RECT expectedBounds{}, effectiveBounds{}, actualBounds{};
    const int expectedKind=GetRgnBox(expected.value,&expectedBounds);
    return expectedKind!=ERROR && GetClipRgn(dc,actual.value)==1 &&
        EqualRgn(actual.value,expected.value) &&
        GetRgnBox(actual.value,&actualBounds)==expectedKind && SameRect(actualBounds,expectedBounds) &&
        GetClipBox(dc,&effectiveBounds)==expectedKind && SameRect(effectiveBounds,expectedBounds);
}
void DifferenceTitle(OffscreenGdiRuntimeCore<16>& runtime, HDC reference, MMDApp* app,
                      RECT client, RECT title) {
    const RECT sentinel{11,22,33,44};
    auto undersized=runtime.Reserve(Win32OffscreenSurfaceOps());
    Require(undersized.epoch && runtime.Initialize(undersized,reinterpret_cast<std::uintptr_t>(reference),1,1)==OffscreenSurfaceInit::Ready,
            "initialize undersized difference surface");
    RECT changed=sentinel;
    Require(!CaptureStatusTitleDifference(runtime,undersized,app,changed) && SameRect(changed,sentinel),
            "undersized difference fails without changing output");
    Require(runtime.Release(undersized),"release rejected difference surface");

    // Keep real DC clipping state across sessions. A hole has the same outer
    // bounding rectangle as the title, but still omits pixels from the capture.
    for (int clipCase = 0; clipCase < 3; ++clipCase) {
        auto clipped=runtime.Reserve(Win32OffscreenSurfaceOps());
        Require(clipped.epoch && runtime.Initialize(clipped,reinterpret_cast<std::uintptr_t>(reference),client.right,client.bottom)==OffscreenSurfaceInit::Ready,
                "initialize clipped difference surface");
        Require(runtime.WithDraw(clipped,[&](StatusTitleGdiSession& session) {
            OffscreenSurfaceView view{};
            if (!session.Read(view) || !view.dc) return false;
            HDC dc=reinterpret_cast<HDC>(view.dc);
            if (clipCase == 1) {
                const RECT empty{};
                return IntersectClipRect(dc,0,0,0,0)!=ERROR && ExactClip(dc,empty);
            }
            if (clipCase == 2) {
                const RECT shortTitle{title.left+1,title.top,title.right,title.bottom};
                return IntersectClipRect(dc,shortTitle.left,shortTitle.top,shortTitle.right,shortTitle.bottom)!=ERROR &&
                    ExactClip(dc,shortTitle);
            }
            const int x=title.left+(title.right-title.left)/2;
            const int y=title.top+(title.bottom-title.top)/2;
            const RECT hole{x,y,x+1,y+1};
            return IntersectClipRect(dc,title.left,title.top,title.right,title.bottom)!=ERROR &&
                ExcludeClipRect(dc,x,y,x+1,y+1)!=ERROR && ExactClip(dc,title,&hole);
        }),"install incomplete clipping region");
        changed=sentinel;
        Require(!CaptureStatusTitleDifference(runtime,clipped,app,changed) && SameRect(changed,sentinel),
                "incomplete clip fails without changing output");
        std::size_t occupied=0,pending=0; bool closing=false;
        Require(runtime.Counts(occupied,pending,closing) && occupied==1 && pending==1 && !closing,
                "rejected clipped surface remains owned for cleanup");
        Require(runtime.Release(clipped),"release clipped difference surface");
    }

    auto surface=runtime.Reserve(Win32OffscreenSurfaceOps());
    Require(surface.epoch && runtime.Initialize(surface,reinterpret_cast<std::uintptr_t>(reference),client.right,client.bottom)==OffscreenSurfaceInit::Ready,
            "initialize camera difference surface");
    // Establish the same initial pixels for the independent reference image and
    // the tested surface. The title band can extend below the gradient.
    Require(runtime.WithDraw(surface,[&](StatusTitleGdiSession& session) {
        OffscreenSurfaceView view{};
        if (!session.Read(view) || !view.pixels || view.width!=client.right || view.height!=client.bottom) return false;
        const HDC dc=reinterpret_cast<HDC>(view.dc);
        if (!dc || IntersectClipRect(dc,title.left,title.top,title.right,title.bottom)==ERROR || !ExactClip(dc,title)) return false;
        std::memset(view.pixels,0,static_cast<std::size_t>(view.width)*view.height*sizeof(std::uint32_t));
        return true;
    }),"clear title difference surface");
    const auto layout=FramePanelLayout(app);
    bool backgroundOk=false;
    status_title_capture::Image background;
    const auto paintBackground=[&](HDC dc) {
        if (IntersectClipRect(dc,title.left,title.top,title.right,title.bottom)==ERROR || !ExactClip(dc,title)) return;
        TRIVERTEX vertices[2]{};
        vertices[0].x=layout.overlay.left; vertices[0].y=layout.topBand.top;
        vertices[0].Red=vertices[0].Green=vertices[0].Blue=0x9600;
        vertices[1].x=layout.overlay.right; vertices[1].y=layout.topBand.bottom-1;
        vertices[1].Red=vertices[1].Green=vertices[1].Blue=0x6400;
        GRADIENT_RECT mesh{0,1};
        if (!GradientFill(dc,vertices,2,&mesh,1,GRADIENT_FILL_RECT_V)) return;
        HGDIOBJ pen=GetStockObject(BLACK_PEN);
        if (!pen) return;
        HGDIOBJ previous=SelectObject(dc,pen);
        if (!previous || previous==HGDI_ERROR) return;
        const bool lines=MoveToEx(dc,layout.overlay.left,layout.topBand.top,nullptr) &&
            LineTo(dc,layout.overlay.right,layout.topBand.top) &&
            LineTo(dc,layout.overlay.right,layout.topBand.bottom-1) &&
            LineTo(dc,layout.overlay.left,layout.topBand.bottom-1) &&
            LineTo(dc,layout.overlay.left,layout.topBand.top);
        const auto restored=SelectObject(dc,previous);
        backgroundOk=lines && restored && restored!=HGDI_ERROR;
    };
    Require(status_title_capture::Capture(reference,client.right,client.bottom,title,paintBackground,background) && backgroundOk,
            "capture independent title background");
    changed=sentinel;
    Require(CaptureStatusTitleDifference(runtime,surface,app,changed),"capture camera title difference");
    Require(changed.left>=title.left && changed.top>=title.top && changed.right<=title.right && changed.bottom<=title.bottom &&
            changed.right>changed.left && changed.bottom>changed.top,"camera difference is nonempty and within title clip");
    // Derive an independent bounding box from the final DIB, without using
    // the production scan or its returned coordinates.
    RECT expected{title.right,title.bottom,title.left,title.top};
    std::size_t differences=0;
    Require(runtime.WithDraw(surface,[&](StatusTitleGdiSession& session) {
        OffscreenSurfaceView view{};
        if (!session.Read(view) || !view.pixels || view.width!=client.right || view.height!=client.bottom || !GdiFlush()) return false;
        const auto* pixels=static_cast<const std::uint32_t*>(view.pixels);
        std::size_t index=0;
        for (LONG y=title.top;y<title.bottom;++y) for (LONG x=title.left;x<title.right;++x,++index) {
            if ((pixels[static_cast<std::size_t>(y)*view.width+x]&0x00ffffffu)==background.rgb[index]) continue;
            ++differences;
            expected.left=(std::min)(expected.left,x); expected.top=(std::min)(expected.top,y);
            expected.right=(std::max)(expected.right,x+1); expected.bottom=(std::max)(expected.bottom,y+1);
        }
        return true;
    }),"inspect title difference pixels");
    Require(differences && SameRect(changed,expected),"title difference exactly bounds changed pixels");
    Require(runtime.Release(surface),"release camera difference surface");
}
void CheckedCamera(StatusTitleGdiOwner& owner, HDC reference, HWND window, MMDApp* app,
                   RECT client, RECT band) {
    const auto ops = Win32OffscreenSurfaceOps();
    const auto undersized = owner.Reserve(ops);
    Require(undersized.generation != 0, "reserve undersized checked surface");
    Require(owner.Initialize(undersized, reinterpret_cast<std::uintptr_t>(reference), 1, 1) == OffscreenSurfaceInit::Ready,
            "initialize undersized checked surface");
    Require(!PaintStatusTextChecked(owner, undersized, app), "reject undersized checked surface");
    std::size_t occupied=0, pending=0; bool closing=false;
    Require(owner.Counts(occupied,pending,closing) && occupied==1 && pending==1 && !closing,
            "undersized surface retires without publication");
    Require(owner.Release(undersized), "release undersized checked surface");
    Require(!owner.Release(undersized), "released checked handle cannot be reused");

    const auto surface = owner.Reserve(ops);
    Require(surface.generation != 0, "reserve checked surface");
    Require(owner.Initialize(surface, reinterpret_cast<std::uintptr_t>(reference), client.right, client.bottom) == OffscreenSurfaceInit::Ready,
            "initialize full checked surface");
    OffscreenSurfaceView view{};
    Require(owner.Read(surface,view) && view.ready && view.pixels && view.width==client.right && view.height==client.bottom,
            "checked surface reports full client size");
    std::memset(view.pixels, 0, static_cast<std::size_t>(view.width)*view.height*sizeof(std::uint32_t));
    status_title_capture::Image before, after;
    const auto paint=[](HDC dc){HandlePaletteChanged2(dc);};
    Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,before), "capture title before checked draw");
    Require(PaintStatusTextChecked(owner,surface,app), "checked title transaction succeeds");
    Require(owner.Read(surface,view) && view.ready && !view.font && view.pixels, "checked draw restored font");
    const auto* pixels=static_cast<const std::uint32_t*>(view.pixels);
    bool any=false;
    for (LONG y=band.top; y<band.bottom && !any; ++y)
        for (LONG x=band.left; x<band.right; ++x)
            if (pixels[static_cast<std::size_t>(y)*view.width+x] & 0x00ffffffu) { any=true; break; }
    Require(any, "checked title produced visible pixels");
    Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,after) &&
            status_title_capture::Equal(before,after), "checked draw leaves ordinary title unchanged");
    Require(!IsWindowVisible(window), "checked draw keeps host hidden");
    Require(owner.Release(surface), "release checked surface");
    Require(owner.Counts(occupied,pending,closing) && occupied==0 && pending==0, "checked resources closed");
}
}
int main() {
    MMDApp* app=nullptr; bool hooked=false, appInitialized=false;
    try {
        fs::path root; VerifyInstallation(root);
        const fs::path output=root/L"title-baseline";
        Require(!fs::exists(output) && fs::create_directory(output),"create fresh baseline directory");
        SetProcessDPIAware(); Require(MH_Initialize()==MH_OK,"initialize hidden host hooks");hooked=true;
        Hook(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(&Hidden),reinterpret_cast<void**>(&originalShow));
        void* unused=nullptr;
        Hook(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(&UnexpectedA),&unused);
        Hook(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(&UnexpectedW),&unused);
        app=new MMDApp;g_Block=app;std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
        Require(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize hidden MMS host");appInitialized=true;
        const HWND window=app->MainWindow();Require(window && !IsWindowVisible(window),"host must remain hidden");
        SetWindowPos(window,nullptr,0,0,1200,820,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        HandleWindowSize(app);app->CameraMode()=1;app->FrameStepPlayback()=0;
        HDC reference=GetDC(window);Require(reference!=nullptr,"obtain hidden host DC");
        struct Release { HWND window; HDC dc; ~Release(){if(dc)ReleaseDC(window,dc);} } release{window,reference};
        RECT client{};Require(GetClientRect(window,&client)!=0,"read client rectangle");
        // Retain the owner until process exit if native cleanup ever fails;
        // the test does not destroy an owner holding a failed GDI resource.
        auto* checkedOwner = new StatusTitleGdiOwner({nullptr,ThreadToken});
        // Retain this runtime on failure too: native cleanup must not outlive it.
        auto* differenceRuntime = new OffscreenGdiRuntimeCore<16>({nullptr,ThreadToken});
        Require(differenceRuntime->Start(),"start title difference runtime");
        for (const int language : {kUiChinese,kUiEnglish,kUiJapanese}) {
            app->EnglishUI()=language;LocalizeUI(app);PostLanguageSweep(app);HandleWindowSize(app);
            const auto layout=FramePanelLayout(app);
            RECT band{layout.topBand.left,layout.topBand.top,layout.TitleRight(),layout.topBand.top+25};
            Require(band.left>=0 && band.top>=0 && band.right<=client.right && band.bottom<=client.bottom &&
                band.right>band.left && band.bottom>band.top,"title band inside client");
            for (int playing : {0,1}) {
                app->PlaybackActive()=playing;
                status_title_capture::Image first,repeat;
                const auto paint=[](HDC dc){HandlePaletteChanged2(dc);};
                Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,first),"capture production title");
                Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,repeat),"repeat production title");
                Require(status_title_capture::Equal(first,repeat),"same state paints identical title pixels");
                CheckedCamera(*checkedOwner,reference,window,app,client,band);
                DifferenceTitle(*differenceRuntime,reference,app,client,band);
                Write(output/(L"camera-"+std::to_wstring(language)+L"-"+std::to_wstring(playing)+L".rgb"),first);
            }
        }
        // The caller creates this test-authored PMX inside the isolated fixture
        // directory; never fall back to a user model or another installation.
        const auto modelPath=root/L"status-title-model.pmx";
        Require(fs::is_regular_file(modelPath),"dedicated status-title-model.pmx fixture");
        LoadModelFile(app,modelPath.c_str(),false);
        app->SelectedModelSlot()=0;
        auto* raw=app->SelectedModel();
        Require(raw!=nullptr,"load isolated title model");
        auto* model=mdl::Mdl(raw);
        Require(model && model->boneCount && model->boneTable,"fixture has a selectable bone");
        model->selectedBone=0;
        app->CameraMode()=0;
        app->PlaybackActive()=0;
        app->CameraParentModel()=0;
        for (const int language : {kUiChinese,kUiEnglish,kUiJapanese}) {
            app->EnglishUI()=language;LocalizeUI(app);PostLanguageSweep(app);HandleWindowSize(app);
            const auto layout=FramePanelLayout(app);
            const RECT band{layout.topBand.left,layout.topBand.top,layout.TitleRight(),layout.topBand.top+25};
            Require(band.left>=0 && band.top>=0 && band.right<=client.right && band.bottom<=client.bottom &&
                band.right>band.left && band.bottom>band.top,"model title band inside client");
            // No trace, trace, active trace indicator, and playback. These are
            // actual painter branches, not simulated text drawing.
            for(int state=0;state<4;++state) {
                app->state.followCameraEnabled=(state==1 || state==2);
                app->state.viewDirty=(state==2);
                app->PlaybackActive()=(state==3);
                status_title_capture::Image first,repeat;
                const auto paint=[](HDC dc){HandlePaletteChanged2(dc);};
                Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,first),"capture model title");
                Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,repeat),"repeat model title");
                Require(status_title_capture::Equal(first,repeat),"same model state paints identical title pixels");
                DifferenceTitle(*differenceRuntime,reference,app,client,band);
                Write(output/(L"model-"+std::to_wstring(language)+L"-"+std::to_wstring(state)+L".rgb"),first);
            }
        }
        app->PlaybackActive()=0;
        app->state.followCameraEnabled=0;
        background_save::SetStatus(app,background_save::Status::Saving,true);
        const auto savingDifference=differenceRuntime->Reserve(Win32OffscreenSurfaceOps());
        Require(savingDifference.epoch && differenceRuntime->Initialize(savingDifference,reinterpret_cast<std::uintptr_t>(reference),
            client.right,client.bottom)==OffscreenSurfaceInit::Ready,"initialize saving difference surface");
        const RECT sentinel{11,22,33,44}; RECT unchanged=sentinel;
        Require(!CaptureStatusTitleDifference(*differenceRuntime,savingDifference,app,unchanged) && SameRect(unchanged,sentinel),
            "saving difference fails without changing output");
        Require(differenceRuntime->Release(savingDifference),"release rejected saving difference surface");
        const auto savingSurface=checkedOwner->Reserve(Win32OffscreenSurfaceOps());
        Require(savingSurface.generation && checkedOwner->Initialize(savingSurface,reinterpret_cast<std::uintptr_t>(reference),
            client.right,client.bottom)==OffscreenSurfaceInit::Ready,"initialize saving checked surface");
        Require(!PaintStatusTextChecked(*checkedOwner,savingSurface,app),"checked save title remains unsupported");
        Require(checkedOwner->Release(savingSurface),"release rejected saving surface");
        for (const int language : {kUiChinese,kUiEnglish,kUiJapanese}) {
            app->EnglishUI()=language;LocalizeUI(app);PostLanguageSweep(app);HandleWindowSize(app);
            const auto layout=FramePanelLayout(app);
            const RECT band{layout.topBand.left,layout.topBand.top,layout.TitleRight(),layout.topBand.top+25};
            status_title_capture::Image first,repeat;
            const auto paint=[](HDC dc){HandlePaletteChanged2(dc);};
            Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,first),"capture saving title");
            Require(status_title_capture::Capture(reference,client.right,client.bottom,band,paint,repeat),"repeat saving title");
            Require(status_title_capture::Equal(first,repeat),"same saving state paints identical title pixels");
            Write(output/(L"saving-"+std::to_wstring(language)+L".rgb"),first);
        }
        Require(!IsWindowVisible(window),"host remained hidden");
        Require(checkedOwner->Shutdown(),"checked owner shuts down without retained resources");
        delete checkedOwner; checkedOwner=nullptr;
        Require(differenceRuntime->ShutdownAtSafePoint(),"difference runtime shuts down without retained resources");
        delete differenceRuntime; differenceRuntime=nullptr;
        std::puts("status title checked GDI camera PASS: 6 states, undersized and saving rejected");
        std::puts("status title production baseline PASS: 21 states");
        ReleaseDC(window,reference);release.dc=nullptr;
        ShutdownCleanup(app);appInitialized=false;g_Block=nullptr;delete app;app=nullptr;
        MH_Uninitialize();return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"status title production baseline FAIL: %s\n",e.what());
        if(appInitialized)ShutdownCleanup(app);
        g_Block=nullptr;delete app;
        if(hooked)MH_Uninitialize();return 1;
    }
}
