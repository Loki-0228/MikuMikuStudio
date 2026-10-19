// MAINTENANCE: regression-keep; real Win32 popup lifecycle for the Open split menu.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>

using namespace mikumikustudio;
namespace {
enum class Scenario { Label, ExpandedLabel, Arrow, Enter, Browse, Disabled };
MMDApp* app=nullptr;
HMENU fileMenu=nullptr;
Scenario scenario{};
unsigned stage=0,browsers=0,commands=0;
bool selectedRecent=false,consumed=false,geometryValid=false;
BOOL CancelBrowser(LPOPENFILENAMEW parameters) {
    if(parameters->hwndOwner!=app->MainWindow() || std::wcscmp(parameters->lpstrDefExt,L"pmm"))std::abort();
    ++browsers;return FALSE;
}
LRESULT CALLBACK OwnerProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(!app)return DefWindowProcW(window,message,wp,lp);
    if(message==WM_MENUSELECT && reinterpret_cast<HMENU>(lp)!=fileMenu && HIWORD(wp)!=0xffff && lp)
        selectedRecent=true;
    if(recent_projects::HandleMessage(app,message,wp,lp))return 0;
    if(message==WM_COMMAND && LOWORD(wp)==205) {++commands;CmdOpenScene(app);return 0;}
    if(message==WM_TIMER) {
        // Let Windows perform navigation and emit its own WM_MENUSELECT and
        // popup lifecycle notifications. Only the final pointer location is
        // supplied to the installed filter, without moving the user's mouse.
        if(stage<2)PostMessageW(window,WM_KEYDOWN,VK_DOWN,0);
        else if(stage==2 && (scenario==Scenario::ExpandedLabel || scenario==Scenario::Browse))
            PostMessageW(window,WM_KEYDOWN,VK_RIGHT,0);
        else if((stage==3 || stage==4) && scenario==Scenario::Browse)PostMessageW(window,WM_KEYDOWN,VK_DOWN,0);
        else if(stage==6) {
            RECT rect{};geometryValid=GetMenuItemRect(window,fileMenu,1,&rect)!=FALSE;
            if(scenario==Scenario::Browse)PostMessageW(window,WM_KEYDOWN,VK_RETURN,0);
            else {
                MSG input{};input.hwnd=window;
                input.message=scenario==Scenario::Enter?WM_KEYDOWN:WM_LBUTTONUP;
                input.wParam=scenario==Scenario::Enter?VK_RETURN:0;
                input.pt={scenario==Scenario::Arrow?rect.right-3:rect.left+40,(rect.top+rect.bottom)/2};
                consumed=CallMsgFilterW(&input,MSGF_MENU)!=0;
                if(!consumed)EndMenu();
            }
        } else if(stage>12)EndMenu();
        ++stage;return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto previous=GetThreadDesktop(GetCurrentThreadId());
    wchar_t name[80]{};swprintf_s(name,L"MMS-recent-menu-%lu",GetCurrentProcessId());
    const auto desktop=CreateDesktopW(name,nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop || !SetThreadDesktop(desktop)){std::puts("FAIL private desktop");return 1;}
    auto storage=std::make_unique<MMDApp>();app=storage.get();app->state=MMDAppState{};
    GetCurrentDirectoryW(MAX_PATH,app->ExeDir());
    wchar_t history[MAX_PATH]{};swprintf_s(history,L"%ls\\%ls-missing-history.txt",app->ExeDir(),name);
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",history);
    if(MH_Initialize()!=MH_OK || MH_CreateHook(reinterpret_cast<void*>(&GetUiOpenFileNameW),
        &CancelBrowser,nullptr)!=MH_OK || MH_EnableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW))!=MH_OK)return 1;
    WNDCLASSW cls{};cls.lpszClassName=name;cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=OwnerProc;
    RegisterClassW(&cls);
    HWND owner=CreateWindowW(name,L"",WS_OVERLAPPEDWINDOW,0,0,400,400,nullptr,nullptr,cls.hInstance,nullptr);
    app->Hwnd()=owner;
    HMENU bar=CreateMenu();fileMenu=CreatePopupMenu();
    AppendMenuW(fileMenu,MF_STRING,204,L"New");AppendMenuW(fileMenu,MF_STRING,205,L"Open");
    AppendMenuW(fileMenu,MF_STRING,207,L"Save");AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(fileMenu),L"File");
    SetMenu(owner,bar);
    unsigned failures=0;
    for(int language:{0,1,2})for(auto test:{Scenario::Label,Scenario::ExpandedLabel,Scenario::Arrow,Scenario::Enter,Scenario::Browse,Scenario::Disabled}) {
        app->EnglishUI()=language;scenario=test;stage=commands=browsers=0;
        selectedRecent=consumed=geometryValid=false;
        EnableMenuItem(fileMenu,205,MF_BYCOMMAND|(test==Scenario::Disabled?MF_GRAYED:MF_ENABLED));
        SetTimer(owner,1,30,nullptr);
        TrackPopupMenuEx(fileMenu,0,20,20,owner,nullptr);
        KillTimer(owner,1);
        MSG pending{};while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE))DispatchMessageW(&pending);
        const bool opens=test!=Scenario::Arrow && test!=Scenario::Disabled;
        const bool child=test==Scenario::ExpandedLabel || test==Scenario::Browse;
        const bool ok=geometryValid && commands==unsigned(opens) && browsers==unsigned(opens) &&
            (!child || selectedRecent) && (test==Scenario::Browse || consumed==opens);
        std::printf("RECENT language=%d scenario=%u native_child_selected=%d consumed=%d commands=%u browsers=%u %s\n",
            language,unsigned(test),selectedRecent,consumed,commands,browsers,ok?"PASS":"FAIL");
        failures+=!ok;
    }
    recent_projects::HandleMessage(app,WM_DESTROY,0,0);
    SetMenu(owner,nullptr);DestroyWindow(owner);DestroyMenu(bar);app=nullptr;storage.reset();
    UnregisterClassW(name,cls.hInstance);MH_Uninitialize();
    SetThreadDesktop(previous);CloseDesktop(desktop);
    return failures?1:0;
}
