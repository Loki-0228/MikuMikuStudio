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
#include "plugins/slider_paint_trace_host_checks.inc"
#include "plugins/slider_focus_cues_host_checks.inc"
#include "plugins/light_slider_geometry_checks.inc"
#include "plugins/light_slider_clip_checks.inc"
#include "plugins/light_slider_paint_checks.inc"
#include "plugins/light_slider_native_surface_checks.inc"
#include "plugins/light_slider_buffer_checks.inc"
#include "plugins/slider_user_capture_checks.inc"
#include "plugins/layout_slider_repaint_checks.inc"
#include "plugins/layout_slider_guard_checks.inc"

int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const bool sliderUser=argc==3&&!std::strcmp(argv[2],"slider-user");
    if(!sliderUser)SetProcessDPIAware();
    const bool focusCues=argc==3&&!std::strcmp(argv[2],"focus-cues");
    const bool lightPaint=argc==3&&!std::strcmp(argv[2],"light-paint");
    const bool lightGeometry=argc==3&&!std::strcmp(argv[2],"light-geometry");
    const bool lightSurface=argc==3&&!std::strcmp(argv[2],"light-surface");
    const bool lightClip=argc==3&&!std::strcmp(argv[2],"light-clip");
    const bool lightBuffer=argc==3&&!std::strcmp(argv[2],"light-buffer");
    const bool layoutRepaint=argc==3&&!std::strcmp(argv[2],"layout-repaint");
    const bool layoutGuards=argc==3&&!std::strcmp(argv[2],"layout-guards");
    if((argc!=2&&!focusCues&&!lightPaint&&!lightGeometry&&!lightSurface&&!lightClip&&!lightBuffer&&!sliderUser&&!layoutRepaint&&!layoutGuards)||(std::strcmp(argv[1],"ordinary")&&std::strcmp(argv[1],"bound"))){
        std::fprintf(stderr,"usage: small_viewport_host_tests ordinary|bound [focus-cues|light-paint|light-geometry|light-surface|light-clip|light-buffer|slider-user|layout-repaint|layout-guards]\n");return 2;
    }
    if(MH_Initialize()!=MH_OK)return 2;
    Install(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(Hidden),reinterpret_cast<void**>(&realShow));
    Install(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(UnexpectedA));
    Install(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(UnexpectedW));
    auto* app=new MMDApp;g_Block=app;std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
    bool initialized=false;int result=0;
    try{
        if(sliderUser)slider_user_capture_checks::ProductDpi();
        small_viewport_host_checks::Require(InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE),"initialize actual hidden viewport test host");
        initialized=true;
        if(!std::strcmp(argv[1],"bound")){
            plugins::Shutdown();
            small_viewport_host_checks::Require(plugins::InitializeHistoryTestRuntime(app),"bind the actual common-history host");
        }
        const bool expectedBound=!std::strcmp(argv[1],"bound");
        small_viewport_host_checks::Require(bool(plugins::BoundNativePoseHistory(app))==expectedBound,
            "actual native editing binding matches the isolated viewport test mode");
        if(sliderUser){
            slider_user_capture_checks::Run(*app,std::filesystem::current_path()/"slider-user");
        }else if(focusCues||lightPaint||lightGeometry||lightSurface||lightClip||lightBuffer||layoutRepaint||layoutGuards){
            app->CameraMode()=1;PostLanguageSweep2(app);PostViewRefresh(app);
            small_viewport_host_checks::ClientSize(*app,1200,820);
            realShow(app->MainWindow(),lightPaint?SW_SHOW:SW_SHOWNOACTIVATE);
            if(lightPaint){
                const BOOL requested=SetForegroundWindow(app->MainWindow());
                std::printf("LIGHT_DIAGNOSTIC_FOREGROUND requested=%d actual_self=%d; one initial request, no retry\n",
                    int(requested),int(GetForegroundWindow()==app->MainWindow()));
            }
            RedrawWindow(app->MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            if(layoutRepaint||layoutGuards){
                small_viewport_host_checks::ClientSize(*app,1400,820);
#if !MMD_ORIGINAL_PROFILE
                if(layoutGuards)layout_slider_guard_checks::Run(*app,std::filesystem::current_path()/"layout-guards");
                else layout_slider_repaint_checks::Run(*app,std::filesystem::current_path()/"layout-repaint");
#else
                throw std::runtime_error("layout paint buffer diagnostics require the full profile");
#endif
            }else if(focusCues){
                slider_focus_cues_host_checks::Run(app,std::filesystem::current_path()/"focus-cues",expectedBound);
            }else{
                const auto output=std::filesystem::current_path()/"light-paint";
                if(!lightSurface&&!lightBuffer){
                    const auto geometry=light_slider_geometry_checks::Run(*app,output);
                    small_viewport_host_checks::Require(geometry.physical!=light_slider_geometry_checks::PhysicalStatus::Failed,
                        "light slider geometry capture and DPI context restoration have no API error");
                }
                if(lightPaint)light_slider_paint_checks::Run(app,output,expectedBound,false);
                if(lightPaint||lightSurface)light_slider_native_surface_checks::Run(app,output);
                if(lightClip)light_slider_clip_checks::Run(app,output);
                if(lightBuffer)light_slider_buffer_checks::Run(app,output,expectedBound);
            }
            realShow(app->MainWindow(),SW_HIDE);
        }else{
            small_viewport_host_checks::Run(app);
            bake_mic_guard_checks::Run(app);
        }
        if(!sliderUser)std::printf("PASS SMALL_VIEWPORT_MODE actual_bound=%u\n",expectedBound?1u:0u);
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL small viewport host: %s\n",error.what());result=1;}
    if(initialized)ShutdownCleanup(app);
    g_Block=nullptr;delete app;MH_Uninitialize();return result;
}
