// MAINTENANCE: regression-keep
// Real MF/FFmpeg decoding from consumed raw-source leases. Only copies of the
// supplied fixtures are removed. No product window, renderer or budget claims.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/cache_files.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs=std::filesystem;
namespace media=mikumikustudio::media;
namespace files=mikumikustudio::cache_files;
using files::Result;
namespace {
unsigned checks=0;
void Check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
void CheckVideo(bool ok,const std::wstring& error,const char* label){
    if(!ok)std::fwprintf(stderr,L"video error: %ls\n",error.c_str());Check(ok,label);
}
struct ProcessObservation{
    std::wstring inputArgument,forbiddenSource,forbiddenFixture,lastCommand;
    unsigned starts=0;bool wrongPath=false;
};
std::mutex observationMutex;
ProcessObservation* observation=nullptr;
std::atomic_bool observationFailed{false};
decltype(&CreateProcessW) realCreateProcess=nullptr;
BOOL WINAPI ObserveProcess(LPCWSTR application,LPWSTR command,LPSECURITY_ATTRIBUTES processSecurity,
    LPSECURITY_ATTRIBUTES threadSecurity,BOOL inherit,DWORD flags,LPVOID environment,LPCWSTR directory,
    LPSTARTUPINFOW startup,LPPROCESS_INFORMATION process)noexcept{
    const wchar_t* name=application;
    if(name){for(const auto* p=name;*p;++p)if(*p==L'\\'||*p==L'/')name=p+1;}
    const bool ffmpeg=name&&!_wcsicmp(name,L"ffmpeg.exe");
    // CreateProcess may modify its command buffer. Copy without allocation
    // before the real call, then observe only successful actual child creation.
    wchar_t saved[32768]{};bool captured=false;
    if(ffmpeg&&command){const size_t count=wcsnlen_s(command,32768);if(count<32768){std::wmemcpy(saved,command,count+1);captured=true;}}
    const BOOL result=realCreateProcess(application,command,processSecurity,threadSecurity,inherit,flags,environment,directory,startup,process);
    const DWORD error=GetLastError();
    if(result&&ffmpeg){try{
        std::lock_guard<std::mutex> lock(observationMutex);
        if(observation){++observation->starts;
            observation->wrongPath|=!captured||!std::wcsstr(saved,observation->inputArgument.c_str())||
                std::wcsstr(saved,observation->forbiddenSource.c_str())||std::wcsstr(saved,observation->forbiddenFixture.c_str());
            observation->lastCommand=saved;}
    }catch(...){observationFailed=true;}}
    SetLastError(error);return result;
}
struct ProcessHook{
    ProcessHook(){Check(MH_Initialize()==MH_OK,"initialize local process observation");
        Check(MH_CreateHook(reinterpret_cast<void*>(&CreateProcessW),reinterpret_cast<void*>(&ObserveProcess),
            reinterpret_cast<void**>(&realCreateProcess))==MH_OK,"observe actual CreateProcessW without replacing result");
        Check(MH_EnableHook(reinterpret_cast<void*>(&CreateProcessW))==MH_OK,"enable process observation in this test process");}
    ~ProcessHook(){MH_DisableHook(reinterpret_cast<void*>(&CreateProcessW));MH_Uninitialize();}
};
struct ProcessScope{
    ProcessObservation value;
    ProcessScope(const std::wstring& source,const fs::path& original,const fs::path& fixture){
        value.inputArgument=L" -i \""+source+L"\"";value.forbiddenSource=original.wstring();value.forbiddenFixture=fixture.wstring();
        std::lock_guard<std::mutex> lock(observationMutex);Check(!observation,"process observer scope is exclusive");observation=&value;
    }
    void Finish(bool requireFallback){
        unsigned starts=0;bool wrong=false;std::wstring command;
        {std::lock_guard<std::mutex> lock(observationMutex);observation=nullptr;starts=value.starts;wrong=value.wrongPath;command=value.lastCommand;}
        std::printf("FFMPEG actual_successful_starts=%u wrong_source=%u\n",starts,unsigned(wrong));
        if(!command.empty())std::fwprintf(stdout,L"FFMPEG last_command=%ls\n",command.c_str());
        Check(!observationFailed.load()&&!wrong,"every observed successful FFmpeg launch uses this captured source path and no original path");
        if(requireFallback&&starts<2)std::fprintf(stderr,"backend coverage unavailable: high10 requires at least two actual successful FFmpeg starts, got %u\n",starts);
        Check(!requireFallback||starts>=2,"high10 proves actual FFmpeg startup and seek reopening from captured bytes");
    }
    ~ProcessScope(){std::lock_guard<std::mutex> lock(observationMutex);if(observation==&value)observation=nullptr;}
};
void Claim(const fs::path& root){
    Check(root.is_absolute()&&root.has_filename()&&root!=root.root_path()&&root==root.lexically_normal(),"fresh absolute normalized fixture root required");
    Check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"exclusively create own video-source fixture directory");
}
std::vector<unsigned char> Bytes(const fs::path& path){
    std::ifstream stream(path,std::ios::binary);Check(bool(stream),"open owned or supplied fixture for reading");
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    Check(!stream.bad(),"complete fixture read");return bytes;
}
files::RawSourceLease Capture(files::Directory& cache,const fs::path& source){
    const auto size=fs::file_size(source);files::RawSourceWrite write;files::RawSourceCleanup cleanup;files::RawSourceLease lease;
    const auto prepared=cache.PrepareRawSource(source.c_str(),size,write,cleanup);
    Check(prepared==Result::Ok&&!cleanup.IsValid()&&write.IsValid(),"capture real encoded fixture with no cleanup debt");
    const auto completed=write.Complete(lease);
    if(completed!=Result::Ok){const auto aborted=write.Abort();std::fprintf(stderr,"failed source copy result=%u abort=%u\n",unsigned(completed),unsigned(aborted));}
    Check(completed==Result::Ok&&lease.IsValid()&&!write.IsValid(),"finish actual encoded source copy");return lease;
}
struct VideoOwner{
    // As in native_media_tests, NativeVideo uses this key only; it does not
    // dereference MMDApp or require a renderer/UI fixture.
    mikumikustudio::MMDApp* app=reinterpret_cast<mikumikustudio::MMDApp*>(std::uintptr_t(1));
    ~VideoOwner(){media::ReleaseVideo(app);}
};
void Frames(mikumikustudio::MMDApp* app,int width,int height,double duration,const wchar_t* label){
    Check(width==64&&height==48&&duration>=7.99&&duration<=8.01,"fixture dimensions and duration are preserved");
    std::wstring error;
    for(double seconds:{0.,1.1,.1,6.1,7.9,0.}){
        const auto* dib=media::VideoFrame(app,seconds,error);
        CheckVideo(dib!=nullptr,error,"real forward reverse long-distance EOF and restart seek");
        Check(dib->biSize==sizeof(BITMAPINFOHEADER)&&dib->biWidth==width&&dib->biHeight==height&&
            dib->biPlanes==1&&dib->biBitCount==32&&dib->biCompression==BI_RGB,"decoder publishes expected bottom-up RGB32 DIB");
        const auto* pixels=reinterpret_cast<const unsigned char*>(dib+1);
        const auto* top=pixels+((height-1-8)*width+8)*4;
        const auto* bottom=pixels+(8*width+8)*4;
        const unsigned channel=static_cast<unsigned>(seconds)%2==0?2:1;
        std::printf("FRAME t=%.1f top=%u,%u,%u bottom=%u,%u,%u\n",seconds,top[0],top[1],top[2],bottom[0],bottom[1],bottom[2]);
        Check(top[channel]>235&&top[0]<20&&top[channel==1?2:1]<20,"exact frame-time red/green top colour after source deletion");
        Check(bottom[0]>235&&bottom[1]<20&&bottom[2]<20,"bottom blue confirms RGB channel order and vertical orientation");
    }
    std::wprintf(L"PASS decoded source %ls\n",label);
}
void Reclaim(files::Directory& cache,const files::RawSourceInfo& info,const fs::path& root){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    bool dispositionAccepted=false;Result last=Result::Invalid;
    do{
        if(!dispositionAccepted){
            last=cache.RemoveRawSource(info);
            if(last==Result::DeletePending)dispositionAccepted=true;
            else if(last!=Result::Busy){std::fprintf(stderr,"source reclaim result=%u\n",unsigned(last));Check(false,"retired source removal only waits for actual outstanding readers");}
        }
        if(dispositionAccepted){
            const auto attributes=GetFileAttributesW((root/info.name).c_str());const auto error=GetLastError();
            if(attributes==INVALID_FILE_ATTRIBUTES&&error==ERROR_FILE_NOT_FOUND){Check(true,"retired decoder releases source and owned namespace becomes absent");return;}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }while(std::chrono::steady_clock::now()<deadline);
    std::fprintf(stderr,"source reclaim timed out result=%u accepted=%u\n",unsigned(last),unsigned(dispositionAccepted));
    Check(false,"retired decoder source is reclaimed within bounded wait");
}
std::vector<unsigned char> CurrentFrame(mikumikustudio::MMDApp* app){
    std::wstring error;const auto* dib=media::VideoFrame(app,0,error);CheckVideo(dib!=nullptr,error,"capture existing video frame before failed replacement");
    Check(dib->biWidth==64&&dib->biHeight==48&&dib->biBitCount==32,"existing video frame has fixture dimensions");
    const auto* begin=reinterpret_cast<const unsigned char*>(dib);return {begin,begin+sizeof(*dib)+64*48*4};
}
void FailedReplacement(VideoOwner& owner,files::Directory& cache,const fs::path& cacheRoot,const fs::path& corrupt){
    const auto before=CurrentFrame(owner.app);const auto revision=media::VideoFrameRevision(owner.app);
    int width=713,height=-19;double duration=53.25;std::wstring error=L"before";files::RawSourceLease invalid;
    Check(!media::OpenVideoFromSource(owner.app,std::move(invalid),width,height,duration,error)&&!invalid.IsValid()&&
        width==713&&height==-19&&duration==53.25,"invalid source consumes input without publishing dimensions or duration");
    Check(media::HasVideo(owner.app)&&media::VideoFrameRevision(owner.app)==revision&&CurrentFrame(owner.app)==before,
        "invalid source preserves existing video and exact current frame");
    auto broken=Capture(cache,corrupt);const auto info=broken.Info();
    Check(DeleteFileW(corrupt.c_str())!=FALSE,"delete only owned corrupt input before decode");
    error=L"before";
    Check(!media::OpenVideoFromSource(owner.app,std::move(broken),width,height,duration,error)&&!broken.IsValid()&&
        width==713&&height==-19&&duration==53.25&&!error.empty(),"actual decoder failure consumes candidate and preserves caller dimensions");
    Check(media::HasVideo(owner.app)&&media::VideoFrameRevision(owner.app)==revision&&CurrentFrame(owner.app)==before,
        "actual decoder failure does not replace existing video or displayed frame");
    Reclaim(cache,info,cacheRoot);
}
void Run(const fs::path& root,const fs::path& fixtures){
    Check(fixtures.is_absolute()&&fs::is_directory(fixtures),"existing absolute fixtures directory required");Claim(root);
    constexpr const wchar_t* names[]{L"corners.mp4",L"corners.mov",L"h264-high10.mp4"};
    for(const auto* name:names){
        const auto original=fixtures/name,copy=root/name;
        Check(fs::is_regular_file(original)&&CopyFileW(original.c_str(),copy.c_str(),TRUE),"copy supplied encoded fixture into fresh owned directory");
        Check(Bytes(copy)==Bytes(original),"owned source copy exactly matches supplied read-only fixture");
    }
    const auto corrupt=root/L"invalid.mp4";
    {std::ofstream out(corrupt,std::ios::binary);out<<"MMS owned invalid encoded video fixture\n";Check(bool(out),"create only own undecodable candidate");}
    const auto cacheRoot=root/L"video-cache";files::Directory cache;
    Check(files::Directory::Open(cacheRoot,true,cache)==Result::Ok,"open actual pinned raw source directory");
    VideoOwner owner;
    for(unsigned index=0;index<3;++index){
        const auto source=root/names[index];auto lease=Capture(cache,source);const auto info=lease.Info();
        // An additional acquisition is deliberately released before Open. The
        // core path below has no caller-owned source lease to hide a lifetime bug.
        {files::RawSourceLease verified;Check(cache.AcquireRawSource(info,verified)==Result::Ok&&verified.IsValid(),"metadata can independently verify and lease encoded source");}
        Check(DeleteFileW(source.c_str())!=FALSE&&!fs::exists(source),"delete only this test's original media copy");
        const std::wstring capturedPath=lease.Path();
        ProcessScope processes(capturedPath,source,fixtures/names[index]);
        int width=-1,height=-1;double duration=-1;std::wstring error;
        const bool opened=media::OpenVideoFromSource(owner.app,std::move(lease),width,height,duration,error);
        if(!opened)std::fwprintf(stderr,L"required fixture %ls failed (high10 requires functioning FFmpeg fallback): %ls\n",names[index],error.c_str());
        Check(opened&&!lease.IsValid()&&media::HasVideo(owner.app),"decoder takes sole source lease ownership through public internal entry");
        Check(cache.RemoveRawSource(info)==Result::Busy,"live Video itself pins encoded source against deletion without caller lease");
        Frames(owner.app,width,height,duration,names[index]);
        processes.Finish(index==2);
        if(index==0)FailedReplacement(owner,cache,cacheRoot,corrupt);
        // Leave a real decode pending; release may retire its worker before its
        // final pipe/reader closes. Reclaim polls only Busy, never ignores errors.
        CheckVideo(media::VideoFrame(owner.app,6.1,error,false)!=nullptr,error,"queue real asynchronous decode before video release");
        media::ReleaseVideo(owner.app);Check(!media::HasVideo(owner.app),"released source video leaves the active map");
        Reclaim(cache,info,cacheRoot);
    }
    // Original path API still uses the caller's read-only fixture path.
    int width=0,height=0;double duration=0;std::wstring error;
    CheckVideo(media::OpenVideo(owner.app,(fixtures/L"corners.mp4").c_str(),width,height,duration,error),error,"original video path entry remains functional");
    Frames(owner.app,width,height,duration,L"legacy path API");media::ReleaseVideo(owner.app);
    Check(!media::HasVideo(owner.app),"legacy path video closes");
    for(const auto* name:names)Check(fs::is_regular_file(fixtures/name),"supplied original fixtures remain present");
}
}
int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Check(argc==5&&std::wcscmp(argv[1],L"--store")==0&&std::wcscmp(argv[3],L"--fixtures")==0,
            "usage: --store NEW_ABSOLUTE_DIR --fixtures ABSOLUTE_FIXTURES_DIR");
        ProcessHook hook;Run(argv[2],argv[4]);std::printf("PASS native video source checks=%u\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL native video source checks=%u: %s\n",checks,e.what());return 1;}
    catch(...){std::fprintf(stderr,"FAIL native video source unexpected exception\n");return 2;}
}
