// The production 286 branch in ordinary full and original profiles. No GPU or
// visible dialog is needed to verify the complete sixteen-slot picker table.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <MinHook.h>
#include <commdlg.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace mikumikustudio {
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}

using namespace mikumikustudio;
namespace {
unsigned checks=0,calls=0;
bool accept=false;
MMDApp* owner=nullptr;
std::array<COLORREF,16> source{};
void Check(bool condition,const char* message){++checks;if(!condition){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
BOOL WINAPI Pick(LPCHOOSECOLORA cc) {
    ++calls;
    Check(cc&&cc->lStructSize==sizeof(*cc)&&cc->hwndOwner==owner->MainWindow(),"actual286 enters the native picker boundary");
    Check(cc->lpCustColors&&cc->lpCustColors!=reinterpret_cast<COLORREF*>(owner->state.customColorTable),"picker owns a complete local COLORREF array");
    Check(cc->rgbResult==RGB(33,66,99)&&(cc->Flags&(CC_RGBINIT|CC_FULLOPEN))==(CC_RGBINIT|CC_FULLOPEN),"picker preserves original initial color and native flags");
    for(unsigned i=0;i<16;++i){Check(cc->lpCustColors[i]==source[i],"picker receives all sixteen original custom colors");cc->lpCustColors[i]=RGB(i+1,i+2,i+3);}
    cc->rgbResult=RGB(17,34,51);return accept;
}
}
int main() {
    Check(MH_Initialize()==MH_OK,"initialize native286 picker interception");
    Check(MH_CreateHook(reinterpret_cast<void*>(&ChooseColorA),reinterpret_cast<void*>(&Pick),nullptr)==MH_OK&&
        MH_EnableHook(reinterpret_cast<void*>(&ChooseColorA))==MH_OK,"intercept actual native color picker only");
    for(bool answer:{false,true}) {
        auto app=std::make_unique<MMDApp>();::new(static_cast<void*>(&app->state))MMDAppState{};owner=app.get();
        app->MainWindow()=CreateWindowExW(0,L"STATIC",L"hidden legacy outline picker test",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(app->MainWindow()!=nullptr,"create hidden native286 command owner");app->HInstance()=GetModuleHandleW(nullptr);
        app->state.modelOutlineColorRed=33;app->state.modelOutlineColorGreen=66;app->state.modelOutlineColorBlue=99;
        for(unsigned i=0;i<16;++i)source[i]=RGB(i+20,i+40,i+60);
        static_assert(sizeof(app->state.customColorTable)+sizeof(app->state.pad209)==sizeof(source));
        std::memcpy(app->state.customColorTable,source.data(),sizeof(app->state.customColorTable));
        std::memcpy(&app->state.pad209,reinterpret_cast<const unsigned char*>(source.data())+sizeof(app->state.customColorTable),sizeof(app->state.pad209));
        app->WireframeRenderingEnabled()=37;accept=answer;const auto previousCalls=calls;
        CmdViewMenu(app.get(),app->MainWindow(),286,0);
        Check(calls==previousCalls+1,"actual286 opens exactly one picker");
        std::array<COLORREF,16> actual{};std::memcpy(actual.data(),app->state.customColorTable,sizeof(app->state.customColorTable));
        std::memcpy(reinterpret_cast<unsigned char*>(actual.data())+sizeof(app->state.customColorTable),&app->state.pad209,sizeof(app->state.pad209));
        for(unsigned i=0;i<16;++i)Check(actual[i]==(answer?RGB(i+1,i+2,i+3):source[i]),"confirmation preserves all sixteen custom colors and cancellation writes none");
        Check(app->WireframeRenderingEnabled()==37,"sixteen-color picker never overwrites the following wireframe flag");
        Check(app->state.modelOutlineColorRed==(answer?17:33)&&app->state.modelOutlineColorGreen==(answer?34:66)&&
            app->state.modelOutlineColorBlue==(answer?51:99),"legacy RGB confirmation and cancellation semantics are unchanged");
        DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;
    }
    MH_DisableHook(reinterpret_cast<void*>(&ChooseColorA));MH_RemoveHook(reinterpret_cast<void*>(&ChooseColorA));MH_Uninitialize();
    std::printf("PASS native legacy outline color: %u checks\n",checks);return 0;
}
