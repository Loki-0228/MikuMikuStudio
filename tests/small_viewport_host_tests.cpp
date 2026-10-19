#define NOMINMAX
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/frame_sampling.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

using namespace mikudancestudio;
namespace {
decltype(&ShowWindow) realShow=nullptr;
BOOL WINAPI Hidden(HWND window,int command){return realShow(window,GetParent(window)?command:SW_HIDE);}
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT){std::fprintf(stderr,"FAIL unexpected dialog: %s\n",text);std::exit(2);}
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT){std::fwprintf(stderr,L"FAIL unexpected dialog: %ls\n",text);std::exit(2);}
void Install(void* target,void* detour,void** original=nullptr){
    if(MH_CreateHook(target,detour,original)!=MH_OK||MH_EnableHook(target)!=MH_OK){
        std::fprintf(stderr,"FAIL install hidden host guard\n");std::exit(2);
    }
}
}
#include "plugins/small_viewport_host_checks.inc"
#include "plugins/bake_mic_guard_checks.inc"

int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);SetProcessDPIAware();
    if(argc!=2||(std::strcmp(argv[1],"ordinary")&&std::strcmp(argv[1],"bound"))){
        std::fprintf(stderr,"usage: small_viewport_host_tests ordinary|bound\n");return 2;
    }
    if(MH_Initialize()!=MH_OK)return 2;
    Install(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(Hidden),reinterpret_cast<void**>(&realShow));
    Install(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(UnexpectedA));
    Install(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(UnexpectedW));
    auto* app=new MMDApp;g_Block=app;std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
    bool initialized=false;int result=0;
    try{
        small_viewport_host_checks::Require(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize actual hidden viewport test host");
        initialized=true;
        if(!std::strcmp(argv[1],"bound")){
            plugins::Shutdown();
            small_viewport_host_checks::Require(plugins::InitializeHistoryTestRuntime(app),"bind the actual common-history host");
        }
        const bool expectedBound=!std::strcmp(argv[1],"bound");
        small_viewport_host_checks::Require(bool(plugins::BoundNativePoseHistory(app))==expectedBound,
            "actual native editing binding matches the isolated viewport test mode");
        small_viewport_host_checks::Run(app);
        bake_mic_guard_checks::Run(app);
        std::printf("PASS SMALL_VIEWPORT_MODE actual_bound=%u\n",expectedBound?1u:0u);
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL small viewport host: %s\n",error.what());result=1;}
    if(initialized)ShutdownCleanup(app);
    g_Block=nullptr;delete app;MH_Uninitialize();return result;
}
