#include "../io/project_open.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "settings_json.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_message.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mikumikustudio::recent_projects {
namespace {
constexpr UINT first=34800, browse=34810;
MMDApp* owner=nullptr;
HMENU fileMenu=nullptr, recentMenu=nullptr;
HHOOK hook=nullptr;
bool openSelected=false, recentSelected=false, filePopupOpen=false;
int openPosition=-1;
using Paths = std::vector<std::wstring>;
using settings_json::Json;
using system_settings::Result;
constexpr char recentSection[] = "recentProjects";
constexpr wchar_t legacyFile[] = L"recent-pmm.txt";
constexpr std::size_t maximumPaths = 10;
Paths menuPaths;
std::filesystem::path LegacyStorePath() {
    wchar_t path[32768]{};
    DWORD n=GetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",path,32768);
    if(n && n<32768)return path;
    return local_data::File(legacyFile);
}
bool ExplicitStore() { wchar_t value[2]{}; return GetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",value,2)!=0; }
std::string RecentJson(Json section, const Paths& paths) {
    Json entries;
    entries.kind=Json::Array;
    for(const auto& path:paths) {
        Json entry;
        entry.kind=Json::StringValue;
        entry.text=plugins::Utf8(path);
        entries.array.push_back(std::move(entry));
    }
    section.kind=Json::Object;
    section.object["paths"]=std::move(entries);
    return settings_json::Serialize(section);
}
Paths ReadCentralPaths(const Json& section) {
    Paths paths;
    const auto found=section.object.find("paths");
    if(found==section.object.end() || found->second.kind!=Json::Array)return paths;
    for(const auto& entry:found->second.array) {
        if(paths.size()>=maximumPaths)break;
        if(entry.kind!=Json::StringValue)continue;
        std::wstring path;
        try { path=plugins::Wide(entry.text); } catch(const std::invalid_argument&) { continue; }
        if(!path.empty() && path.size()<32768 && path.find(L'\0')==std::wstring::npos)
            paths.push_back(std::move(path));
    }
    return paths;
}
Paths ReadLegacy(const std::filesystem::path& path) {
    Paths paths;
    std::ifstream input(path,std::ios::binary);
    std::wstring line;wchar_t c=0;
    while(input.read(reinterpret_cast<char*>(&c),sizeof(c))) {
        if(c==L'\n') { if(!line.empty() && paths.size()<maximumPaths)paths.push_back(line);line.clear(); }
        else if(c!=L'\r' && c!=0xfeff && line.size()<32767)line+=c;
    }
    return paths;
}
Paths Read() {
    if(ExplicitStore())return ReadLegacy(LegacyStorePath());
    std::string raw;
    const auto result=system_settings::ReadSection(recentSection,raw);
    if(result==Result::Ok) {
        try { return ReadCentralPaths(settings_json::Object(raw)); } catch(...) { return {}; }
    }
    if(result!=Result::Missing)return {};
    auto paths=ReadLegacy(local_data::File(legacyFile));
    system_settings::CompareExchangeSection(recentSection,nullptr,RecentJson({},paths));
    return paths;
}
void WriteLegacy(const std::filesystem::path& path, const Paths& paths) {
    if(path.empty())return;
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);
    auto temp=path;temp+=L".tmp";
    { std::ofstream output(temp,std::ios::binary|std::ios::trunc);
      const wchar_t bom=0xfeff;output.write(reinterpret_cast<const char*>(&bom),sizeof(bom));
      for(const auto& p:paths){auto line=p+L"\r\n";output.write(reinterpret_cast<const char*>(line.data()),line.size()*sizeof(wchar_t));}
      output.flush();if(!output)return; }
    MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
void Promote(Paths& paths, const wchar_t* path) {
    paths.erase(std::remove_if(paths.begin(),paths.end(),
        [path](const auto& entry){return !_wcsicmp(entry.c_str(),path);}),paths.end());
    paths.insert(paths.begin(),path);
    if(paths.size()>maximumPaths)paths.resize(maximumPaths);
}
void RecordCentral(const wchar_t* path) {
    constexpr unsigned maximumAttempts = 4;
    for(unsigned attempt=0;attempt<maximumAttempts;++attempt) {
        std::string raw;
        const auto result=system_settings::ReadSection(recentSection,raw);
        Json section;
        Paths paths;
        if(result==Result::Ok) {
            try {
                section=settings_json::Object(raw);
                paths=ReadCentralPaths(section);
            } catch(...) { return; }
        } else if(result==Result::Missing) {
            paths=ReadLegacy(local_data::File(legacyFile));
        } else {
            return;
        }
        Promote(paths,path);
        // Keep the unfiltered read as the compare token and replace only paths.
        // A concurrent update requires a fresh read before promoting again.
        if(system_settings::CompareExchangeSection(recentSection,result==Result::Ok?&raw:nullptr,
            RecentJson(std::move(section),paths))!=Result::Conflict)return;
    }
}
const wchar_t* Text(MMDApp* app,const wchar_t* jp,const wchar_t* en,const wchar_t* zh) {
    return app->EnglishUI()==2?zh:app->EnglishUI()==1?en:jp;
}
bool Exists(const std::wstring& path) {
    const auto attributes=GetFileAttributesW(path.c_str());
    return path.size()<kPathFieldCapacity && attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
void RefreshAvailability() {
    // Attach already populated these rows before the menu loop was painted.
    // Files can disappear while the loop is open, but that only changes state;
    // deleting and appending the same rows would invalidate the live popup.
    for(size_t i=0;i<menuPaths.size();++i) {
        const UINT command=first+static_cast<UINT>(i);
        const UINT previous=GetMenuState(recentMenu,command,MF_BYCOMMAND);
        const bool enabled=Exists(menuPaths[i]);
        const bool wasEnabled=(previous&(MF_DISABLED|MF_GRAYED))==0;
        if(previous!=UINT(-1) && enabled!=wasEnabled)
            EnableMenuItem(recentMenu,command,MF_BYCOMMAND|(enabled?MF_ENABLED:MF_GRAYED));
    }
}
void Populate(MMDApp* app) {
    while(GetMenuItemCount(recentMenu)>0)DeleteMenu(recentMenu,0,MF_BYPOSITION);
    menuPaths=Read();
    AppendMenuW(recentMenu,MF_STRING|MF_GRAYED,0,Text(app,L"最近開いた PMM",L"Recently opened PMM",L"最近打开的 PMM"));
    AppendMenuW(recentMenu,MF_SEPARATOR,0,nullptr);
    for(size_t i=0;i<menuPaths.size();++i) {
        const auto& path=menuPaths[i];std::wstring label=std::to_wstring(i+1)+L"  ";
        for(wchar_t c:path){label+=c;if(c==L'&')label+=c;}
        AppendMenuW(recentMenu,MF_STRING|(Exists(path)?0:MF_GRAYED),first+static_cast<UINT>(i),label.c_str());
    }
    if(menuPaths.empty())AppendMenuW(recentMenu,MF_STRING|MF_GRAYED,0,Text(app,L"履歴はありません",L"No recent projects",L"暂无最近工程"));
    AppendMenuW(recentMenu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(recentMenu,MF_STRING,browse,Text(app,L"参照…",L"Browse…",L"浏览文件…"));
}
void Attach(MMDApp* app) {
    HMENU menu=GetMenu(app->MainWindow());if(!menu)return;
    HMENU file=GetSubMenu(menu,0);if(!file)return;
    for(int i=0;i<GetMenuItemCount(file);++i) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU;
        if(!GetMenuItemInfoW(file,i,TRUE,&item) || item.wID!=205)continue;
        owner=app;fileMenu=file;openPosition=i;
        if(!item.hSubMenu) {
            recentMenu=CreatePopupMenu();item.fMask=MIIM_SUBMENU;item.hSubMenu=recentMenu;
            SetMenuItemInfoW(file,i,TRUE,&item);
        } else recentMenu=item.hSubMenu;
        Populate(app);return;
    }
}
}
void Record(MMDApp* app,const wchar_t* path) {
#if !MMD_ORIGINAL_PROFILE
    if(!app || !path || !*path || !pmm_paths::IsProject(path))return;
    wchar_t full[32768]{};DWORD size=GetFullPathNameW(path,32768,full,nullptr);if(!size || size>=32768)return;
    if(ExplicitStore()) {
        const auto legacy=LegacyStorePath();
        auto paths=ReadLegacy(legacy);
        Promote(paths,full);
        WriteLegacy(legacy,paths);
    } else {
        RecordCentral(full);
    }
#endif
}
LRESULT CALLBACK MenuFilter(int code,WPARAM wp,LPARAM lp) {
    if(code==MSGF_MENU && owner && openPosition>=0) {
        const auto& msg=*reinterpret_cast<MSG*>(lp);
        bool activate=msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && openSelected;
        if(msg.message==WM_LBUTTONUP && filePopupOpen && (openSelected || recentSelected)) {
            // The recent submenu can own the selection while the pointer is
            // still over Open. Locate the visible File popup for the hit test.
            RECT bounds{};activate=GetMenuItemRect(nullptr,fileMenu,openPosition,&bounds) && PtInRect(&bounds,msg.pt);
            // Keep the arrow available for native submenu navigation.
            if(activate && msg.pt.x>=bounds.right-GetSystemMetrics(SM_CXMENUCHECK)-8)activate=false;
        }
        if(activate && !(GetMenuState(fileMenu,openPosition,MF_BYPOSITION)&(MF_DISABLED|MF_GRAYED))) {
            EndMenu();PostMessageW(owner->MainWindow(),WM_COMMAND,205,0);return 1;
        }
    }
    return CallNextHookEx(hook,code,wp,lp);
}
bool HandleMessage(MMDApp* app,UINT msg,WPARAM wp,LPARAM lp) {
#if !MMD_ORIGINAL_PROFILE
    if(msg==WM_ENTERMENULOOP) {
        owner=app;openSelected=recentSelected=filePopupOpen=false;
        hook=SetWindowsHookExW(WH_MSGFILTER,MenuFilter,nullptr,GetCurrentThreadId());
        if(hook)Attach(app);
    } else if(msg==WM_INITMENUPOPUP) {
        if(fileMenu && reinterpret_cast<HMENU>(wp)==fileMenu)filePopupOpen=true;
        if(recentMenu && reinterpret_cast<HMENU>(wp)==recentMenu)RefreshAvailability();
    } else if(msg==WM_UNINITMENUPOPUP) {
        const auto popup=reinterpret_cast<HMENU>(wp);
        if(fileMenu && popup==fileMenu)openSelected=recentSelected=filePopupOpen=false;
        if(recentMenu && popup==recentMenu)recentSelected=false;
    } else if(msg==WM_MENUSELECT) {
        const auto popup=reinterpret_cast<HMENU>(lp);
        openSelected=fileMenu && popup==fileMenu &&
            ((HIWORD(wp)&MF_POPUP)?LOWORD(wp)==openPosition:LOWORD(wp)==205);
        recentSelected=recentMenu && popup==recentMenu;
    } else if(msg==WM_EXITMENULOOP || msg==WM_DESTROY) {
        if(hook){UnhookWindowsHookEx(hook);hook=nullptr;}openSelected=recentSelected=filePopupOpen=false;
        if(IsMenu(fileMenu) && openPosition>=0) {
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_SUBMENU;item.hSubMenu=nullptr;
            SetMenuItemInfoW(fileMenu,openPosition,TRUE,&item);
        }
        if(IsMenu(recentMenu))DestroyMenu(recentMenu);
        recentMenu=fileMenu=nullptr;openPosition=-1;
        if(msg==WM_DESTROY)owner=nullptr;
    } else if(msg==WM_COMMAND && HIWORD(wp)==0) {
        UINT id=LOWORD(wp);
        if(id==browse){PostMessageW(app->MainWindow(),WM_COMMAND,205,0);return true;}
        if(id>=first && id<first+menuPaths.size()) {
            const auto path=menuPaths[id-first];
            const auto attributes=GetFileAttributesW(path.c_str());
            if(path.size()>=kPathFieldCapacity || attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) {
                ShowUiMessage(app,app->MainWindow(),UiMessage::OpenProjectError,MB_OK|MB_ICONERROR,path.c_str());return true;
            }
            ProjectOpenUiScope open(app);
            if (!open.Prepare() || !open.ConfirmDiscard(true)) return true;
            (void)LoadSceneFile(path.c_str());return true;
        }
    }
#endif
    return false;
}
}
