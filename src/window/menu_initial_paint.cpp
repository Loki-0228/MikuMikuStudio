#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/menu_initial_paint.hpp"
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <vector>

namespace mikumikustudio::menu_initial_paint {
namespace {
constexpr UINT_PTR kTimer=0x4d4e5054, kSubclass=0x4d4e5054;
struct Opening {
    HMENU menu=nullptr;
    ULONGLONG opened=0, visibleSince=0;
    bool first=false, idle=false, finished=false;
};
// A dialog, child control and main window may all own popups on one thread.
// Closing one owner must not cancel another owner's pending first paint.
thread_local std::map<HWND,std::vector<Opening>> owners;
struct Visit {HWND owner; bool idle;};
BOOL CALLBACK Paint(HWND popup, LPARAM param) {
    if(!IsWindowVisible(popup) || GetWindowThreadProcessId(popup,nullptr)!=GetCurrentThreadId())return TRUE;
    wchar_t name[32]{};GetClassNameW(popup,name,32);
    if(lstrcmpW(name,L"#32768"))return TRUE;
    MENUBARINFO info{sizeof(info)};
    if(!GetMenuBarInfo(popup,OBJID_CLIENT,0,&info))return TRUE;
    const auto& visit=*reinterpret_cast<const Visit*>(param);
    auto owner=owners.find(visit.owner);if(owner==owners.end())return TRUE;
    for(auto& opening:owner->second)if(opening.menu==info.hMenu && !opening.finished) {
        const auto now=GetTickCount64();
        const bool final=opening.first && now-opening.visibleSince>=220;
        const bool repaint=!opening.first || (visit.idle && !opening.idle) || final;
        if(visit.idle)opening.idle=true;
        if(repaint) {
            if(!opening.first)opening.visibleSince=now;
            opening.first=true;opening.finished=final;
            // Set state before RedrawWindow, which can reenter the owner.
            RedrawWindow(popup,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
        }
        break;
    }
    return TRUE;
}
LRESULT CALLBACK OwnerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_TIMER && HandleMessage(hwnd,msg,wp,lp))return 0;
    const auto result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg!=WM_TIMER)HandleMessage(hwnd,msg,wp,lp);
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,OwnerProc,id);
    return result;
}
}
bool HandleMessage(HWND owner,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_INITMENUPOPUP) {
        const auto menu=reinterpret_cast<HMENU>(wp);
        auto& list=owners[owner];
        auto found=std::find_if(list.begin(),list.end(),[&](const auto& item){return item.menu==menu;});
        if(found==list.end())list.push_back({menu,GetTickCount64()});
        else *found={menu,GetTickCount64()};
        SetTimer(owner,kTimer,16,nullptr);
    } else if(msg==WM_UNINITMENUPOPUP) {
        auto found=owners.find(owner);
        if(found!=owners.end()) {
            auto& list=found->second;
            list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& item){return item.menu==reinterpret_cast<HMENU>(wp);}),list.end());
            if(list.empty()){KillTimer(owner,kTimer);owners.erase(found);}
        }
    } else if(msg==WM_EXITMENULOOP || msg==WM_DESTROY || msg==WM_NCDESTROY) {
        KillTimer(owner,kTimer);owners.erase(owner);
    } else if(msg==WM_TIMER && wp==kTimer) {
        Visit visit{owner,false};EnumThreadWindows(GetCurrentThreadId(),Paint,reinterpret_cast<LPARAM>(&visit));
        auto found=owners.find(owner);const auto now=GetTickCount64();
        if(found==owners.end() || std::none_of(found->second.begin(),found->second.end(),[&](const auto& item){return !item.finished && now-(item.first?item.visibleSince:item.opened)<600;}))
            KillTimer(owner,kTimer);
        return true;
    } else if(msg==WM_ENTERIDLE && wp==MSGF_MENU) {
        Visit visit{owner,true};Paint(reinterpret_cast<HWND>(lp),reinterpret_cast<LPARAM>(&visit));
    }
    return false;
}
void Attach(HWND owner) {
    if(owner && GetWindowThreadProcessId(owner,nullptr)==GetCurrentThreadId())
        SetWindowSubclass(owner,OwnerProc,kSubclass,0);
}
}
