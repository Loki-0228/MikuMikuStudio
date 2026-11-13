// Actual VfW decoding from immutable raw cache paths. No MMDApp, GPU, window,
// MF reader or FFmpeg process is created. Only --store's new directory is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <vfw.h>
#include "mikumikustudio/cache_files.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace files=mikumikustudio::cache_files;
namespace {
unsigned checks=0;
constexpr int width=16,height=12;
void Check(bool ok,const char* what){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",what);throw std::runtime_error(what);}}
void Result(files::Result actual,files::Result expected,const char* what){
    if(actual!=expected)std::fprintf(stderr,"cache result actual=%u expected=%u\n",unsigned(actual),unsigned(expected));
    Check(actual==expected,what);
}
void AviResult(HRESULT result,const char* what,const wchar_t* path=nullptr){
    if(result!=AVIERR_OK)std::fprintf(stderr,"VfW call=%s result=0x%08lX path=%ls\n",what,static_cast<unsigned long>(result),path?path:L"<existing reader>");
    Check(result==AVIERR_OK,what);
}
struct Runtime{Runtime(){AVIFileInit();}~Runtime(){AVIFileExit();}};
struct Reader {
    PAVIFILE file=nullptr;PAVISTREAM stream=nullptr;PGETFRAME frames=nullptr;
    Reader()=default;Reader(const Reader&)=delete;Reader& operator=(const Reader&)=delete;
    ~Reader(){Close();}
    void Close() noexcept {if(frames){AVIStreamGetFrameClose(frames);frames=nullptr;}
        if(stream){AVIStreamRelease(stream);stream=nullptr;}if(file){AVIFileRelease(file);file=nullptr;}}
    void Open(const std::wstring& path){
        Check(!file&&!stream&&!frames,"reader starts without previous VfW resources");
        // Intentionally pass the lease's exact volume-GUID path. No DOS-path
        // fallback, wrapper parser, copied shortcut or fake decode is allowed.
        AviResult(AVIFileOpenW(&file,path.c_str(),OF_SHARE_DENY_NONE,nullptr),"AVIFileOpenW raw cache GUID path",path.c_str());
        AviResult(AVIFileGetStream(file,&stream,streamtypeVIDEO,0),"AVIFileGetStream raw cache");
        AVISTREAMINFOW info{};AviResult(AVIStreamInfoW(stream,&info,sizeof info),"AVIStreamInfoW raw cache");
        Check(info.dwLength==2&&info.dwRate==30&&info.dwScale==1,"VfW retains two-frame AVI timing from raw payload");
        SetLastError(ERROR_SUCCESS);frames=AVIStreamGetFrameOpen(stream,nullptr);const DWORD error=GetLastError();
        if(!frames)std::fprintf(stderr,"AVIStreamGetFrameOpen failed error=%lu path=%ls\n",error,path.c_str());
        Check(frames!=nullptr,"open real VfW frame decoder from immutable raw lease");
    }
    std::vector<unsigned char> Frame(int frame){
        SetLastError(ERROR_SUCCESS);
        const auto* header=static_cast<const BITMAPINFOHEADER*>(AVIStreamGetFrame(frames,frame));const DWORD error=GetLastError();
        if(!header)std::fprintf(stderr,"AVIStreamGetFrame failed frame=%d error=%lu\n",frame,error);
        Check(header!=nullptr,"decode requested raw-cache AVI frame");
        Check(header->biSize>=sizeof(BITMAPINFOHEADER)&&header->biWidth==width&&
            (header->biHeight==height||header->biHeight==-height)&&header->biPlanes==1&&
            (header->biBitCount==24||header->biBitCount==32)&&header->biCompression==BI_RGB&&!header->biClrUsed,
            "decoded VfW frame is a real uncompressed RGB image with expected dimensions");
        const auto pixelBytes=unsigned(header->biBitCount/8);
        const auto stride=((unsigned(width)*header->biBitCount+31)/32)*4;
        const auto* pixels=reinterpret_cast<const unsigned char*>(header)+header->biSize;
        std::vector<unsigned char> canonical(width*height*3);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const auto row=header->biHeight>0?height-1-y:y;
            const auto* pixel=pixels+size_t(row)*stride+size_t(x)*pixelBytes;
            auto* out=canonical.data()+size_t(y*width+x)*3;
            std::copy_n(pixel,3,out);
            Check(pixel[0]==20+frame*150&&pixel[1]==32+y*2&&pixel[2]==48+x,
                "actual decoded BGR pixels match the requested distinct AVI frame");
        }
        return canonical;
    }
};
std::vector<unsigned char> Pixels(int frame){
    std::vector<unsigned char> pixels(width*height*3);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto* pixel=pixels.data()+size_t((height-1-y)*width+x)*3;
        pixel[0]=static_cast<unsigned char>(20+frame*150);pixel[1]=static_cast<unsigned char>(32+y*2);pixel[2]=static_cast<unsigned char>(48+x);}
    return pixels;
}
void CreateAvi(const fs::path& source){
    Check(!fs::exists(source),"new source AVI does not replace any existing file");
    Reader writer;AviResult(AVIFileOpenW(&writer.file,source.c_str(),OF_WRITE|OF_CREATE,nullptr),"create private source AVI",source.c_str());
    AVISTREAMINFOW info{};info.fccType=streamtypeVIDEO;info.dwScale=1;info.dwRate=30;info.rcFrame={0,0,width,height};
    AviResult(AVIFileCreateStreamW(writer.file,&writer.stream,&info),"create raw RGB AVI stream");
    BITMAPINFOHEADER format{};format.biSize=sizeof format;format.biWidth=width;format.biHeight=height;
    format.biPlanes=1;format.biBitCount=24;format.biCompression=BI_RGB;format.biSizeImage=width*height*3;
    AviResult(AVIStreamSetFormat(writer.stream,0,&format,sizeof format),"set AVI RGB24 format");
    for(int frame=0;frame<2;++frame){const auto pixels=Pixels(frame);LONG samples=0,bytes=0;
        AviResult(AVIStreamWrite(writer.stream,frame,1,const_cast<unsigned char*>(pixels.data()),LONG(pixels.size()),AVIIF_KEYFRAME,&samples,&bytes),"write distinct raw AVI frame");
        Check(samples==1&&bytes==LONG(pixels.size()),"source writer accepted one complete frame");}
}
void Run(const fs::path& root){
    Check(root.is_absolute()&&root==root.lexically_normal()&&fs::is_directory(root.parent_path()),"store is a new absolute test directory under an existing parent");
    Check(CreateDirectoryW(root.c_str(),nullptr)!=FALSE,"claim test store exclusively without deleting or reusing existing data");
    Runtime runtime;const auto source=root/L"original two frames.AvI";CreateAvi(source);
    const auto bytes=fs::file_size(source);Check(bytes>0&&bytes<1024*1024,"fixture AVI is small and complete");
    files::Directory directory;Result(files::Directory::Open(root/L"raw-cache",true,directory),files::Result::Ok,"open private raw cache directory");
    files::RawSourceWrite prepared;files::RawSourceCleanup cleanup;
    Result(directory.PrepareRawSource(source.c_str(),1024*1024,prepared,cleanup),files::Result::Ok,"prepare immutable encoded AVI source");
    Check(!cleanup.IsValid(),"successful preparation does not leave an exceptional cleanup receipt");
    files::RawSourceLease first;Result(prepared.Complete(first),files::Result::Ok,"complete and pin raw AVI without envelope");
    const auto info=first.Info();const auto path=first.Path();
    Check(info.name.size()==40&&info.name.substr(32)==L".raw.avi"&&
        std::all_of(info.name.begin(),info.name.begin()+32,[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),
        "raw filename keeps lower-case AVI extension after unique 32-hex identity");
    Check(info.payloadBytes==bytes&&info.identity.fileBytes==bytes,"raw cache file size equals original AVI, with no envelope");
    Check(path.rfind(L"\\\\?\\Volume{",0)==0&&fs::path(path).filename()==info.name,"decoder receives the lease's fixed absolute volume GUID path");
    std::printf("VfW cache path: %ls bytes=%llu\n",path.c_str(),static_cast<unsigned long long>(bytes));
    Check(DeleteFileW(source.c_str())!=FALSE&&!fs::exists(source),"remove original AVI before any cache decode");
    Reader reader;reader.Open(path);const auto zero=reader.Frame(0),one=reader.Frame(1);
    Check(zero!=one&&reader.Frame(0)==zero,"real VfW changes frames and seeks back after original deletion");
    files::RawSourceLease second;Result(directory.AcquireRawSource(info,second),files::Result::Ok,"acquire independent reader lease for the same immutable AVI");
    Check(second.Path()==path&&second.Info().payloadDigest==info.payloadDigest,"leases share the exact stable pathname and digest");
    Reader another;another.Open(second.Path());Check(another.Frame(1)==one,"second live VfW reader decodes the same cached bytes");
    Result(directory.RemoveRawSource(info),files::Result::Busy,"active leases and readers prevent raw source removal");
    reader.Close();first=files::RawSourceLease{};
    Result(directory.RemoveRawSource(info),files::Result::Busy,"another lease and reader keep raw source pinned after first release");
    Check(another.Frame(0)==zero&&another.Frame(1)==one,"remaining decoder survives first lease and reader release");
    another.Close();Reader reopened;reopened.Open(second.Path());
    Check(reopened.Frame(1)==one&&reopened.Frame(0)==zero,"closed decoder reopens identical GUID cache path without original file");
    second=files::RawSourceLease{};
    Result(directory.RemoveRawSource(info),files::Result::Busy,"VfW reader alone prevents deletion until decoder resources close");
    Check(reopened.Frame(1)==one,"failed removal does not invalidate the active VfW reader");reopened.Close();
    Result(directory.RemoveRawSource(info),files::Result::DeletePending,"remove raw AVI only after every lease and decoder closes");
    std::vector<std::wstring> names;Result(directory.EnumerateAll(64,names),files::Result::Ok,"enumerate cache namespace after raw deletion");
    Check(std::find(names.begin(),names.end(),info.name)==names.end(),"deleted raw AVI is absent from cache namespace");
    SetLastError(ERROR_SUCCESS);const auto attributes=GetFileAttributesW(path.c_str());const auto missing=GetLastError();
    Check(attributes==INVALID_FILE_ATTRIBUTES&&(missing==ERROR_FILE_NOT_FOUND||missing==ERROR_PATH_NOT_FOUND),"raw GUID path no longer resolves after all readers release");
    files::RawSourceLease absent;Result(directory.AcquireRawSource(info,absent),files::Result::Missing,"removed identity cannot be reacquired");
}
}
int wmain(int argc,wchar_t** argv){std::setvbuf(stdout,nullptr,_IONBF,0);
    try{Check(argc==3&&std::wstring(argv[1])==L"--store","usage: --store NEW_ABSOLUTE_DIR");Run(fs::path(argv[2]));
        std::printf("PASS cache raw video checks=%u; only VfW actual GUID-path decoding and file lifetime; MF/FFmpeg not tested\n",checks);return 0;}
    catch(const std::exception& e){std::fprintf(stderr,"FAIL cache raw video checks=%u: %s\n",checks,e.what());return 1;}}
