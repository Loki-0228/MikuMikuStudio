#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <MinHook.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "../src/window/cache_manager_dialog.hpp"
#include "../src/window/settings_json.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
namespace fs=std::filesystem;
namespace cache=mikumikustudio::cache_manager;
namespace ui=mikumikustudio::cache_manager_ui;
unsigned checks=0,failures=0,language=0,stage=0,ticks=0;
int interfacePercent=100;
int constrainedPercent=0;
RECT constrainedWork{};
HMONITOR constrainedMonitor=nullptr;
DWORD constrainedThread=0;
unsigned workAreaReads=0;
decltype(&GetMonitorInfoW) realMonitorInfo=nullptr;
RECT templateClient{},templateFrame{};
LONG templateFontHeight=0;
fs::path output;
HWND dialog=nullptr,fixtureOwner=nullptr;
HDESK privateDesktop=nullptr; // Intentionally retained until process exit, including library worker threads.
std::wstring desktopName;
std::ofstream evidence;
unsigned confirmations=0;
bool fixtureVerified=false;
ULONGLONG deadline=0;
// Selected-project cleanup while another process uses the exclusive payload.
unsigned projectPhase=0;
std::wstring projectPath;
fs::path blockedPayload;
std::vector<unsigned char> blockedBytes,sharedBytes,otherBytes;
HANDLE leaseProcess=nullptr,leaseRelease=nullptr;
// Busy close: gate only the first exact fixture file disposition on a worker. The
// selected outcome is immutable while installed; subsequent calls (the explicit
// retry included), UI-thread calls and all other paths use the real API.
enum class BusyDeleteOutcome { Delete, AccessDenied };
BusyDeleteOutcome busyDeleteOutcome=BusyDeleteOutcome::Delete;
decltype(&SetFileInformationByHandle) realDelete=nullptr;
HANDLE busyEntered=nullptr,busyRelease=nullptr,busyReturned=nullptr;
std::atomic<bool> busyHookSeen{false},busyGateTimedOut{false};
std::atomic<long> busyHookInFlight{0};
std::atomic<unsigned> busyDeleteCalls{0},busyDeleteDenials{0},busyDeleteSuccesses{0};
std::atomic<DWORD> busyWorkerThread{0},busyDeleteError{ERROR_SUCCESS};
DWORD busyUiThread=0;
std::wstring busyTarget;
bool busyHookInitialized=false;
unsigned busyStage=0,busyTicksAfterClose=0;
enum class BusyFailureStage {
    Opening, Selecting, Entering, PendingClose, PendingAgain, Failure,
    RetainFailure, Retry, RetainRetry, Closing, Reopening, ReopenApply,
    RetainReopen, Reclosed
};
BusyFailureStage busyFailureStage=BusyFailureStage::Opening;
HWND busyFailureWindow=nullptr;
unsigned busyFailureStableTicks=0;
std::vector<unsigned char> busyFailureBytes;
using SetDpiContext=HANDLE(WINAPI*)(HANDLE);
using GetDpiContext=HANDLE(WINAPI*)();
using GetWindowDpiContext=HANDLE(WINAPI*)(HWND);
using GetAwareness=int(WINAPI*)(HANDLE);
using GetSystemDpi=UINT(WINAPI*)();
using GetWindowDpi=UINT(WINAPI*)(HWND);
SetDpiContext setDpiContext=nullptr;
GetDpiContext getDpiContext=nullptr;
GetWindowDpiContext getWindowDpiContext=nullptr;
GetAwareness getAwareness=nullptr;
GetSystemDpi getSystemDpi=nullptr;
GetWindowDpi getWindowDpi=nullptr;
void Check(bool ok,const char* message){
    ++checks;
    if(!ok){++failures;std::printf("FAIL language=%u stage=%u %s\n",language,stage,message);
        if(evidence.is_open()){evidence<<"FAIL\t"<<language<<'\t'<<stage<<'\t'<<message<<'\n';evidence.flush();}}
}
void Need(bool ok,const char* message){
    if(ok){Check(true,message);return;}
    const auto error=GetLastError();Check(false,message);
    std::printf("FATAL win32=%lu; supervisor must wait for process termination before releasing desktop\n",error);
    if(evidence.is_open()){evidence<<"FATAL\twin32="<<error<<'\n';evidence.flush();}
    // Also stops a nested modal loop; do not answer an unexpected dialog to unwind it.
    ExitProcess(2);
}
BOOL WINAPI ConstrainedMonitorInfo(HMONITOR monitor, LPMONITORINFO info) {
    const BOOL found = realMonitorInfo(monitor, info);
    if (found && GetCurrentThreadId() == constrainedThread && monitor == constrainedMonitor) {
        info->rcWork = constrainedWork;
        ++workAreaReads;
    }
    return found;
}
std::string Utf8(const std::wstring& text){
    if(text.empty())return {};
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    Need(size>0,"encode evidence text");std::string result(size,'\0');
    Need(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),size,nullptr,nullptr)==size,"encode complete evidence text");
    std::string escaped;
    for(char c:result){if(c=='\n')escaped+="\\n";else if(c=='\r')escaped+="\\r";else if(c=='\t')escaped+="\\t";else if(c=='\\')escaped+="\\\\";else escaped+=c;}
    return escaped;
}
std::wstring Text(HWND window){
    const int length=GetWindowTextLengthW(window);std::vector<wchar_t> text(std::size_t(length)+1);
    const int copied=GetWindowTextW(window,text.data(),int(text.size()));
    Need(copied==length,"read complete window text");return {text.data(),std::size_t(copied)};
}
std::wstring Class(HWND window){wchar_t name[256]{};Need(GetClassNameW(window,name,256)>0,"read window class");return name;}
std::wstring Environment(const wchar_t* name){
    const auto count=GetEnvironmentVariableW(name,nullptr,0);Need(count>1&&count<=32768,"explicit isolation environment variable");
    std::vector<wchar_t> value(count);Need(GetEnvironmentVariableW(name,value.data(),count)==count-1,"read isolation environment variable");return value.data();
}
void NoReparseParents(fs::path path){
    for(;!path.empty();){
        const auto attrs=GetFileAttributesW(path.c_str());
        Need(attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&FILE_ATTRIBUTE_REPARSE_POINT),"fixture parents exist and contain no reparse points");
        const auto parent=path.parent_path();if(parent==path)break;path=parent;
    }
}
void EnvironmentIs(const wchar_t* name,const fs::path& expected){
    const fs::path actual(Environment(name));
    Need(actual.is_absolute()&&actual.lexically_normal()==expected,"environment resolves to the exact isolated fixture path");
}
fs::path CaseRoot(){return output/(L"lang-"+std::to_wstring(language));}
void RequireStorageAt(const fs::path& root){
    EnvironmentIs(L"LOCALAPPDATA",output/L"Local");EnvironmentIs(L"APPDATA",output/L"Roaming");
    EnvironmentIs(L"TEMP",output/L"Temp");EnvironmentIs(L"TMP",output/L"Temp");
    EnvironmentIs(L"MMS_CACHE_MANAGER_DIR",root/L"Cache");
    EnvironmentIs(L"MMS_CACHE_MANAGER_SETTINGS",root/L"settings.json");
    for(const auto* leaf:{L"Local",L"Roaming",L"Temp"})NoReparseParents(output/leaf);
    Need(fs::current_path()==output,"working directory remains inside isolated fixture");
    NoReparseParents(root/L"Cache");
    Need(cache::Root()==root/L"Cache","production cache root is the isolated fixture");
    const auto settings=root/L"settings.json";
    if(fs::exists(settings))NoReparseParents(settings);
}
void RequireStorage(){RequireStorageAt(CaseRoot());}
void RequireDesktop(){
    wchar_t actual[256]{};DWORD bytes=0;BOOL input=TRUE;
    const auto desktop=GetThreadDesktop(GetCurrentThreadId());
    Need(desktop&&GetUserObjectInformationW(desktop,UOI_NAME,actual,sizeof actual,&bytes),"query private desktop name");
    Need(desktopName==actual&&_wcsicmp(actual,L"Default")!=0,"exact supervisor-held non-default desktop");
    Need(GetUserObjectInformationW(desktop,UOI_IO,&input,sizeof input,&bytes)&&!input,"private desktop is not an input desktop");
}
// Supervisor protocol (no token/ACL edits, no SwitchDesktop, no hidden/default fallback):
//   exe --desktop-bootstrap UNIQUE_NAME NEW_ABSOLUTE_OUTPUT
//   exe --private-desktop UNIQUE_NAME NEW_ABSOLUTE_OUTPUT
// The second mode requires STARTUPINFO.lpDesktop=UNIQUE_NAME. In either mode the
// child publishes OUTPUT/desktop-ready.txt, containing only the ASCII name.
// The runner must OpenDesktopW that name, verify UOI_IO=false, keep its HDESK,
// then atomically publish OUTPUT/desktop-held.txt with the exact same content.
// Keep HDESK until the child process is signalled (all threads have exited),
// including failure/timeout paths. Never release it merely when a file appears
// or wmain finishes. Enforce an external timeout and capture stdout/stderr.
// Before launch, set LOCALAPPDATA=OUTPUT/Local, APPDATA=OUTPUT/Roaming,
// TEMP=TMP=OUTPUT/Temp, MMS_CACHE_MANAGER_DIR=OUTPUT/lang-0/Cache and
// MMS_CACHE_MANAGER_SETTINGS=OUTPUT/lang-0/settings.json. OUTPUT must not exist.
void Prepare(int argc,wchar_t** argv){
    Need(argc>=4&&argc<=6,"usage: cache_manager_ui_tests (--desktop-bootstrap|--private-desktop) UNIQUE_NAME NEW_ABSOLUTE_OUTPUT [125|200 [work-135|work-100|work-85]]");
    if(argc>=5){
        const std::wstring scale=argv[4];
        Need(scale==L"125"||scale==L"200","choose one application scale test level");
        interfacePercent=scale==L"125"?125:200;
    }
    if(argc==6){
        const std::wstring constraint=argv[5];
        Need(interfacePercent==200&&(constraint==L"work-135"||constraint==L"work-100"||constraint==L"work-85"),
            "work area cases request 200 percent with a known 135, 100 or 85 percent capacity");
        constrainedPercent=constraint==L"work-135"?135:constraint==L"work-100"?100:85;
    }
    const std::wstring mode=argv[1];Need(mode==L"--desktop-bootstrap"||mode==L"--private-desktop","explicit private desktop mode");
    desktopName=argv[2];Need(!desktopName.empty()&&desktopName.size()<128&&_wcsicmp(desktopName.c_str(),L"Default")!=0,"unique private desktop name");
    for(wchar_t c:desktopName)Need((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_',"ASCII desktop name");
    const fs::path requested(argv[3]);Need(requested.is_absolute(),"absolute output path required");output=requested.lexically_normal();
    Need(output!=output.root_path()&&output.has_filename()&&!fs::exists(output),"new output directory required");
    NoReparseParents(output.parent_path());
    // Validate the launch environment before creating a cache manager, app or window.
    EnvironmentIs(L"LOCALAPPDATA",output/L"Local");EnvironmentIs(L"APPDATA",output/L"Roaming");
    EnvironmentIs(L"TEMP",output/L"Temp");EnvironmentIs(L"TMP",output/L"Temp");
    EnvironmentIs(L"MMS_CACHE_MANAGER_DIR",CaseRoot()/L"Cache");EnvironmentIs(L"MMS_CACHE_MANAGER_SETTINGS",CaseRoot()/L"settings.json");
    Need(fs::create_directory(output),"exclusively create output directory");
    for(const auto* leaf:{L"Local",L"Roaming",L"Temp"})Need(fs::create_directory(output/leaf),"create isolated profile directory");
    fs::current_path(output);
    evidence.open(output/L"evidence.tsv",std::ios::binary);Need(bool(evidence),"open UTF-8 evidence log");
    evidence<<"run\tpid="<<GetCurrentProcessId()<<"\tmode="<<Utf8(mode)<<"\tdesktop="<<Utf8(desktopName)<<"\toutput="<<Utf8(output.wstring())<<'\n';
    if(mode==L"--desktop-bootstrap"){
        unsigned windows=0;EnumThreadWindows(GetCurrentThreadId(),[](HWND,LPARAM value)->BOOL{++*reinterpret_cast<unsigned*>(value);return TRUE;},reinterpret_cast<LPARAM>(&windows));
        Need(windows==0,"no thread windows before private desktop bootstrap");
        bool exists=false;
        Need(EnumDesktopsW(GetProcessWindowStation(),[](LPWSTR name,LPARAM value)->BOOL{
            if(_wcsicmp(name,desktopName.c_str())==0)*reinterpret_cast<bool*>(value)=true;return TRUE;
        },reinterpret_cast<LPARAM>(&exists))!=FALSE&&!exists,"bootstrap desktop name is not already in use");
        SetLastError(ERROR_SUCCESS);
        privateDesktop=CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        const auto createdError=GetLastError();
        Need(privateDesktop&&createdError!=ERROR_ALREADY_EXISTS,"create fresh private desktop at unchanged process token and default ACL");
        Need(SetThreadDesktop(privateDesktop)!=FALSE,"attach private desktop before creating any GUI");
    }
    RequireDesktop();
    const auto ready=output/L"desktop-ready.txt",held=output/L"desktop-held.txt";
    Need(!fs::exists(ready)&&!fs::exists(held),"fresh desktop handshake files");
    const std::string expected(desktopName.begin(),desktopName.end());
    {std::ofstream stream(output/L"desktop-ready.tmp",std::ios::binary);stream<<expected;stream.close();Need(bool(stream),"write desktop ready record");}
    Need(MoveFileExW((output/L"desktop-ready.tmp").c_str(),ready.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"publish desktop ready record");
    const auto until=GetTickCount64()+30000;
    while(!fs::exists(held)&&GetTickCount64()<until)Sleep(10);
    Need(fs::is_regular_file(held),"supervisor must acknowledge holding desktop within 30 seconds");NoReparseParents(held);
    Need(fs::file_size(held)==expected.size(),"desktop receipt is the exact name without BOM or newline");
    std::ifstream receipt(held,std::ios::binary);std::string actual((std::istreambuf_iterator<char>(receipt)),std::istreambuf_iterator<char>());
    Need(actual==expected,"supervisor acknowledgement exactly matches desktop name");RequireDesktop();
    evidence<<"desktop\theld=true\tinput=false\n";evidence.flush();
}
void ConfigureDpi(){
    const auto user=GetModuleHandleW(L"user32.dll");
    setDpiContext=reinterpret_cast<SetDpiContext>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
    getDpiContext=reinterpret_cast<GetDpiContext>(GetProcAddress(user,"GetThreadDpiAwarenessContext"));
    getWindowDpiContext=reinterpret_cast<GetWindowDpiContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    getAwareness=reinterpret_cast<GetAwareness>(GetProcAddress(user,"GetAwarenessFromDpiAwarenessContext"));
    getSystemDpi=reinterpret_cast<GetSystemDpi>(GetProcAddress(user,"GetDpiForSystem"));
    getWindowDpi=reinterpret_cast<GetWindowDpi>(GetProcAddress(user,"GetDpiForWindow"));
    Need(setDpiContext&&getDpiContext&&getWindowDpiContext&&getAwareness&&getSystemDpi&&getWindowDpi,"Windows 10 1607+ DPI observation APIs required");
    // Match WinMain, including compatibility-override handling. Do not scale resources.
    Need(setDpiContext(reinterpret_cast<HANDLE>(INT_PTR(-1)))!=nullptr,"set main-equivalent DPI-unaware UI context");
    Need(getAwareness(getDpiContext())==0,"main-equivalent UI DPI context verified");
}
UINT SystemDpi(){
    // GetDpiForSystem virtualizes to 96 in an unaware context. Temporarily query
    // from system-aware context, without creating windows or changing UI layout.
    const auto previous=setDpiContext(reinterpret_cast<HANDLE>(INT_PTR(-2)));
    Need(previous!=nullptr,"enter system-aware measurement context");const UINT dpi=getSystemDpi();
    Need(setDpiContext(previous)!=nullptr,"restore production UI context after DPI measurement");
    Need(dpi!=0&&getAwareness(getDpiContext())==0,"actual system DPI measured and UI context restored");return dpi;
}
std::wstring Hash(unsigned id){wchar_t hash[65]{};swprintf_s(hash,L"%064x",id);return hash;}
fs::path FileAt(const fs::path& root,unsigned id){return root/L"Textures"/L"texture"/(Hash(id)+L".mtc");}
fs::path File(unsigned id){return FileAt(cache::Root(),id);}
void WritePayload(const fs::path& root,unsigned id,unsigned size) {
    const auto path=FileAt(root,id);cache::WriteReservation write(path,size);Need(bool(write),"fixture reservation");fs::create_directories(path.parent_path());
    const fs::path temp=path.wstring()+L".tmp-ui-fixture";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Need(file!=INVALID_HANDLE_VALUE,"create UI fixture temporary");
    Need(write.TrackTemporary(temp),"track empty UI fixture temporary");CloseHandle(file);
    std::uint32_t header[19]{};header[0]=0x4354504d;header[1]=2;header[2]=size-76;
    std::ofstream stream(temp,std::ios::binary);stream.write(reinterpret_cast<char*>(header),sizeof(header));stream.seekp(size-1);stream.put(0);stream.close();
    Need(bool(stream),"write complete UI fixture payload");
    Need(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"publish UI fixture");
    Need(write.Commit(),"persist UI fixture registration");
}
void Store(unsigned id,unsigned size){RequireStorage();WritePayload(cache::Root(),id,size);}
std::vector<unsigned char> ReadBytes(const fs::path& path){
    std::ifstream stream(path,std::ios::binary);Need(bool(stream),"read isolated fixture payload bytes");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
DWORD DeleteAccess(const fs::path& path) {
    const HANDLE probe=CreateFileW(path.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(probe==INVALID_HANDLE_VALUE)return GetLastError();
    CloseHandle(probe);
    return ERROR_SUCCESS;
}

std::wstring LeaseEvent(const wchar_t* purpose) {
    return L"Local\\"+desktopName+L"-"+std::to_wstring(language)+L"-"+purpose;
}

int RunLeaseChild(wchar_t** argv) {
    desktopName=argv[2];
    output=fs::path(argv[3]).lexically_normal();
    const std::wstring locale=argv[4];
    Need(locale.size()==1&&locale[0]>=L'0'&&locale[0]<=L'2',"valid isolated case number");
    language=locale[0]-L'0';
    const std::wstring payloadId=argv[5];
    Need(payloadId==L"1"||payloadId==L"3","reader can only use the exact shared or exclusive fixture payload");
    Need(output.is_absolute()&&output==fs::current_path(),"lease child stays inside the supervisor fixture");
    RequireDesktop();RequireStorage();
    evidence.open(CaseRoot()/(L"lease-child-"+payloadId+L".tsv"),std::ios::binary);
    Need(bool(evidence),"open independent lease evidence");
    const HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,LeaseEvent(L"ready").c_str());
    const HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,LeaseEvent(L"release").c_str());
    Need(ready&&release,"open the parent fixture's lease events");
    const auto payload=File(payloadId[0]-L'0');
    const auto project=output/L"另一个工程"/L"Stage.pmm";
    const auto original=ReadBytes(payload);
    Need(DeleteAccess(payload)==ERROR_SUCCESS,"idle payload is deletable before production lease");
    const auto load=cache::AcceptScene(cache::SceneOperation::Load);
    Need(bool(load),"accept the cache consumer's scene");
    cache::Touch(payload);
    Need(cache::FinishScene(load,cache::SceneOutcome::Loaded,project.c_str()),"finish the cache consumer's scene load");
    const auto view=cache::Inspect();
    Need(view.error.empty()&&std::any_of(view.projects.begin(),view.projects.end(),[&](const auto& row){
        return row.path==project.wstring()&&row.current;
    }),"reader process owns the active project");
    Need(DeleteAccess(payload)==ERROR_SHARING_VIOLATION,"production cache lease prevents deletion");
    evidence<<"held\tpid="<<GetCurrentProcessId()<<"\tbytes="<<original.size()<<'\n';
    evidence.flush();
    Need(SetEvent(ready)!=FALSE,"signal that the production lease is held");
    Need(WaitForSingleObject(release,30000)==WAIT_OBJECT_0,"parent finishes its first clear request");
    Need(ReadBytes(payload)==original,"reader can still read the original bytes after cleanup");
    const auto reset=cache::AcceptScene(cache::SceneOperation::Reset);
    Need(cache::FinishScene(reset,cache::SceneOutcome::ResetComplete),"release the lease through scene reset");
    Need(DeleteAccess(payload)==ERROR_SUCCESS,"reset makes the file deletable while reader process is still alive");
    evidence<<"released\tpid="<<GetCurrentProcessId()<<"\tbytes="<<original.size()<<'\n';
    evidence.flush();
    CloseHandle(ready);CloseHandle(release);
    return 0;
}

void StartLeaseChild(unsigned payloadId) {
    SetLastError(ERROR_SUCCESS);
    const HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,LeaseEvent(L"ready").c_str());
    Need(ready&&GetLastError()!=ERROR_ALREADY_EXISTS,"create an exclusive readiness event");
    SetLastError(ERROR_SUCCESS);
    leaseRelease=CreateEventW(nullptr,TRUE,FALSE,LeaseEvent(L"release").c_str());
    Need(leaseRelease&&GetLastError()!=ERROR_ALREADY_EXISTS,"create an exclusive release event");
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    Need(length&&length<executable.size(),"find the current fixture executable");
    executable.resize(length);
    auto command=L"\""+executable+L"\" --scene-lease-child "+desktopName+L" \""+
        output.wstring()+L"\" "+std::to_wstring(language)+L" "+std::to_wstring(payloadId);
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.lpDesktop=desktopName.data();
    PROCESS_INFORMATION child{};
    Need(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,output.c_str(),&startup,&child)!=FALSE,"start the independent reader on the held private desktop");
    CloseHandle(child.hThread);leaseProcess=child.hProcess;
    const HANDLE waits[]={ready,leaseProcess};
    const auto waited=WaitForMultipleObjects(2,waits,FALSE,10000);
    CloseHandle(ready);
    Need(waited==WAIT_OBJECT_0&&WaitForSingleObject(leaseProcess,0)==WAIT_TIMEOUT&&
        child.dwProcessId!=GetCurrentProcessId(),"another live process confirms holding its scene lease");
    evidence<<"reader-lease\tphase=held\tlanguage="<<language<<"\tpayload="<<payloadId<<"\treader-pid="<<child.dwProcessId
        <<"\tcleaner-pid="<<GetCurrentProcessId()<<'\n';
}

void ReleaseLeaseChild() {
    Need(SetEvent(leaseRelease)!=FALSE,"let the reader verify bytes and reset its scene");
    DWORD code=0;
    Need(WaitForSingleObject(leaseProcess,10000)==WAIT_OBJECT_0&&GetExitCodeProcess(leaseProcess,&code)&&code==0,
        "reader verifies byte identity and deletion access after reset");
    CloseHandle(leaseRelease);leaseRelease=nullptr;
    CloseHandle(leaseProcess);leaseProcess=nullptr;
}

void Fixture() {
    fixtureVerified=false;const auto caseRoot=CaseRoot();
    Need(fs::create_directory(caseRoot)&&fs::create_directory(caseRoot/L"Cache"),"new per-language fixture and cache directory");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(caseRoot/L"Cache").c_str())!=FALSE,"set isolated cache override");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(caseRoot/L"settings.json").c_str())!=FALSE,"set isolated settings override");
    RequireStorage();Need(!fs::exists(caseRoot/L"settings.json"),"fresh per-language settings");
    Store(1,8192);Store(2,12288);Store(3,10240);cache::ResetScene();
    const auto a=output/L"舞台演出"/L"Stage.pmm",b=output/L"另一个工程"/L"Stage.pmm";
    cache::BeginProject(a.c_str());cache::Touch(File(1));cache::Touch(File(2));cache::ProjectOpened(a.c_str());cache::ResetScene();Sleep(2);
    cache::BeginProject(b.c_str());cache::Touch(File(1));cache::Touch(File(3));cache::ProjectOpened(b.c_str());cache::ResetScene();
    const auto snapshot=cache::Inspect();
    Need(snapshot.error.empty()&&snapshot.projects.size()==2&&fs::exists(File(1))&&fs::exists(File(2))&&fs::exists(File(3)),"isolated fixture verified before real window opens");
    evidence<<"fixture\tlanguage="<<language<<"\tcache="<<Utf8(cache::Root().wstring())<<"\tsettings="<<Utf8((caseRoot/L"settings.json").wstring())<<'\n';
    fixtureVerified=true;
}
fs::path BusyRoot(){return output/(L"lang-"+std::to_wstring(language)+L"-busy-close");}
fs::path BusyPayload(){return FileAt(BusyRoot()/L"Cache",0x400u+language);}
void BusyFixture(){
    fixtureVerified=false;const auto root=BusyRoot();
    Need(fs::create_directory(root)&&fs::create_directory(root/L"Cache"),"new per-language busy-close fixture and cache directory");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(root/L"Cache").c_str())!=FALSE,"set isolated busy-close cache override");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root/L"settings.json").c_str())!=FALSE,"set isolated busy-close settings override");
    RequireStorageAt(root);
    WritePayload(root/L"Cache",0x400u+language,16384);
    cache::ResetScene();
    const auto project=root/L"busy-close-project"/L"Stage.pmm";
    cache::BeginProject(project.c_str());cache::Touch(BusyPayload());cache::ProjectOpened(project.c_str());cache::ResetScene();
    const auto snapshot=cache::Inspect();
    Need(snapshot.error.empty()&&snapshot.projects.size()==1&&fs::is_regular_file(BusyPayload()),"busy-close isolated fixture verified before real window opens");
    evidence<<"busy-close-fixture\tlanguage="<<language<<"\tcache="<<Utf8((root/L"Cache").wstring())<<"\tpayload="<<Utf8(BusyPayload().wstring())<<'\n';
    fixtureVerified=true;
}
fs::path BusyFailureRoot(){return output/(L"lang-"+std::to_wstring(language)+L"-busy-close-failure");}
fs::path BusyFailurePayload(){return FileAt(BusyFailureRoot()/L"Cache",0x500u+language);}
fs::path BusyFailureProject(){return BusyFailureRoot()/L"busy-close-failure-project"/L"Stage.pmm";}
void BusyFailureFixture(){
    fixtureVerified=false;const auto root=BusyFailureRoot();
    Need(fs::create_directory(root)&&fs::create_directory(root/L"Cache"),"new independent per-language busy-close failure fixture");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(root/L"Cache").c_str())!=FALSE,"set isolated busy-close failure cache override");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(root/L"settings.json").c_str())!=FALSE,"set isolated busy-close failure settings override");
    RequireStorageAt(root);
    WritePayload(root/L"Cache",0x500u+language,16384);cache::ResetScene();
    const auto project=BusyFailureProject();
    cache::BeginProject(project.c_str());cache::Touch(BusyFailurePayload());cache::ProjectOpened(project.c_str());cache::ResetScene();
    const auto snapshot=cache::Inspect();
    Need(snapshot.error.empty()&&snapshot.projects.size()==1&&snapshot.projects[0].path==project.wstring()&&
        !snapshot.projects[0].current&&snapshot.projects[0].sharedBytes==0&&snapshot.projects[0].reclaimableBytes>0&&
        fs::is_regular_file(BusyFailurePayload()),"failure fixture has exactly one idle exclusive deletable payload");
    busyFailureBytes=ReadBytes(BusyFailurePayload());
    Need(!busyFailureBytes.empty(),"retain original failure fixture bytes for comparison");
    evidence<<"busy-close-failure-fixture\tlanguage="<<language<<"\tcache="<<Utf8((root/L"Cache").wstring())
        <<"\tpayload="<<Utf8(BusyFailurePayload().wstring())<<'\n';
    fixtureVerified=true;
}
bool OwnedByTest(HWND window){DWORD pid=0;const auto tid=GetWindowThreadProcessId(window,&pid);return pid==GetCurrentProcessId()&&tid==GetCurrentThreadId();}
const wchar_t* ExpectedTitle(){return language==0?L"キャッシュ管理":language==1?L"Cache management":L"缓存管理";}
const wchar_t* ExpectedPrompt(){
    return language==0?L"削除可能なキャッシュを消去しますか？必要時に再生成し、使用中のファイルは残します。":
        language==1?L"Clear deletable disk cache? It will be rebuilt when needed. In-use files will be skipped.":
        L"清空可删除的磁盘缓存？下次打开工程时会重新生成。正在使用的缓存会跳过。";
}
const wchar_t* ExpectedSkipStatus(){
    return language==0?L"0 ファイル（0 B）を削除、使用中など 1 ファイルをスキップ。":
        language==1?L"Removed 0 files (0 B); skipped 1 in-use or unavailable files.":
        L"已清理 0 个文件，释放 0 B；跳过 1 个使用中或不可删除的文件。";
}
const wchar_t* ExpectedWorkingStatus(){
    return language==0?L"処理中…":language==1?L"Working…":L"正在处理…";
}
const wchar_t* ExpectedClosingStatus(){
    return language==0?L"処理の完了後に閉じます…":language==1?L"Closing when the operation finishes…":L"操作完成后关闭…";
}
const wchar_t* ExpectedSavedStatus(){
    return language==0?L"設定を保存しました。":language==1?L"Cache settings saved.":L"缓存设置已保存。";
}
bool RetriedStatus(const std::wstring& text){
    const std::wstring prefix=language==0?L"1 ファイル（":language==1?L"Removed 1 files (":L"已清理 1 个文件，释放 ";
    const std::wstring suffix=language==0?L"）を削除、使用中など 0 ファイルをスキップ。":language==1?L"); skipped 0 in-use or unavailable files.":L"；跳过 0 个使用中或不可删除的文件。";
    if(text.size()<=prefix.size()+suffix.size())return false;
    return text.compare(0,prefix.size(),prefix)==0&&text.compare(text.size()-suffix.size(),suffix.size(),suffix)==0&&
        text.substr(prefix.size(),text.size()-prefix.size()-suffix.size())!=L"0 B";
}
void RecordWindow(HWND window,const char* tag){
    DWORD pid=0;const auto tid=GetWindowThreadProcessId(window,&pid);
    evidence<<tag<<"\tlanguage="<<language<<"\tstage="<<stage<<"\thwnd="<<window<<"\towner="<<GetWindow(window,GW_OWNER)
        <<"\troot-owner="<<GetAncestor(window,GA_ROOTOWNER)<<"\tpid="<<pid<<"\ttid="<<tid
        <<"\tclass="<<Utf8(Class(window))<<"\ttext="<<Utf8(Text(window))<<'\n';
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        evidence<<"dialog-child\tid="<<GetDlgCtrlID(child)<<"\tclass="<<Utf8(Class(child))<<"\ttext="<<Utf8(Text(child))<<'\n';
    evidence.flush();
}
BOOL CALLBACK Find(HWND window,LPARAM){
    if(OwnedByTest(window)&&GetWindow(window,GW_OWNER)==fixtureOwner&&Class(window)==L"#32770"&&
       GetDlgItem(window,ui::Enabled)&&GetDlgItem(window,ui::Projects)){
        Need(!dialog||dialog==window,"only one fixture cache management dialog");dialog=window;
        Need(Text(window)==ExpectedTitle(),"real cache dialog has the expected localized title");
    }
    return TRUE;
}
void CheckConfirmationWorkArea(HWND window, HWND yes, HWND no);
HWND VerifyConfirmation(HWND window){
    RecordWindow(window,"observed-modal");
    bool prompt=false;unsigned textCount=0,buttons=0;
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        const auto type=Class(child),text=Text(child);
        if(type==L"Static"&&!text.empty()){++textCount;prompt=text==ExpectedPrompt();}
        if(type==L"Button")++buttons;
    }
    HWND yes=GetDlgItem(window,IDYES),no=GetDlgItem(window,IDNO);
    const bool expected=fixtureVerified&&OwnedByTest(window)&&
        GetWindow(window,GW_OWNER)==dialog&&GetAncestor(window,GA_ROOTOWNER)==fixtureOwner&&
        Text(window)==ExpectedTitle()&&prompt&&textCount==1&&buttons==2&&
        yes&&no&&GetParent(yes)==window&&GetParent(no)==window&&Class(yes)==L"Button"&&Class(no)==L"Button";
    Need(expected,"unexpected dialog: never auto-answer; see observed-modal evidence");
    if (constrainedThread) CheckConfirmationWorkArea(window, yes, no);
    return yes;
}
BOOL CALLBACK Confirm(HWND window,LPARAM){
    if(window==dialog||Class(window)!=L"#32770")return TRUE;
    const HWND yes=VerifyConfirmation(window);
    Need(stage==10&&confirmations<2,"unexpected clear-cache confirmation stage or count");
    RequireDesktop();RequireStorage();Need(IsWindowEnabled(yes)!=FALSE,"expected confirmation Yes button enabled");
    ++confirmations;stage=11;
    Need(PostMessageW(yes,BM_CLICK,0,0)!=FALSE,"click Yes only in fixture-owned exact clear-cache confirmation");
    return TRUE;
}
void Capture(const std::wstring& suffix) {
    RECT bounds{};Need(GetWindowRect(dialog,&bounds)!=FALSE,"capture real dialog bounds");int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    Need(width>0&&height>0&&width<=16384&&height<=16384,"bounded nonempty native capture dimensions");
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    Need(dc!=nullptr,"capture DC");void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    Need(bitmap&&pixels,"capture bitmap");auto old=SelectObject(dc,bitmap);Need(old&&old!=HGDI_ERROR,"select capture bitmap");
    auto* data=static_cast<DWORD*>(pixels);const auto count=std::size_t(width)*height;std::fill_n(data,count,0x00ff00ff);
    SendMessageW(dialog,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);GdiFlush();
    Check(std::any_of(data,data+count,[](DWORD pixel){return (pixel&0x00ffffff)!=0x00ff00ff;}),"production WM_PRINT painted capture");
    Check(std::any_of(data,data+count,[&](DWORD pixel){return (pixel&0x00ffffff)!=(data[0]&0x00ffffff);}),"native capture is not a uniform blank surface");
    const auto path=output/(L"dialog-"+std::to_wstring(language)+L"-"+suffix+L".bmp");FILE* file=nullptr;
    Need(_wfopen_s(&file,path.c_str(),L"wb")==0&&file,"open BMP evidence");
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+DWORD(count*4);
    const bool written=fwrite(&header,sizeof(header),1,file)==1&&fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,file)==1&&fwrite(pixels,4,count,file)==count;
    const auto closed=fclose(file);Need(written&&closed==0&&fs::file_size(path)==header.bfSize,"write complete BMP evidence");
    evidence<<"bmp\tfile="<<Utf8(path.filename().wstring())<<"\twidth="<<width<<"\theight="<<height<<"\tsource=production-WM_PRINT-logical-pixels\n";
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
void RectEvidence(const char* name,const RECT& rect){evidence<<'\t'<<name<<'='<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom;}
bool Inside(const RECT& outer,const RECT& inner){return inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom&&inner.right>inner.left&&inner.bottom>inner.top;}

void CheckConfirmationWorkArea(HWND window, HWND yes, HWND no) {
    RECT bounds{}, yesBounds{}, noBounds{};
    Need(GetWindowRect(window, &bounds) && GetWindowRect(yes, &yesBounds) &&
        GetWindowRect(no, &noBounds), "measure the real clear-cache confirmation and its buttons");
    Check(Inside(constrainedWork, bounds) && Inside(bounds, yesBounds) && Inside(bounds, noBounds),
        "the confirmation and both answers fit the constrained work area");
    const auto defaultButton = SendMessageW(window, DM_GETDEFID, 0, 0);
    Check(HIWORD(defaultButton) == DC_HASDEFID && LOWORD(defaultButton) == IDNO && IsWindowEnabled(no),
        "moving the confirmation keeps No as its enabled default answer");
    evidence << "confirmation-work-area\tlang=" << language;
    RectEvidence("work", constrainedWork);
    RectEvidence("window", bounds);
    RectEvidence("yes", yesBounds);
    RectEvidence("no", noBounds);
    evidence << '\n';
}

bool InstallConstrainedWorkArea() {
    constrainedMonitor = MonitorFromWindow(fixtureOwner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof info};
    Need(constrainedMonitor && GetMonitorInfoW(constrainedMonitor, &info), "read the real monitor before constraining its work area");
    const int frameWidth = templateFrame.right - templateFrame.left - templateClient.right;
    const int frameHeight = templateFrame.bottom - templateFrame.top - templateClient.bottom;
    const int width = frameWidth + (templateClient.right * constrainedPercent + 99) / 100;
    const int height = frameHeight + (templateClient.bottom * constrainedPercent + 99) / 100;
    const auto right = info.rcWork.right - 8;
    const auto bottom = info.rcWork.bottom - 8;
    constrainedWork = {right - width, bottom - height, right, bottom};
    Need(Inside(info.rcWork, constrainedWork), "the constrained work area is a strict subset of the physical work area");
    const auto init = MH_Initialize();
    Need(init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED, "initialize the constrained work area probe");
    const auto target = reinterpret_cast<void*>(&GetMonitorInfoW);
    Need(MH_CreateHook(target, reinterpret_cast<void*>(&ConstrainedMonitorInfo),
        reinterpret_cast<void**>(&realMonitorInfo)) == MH_OK, "observe the production monitor work area query");
    constrainedThread = GetCurrentThreadId();
    workAreaReads = 0;
    Need(MH_EnableHook(target) == MH_OK, "return the smaller work area on the fixture UI thread only");
    evidence << "constrained-work-area\trequested=" << interfacePercent << "\tcapacity=" << constrainedPercent;
    RectEvidence("real-work", info.rcWork);
    RectEvidence("work", constrainedWork);
    evidence << '\n';
    return init == MH_OK;
}

void RemoveConstrainedWorkArea(bool initialized) {
    Check(workAreaReads > 0, "the production dialog queried the constrained work area");
    evidence << "work-area-reads\tlang=" << language << "\tcalls=" << workAreaReads << '\n';
    const auto target = reinterpret_cast<void*>(&GetMonitorInfoW);
    Need(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK, "remove the constrained work area probe");
    if (initialized) Need(MH_Uninitialize() == MH_OK, "release the constrained work area probe");
    constrainedThread = 0;
}
struct FontMeasurement {
    HWND window;HDC dc;HGDIOBJ previous;
    LOGFONTW logical{};
    explicit FontMeasurement(HWND control):window(control),dc(GetDC(control)),previous(nullptr){
        Need(dc!=nullptr,"control text measurement DC");
        const auto assigned=reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0));
        const auto font=assigned?assigned:reinterpret_cast<HFONT>(GetStockObject(SYSTEM_FONT));
        previous=SelectObject(dc,font);Need(previous&&previous!=HGDI_ERROR,"select actual control font");
        TEXTMETRICW metrics{};wchar_t face[LF_FACESIZE]{};
        Need(GetObjectW(font,sizeof logical,&logical)==sizeof logical&&GetTextMetricsW(dc,&metrics)&&GetTextFaceW(dc,LF_FACESIZE,face)>0,"read actual control font evidence");
        evidence<<"font\tid="<<GetDlgCtrlID(control)<<"\tsource="<<(assigned?"WM_GETFONT":"SYSTEM_FONT")
            <<"\trequested="<<Utf8(logical.lfFaceName)<<"\tactual="<<Utf8(face)<<"\theight="<<logical.lfHeight
            <<"\tweight="<<logical.lfWeight<<"\tcharset="<<unsigned(logical.lfCharSet)<<"\titalic="<<unsigned(logical.lfItalic)
            <<"\tmetrics="<<metrics.tmHeight<<','<<metrics.tmAscent<<','<<metrics.tmDescent<<','<<metrics.tmExternalLeading
            <<"\tdc-dpi="<<GetDeviceCaps(dc,LOGPIXELSX)<<','<<GetDeviceCaps(dc,LOGPIXELSY)<<'\n';
    }
    ~FontMeasurement(){SelectObject(dc,previous);ReleaseDC(window,dc);}
    RECT Measure(const std::wstring& text,int width,UINT flags){
        RECT rect{0,0,width,0};if(!text.empty())Need(DrawTextW(dc,text.c_str(),int(text.size()),&rect,flags|DT_CALCRECT)>0,"measure complete localized text");return rect;
    }
};
void ListGeometry(HWND list,int effective){
    HWND header=ListView_GetHeader(list);Need(header!=nullptr,"real project list header");
    if(interfacePercent>100)for(HWND control:{list,header}){
        LOGFONTW font{};
        const auto assigned=reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0));
        Check(assigned&&GetObjectW(assigned,sizeof font,&font)==sizeof font&&
            font.lfHeight==MulDiv(templateFontHeight,effective,100),"list rows and header follow the application percentage");
    }
    const int columns=Header_GetItemCount(header),rows=ListView_GetItemCount(list);
    Check(columns==6,"all six project columns exist");RECT viewport{};GetClientRect(list,&viewport);
    {FontMeasurement font(header);
        for(int col=0;col<columns;++col){
            wchar_t text[2048]{};HDITEMW item{};item.mask=HDI_TEXT|HDI_FORMAT|HDI_WIDTH;item.pszText=text;item.cchTextMax=2048;
            Need(SendMessageW(header,HDM_GETITEMW,col,reinterpret_cast<LPARAM>(&item))!=FALSE,"read actual localized list header");
            Need(std::wcslen(text)<2047,"complete header text evidence");
            RECT rect{};Need(Header_GetItemRect(header,col,&rect)!=FALSE,"header item geometry");
            const auto needed=font.Measure(text,0,DT_SINGLELINE);
            const int padding=int(SendMessageW(header,HDM_GETBITMAPMARGIN,0,0))*2;
            evidence<<"header\tcolumn="<<col<<"\ttext="<<Utf8(text)<<"\trequired="<<needed.right+padding<<','<<needed.bottom;RectEvidence("rect",rect);evidence<<'\n';
            Check(needed.right+padding<=rect.right-rect.left&&needed.bottom<=rect.bottom-rect.top,"localized list header fits without truncation");
            Check(rect.left>=viewport.left&&rect.right<=viewport.right,"all project columns fit the visible list width");
        }
    }
    FontMeasurement font(list);
    for(int row=0;row<rows;++row)for(int col=0;col<columns;++col){
        std::vector<wchar_t> text(256);LRESULT copied=0;
        for(;;){LVITEMW item{};item.iSubItem=col;item.pszText=text.data();item.cchTextMax=int(text.size());
            copied=SendMessageW(list,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));
            if(copied<int(text.size())-1)break;Need(text.size()<32768,"bounded full list cell text");text.resize(text.size()*2);}
        RECT rect{};Need(ListView_GetSubItemRect(list,row,col,LVIR_LABEL,&rect)!=FALSE,"list cell label bounds");
        // For subitem zero some comctl versions return the whole row. Intersect
        // with the actual header column before checking the available text width.
        RECT column{};Need(Header_GetItemRect(header,col,&column)!=FALSE,"cell column bounds");
        MapWindowPoints(header,list,reinterpret_cast<POINT*>(&column),2);
        rect.left=std::max(rect.left,column.left);rect.right=std::min(rect.right,column.right);
        const auto needed=font.Measure(text.data(),0,DT_SINGLELINE|DT_NOPREFIX);
        const int padding=MulDiv(6,int(getWindowDpi(list)),96);
        evidence<<"cell\trow="<<row<<"\tcolumn="<<col<<"\ttext="<<Utf8(text.data())<<"\trequired="<<needed.right+padding<<','<<needed.bottom;RectEvidence("rect",rect);evidence<<'\n';
        Check(Inside(viewport,rect),"project cell is wholly visible");
        Check(needed.right+padding<=rect.right-rect.left&&needed.bottom<=rect.bottom-rect.top,"project cell text fits without ellipsis");
    }
}
void Geometry() {
    const UINT systemDpi=SystemDpi(),windowDpi=getWindowDpi(dialog);
    Need(windowDpi!=0,"query actual dialog DPI");
    Check(getAwareness(getWindowDpiContext(dialog))==0,"dialog awareness matches production main");
    RECT client{},window{};GetClientRect(dialog,&client);GetWindowRect(dialog,&window);
    RECT expected{0,0,360,206};Need(MapDialogRect(dialog,&expected)!=FALSE,"map production dialog units");
    int effective=100;
    if(interfacePercent>100){
        MONITORINFO monitor{sizeof monitor};
        Need(GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"read the actual work area for this scaled dialog");
        const RECT work=monitor.rcWork;
        const int frameWidth=templateFrame.right-templateFrame.left-templateClient.right;
        const int frameHeight=templateFrame.bottom-templateFrame.top-templateClient.bottom;
        effective=std::min({interfacePercent,int((work.right-work.left-frameWidth)*100/templateClient.right),
            int((work.bottom-work.top-frameHeight)*100/templateClient.bottom)});
        expected={0,0,MulDiv(templateClient.right,effective,100),MulDiv(templateClient.bottom,effective,100)};
        if (constrainedThread) {
            Check(effective==constrainedPercent && EqualRect(&work,&constrainedWork),
                "the constrained monitor forces the expected reduced application percentage");
            Check(Inside(work,window),"the cache manager fits and is repositioned inside the smaller work area");
        } else {
            Check(effective>100&&Inside(work,window),"larger dialog stays entirely inside the monitor work area");
        }
        evidence<<"app-scale\trequested="<<interfacePercent<<"\teffective="<<effective<<"\tlang="<<language<<"\tstage="<<stage;
        RectEvidence("work",work);evidence<<'\n';
    }
    evidence<<"snapshot\tlanguage="<<language<<"\tstage="<<stage<<"\tsystem-dpi="<<systemDpi<<"\twindow-dpi="<<windowDpi
        <<"\tawareness="<<getAwareness(getWindowDpiContext(dialog))<<"\ttitle="<<Utf8(Text(dialog));
    RectEvidence("window",window);RectEvidence("client",client);RectEvidence("expected-client",expected);evidence<<'\n';
    Check(client.right==expected.right&&client.bottom==expected.bottom,"dialog matches the template at the effective application percentage");
    {FontMeasurement font(dialog);}
    const int controls[]={ui::Enabled,ui::Automatic,ui::LimitLabel,ui::Limit,ui::RecentLabel,ui::Recent,ui::Summary,ui::Kinds,
        ui::Projects,ui::Path,ui::Pin,ui::ClearProject,ui::ClearReclaimable,ui::ClearAll,ui::Status,ui::Apply,IDCANCEL};
    unsigned directChildren=0;
    for(HWND child=GetWindow(dialog,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))++directChildren;
    Check(directChildren==sizeof controls/sizeof controls[0],"all direct dialog controls covered by geometry evidence");
    std::vector<RECT> rectangles;
    for(int id:controls) {
        HWND control=GetDlgItem(dialog,id);Need(control&&GetParent(control)==dialog,"required production control exists");
        RECT rect{},inner{};GetWindowRect(control,&rect);GetClientRect(control,&inner);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&rect),2);
        const auto type=Class(control),text=Text(control);const auto style=GetWindowLongPtrW(control,GWL_STYLE);
        evidence<<"control\tid="<<id<<"\tclass="<<Utf8(type)<<"\tstyle="<<style<<"\texstyle="<<GetWindowLongPtrW(control,GWL_EXSTYLE)
            <<"\tenabled="<<IsWindowEnabled(control)<<"\tdpi="<<getWindowDpi(control)<<"\ttext="<<Utf8(text);
        RectEvidence("rect",rect);RectEvidence("client",inner);evidence<<'\n';
        Check(Inside(client,rect),"control stays inside dialog");
        for(const auto& other:rectangles){RECT intersection{};Check(!IntersectRect(&intersection,&rect,&other),"sibling controls do not overlap");}rectangles.push_back(rect);
        FontMeasurement font(control);
        if(interfacePercent>100)Check(font.logical.lfHeight==MulDiv(templateFontHeight,effective,100),
            "visible control font follows the effective application percentage");
        if(id==ui::Projects)continue;
        const bool button=type==L"Button",edit=type==L"Edit",check=id==ui::Enabled||id==ui::Automatic;
        UINT flags=DT_SINGLELINE;
        if(type==L"Static"){
            if(style&SS_NOPREFIX)flags|=DT_NOPREFIX;
            // Do not permit an ellipsis style to hide a text-fit failure.
            if(!(style&SS_ELLIPSISMASK)&&((style&SS_TYPEMASK)==SS_LEFT||(style&SS_TYPEMASK)==SS_CENTER||(style&SS_TYPEMASK)==SS_RIGHT))flags&=~DT_SINGLELINE;
            if(!(flags&DT_SINGLELINE))flags|=DT_WORDBREAK;
        }
        if(edit){SendMessageW(control,EM_GETRECT,0,reinterpret_cast<LPARAM>(&inner));flags|=DT_NOPREFIX;}
        const int padding=button?MulDiv(check?24:12,int(windowDpi),96):0;
        const auto needed=font.Measure(text,inner.right-inner.left-padding,flags);
        evidence<<"text-fit\tid="<<id<<"\trequired="<<needed.right+padding<<','<<needed.bottom
            <<"\tavailable="<<inner.right-inner.left<<','<<inner.bottom-inner.top<<"\tflags="<<flags<<'\n';
        if(id==ui::Path) {
            // This existing full-path field intentionally uses native middle
            // ellipsis. Arbitrarily long project paths cannot fit one line;
            // labels and buttons below still require their complete captions.
            Check(type==L"Static" && (style&SS_ELLIPSISMASK)==SS_PATHELLIPSIS &&
                  needed.bottom<=inner.bottom-inner.top,"native project-path ellipsis retains a full-height line");
            const int row=ListView_GetNextItem(GetDlgItem(dialog,ui::Projects),-1,LVNI_SELECTED);
            const auto view=cache::Inspect();
            Check(row<0 ? text.empty() : std::size_t(row)<view.projects.size() &&
                  text==view.projects[std::size_t(row)].path,"path control retains the exact unabridged selected project path");
        } else Check(needed.right+padding<=inner.right-inner.left&&needed.bottom<=inner.bottom-inner.top,"complete localized control text fits width and height");
        if(button&&!check){SIZE ideal{};Need(SendMessageW(control,BCM_GETIDEALSIZE,0,reinterpret_cast<LPARAM>(&ideal))!=FALSE,"native button ideal size");
            evidence<<"button-ideal\tid="<<id<<"\tsize="<<ideal.cx<<','<<ideal.cy<<'\n';
            Check(ideal.cx<=rect.right-rect.left&&ideal.cy<=rect.bottom-rect.top,"native pushbutton ideal text size fits");}
    }
    Check(!GetDlgItem(dialog,ui::Explanation),"footer paragraphs removed");
    RECT summary{},kinds{};GetWindowRect(GetDlgItem(dialog,ui::Summary),&summary);GetWindowRect(GetDlgItem(dialog,ui::Kinds),&kinds);
    Check(summary.top==kinds.top&&summary.right<=kinds.left,"MME and texture usage shares the summary row");
    ListGeometry(GetDlgItem(dialog,ui::Projects),effective);
    Capture(L"stage-"+std::to_wstring(stage)+L"-system-"+std::to_wstring(systemDpi)+L"-window-"+std::to_wstring(windowDpi));
    evidence.flush();Need(bool(evidence),"flush complete geometry/font/text/DPI evidence");
}

#if !MMD_ORIGINAL_PROFILE
// ---- Real cache-dialog production backdrop paint regression ----
// The accepted LivePaintProbe principle from tests/backup_dialog_checks.hpp is
// adapted here for this dialog only, not the backup framework. Detours stay
// inert unless a stack-owned probe is active for this exact dialog, this UI
// thread, and the live lease between its matching BeginPaint and EndPaint.
// Every unmatched call and every other thread is forwarded unchanged.
constexpr unsigned paintMaxPasses = 4;
constexpr unsigned paintMaxChildren = 32;
constexpr unsigned paintMaxBackground = 2;
constexpr unsigned paintSnapshotChildren = 32;
constexpr UINT_PTR paintObserverId = 0x4D4D5332;
struct PaintChild { HWND window = nullptr; RECT client{}; };
struct PaintClip {
    bool recorded = false;
    unsigned childRects = 0, clippedChildren = 0;
    int childVisible[paintMaxChildren]{};
    unsigned backgroundPoints = 0, backgroundVisible = 0;
    int backgroundPtVisible[paintMaxBackground]{-1, -1};
    POINT gap{}; int gapVisible = -1;
    POINT outside{}; int outsideVisible = -1;
    bool drawDcLive = false;
};
struct PaintProbe {
    DWORD thread = 0;
    HWND window = nullptr;
    bool active = false;
    HDC paintDc = nullptr;
    bool paintDcLive = false;
    bool beginPaintFailed = false;
    unsigned beginPaintCalls = 0;
    // Production paint lease: armed only after the first real matching BeginPaint
    // returns and revoked before its matching EndPaint is forwarded. EndPaint can
    // release the DC and a later callback may reuse the same numeric HDC; the
    // lease and WindowFromDC/GetObjectType checks keep injection off that reuse.
    bool firstPaintSeen = false;
    bool leaseActive = false;
    bool firstEndPaintSeen = false;
    bool leaseClosed = false;
    bool endPaintHdcMatched = false;
    bool laterBeginPaintSeen = false;
    unsigned endPaintCalls = 0;
    HDC paintDcs[paintMaxPasses]{};
    RECT paintRects[paintMaxPasses]{};
    int mapMode = 0;
    POINT origin{};
    unsigned paintMessages = 0, eraseMessages = 0, eraseAfterReplay = 0, eraseNonzeroAfterReplay = 0;
    HRGN expectedDamage = nullptr;
    HRGN childrenRegion = nullptr;
    RECT client{};
    RECT expectedBox{};
    PaintChild children[paintMaxChildren]{};
    unsigned childCount = 0;
    unsigned backgroundCount = 0;
    POINT background[paintMaxBackground]{};
    POINT gap{};
    POINT outside{};
    unsigned allocationAttempts = 0, allocationSuccesses = 0, allocationDenials = 0;
    HBITMAP allocated = nullptr, bufferBitmap = nullptr;
    unsigned copyAttempts = 0, copySuccesses = 0, copyDenials = 0;
    bool copyRecorded = false;
    RECT copyRect{};
    int copySourceX = 0, copySourceY = 0;
    DWORD copyRop = 0;
    HBITMAP copySource = nullptr;
    int copySourceWidth = 0, copySourceHeight = 0;
    unsigned fillAttempts = 0, fillSuccesses = 0, fillDenials = 0;
    bool fillRecorded = false;
    RECT fillRect{};
    HBRUSH fillBrush = nullptr;
    bool deniedFillDamage = false;
    unsigned invalidateCalls = 0, eraseInvalidates = 0, replays = 0;
    // Native replay erase evidence. The OS is not required to route the base
    // dialog procedure's replay through the exported BeginPaint/EndPaint pair,
    // so proof of native painting comes from the real WM_ERASEBKGND callback.
    unsigned replayCallbackCount = 0, replayLiveErase = 0, replaySuppliedErase = 0;
    bool replaySuppliedRecorded = false;
    int replaySuppliedObjectType = 0;
    HWND replaySuppliedWindowFromDc = nullptr;
    int replaySuppliedClipBoxType = 0;
    RECT replaySuppliedClipBox{};
    int replaySuppliedMapMode = 0;
    POINT replaySuppliedOrigin{};
    bool replayClipRecorded = false;
    int replayObjectType = 0;
    HWND replayWindowFromDc = nullptr;
    int replayClipBoxType = 0;
    RECT replayClipBox{};
    int replayMapMode = 0;
    POINT replayOrigin{};
    unsigned replayChildRects = 0, replayClippedChildren = 0;
    int replayChildVisible[paintMaxChildren]{};
    unsigned replayBackgroundVisible = 0;
    int replayBackgroundPtVisible[paintMaxBackground]{-1, -1};
    int replayGapVisible = -1, replayOutsideVisible = -1;
    bool replaySentinelAttempted = false, replaySentinelWritten = false;
    bool replayPixelProofDone = false;
    bool replaySetPixelOk[paintMaxBackground]{}, replayGetPixelBeforeOk[paintMaxBackground]{};
    bool replayGetPixelAfterOk[paintMaxBackground]{}, replayAfterFace[paintMaxBackground]{};
    COLORREF replaySentinelColor = 0;
    COLORREF replayBeforeColor[paintMaxBackground]{}, replayAfterColor[paintMaxBackground]{};
    bool replayHdcValidAfter = false, replayAfterReadValid = false;
    bool replayInjectionInactive = false;
    LRESULT replayCallbackResult = 0;
    // Inner dialog-procedure observation: the message has already passed the
    // production backdrop subclass, so the handed-down DC clip is stable and is
    // sampled before the original dialog procedure/default erase runs.
    unsigned innerCallbacks = 0, innerLiveDc = 0, innerSuppliedDc = 0;
    bool innerRecorded = false;
    int innerObjectType = 0;
    HWND innerWindowFromDc = nullptr;
    int innerClipBoxType = 0;
    RECT innerClipBox{};
    int innerMapMode = 0;
    POINT innerOrigin{};
    unsigned innerChildRects = 0, innerClippedChildren = 0;
    int innerChildVisible[paintMaxChildren]{};
    unsigned innerBackgroundVisible = 0;
    int innerBackgroundPtVisible[paintMaxBackground]{-1, -1};
    int innerGapVisible = -1, innerOutsideVisible = -1;
    bool innerSuppliedRecorded = false;
    int innerSuppliedObjectType = 0;
    HWND innerSuppliedWindowFromDc = nullptr;
    int innerSuppliedClipBoxType = 0;
    RECT innerSuppliedClipBox{};
    int innerSuppliedMapMode = 0;
    POINT innerSuppliedOrigin{};
    unsigned allocationDenialBudget = 0, copyDenialBudget = 0, fillDenialBudget = 0;
    PaintClip clip;
};
thread_local PaintProbe* activePaintProbe = nullptr;
HBITMAP paintColdSource = nullptr, paintRecoveredSource = nullptr;
bool paintHooksInitialized = false;
DLGPROC paintOriginalDialogProc = nullptr;
using PaintCreateDibFn = HBITMAP(WINAPI*)(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
using PaintBitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
using PaintFillRectFn = int(WINAPI*)(HDC, const RECT*, HBRUSH);
using PaintBeginPaintFn = HDC(WINAPI*)(HWND, PAINTSTRUCT*);
using PaintEndPaintFn = BOOL(WINAPI*)(HWND, const PAINTSTRUCT*);
using PaintInvalidateRgnFn = BOOL(WINAPI*)(HWND, HRGN, BOOL);
PaintCreateDibFn realPaintCreateDib = nullptr;
PaintBitBltFn realPaintBitBlt = nullptr;
PaintFillRectFn realPaintFillRect = nullptr;
PaintBeginPaintFn realPaintBeginPaint = nullptr;
PaintEndPaintFn realPaintEndPaint = nullptr;
PaintInvalidateRgnFn realPaintInvalidateRgn = nullptr;
// Runs inside the matched production draw call: the BeginPaint DC clip is the
// real update region intersected with the client minus immediate visible
// children. -1 from RectVisible/PtVisible is a query failure, never a pass.
void PaintObserveClip(PaintProbe& probe, HDC target) {
    if (probe.clip.recorded) return;
    probe.clip.recorded = true;
    probe.clip.drawDcLive = WindowFromDC(target) == probe.window && GetObjectType(target) == OBJ_DC;
    probe.clip.childRects = probe.childCount;
    for (unsigned i = 0; i < probe.childCount; ++i) {
        RECT rect = probe.children[i].client;
        probe.clip.childVisible[i] = RectVisible(target, &rect);
        if (probe.clip.childVisible[i] == FALSE) ++probe.clip.clippedChildren;
    }
    probe.clip.backgroundPoints = probe.backgroundCount;
    for (unsigned i = 0; i < probe.backgroundCount; ++i) {
        probe.clip.backgroundPtVisible[i] = PtVisible(target, probe.background[i].x, probe.background[i].y);
        if (probe.clip.backgroundPtVisible[i] == TRUE) ++probe.clip.backgroundVisible;
    }
    probe.clip.gap = probe.gap;
    probe.clip.gapVisible = PtVisible(target, probe.gap.x, probe.gap.y);
    probe.clip.outside = probe.outside;
    probe.clip.outsideVisible = PtVisible(target, probe.outside.x, probe.outside.y);
}
// Current ownership of the live production paint DC. WindowFromDC/GetObjectType
// are re-queried instead of trusting the cached BeginPaint fact; a numeric HDC
// reused after EndPaint is not that production DC. LastError is preserved.
bool PaintOwnedDc(PaintProbe& live, HDC target) {
    if (!live.leaseActive || target != live.paintDc) return false;
    const DWORD saved = GetLastError();
    const bool owned = WindowFromDC(target) == live.window && GetObjectType(target) == OBJ_DC;
    SetLastError(saved);
    return owned;
}
HBITMAP WINAPI ObservedPaintCreateDib(HDC target, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread) return realPaintCreateDib(target, info, usage, bits, section, offset);
    const DWORD incoming = GetLastError();
    if (!PaintOwnedDc(*live, target)) {
        SetLastError(incoming);
        return realPaintCreateDib(target, info, usage, bits, section, offset);
    }
    ++live->allocationAttempts;
    if (live->allocationDenialBudget > 0) {
        --live->allocationDenialBudget;
        ++live->allocationDenials;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    SetLastError(incoming);
    const HBITMAP bitmap = realPaintCreateDib(target, info, usage, bits, section, offset);
    const DWORD returned = GetLastError();
    if (bitmap) { ++live->allocationSuccesses; live->allocated = bitmap; live->bufferBitmap = bitmap; }
    SetLastError(returned);
    return bitmap;
}
BOOL WINAPI ObservedPaintBitBlt(HDC target, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD operation) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread) return realPaintBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
    const DWORD incoming = GetLastError();
    if (!PaintOwnedDc(*live, target)) {
        SetLastError(incoming);
        return realPaintBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
    }
    const HBITMAP sourceBitmap = reinterpret_cast<HBITMAP>(GetCurrentObject(source, OBJ_BITMAP));
    if (live->bufferBitmap && sourceBitmap == live->bufferBitmap) {
        ++live->copyAttempts;
        if (!live->copyRecorded) {
            live->copyRecorded = true;
            live->copyRect = RECT{x, y, x + width, y + height};
            live->copySourceX = sourceX;
            live->copySourceY = sourceY;
            live->copyRop = operation;
            live->copySource = sourceBitmap;
            BITMAP bitmap{};
            if (GetObjectW(sourceBitmap, sizeof bitmap, &bitmap) == sizeof bitmap) {
                live->copySourceWidth = bitmap.bmWidth;
                live->copySourceHeight = bitmap.bmHeight;
            }
            PaintObserveClip(*live, target);
        }
        const bool expectedDamage = x == live->expectedBox.left && y == live->expectedBox.top &&
            width == live->expectedBox.right - live->expectedBox.left &&
            height == live->expectedBox.bottom - live->expectedBox.top;
        if (live->copyDenialBudget > 0 && expectedDamage) {
            --live->copyDenialBudget;
            ++live->copyDenials;
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        SetLastError(incoming);
        const BOOL result = realPaintBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
        const DWORD returned = GetLastError();
        if (result) ++live->copySuccesses;
        SetLastError(returned);
        return result;
    }
    SetLastError(incoming);
    return realPaintBitBlt(target, x, y, width, height, source, sourceX, sourceY, operation);
}
int WINAPI ObservedPaintFillRect(HDC target, const RECT* rect, HBRUSH brush) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread) return realPaintFillRect(target, rect, brush);
    const DWORD incoming = GetLastError();
    if (!PaintOwnedDc(*live, target)) {
        SetLastError(incoming);
        return realPaintFillRect(target, rect, brush);
    }
    ++live->fillAttempts;
    if (!live->fillRecorded) {
        live->fillRecorded = true;
        if (rect) live->fillRect = *rect;
        live->fillBrush = brush;
        PaintObserveClip(*live, target);
    }
    const bool expectedDamage = rect && EqualRect(rect, &live->expectedBox);
    if (live->fillDenialBudget > 0 && expectedDamage) {
        --live->fillDenialBudget;
        ++live->fillDenials;
        live->deniedFillDamage = true;
        SetLastError(ERROR_GEN_FAILURE);
        return 0;
    }
    SetLastError(incoming);
    const int result = realPaintFillRect(target, rect, brush);
    const DWORD returned = GetLastError();
    if (result != 0) ++live->fillSuccesses;
    SetLastError(returned);
    return result;
}
HDC WINAPI ObservedPaintBeginPaint(HWND window, PAINTSTRUCT* paint) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return realPaintBeginPaint(window, paint);
    const DWORD incoming = GetLastError();
    SetLastError(incoming);
    const HDC target = realPaintBeginPaint(window, paint);
    const DWORD returned = GetLastError();
    const unsigned pass = live->beginPaintCalls++;
    if (target && paint) {
        if (pass < paintMaxPasses) { live->paintDcs[pass] = target; live->paintRects[pass] = paint->rcPaint; }
        if (pass == 0) {
            // First real production BeginPaint: record it, then arm the lease
            // after the real call returned. Only this pass may be injected.
            live->paintDc = target;
            live->paintDcLive = WindowFromDC(target) == window && GetObjectType(target) == OBJ_DC;
            live->mapMode = GetMapMode(target);
            GetViewportOrgEx(target, &live->origin);
            live->firstPaintSeen = true;
            live->leaseActive = true;
        } else {
            live->laterBeginPaintSeen = true;
        }
    } else {
        live->beginPaintFailed = true;
    }
    SetLastError(returned);
    return target;
}
BOOL WINAPI ObservedPaintEndPaint(HWND window, const PAINTSTRUCT* paint) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return realPaintEndPaint(window, paint);
    const DWORD incoming = GetLastError();
    ++live->endPaintCalls;
    if (live->leaseActive && live->firstPaintSeen) {
        // Revoke before forwarding: the real EndPaint may release this DC and the
        // system may hand the same numeric HDC to later paint work immediately.
        live->endPaintHdcMatched = paint && paint->hdc == live->paintDc;
        live->leaseActive = false;
        live->firstEndPaintSeen = true;
        live->leaseClosed = true;
    }
    SetLastError(incoming);
    const BOOL result = realPaintEndPaint(window, paint);
    const DWORD returned = GetLastError();
    SetLastError(returned);
    return result;
}
BOOL WINAPI ObservedPaintInvalidateRgn(HWND window, HRGN region, BOOL erase) {
    auto* live = activePaintProbe;
    if (!live || !live->active || GetCurrentThreadId() != live->thread || window != live->window)
        return realPaintInvalidateRgn(window, region, erase);
    const DWORD incoming = GetLastError();
    ++live->invalidateCalls;
    if (erase) {
        ++live->eraseInvalidates;
        if (region && live->expectedDamage && EqualRgn(live->expectedDamage, region) == TRUE) ++live->replays;
    }
    SetLastError(incoming);
    const BOOL result = realPaintInvalidateRgn(window, region, erase);
    const DWORD returned = GetLastError();
    SetLastError(returned);
    return result;
}
// True only for a point that lies inside the real staged damage and outside
// every immediate visible child. The replay sentinel writes are restricted to
// exactly these points.
bool PaintReplayBackgroundPoint(const PaintProbe& live, POINT point) {
    if (!live.expectedDamage || PtInRegion(live.expectedDamage, point.x, point.y) == FALSE) return false;
    for (unsigned i = 0; i < live.childCount; ++i) {
        const RECT& rect = live.children[i].client;
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom) return false;
    }
    return true;
}
// Called only from a real WM_ERASEBKGND delivered after the native replay
// invalidation: the first production paint lease is already closed and exactly
// one erase-enabled replay invalidation matched. A live dialog window DC gets
// full clip checks plus two sentinel pixels in damaged parent background; a
// supplied memory DC is recorded separately and never treated as screen proof.
bool PaintReplayObserveErase(PaintProbe& live, HDC target) {
    const DWORD saved = GetLastError();
    const DWORD objectType = DWORD(GetObjectType(target));
    const HWND fromDc = WindowFromDC(target);
    const bool liveDc = objectType == OBJ_DC && fromDc == live.window;
    if (!liveDc) {
        ++live.replaySuppliedErase;
        if (!live.replaySuppliedRecorded) {
            live.replaySuppliedRecorded = true;
            live.replaySuppliedObjectType = int(objectType);
            live.replaySuppliedWindowFromDc = fromDc;
            live.replaySuppliedClipBoxType = GetClipBox(target, &live.replaySuppliedClipBox);
            live.replaySuppliedMapMode = GetMapMode(target);
            GetViewportOrgEx(target, &live.replaySuppliedOrigin);
        }
        SetLastError(saved);
        return false;
    }
    ++live.replayLiveErase;
    // Evaluate live erase callbacks until one provides the pixel proof; after
    // that, later live erase callbacks are counted but do not overwrite it.
    if (live.replayPixelProofDone) { SetLastError(saved); return false; }
    live.replayClipRecorded = true;
    live.replayObjectType = int(objectType);
    live.replayWindowFromDc = fromDc;
    live.replayClipBoxType = GetClipBox(target, &live.replayClipBox);
    live.replayMapMode = GetMapMode(target);
    GetViewportOrgEx(target, &live.replayOrigin);
    live.replayChildRects = live.childCount;
    live.replayClippedChildren = 0;
    for (unsigned i = 0; i < live.childCount; ++i) {
        RECT rect = live.children[i].client;
        live.replayChildVisible[i] = RectVisible(target, &rect);
        if (live.replayChildVisible[i] == FALSE) ++live.replayClippedChildren;
    }
    live.replayBackgroundVisible = 0;
    for (unsigned i = 0; i < live.backgroundCount; ++i) {
        live.replayBackgroundPtVisible[i] = PtVisible(target, live.background[i].x, live.background[i].y);
        if (live.replayBackgroundPtVisible[i] == TRUE) ++live.replayBackgroundVisible;
    }
    live.replayGapVisible = PtVisible(target, live.gap.x, live.gap.y);
    live.replayOutsideVisible = PtVisible(target, live.outside.x, live.outside.y);
    bool sentinel = false;
    const bool bothVisible = live.backgroundCount == paintMaxBackground &&
        live.replayBackgroundPtVisible[0] == TRUE && live.replayBackgroundPtVisible[1] == TRUE;
    if (bothVisible) {
        live.replaySentinelAttempted = true;
        bool validPoints = true;
        for (unsigned i = 0; i < paintMaxBackground; ++i)
            if (!PaintReplayBackgroundPoint(live, live.background[i])) validPoints = false;
        if (validPoints) {
            COLORREF face = GetSysColor(COLOR_3DFACE);
            COLORREF sentinelColor = RGB(0xFF, 0x00, 0xFF);
            if (sentinelColor == face) sentinelColor = RGB(0x00, 0x01, 0x02);
            live.replaySentinelColor = sentinelColor;
            bool written = true, confirmed = true;
            for (unsigned i = 0; i < paintMaxBackground; ++i) {
                live.replaySetPixelOk[i] = SetPixelV(target, live.background[i].x, live.background[i].y, sentinelColor) != FALSE;
                if (!live.replaySetPixelOk[i]) written = false;
            }
            GdiFlush();
            for (unsigned i = 0; i < paintMaxBackground; ++i) {
                live.replayBeforeColor[i] = GetPixel(target, live.background[i].x, live.background[i].y);
                live.replayGetPixelBeforeOk[i] = live.replayBeforeColor[i] != CLR_INVALID && live.replayBeforeColor[i] == sentinelColor;
                if (!live.replayGetPixelBeforeOk[i]) confirmed = false;
            }
            live.replaySentinelWritten = written && confirmed;
            sentinel = live.replaySentinelWritten;
        }
    }
    SetLastError(saved);
    return sentinel;
}
void PaintReplayFinishErase(PaintProbe& live, HDC target, bool sentinel, LRESULT result) {
    const DWORD saved = GetLastError();
    live.replayCallbackResult = result;
    live.replayInjectionInactive = live.allocationDenialBudget == 0 && live.copyDenialBudget == 0 && live.fillDenialBudget == 0;
    if (!sentinel) { SetLastError(saved); return; }
    GdiFlush();
    const COLORREF face = GetSysColor(COLOR_3DFACE);
    bool readValid = true;
    for (unsigned i = 0; i < paintMaxBackground; ++i) {
        live.replayAfterColor[i] = GetPixel(target, live.background[i].x, live.background[i].y);
        live.replayGetPixelAfterOk[i] = live.replayAfterColor[i] != CLR_INVALID;
        live.replayAfterFace[i] = live.replayGetPixelAfterOk[i] && live.replayAfterColor[i] == face &&
            live.replayAfterColor[i] != live.replaySentinelColor;
        if (!live.replayGetPixelAfterOk[i]) readValid = false;
    }
    live.replayAfterReadValid = readValid;
    live.replayHdcValidAfter = WindowFromDC(target) == live.window && GetObjectType(target) == OBJ_DC;
    live.replayPixelProofDone = readValid && live.replayAfterFace[0] && live.replayAfterFace[1] && live.replayHdcValidAfter;
    SetLastError(saved);
}
// Read-only observation at the real inner dialog procedure entry. This is
// after production BackdropProc forwarded through DefSubclassProc and before
// the original dialog procedure/default erase, so it sees the actual handed-down
// clip rather than the outer subclass entry state. Queries only; no message
// result, DWLP_USER or subclass order is changed here.
void PaintInnerObserveErase(PaintProbe& live, HDC target) {
    const DWORD saved = GetLastError();
    ++live.innerCallbacks;
    const DWORD objectType = DWORD(GetObjectType(target));
    const HWND fromDc = WindowFromDC(target);
    if (!(objectType == OBJ_DC && fromDc == live.window)) {
        ++live.innerSuppliedDc;
        if (!live.innerSuppliedRecorded) {
            live.innerSuppliedRecorded = true;
            live.innerSuppliedObjectType = int(objectType);
            live.innerSuppliedWindowFromDc = fromDc;
            live.innerSuppliedClipBoxType = GetClipBox(target, &live.innerSuppliedClipBox);
            live.innerSuppliedMapMode = GetMapMode(target);
            GetViewportOrgEx(target, &live.innerSuppliedOrigin);
        }
        SetLastError(saved);
        return;
    }
    ++live.innerLiveDc;
    if (live.innerRecorded) { SetLastError(saved); return; }
    live.innerRecorded = true;
    live.innerObjectType = int(objectType);
    live.innerWindowFromDc = fromDc;
    live.innerClipBoxType = GetClipBox(target, &live.innerClipBox);
    live.innerMapMode = GetMapMode(target);
    GetViewportOrgEx(target, &live.innerOrigin);
    live.innerChildRects = live.childCount;
    live.innerClippedChildren = 0;
    for (unsigned i = 0; i < live.childCount; ++i) {
        RECT rect = live.children[i].client;
        live.innerChildVisible[i] = RectVisible(target, &rect);
        if (live.innerChildVisible[i] == FALSE) ++live.innerClippedChildren;
    }
    live.innerBackgroundVisible = 0;
    for (unsigned i = 0; i < live.backgroundCount; ++i) {
        live.innerBackgroundPtVisible[i] = PtVisible(target, live.background[i].x, live.background[i].y);
        if (live.innerBackgroundPtVisible[i] == TRUE) ++live.innerBackgroundVisible;
    }
    live.innerGapVisible = PtVisible(target, live.gap.x, live.gap.y);
    live.innerOutsideVisible = PtVisible(target, live.outside.x, live.outside.y);
    SetLastError(saved);
}
INT_PTR CALLBACK PaintObserveInnerDialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const DWORD incoming = GetLastError();
    auto* live = activePaintProbe;
    if (live && live->active && live->window == window && live->thread == GetCurrentThreadId() &&
        message == WM_ERASEBKGND && wp != 0 && live->replays == 1 && live->leaseClosed && !live->leaseActive) {
        PaintInnerObserveErase(*live, reinterpret_cast<HDC>(wp));
    }
    SetLastError(incoming);
    if (!paintOriginalDialogProc) return DefWindowProcW(window, message, wp, lp);
    const INT_PTR result = CallWindowProcW(reinterpret_cast<WNDPROC>(paintOriginalDialogProc), window, message, wp, lp);
    const DWORD returned = GetLastError();
    SetLastError(returned);
    return result;
}
// Observation-only message counts. Counting only; every verdict is asserted
// after UpdateWindow returns and the probe is cleared. The synthetic native
// replay calls the base dialog procedure directly, so a second exported
// BeginPaint is recorded when it happens but is not required by this probe.
// Native painting is proven through the real erase callback pixels below.
LRESULT CALLBACK PaintObserve(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    auto* live = activePaintProbe;
    const bool matched = live && live->active && live->window == window && live->thread == GetCurrentThreadId();
    bool replaySentinel = false;
    HDC replayDc = nullptr;
    if (matched) {
        if (message == WM_PAINT) ++live->paintMessages;
        else if (message == WM_ERASEBKGND) {
            ++live->eraseMessages;
            if (live->replays > 0) {
                ++live->eraseAfterReplay;
                if (live->replays == 1 && live->leaseClosed && !live->leaseActive && wp != 0) {
                    ++live->replayCallbackCount;
                    replayDc = reinterpret_cast<HDC>(wp);
                    replaySentinel = PaintReplayObserveErase(*live, replayDc);
                }
            }
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, PaintObserve, paintObserverId);
    const LRESULT result = DefSubclassProc(window, message, wp, lp);
    if (matched && message == WM_ERASEBKGND && live->replays > 0) {
        if (result != 0) ++live->eraseNonzeroAfterReplay;
        if (replayDc) PaintReplayFinishErase(*live, replayDc, replaySentinel, result);
    }
    return result;
}
void InstallPaintHooks() {
    const auto initialized = MH_Initialize();
    Need(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED, "initialize MinHook for the production backdrop paint regression");
    paintHooksInitialized = initialized == MH_OK;
    Need(MH_CreateHook(reinterpret_cast<void*>(&CreateDIBSection), reinterpret_cast<void*>(&ObservedPaintCreateDib),
        reinterpret_cast<void**>(&realPaintCreateDib)) == MH_OK, "hook only the production backdrop buffer allocation");
    Need(MH_CreateHook(reinterpret_cast<void*>(&BitBlt), reinterpret_cast<void*>(&ObservedPaintBitBlt),
        reinterpret_cast<void**>(&realPaintBitBlt)) == MH_OK, "hook only the production backdrop buffer copy");
    Need(MH_CreateHook(reinterpret_cast<void*>(&FillRect), reinterpret_cast<void*>(&ObservedPaintFillRect),
        reinterpret_cast<void**>(&realPaintFillRect)) == MH_OK, "hook only the production backdrop direct fill");
    Need(MH_CreateHook(reinterpret_cast<void*>(&BeginPaint), reinterpret_cast<void*>(&ObservedPaintBeginPaint),
        reinterpret_cast<void**>(&realPaintBeginPaint)) == MH_OK, "hook only the production backdrop paint lease entry");
    Need(MH_CreateHook(reinterpret_cast<void*>(&EndPaint), reinterpret_cast<void*>(&ObservedPaintEndPaint),
        reinterpret_cast<void**>(&realPaintEndPaint)) == MH_OK, "hook only the production backdrop paint lease exit");
    Need(MH_CreateHook(reinterpret_cast<void*>(&InvalidateRgn), reinterpret_cast<void*>(&ObservedPaintInvalidateRgn),
        reinterpret_cast<void**>(&realPaintInvalidateRgn)) == MH_OK, "hook only the production backdrop native replay invalidation");
    // Enable only the six detours created above; never MH_ALL_HOOKS, so no
    // existing production or fixture hook in this process is touched.
    Need(MH_EnableHook(reinterpret_cast<void*>(&CreateDIBSection)) == MH_OK, "enable only the production backdrop allocation hook");
    Need(MH_EnableHook(reinterpret_cast<void*>(&BitBlt)) == MH_OK, "enable only the production backdrop copy hook");
    Need(MH_EnableHook(reinterpret_cast<void*>(&FillRect)) == MH_OK, "enable only the production backdrop fill hook");
    Need(MH_EnableHook(reinterpret_cast<void*>(&BeginPaint)) == MH_OK, "enable only the production backdrop paint-entry hook");
    Need(MH_EnableHook(reinterpret_cast<void*>(&EndPaint)) == MH_OK, "enable only the production backdrop paint-exit hook");
    Need(MH_EnableHook(reinterpret_cast<void*>(&InvalidateRgn)) == MH_OK, "enable only the production backdrop replay-invalidation hook");
}
void RemovePaintHooks() {
    // Disable exactly the six detours enabled above, in reverse order, so no
    // unrelated hook installed elsewhere is disabled or removed.
    Need(MH_DisableHook(reinterpret_cast<void*>(&InvalidateRgn)) == MH_OK, "disable only the production backdrop replay-invalidation hook");
    Need(MH_DisableHook(reinterpret_cast<void*>(&EndPaint)) == MH_OK, "disable only the production backdrop paint-exit hook");
    Need(MH_DisableHook(reinterpret_cast<void*>(&BeginPaint)) == MH_OK, "disable only the production backdrop paint-entry hook");
    Need(MH_DisableHook(reinterpret_cast<void*>(&FillRect)) == MH_OK, "disable only the production backdrop fill hook");
    Need(MH_DisableHook(reinterpret_cast<void*>(&BitBlt)) == MH_OK, "disable only the production backdrop copy hook");
    Need(MH_DisableHook(reinterpret_cast<void*>(&CreateDIBSection)) == MH_OK, "disable only the production backdrop allocation hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&CreateDIBSection)) == MH_OK, "remove the production backdrop allocation hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&BitBlt)) == MH_OK, "remove the production backdrop copy hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&FillRect)) == MH_OK, "remove the production backdrop fill hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&BeginPaint)) == MH_OK, "remove the production backdrop paint-entry hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&EndPaint)) == MH_OK, "remove the production backdrop paint-exit hook");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&InvalidateRgn)) == MH_OK, "remove the production backdrop replay-invalidation hook");
    if (paintHooksInitialized) Need(MH_Uninitialize() == MH_OK, "uninitialize MinHook after the paint regression hooks are gone");
    paintHooksInitialized = false;
}
struct PaintControlSnapshot {
    HWND window = nullptr;
    int id = 0;
    RECT rect{};
    LONG_PTR style = 0, exstyle = 0;
    bool visible = false, enabled = false;
    std::wstring text;
    HFONT font = nullptr;
    LOGFONTW logfont{};
};
struct PaintSnapshot {
    RECT frame{};
    RECT client{};
    HWND focus = nullptr;
    LONG_PTR context = 0;
    std::wstring title;
    unsigned children = 0;
    PaintControlSnapshot entries[paintSnapshotChildren];
};
void CapturePaintSnapshot(HWND window, PaintSnapshot& snapshot) {
    snapshot = PaintSnapshot{};
    Need(GetWindowRect(window, &snapshot.frame) != FALSE, "read the real dialog frame for paint-regression snapshots");
    Need(GetClientRect(window, &snapshot.client) != FALSE, "read the real dialog client for paint-regression snapshots");
    snapshot.focus = GetFocus();
    snapshot.context = GetWindowLongPtrW(window, DWLP_USER);
    snapshot.title = Text(window);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        Need(snapshot.children < paintSnapshotChildren, "bounded immediate control snapshot for the paint regression");
        auto& entry = snapshot.entries[snapshot.children];
        RECT rect{};
        Need(GetWindowRect(child, &rect) != FALSE, "read an immediate control frame for the paint-regression snapshot");
        SetLastError(ERROR_SUCCESS);
        const int mapped = MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
        Need(mapped != 0 || GetLastError() == ERROR_SUCCESS, "map an immediate control into dialog client coordinates");
        entry.window = child;
        entry.id = GetDlgCtrlID(child);
        entry.rect = rect;
        entry.style = GetWindowLongPtrW(child, GWL_STYLE);
        entry.exstyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
        entry.visible = IsWindowVisible(child) != FALSE;
        entry.enabled = IsWindowEnabled(child) != FALSE;
        entry.text = Text(child);
        entry.font = reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
        if (entry.font) GetObjectW(entry.font, sizeof entry.logfont, &entry.logfont);
        ++snapshot.children;
    }
}
bool PaintSnapshotSame(const PaintSnapshot& before, const PaintSnapshot& after, const char*& why) {
    why = "the staged paint regression leaves the dialog layout unchanged";
    if (!EqualRect(&before.frame, &after.frame)) {
        why = "the staged paint regression leaves the exact dialog frame unchanged"; return false;
    }
    if (!EqualRect(&before.client, &after.client)) {
        why = "the staged paint regression leaves the exact dialog client unchanged"; return false;
    }
    if (before.title != after.title) {
        why = "the staged paint regression leaves the exact dialog title unchanged"; return false;
    }
    if (before.context != after.context) {
        why = "the staged paint regression leaves the production business context unchanged"; return false;
    }
    if (before.focus != after.focus) {
        why = "the staged paint regression leaves the dialog focus unchanged"; return false;
    }
    if (before.children != after.children) {
        why = "the staged paint regression leaves the exact immediate control set unchanged"; return false;
    }
    for (unsigned index = 0; index < before.children; ++index) {
        const auto& a = before.entries[index];
        const PaintControlSnapshot* b = nullptr;
        for (unsigned other = 0; other < after.children; ++other)
            if (after.entries[other].window == a.window) { b = &after.entries[other]; break; }
        if (!b || a.id != b->id) {
            why = "the staged paint regression leaves the exact immediate control set unchanged"; return false;
        }
        if (!EqualRect(&a.rect, &b->rect)) {
            why = "the staged paint regression leaves the exact immediate control rectangles unchanged"; return false;
        }
        if (a.style != b->style || a.exstyle != b->exstyle || a.visible != b->visible || a.enabled != b->enabled) {
            why = "the staged paint regression leaves the exact immediate control states unchanged"; return false;
        }
        if (a.text != b->text) {
            why = "the staged paint regression leaves the exact localized control texts unchanged"; return false;
        }
        if (a.font != b->font || a.logfont.lfHeight != b->logfont.lfHeight ||
            a.logfont.lfWeight != b->logfont.lfWeight || a.logfont.lfItalic != b->logfont.lfItalic ||
            a.logfont.lfCharSet != b->logfont.lfCharSet ||
            std::wstring(a.logfont.lfFaceName) != std::wstring(b->logfont.lfFaceName)) {
            why = "the staged paint regression leaves the exact control fonts unchanged"; return false;
        }
    }
    return true;
}
void PaintEvidenceRect(const char* name, const RECT& rect) {
    evidence << '\t' << name << '=' << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom;
}
// ---- end paint probe primitives ----

// ---- Real cache-dialog staged paint damage and verification ----
int PaintScale(HWND window) {
    const UINT dpi = getWindowDpi ? getWindowDpi(window) : 96u;
    return (std::max)(1, int(MulDiv(1, int(dpi), 96)));
}
bool PaintPointInRect(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}
bool PaintPointInsideClient(const PaintProbe& probe, POINT point) {
    return PaintPointInRect(probe.client, point);
}
bool PaintPointInChild(const PaintProbe& probe, POINT point) {
    for (unsigned i = 0; i < probe.childCount; ++i)
        if (PaintPointInRect(probe.children[i].client, point)) return true;
    return false;
}
bool PaintRectBackground(const PaintProbe& probe, const RECT& rect) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
    if (rect.left < probe.client.left || rect.top < probe.client.top ||
        rect.right > probe.client.right || rect.bottom > probe.client.bottom) return false;
    for (unsigned i = 0; i < probe.childCount; ++i) {
        RECT intersection{};
        if (IntersectRect(&intersection, &rect, &probe.children[i].client)) return false;
    }
    return true;
}
// True when the rectangle still has area after removing every immediate visible
// child; a region with the same bounding box but no area is not background.
bool PaintRectHasBackground(const PaintProbe& probe, const RECT& rect) {
    if (!probe.childrenRegion || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    HRGN region = CreateRectRgnIndirect(&rect);
    if (!region) return false;
    const int combined = CombineRgn(region, region, probe.childrenRegion, RGN_DIFF);
    RECT box{};
    const int type = GetRgnBox(region, &box);
    DeleteObject(region);
    return combined != ERROR && type != NULLREGION;
}
bool PaintFindParentBackgroundPoint(const PaintProbe& probe, const RECT& rect, POINT& point) {
    for (int y = rect.top; y < rect.bottom; ++y) for (int x = rect.left; x < rect.right; ++x) {
        const POINT candidate{x, y};
        if (!PaintPointInsideClient(probe, candidate)) continue;
        if (PaintPointInChild(probe, candidate)) continue;
        point = candidate;
        return true;
    }
    return false;
}
bool PaintFindGapPoint(const PaintProbe& probe, const RECT& box, const RECT& first, const RECT& second, POINT& point) {
    for (int y = box.top; y < box.bottom; ++y) for (int x = box.left; x < box.right; ++x) {
        const POINT candidate{x, y};
        if (!PaintPointInsideClient(probe, candidate)) continue;
        if (PaintPointInRect(first, candidate) || PaintPointInRect(second, candidate)) continue;
        if (PaintPointInChild(probe, candidate)) continue;
        point = candidate;
        return true;
    }
    return false;
}
bool PaintFindOutsidePoint(const PaintProbe& probe, const RECT& box, POINT& point) {
    const int scaled = (std::max)(1, PaintScale(probe.window));
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? scaled : 1;
        for (int y = probe.client.top; y < probe.client.bottom; y += step)
            for (int x = probe.client.left; x < probe.client.right; x += step) {
                const POINT candidate{x, y};
                if (PaintPointInRect(box, candidate)) continue;
                if (PaintPointInChild(probe, candidate)) continue;
                point = candidate;
                return true;
            }
    }
    return false;
}
int PaintAxisGap(const RECT& first, const RECT& second) {
    if (first.right <= second.left) return second.left - first.right;
    if (second.right <= first.left) return first.left - second.right;
    if (first.bottom <= second.top) return second.top - first.bottom;
    if (second.bottom <= first.top) return first.top - second.bottom;
    return 0;
}
void PaintPrepareDamage(PaintProbe& probe, const wchar_t* tag) {
    Need(probe.window && IsWindow(probe.window), "the staged paint damage targets the real dialog");
    Need(GetClientRect(probe.window, &probe.client) != FALSE, "read the live dialog client before staged paint damage");
    probe.expectedBox = RECT{};
    probe.childCount = 0;
    probe.backgroundCount = 0;
    probe.clip = PaintClip{};
    if (probe.expectedDamage) { DeleteObject(probe.expectedDamage); probe.expectedDamage = nullptr; }
    if (probe.childrenRegion) { DeleteObject(probe.childrenRegion); probe.childrenRegion = nullptr; }
    probe.childrenRegion = CreateRectRgn(0, 0, 0, 0);
    Need(probe.childrenRegion != nullptr, "allocate the immediate visible child region for staged paint damage");
    const RECT client = probe.client;
    for (HWND child = GetWindow(probe.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        Need(probe.childCount < paintMaxChildren, "bounded immediate visible child list for staged paint damage");
        RECT rect{};
        Need(GetWindowRect(child, &rect) != FALSE, "read a visible immediate child rectangle for staged paint damage");
        SetLastError(ERROR_SUCCESS);
        const int mapped = MapWindowPoints(HWND_DESKTOP, probe.window, reinterpret_cast<POINT*>(&rect), 2);
        Need(mapped != 0 || GetLastError() == ERROR_SUCCESS, "map a visible immediate child into dialog client coordinates");
        auto& entry = probe.children[probe.childCount];
        entry.window = child;
        entry.client = rect;
        HRGN region = CreateRectRgnIndirect(&rect);
        Need(region != nullptr, "allocate one immediate visible child region");
        Need(CombineRgn(probe.childrenRegion, probe.childrenRegion, region, RGN_OR) != ERROR, "union an immediate visible child into the clip region");
        DeleteObject(region);
        ++probe.childCount;
    }
    Need(probe.childCount > 0, "the real cache dialog exposes immediate visible controls");
    const int width = client.right - client.left, height = client.bottom - client.top;
    Need(width > 32 && height > 32, "the real dialog client is large enough for staged paint damage");
    const int scale = PaintScale(probe.window);
    // 1. A damage rectangle that overlaps one real child and its background margin.
    RECT childOverlap{};
    bool haveChildOverlap = false;
    const int margins[5] = {2 * scale, 3 * scale, 5 * scale, 8 * scale, 12 * scale};
    for (int marginIndex = 0; marginIndex < 5 && !haveChildOverlap; ++marginIndex) {
        const int margin = (std::max)(1, margins[marginIndex]);
        for (unsigned index = 0; index < probe.childCount && !haveChildOverlap; ++index) {
            const RECT child = probe.children[index].client;
            const RECT candidates[5] = {
                {child.left - margin, child.top - margin, child.right + margin, child.bottom + margin},
                {child.left - margin, child.top, child.right, child.bottom},
                {child.left, child.top, child.right + margin, child.bottom},
                {child.left, child.top - margin, child.right, child.bottom},
                {child.left, child.top, child.right, child.bottom + margin}};
            for (int candidate = 0; candidate < 5 && !haveChildOverlap; ++candidate) {
                RECT rect = candidates[candidate];
                rect.left = (std::max)(rect.left, client.left);
                rect.top = (std::max)(rect.top, client.top);
                rect.right = (std::min)(rect.right, client.right);
                rect.bottom = (std::min)(rect.bottom, client.bottom);
                if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
                const long long area = 1ll * (rect.right - rect.left) * (rect.bottom - rect.top);
                if (area * 2 > 1ll * width * height) continue;
                if (!PaintRectHasBackground(probe, rect)) continue;
                childOverlap = rect;
                haveChildOverlap = true;
            }
        }
    }
    Need(haveChildOverlap, "construct staged damage that overlaps a visible control and parent background");
    unsigned damagedChildren = 0;
    for (unsigned index = 0; index < probe.childCount; ++index) {
        RECT intersection{};
        if (IntersectRect(&intersection, &probe.children[index].client, &childOverlap)) ++damagedChildren;
    }
    Need(damagedChildren > 0, "the staged damage genuinely overlaps an immediate visible control");
    // 2. A disjoint parent-background rectangle separated by a real gap.
    RECT background{};
    bool haveBackground = false;
    POINT gapPoint{}, outsidePoint{};
    const int backgroundSizes[4] = {8 * scale, 6 * scale, 4 * scale, 3 * scale};
    const int requiredGaps[3] = {(std::max)(1, 3 * scale), (std::max)(1, 2 * scale), 1};
    const int centerY = childOverlap.top + (childOverlap.bottom - childOverlap.top) / 2;
    const int centerX = childOverlap.left + (childOverlap.right - childOverlap.left) / 2;
    for (int gapIndex = 0; gapIndex < 3 && !haveBackground; ++gapIndex) {
        const int gap = requiredGaps[gapIndex];
        for (int sizeIndex = 0; sizeIndex < 4 && !haveBackground; ++sizeIndex) {
            const int aw = (std::max)(3, backgroundSizes[sizeIndex]);
            const int ah = (std::max)(3, aw * 3 / 4);
            const int offsets[5] = {0, -(ah + gap) / 2, (ah + gap) / 2, -(ah + gap), ah + gap};
            for (int direction = 0; direction < 4 && !haveBackground; ++direction) {
                for (int offsetIndex = 0; offsetIndex < 5 && !haveBackground; ++offsetIndex) {
                    RECT rect{};
                    if (direction < 2) {
                        const int top = centerY + offsets[offsetIndex] - ah / 2;
                        rect.top = top;
                        rect.bottom = top + ah;
                        if (direction == 0) { rect.left = childOverlap.left - gap - aw; rect.right = childOverlap.left - gap; }
                        else { rect.left = childOverlap.right + gap; rect.right = childOverlap.right + gap + aw; }
                    } else {
                        const int left = centerX + offsets[offsetIndex] - aw / 2;
                        rect.left = left;
                        rect.right = left + aw;
                        if (direction == 2) { rect.top = childOverlap.top - gap - ah; rect.bottom = childOverlap.top - gap; }
                        else { rect.top = childOverlap.bottom + gap; rect.bottom = childOverlap.bottom + gap + ah; }
                    }
                    if (!PaintRectBackground(probe, rect)) continue;
                    if (PaintAxisGap(rect, childOverlap) < gap) continue;
                    RECT box{};
                    box.left = (std::min)(rect.left, childOverlap.left);
                    box.top = (std::min)(rect.top, childOverlap.top);
                    box.right = (std::max)(rect.right, childOverlap.right);
                    box.bottom = (std::max)(rect.bottom, childOverlap.bottom);
                    POINT foundGap{}, foundOutside{};
                    if (!PaintFindGapPoint(probe, box, rect, childOverlap, foundGap)) continue;
                    if (!PaintFindOutsidePoint(probe, box, foundOutside)) continue;
                    background = rect;
                    gapPoint = foundGap;
                    outsidePoint = foundOutside;
                    haveBackground = true;
                }
            }
        }
    }
    if (!haveBackground) {
        // Bounded fallback: any background rectangle separated by at least one
        // pixel from the child-overlap damage.
        const int step = (std::max)(1, scale);
        const int aw = (std::max)(3, 4 * scale), ah = (std::max)(3, 3 * scale);
        for (int y = client.top; y + ah <= client.bottom && !haveBackground; y += step)
            for (int x = client.left; x + aw <= client.right && !haveBackground; x += step) {
                const RECT rect{x, y, x + aw, y + ah};
                if (!PaintRectBackground(probe, rect)) continue;
                if (PaintAxisGap(rect, childOverlap) < 1) continue;
                RECT box{};
                box.left = (std::min)(rect.left, childOverlap.left);
                box.top = (std::min)(rect.top, childOverlap.top);
                box.right = (std::max)(rect.right, childOverlap.right);
                box.bottom = (std::max)(rect.bottom, childOverlap.bottom);
                POINT foundGap{}, foundOutside{};
                if (!PaintFindGapPoint(probe, box, rect, childOverlap, foundGap)) continue;
                if (!PaintFindOutsidePoint(probe, box, foundOutside)) continue;
                background = rect;
                gapPoint = foundGap;
                outsidePoint = foundOutside;
                haveBackground = true;
            }
    }
    Need(haveBackground, "construct a separated parent-background staged damage rectangle");
    // 3. Exact expected region, clip sample points and evidence.
    probe.expectedBox.left = (std::min)(background.left, childOverlap.left);
    probe.expectedBox.top = (std::min)(background.top, childOverlap.top);
    probe.expectedBox.right = (std::max)(background.right, childOverlap.right);
    probe.expectedBox.bottom = (std::max)(background.bottom, childOverlap.bottom);
    HRGN first = CreateRectRgnIndirect(&background);
    HRGN second = CreateRectRgnIndirect(&childOverlap);
    Need(first != nullptr && second != nullptr, "allocate the two staged damage rectangles");
    probe.expectedDamage = CreateRectRgn(0, 0, 0, 0);
    Need(probe.expectedDamage != nullptr, "allocate the staged damage region");
    Need(CombineRgn(probe.expectedDamage, first, second, RGN_OR) == COMPLEXREGION, "the staged damage is a real two-rectangle complex region");
    DeleteObject(first);
    DeleteObject(second);
    RECT regionBox{};
    Need(GetRgnBox(probe.expectedDamage, &regionBox) != ERROR && EqualRect(&regionBox, &probe.expectedBox), "the staged damage region has the expected bounding box");
    probe.background[0].x = background.left + (background.right - background.left) / 2;
    probe.background[0].y = background.top + (background.bottom - background.top) / 2;
    probe.backgroundCount = 1;
    POINT backgroundInChild{};
    Need(PaintFindParentBackgroundPoint(probe, childOverlap, backgroundInChild), "find parent background inside the control-overlap staged damage");
    probe.background[probe.backgroundCount++] = backgroundInChild;
    probe.gap = gapPoint;
    probe.outside = outsidePoint;
    Check(probe.backgroundCount == 2, "staged damage records parent background and child-overlap background points");
    Check(PtInRect(&probe.expectedBox, probe.gap) && PtInRegion(probe.expectedDamage, probe.gap.x, probe.gap.y) == FALSE, "the staged gap point is inside the damage box and outside both damage rectangles");
    Check(PaintPointInsideClient(probe, probe.outside) && !PtInRect(&probe.expectedBox, probe.outside), "the staged outside point lies beyond the damage box inside the dialog client");
    Check(PaintAxisGap(background, childOverlap) > 0, "the two staged damage rectangles are disjoint with a real gap");
    evidence << "paint-damage\tlanguage=" << language << "\tname=" << Utf8(tag) << "\thwnd=" << probe.window
        << "\tclient=" << width << 'x' << height << "\tchildren=" << probe.childCount
        << "\tdamaged-children=" << damagedChildren
        << "\tbackground-points=" << probe.backgroundCount;
    PaintEvidenceRect("background", background);
    PaintEvidenceRect("child-overlap", childOverlap);
    PaintEvidenceRect("box", probe.expectedBox);
    evidence << "\tgap=" << probe.gap.x << ',' << probe.gap.y << "\toutside=" << probe.outside.x << ',' << probe.outside.y << '\n';
    evidence.flush();
}
bool PaintClearPending() {
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return false;
    bool cleared = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ValidateRect(dialog, nullptr);
        const int type = GetUpdateRgn(dialog, region, FALSE);
        if (type == NULLREGION) { cleared = true; break; }
        if (type == ERROR) break;
    }
    DeleteObject(region);
    return cleared;
}
void PaintSetGeometry(const RECT& frame, const RECT& expectedClient, const wchar_t* tag) {
    Need(SetWindowPos(dialog, nullptr, int(frame.left), int(frame.top), int(frame.right - frame.left), int(frame.bottom - frame.top),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW) != FALSE,
        "apply a real dialog geometry change for the staged paint regression");
    RECT actualFrame{}, actualClient{};
    Need(GetWindowRect(dialog, &actualFrame) != FALSE, "read the real staged dialog frame");
    Need(GetClientRect(dialog, &actualClient) != FALSE, "read the real staged dialog client");
    Check(EqualRect(&actualFrame, &frame), "the real dialog accepts the staged geometry change unchanged");
    Check(EqualRect(&actualClient, &expectedClient), "the real dialog client matches the staged geometry change");
    evidence << "paint-geometry\tlanguage=" << language << "\tname=" << Utf8(tag);
    PaintEvidenceRect("frame", actualFrame);
    PaintEvidenceRect("client", actualClient);
    evidence << '\n';
    evidence.flush();
    Need(PaintClearPending(), "clear pending dialog paint without painting after the staged geometry change");
}
void PaintResetStageCounters(PaintProbe& probe) {
    probe.paintDc = nullptr;
    probe.paintDcLive = false;
    probe.beginPaintFailed = false;
    probe.beginPaintCalls = 0;
    probe.firstPaintSeen = false;
    probe.leaseActive = false;
    probe.firstEndPaintSeen = false;
    probe.leaseClosed = false;
    probe.endPaintHdcMatched = false;
    probe.laterBeginPaintSeen = false;
    probe.endPaintCalls = 0;
    for (unsigned pass = 0; pass < paintMaxPasses; ++pass) { probe.paintDcs[pass] = nullptr; probe.paintRects[pass] = RECT{}; }
    probe.mapMode = 0;
    probe.origin = POINT{};
    probe.paintMessages = probe.eraseMessages = probe.eraseAfterReplay = probe.eraseNonzeroAfterReplay = 0;
    probe.allocationAttempts = probe.allocationSuccesses = probe.allocationDenials = 0;
    probe.allocated = nullptr;
    probe.copyAttempts = probe.copySuccesses = probe.copyDenials = 0;
    probe.copyRecorded = false;
    probe.copyRect = RECT{};
    probe.copySourceX = probe.copySourceY = 0;
    probe.copyRop = 0;
    probe.copySource = nullptr;
    probe.copySourceWidth = probe.copySourceHeight = 0;
    probe.fillAttempts = probe.fillSuccesses = probe.fillDenials = 0;
    probe.fillRecorded = false;
    probe.fillRect = RECT{};
    probe.fillBrush = nullptr;
    probe.deniedFillDamage = false;
    probe.invalidateCalls = probe.eraseInvalidates = probe.replays = 0;
    probe.replayCallbackCount = probe.replayLiveErase = probe.replaySuppliedErase = 0;
    probe.replaySuppliedRecorded = false;
    probe.replaySuppliedObjectType = 0;
    probe.replaySuppliedWindowFromDc = nullptr;
    probe.replaySuppliedClipBoxType = 0;
    probe.replaySuppliedClipBox = RECT{};
    probe.replaySuppliedMapMode = 0;
    probe.replaySuppliedOrigin = POINT{};
    probe.replayClipRecorded = false;
    probe.replayObjectType = 0;
    probe.replayWindowFromDc = nullptr;
    probe.replayClipBoxType = 0;
    probe.replayClipBox = RECT{};
    probe.replayMapMode = 0;
    probe.replayOrigin = POINT{};
    probe.replayChildRects = probe.replayClippedChildren = 0;
    for (unsigned i = 0; i < paintMaxChildren; ++i) probe.replayChildVisible[i] = -1;
    probe.replayBackgroundVisible = 0;
    for (unsigned i = 0; i < paintMaxBackground; ++i) probe.replayBackgroundPtVisible[i] = -1;
    probe.replayGapVisible = probe.replayOutsideVisible = -1;
    probe.replaySentinelAttempted = probe.replaySentinelWritten = false;
    probe.replayPixelProofDone = false;
    for (unsigned i = 0; i < paintMaxBackground; ++i) {
        probe.replaySetPixelOk[i] = probe.replayGetPixelBeforeOk[i] = false;
        probe.replayGetPixelAfterOk[i] = probe.replayAfterFace[i] = false;
        probe.replayBeforeColor[i] = probe.replayAfterColor[i] = 0;
    }
    probe.replaySentinelColor = 0;
    probe.replayHdcValidAfter = probe.replayAfterReadValid = false;
    probe.replayInjectionInactive = false;
    probe.replayCallbackResult = 0;
    probe.innerCallbacks = probe.innerLiveDc = probe.innerSuppliedDc = 0;
    probe.innerRecorded = false;
    probe.innerObjectType = 0;
    probe.innerWindowFromDc = nullptr;
    probe.innerClipBoxType = 0;
    probe.innerClipBox = RECT{};
    probe.innerMapMode = 0;
    probe.innerOrigin = POINT{};
    probe.innerChildRects = probe.innerClippedChildren = 0;
    for (unsigned i = 0; i < paintMaxChildren; ++i) probe.innerChildVisible[i] = -1;
    probe.innerBackgroundVisible = 0;
    for (unsigned i = 0; i < paintMaxBackground; ++i) probe.innerBackgroundPtVisible[i] = -1;
    probe.innerGapVisible = probe.innerOutsideVisible = -1;
    probe.innerSuppliedRecorded = false;
    probe.innerSuppliedObjectType = 0;
    probe.innerSuppliedWindowFromDc = nullptr;
    probe.innerSuppliedClipBoxType = 0;
    probe.innerSuppliedClipBox = RECT{};
    probe.innerSuppliedMapMode = 0;
    probe.innerSuppliedOrigin = POINT{};
    probe.clip = PaintClip{};
}
void RunPaintStage(PaintProbe& probe, const wchar_t* name, unsigned index,
    unsigned allocationBudget, unsigned copyBudget, unsigned fillBudget) {
    PaintSnapshot snapshotBefore;
    CapturePaintSnapshot(probe.window, snapshotBefore);
    Need(PaintClearPending(), "clear pending dialog damage before the staged paint");
    Check(InvalidateRgn(probe.window, probe.expectedDamage, FALSE) != FALSE, "invalidate the real dialog with the constructed staged damage");
    HRGN actual = CreateRectRgn(0, 0, 0, 0);
    Need(actual != nullptr, "allocate the staged update-region reader");
    const int updateType = GetUpdateRgn(probe.window, actual, FALSE);
    const bool damageEqual = updateType == COMPLEXREGION && EqualRgn(actual, probe.expectedDamage) != FALSE;
    DeleteObject(actual);
    evidence << "paint-update\tlanguage=" << language << "\tindex=" << index << "\tname=" << Utf8(name)
        << "\ttype=" << updateType << "\tequal=" << damageEqual << '\n';
    evidence.flush();
    Check(damageEqual, "the real dialog update region exactly equals the constructed two-rectangle damage");
    PaintResetStageCounters(probe);
    probe.allocationDenialBudget = allocationBudget;
    probe.copyDenialBudget = copyBudget;
    probe.fillDenialBudget = fillBudget;
    probe.active = true;
    activePaintProbe = &probe;
    const BOOL updated = UpdateWindow(probe.window);
    probe.active = false;
    activePaintProbe = nullptr;
    const BOOL flushed = GdiFlush();
    HRGN after = CreateRectRgn(0, 0, 0, 0);
    Need(after != nullptr, "allocate the post-paint update-region reader");
    const int afterType = GetUpdateRgn(probe.window, after, FALSE);
    DeleteObject(after);
    PaintSnapshot snapshotAfter;
    CapturePaintSnapshot(probe.window, snapshotAfter);
    const char* mismatch = nullptr;
    const bool snapshotSame = PaintSnapshotSame(snapshotBefore, snapshotAfter, mismatch);
    evidence << "paint-stage\tlanguage=" << language << "\tindex=" << index << "\tname=" << Utf8(name)
        << "\thwnd=" << probe.window << "\ttid=" << probe.thread
        << "\tclient=" << probe.client.right - probe.client.left << 'x' << probe.client.bottom - probe.client.top;
    PaintEvidenceRect("box", probe.expectedBox);
    PaintEvidenceRect("rc-paint", probe.paintRects[0]);
    PaintEvidenceRect("replay-rc-paint", probe.paintRects[1]);
    PaintEvidenceRect("copy-rect", probe.copyRect);
    PaintEvidenceRect("fill-rect", probe.fillRect);
    evidence << "\tbeginpaint=" << probe.beginPaintCalls << "\tlater-beginpaint=" << probe.laterBeginPaintSeen
        << "\tlease-closed=" << probe.leaseClosed << "\tendpaint-match=" << probe.endPaintHdcMatched
        << "\talloc=" << probe.allocationAttempts << '/' << probe.allocationSuccesses << '/' << probe.allocationDenials
        << "\talloc-bitmap=" << probe.allocated
        << "\tcopy=" << probe.copyAttempts << '/' << probe.copySuccesses << '/' << probe.copyDenials
        << "\tcopy-source=" << probe.copySource << "\tcopy-src=" << probe.copySourceX << ',' << probe.copySourceY
        << "\tcopy-rop=" << probe.copyRop << "\tcopy-size=" << probe.copySourceWidth << 'x' << probe.copySourceHeight
        << "\tfill=" << probe.fillAttempts << '/' << probe.fillSuccesses << '/' << probe.fillDenials
        << "\tdenied-fill-damage=" << probe.deniedFillDamage
        << "\tinvalidates=" << probe.invalidateCalls << "/erase=" << probe.eraseInvalidates << "/replays=" << probe.replays
        << "\terase-msgs=" << probe.eraseMessages << "/after-replay=" << probe.eraseAfterReplay
        << "/nonzero=" << probe.eraseNonzeroAfterReplay
        << "\tchildren-clipped=" << probe.clip.clippedChildren << '/' << probe.clip.childRects
        << "\tbackground-visible=" << probe.clip.backgroundVisible << '/' << probe.clip.backgroundPoints
        << "\tgap-visible=" << probe.clip.gapVisible << "\toutside-visible=" << probe.clip.outsideVisible
        << "\tdraw-dc-live=" << probe.clip.drawDcLive
        << "\tbuffer-identity=" << probe.bufferBitmap
        << "\tupdate-after=" << afterType << "\tsnapshot-same=" << snapshotSame << '\n';
    if (index == 5) {
        evidence << "paint-native-replay\tlanguage=" << language << "\thwnd=" << probe.window
            << "\tcallbacks=" << probe.replayCallbackCount
            << "\tlive-dc=" << probe.replayLiveErase << "\tsupplied-dc=" << probe.replaySuppliedErase
            << "\tobject-type=" << probe.replayObjectType
            << "\twindow-from-dc=" << probe.replayWindowFromDc
            << "\tclipbox-type=" << probe.replayClipBoxType
            << "\tclipbox=" << probe.replayClipBox.left << ',' << probe.replayClipBox.top << ','
            << probe.replayClipBox.right << ',' << probe.replayClipBox.bottom
            << "\tmap-mode=" << probe.replayMapMode
            << "\torigin=" << probe.replayOrigin.x << ',' << probe.replayOrigin.y
            << "\tsupplied-object=" << probe.replaySuppliedObjectType
            << "\tsupplied-window-from-dc=" << probe.replaySuppliedWindowFromDc
            << "\tsupplied-clipbox-type=" << probe.replaySuppliedClipBoxType
            << "\tsupplied-map-mode=" << probe.replaySuppliedMapMode
            << "\tsupplied-origin=" << probe.replaySuppliedOrigin.x << ',' << probe.replaySuppliedOrigin.y
            << "\tbackground-visible=" << probe.replayBackgroundVisible << '/' << probe.backgroundCount
            << "\tgap-visible=" << probe.replayGapVisible << "\toutside-visible=" << probe.replayOutsideVisible
            << "\tchildren-clipped=" << probe.replayClippedChildren << '/' << probe.replayChildRects
            << "\tsentinel=" << unsigned(probe.replaySentinelColor)
            << "\tbefore0=" << unsigned(probe.replayBeforeColor[0])
            << "\tbefore1=" << unsigned(probe.replayBeforeColor[1])
            << "\tafter0=" << unsigned(probe.replayAfterColor[0])
            << "\tafter1=" << unsigned(probe.replayAfterColor[1])
            << "\tface=" << unsigned(GetSysColor(COLOR_3DFACE))
            << "\tsetpixel=" << probe.replaySetPixelOk[0] << '/' << probe.replaySetPixelOk[1]
            << "\tgetpixel-before=" << probe.replayGetPixelBeforeOk[0] << '/' << probe.replayGetPixelBeforeOk[1]
            << "\tgetpixel-after=" << probe.replayGetPixelAfterOk[0] << '/' << probe.replayGetPixelAfterOk[1]
            << "\tafter-face=" << probe.replayAfterFace[0] << '/' << probe.replayAfterFace[1]
            << "\tread-valid=" << probe.replayAfterReadValid
            << "\thdc-valid-after=" << probe.replayHdcValidAfter
            << "\tpixel-proof-done=" << probe.replayPixelProofDone
            << "\tinjection-inactive=" << probe.replayInjectionInactive
            << "\tresult=" << probe.replayCallbackResult << '\n';
        evidence << "paint-native-replay-inner\tlanguage=" << language << "\thwnd=" << probe.window
            << "\tcallbacks=" << probe.innerCallbacks
            << "\tlive-dc=" << probe.innerLiveDc << "\tsupplied-dc=" << probe.innerSuppliedDc
            << "\tobject-type=" << probe.innerObjectType
            << "\twindow-from-dc=" << probe.innerWindowFromDc
            << "\tclipbox-type=" << probe.innerClipBoxType
            << "\tclipbox=" << probe.innerClipBox.left << ',' << probe.innerClipBox.top << ','
            << probe.innerClipBox.right << ',' << probe.innerClipBox.bottom
            << "\tmap-mode=" << probe.innerMapMode
            << "\torigin=" << probe.innerOrigin.x << ',' << probe.innerOrigin.y
            << "\tsupplied-object=" << probe.innerSuppliedObjectType
            << "\tsupplied-window-from-dc=" << probe.innerSuppliedWindowFromDc
            << "\tsupplied-clipbox-type=" << probe.innerSuppliedClipBoxType
            << "\tsupplied-map-mode=" << probe.innerSuppliedMapMode
            << "\tsupplied-origin=" << probe.innerSuppliedOrigin.x << ',' << probe.innerSuppliedOrigin.y
            << "\tbackground-visible=" << probe.innerBackgroundVisible << '/' << probe.backgroundCount
            << "\tgap-visible=" << probe.innerGapVisible << "\toutside-visible=" << probe.innerOutsideVisible
            << "\tchildren-clipped=" << probe.innerClippedChildren << '/' << probe.innerChildRects
            << '\n';
    }
    evidence.flush();
    Check(updated != FALSE, "UpdateWindow delivers the real staged WM_PAINT");
    Check(flushed != FALSE, "flush staged GDI output before inspecting it");
    Check(afterType == NULLREGION, "the staged paint validates the real update region without leftovers");
    if (snapshotSame) Check(true, "the staged paint leaves dialog frame, controls, fonts and texts unchanged");
    else Check(false, mismatch);
    Check(probe.beginPaintCalls >= 1 && !probe.beginPaintFailed && probe.paintDcLive, "each staged paint observes a real BeginPaint live dialog DC");
    Check(probe.firstPaintSeen && probe.firstEndPaintSeen && probe.leaseClosed && probe.endPaintHdcMatched && !probe.leaseActive, "the staged production paint lease closes at its matching EndPaint before forwarding");
    Check(probe.paintMessages == 1, "each staged paint receives exactly one real target WM_PAINT message");
    Check(probe.clip.recorded && probe.clip.childRects == probe.childCount && probe.clip.clippedChildren == probe.childCount && probe.clip.drawDcLive, "the production paint DC clips every immediate visible child rectangle");
    Check(probe.clip.backgroundVisible >= 1, "the production paint DC keeps damaged parent background visible");
    Check(probe.clip.gapVisible == FALSE && probe.clip.outsideVisible == FALSE, "the production paint DC clips the non-damaged gap and an outside point");
    Check(probe.mapMode == MM_TEXT && probe.origin.x == 0 && probe.origin.y == 0, "BeginPaint keeps MM_TEXT and the default origin for logical client checks");
    Check(EqualRect(&probe.paintRects[0], &probe.expectedBox), "BeginPaint reports the constructed damage box as the real rcPaint");
    if (probe.copyRecorded) {
        Check(EqualRect(&probe.copyRect, &probe.expectedBox), "the production buffered copy targets exactly the constructed damage box");
        Check(probe.copySourceX == probe.copyRect.left && probe.copySourceY == probe.copyRect.top && probe.copyRop == SRCCOPY, "the production buffered copy uses the same source origin and SRCCOPY");
        Check(probe.copySourceWidth == probe.client.right - probe.client.left && probe.copySourceHeight == probe.client.bottom - probe.client.top, "the production buffered source bitmap matches the current dialog client size");
    }
    if (probe.fillRecorded) {
        Check(EqualRect(&probe.fillRect, &probe.expectedBox), "the production direct FillRect targets exactly the constructed damage box");
        Check(probe.fillBrush != nullptr, "the production fallback FillRect passes a real brush");
    }
    switch (index) {
    case 0:
        Check(probe.allocationAttempts == 1 && probe.allocationSuccesses == 1 && probe.allocationDenials == 0, "cold staged paint reallocates the production buffer exactly once");
        Check(probe.allocated != nullptr && probe.copyRecorded && probe.copySource == probe.allocated, "cold staged buffered copy uses the newly allocated production bitmap");
        Check(probe.copyAttempts == 1 && probe.copySuccesses == 1 && probe.copyDenials == 0 && probe.fillAttempts == 0, "cold staged paint performs exactly one buffered copy and no direct fill");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "cold staged paint performs one BeginPaint pass and no native replay");
        paintColdSource = probe.copySource;
        break;
    case 1:
        Check(probe.allocationAttempts == 0 && probe.allocationSuccesses == 0 && probe.allocationDenials == 0, "warm staged paint reuses the cached production buffer");
        Check(probe.copyAttempts == 1 && probe.copySuccesses == 1 && probe.copyDenials == 0 && probe.fillAttempts == 0, "warm staged paint performs exactly one buffered copy and no direct fill");
        Check(probe.copyRecorded && probe.copySource != nullptr && probe.copySource == paintColdSource, "warm staged buffered copy uses the same source HBITMAP as the cold stage at the same size");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "warm staged paint performs one BeginPaint pass and no native replay");
        break;
    case 2:
        Check(probe.allocationAttempts == 1 && probe.allocationDenials == 1 && probe.allocationSuccesses == 0 && probe.allocated == nullptr, "allocation denial fails exactly one matched production CreateDIBSection");
        Check(probe.copyAttempts == 0 && probe.copySuccesses == 0, "allocation denial exposes no buffered copy");
        Check(probe.fillAttempts == 1 && probe.fillSuccesses == 1 && probe.fillDenials == 0 && probe.fillRecorded && EqualRect(&probe.fillRect, &probe.expectedBox), "allocation denial is covered by exactly one real direct FillRect for the constructed damage");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "allocation denial performs one BeginPaint pass and no native replay");
        break;
    case 3:
        Check(probe.allocationAttempts == 1 && probe.allocationSuccesses == 1 && probe.allocationDenials == 0, "recovery reallocates the production buffer exactly once after the denied allocation");
        Check(probe.copyAttempts == 1 && probe.copySuccesses == 1 && probe.copyDenials == 0 && probe.fillAttempts == 0, "recovery performs exactly one buffered copy and no direct fill");
        Check(probe.copyRecorded && probe.copySource != nullptr && probe.copySource == probe.allocated, "recovery copies from the actual newly allocated production bitmap");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "recovery performs one BeginPaint pass and no native replay");
        paintRecoveredSource = probe.copySource;
        break;
    case 4:
        Check(probe.allocationAttempts == 0 && probe.allocationDenials == 0, "copy denial does not reallocate the production buffer");
        Check(probe.copyAttempts == 1 && probe.copyDenials == 1 && probe.copySuccesses == 0 && probe.copyRecorded && EqualRect(&probe.copyRect, &probe.expectedBox), "copy denial fails exactly one matched production BitBlt for the constructed damage");
        Check(probe.copySource == paintRecoveredSource, "copy denial records the recovered production source HBITMAP");
        Check(probe.fillAttempts == 1 && probe.fillSuccesses == 1 && probe.fillDenials == 0 && probe.fillRecorded && EqualRect(&probe.fillRect, &probe.expectedBox), "copy denial is covered by exactly one real direct FillRect");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "copy denial performs one BeginPaint pass and no native replay");
        break;
    case 5:
        Check(probe.allocationAttempts == 0 && probe.allocationDenials == 0, "native replay does not reallocate the production buffer");
        Check(probe.copyAttempts == 1 && probe.copyDenials == 1 && probe.copySuccesses == 0 && probe.copyRecorded && EqualRect(&probe.copyRect, &probe.expectedBox) && probe.copySource == paintRecoveredSource, "native replay denies exactly the first matched production buffered copy");
        Check(probe.fillDenials == 1 && probe.deniedFillDamage && probe.fillRecorded && EqualRect(&probe.fillRect, &probe.expectedBox), "native replay denies exactly the first matched direct FillRect for the same damage");
        Check(probe.replays == 1 && probe.eraseInvalidates == 1 && probe.invalidateCalls == 1, "native replay restores exactly one erase-enabled invalidation equal to the original damage");
        Check(probe.eraseAfterReplay >= 1 && probe.eraseMessages >= 1, "native replay receives a real WM_ERASEBKGND after the replay invalidation");
        // The OS is not required to route the base dialog procedure's replay
        // through the exported BeginPaint/EndPaint thunk. The observed call count
        // and any second rcPaint stay in evidence but are not asserted. Native
        // painting is proven on the real erase callback instead: the live dialog
        // window DC must see the damaged background points and must replace the
        // sentinel pixels with the real default background color.
        Check(probe.replayLiveErase >= 1, "native replay erase provides the live dialog window DC, not only a supplied memory DC");
        // The outer subclass entry state stays in evidence (it can show 16/17
        // because it is sampled before/differently from the handed-down clip).
        // Acceptance uses the independent inner dialog-procedure observation.
        Check(probe.innerLiveDc >= 1, "native replay inner dialog procedure observes the live dialog window DC, not only a supplied memory DC");
        Check(probe.innerBackgroundVisible == paintMaxBackground, "native replay inner dialog procedure keeps both damaged parent background points drawable");
        Check(probe.innerGapVisible == FALSE && probe.innerOutsideVisible == FALSE, "native replay inner dialog procedure sees the non-damaged gap and an outside point clipped");
        Check(probe.innerChildRects == probe.childCount && probe.innerClippedChildren == probe.childCount, "native replay inner dialog procedure sees every visible immediate child clipped from the handed-down DC");
        Check(probe.replaySentinelAttempted && probe.replaySentinelWritten, "native replay erase sentinels were written and read back before forwarding");
        Check(probe.replayGetPixelAfterOk[0] && probe.replayGetPixelAfterOk[1] && probe.replayAfterReadValid && probe.replayHdcValidAfter && probe.replayPixelProofDone, "native replay erase readback returned valid pixels on the still-live dialog DC");
        Check(probe.replayAfterFace[0] && probe.replayAfterFace[1], "native replay default erase replaced both sentinels with the real background color");
        Check(probe.replayInjectionInactive, "native replay erase runs with the one-shot fault injections consumed");
        break;
    default:
        Check(probe.allocationAttempts == 0 && probe.allocationDenials == 0, "final recovery reuses the cached production buffer");
        Check(probe.copyAttempts == 1 && probe.copySuccesses == 1 && probe.copyDenials == 0 && probe.fillAttempts == 0, "final recovery restores exactly one successful buffered copy and no direct fill");
        Check(probe.copyRecorded && probe.copySource == paintRecoveredSource, "final recovery copies from the same recovered production HBITMAP");
        Check(probe.replays == 0 && probe.beginPaintCalls == 1, "final recovery performs one BeginPaint pass and no native replay");
        break;
    }
}
void PaintRegression() {
    Need(dialog && IsWindow(dialog), "the real cache management dialog exists before the staged paint regression");
    Need(GetWindowThreadProcessId(dialog, nullptr) == GetCurrentThreadId(), "the staged paint regression runs on the real dialog UI thread");
    Check(IsWindowVisible(dialog) != FALSE, "the staged paint regression targets a visible real dialog");
    const auto inspect = cache::Inspect();
    Check(inspect.error.empty() && inspect.projects.size() == 2, "asynchronous cache inspection has settled before the staged paint regression");
    Check(ListView_GetItemCount(GetDlgItem(dialog, ui::Projects)) == 2, "the settled project list is present before the staged paint regression");
    std::vector<std::wstring> projectPaths;
    for (const auto& project : inspect.projects) projectPaths.push_back(project.path);
    PaintSnapshot baseline;
    CapturePaintSnapshot(dialog, baseline);
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    evidence << "paint-regression\tphase=begin\tlanguage=" << language << "\thwnd=" << dialog
        << "\ttid=" << GetCurrentThreadId() << "\tprojects=" << inspect.projects.size()
        << "\tgdi=" << gdiBefore << "\tuser=" << userBefore << '\n';
    evidence.flush();
    Capture(L"paint-before");
    Need(SetWindowSubclass(dialog, PaintObserve, paintObserverId, 0) != FALSE, "observe only the real cache dialog paint messages");
    InstallPaintHooks();
    PaintProbe probe;
    probe.thread = GetCurrentThreadId();
    probe.window = dialog;
    paintColdSource = nullptr;
    paintRecoveredSource = nullptr;
    // Wrap only the real dialog procedure for the seven staged paints so the
    // handed-down erase clip is observed at the exact inner entry point. The
    // wrapper is removed before any later business operation runs.
    const LONG_PTR originalDialogProc = GetWindowLongPtrW(dialog, DWLP_DLGPROC);
    Need(originalDialogProc != 0, "read the real dialog procedure before the staged paint regression");
    paintOriginalDialogProc = reinterpret_cast<DLGPROC>(originalDialogProc);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previousDialogProc = SetWindowLongPtrW(dialog, DWLP_DLGPROC,
        reinterpret_cast<LONG_PTR>(&PaintObserveInnerDialog));
    const DWORD installDialogProcError = GetLastError();
    if (previousDialogProc == originalDialogProc && installDialogProcError == ERROR_SUCCESS)
        Check(true, "install the read-only inner dialog procedure observation on the exact real dialog");
    else
        Check(false, "install the read-only inner dialog procedure observation on the exact real dialog");
    evidence << "paint-inner-wrapper\tlanguage=" << language << "\thwnd=" << dialog
        << "\toriginal=" << originalDialogProc
        << "\tinstalled=" << GetWindowLongPtrW(dialog, DWLP_DLGPROC) << '\n';
    Need(GetWindowLongPtrW(dialog, DWLP_DLGPROC) == reinterpret_cast<LONG_PTR>(&PaintObserveInnerDialog),
        "the inner dialog procedure observation wrapper is active before the staged paints");
    const RECT originalFrame = baseline.frame;
    const RECT originalClient = baseline.client;
    const UINT dpi = getWindowDpi(dialog);
    const int grow = (std::max)(24, int(MulDiv(16, int(dpi), 96)));
    RECT frame1 = originalFrame, client1 = originalClient;
    frame1.right += grow; frame1.bottom += grow;
    client1.right += grow; client1.bottom += grow;
    RECT frame2 = originalFrame, client2 = originalClient;
    frame2.right += 2 * grow; frame2.bottom += 2 * grow;
    client2.right += 2 * grow; client2.bottom += 2 * grow;
    evidence << "paint-plan\tlanguage=" << language << "\tgrow=" << grow
        << "\toriginal-client=" << originalClient.right << 'x' << originalClient.bottom << '\n';
    // Stage 0 cold: the initial dialog buffer already exists, so a real size
    // change forces the production PanelBuffer to reallocate.
    PaintSetGeometry(frame1, client1, L"cold");
    PaintPrepareDamage(probe, L"cold");
    RunPaintStage(probe, L"cold", 0, 0, 0, 0);
    // Stage 1 warm: same size and key, so the production source HBITMAP must be
    // reused without any allocation.
    RunPaintStage(probe, L"warm", 1, 0, 0, 0);
    // Stage 2 allocation denial after a second real size change.
    PaintSetGeometry(frame2, client2, L"allocation-denial");
    PaintPrepareDamage(probe, L"allocation-denial");
    RunPaintStage(probe, L"allocation-denial", 2, 1, 0, 0);
    // Restore the exact original geometry; the recovery paint must then allocate
    // the production buffer for the restored client size and copy successfully.
    PaintSetGeometry(originalFrame, originalClient, L"recovery");
    PaintSnapshot restored;
    CapturePaintSnapshot(dialog, restored);
    const char* restoreMismatch = nullptr;
    if (PaintSnapshotSame(baseline, restored, restoreMismatch)) Check(true, "the staged size changes restore the exact dialog geometry, controls, fonts and texts");
    else Check(false, restoreMismatch);
    PaintPrepareDamage(probe, L"recovery");
    RunPaintStage(probe, L"recovery", 3, 0, 0, 0);
    RunPaintStage(probe, L"copy-denial", 4, 0, 1, 0);
    RunPaintStage(probe, L"native-replay", 5, 0, 1, 1);
    RunPaintStage(probe, L"final-recovery", 6, 0, 0, 0);
    // Restore the exact original dialog procedure before any later business work.
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR restoredPrevious = SetWindowLongPtrW(dialog, DWLP_DLGPROC, originalDialogProc);
    const DWORD restoreDialogProcError = GetLastError();
    const LONG_PTR restoredDialogProc = GetWindowLongPtrW(dialog, DWLP_DLGPROC);
    if (restoredPrevious == reinterpret_cast<LONG_PTR>(&PaintObserveInnerDialog) &&
        restoreDialogProcError == ERROR_SUCCESS && restoredDialogProc == originalDialogProc)
        Check(true, "restore the exact original dialog procedure after the staged paint regression");
    else
        Check(false, "restore the exact original dialog procedure after the staged paint regression");
    Need(restoredDialogProc == originalDialogProc &&
        restoredPrevious == reinterpret_cast<LONG_PTR>(&PaintObserveInnerDialog),
        "restore the exact original dialog procedure after the staged paint regression");
    evidence << "paint-inner-wrapper\tlanguage=" << language << "\thwnd=" << dialog
        << "\trestored=" << restoredDialogProc << "\toriginal=" << originalDialogProc
        << "\tprevious=" << restoredPrevious << '\n';
    paintOriginalDialogProc = nullptr;
    PaintSnapshot finalSnapshot;
    CapturePaintSnapshot(dialog, finalSnapshot);
    const char* finalMismatch = nullptr;
    if (PaintSnapshotSame(baseline, finalSnapshot, finalMismatch)) Check(true, "the staged paint regression restores the exact dialog geometry, controls, fonts and texts");
    else Check(false, finalMismatch);
    const auto afterInspect = cache::Inspect();
    bool sameProjects = afterInspect.error.empty() && afterInspect.projects.size() == projectPaths.size();
    if (sameProjects) for (std::size_t index = 0; index < projectPaths.size(); ++index)
        if (afterInspect.projects[index].path != projectPaths[index]) sameProjects = false;
    Check(sameProjects, "the staged paint regression leaves the inspected cache business snapshot unchanged");
    // Leave one coherent full frame; the probe is inactive so no fault can apply.
    Need(InvalidateRect(dialog, nullptr, FALSE) != FALSE, "invalidate the complete dialog after the staged paint regression");
    Need(UpdateWindow(dialog) != FALSE, "repaint the complete dialog after the staged paint regression");
    RemovePaintHooks();
    if (probe.expectedDamage) { DeleteObject(probe.expectedDamage); probe.expectedDamage = nullptr; }
    if (probe.childrenRegion) { DeleteObject(probe.childrenRegion); probe.childrenRegion = nullptr; }
    Need(RemoveWindowSubclass(dialog, PaintObserve, paintObserverId) != FALSE, "stop observing cache dialog paint messages");
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    evidence << "paint-regression\tphase=end\tlanguage=" << language << "\thwnd=" << dialog
        << "\tgdi-before=" << gdiBefore << "\tgdi-after=" << gdiAfter
        << "\tuser-before=" << userBefore << "\tuser-after=" << userAfter
        << "\tcold-source=" << paintColdSource << "\trecovered-source=" << paintRecoveredSource << '\n';
    evidence.flush();
    Capture(L"paint-after");
}
#endif // !MMD_ORIGINAL_PROFILE
BOOL CALLBACK Tooltips(HWND window,LPARAM result) {
    wchar_t name[80]{};GetClassNameW(window,name,80);
    if(std::wstring(name)==TOOLTIPS_CLASSW&&GetWindow(window,GW_OWNER)==dialog)*reinterpret_cast<HWND*>(result)=window;
    return TRUE;
}
void CheckTooltips() {
    HWND tips=nullptr;EnumThreadWindows(GetCurrentThreadId(),Tooltips,reinterpret_cast<LPARAM>(&tips));
    Check(tips&&SendMessageW(tips,TTM_GETTOOLCOUNT,0,0)==4,"automatic option and all cleaning buttons have hover hints");
    for(int id:{ui::Automatic,ui::ClearProject,ui::ClearReclaimable,ui::ClearAll}) {
        wchar_t text[1024]{};TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.hwnd=dialog;
        tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(dialog,id));tool.lpszText=text;
        SendMessageW(tips,TTM_GETTEXTW,1024,reinterpret_cast<LPARAM>(&tool));
        Check(text[0]!=0,"localized cleaning tooltip text is present");
        if(id==ui::ClearReclaimable)Check(std::wstring(text).find(L"80%")!=std::wstring::npos,"reclaimable tooltip explains automatic target");
    }
}
void ClickAt(HWND window,int id,const fs::path& root){
    RequireDesktop();RequireStorageAt(root);Need(fixtureVerified&&OwnedByTest(window)&&GetWindow(window,GW_OWNER)==fixtureOwner,"click only verified fixture dialog");
    const auto control=GetDlgItem(window,id);
    Need(IsWindowEnabled(control)!=FALSE,"real clicked control is enabled");
    if (constrainedThread) {
        RECT bounds{};
        Need(GetWindowRect(control,&bounds),"measure the real button before activating it");
        POINT center{bounds.left+(bounds.right-bounds.left)/2,bounds.top+(bounds.bottom-bounds.top)/2};
        Check(Inside(constrainedWork,bounds)&&ScreenToClient(window,&center)&&
            ChildWindowFromPointEx(window,center,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED)==control,
            "the action button is fully inside the work area and hittable");
    }
    SendDlgItemMessageW(window,id,BM_CLICK,0,0);
}
void Click(int id){ClickAt(dialog,id,CaseRoot());}
std::wstring SelectedProject(){
    const int row=ListView_GetNextItem(GetDlgItem(dialog,ui::Projects),-1,LVNI_SELECTED);
    if(row<0)return {};
    const auto view=cache::Inspect();
    return std::size_t(row)<view.projects.size()?view.projects[std::size_t(row)].path:L"";
}
void CALLBACK Poll(HWND,UINT,UINT_PTR,DWORD) {
    try {
    ++ticks;Need(GetTickCount64()<deadline,"real dialog actions complete before 60 second per-language deadline");
    RequireDesktop();
    EnumThreadWindows(GetCurrentThreadId(),Find,0);
    // Inspect modal windows during every stage, including before the main
    // dialog is found. A mismatched prompt is recorded and aborts, never clicked.
    const bool waitingForConfirmation=stage==10;
    EnumThreadWindows(GetCurrentThreadId(),Confirm,0);
    if(waitingForConfirmation||!dialog||!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
    RequireStorage();Geometry();
    switch(stage) {
    case 0: {
        // Before any original per-language stage 0 operation, with the settled
        // cache inspection in place, exercise the production backdrop paint path
        // on this real dialog and then keep the original behavior unchanged.
#if !MMD_ORIGINAL_PROFILE
        if(interfacePercent==100)PaintRegression();
#endif
        Check(IsDlgButtonChecked(dialog,ui::Enabled)==BST_CHECKED&&IsDlgButtonChecked(dialog,ui::Automatic)==BST_CHECKED,"both caching and automatic cleanup start checked");
        Check(ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==2,"both fixture projects appear");
        wchar_t text[32]{};GetDlgItemTextW(dialog,ui::Limit,text,32);Check(std::wstring(text)==L"4","four GB shown by default");
        Check(GetDlgItemInt(dialog,ui::Recent,nullptr,FALSE)==10,"ten recent projects shown by default");
        CheckTooltips();stage=1;Click(ui::Enabled);Click(ui::Apply);break;
    }
    case 1:Check(!cache::Current().enabled,"disable caching is persisted");Check(!IsWindowEnabled(GetDlgItem(dialog,ui::Automatic)),"automatic cleanup disabled while caching is off");stage=2;Click(ui::Enabled);Click(ui::Apply);break;
    case 2:Check(cache::Current().enabled,"caching can be re-enabled");stage=3;Click(ui::Automatic);Click(ui::Apply);break;
    case 3:Check(!cache::Current().automaticCleanup,"automatic cleanup can be disabled independently");stage=4;Click(ui::Automatic);Click(ui::Apply);break;
    case 4:Check(cache::Current().automaticCleanup,"automatic cleanup can be re-enabled");
        ListView_SetItemState(GetDlgItem(dialog,ui::Projects),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);stage=5;Click(ui::Pin);break;
    case 5: {
        auto view=cache::Inspect();Check(!view.projects.empty()&&view.projects[0].pinned,"pin button saves project preference");
        Check(IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)),"selected exclusive cache can be manually cleaned");
        const int row=ListView_GetNextItem(GetDlgItem(dialog,ui::Projects),-1,LVNI_SELECTED);
        Need(row==0&&std::size_t(row)<view.projects.size(),"selected-project cleanup has one selected fixture row");
        projectPath=view.projects[std::size_t(row)].path;
        Need(projectPath==(output/L"另一个工程"/L"Stage.pmm").wstring(),"selected project owns the exclusive later fixture payload");
        blockedPayload=File(3);blockedBytes=ReadBytes(blockedPayload);sharedBytes=ReadBytes(File(1));otherBytes=ReadBytes(File(2));
        StartLeaseChild(3);
        Need(DeleteAccess(blockedPayload)==ERROR_SHARING_VIOLATION,"the other process's production lease denies deletion");
        evidence<<"project-clean\tphase=blocker-held\tlanguage="<<language<<"\tfile="<<Utf8(blockedPayload.wstring())<<"\tbytes="<<blockedBytes.size()<<'\n';
        projectPhase=1;stage=6;Click(ui::ClearProject);break;
    }
    case 6: {
        if(projectPhase==1) {
            projectPhase=2;
            Check(fs::exists(blockedPayload)&&ReadBytes(blockedPayload)==blockedBytes,"skipped cleanup leaves the blocked exclusive payload byte-identical");
            Check(fs::exists(File(1))&&ReadBytes(File(1))==sharedBytes&&fs::exists(File(2))&&ReadBytes(File(2))==otherBytes,"skipped cleanup leaves shared and other-project payloads unchanged");
            Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedSkipStatus(),"skipped cleanup reports exactly one skipped file");
            Check(SelectedProject()==projectPath&&Text(GetDlgItem(dialog,ui::Path))==projectPath,"the same project remains selected after the skipped cleanup");
            Check(IsWindowEnabled(GetDlgItem(dialog,ui::Apply))&&IsWindowEnabled(GetDlgItem(dialog,ui::ClearAll))&&
                IsWindowEnabled(GetDlgItem(dialog,ui::ClearReclaimable))&&IsWindowEnabled(GetDlgItem(dialog,ui::Enabled)),"busy controls are usable after the skipped cleanup");
            evidence<<"project-clean\tphase=skipped\tlanguage="<<language<<"\tskipped=1\tstatus="<<Utf8(Text(GetDlgItem(dialog,ui::Status)))
                <<"\tselected="<<Utf8(projectPath)<<"\tclear-project-enabled="<<IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject))<<'\n';
            Capture(L"project-clean-skipped");
            ReleaseLeaseChild();
            return;
        }
        if(projectPhase==2) {
            projectPhase=3;
            // Red regression: after the blocking handle is gone, the same window
            // must offer the selected-project cleanup again without another action
            // refreshing the snapshot, reopening the dialog, or synthesizing input.
            Check(IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)),"selected-project cleanup is available again in the same window after the blocker is released");
            Check(SelectedProject()==projectPath,"the same project is still selected before the retry");
            evidence<<"project-clean\tphase=released\tlanguage="<<language<<"\tclear-project-enabled="<<IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject))<<'\n';
            Click(ui::ClearProject);return;
        }
        Check(!fs::exists(blockedPayload),"retried cleanup removes the previously blocked exclusive payload");
        Check(!fs::exists(File(3))&&fs::exists(File(1))&&fs::exists(File(2)),"manual project action preserves shared cache");
        Check(fs::exists(File(1))&&ReadBytes(File(1))==sharedBytes&&fs::exists(File(2))&&ReadBytes(File(2))==otherBytes,"retried cleanup leaves shared and other-project payloads unchanged");
        Check(RetriedStatus(Text(GetDlgItem(dialog,ui::Status))),"retried cleanup reports one removed file without skips");
        Check(SelectedProject()==projectPath,"the same project remains selected after the retry");
        Check(IsWindowEnabled(GetDlgItem(dialog,ui::Apply))&&IsWindowEnabled(GetDlgItem(dialog,ui::ClearAll)),"busy controls are usable after the retried cleanup");
        evidence<<"project-clean\tphase=retried\tlanguage="<<language<<"\tremoved=1\tskipped=0\tstatus="<<Utf8(Text(GetDlgItem(dialog,ui::Status)))<<'\n';
        projectPhase=4;SetDlgItemTextW(dialog,ui::Recent,L"1");stage=7;Click(ui::Apply);break;
    }
    case 7:Check(cache::Current().recentProjects==1,"recent project preference is saved");stage=8;Click(ui::ClearReclaimable);break;
    case 8:
        Check(!fs::exists(File(2))&&fs::exists(File(1)),"manual reclaimable action cleans older exclusive data");
        StartLeaseChild(1);
        Need(DeleteAccess(File(1))==ERROR_SHARING_VIOLATION,"shared payload is protected before the clear-all confirmation");
        stage=10;Click(ui::ClearAll);break;
    case 11:{
        if(confirmations==1){
            Check(fs::exists(File(1))&&ReadBytes(File(1))==sharedBytes,"clear-all preserves the shared payload used by another process");
            const auto status=Text(GetDlgItem(dialog,ui::Status));
            Check(status==ExpectedSkipStatus(),"first clear-all reports one in-use file skipped");
            evidence<<"clear-all\tphase=skipped\tlanguage="<<language<<"\tstatus="<<Utf8(status)<<'\n';
            ReleaseLeaseChild();
            Check(IsWindowEnabled(GetDlgItem(dialog,ui::ClearAll)),"same window still allows clear-all after reader reset");
            stage=10;Click(ui::ClearAll);break;
        }
        Check(confirmations==2,"both fixture clear-all confirmations were clicked");
        const auto view=cache::Inspect();
        HANDLE file=CreateFileW((cache::Root()/L"Management"/L"index.bin").c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        FILE_STANDARD_INFO info{};const bool measured=file!=INVALID_HANDLE_VALUE&&GetFileInformationByHandleEx(file,FileStandardInfo,&info,sizeof info);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        const auto bytes=measured?(std::max)(info.EndOfFile.QuadPart,info.AllocationSize.QuadPart):0;
        Check(measured&&view.error.empty()&&view.effectBytes==0&&view.textureBytes==0&&view.totalBytes==bytes,
            "clear-all removes unused payloads while retained index remains accounted");
        Check(!fs::exists(File(1))&&!fs::exists(File(2))&&!fs::exists(File(3)),"all three isolated payloads removed");
        const auto status=Text(GetDlgItem(dialog,ui::Status));
        Check(RetriedStatus(status),"retried clear-all reports one removed file without skips");
        evidence<<"clear-all\tphase=retried\tlanguage="<<language<<"\tstatus="<<Utf8(status)<<'\n';
        stage=12;if(constrainedThread)Click(IDCANCEL);else PostMessageW(dialog,WM_CLOSE,0,0);break;}
    }
    }catch(const std::exception& error){std::printf("poll exception: %s\n",error.what());Need(false,"exception in native dialog poll");}
    catch(...){Need(false,"unknown exception in native dialog poll");}
}
BOOL WINAPI GatedDelete(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID info,DWORD size){
    const DWORD incoming=GetLastError();
    bool match=false;
    if(kind==FileDispositionInfo&&!busyTarget.empty()&&GetCurrentThreadId()!=busyUiThread) {
        wchar_t path[1024]{};
        const auto n=GetFinalPathNameByHandleW(file,path,_countof(path),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        match=n>4&&n<_countof(path)&&_wcsicmp(path+4,busyTarget.c_str())==0;
    }
    if(!match){SetLastError(incoming);return realDelete(file,kind,info,size);}
    const bool first=busyDeleteCalls.fetch_add(1)==0;
    busyHookSeen.store(true);
    busyHookInFlight.fetch_add(1);
    if(first) {
        busyWorkerThread.store(GetCurrentThreadId());
        SetEvent(busyEntered);
        const DWORD waited=WaitForSingleObject(busyRelease,5000);
        if(waited!=WAIT_OBJECT_0)busyGateTimedOut.store(true);
    }
    BOOL result=FALSE;DWORD error=ERROR_ACCESS_DENIED;
    if(first&&busyDeleteOutcome==BusyDeleteOutcome::AccessDenied)busyDeleteDenials.fetch_add(1);
    else {
        SetLastError(incoming);result=realDelete(file,kind,info,size);error=GetLastError();
        if(result)busyDeleteSuccesses.fetch_add(1);
    }
    busyDeleteError.store(error);
    busyHookInFlight.fetch_sub(1);
    SetEvent(busyReturned);
    SetLastError(error);
    return result;
}
void InstallBusyGate(const fs::path& payload,BusyDeleteOutcome outcome=BusyDeleteOutcome::Delete){
    busyTarget=payload.wstring();busyUiThread=GetCurrentThreadId();
    busyDeleteOutcome=outcome;
    busyHookSeen.store(false);busyGateTimedOut.store(false);busyHookInFlight.store(0);
    busyDeleteCalls.store(0);busyDeleteDenials.store(0);busyDeleteSuccesses.store(0);
    busyWorkerThread.store(0);busyDeleteError.store(ERROR_SUCCESS);
    Need((busyEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr))!=nullptr,"create deletion-enter event");
    Need((busyRelease=CreateEventW(nullptr,TRUE,FALSE,nullptr))!=nullptr,"create deletion-release event");
    Need((busyReturned=CreateEventW(nullptr,TRUE,FALSE,nullptr))!=nullptr,"create deletion-return event");
    const auto initialized=MH_Initialize();
    Need(initialized==MH_OK||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize MinHook for the busy-close fixture deletion");
    busyHookInitialized=initialized==MH_OK;
    Need(MH_CreateHook(reinterpret_cast<void*>(&SetFileInformationByHandle),reinterpret_cast<void*>(&GatedDelete),reinterpret_cast<void**>(&realDelete))==MH_OK,"hook only the busy-close fixture deletion");
    Need(MH_EnableHook(reinterpret_cast<void*>(&SetFileInformationByHandle))==MH_OK,"enable only the busy-close fixture deletion hook");
}
void RemoveBusyGate(){
    Need(MH_DisableHook(reinterpret_cast<void*>(&SetFileInformationByHandle))==MH_OK,"disable the busy-close deletion hook after the worker exited");
    Need(MH_RemoveHook(reinterpret_cast<void*>(&SetFileInformationByHandle))==MH_OK,"remove the busy-close deletion hook after the worker exited");
    if(busyHookInitialized)Need(MH_Uninitialize()==MH_OK,"uninitialize MinHook after the busy-close worker exited");
    realDelete=nullptr;busyTarget.clear();busyHookInitialized=false;
    for(HANDLE* event:{&busyEntered,&busyRelease,&busyReturned}) {
        if(*event){Need(CloseHandle(*event)!=FALSE,"close the busy-close gate events");*event=nullptr;}
    }
}
void CALLBACK PollBusy(HWND,UINT,UINT_PTR,DWORD) {
    try {
    ++ticks;Need(GetTickCount64()<deadline,"busy-close supervision completes before its deadline");
    RequireDesktop();
    EnumThreadWindows(GetCurrentThreadId(),Find,0);
    // Never answer an unexpected modal during the busy-close scenario.
    EnumThreadWindows(GetCurrentThreadId(),Confirm,0);
    if(!dialog)return;
    if(busyStage==6) {
        if(!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        Check(Text(GetDlgItem(dialog,ui::Status))!=ExpectedClosingStatus(),"reopened dialog has no stale pending-close status");
        Check(ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==1,"reopened dialog still lists the fixture project");
        evidence<<"busy-close\tphase=reopen\tlanguage="<<language<<"\tstatus="<<Utf8(Text(GetDlgItem(dialog,ui::Status)))<<'\n';
        Need(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"close the reopened dialog normally");
        busyStage=7;return;
    }
    if(busyStage==0) {
        if(!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        RequireStorageAt(BusyRoot());
        Check(Text(dialog)==ExpectedTitle(),"busy-close dialog has the expected localized title");
        Check(ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==1,"busy-close fixture lists exactly one project");
        RecordWindow(dialog,"busy-close-dialog");
        ListView_SetItemState(GetDlgItem(dialog,ui::Projects),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        busyStage=1;return;
    }
    if(busyStage==1) {
        if(!IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)))return;
        Check(SelectedProject()==(BusyRoot()/L"busy-close-project"/L"Stage.pmm").wstring(),"busy-close cleanup targets the new fixture project");
        evidence<<"busy-close\tphase=start\tlanguage="<<language<<"\thwnd="<<dialog<<'\n';
        busyStage=2;ClickAt(dialog,ui::ClearProject,BusyRoot());return;
    }
    if(busyStage==2) {
        if(WaitForSingleObject(busyEntered,0)!=WAIT_OBJECT_0)return;
        Check(IsWindow(dialog)!=FALSE,"busy-close dialog remains alive while the worker is blocked");
        Check(!IsWindowEnabled(GetDlgItem(dialog,ui::Apply))&&!IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject))&&
            !IsWindowEnabled(GetDlgItem(dialog,ui::ClearAll))&&!IsWindowEnabled(GetDlgItem(dialog,ui::ClearReclaimable)),"busy-close actions are disabled while the worker is blocked");
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedWorkingStatus(),"busy-close shows the localized working status");
        Check(busyHookInFlight.load()==1,"busy-close worker is inside the real deletion call");
        evidence<<"busy-close\tphase=entered\tlanguage="<<language<<"\tin-flight="<<busyHookInFlight.load()<<'\n';
        Need(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"request close while the cleanup is busy");
        busyStage=3;return;
    }
    if(busyStage==3) {
        ++busyTicksAfterClose;
        Check(IsWindow(dialog)!=FALSE,"busy-close window still exists after the close request");
        Check(Text(dialog)==ExpectedTitle(),"busy-close title is stable after the close request");
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedClosingStatus(),"busy-close shows the localized pending-close status");
        Check(!IsWindowEnabled(GetDlgItem(dialog,ui::Apply))&&!IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)),"busy-close actions stay disabled while closing is pending");
        Check(busyHookInFlight.load()==1,"busy-close worker is still blocked after the close request");
        Capture(L"busy-close-pending-"+std::to_wstring(busyTicksAfterClose));
        evidence<<"busy-close\tphase=closing\tlanguage="<<language<<"\tticks="<<busyTicksAfterClose<<"\tstatus="<<Utf8(Text(GetDlgItem(dialog,ui::Status)))<<'\n';
        busyStage=4;return;
    }
    if(busyStage==4) {
        ++busyTicksAfterClose;
        Check(busyTicksAfterClose>=2,"at least two UI timer ticks were serviced while the worker was blocked");
        Check(IsWindow(dialog)!=FALSE,"busy-close window survives another supervision tick");
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedClosingStatus(),"pending-close status persists without a UI join block");
        Check(busyHookInFlight.load()==1,"busy-close worker is still blocked before the gate release");
        evidence<<"busy-close\tphase=release\tlanguage="<<language<<"\tticks="<<busyTicksAfterClose<<'\n';
        busyStage=5;Need(SetEvent(busyRelease)!=FALSE,"release the real deletion gate");return;
    }
    }catch(const std::exception& error){std::printf("busy poll exception: %s\n",error.what());Need(false,"exception in busy-close supervision");}
    catch(...){Need(false,"unknown exception in busy-close supervision");}
}
void BusyCloseScenario(mikumikustudio::MMDApp* app){
    BusyFixture();
    stage=20;ticks=0;confirmations=0;dialog=nullptr;busyStage=0;busyTicksAfterClose=0;
    InstallBusyGate(BusyPayload());
    evidence<<"busy-close\tphase=hook-installed\tlanguage="<<language<<"\ttarget="<<Utf8(BusyPayload().wstring())<<'\n';
    deadline=GetTickCount64()+30000;
    Need(SetTimer(fixtureOwner,78,100,PollBusy)!=0,"start busy-close supervision timer");
    Check(ui::HandleCommand(app,ui::kCommand),"busy-close cache management command opens the real dialog");
    KillTimer(fixtureOwner,78);
    Check(busyStage==5,"busy-close reached the gated deletion and pending close");
    Check(busyHookSeen.load(),"busy-close worker reached the real deletion call");
    Check(!busyGateTimedOut.load(),"busy-close gate was released before its bounded timeout");
    Check(IsWindow(dialog)==FALSE,"busy-close modal ended automatically after the worker finished");
    Check(WaitForSingleObject(busyReturned,2000)==WAIT_OBJECT_0&&busyHookInFlight.load()==0,"busy-close background deletion callback exited before hook removal");
    Check(!fs::exists(BusyPayload()),"busy-close real deletion removed the exact fixture payload");
    Check(fs::is_regular_file(BusyRoot()/L"Cache"/L"Management"/L"index.bin"),"busy-close cleanup persisted the isolated index");
    evidence<<"busy-close\tphase=closed\tlanguage="<<language<<"\tdeleted="<<(!fs::exists(BusyPayload()))<<'\n';
    RemoveBusyGate();
    // Reopen once after the gate is gone: no stale closing state may persist.
    dialog=nullptr;busyStage=6;deadline=GetTickCount64()+15000;
    Need(SetTimer(fixtureOwner,78,100,PollBusy)!=0,"restart busy-close supervision for the reopen");
    Check(ui::HandleCommand(app,ui::kCommand),"busy-close reopened cache management dialog");
    KillTimer(fixtureOwner,78);
    Check(busyStage==7&&IsWindow(dialog)==FALSE,"reopened dialog closed normally without a stale closing state");
    evidence<<"busy-close\tphase=reopen-closed\tlanguage="<<language<<'\n';
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(CaseRoot()/L"Cache").c_str())!=FALSE,"restore the normal fixture cache override");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(CaseRoot()/L"settings.json").c_str())!=FALSE,"restore the normal fixture settings override");
    cache::Shutdown();
    RequireStorage();
}
void CheckBusyFailureActions(bool canClean){
    Check(dialog==busyFailureWindow&&IsWindow(dialog)&&IsWindowEnabled(dialog),"busy-close failure retains the same usable dialog HWND");
    bool enabled=IsWindowEnabled(GetDlgItem(dialog,IDCANCEL))!=FALSE;
    for(int id:{ui::Enabled,ui::Automatic,ui::Limit,ui::Recent,ui::Apply,ui::ClearReclaimable,ui::ClearAll,ui::Pin,ui::Projects})
        enabled=enabled&&IsWindowEnabled(GetDlgItem(dialog,id));
    Check(enabled,"busy-close failure restores settings, selection, cleaning and close controls");
    Check((IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject))!=FALSE)==canClean,"selected-project cleanup availability matches the remaining exclusive payload");
    Check(SelectedProject()==BusyFailureProject().wstring()&&Text(GetDlgItem(dialog,ui::Path))==BusyFailureProject().wstring(),
        "busy-close failure and retry preserve the exact selected fixture project");
}
void CheckBusyFailureStorage(bool present){
    RequireStorageAt(BusyFailureRoot());
    const auto payload=BusyFailurePayload(),index=BusyFailureRoot()/L"Cache"/L"Management"/L"index.bin";
    if(present)Check(fs::is_regular_file(payload)&&ReadBytes(payload)==busyFailureBytes,"denied busy-close deletion leaves the exact payload byte-identical");
    else Check(!fs::exists(payload),"explicit busy-close retry removes the exact fixture payload");
    Check(fs::is_regular_file(index)&&!ReadBytes(index).empty(),"busy-close failure fixture index remains readable on disk");
    const auto snapshot=cache::Inspect();
    Check(snapshot.error.empty()&&snapshot.projects.size()==1&&snapshot.projects[0].path==BusyFailureProject().wstring()&&
        snapshot.projects[0].sharedBytes==0&&(present?snapshot.projects[0].totalBytes>0:snapshot.projects[0].totalBytes==0)&&
        (present?snapshot.textureBytes>0:snapshot.textureBytes==0),"readable index retains the fixture project and accounts for the expected payload");
}
void BusyFailureEvidence(const char* phase){
    evidence<<"busy-close-failure\tphase="<<phase<<"\tlanguage="<<language<<"\thwnd="<<dialog
        <<"\tdelete-calls="<<busyDeleteCalls.load()<<"\tdenied="<<busyDeleteDenials.load()<<"\tdeleted="<<busyDeleteSuccesses.load()
        <<"\tstatus="<<Utf8(Text(GetDlgItem(dialog,ui::Status)))<<'\n';
    evidence.flush();
}
void CALLBACK PollBusyFailure(HWND,UINT,UINT_PTR,DWORD){
    try {
    ++ticks;Need(GetTickCount64()<deadline,"busy-close failure supervision completes before its bounded deadline");
    RequireDesktop();EnumThreadWindows(GetCurrentThreadId(),Find,0);EnumThreadWindows(GetCurrentThreadId(),Confirm,0);
    if(!dialog)return;
    using Phase=BusyFailureStage;
    switch(busyFailureStage) {
    case Phase::Opening:
        if(!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        RequireStorageAt(BusyFailureRoot());busyFailureWindow=dialog;
        Check(Text(dialog)==ExpectedTitle()&&ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==1,"failure dialog opens with the localized title and one isolated project");
        RecordWindow(dialog,"busy-close-failure-dialog");
        ListView_SetItemState(GetDlgItem(dialog,ui::Projects),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        busyFailureStage=Phase::Selecting;return;
    case Phase::Selecting:
        if(!IsWindowEnabled(GetDlgItem(dialog,ui::ClearProject)))return;
        CheckBusyFailureActions(true);BusyFailureEvidence("start");
        busyFailureStage=Phase::Entering;ClickAt(dialog,ui::ClearProject,BusyFailureRoot());return;
    case Phase::Entering:
        if(WaitForSingleObject(busyEntered,0)!=WAIT_OBJECT_0)return;
        Check(busyDeleteCalls.load()==1&&busyHookInFlight.load()==1&&busyWorkerThread.load()!=0&&busyWorkerThread.load()!=busyUiThread,
            "exact failure payload reaches handle disposition once on the real background worker");
        Check(busyDeleteDenials.load()==0&&busyDeleteSuccesses.load()==0,"failure is injected only after releasing the worker gate");
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedWorkingStatus(),"failure fixture initially shows the localized working status");
        BusyFailureEvidence("entered");
        Need(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"send real WM_CLOSE while the failure worker is gated");
        busyFailureStage=Phase::PendingClose;return;
    case Phase::PendingClose:
    case Phase::PendingAgain: {
        // No cache API while the worker holds its storage lock. UI ticks and
        // disabled controls prove the close request did not synchronously join.
        Check(dialog==busyFailureWindow&&IsWindow(dialog),"failure window survives the close request until its worker finishes");
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedClosingStatus(),"failure fixture shows the exact localized pending-close status");
        bool disabled=true;
        for(int id:{ui::Enabled,ui::Automatic,ui::Limit,ui::Recent,ui::Apply,ui::ClearProject,ui::ClearAll,ui::ClearReclaimable,ui::Pin})
            disabled=disabled&&!IsWindowEnabled(GetDlgItem(dialog,id));
        Check(disabled&&busyHookInFlight.load()==1,"failure fixture keeps actions disabled and worker gated across two UI ticks");
        Check(WaitForSingleObject(busyReturned,0)==WAIT_TIMEOUT,"pending close is observed before the worker deletion returns");
        BusyFailureEvidence("pending-close");
        if(busyFailureStage==Phase::PendingClose){busyFailureStage=Phase::PendingAgain;return;}
        busyFailureStage=Phase::Failure;
        Need(SetEvent(busyRelease)!=FALSE,"release failure gate to return ERROR_ACCESS_DENIED exactly once");return;
    }
    case Phase::Failure:
        if(WaitForSingleObject(busyReturned,0)!=WAIT_OBJECT_0||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        Check(busyHookInFlight.load()==0&&busyDeleteCalls.load()==1&&busyDeleteDenials.load()==1&&busyDeleteSuccesses.load()==0&&
            busyDeleteError.load()==ERROR_ACCESS_DENIED,"worker receives exactly one access-denied deletion without removing the payload");
        // Clean::Remove counts a failed disposition as skipped, with empty error.
        // This exact localized result must remain visible despite pending close;
        // checking only Work.error/Snapshot.error would miss the regression.
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedSkipStatus(),"failed busy-close preserves the localized zero-removed one-skipped result");
        CheckBusyFailureActions(true);CheckBusyFailureStorage(true);BusyFailureEvidence("failure-visible");
        busyFailureStableTicks=0;busyFailureStage=Phase::RetainFailure;return;
    case Phase::RetainFailure:
        CheckBusyFailureActions(true);
        Check(Text(GetDlgItem(dialog,ui::Status))==ExpectedSkipStatus(),"localized failure result remains visible until an explicit retry");
        Check(busyDeleteCalls.load()==1&&busyDeleteDenials.load()==1,"no automatic retry consumes the retained failure");
        if(++busyFailureStableTicks<3)return;
        BusyFailureEvidence("explicit-retry");busyFailureStage=Phase::Retry;
        // The hook stays installed, but its single denial is consumed. Only a
        // genuine button click may launch this second production worker call.
        ClickAt(dialog,ui::ClearProject,BusyFailureRoot());return;
    case Phase::Retry:
        if(!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        Check(busyDeleteCalls.load()==2&&busyDeleteDenials.load()==1&&busyDeleteSuccesses.load()==1&&busyHookInFlight.load()==0,
            "explicit retry calls real handle disposition successfully without another injected failure");
        Check(RetriedStatus(Text(GetDlgItem(dialog,ui::Status))),"explicit retry displays the localized one-removed zero-skipped result");
        CheckBusyFailureActions(false);CheckBusyFailureStorage(false);BusyFailureEvidence("retry-succeeded");
        busyFailureStableTicks=0;busyFailureStage=Phase::RetainRetry;return;
    case Phase::RetainRetry:
        // A fix that preserves the failed window but forgets to clear closing
        // ends the modal on the successful retry, before reaching these checks.
        CheckBusyFailureActions(false);
        Check(RetriedStatus(Text(GetDlgItem(dialog,ui::Status))),"successful retry stays visible in the same window without a delayed pending close");
        if(++busyFailureStableTicks<3)return;
        BusyFailureEvidence("retry-still-open");busyFailureStage=Phase::Closing;
        ClickAt(dialog,IDCANCEL,BusyFailureRoot());return;
    case Phase::Reopening:
        if(!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        RequireStorageAt(BusyFailureRoot());busyFailureWindow=dialog;
        Check(Text(GetDlgItem(dialog,ui::Status)).empty(),"reopened failure fixture has no stale closing or failure status");
        Check(ListView_GetItemCount(GetDlgItem(dialog,ui::Projects))==1,"reopened failure fixture retains its project history");
        BusyFailureEvidence("reopened");busyFailureStage=Phase::ReopenApply;
        ClickAt(dialog,ui::Apply,BusyFailureRoot());return;
    case Phase::ReopenApply:
        if(!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
        Check(dialog==busyFailureWindow&&IsWindow(dialog)&&Text(GetDlgItem(dialog,ui::Status))==ExpectedSavedStatus(),
            "reopened dialog completes another real worker operation without stale closing");
        busyFailureStableTicks=0;busyFailureStage=Phase::RetainReopen;return;
    case Phase::RetainReopen:
        Check(dialog==busyFailureWindow&&IsWindow(dialog)&&IsWindowEnabled(GetDlgItem(dialog,ui::Apply))&&
            Text(GetDlgItem(dialog,ui::Status))==ExpectedSavedStatus(),"reopened dialog stays operable after its worker finishes");
        if(++busyFailureStableTicks<3)return;
        BusyFailureEvidence("reopen-still-open");busyFailureStage=Phase::Reclosed;
        Need(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"explicitly close the reopened failure fixture normally");return;
    case Phase::Closing:
    case Phase::Reclosed:return;
    }
    }catch(const std::exception& error){std::printf("busy failure poll exception: %s\n",error.what());Need(false,"exception in busy-close failure supervision");}
    catch(...){Need(false,"unknown exception in busy-close failure supervision");}
}
void BusyCloseFailureScenario(mikumikustudio::MMDApp* app){
    BusyFailureFixture();stage=21;ticks=0;confirmations=0;dialog=nullptr;busyFailureWindow=nullptr;
    busyFailureStage=BusyFailureStage::Opening;busyFailureStableTicks=0;
    InstallBusyGate(BusyFailurePayload(),BusyDeleteOutcome::AccessDenied);
    evidence<<"busy-close-failure\tphase=hook-installed\tlanguage="<<language<<"\ttarget="<<Utf8(BusyFailurePayload().wstring())<<'\n';
    deadline=GetTickCount64()+30000;
    Need(SetTimer(fixtureOwner,79,100,PollBusyFailure)!=0,"start busy-close failure supervision");
    Check(ui::HandleCommand(app,ui::kCommand),"busy-close failure opens the real production cache dialog");
    KillTimer(fixtureOwner,79);
    // On the old unconditional EndDialog this returns in Phase::Failure. Keep
    // that RED nonfatal so hooks/events are removed and all three languages run.
    Check(busyFailureStage==BusyFailureStage::Closing,"failed busy-close stays open for failure review and successful same-window retry until explicit close");
    Check(busyHookSeen.load()&&!busyGateTimedOut.load(),"failure worker reached the exact gate and was released within five seconds");
    Need(SetEvent(busyRelease)!=FALSE,"leave the failure gate released on every modal return path");
    Check(WaitForSingleObject(busyReturned,2000)==WAIT_OBJECT_0&&busyHookInFlight.load()==0,"failure deletion callbacks have exited before removing the hook");
    Check(busyDeleteCalls.load()==2&&busyDeleteDenials.load()==1&&busyDeleteSuccesses.load()==1,"failure scenario has one denied call and one successful explicit retry");
    Check(IsWindow(dialog)==FALSE&&confirmations==0,"failure scenario ends without any extra confirmation dialog");
    evidence<<"busy-close-failure\tphase=modal-return\tlanguage="<<language<<"\tstage="<<static_cast<unsigned>(busyFailureStage)
        <<"\tdelete-calls="<<busyDeleteCalls.load()<<"\tdenied="<<busyDeleteDenials.load()<<"\tdeleted="<<busyDeleteSuccesses.load()<<'\n';
    RemoveBusyGate();
    // Shutdown only after the UI/worker lifecycle; do not reset the manager to
    // manufacture retryability while the original failed dialog is still open.
    cache::Shutdown();CheckBusyFailureStorage(busyDeleteSuccesses.load()==0);
    dialog=nullptr;busyFailureWindow=nullptr;busyFailureStage=BusyFailureStage::Reopening;
    deadline=GetTickCount64()+15000;
    Need(SetTimer(fixtureOwner,79,100,PollBusyFailure)!=0,"supervise reopening after busy-close failure");
    Check(ui::HandleCommand(app,ui::kCommand),"reopen the real cache dialog after failure and retry");
    KillTimer(fixtureOwner,79);
    Check(busyFailureStage==BusyFailureStage::Reclosed&&IsWindow(dialog)==FALSE,"reopened failure fixture completes a worker operation then closes only on explicit request");
    evidence<<"busy-close-failure\tphase=reopen-closed\tlanguage="<<language<<'\n';
    cache::Shutdown();busyFailureBytes.clear();busyFailureWindow=nullptr;
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(CaseRoot()/L"Cache").c_str())!=FALSE,"restore normal cache override after busy-close failure");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(CaseRoot()/L"settings.json").c_str())!=FALSE,"restore normal settings override after busy-close failure");
    RequireStorage();
}
enum class ConfirmationInterruption { ReentrantCommands, OwnerClose };
enum class ConfirmationPhase { Opening, Waiting, Dispatching, Answered, ExplicitClose };
struct ConfirmationProbe {
    fs::path root;
    ConfirmationInterruption interruption;
    ConfirmationPhase phase=ConfirmationPhase::Opening;
    HWND prompt=nullptr,yes=nullptr;
    unsigned prompts=0;
};
ConfirmationProbe* confirmationProbe=nullptr;

BOOL CALLBACK ObserveConfirmation(HWND window,LPARAM value){
    auto& probe=*reinterpret_cast<ConfirmationProbe*>(value);
    if(window==dialog||window==probe.prompt||Class(window)!=L"#32770")return TRUE;
    const HWND yes=VerifyConfirmation(window);
    ++probe.prompts;
    if(probe.phase==ConfirmationPhase::Waiting&&!probe.prompt){
        probe.prompt=window;probe.yes=yes;return TRUE;
    }
    Need(probe.phase==ConfirmationPhase::Dispatching&&
        probe.interruption==ConfirmationInterruption::ReentrantCommands&&IsWindow(probe.prompt),
        "unexpected modal outside the reentrant clear-cache command");
    Check(false,"reentrant ClearAll opened a second confirmation");
    // On the unfixed dialog, decline only the verified duplicate so the nested
    // SendMessage returns and the rest of the regression can finish.
    const HWND no=GetDlgItem(window,IDNO);
    Need(IsWindowEnabled(no)&&PostMessageW(no,BM_CLICK,0,0),
        "decline the duplicate fixture confirmation");
    return FALSE;
}

void CALLBACK PollConfirmationInterruption(HWND,UINT,UINT_PTR,DWORD){
    try {
        Need(GetTickCount64()<deadline,"clear-cache confirmation supervision completes before its deadline");
        RequireDesktop();
        auto& probe=*confirmationProbe;
        RequireStorageAt(probe.root);
        EnumThreadWindows(GetCurrentThreadId(),Find,0);
        EnumThreadWindows(GetCurrentThreadId(),ObserveConfirmation,reinterpret_cast<LPARAM>(&probe));
        const bool ownerClose=probe.interruption==ConfirmationInterruption::OwnerClose;
        using Phase=ConfirmationPhase;
        switch(probe.phase){
        case Phase::Opening:
            if(!dialog||!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
            // Make an accidental Apply or stale Yes observably different from the saved policy.
            Need(SetDlgItemTextW(dialog,ui::Recent,L"29")!=FALSE,"stage an unsaved cache settings edit");
            probe.phase=Phase::Waiting;
            ClickAt(dialog,ui::ClearAll,probe.root);
            return;
        case Phase::Waiting: {
            if(!probe.prompt)return;
            Need(OwnedByTest(dialog)&&GetWindow(dialog,GW_OWNER)==fixtureOwner,
                "send confirmation-time messages only to the fixture cache dialog");
            probe.phase=Phase::Dispatching;
            if(ownerClose){
                SendMessageW(dialog,WM_CLOSE,0,0);
            }else{
                // Direct commands deliberately bypass the disabled owner and controls.
                for(const int id:{ui::ClearAll,ui::Apply})
                    SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),
                        reinterpret_cast<LPARAM>(GetDlgItem(dialog,id)));
            }
            // Decline in the reentry case to isolate command side effects.
            // After WM_CLOSE, Yes must have lost its authority to clear the cache.
            const HWND answer=ownerClose?probe.yes:GetDlgItem(probe.prompt,IDNO);
            Need(OwnedByTest(probe.prompt)&&GetWindow(probe.prompt,GW_OWNER)==dialog&&
                answer&&GetParent(answer)==probe.prompt&&IsWindowEnabled(answer),
                "the original verified confirmation can still receive its answer");
            probe.phase=Phase::Answered;
            Need(PostMessageW(answer,BM_CLICK,0,0)!=FALSE,"answer the original fixture confirmation after dispatch");
            return;
        }
        case Phase::Answered:
            if(IsWindow(probe.prompt)||!IsWindowEnabled(dialog)||!IsWindowEnabled(GetDlgItem(dialog,ui::Apply)))return;
            if(ownerClose)Check(false,"cache dialog stayed open after close during confirmation");
            probe.phase=Phase::ExplicitClose;
            Need(PostMessageW(dialog,WM_CLOSE,0,0)!=FALSE,"close the idle cache fixture");
            return;
        case Phase::Dispatching:
        case Phase::ExplicitClose:
            return;
        }
    }catch(const std::exception& error){
        std::printf("confirmation poll exception: %s\n",error.what());
        Need(false,"exception in clear-cache confirmation supervision");
    }catch(...){Need(false,"unknown exception in clear-cache confirmation supervision");}
}

void ConfirmationInterruptionScenario(mikumikustudio::MMDApp* app,ConfirmationInterruption interruption){
    const bool ownerClose=interruption==ConfirmationInterruption::OwnerClose;
    stage=ownerClose?23:22;dialog=nullptr;fixtureVerified=false;
    const auto root=output/(L"lang-"+std::to_wstring(language)+(ownerClose?L"-confirm-close":L"-confirm-reentry"));
    const auto settingsPath=root/L"settings.json";
    Need(fs::create_directory(root)&&fs::create_directory(root/L"Cache"),"new confirmation fixture and cache directory");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(root/L"Cache").c_str())!=FALSE,
        "set isolated confirmation cache override");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",settingsPath.c_str())!=FALSE,
        "set isolated confirmation settings override");
    RequireStorageAt(root);
    cache::Settings saved;saved.recentProjects=17;
    std::wstring error;
    Need(cache::Configure(saved,error),"persist confirmation fixture settings");
    const auto settingsBytes=ReadBytes(settingsPath);
    const unsigned id=0x600u+language;
    const auto payload=FileAt(root/L"Cache",id);
    WritePayload(root/L"Cache",id,16384);
    cache::ResetScene();
    const auto payloadBytes=ReadBytes(payload);
    const auto snapshot=cache::Inspect();
    Need(snapshot.error.empty()&&snapshot.textureBytes>0&&snapshot.reclaimableBytes>0&&
        !settingsBytes.empty()&&payloadBytes.size()==16384,
        "confirmation fixture has persisted settings and an idle deletable synthetic payload");
    fixtureVerified=true;

    InstallBusyGate(payload);
    Need(SetEvent(busyRelease)!=FALSE,"let an unexpected confirmation worker finish for bounded failure");
    ConfirmationProbe probe{root,interruption};
    confirmationProbe=&probe;
    deadline=GetTickCount64()+15000;
    Need(SetTimer(fixtureOwner,80,100,PollConfirmationInterruption)!=0,"start confirmation interruption supervision");
    Check(ui::HandleCommand(app,ui::kCommand),"open real cache dialog for confirmation interruption");
    KillTimer(fixtureOwner,80);
    confirmationProbe=nullptr;

    Check(probe.prompts==1,"confirmation interruption observes exactly one clear-cache prompt");
    Check(probe.phase==(ownerClose?ConfirmationPhase::Answered:ConfirmationPhase::ExplicitClose)&&
        !IsWindow(probe.prompt)&&!IsWindow(dialog)&&IsWindowEnabled(fixtureOwner),
        "confirmation and cache dialog finish and restore their fixture owner");
    Check(busyDeleteCalls.load()==0,"confirmation interruption starts no worker deletion of the synthetic payload");
    RemoveBusyGate();
    RequireStorageAt(root);
    Check(fs::is_regular_file(payload)&&ReadBytes(payload)==payloadBytes,
        "confirmation interruption leaves the synthetic cache payload byte-identical");
    const auto actual=cache::Current();
    Check(fs::is_regular_file(settingsPath)&&ReadBytes(settingsPath)==settingsBytes&&
        actual.enabled==saved.enabled&&actual.automaticCleanup==saved.automaticCleanup&&
        actual.limitBytes==saved.limitBytes&&actual.recentProjects==saved.recentProjects,
        "confirmation interruption leaves persisted cache settings unchanged");

    cache::Shutdown();
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",(CaseRoot()/L"Cache").c_str())!=FALSE,
        "restore normal cache override after confirmation interruption");
    Need(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_SETTINGS",(CaseRoot()/L"settings.json").c_str())!=FALSE,
        "restore normal settings override after confirmation interruption");
    RequireStorage();
}

int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
    if(argc==6&&!std::wcscmp(argv[1],L"--scene-lease-child"))return RunLeaseChild(argv);
    Prepare(argc,argv);ConfigureDpi();
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES};Need(InitCommonControlsEx(&common)!=FALSE,"initialize real common controls");
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"MMS-cache-ui-owner";wc.lpfnWndProc=DefWindowProcW;
    Need(RegisterClassW(&wc)!=0,"register fixture owner");
    // Establish and verify the first isolated data set before any real UI window.
    Fixture();RequireDesktop();
    fixtureOwner=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1200,800,nullptr,nullptr,wc.hInstance,nullptr);
    Need(fixtureOwner!=nullptr,"create owner on held private desktop");
    if(interfacePercent>100){
        const HWND baseline=CreateDialogParamW(wc.hInstance,MAKEINTRESOURCEW(ui::kDialog),fixtureOwner,
            [](HWND,UINT,WPARAM,LPARAM)->INT_PTR{return FALSE;},0);
        Need(baseline&&GetClientRect(baseline,&templateClient)&&GetWindowRect(baseline,&templateFrame),
            "measure the unscaled native resource template");
        LOGFONTW font{};
        const auto assigned=reinterpret_cast<HFONT>(SendMessageW(baseline,WM_GETFONT,0,0));
        Need(assigned&&GetObjectW(assigned,sizeof font,&font)==sizeof font,"measure the native template font");
        templateFontHeight=font.lfHeight;
        DestroyWindow(baseline);
        Need(mikumikustudio::system_settings::WriteSection("uiScaling","{\"percent\":"+std::to_string(interfacePercent)+"}")==
            mikumikustudio::system_settings::Result::Ok,"write the requested percentage to isolated application settings");
        mikumikustudio::ui_scale::ApplyAtStartup();
        Need(mikumikustudio::ui_scale::Percent()==interfacePercent,"read the requested percentage through production settings");
    }
    auto app=std::make_unique<mikumikustudio::MMDApp>();
    // Do not materialize the giant aggregate temporary in wmain's stack frame.
    {auto initialState=std::make_unique<mikumikustudio::MMDAppState>();app->state=*initialState;}
    app->Hwnd()=fixtureOwner;app->HInstance()=wc.hInstance;
    HMENU bar=CreateMenu(),help=CreatePopupMenu();Need(bar&&help,"create fixture Help menu");
    Need(AppendMenuW(help,MF_STRING,201,L"About")&&AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(help),L"Help")&&SetMenu(fixtureOwner,bar),"attach real command menu");
    for(language=0;language<3;++language) {
        if(language)Fixture();
        app->EnglishUI()=static_cast<unsigned char>(language);
        if (!constrainedPercent) {
            ConfirmationInterruptionScenario(app.get(),ConfirmationInterruption::ReentrantCommands);
            ConfirmationInterruptionScenario(app.get(),ConfirmationInterruption::OwnerClose);
        }
        stage=ticks=confirmations=0;dialog=nullptr;projectPhase=0;projectPath.clear();
        blockedPayload.clear();blockedBytes.clear();sharedBytes.clear();otherBytes.clear();
        ui::RefreshMenu(app.get());
        Check(GetMenuState(help,ui::kCommand,MF_BYCOMMAND)!=UINT(-1),"Help menu exposes cache management");
        RequireDesktop();RequireStorage();deadline=GetTickCount64()+60000;
        const bool ownsMonitorHook = constrainedPercent && InstallConstrainedWorkArea();
        Need(SetTimer(fixtureOwner,77,100,Poll)!=0,"start native dialog supervision timer");
        Check(ui::HandleCommand(app.get(),ui::kCommand),"cache management command opens real dialog");KillTimer(fixtureOwner,77);
        if (constrainedPercent) RemoveConstrainedWorkArea(ownsMonitorHook);
        Check(stage==12,"all native dialog actions completed");Check(confirmations==2,"expected confirmation count per language");
        cache::Shutdown();RequireStorage();
        const auto persisted=cache::Current();
        Check(fs::is_regular_file(CaseRoot()/L"settings.json")&&persisted.enabled&&persisted.automaticCleanup&&persisted.recentProjects==1,"settings persist after manager shutdown and reload");
        if (!constrainedPercent) {
            BusyCloseScenario(app.get());
            BusyCloseFailureScenario(app.get());
        }
        cache::Shutdown();fixtureVerified=false;
    }
    SetMenu(fixtureOwner,nullptr);DestroyMenu(bar);DestroyWindow(fixtureOwner);app->Hwnd()=nullptr;
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    RequireDesktop();
    // No SetThreadDesktop/CloseDesktop on teardown: helper threads can outlive
    // wmain. The supervisor releases its handle only after process termination.
    evidence<<"result\tchecks="<<checks<<"\tfailures="<<failures<<"\tlanguages=ja,en,zh\tDPI-coverage=measured-system-only\n";
    evidence.flush();Need(bool(evidence),"flush final evidence");
    std::printf("cache manager UI: %u checks, %u failures; DPI coverage is measured system only\n",checks,failures);return failures?1:0;
    }catch(const std::exception& error){std::printf("exception: %s\n",error.what());Need(false,"UI fixture setup or teardown exception");}
    catch(...){Need(false,"unknown UI fixture exception");}
    return 2;
}
