#include "../src/render/nvidia_antialias_profile.hpp"
#include "../src/render/nvidia_antialias_elevation.hpp"
#include "mikumikustudio/nvidia_antialias_fix.hpp"
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <shellapi.h>
#include "../src/window/nvidia_antialias_uninstall_dialog.hpp"
namespace host_mock {
inline int calls = 0;
inline bool cancel = false;
inline DWORD wait = WAIT_OBJECT_0, exitCode = 0;
inline std::wstring parameters;
inline BOOL WINAPI Launch(SHELLEXECUTEINFOW* info) {
    ++calls; parameters = info->lpParameters;
    if (cancel) { SetLastError(ERROR_CANCELLED); return FALSE; }
    info->hProcess = reinterpret_cast<HANDLE>(321); return TRUE;
}
inline DWORD WINAPI Wait(HANDLE, DWORD) { return wait; }
inline BOOL WINAPI Exit(HANDLE, LPDWORD value) { *value = exitCode; return TRUE; }
inline BOOL WINAPI Close(HANDLE) { return TRUE; }
}
#define ShellExecuteExW host_mock::Launch
#define WaitForSingleObject host_mock::Wait
#define GetExitCodeProcess host_mock::Exit
#define CloseHandle host_mock::Close
#include "../src/render/nvidia_antialias_fix.cpp"
#undef ShellExecuteExW
#undef WaitForSingleObject
#undef GetExitCodeProcess
#undef CloseHandle
using namespace mikumikustudio::nvidia_antialias_fix;
using namespace mikumikustudio::nvidia_antialias_fix::detail;
namespace {
const std::wstring path=L"D:\\MMS only\\MikuMikuDance.exe";
struct Database {
    bool exists=false, present=false, failSave=false, mismatched=false;
    bool appFound=true, globalPresent=false, basePresent=false;
    U32 value=1, apps=1, predefined=0;
    U32 globalValue=1, baseValue=1, location=0, valuePredefined=0;
    int failGet=0;
    unsigned saves=0, sets=0, removes=0, destroys=0;
} db;
void Check(bool value,const char* name){if(!value){std::printf("FAIL %s\n",name);std::exit(1);}}
Handle target=reinterpret_cast<Handle>(123);
Handle globalTarget=reinterpret_cast<Handle>(124),baseTarget=reinterpret_cast<Handle>(125);
int __cdecl Create(Handle* s){*s=reinterpret_cast<Handle>(1);return 0;}
int __cdecl Destroy(Handle){++db.destroys;return 0;}
int __cdecl Load(Handle){return 0;}
int __cdecl Save(Handle){++db.saves;return db.failSave?-137:0;}
int __cdecl Find(Handle,const wchar_t* name,Handle* p){
    Check(name==ProfileName(path),"only own path-hashed profile requested");
    *p=target;return db.exists?0:kProfileNotFound;
}
int __cdecl CreateProfile(Handle,Profile* p,Handle* h){
    Check(p->name==ProfileName(path),"new profile has scoped name");db.exists=true;*h=target;return 0;
}
int __cdecl Info(Handle,Handle h,Profile* p){
    Check(h==target,"never global profile");wcscpy_s(p->name,ProfileName(path).c_str());
    p->apps=db.apps;p->predefined=db.predefined;return 0;
}
int __cdecl Enum(Handle,Handle,U32 first,U32* count,Application* p){
    Check(first==0&&*count==1,"check exact application association");
    wcscpy_s(p->name,db.mismatched?L"C:\\Other\\game.exe":NormalizePath(path).c_str());return 0;
}
int __cdecl CreateApp(Handle,Handle,Application* p){
    Check(p->name==path,"full executable path associated, not basename");return 0;
}
int __cdecl FindApp(Handle,const wchar_t* p,Handle* h,Application*){
    Check(p==path,"driver matching checked with full path");*h=target;return db.appFound?0:kExecutableNotFound;
}
int __cdecl Global(Handle,Handle* h){*h=globalTarget;return 0;}
int __cdecl Base(Handle,Handle* h){*h=baseTarget;return 0;}
int __cdecl Get(Handle,Handle h,U32 id,Setting* p){
    Check(id==kFix,"query only AA fix");
    if(db.failGet)return db.failGet;
    if(h==globalTarget){p->current.value=db.globalValue;p->location=1;return db.globalPresent?0:kSettingNotFound;}
    if(h==baseTarget){p->current.value=db.baseValue;p->location=2;return db.basePresent?0:kSettingNotFound;}
    Check(h==target,"scoped profile handle");
    p->current.value=db.value;p->location=db.location;p->currentPredefined=db.valuePredefined;
    return db.present?0:kSettingNotFound;
}
int __cdecl Set(Handle,Handle h,Setting* p){
    Check(h==target&&p->id==0x000858f7&&p->type==0&&p->current.value==0,"enable uses exact DWORD zero");
    ++db.sets;db.present=true;db.value=p->current.value;return 0;
}
int __cdecl Remove(Handle,Handle h,U32 id){Check(h==target&&id==kFix,"restore removes only fix");++db.removes;db.present=false;return 0;}
Api Mock(){return {Create,Destroy,Load,Save,Find,CreateProfile,Info,Enum,CreateApp,FindApp,Get,Set,Remove,Global,Base};}
}
int main(int argc,char** argv){
    if(argc==2 && std::string(argv[1])=="--inspect") {
        State state; RunForPath(CanonicalModulePath(),nullptr,state);std::printf("NVAPI read-only available=%d enabled=%d error=%ls\n",state.available,state.enabled,state.error.c_str());
        return state.available?0:77;
    }
    std::wstring error;
    const auto patch=std::filesystem::path(ModulePath()).parent_path()/kHelperName;
    Check(!std::filesystem::exists(patch),"test directory starts without a patch DLL");
    Check(!ShouldOfferInstall(),"missing patch hides installation");
    Check(!InstallForCurrentExecutable(nullptr,error,1) && error==L"MMSNvidiaAAFix.dll is not in the program folder.",
        "missing DLL reports English error without elevation");
    Check(!InstallForCurrentExecutable(nullptr,error,0) && error==L"プログラムのフォルダーに MMSNvidiaAAFix.dll がありません。",
        "missing DLL reports Japanese error without elevation");
    { std::ofstream file(patch); file << "presence-only fixture, not a DLL"; }
    Check(ShouldOfferInstall(),"DLL presence alone offers install without loading it or querying a GPU");
    bool installed = true;
    Check(!QueryInstalled(installed,error,1) && !installed && !error.empty(),"invalid DLL cannot report installed");
    Check(UninstallForCurrentExecutable(nullptr,error,1) && host_mock::parameters.find(L"/s /u \"")==0,
        "uninstall invokes Windows unregister with quoted DLL path");
    Check(InstallForCurrentExecutable(nullptr,error,1) && host_mock::parameters.find(L"/s \"")==0,
        "install invokes Windows registration with quoted DLL path");
    host_mock::cancel=true;
    Check(!UninstallForCurrentExecutable(nullptr,error,1) && error.empty(),"UAC cancellation performs no successful uninstall");
    host_mock::cancel=false;host_mock::exitCode=3;
    Check(!UninstallForCurrentExecutable(nullptr,error,1) && !error.empty(),"unregister failure is reported");
    host_mock::exitCode=0;host_mock::wait=WAIT_TIMEOUT;
    Check(!InstallForCurrentExecutable(nullptr,error,1) && !error.empty(),"pending registration never reports success");
    host_mock::wait=WAIT_OBJECT_0;
    std::filesystem::remove(patch);
    Check(!ShouldOfferInstall(),"removing the DLL hides installation immediately");
    if(argc==2) {
        std::filesystem::copy_file(argv[1],patch);
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",L"0");
        Check(QueryInstalled(installed,error) && !installed,"uninstalled DLL reports install action");
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",L"1");
        Check(QueryInstalled(installed,error) && installed,"fresh DLL load detects installation without host records");
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",L"0");
        Check(QueryInstalled(installed,error) && !installed,"uninstall status refreshes without restarting");
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",L"?");
        Check(!QueryInstalled(installed,error) && !installed,"query failure does not use stale installation state");
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",L"!");
        for(int language=0;language<3;++language)Check(!QueryInstalled(installed,error,language) &&
            !installed && error.find(L"AMD")!=std::wstring::npos && error.find(L"Intel")!=std::wstring::npos,
            "non-NVIDIA fallback explains installation is unavailable in each language");
        SetEnvironmentVariableW(L"MMS_TEST_PATCH_STATE",nullptr);
        std::filesystem::remove(patch);
    }
    Api api=Mock();State state;bool on=true,off=false;
    Check(Execute(api,path,nullptr,state)&&!db.saves&&!db.exists,"read-only inspection never creates or saves");
    Check(Execute(api,path,&on,state)&&state.enabled&&db.saves==1&&db.sets==1,"default enables scoped profile and reads back");
    Check(Execute(api,path,nullptr,state)&&state.enabled&&db.saves==1,"installed query survives a new driver session without writes");
    Check(Execute(api,path,&on,state)&&db.saves==1&&db.sets==1,"already enabled is idempotent");
    Check(Execute(api,path,&off,state)&&!state.enabled&&db.removes==1&&db.saves==2,"restore preserves profile and unrelated settings");
    Check(Execute(api,path,nullptr,state)&&!state.enabled&&db.saves==2,"post-uninstall query selects install again");
    Check(Execute(api,path,&off,state)&&db.removes==1&&db.saves==2,"repeated uninstall is idempotent");
    db.apps=2;state={};Check(!Execute(api,path,&on,state)&&db.saves==2,"shared profile is never changed");
    db.apps=1;db.predefined=1;Check(!Execute(api,path,&on,state)&&db.saves==2,"predefined profile is never changed");
    db.predefined=0;db.mismatched=true;Check(!Execute(api,path,&on,state)&&db.saves==2,"wrong application is never changed");
    db.mismatched=false;db.failSave=true;Check(!Execute(api,path,&on,state)&&!state.error.empty(),"driver save failure propagates");
    db={};Check(!Execute(api,L"game.exe",&on,state)&&!db.saves&&!db.exists,"relative paths rejected before driver access");
    db={};db.appFound=false;db.basePresent=true;db.baseValue=0;
    Check(Execute(api,path,nullptr,state)&&!state.enabled&&!db.saves,"global fix is not mistaken for DLL-owned installation");
    Check(InspectEffective(api,path,state)&&state.enabled&&!db.saves&&!db.sets&&!db.exists,"original global patch detected without creating an app profile");
    db.globalPresent=true;db.globalValue=1;
    Check(InspectEffective(api,path,state)&&!state.enabled,"global override off wins over base on");
    db.appFound=true;db.present=true;db.value=0;db.location=2;db.valuePredefined=1;
    Check(InspectEffective(api,path,state)&&state.enabled,"inherited predefined enabled value is accepted");
    db.value=1;db.baseValue=0;db.globalValue=0;
    Check(InspectEffective(api,path,state)&&!state.enabled,"application off wins over installed global patch");
    db.failGet=-175;
    Check(!InspectEffective(api,path,state)&&state.driverStatus==-175&&!state.enabled,"read failure cannot become a success record");
    db={};db.appFound=false;
    Check(InspectEffective(api,path,state)&&!state.enabled&&!db.sets&&!db.saves,"missing patch remains visible without driver writes");
    Check(IsSiblingApplication(L"D:\\MMS only\\MMSNvidiaAAFix.dll",path),"helper accepts only its sibling host");
    Check(!IsSiblingApplication(L"C:\\Other\\MMSNvidiaAAFix.dll",path),"helper rejects a different directory");
    Check(!IsSiblingApplication(L"D:\\MMS only\\MMSNvidiaAAFix.dll",L"D:\\MMS only\\other.exe"),"helper rejects another executable name");
    Check(!IsSiblingApplication(L"D:\\MMS only\\MMSNvidiaAAFix.dll",L"D:\\MMS only\\..\\MikuMikuDance.exe"),"helper rejects uncanonical parent traversal");
    std::wstring canonical;
    const bool canonicalValid=CanonicalFile(ModulePath(),canonical);
    if(!canonicalValid)std::printf("canonical diagnostics: module=%ls result=%ls windows=%lu\n",ModulePath().c_str(),canonical.c_str(),GetLastError());
    Check(canonicalValid&&canonical==CanonicalModulePath(),"startup inspection and helper registration share the canonical executable identity");
    namespace dialog = mikumikustudio::nvidia_antialias_menu::uninstall_dialog;
    const auto previousDesktop=GetThreadDesktop(GetCurrentThreadId());
    const auto desktopName=L"MMSNvidiaDialogTest-"+std::to_wstring(GetCurrentProcessId());
    const auto desktop=CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    Check(desktop && SetThreadDesktop(desktop),"dialog audit uses a private desktop");
    for(int language=0;language<3;++language) {
        auto data=dialog::Template();
        HWND window=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(data.data()),
            nullptr,dialog::Proc,language);
        Check(window && LOWORD(SendMessageW(window,DM_GETDEFID,0,0))==IDCANCEL,"cancel is default in each language");
        HDC dc=GetDC(window);
        dialog::Proc(window,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(GetDlgItem(window,dialog::Warning)));
        Check(GetTextColor(dc)==RGB(192,0,0),"uninstall consequence is painted red");
        for(int id : {dialog::Warning,dialog::Detail}) {
            HWND child=GetDlgItem(window,id);RECT bounds{};GetClientRect(child,&bounds);
            wchar_t text[1024]{};GetWindowTextW(child,text,1024);
            auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
            RECT measure=bounds;DrawTextW(dc,text,-1,&measure,DT_CALCRECT|DT_WORDBREAK|DT_LEFT);
            Check(measure.bottom<=bounds.bottom,"localized warning and explanation fit their controls");
            SelectObject(dc,previous);
        }
        ReleaseDC(window,dc);DestroyWindow(window);
    }
    Check(SetThreadDesktop(previousDesktop)!=FALSE,"restore test desktop");CloseDesktop(desktop);
    std::puts("PASS scoped NVIDIA install/uninstall, DLL status, host invocation, cancellation, and trilingual red warning");return 0;
}
