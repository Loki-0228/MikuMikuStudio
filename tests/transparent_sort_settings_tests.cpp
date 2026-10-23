// MAINTENANCE: regression-keep. Hidden native commands and real PMM settings; no renderer pixel or third-party MME coverage.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/ui_translation.hpp"

namespace mikumikustudio { void CmdViewMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace ext = mikumikustudio::pmm_extension;
namespace settings = mikumikustudio::pmm_settings;
namespace {
unsigned checks = 0;
DWORD hostThread = 0;
constexpr char scopeKey[] = "org.mikumikustudio.view.transparent_sort_cross_objects";
constexpr char tierKey[] = "org.mikumikustudio.view.transparent_sort_precision";
constexpr char unknownKey[] = "org.example.transparent-settings.opaque";
const std::vector<std::uint8_t> unknownBytes{0,255,17,0,128,99};
void Check(bool value,const char* reason) {
    ++checks;
    if (!value) { std::fprintf(stderr,"FAIL %s\n",reason); std::exit(1); }
}
std::vector<char> Bytes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void DefaultDesktop() {
    wchar_t name[256]{}; DWORD bytes=0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,name,sizeof name,&bytes) &&
        !_wcsicmp(name,L"Default"),"requires inherited Default desktop; never switches desktops");
}
bool Owned(HWND window) {
    DWORD process=0; const DWORD thread=GetWindowThreadProcessId(window,&process);
    return window && process==GetCurrentProcessId() && thread==hostThread;
}
void HiddenWindows() {
    Check(GetFocus()==nullptr && GetActiveWindow()==nullptr && GetCapture()==nullptr,
        "test thread never acquires focus, activation or mouse capture");
    EnumThreadWindows(hostThread,[](HWND window,LPARAM)->BOOL {
        Check(Owned(window) && !IsWindowVisible(window),"every test top-level window remains hidden"); return TRUE;
    },0);
    Check(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll") && !GetModuleHandleW(L"MMV4MMS.dll"),
        "no third-party effect or compatibility plugin was loaded");
}
void NativeD3D9Only() {
    if (const auto module=GetModuleHandleW(L"d3d9.dll")) {
        wchar_t loaded[32768]{},system[32768]{};
        Check(GetModuleFileNameW(module,loaded,32768)>0 && GetSystemDirectoryW(system,32768)>0 &&
            fs::equivalent(fs::path(loaded),fs::path(system)/L"d3d9.dll"),"loaded D3D9 is the system runtime, not an effect proxy");
    }
}
// Hooks affect only this thread's own HWNDs. Other threads/owners use the
// original API. No hook accepts or answers a dialog; an unexpected dialog fails.
decltype(&ShowWindow) realShow=nullptr;
decltype(&SetWindowPos) realPosition=nullptr;
decltype(&SetWindowPlacement) realPlacement=nullptr;
decltype(&SetFocus) realFocus=nullptr;
decltype(&SetActiveWindow) realActive=nullptr;
decltype(&SetForegroundWindow) realForeground=nullptr;
decltype(&MessageBoxW) realMessageW=nullptr;
decltype(&MessageBoxA) realMessageA=nullptr;
decltype(&MessageBeep) realBeep=nullptr;
BOOL WINAPI Hide(HWND window,int command) {
    return realShow(window,GetCurrentThreadId()==hostThread && Owned(window) ? SW_HIDE : command);
}
BOOL WINAPI Position(HWND window,HWND after,int x,int y,int width,int height,UINT flags) {
    if (GetCurrentThreadId()==hostThread && Owned(window)) flags=(flags & ~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    return realPosition(window,after,x,y,width,height,flags);
}
BOOL WINAPI Placement(HWND window,const WINDOWPLACEMENT* value) {
    if (GetCurrentThreadId()==hostThread && Owned(window) && value) {
        auto hidden=*value; hidden.showCmd=SW_HIDE; return realPlacement(window,&hidden);
    }
    return realPlacement(window,value);
}
HWND WINAPI Focus(HWND window) {
    if (GetCurrentThreadId()==hostThread && (!window || Owned(window))) return GetFocus();
    return realFocus(window);
}
HWND WINAPI Active(HWND window) {
    if (GetCurrentThreadId()==hostThread && (!window || Owned(window))) return GetActiveWindow();
    return realActive(window);
}
BOOL WINAPI Foreground(HWND window) {
    if (GetCurrentThreadId()==hostThread && Owned(window)) return FALSE;
    return realForeground(window);
}
int WINAPI MessageW(HWND owner,LPCWSTR text,LPCWSTR caption,UINT flags) {
    if (GetCurrentThreadId()==hostThread && (!owner || Owned(owner))) {
        std::fwprintf(stderr,L"Unexpected owned dialog: %ls / %ls\n",caption?caption:L"",text?text:L"");
        Check(false,"unexpected dialog is refused, never automatically accepted");
    }
    return realMessageW(owner,text,caption,flags);
}
int WINAPI MessageA(HWND owner,LPCSTR text,LPCSTR caption,UINT flags) {
    if (GetCurrentThreadId()==hostThread && (!owner || Owned(owner))) {
        std::fprintf(stderr,"Unexpected owned dialog: %s / %s\n",caption?caption:"",text?text:"");
        Check(false,"unexpected ANSI dialog is refused, never automatically accepted");
    }
    return realMessageA(owner,text,caption,flags);
}
BOOL WINAPI Beep(UINT kind) { return GetCurrentThreadId()==hostThread ? TRUE : realBeep(kind); }
LRESULT CALLBACK Cbt(int code,WPARAM wp,LPARAM lp) {
    if (code==HCBT_CREATEWND) {
        auto* create=reinterpret_cast<CBT_CREATEWND*>(lp);
        create->lpcs->style &= ~WS_VISIBLE; create->lpcs->dwExStyle |= WS_EX_NOACTIVATE;
        wchar_t cls[64]{}; GetClassNameW(reinterpret_cast<HWND>(wp),cls,64);
        Check(wcscmp(cls,L"#32770")!=0,"unanticipated owned modal dialog fails before visibility");
    }
    if (code==HCBT_ACTIVATE || code==HCBT_SETFOCUS) return 1;
    return CallNextHookEx(nullptr,code,wp,lp);
}
struct Guards {
    HHOOK cbt=nullptr; std::vector<void*> targets;
    template<class T> void Hook(T target,T replacement,T& original) {
        Check(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),
            reinterpret_cast<void**>(&original))==MH_OK,"install process-local guarded API trampoline");
        targets.push_back(reinterpret_cast<void*>(target));
        Check(MH_EnableHook(reinterpret_cast<void*>(target))==MH_OK,"enable only this test's explicit guard");
    }
    Guards() {
        hostThread=GetCurrentThreadId(); Check(MH_Initialize()==MH_OK,"initialize visibility guards before production startup");
        Hook(&ShowWindow,&Hide,realShow); Hook(&SetWindowPos,&Position,realPosition);
        Hook(&SetWindowPlacement,&Placement,realPlacement); Hook(&SetFocus,&Focus,realFocus);
        Hook(&SetActiveWindow,&Active,realActive); Hook(&SetForegroundWindow,&Foreground,realForeground);
        Hook(&MessageBoxW,&MessageW,realMessageW); Hook(&MessageBoxA,&MessageA,realMessageA);
        Hook(&MessageBeep,&Beep,realBeep);
        cbt=SetWindowsHookExW(WH_CBT,Cbt,nullptr,hostThread); Check(cbt!=nullptr,"install only a current-thread CBT guard");
    }
    ~Guards() {
        Check(UnhookWindowsHookEx(cbt)!=FALSE,"release current-thread CBT hook");
        for (auto at=targets.rbegin();at!=targets.rend();++at) {
            Check(MH_DisableHook(*at)==MH_OK && MH_RemoveHook(*at)==MH_OK,"remove only owned API guards");
        }
        Check(MH_Uninitialize()==MH_OK,"release local detour runtime after host shutdown");
    }
};
fs::path PrepareSandbox(const wchar_t* output) {
    const auto install=local_data::ExecutableDirectory(); Check(!install.empty(),"resolve actual copied executable directory");
    const auto marker=Bytes(install/L"transparent-settings-test-installation.txt");
    Check(std::string(marker.begin(),marker.end())=="MMS transparent settings test installation v1\n",
        "require explicit independent-installation marker");
    Check(!fs::exists(install/L"data") && !fs::exists(install/L"PLUGINS"),"refuse existing installation data and plugin directories");
    for (const auto* file : {L"d3d9.dll",L"MMEffect.dll",L"MMHack.dll",L"MMV4MMS.dll"})
        Check(!fs::exists(install/file),"copied test installation must not contain effect or renderer proxies");
    const fs::path out=fs::path(output).lexically_normal();
    Check(out.is_absolute() && !fs::exists(out) && fs::is_directory(out.parent_path()) && out!=install && out!=install/L"data",
        "output is an absolute fresh directory distinct from installation data");
    Check(fs::create_directory(out),"create fresh owned evidence directory");
    for (const auto* folder : {L"cwd",L"temp",L"profile",L"local",L"roaming",L"projects"})
        Check(fs::create_directory(out/folder),"create owned runtime sandbox subdirectory");
    Check(SetCurrentDirectoryW((out/L"cwd").c_str())!=FALSE,"set process-only isolated cwd");
    for (const auto* name : {L"TEMP",L"TMP"}) Check(SetEnvironmentVariableW(name,(out/L"temp").c_str())!=FALSE,"isolate temporary files");
    Check(SetEnvironmentVariableW(L"USERPROFILE",(out/L"profile").c_str()) &&
        SetEnvironmentVariableW(L"LOCALAPPDATA",(out/L"local").c_str()) &&
        SetEnvironmentVariableW(L"APPDATA",(out/L"roaming").c_str()),"isolate process profile fallbacks");
    Check(SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",(out/L"antialias.ini").c_str()) &&
        SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",(out/L"recent.txt").c_str()) &&
        SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG",(out/L"autosave.ini").c_str()) &&
        SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX",L"0"),"redirect optional user settings into owned sandbox");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    return out;
}
LRESULT CALLBACK MenuProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if (message==WM_COMMAND && g_Block && LOWORD(wp)>=304 && LOWORD(wp)<=309) {
        CmdViewMenu(g_Block,window,LOWORD(wp),HIWORD(wp)); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
struct Host {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    MMDApp* previous=g_Block; bool project=false; HMENU simpleMenu=nullptr;
    explicit Host(bool full) : project(full) {
        new (&app->state) MMDAppState{}; g_Block=app.get();
        if (project) {
            processor::Configuration configuration; configuration.vertices.mode=processor::Mode::Cpu; configuration.mmv.mode=processor::Mode::Cpu;
            std::wstring error; Check(processor::SaveRequested(configuration,error),"save CPU vertex preference before startup calibration");
            Check(processor::Initialize(error),"freeze explicit CPU vertex preference using the production startup settings entry");
            app->InitDefaults();
            Check(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize actual production host under visibility/focus guards");
            Check(skinning::GetPreference().mode==skinning::Mode::Cpu && skinning::GetStatus().automaticCalibrations==0,
                "settings roundtrip never starts automatic vertex calibration");
        } else {
            WNDCLASSW cls{}; cls.lpfnWndProc=MenuProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"MMS transparent settings hidden probe";
            Check(RegisterClassW(&cls)!=0,"register owned menu-only host class");
            app->Hwnd()=CreateWindowExW(WS_EX_NOACTIVATE,cls.lpszClassName,L"MMS transparent settings",WS_OVERLAPPED,
                0,0,640,480,nullptr,nullptr,cls.hInstance,nullptr);
            app->HInstance()=cls.hInstance; Check(Owned(app->MainWindow()),"create real hidden same-thread command HWND");
            simpleMenu=CreateMenu(); Check(simpleMenu!=nullptr,"create real owned menu root");
            for (int i=0;i<3;++i) Check(AppendMenuW(simpleMenu,MF_POPUP,reinterpret_cast<UINT_PTR>(CreatePopupMenu()),L"Probe"),"append owned top-level menu");
            Check(SetMenu(app->MainWindow(),simpleMenu),"attach owned menu tree");
            // Zero render dimensions intentionally exercise the existing early
            // viewport guard. Menu-only mode does not claim project startup.
            settings::Reset(*app);
        }
        HiddenWindows(); NativeD3D9Only();
    }
    ~Host() {
        HWND window=app->MainWindow();
        // Match normal application lifetime: WM_DESTROY still has its live
        // subsystems; resource cleanup follows the message-loop shutdown.
        if (IsWindow(window)) {
            if (simpleMenu) SetMenu(window,nullptr);
            Check(DestroyWindow(window),"destroy only the hidden owned main window");
        }
        if (project) ShutdownCleanup(app.get());
        if (simpleMenu) Check(DestroyMenu(simpleMenu),"release owned menu-only tree");
        g_Block=previous;
    }
};
bool Scope(const MMDApp& app) { return app.TransparentSortCrossObjects(); }
void Command(MMDApp& app,unsigned id) { SendMessageW(app.MainWindow(),WM_COMMAND,id,0); HiddenWindows(); }
HMENU Popup(MMDApp& app) {
    HMENU view=GetSubMenu(GetMenu(app.MainWindow()),2); MENUITEMINFOW item{}; item.cbSize=sizeof item; item.fMask=MIIM_SUBMENU;
    return view && GetMenuItemInfoW(view,32004,FALSE,&item) ? item.hSubMenu : nullptr;
}
std::wstring Label(HMENU menu,UINT id) { wchar_t text[256]{}; GetMenuStringW(menu,id,text,256,MF_BYCOMMAND); return text; }
void MenuChecks(MMDApp& app,bool productionLanguage) {
    settings::Reset(app); Check(!Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off,"new settings default to Off and object-local scope");
    HMENU stable=nullptr;
    const wchar_t* titles[]{L"透明ソート",L"Transparent sorting",L"透明排序"};
    const wchar_t* scopes[]{L"オブジェクト間もソート",L"Sort across objects",L"开启跨物体排序"};
    const wchar_t* headings[]{L"ソート精度",L"Sorting strength",L"排序程度"};
    const wchar_t* labels[3][5]{{L"低",L"標準",L"高",L"最高",L"オフ（既定）"},
        {L"Low",L"Standard",L"High",L"Maximum",L"Off (default)"},{L"低",L"标准",L"高",L"最高",L"关闭（默认）"}};
    for (int language : {2,1,0,2,0,1}) {
        app.EnglishUI()=static_cast<unsigned char>(language);
        if (productionLanguage) RefreshMenuLanguage(&app); else RefreshTransparentSortPrecisionMenu(&app);
        auto popup=Popup(app);
#if MMD_ORIGINAL_PROFILE
        Check(!popup && !Scope(app),"original profile exposes neither transparent-sort popup nor cross-object scope");
#else
        Check(popup && (!stable || stable==popup),"repeated language refresh retains the same real submenu HMENU"); stable=popup;
        HMENU view=GetSubMenu(GetMenu(app.MainWindow()),2); unsigned parents=0;
        for (int i=0;i<GetMenuItemCount(view);++i) {
            MENUITEMINFOW parent{}; parent.cbSize=sizeof parent; parent.fMask=MIIM_ID;
            Check(GetMenuItemInfoW(view,i,TRUE,&parent),"read actual top-level item identity without submenu-ID ambiguity");
            if (parent.wID==32004) ++parents;
        }
        Check(parents==1 && GetMenuItemCount(popup)==9,"exactly one submenu has the nine contracted positions");
        const UINT order[]{308,0,32005,304,305,306,307,0,309};
        for (UINT pos=0;pos<9;++pos) {
            MENUITEMINFOW item{}; item.cbSize=sizeof item; item.fMask=MIIM_ID|MIIM_FTYPE|MIIM_SUBMENU;
            Check(GetMenuItemInfoW(popup,pos,TRUE,&item) && !item.hSubMenu &&
                (order[pos] ? item.wID==order[pos] && !(item.fType&MFT_SEPARATOR) : (item.fType&MFT_SEPARATOR)!=0),
                "menu command identities, separators and item order are stable and unique");
        }
        Check(Label(view,32004)==titles[language] && Label(popup,309)==scopes[language] &&
            Label(popup,32005)==headings[language],"all three languages expose distinct precision heading and scope label");
        Check(GetMenuState(popup,32005,MF_BYCOMMAND)&MF_GRAYED,"strength heading is not a selectable command");
        for (unsigned tier=0;tier<5;++tier) {
            MENUITEMINFOW item{}; item.cbSize=sizeof item; item.fMask=MIIM_FTYPE;
            Check(GetMenuItemInfoW(popup,304+tier,FALSE,&item) && (item.fType&MFT_RADIOCHECK) && Label(popup,304+tier)==labels[language][tier],
                "each precision has the expected localized label and radio style");
        }
        MENUITEMINFOW scope{}; scope.cbSize=sizeof scope; scope.fMask=MIIM_FTYPE;
        Check(GetMenuItemInfoW(popup,309,FALSE,&scope) && !(scope.fType&MFT_RADIOCHECK),"scope uses a separate ordinary checkbox");
#endif
    }
    for (unsigned tier=0;tier<5;++tier) {
        for (unsigned round=0;round<2;++round) {
            const bool beforeScope=Scope(app); const auto oldTier=app.TransparentMaterialSortPrecision();
            app.SceneModified()=0; const auto frame=app.CurrentFrame();
            Command(app,304+tier);
#if MMD_ORIGINAL_PROFILE
            Check(app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off && !Scope(app) && !app.SceneModified(),"original profile precision commands cannot enable extensions");
#else
            Check(unsigned(app.TransparentMaterialSortPrecision())==tier && Scope(app)==beforeScope &&
                bool(app.SceneModified())==(unsigned(oldTier)!=tier),"precision command edits only changed precision and leaves scope independent");
            app.SceneModified()=0; Command(app,304+tier); Check(!app.SceneModified(),"reselecting known precision is a clean no-op");
#endif
            app.SceneModified()=0; const auto scopeRevision=app.SceneRevision(); Command(app,309);
#if MMD_ORIGINAL_PROFILE
            Check(!Scope(app) && !app.SceneModified(),"original profile ignores cross-object toggle");
#else
            Check(Scope(app)!=beforeScope && unsigned(app.TransparentMaterialSortPrecision())==tier && app.SceneModified(),"scope toggles and marks dirty without changing precision, including Off");
            Check(app.SceneRevision()>scopeRevision && app.PmmProjectSettings().sortingCrossObjectsEdited,
                "actual scope command advances scene revision and its independent edit marker");
            auto popup=Popup(app);
            for (unsigned value=0;value<5;++value) Check(bool(GetMenuState(popup,304+value,MF_BYCOMMAND)&MF_CHECKED)==(value==tier),"scope change preserves exactly one precision radio check");
            Check(bool(GetMenuState(popup,309,MF_BYCOMMAND)&MF_CHECKED)==Scope(app),"scope menu check follows only scope state");
#endif
            Check(app.CurrentFrame()==frame,"menu settings never move the playhead");
        }
    }
}
int Open(const fs::path& path,int mode) {
    int fd=-1; Check(_wsopen_s(&fd,path.c_str(),mode|_O_BINARY,_SH_DENYNO,_S_IREAD|_S_IWRITE)==0,"open only owned PMM fixture"); return fd;
}
ext::Document Read(const fs::path& path,std::uint64_t* boundary=nullptr,bool original=false) {
    int fd=Open(path,_O_RDONLY); char magic[30]{};
    Check(_read(fd,magic,30)==30 && !std::memcmp(magic,"Polygon Movie maker 0002",24),"real writer retains standard PMM v2 header");
    ext::Document document; std::uint64_t size=0; const auto result=ext::Read(fd,document,size);
    Check(result==(original ? ext::ReadResult::Absent : ext::ReadResult::Loaded),"real PMM extension presence matches build profile");
    if (original) size=static_cast<std::uint64_t>(_filelengthi64(fd));
    _close(fd); if (boundary) *boundary=size; return document;
}
void Has(const ext::Document& doc,const char* key,const std::vector<std::uint8_t>& expected) {
    const auto* record=ext::Find(doc,key); Check(record && record->value==expected,"serialized record retains exact expected key and byte payload");
}
void Save(MMDApp& app,const fs::path& path) {
    Check(wcscpy_s(app.EnvFileName(),256,path.c_str())==0,"set owned short output path");
    app.SceneModified()=1; Check(SaveSceneFile(&app) && !app.SceneModified() && fs::file_size(path)>30,"actual synchronous production PMM writer succeeds");
    Check(!app.PmmProjectSettings().sortingCrossObjectsEdited && !app.PmmProjectSettings().sortingPrecisionEdited,
        "completed unchanged save clears both independent edited markers"); HiddenWindows();
}
void Load(MMDApp& app,const fs::path& path) {
    Check(LoadSceneFile(path.c_str()),"actual production PMM load completes");
    Check(!app.SceneModified(),"opening settings is not an unsaved edit"); HiddenWindows();
}
void Fixture(const fs::path& source,const fs::path& path,std::uint64_t boundary,const ext::Document* doc) {
    Check(fs::copy_file(source,path,fs::copy_options::overwrite_existing),"copy owned standard body produced by the real writer");
    int fd=Open(path,_O_RDWR);
    Check(_chsize_s(fd,boundary)==0 && _lseeki64(fd,0,SEEK_END)==static_cast<__int64>(boundary),"trim only copied fixture trailer");
    if (doc) Check(ext::Append(fd,*doc),"fixture bytes use production extension codec, not a mock project serializer");
    _close(fd);
}
void FinishBackground() {
    const auto deadline=GetTickCount64()+15000;
    while (SceneSaveInProgress() && GetTickCount64()<deadline) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            Check(message.message!=WM_QUIT,"asynchronous test host did not request exit");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (SceneSaveInProgress()) MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
    }
    Check(!SceneSaveInProgress(),"real background save reaches its UI completion within bounded time"); HiddenWindows();
}
void AsyncScopeChecks(MMDApp& app,const fs::path& out) {
#if !MMD_ORIGINAL_PROFILE
    const auto baseline=out/L"async-baseline.pmm",captured=out/L"async-captured.pmm",later=out/L"async-later.pmm";
    Command(app,306); if (Scope(app)) Command(app,309); Save(app,baseline);
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Check(entered && release,"create only process-local worker synchronization events");
    background_save::Options options;
    options.beforeWrite=[=] { SetEvent(entered); return WaitForSingleObject(release,10000)==WAIT_OBJECT_0; };
    app.SceneModified()=1;
    Check(background_save::Request(&app,captured.c_str(),options),"queue actual immutable save snapshot with object-local scope");
    Check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0 && SceneSaveInProgress() && !fs::exists(captured),
        "real writer is held after capture and before destination publication");
    const auto before=app.SceneRevision(); Command(app,309);
    Check(Scope(app) && app.SceneRevision()>before && app.PmmProjectSettings().sortingCrossObjectsEdited && app.SceneModified(),
        "scope can change after capture and records the newer live edit");
    Check(SetEvent(release)!=FALSE,"release owned background writer"); FinishBackground();
    Has(Read(captured),scopeKey,{0}); Has(Read(captured),tierKey,{2});
    Check(Scope(app) && app.PmmProjectSettings().sortingCrossObjectsEdited && app.SceneModified(),
        "older save completion preserves newer live scope, edited marker and dirty state");
    Check(CloseHandle(entered) && CloseHandle(release),"release both owned synchronization events");
    Check(background_save::Request(&app,later.c_str()),"queue follow-up snapshot with the new scope"); FinishBackground();
    Has(Read(later),scopeKey,{1}); Has(Read(later),tierKey,{2});
    Check(Scope(app) && !app.PmmProjectSettings().sortingCrossObjectsEdited && !app.PmmProjectSettings().sortingPrecisionEdited && !app.SceneModified(),
        "unchanged follow-up completion clears edit markers only after the new scope has been saved");
#else
    (void)app; (void)out;
#endif
}
void ProjectChecks(MMDApp& app,const fs::path& out) {
    const auto source=out/L"source.pmm",fixture=out/L"fixture.pmm",saved=out/L"saved.pmm";
    settings::Reset(app); Save(app,source); std::uint64_t boundary=0;
    Read(source,&boundary,build_profile::original);
#if !MMD_ORIGINAL_PROFILE
    for (unsigned tier=0;tier<5;++tier) for (bool scope : {false,true}) {
        Command(app,304+tier); if (Scope(app)!=scope) Command(app,309);
        Save(app,saved); const auto document=Read(saved);
        Has(document,tierKey,{static_cast<std::uint8_t>(tier)}); Has(document,scopeKey,{static_cast<std::uint8_t>(scope)});
        Command(app,309); Command(app,304+(tier+1)%5); Load(app,saved);
        Check(unsigned(app.TransparentMaterialSortPrecision())==tier && Scope(app)==scope,"all ten precision/scope combinations survive actual save and reopen independently");
    }
#endif
    for (const auto& raw : {std::vector<std::uint8_t>{0},{1},{2},{255},{0,1},{}}) {
        ext::Document document; ext::Set(document,scopeKey,raw); ext::Set(document,tierKey,{2}); ext::Set(document,unknownKey,unknownBytes);
        Fixture(source,fixture,boundary,&document); Load(app,fixture);
#if MMD_ORIGINAL_PROFILE
        Check(!Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off && app.PmmProjectSettings().document.records.empty(),
            "original reader never applies or retains enhanced scope, tier or unknown extension fields");
        Save(app,saved); Read(saved,nullptr,true);
#else
        const bool expected=raw==std::vector<std::uint8_t>{1};
        Check(Scope(app)==expected && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::High,"only exact 0/1 scope bytes apply; invalid scope defaults false without changing precision");
        Save(app,saved); auto preserved=Read(saved); Has(preserved,scopeKey,raw); Has(preserved,unknownKey,unknownBytes);
        Command(app,305); Save(app,saved); preserved=Read(saved);
        Has(preserved,scopeKey,raw); Has(preserved,tierKey,{1}); Has(preserved,unknownKey,unknownBytes);
        Command(app,309); Save(app,saved); auto changed=Read(saved);
        Has(changed,scopeKey,{static_cast<std::uint8_t>(!expected)}); Has(changed,tierKey,{1}); Has(changed,unknownKey,unknownBytes);
        Load(app,saved); Check(Scope(app)==!expected,"explicit scope override survives reopen after unknown-byte preservation");
        Command(app,309); Save(app,saved); Has(Read(saved),scopeKey,{static_cast<std::uint8_t>(expected)});
#endif
    }
    ext::Document missing; ext::Set(missing,tierKey,{3}); ext::Set(missing,unknownKey,unknownBytes);
    Fixture(source,fixture,boundary,&missing); Load(app,fixture); Check(!Scope(app),"missing scope starts false instead of inheriting previous project");
#if !MMD_ORIGINAL_PROFILE
    Save(app,saved); Has(Read(saved),scopeKey,{0}); Has(Read(saved),tierKey,{3}); Has(Read(saved),unknownKey,unknownBytes);
    for (const auto& badTier : {std::vector<std::uint8_t>{255},{2,9},{}}) {
        ext::Document doc; ext::Set(doc,scopeKey,{1}); ext::Set(doc,tierKey,badTier); ext::Set(doc,unknownKey,unknownBytes);
        Fixture(source,fixture,boundary,&doc); Load(app,fixture);
        Check(Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off,"unknown precision defaults Off while valid scope still applies");
        Command(app,309); Save(app,saved); Has(Read(saved),tierKey,badTier); Has(Read(saved),scopeKey,{0}); Has(Read(saved),unknownKey,unknownBytes);
    }
    // MDS-namespaced scope is unknown payload, never an alternative source key.
    ext::Document oldNamespace; ext::Set(oldNamespace,"org.mikudancestudio.view.transparent_sort_cross_objects",{1});
    Fixture(source,fixture,boundary,&oldNamespace); Load(app,fixture); Check(!Scope(app),"MDS scope spelling is never consumed as compatibility input");
    Save(app,saved); Has(Read(saved),scopeKey,{0}); Has(Read(saved),"org.mikudancestudio.view.transparent_sort_cross_objects",{1});
#endif
    // Legacy standard body and broken trailer both use the real project loader.
    Fixture(source,fixture,boundary,nullptr); Load(app,fixture);
    Check(!Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off,"standard PMM without trailer resets both settings");
    ext::Document enabled; ext::Set(enabled,scopeKey,{1}); ext::Set(enabled,tierKey,{2});
    Fixture(source,fixture,boundary,&enabled); Load(app,fixture);
    int fd=Open(fixture,_O_RDWR); Check(_lseeki64(fd,-8,SEEK_END)>=0,"seek owned trailer CRC");
    std::uint32_t crc=0; Check(_read(fd,&crc,4)==4,"read owned trailer CRC"); crc^=1;
    Check(_lseeki64(fd,-8,SEEK_END)>=0 && _write(fd,&crc,4)==4,"corrupt only fixture CRC"); _close(fd);
    Load(app,fixture); Check(!Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off &&
        app.PmmProjectSettings().document.records.empty(),"damaged trailer never leaks previous scope or precision into the next project");
    Command(app,307); Command(app,309); ResetAppState(&app);
    Check(!Scope(app) && app.TransparentMaterialSortPrecision()==TransparentSortPrecision::Off && app.PmmProjectSettings().document.records.empty(),
        "actual new-project reset clears scope, precision and retained payload"); HiddenWindows();
    AsyncScopeChecks(app,out);
}
#include "transparent_settings_cancel_checks.hpp"
#include "d3d12_frame_fallback_checks.hpp"
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc!=3 || (wcscmp(argv[1],L"--menu-only") && wcscmp(argv[1],L"--project-roundtrip") && wcscmp(argv[1],L"--d3d12-fallback"))) {
        std::puts("Use --menu-only, --project-roundtrip or --d3d12-fallback <absolute fresh output> in a marked copied installation."); return 2;
    }
    DefaultDesktop(); const auto out=PrepareSandbox(argv[2]); NativeD3D9Only();
    const bool fallback=!wcscmp(argv[1],L"--d3d12-fallback");
    { Guards guards; Host host(fallback || !wcscmp(argv[1],L"--project-roundtrip"));
      if (fallback) {
          std::puts("RUN real-frame D3D12 sorting fallback"); D3D12FrameFallbackChecks(*host.app);
      } else {
          std::puts("RUN native transparent settings menus"); MenuChecks(*host.app,host.project);
      }
      if (host.project && !fallback) {
          std::puts("RUN actual PMM settings roundtrip"); ProjectChecks(*host.app,out/L"projects");
          std::puts("RUN actual Save As cancellation"); SettingsCancelChecks(*host.app,out/L"projects");
      }
      HiddenWindows(); }
    Check(!GetFocus() && !GetActiveWindow() && !GetCapture(),"owned host cleanup leaves no input ownership");
    std::printf("PASS transparent settings checks=%u profile=%s mode=%ls (no renderer pixels or third-party effects)\n",
        checks,build_profile::original?"original":"full",argv[1]);
}
