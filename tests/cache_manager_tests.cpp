#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/cache_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
namespace cache=mikumikustudio::cache_manager;
constexpr std::uint64_t MiB=1024ull*1024;
unsigned checks=0;
void Check(bool ok,const char* text) {++checks;if(!ok){std::fprintf(stderr,"FAIL %s (%lu)\n",text,GetLastError());throw std::runtime_error(text);}}
void SetRoot(const fs::path& root) {Check(SetEnvironmentVariableW(L"MMS_CACHE_MANAGER_DIR",root.c_str())!=FALSE,"set isolated managed root");}
fs::path Entry(unsigned id) {wchar_t hash[65]{};swprintf_s(hash,L"%064x",id);return cache::Root()/L"Textures"/L"shared-texture"/(std::wstring(hash)+L".mtc");}
void Bytes(const fs::path& file,std::uint64_t length,bool recognized=true) {
    fs::create_directories(file.parent_path());
    HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(h!=INVALID_HANDLE_VALUE,"create isolated cache file");
    std::uint32_t header[19]{};header[0]=recognized?0x4354504d:0;header[1]=2;header[2]=std::uint32_t(length-76);
    DWORD written=0;Check(WriteFile(h,header,sizeof(header),&written,nullptr)&&written==sizeof(header),"write recognizable header");
    LARGE_INTEGER end{};end.QuadPart=length;Check(SetFilePointerEx(h,end,nullptr,FILE_BEGIN)&&SetEndOfFile(h),"size cache fixture");CloseHandle(h);
}
bool Store(unsigned id,std::uint64_t length) {
    const auto path=Entry(id);cache::WriteReservation write(path,length);
    if(!write)return false;Bytes(path,length);write.Commit();return true;
}
fs::path Case(const fs::path& output,const wchar_t* name) {
    const auto root=output/name/L"Cache";SetRoot(root);cache::Settings settings;settings.limitBytes=64*MiB;settings.recentProjects=1;
    std::wstring error;Check(cache::Configure(settings,error),"save isolated settings");return root;
}
void Project(const fs::path& name,std::initializer_list<unsigned> ids) {
    cache::BeginProject(name.c_str());for(auto id:ids)cache::Touch(Entry(id));cache::ProjectOpened(name.c_str());cache::ResetScene();Sleep(2);
}
std::wstring Quote(const std::wstring& p) {return L"\""+p+L"\"";}
PROCESS_INFORMATION Child(const std::wstring& args) {
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    auto command=Quote(exe)+L" "+args;STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"start own background cache worker");return process;
}
void Join(PROCESS_INFORMATION& child) {
    const auto wait=WaitForSingleObject(child.hProcess,15000);
    if(wait!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,99);
    DWORD code=99;GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hThread);CloseHandle(child.hProcess);
    std::printf("worker wait=%lu exit=%08lx\n",wait,code);
    Check(wait==WAIT_OBJECT_0&&code==0,"background cache worker completed");
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if(argc==3 && std::wstring(argv[1])==L"--inspect") {
            SetRoot(argv[2]);const auto view=cache::Inspect();
            std::printf("cache bytes=%llu effects=%llu textures=%llu projects=%zu error=%ls\n",view.totalBytes,view.effectBytes,view.textureBytes,view.projects.size(),view.error.c_str());
            for(const auto& p:view.projects)std::printf("project bytes=%llu shared=%llu path=%ls\n",p.totalBytes,p.sharedBytes,p.path.c_str());
            return view.error.empty()?0:1;
        }
        if(argc==6 && std::wstring(argv[1])==L"--lease") {
            SetRoot(argv[2]);cache::Touch(argv[3]);
            {std::ofstream log(fs::path(argv[2]).parent_path()/L"lease-child.log");const auto view=cache::Inspect();
             log<<"bytes="<<view.totalBytes<<" reclaimable="<<view.reclaimableBytes<<" error="<<std::string(view.error.begin(),view.error.end())<<"\n";}
            HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[4]),release=OpenEventW(SYNCHRONIZE,FALSE,argv[5]);
            Check(ready&&release,"open lease synchronization events");SetEvent(ready);
            const auto wait=WaitForSingleObject(release,10000);
            {std::ofstream log(fs::path(argv[2]).parent_path()/L"lease-child.log",std::ios::app);log<<"release_wait="<<wait<<"\n";}
            Check(wait==WAIT_OBJECT_0,"lease worker released");cache::Shutdown();CloseHandle(ready);CloseHandle(release);return 0;
        }
        if(argc==4 && std::wstring(argv[1])==L"--store") {
            SetRoot(argv[2]);const bool stored=Store(unsigned(std::wcstoul(argv[3],nullptr,10)),32*MiB);
            std::printf("concurrent publication=%s\n",stored?"stored":"full/in-use, normal loader fallback");cache::Shutdown();return 0;
        }
        Check(argc==2,"explicit new output directory required");
        const auto output=fs::absolute(argv[1]);Check(!fs::exists(output),"refuse to overwrite old test output");fs::create_directories(output);
        SetRoot(output/L"defaults"/L"Cache");const auto defaults=cache::Current();
        Check(defaults.enabled&&defaults.automaticCleanup&&defaults.limitBytes==4096*MiB&&defaults.recentProjects==10,"fresh defaults are enabled, automatic, four GiB and ten projects");
        auto settings=defaults;settings.enabled=false;settings.automaticCleanup=false;settings.limitBytes=128*MiB;settings.recentProjects=7;std::wstring error;
        Check(cache::Configure(settings,error),"persist disabled settings");SetRoot(output/L"other"/L"Cache");SetRoot(output/L"defaults"/L"Cache");
        const auto persisted=cache::Current();Check(!persisted.enabled&&!persisted.automaticCleanup&&persisted.recentProjects==7,"settings reload preserves explicit opt-out");
        Check(!cache::WriteReservation(Entry(1),MiB),"disabled cache declines writes");

        SetRoot(output/L"memory-only"/L"Cache");SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1");
        cache::BeginProject((output/L"memory-only.pmm").c_str());cache::Touch(Entry(1));
        cache::ProjectOpened((output/L"memory-only.pmm").c_str());cache::EndProjectLoad();cache::ResetScene();cache::Shutdown();
        Check(!cache::WriteReservation(Entry(2),MiB),"memory-only mode declines managed disk writes");
        Check(!fs::exists(cache::Root())&&!fs::exists(cache::Root().parent_path()/L"data"),"memory-only project lifecycle creates no disk cache or index");
        SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr);

        auto root=Case(output,L"shared");
        Check(Store(1,8*MiB)&&Store(2,8*MiB)&&Store(3,8*MiB),"create shared and exclusive entries");cache::ResetScene();
        const auto a=output/L"A.pmm",b=output/L"B.pmm";
        Project(a,{1,2});Project(b,{1,3});auto view=cache::Inspect();
        Check(view.totalBytes==24*MiB && view.projects.size()==2,"shared data counts once globally");
        const auto row=std::find_if(view.projects.begin(),view.projects.end(),[&](const auto& p){return p.path==a;});
        Check(row!=view.projects.end()&&row->totalBytes==16*MiB&&row->sharedBytes==8*MiB&&row->reclaimableBytes==8*MiB,"project view separates shared, exclusive and releasable bytes");
        const auto clean=cache::Clean(cache::Cleanup::Project,a.wstring());
        Check(clean.files==1&&!fs::exists(Entry(2))&&fs::exists(Entry(1))&&fs::exists(Entry(3)),"manual project cleanup preserves another project's shared cache");
        cache::BeginProject(b.c_str());cache::Touch(Entry(1));cache::Touch(Entry(3));cache::ProjectOpened(b.c_str());
        Check(cache::Clean(cache::Cleanup::All).files==0,"current project cache cannot be deleted while leased");cache::ResetScene();
        Check(cache::Clean(cache::Cleanup::All).files==2,"closing project releases cache leases");

        root=Case(output,L"lru");Check(Store(1,24*MiB)&&Store(2,24*MiB),"fill eviction fixture");cache::ResetScene();Project(a,{1});Project(b,{2});
        Check(Store(3,24*MiB),"automatic cleanup admits a new entry");
        Check(!fs::exists(Entry(1))&&fs::exists(Entry(2))&&fs::exists(Entry(3)),"older non-recent project evicted first");
        Check(cache::Inspect().totalBytes<=64*MiB,"unified quota is enforced");cache::ResetScene();

        root=Case(output,L"pin");Check(Store(1,32*MiB)&&Store(2,32*MiB),"fill protected fixture");cache::ResetScene();Project(a,{1});Check(cache::Pin(a.wstring(),true),"pin project");
        cache::BeginProject(b.c_str());cache::Touch(Entry(2));cache::ProjectOpened(b.c_str());
        Check(!Store(3,8*MiB)&&fs::exists(Entry(1))&&fs::exists(Entry(2)),"pinned and current cache stop new writes when quota cannot be freed");cache::ResetScene();
        Check(cache::Pin(a.wstring(),false),"unpin project");Check(Store(3,8*MiB),"unpin makes room reclaimable");cache::ResetScene();

        root=Case(output,L"manual");settings=cache::Current();settings.automaticCleanup=false;Check(cache::Configure(settings,error),"turn automatic cleanup off");
        Check(Store(1,32*MiB)&&Store(2,32*MiB),"fill manual-only cache");cache::ResetScene();
        Check(!Store(3,MiB)&&fs::exists(Entry(1))&&fs::exists(Entry(2)),"manual-only mode never evicts automatically");
        Check(cache::Clean(cache::Cleanup::Reclaimable).files==2,"manual cleanup works with automatic cleanup disabled");
        const auto unknown=root/L"Diagnostics"/L"keep.txt",source=output/L"source.pmm",foreign=root/L"Textures"/L"notes.txt";
        Bytes(unknown,128,false);Bytes(source,128,false);Bytes(foreign,128,false);Check(Store(4,MiB),"managed cache alongside non-cache files");cache::ResetScene();cache::Clean(cache::Cleanup::All);
        Check(fs::exists(unknown)&&fs::exists(source)&&fs::exists(foreign),"cleanup never traverses diagnostic folders or deletes project/source files");
        const auto link=root/L"Textures"/L"linked",external=output/L"external";fs::create_directories(external);
        if(CreateSymbolicLinkW(link.c_str(),external.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)) {
            Check(!cache::Managed(link/(std::wstring(64,L'a')+L".mtc")),"reparse-point cache paths are rejected");
        } else std::puts("SKIP directory symlink: host did not grant creation");

        root=Case(output,L"cross-process");Check(Store(1,MiB),"create cross-process lease entry");cache::ResetScene();
        const auto prefix=L"Local\\MMS-cache-test-"+std::to_wstring(GetCurrentProcessId());
        const auto readyName=prefix+L"-ready",releaseName=prefix+L"-release";
        HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,readyName.c_str()),release=CreateEventW(nullptr,FALSE,FALSE,releaseName.c_str());
        auto child=Child(L"--lease "+Quote(root.wstring())+L" "+Quote(Entry(1).wstring())+L" "+Quote(readyName)+L" "+Quote(releaseName));
        Check(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0,"other process acquired lease");
        const bool protectedByChild=cache::Clean(cache::Cleanup::All).files==0 && fs::exists(Entry(1));
        SetEvent(release);Join(child);CloseHandle(ready);CloseHandle(release);
        Check(protectedByChild,"other process's in-use cache survives manual cleanup");
        Check(cache::Clean(cache::Cleanup::All).files==1,"cross-process lease is released on shutdown");

        root=Case(output,L"concurrent");std::vector<PROCESS_INFORMATION> workers;
        for(unsigned i=1;i<=3;++i)workers.push_back(Child(L"--store "+Quote(root.wstring())+L" "+std::to_wstring(i)));
        for(auto& worker:workers)Join(worker);
        Check(cache::Inspect().totalBytes>=32*MiB&&cache::Inspect().totalBytes<=64*MiB,"concurrent publications respect one installation quota");

        root=Case(output,L"failed-load");cache::BeginProject(a.c_str());Check(Store(1,MiB),"cache created during tentative project load");cache::EndProjectLoad();
        Check(cache::Inspect().projects.empty(),"failed load does not publish a recent project association");
        Check(cache::Clean(cache::Cleanup::Reclaimable).files==1,"failed load releases temporary leases");

        root=Case(output,L"invalid");Check(Store(1,MiB),"create validated-invalid fixture");cache::ResetScene();Project(a,{1});
        cache::Invalidate(Entry(1));
        Check(cache::Inspect().reclaimableBytes==MiB,"invalid entry is reclaimable even for a recent project");
        Check(cache::Clean(cache::Cleanup::Reclaimable).files==1,"invalid entry is cleaned before recent preference");

        root=Case(output,L"corrupt-index");Check(Store(1,MiB),"create index recovery fixture");cache::ResetScene();
        {std::ofstream broken(root/L"Management"/L"index.bin",std::ios::binary|std::ios::trunc);broken<<"broken-index";}
        Check(!cache::Inspect().error.empty(),"damaged index reports failure rather than trusting missing ownership");
        Check(!Store(2,MiB)&&cache::Clean(cache::Cleanup::Reclaimable).files==0&&fs::exists(Entry(1)),"damaged index stops automatic and ordinary cleanup");
        Check(cache::Clean(cache::Cleanup::All).files==1&&cache::Inspect().error.empty(),"explicit clear-all safely recovers a damaged index");
        std::printf("PASS cache manager: %u checks\n",checks);return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
