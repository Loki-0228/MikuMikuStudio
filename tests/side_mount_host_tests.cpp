#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/frame_sampling.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/ui_metrics.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include <mms/refresh.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace mikumikustudio {
int ViewportToolAtPointForTest(MMDApp*);
void LayoutViewportPanels(MMDApp*);
}
namespace {
decltype(&ShowWindow) realShow = nullptr;
decltype(&SetWindowPos) realPosition = nullptr;
bool OwnTopLevel(HWND window) {
    DWORD process=0;GetWindowThreadProcessId(window,&process);
    return process==GetCurrentProcessId()&&GetParent(window)==nullptr;
}
BOOL WINAPI Hidden(HWND window,int command) { return realShow(window,OwnTopLevel(window)?SW_HIDE:command); }
BOOL WINAPI HiddenPosition(HWND window,HWND after,int x,int y,int width,int height,UINT flags) {
    if(OwnTopLevel(window))flags=(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    return realPosition(window,after,x,y,width,height,flags);
}
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT) { std::fprintf(stderr,"FAIL unexpected dialog: %s\n",text);std::exit(2); }
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT) { std::fwprintf(stderr,L"FAIL unexpected dialog: %ls\n",text);std::exit(2); }
void Install(void* target,void* detour,void** original=nullptr) {
    if(MH_CreateHook(target,detour,original)!=MH_OK||MH_EnableHook(target)!=MH_OK)
        throw std::runtime_error("install own hidden-host guard");
}
}
// Reuse the existing actual-scene/key-pool/history invariant and native client
// sizing helper. Its other checks are not executed by this dedicated host.
#include "plugins/small_viewport_host_checks.inc"
#include "ui_scale_popup_fixture.hpp"
#include "plugins/side_mount_host_checks.inc"

int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<2||argc>3||(argc==3&&std::wcscmp(argv[2],L"ordinary")&&std::wcscmp(argv[2],L"bound"))) {
        std::fprintf(stderr,"usage: side_mount_host_tests <absolute-probe-dll> [ordinary|bound]\n");return 2;
    }
#if MMD_ORIGINAL_PROFILE
    // The production compatibility profile deliberately does not initialize
    // plugin runtime. Its pure geometry/NativeUi checks live in other targets.
    std::puts("SKIP actual side mount host: original runtime gate is closed");return 0;
#else
    const bool bound=argc==3&&!std::wcscmp(argv[2],L"bound");
    SetProcessDPIAware();
    auto app=std::make_unique<MMDApp>();bool initialized=false,minhook=false;
    try {
        side_mount_host_checks::PreparePackage(argv[1]);
        if(MH_Initialize()!=MH_OK)throw std::runtime_error("initialize own hidden-host hooks");
        minhook=true;
        Install(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(Hidden),reinterpret_cast<void**>(&realShow));
        Install(reinterpret_cast<void*>(&SetWindowPos),reinterpret_cast<void*>(HiddenPosition),reinterpret_cast<void**>(&realPosition));
        Install(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(UnexpectedA));
        Install(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(UnexpectedW));
        g_Block=app.get();std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
        small_viewport_host_checks::Require(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize actual hidden side mount host");
        initialized=true;
        if(bound) {
            plugins::Shutdown();
            small_viewport_host_checks::Require(plugins::InitializeHistoryTestRuntime(app.get()),"bind actual common history without opening ordinary production edit gate");
        }
        small_viewport_host_checks::Require(bool(plugins::BoundNativePoseHistory(app.get()))==bound,"actual side mount history mode");
        side_mount_host_checks::Run(*app,bound);
        ShutdownCleanup(app.get());initialized=false;g_Block=nullptr;
        MH_Uninitialize();minhook=false;
        std::printf("PASS actual side mount host mode=%s\n",bound?"bound":"ordinary");return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL actual side mount host: %s\n",error.what());
        if(initialized)ShutdownCleanup(app.get());
        g_Block=nullptr;if(minhook)MH_Uninitialize();return 1;
    }
#endif
}
