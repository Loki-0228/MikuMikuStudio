#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/plugin_menu.h"
using namespace mikumikustudio;
void Check(bool ok,const char* message){if(!ok){std::printf("FAIL: %s\n",message);std::exit(1);}}
std::wstring Caption(HMENU popup,UINT id){wchar_t text[256]{};GetMenuStringW(popup,id,text,256,MF_BYCOMMAND);return text;}
struct Fixture {
    const char* id;
    uint32_t first=0,calls=0;
    uint64_t token=0;
    bool checked=false,removeOnCommand=false;
    MmsMenuHostApi* host;
    static const wchar_t* __cdecl Title(void*,uint32_t language){return language==2?L"插件":L"Plugin";}
    static void __cdecl Populate(void* p,HMENU popup,uint32_t first,uint32_t language){
        auto& f=*static_cast<Fixture*>(p);f.first=first;
        while(GetMenuItemCount(popup)>0)DeleteMenu(popup,0,MF_BYPOSITION);
        AppendMenuW(popup,MF_STRING|(f.checked?MF_CHECKED:0),first,language==2?L"测试命令":L"Test command");
    }
    static void __cdecl Command(void* p,uint32_t offset){
        auto& f=*static_cast<Fixture*>(p);Check(offset==0,"DLL receives command offset");++f.calls;f.checked=!f.checked;
        if(f.removeOnCommand)f.host->unregisterMenu(f.token);
    }
    static int __cdecl Shortcut(void* p,uint32_t key,uint32_t modifiers){if(key!=VK_F12||modifiers)return 0;Command(p,0);return 1;}
    MmsPluginMenu Description(HWND window){return {sizeof(MmsPluginMenu),MMS_MENU_ABI_VERSION,window,id,this,Title,Populate,Command,Shortcut};}
};
int main(int argc,char** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    auto app=std::make_unique<MMDApp>();
    HWND window=CreateWindowExW(0,L"STATIC",L"Hidden plugin menu check",WS_OVERLAPPED,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"create hidden window");
    HMENU menu=CreateMenu(),file=CreatePopupMenu();AppendMenuW(file,MF_STRING,200,L"Exit");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"File");SetMenu(window,menu);
    app->MainWindow()=window;app->EnglishUI()=2;
    plugin_menu::Refresh(app.get());plugin_menu::Refresh(app.get());
    Check(GetMenuItemCount(menu)==1&&!mmv4mms::Active(),"no built-in plugin menu");
    if(argc>1&&std::strcmp(argv[1],"--absent")==0){
        mmv4mms::Initialize(app.get());
        Check(GetMenuItemCount(menu)==1&&!mmv4mms::Active(),"missing or invalid DLL does not register menu");
        std::printf("plugin_absent=1 menu_count=%d expected=1\n",GetMenuItemCount(menu));
        mmv4mms::Shutdown();DestroyWindow(window);return 0;
    }
    if(argc>1&&std::strcmp(argv[1],"--real")==0){
        mmv4mms::Initialize(app.get());
        Check(mmv4mms::Active()&&GetMenuItemCount(menu)==2,"real DLL registers its own menu");
        HMENU popup=GetSubMenu(menu,1);const UINT first=GetMenuItemID(popup,0);
        Check(Caption(popup,first)==L"效果映射(&M)…","DLL supplies Chinese labels");
        Check(plugin_menu::HandleCommand(app.get(),first+1)&&!mmv4mms::Active(),"DLL command disables renderer");
        Check(plugin_menu::HandleCommand(app.get(),first+1)&&mmv4mms::Active(),"DLL command enables renderer");
        BYTE savedKeys[256]{};GetKeyboardState(savedKeys);BYTE shortcutKeys[256]{};
        shortcutKeys[VK_CONTROL]=shortcutKeys[VK_SHIFT]=0x80;SetKeyboardState(shortcutKeys);
        const bool shortcut=plugin_menu::HandleShortcut(app.get(),'E')&&!mmv4mms::Active();
        const bool restored=plugin_menu::HandleShortcut(app.get(),'E')&&mmv4mms::Active();SetKeyboardState(savedKeys);
        Check(shortcut&&restored,"DLL owns Ctrl+Shift+E shortcut");
        const UINT checked=GetMenuState(popup,first+2,MF_BYCOMMAND)&MF_CHECKED;
        Check(plugin_menu::HandleCommand(app.get(),first+2)&&(GetMenuState(popup,first+2,MF_BYCOMMAND)&MF_CHECKED)!=checked,"DLL owns auto-reload toggle");
        app->EnglishUI()=1;plugin_menu::Refresh(app.get());
        Check(Caption(popup,first)==L"Effect Mapping(&M)...","DLL supplies English labels");
        app->EnglishUI()=0;plugin_menu::Refresh(app.get());
        Check(Caption(popup,first)==L"エフェクト割り当て(&M)…","DLL supplies Japanese labels");
        HMENU replacement=CreateMenu();SetMenu(window,replacement);DestroyMenu(menu);plugin_menu::Refresh(app.get());menu=replacement;
        Check(GetMenuItemCount(menu)==1,"DLL menu survives menu resource replacement");
        mmv4mms::Shutdown();Check(GetMenuItemCount(menu)==0&&!GetModuleHandleW(L"MMV4MMS.dll"),"DLL unload unregisters menu and releases module");
        Check(!plugin_menu::HandleCommand(app.get(),first),"stale plugin command is ignored");
        Check(!IsWindowVisible(window),"real plugin test stays hidden");
        DestroyWindow(window);std::puts("PASS: real DLL registration, three languages, toggles, replacement, unload");return 0;
    }
    const auto get=reinterpret_cast<MmsGetMenuHostApiFn>(GetProcAddress(GetModuleHandleW(nullptr),"MMSGetMenuHostApi"));
    MmsMenuHostApi api{sizeof(api)};
    Check(get&&get(MMS_MENU_ABI_VERSION,&api),"public host export is available");
    Check(!get(999,&api),"unsupported ABI rejected");
    MmsMenuHostApi shortApi{4};Check(!get(MMS_MENU_ABI_VERSION,&shortApi),"short ABI rejected");
    HMENU legacy=CreatePopupMenu();AppendMenuW(legacy,MF_STRING,40000,L"Legacy MME");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(legacy),L"MMEffect");
    Fixture fixtures[]={{"MMHack",0,0,0,false,false,&api},{"MME",0,0,0,false,false,&api},{"MMV",0,0,0,false,false,&api}};
    for(auto& f:fixtures){auto d=f.Description(window);f.token=api.registerMenu(&d);Check(f.token!=0,"independent plugin registration");Check(!api.registerMenu(&d),"duplicate plugin rejected");}
    Check(GetMenuItemCount(menu)==5,"only registered plugins add entries");
    for(int i=0;i<3;++i){
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_SUBMENU|MIIM_FTYPE;Check(GetMenuItemInfoW(menu,i+2,TRUE,&item)&&(item.fType&MFT_RIGHTJUSTIFY),"right-aligned plugin menu");
        Check(Caption(item.hSubMenu,fixtures[i].first)==L"测试命令","callback labels reach menu");
        Check(plugin_menu::HandleCommand(app.get(),fixtures[i].first)&&fixtures[i].calls==1,"commands reach correct plugin");
        Check(GetMenuState(item.hSubMenu,fixtures[i].first,MF_BYCOMMAND)&MF_CHECKED,"callback check state refreshed");
    }
    Check(!plugin_menu::HandleCommand(app.get(),40000)&&Caption(legacy,40000)==L"Legacy MME","legacy MME menu is not intercepted");
    Check(fixtures[0].first!=fixtures[1].first&&fixtures[1].first!=fixtures[2].first,"command ranges do not overlap");
    BYTE keys[256]{};GetKeyboardState(keys);BYTE clear[256]{};SetKeyboardState(clear);
    Check(plugin_menu::HandleShortcut(app.get(),VK_F12)&&fixtures[0].calls==2&&fixtures[1].calls==1,"shortcut dispatch stops at first handler");SetKeyboardState(keys);
    HMENU replacement=CreateMenu();SetMenu(window,replacement);DestroyMenu(menu);menu=replacement;
    app->EnglishUI()=1;plugin_menu::Refresh(app.get());plugin_menu::Refresh(app.get());
    Check(GetMenuItemCount(menu)==3&&Caption(GetSubMenu(menu,0),fixtures[0].first)==L"Test command","replacement and localization are idempotent");
    const UINT retired=fixtures[0].first;fixtures[0].removeOnCommand=true;
    Check(plugin_menu::HandleCommand(app.get(),retired)&&GetMenuItemCount(menu)==2,"command can unregister itself");
    Check(!plugin_menu::HandleCommand(app.get(),retired),"retired commands ignored");
    auto d=fixtures[0].Description(window);fixtures[0].token=api.registerMenu(&d);
    Check(fixtures[0].token&&fixtures[0].first!=retired,"re-registration never reuses stale IDs");
    Check(!IsWindowVisible(window),"all checks run hidden");
    DestroyWindow(window);for(auto& f:fixtures)api.unregisterMenu(f.token);
    app->MainWindow()=nullptr;mmv4mms::Shutdown();
    std::puts("PASS: absent plugins, public ABI, independent plugins, command isolation, legacy menus, localization, shortcuts, replacement, unregister, window cleanup");return 0;
}
