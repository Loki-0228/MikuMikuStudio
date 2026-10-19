#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/plugin_menu.h"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio::plugin_menu {
namespace {
struct Entry {
    MmsPluginMenu api{};
    std::string id;
    std::wstring caption;
    uint64_t token=0;
    UINT first=0;
    HMENU root=nullptr,popup=nullptr;
    HMODULE module=nullptr;
    ~Entry(){if(module)FreeLibrary(module);}
};
std::vector<std::shared_ptr<Entry>> entries;
std::map<HWND,uint32_t> languages;
uint64_t nextToken=1;
UINT nextCommand=0xd000;
bool refreshing=false;
bool UiThread(HWND window){return window&&GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId();}
bool Alive(const std::shared_ptr<Entry>& e){return std::find(entries.begin(),entries.end(),e)!=entries.end();}
int Position(const Entry& e){
    if(!IsMenu(e.root))return -1;
    for(int i=0;i<GetMenuItemCount(e.root);++i){
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_DATA|MIIM_SUBMENU;
        if(GetMenuItemInfoW(e.root,i,TRUE,&item)&&item.dwItemData==reinterpret_cast<ULONG_PTR>(&e)&&item.hSubMenu==e.popup)return i;
    }
    return -1;
}
void Detach(Entry& e){
    const int position=Position(e);
    if(position>=0){RemoveMenu(e.root,position,MF_BYPOSITION);DestroyMenu(e.popup);}
    e.root=e.popup=nullptr;
}
void ShutdownWindow(HWND window){
    for(auto i=entries.begin();i!=entries.end();)if((*i)->api.owner==window){Detach(**i);i=entries.erase(i);}else ++i;
    languages.erase(window);
}
LRESULT CALLBACK Lifetime(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(msg==WM_NCDESTROY){ShutdownWindow(hwnd);RemoveWindowSubclass(hwnd,Lifetime,id);}
    return DefSubclassProc(hwnd,msg,wp,lp);
}
void RefreshWindow(HWND window,uint32_t language){
    if(!UiThread(window)||refreshing)return;
    languages[window]=language;
    HMENU root=GetMenu(window);if(!root)return;
    struct Scope {Scope(){refreshing=true;}~Scope(){refreshing=false;}} scope;
    const auto snapshot=entries;
    bool barChanged=false;
    for(const auto& e:snapshot)if(e->api.owner==window&&Alive(e)){
        const wchar_t* label=e->api.title(e->api.context,language);
        const std::wstring caption=label?label:L"";
        if(e->root!=root||Position(*e)<0){
            Detach(*e);
            HMENU popup=CreatePopupMenu();if(!popup)continue;
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_SUBMENU|MIIM_STRING|MIIM_FTYPE|MIIM_DATA;
            item.fType=MFT_STRING|MFT_RIGHTJUSTIFY;item.hSubMenu=popup;
            item.dwItemData=reinterpret_cast<ULONG_PTR>(e.get());
            item.dwTypeData=const_cast<wchar_t*>(caption.c_str());
            if(!InsertMenuItemW(root,GetMenuItemCount(root),TRUE,&item)){DestroyMenu(popup);continue;}
            e->root=root;e->popup=popup;e->caption=caption;barChanged=true;
        }
        if(e->caption!=caption) {
            MENUITEMINFOW title{sizeof(title)};title.fMask=MIIM_STRING;
            title.dwTypeData=const_cast<wchar_t*>(caption.c_str());
            if(SetMenuItemInfoW(root,Position(*e),TRUE,&title)) {
                e->caption=caption;barChanged=true;
            }
        }
        e->api.populate(e->api.context,e->popup,e->first,language);
    }
    // Dynamic popup contents still refresh on every request. Only changing a
    // top-level entry needs to repaint the bar, never an empty/stable refresh.
    if(barChanged)DrawMenuBar(window);
}
uint64_t __cdecl Register(const MmsPluginMenu* api){
    if(!api||api->structureSize<sizeof(*api)||api->abiVersion!=MMS_MENU_ABI_VERSION||
       !UiThread(api->owner)||!api->pluginId||!*api->pluginId||!api->title||!api->populate||!api->command||refreshing)return 0;
    for(const auto& e:entries)if(e->api.owner==api->owner&&e->id==api->pluginId)return 0;
    std::shared_ptr<Entry> e;
    try{
        // Keep allocated ranges distinct, including retired registrations, so
        // queued WM_COMMANDs can never invoke a replacement DLL's command.
        HMENU root=GetMenu(api->owner);
        for(;nextCommand+MMS_MENU_COMMAND_COUNT<=0xf000;nextCommand+=MMS_MENU_COMMAND_COUNT){
            bool collision=false;
            for(UINT i=0;i<MMS_MENU_COMMAND_COUNT;++i)if(root&&GetMenuState(root,nextCommand+i,MF_BYCOMMAND)!=UINT(-1)){collision=true;break;}
            if(!collision)break;
        }
        if(nextCommand+MMS_MENU_COMMAND_COUNT>0xf000)return 0;
        e=std::make_shared<Entry>();e->api=*api;e->id=api->pluginId;e->api.pluginId=e->id.c_str();
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(api->populate),&module))return 0;
        e->module=module;e->token=nextToken++;e->first=nextCommand;nextCommand+=MMS_MENU_COMMAND_COUNT;
        if(!SetWindowSubclass(api->owner,Lifetime,1,0))return 0;
        entries.push_back(e);RefreshWindow(api->owner,languages[api->owner]);return e->token;
    }catch(...){
        // A failed initial refresh must not retain a callback whose caller
        // received no token and may immediately destroy its context.
        if(e){Detach(*e);entries.erase(std::remove(entries.begin(),entries.end(),e),entries.end());}
        return 0;
    }
}
void __cdecl Unregister(uint64_t token){
    if(refreshing)return;
    const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e->token==token;});
    if(found==entries.end()||!UiThread((*found)->api.owner))return;
    const auto e=*found;Detach(*e);entries.erase(found);DrawMenuBar(e->api.owner);
}
void __cdecl RefreshToken(uint64_t token){
    try{
        const auto snapshot=entries;
        for(const auto& e:snapshot)if(e->token==token&&UiThread(e->api.owner)){RefreshWindow(e->api.owner,languages[e->api.owner]);return;}
    }catch(...){} // Never unwind across the C ABI.
}
}
void Refresh(MMDApp* app){if(app)RefreshWindow(app->MainWindow(),app->EnglishUI());}
void Shutdown(MMDApp* app){if(app&&UiThread(app->MainWindow()))ShutdownWindow(app->MainWindow());}
bool HandleCommand(MMDApp* app,unsigned command){
    if(!app)return false;
    const auto snapshot=entries;
    for(const auto& e:snapshot)if(e->api.owner==app->MainWindow()&&command>=e->first&&command<e->first+MMS_MENU_COMMAND_COUNT){
        e->api.command(e->api.context,command-e->first);Refresh(app);return true;
    }
    return false;
}
bool HandleShortcut(MMDApp* app,unsigned key){
    if(!app)return false;
    const uint32_t modifiers=((GetKeyState(VK_CONTROL)&0x8000)?MMS_MENU_CONTROL:0u)|
        ((GetKeyState(VK_SHIFT)&0x8000)?MMS_MENU_SHIFT:0u)|((GetKeyState(VK_MENU)&0x8000)?MMS_MENU_ALT:0u);
    const auto snapshot=entries;
    for(const auto& e:snapshot)if(Alive(e)&&e->api.owner==app->MainWindow()&&e->api.shortcut&&e->api.shortcut(e->api.context,key,modifiers)){
        Refresh(app);return true;
    }
    return false;
}
}
extern "C" __declspec(dllexport) int __cdecl MMSGetMenuHostApi(uint32_t version,MmsMenuHostApi* out){
    if(!out||out->structureSize<sizeof(*out)||version!=MMS_MENU_ABI_VERSION)return 0;
    using namespace mikumikustudio::plugin_menu;
    *out={sizeof(*out),MMS_MENU_ABI_VERSION,Register,Unregister,RefreshToken};return 1;
}
