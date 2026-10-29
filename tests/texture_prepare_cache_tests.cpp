#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/texture_prepare_cache.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace cache = mikumikustudio::texture_prepare_cache;
using Fn = mikumikustudio::d3dx::FnCreateTexFromFileExW;
using Bytes = std::vector<unsigned char>;
struct Info { UINT width, height, depth, levels, format, type, fileFormat; };
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s error=%lu\n", message, GetLastError()); std::exit(1); }
}
void Write(const fs::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Check(bool(stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())), "write owned fixture");
}
Bytes Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}
Bytes Tga() {
    constexpr UINT w=37, h=25;
    Bytes bytes(18+w*h*4);
    bytes[2]=2; bytes[12]=w; bytes[14]=h; bytes[16]=32; bytes[17]=0x28;
    for (UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) {
        auto* p=bytes.data()+18+(y*w+x)*4;
        p[0]=static_cast<unsigned char>(x*29+y*5);p[1]=static_cast<unsigned char>(x*3+y*19);
        p[2]=static_cast<unsigned char>(x*13+y*7);p[3]=static_cast<unsigned char>(x*11+y*17);
    }
    return bytes;
}
struct Texture { ComPtr<IDirect3DTexture9> value; Info info{}; HRESULT result=E_FAIL; };
Texture Load(Fn fn, IDirect3DDevice9* device, const fs::path& path, bool cached, DWORD usage) {
    Texture t;
    t.result=cached ? cache::Create(fn,device,path.c_str(),UINT_MAX,UINT_MAX,1,usage,D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,UINT_MAX,UINT_MAX,0,&t.info,nullptr,&t.value) :
        fn(device,path.c_str(),UINT_MAX,UINT_MAX,1,usage,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
           UINT_MAX,UINT_MAX,0,&t.info,nullptr,&t.value);
    return t;
}
Bytes Pixels(IDirect3DTexture9* texture) {
    D3DSURFACE_DESC d{};Check(SUCCEEDED(texture->GetLevelDesc(0,&d)),"texture description");
    Bytes bytes(std::size_t(d.Width)*d.Height*4);
    D3DLOCKED_RECT r{};Check(SUCCEEDED(texture->LockRect(0,&r,nullptr,D3DLOCK_READONLY)),"read texture");
    for(UINT y=0;y<d.Height;++y) std::memcpy(bytes.data()+std::size_t(y)*d.Width*4,
        static_cast<const unsigned char*>(r.pBits)+std::size_t(y)*r.Pitch,d.Width*4);
    Check(SUCCEEDED(texture->UnlockRect(0)),"unlock read texture");return bytes;
}
Bytes Render(IDirect3DDevice9* device, IDirect3DTexture9* texture, float bias) {
    ComPtr<IDirect3DSurface9> old,target,read;
    Check(SUCCEEDED(device->GetRenderTarget(0,&old)),"old target");
    Check(SUCCEEDED(device->CreateRenderTarget(64,64,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr)),"test target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)),"test readback");
    device->SetRenderTarget(0,target.Get()); device->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 viewport{0,0,64,64,0,1};device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    device->SetTexture(0,texture);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
    device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);
    device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);
    device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
    DWORD bits=0;std::memcpy(&bits,&bias,4);device->SetSamplerState(0,D3DSAMP_MIPMAPLODBIAS,bits);
    struct Vertex{float x,y,z,rhw,u,v;};
    const Vertex vertices[]={{-.5f,-.5f,0,1,0,0},{63.5f,-.5f,0,1,4,0},{-.5f,63.5f,0,1,0,4},{63.5f,63.5f,0,1,4,4}};
    device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
    Check(SUCCEEDED(device->BeginScene()),"begin texture draw");
    Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex))),"texture draw");
    Check(SUCCEEDED(device->EndScene()),"end texture draw");device->SetTexture(0,nullptr);
    Check(SUCCEEDED(device->GetRenderTargetData(target.Get(),read.Get())),"read drawn pixels");
    D3DLOCKED_RECT locked{};Check(SUCCEEDED(read->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"lock drawn pixels");
    Bytes bytes(64*64*4);
    for(UINT y=0;y<64;++y)std::memcpy(bytes.data()+y*64*4,
        static_cast<const unsigned char*>(locked.pBits)+std::size_t(y)*locked.Pitch,64*4);
    read->UnlockRect();device->SetRenderTarget(0,old.Get());return bytes;
}
void Equivalent(IDirect3DDevice9* device,const Texture& a,const Texture& b) {
    Check(a.result==b.result && a.value && b.value,"native and cached texture load");
    Check(a.value.Get()!=b.value.Get(),"independent texture objects");
    Check(std::memcmp(&a.info,&b.info,sizeof(Info))==0,"original source image metadata");
    D3DSURFACE_DESC da{},db{};a.value->GetLevelDesc(0,&da);b.value->GetLevelDesc(0,&db);
    Check(std::memcmp(&da,&db,sizeof da)==0,"original resized dimensions, format, pool and usage");
    Check(a.value->GetLevelCount()==b.value->GetLevelCount() &&
        a.value->GetAutoGenFilterType()==b.value->GetAutoGenFilterType(),"mipmap descriptor and filter");
    Check(Pixels(a.value.Get())==Pixels(b.value.Get()),"all resized pixels and alpha match exactly");
    for(float bias:{0.f,1.5f,4.f}) Check(Render(device,a.value.Get(),bias)==Render(device,b.value.Get(),bias),
        "rendered texture including autogenerated mipmaps matches exactly");
}
std::vector<fs::path> Entries(const fs::path& root) {
    std::vector<fs::path> files;
    for(const auto& f:fs::recursive_directory_iterator(root))if(f.path().extension()==L".mtc")files.push_back(f.path());
    return files;
}
int wmain(int argc,wchar_t** argv) {
    Check(argc>=3,"usage: output-root D3DX-DLL [real textures]");
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const fs::path root=fs::absolute(argv[1]);Check(!fs::exists(root),"new isolated output directory");
    fs::create_directories(root);
    const auto cacheRoot=root/L"Decoded";fs::create_directories(cacheRoot);
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",cacheRoot.c_str());
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE",nullptr);
    const auto dll=LoadLibraryW(argv[2]);Check(dll!=nullptr,"load selected D3DX");
    const auto fn=reinterpret_cast<Fn>(GetProcAddress(dll,"D3DXCreateTextureFromFileExW"));Check(fn!=nullptr,"D3DX file loader");
    ComPtr<IDirect3D9> d3d;d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));Check(bool(d3d),"D3D9");
    const auto window=CreateWindowW(L"STATIC",L"MMS private texture cache test",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
    ComPtr<IDirect3DDevice9> device;
    Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)),"hidden HAL device");
    const auto source=root/L"夕空_测试.tga";auto fixture=Tga();Write(source,fixture);
    for(DWORD usage:{0ul,DWORD(D3DUSAGE_AUTOGENMIPMAP)}){
        auto native=Load(fn,device.Get(),source,false,usage);Check(SUCCEEDED(native.result),"native fixture supported");
        const auto before=cache::GetStatistics();
        auto cold=Load(fn,device.Get(),source,true,usage);Equivalent(device.Get(),native,cold);
        const auto seeded=cache::GetStatistics();
        Check(seeded.stores==before.stores+1 && seeded.nativeCalls==before.nativeCalls+1,"cold texture stored once");
        auto hit=Load(fn,device.Get(),source,true,usage);Equivalent(device.Get(),native,hit);
        const auto reused=cache::GetStatistics();
        Check(reused.hits==seeded.hits+1 && reused.nativeCalls==seeded.nativeCalls,"disk hit skips native decode");
        D3DLOCKED_RECT changed{};Check(SUCCEEDED(hit.value->LockRect(0,&changed,nullptr,0)),"edit independent cached texture");
        static_cast<unsigned char*>(changed.pBits)[0]^=0xff;hit.value->UnlockRect(0);
        auto fresh=Load(fn,device.Get(),source,true,usage);Equivalent(device.Get(),native,fresh);
        std::printf("PASS usage=%lu exact pixels, metadata, independent ownership and mip rendering\n",usage);
    }
    const auto entries=Entries(cacheRoot);Check(entries.size()==2,"two parameter variants");
    for(const auto& f:entries){auto bytes=Read(f);Check(bytes.size()>100,"full cache entry");bytes.back()^=0x80;Write(f,bytes);}
    const auto beforeCorrupt=cache::GetStatistics();
    auto repaired=Load(fn,device.Get(),source,true,0);
    auto native=Load(fn,device.Get(),source,false,0);Equivalent(device.Get(),native,repaired);
    Check(cache::GetStatistics().nativeCalls==beforeCorrupt.nativeCalls+1,"corrupt cache recompiles through native decoder");
    const auto oldTime=fs::last_write_time(source);fixture[29]^=0x7f;Write(source,fixture);fs::last_write_time(source,oldTime);
    const auto beforeEdit=cache::GetStatistics();
    auto changed=Load(fn,device.Get(),source,true,0);auto changedNative=Load(fn,device.Get(),source,false,0);
    Equivalent(device.Get(),changedNative,changed);
    Check(cache::GetStatistics().nativeCalls==beforeEdit.nativeCalls+1,"content edit with restored mtime invalidates cache");
    const auto blocker=root/L"not-a-directory";Write(blocker,Bytes{1});
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",(blocker/L"cache").c_str());
    auto blocked=Load(fn,device.Get(),source,true,0);Equivalent(device.Get(),changedNative,blocked);
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",cacheRoot.c_str());
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE",L"1");
    const auto beforeDisabled=cache::GetStatistics();auto disabled=Load(fn,device.Get(),source,true,0);Equivalent(device.Get(),changedNative,disabled);
    Check(cache::GetStatistics().hits==beforeDisabled.hits && cache::GetStatistics().nativeCalls==beforeDisabled.nativeCalls+1,"disabled path calls native decoder");
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DISABLE",nullptr);
    const auto secondWindow=CreateWindowW(L"STATIC",L"MMS alternate device test",WS_OVERLAPPEDWINDOW,
        0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    pp.hDeviceWindow=secondWindow;
    ComPtr<IDirect3DDevice9> otherDevice;
    Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,secondWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE|D3DCREATE_MULTITHREADED,
        &pp,&otherDevice)),"alternate device creation parameters");
    const auto beforeDevice=cache::GetStatistics();
    auto other=Load(fn,otherDevice.Get(),source,true,0);
    auto otherNative=Load(fn,otherDevice.Get(),source,false,0);
    Equivalent(otherDevice.Get(),otherNative,other);
    Check(cache::GetStatistics().nativeCalls==beforeDevice.nativeCalls+1,
        "device creation flags have their own decoded cache key");
    other.value.Reset();otherNative.value.Reset();otherDevice.Reset();DestroyWindow(secondWindow);
    // DDS files can supply an authored mip chain. The native autogen request
    // must keep its own behavior instead of rebuilding that chain from level 0.
    ComPtr<IDirect3DTexture9> authored;
    Check(SUCCEEDED(device->CreateTexture(8,8,2,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&authored,nullptr)),
        "create authored mip fixture");
    for(UINT level=0;level<2;++level){
        D3DSURFACE_DESC desc{};Check(SUCCEEDED(authored->GetLevelDesc(level,&desc)),"authored mip descriptor");
        D3DLOCKED_RECT pixels{};Check(SUCCEEDED(authored->LockRect(level,&pixels,nullptr,0)),"fill authored mip");
        for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x)
            reinterpret_cast<DWORD*>(static_cast<unsigned char*>(pixels.pBits)+y*pixels.Pitch)[x]=
                level ? 0xff1267a2u : (0xffe13842u ^ (x*31+y*19));
        Check(SUCCEEDED(authored->UnlockRect(level)),"unlock authored mip");
    }
    using SaveTexture=HRESULT(WINAPI*)(LPCWSTR,int,IDirect3DBaseTexture9*,const PALETTEENTRY*);
    const auto saveTexture=reinterpret_cast<SaveTexture>(GetProcAddress(dll,"D3DXSaveTextureToFileW"));
    const auto authoredFile=root/L"authored-mips.dds";
    Check(saveTexture && SUCCEEDED(saveTexture(authoredFile.c_str(),4,authored.Get(),nullptr)),"save authored DDS fixture");
    authored.Reset();
    const auto authoredNative=Load(fn,device.Get(),authoredFile,false,D3DUSAGE_AUTOGENMIPMAP);
    Check(SUCCEEDED(authoredNative.result) && authoredNative.info.levels==2,"native authored DDS metadata");
    const auto beforeAuthored=cache::GetStatistics();
    const auto authoredFirst=Load(fn,device.Get(),authoredFile,true,D3DUSAGE_AUTOGENMIPMAP);
    const auto authoredSecond=Load(fn,device.Get(),authoredFile,true,D3DUSAGE_AUTOGENMIPMAP);
    Equivalent(device.Get(),authoredNative,authoredFirst);Equivalent(device.Get(),authoredNative,authoredSecond);
    const auto afterAuthored=cache::GetStatistics();
    Check(afterAuthored.stores==beforeAuthored.stores && afterAuthored.hits==beforeAuthored.hits &&
        afterAuthored.nativeCalls==beforeAuthored.nativeCalls+2,"authored mip chain stays on native autogen path");
    std::puts("PASS authored DDS mip chain preserves native autogen behavior");
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1");
    const auto beforeMemoryOnly=cache::GetStatistics();
    const auto memoryOnly=Load(fn,device.Get(),source,true,0);
    Equivalent(device.Get(),changedNative,memoryOnly);
    Check(cache::GetStatistics().nativeCalls==beforeMemoryOnly.nativeCalls+1 &&
        cache::GetStatistics().hits==beforeMemoryOnly.hits &&
        cache::GetStatistics().stores==beforeMemoryOnly.stores,
        "RAM-only texture path preserves native loading without prepared disk data");
    const auto absentCache=root/L"ram-only-no-disk";
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",absentCache.c_str());
    const auto noDisk=Load(fn,device.Get(),source,true,0);Equivalent(device.Get(),changedNative,noDisk);
    Check(!fs::exists(absentCache),"RAM-only mode never creates a texture cache directory");
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr);
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",cacheRoot.c_str());
    // Cold CPU preparation must match the real original file decoder, not
    // merely its own serial output. Both 32 and 43 run this same comparison.
    const auto parallelRoot=root/L"Parallel";
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",parallelRoot.c_str());
    std::vector<std::wstring> preparePaths{source.wstring(),source.wstring(),authoredFile.wstring()};
    for(unsigned origin:{0u,16u,32u,48u}) for(unsigned components:{3u,4u}) for(bool rle:{false,true}) {
        auto bytes=Tga();const auto raw=bytes;
        bytes.resize(18);bytes[16]=static_cast<unsigned char>(components*8);
        bytes[17]=static_cast<unsigned char>(origin+(components==4 ? 8 : 0));bytes[2]=rle ? 10 : 2;
        for(unsigned i=0;i<37*25;){
            const unsigned run=rle && i%7==0 && 37*25-i>=3 && 37-i%37>=3 ? 3 : 1;
            if(rle)bytes.push_back(run==3 ? 0x82 : 0);
            bytes.insert(bytes.end(),raw.begin()+18+i*4,raw.begin()+18+i*4+components);
            i+=run;
        }
        const auto path=root/(L"origin-"+std::to_wstring(origin)+L"-"+std::to_wstring(components)+
            (rle ? L"-rle.tga" : L"-raw.tga"));Write(path,bytes);preparePaths.push_back(path.wstring());
    }
    const auto png=root/L"truecolour-alpha.png";
    Check(SUCCEEDED(saveTexture(png.c_str(),3,changedNative.value.Get(),nullptr)),"create native PNG fixture");
    preparePaths.push_back(png.wstring());
    for (int i=3;i<argc;++i) preparePaths.push_back(fs::absolute(argv[i]).wstring());
    const auto beforeParallel=cache::GetParallelStatistics();
    {
        cache::Batch batch(fn,device.Get(),preparePaths);
        for (const auto& path:preparePaths) for(DWORD usage:{0ul,DWORD(D3DUSAGE_AUTOGENMIPMAP)}) {
            auto expected=Load(fn,device.Get(),path,false,usage);
            auto actual=Load(fn,device.Get(),path,true,usage);
            if(expected.result!=actual.result || !expected.value || !actual.value)
                std::fwprintf(stderr,L"CASE %s usage=%lu native=%08lx prepared=%08lx objects=%u/%u\n",
                    path.c_str(),usage,expected.result,actual.result,bool(expected.value),bool(actual.value));
            Equivalent(device.Get(),expected,actual);
        }
        HANDLE writer=CreateFileW(source.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(writer==INVALID_HANDLE_VALUE,"staged source cannot change during preparation");
    }
    const auto afterParallel=cache::GetParallelStatistics();
    Check(afterParallel.used>beforeParallel.used && afterParallel.decoded>beforeParallel.decoded,
          "production CPU workers supplied cold textures");
    Check(afterParallel.failures==beforeParallel.failures,"CPU preparation succeeds");
    // Destruction before consumption must join or cancel pending decodes.
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1");
    {cache::Batch abandoned(fn,device.Get(),preparePaths);}
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr);
    {
        const auto beforeWarm=cache::GetParallelStatistics();
        cache::Batch batch(fn,device.Get(),preparePaths);
        for(const auto& path:preparePaths){auto a=Load(fn,device.Get(),path,false,D3DUSAGE_AUTOGENMIPMAP);
            auto b=Load(fn,device.Get(),path,true,D3DUSAGE_AUTOGENMIPMAP);Equivalent(device.Get(),a,b);}
        const auto warm=cache::GetParallelStatistics();
        Check(warm.diskReady>beforeWarm.diskReady && warm.decoded==beforeWarm.decoded,
              "warm disk entries skip repeated CPU decoding");
    }
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1");
    const auto ramRoot=root/L"ParallelRamOnly";
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",ramRoot.c_str());
    {
        const auto beforeRam=cache::GetParallelStatistics();
        cache::Batch batch(fn,device.Get(),{source.wstring()});
        auto a=Load(fn,device.Get(),source,false,0),b=Load(fn,device.Get(),source,true,0);
        Equivalent(device.Get(),a,b);
        Check(cache::GetParallelStatistics().used==beforeRam.used+1,"CPU preparation works in memory-only mode");
        Check(!fs::exists(ramRoot),"parallel memory-only mode writes no texture cache");
    }
    SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr);
    SetEnvironmentVariableW(L"MMS_PARALLEL_PREPARE_DISABLE",L"1");
    {
        const auto beforeOff=cache::GetParallelStatistics();
        cache::Batch batch(fn,device.Get(),{source.wstring()});
        Check(cache::GetParallelStatistics().staged==beforeOff.staged,"parallel preparation can be disabled");
    }
    SetEnvironmentVariableW(L"MMS_PARALLEL_PREPARE_DISABLE",nullptr);
    SetEnvironmentVariableW(L"MMS_TEXTURE_CACHE_DIR",cacheRoot.c_str());
    const auto truncated=root/L"truncated.tga";auto shortImage=Tga();shortImage.pop_back();Write(truncated,shortImage);
    const auto crossLine=root/L"crossline.tga";auto cross=Tga();const auto sourcePixels=cross;
    cross.resize(18);cross[2]=10;
    for(unsigned i=0;i<37*25;){
        const unsigned run=i%7==0 && 37*25-i>=3 ? 3 : 1;
        cross.push_back(run==3 ? 0x82 : 0);
        cross.insert(cross.end(),sourcePixels.begin()+18+i*4,sourcePixels.begin()+18+i*4+4);i+=run;
    }
    Write(crossLine,cross);
    for(const auto& invalid:{truncated,crossLine}) {
        const auto beforeFailure=cache::GetParallelStatistics();
        cache::Batch batch(fn,device.Get(),{invalid.wstring()});
        auto expected=Load(fn,device.Get(),invalid,false,0);
        auto actual=Load(fn,device.Get(),invalid,true,0);
        Check(actual.result==expected.result && bool(actual.value)==bool(expected.value),
              "failed worker returns original file loader behavior");
        if(SUCCEEDED(expected.result))Equivalent(device.Get(),expected,actual);
        Check(cache::GetParallelStatistics().failures==beforeFailure.failures+1 &&
            cache::GetParallelStatistics().used==beforeFailure.used,"truncated decode falls back without using partial pixels");
    }
    std::puts("PASS production parallel workers, exact native output, stable source, deduplication, warm reuse and RAM-only");
    if(argc>3){
        std::vector<std::wstring> realPaths;
        for(int i=3;i<argc;++i)realPaths.push_back(fs::absolute(argv[i]).wstring());
        SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",L"1");
        for(char mode:std::string("ABBAABBA")){
            SetEnvironmentVariableW(L"MMS_PARALLEL_PREPARE_DISABLE",mode=='A' ? L"1" : L"0");
            const auto begin=std::chrono::steady_clock::now();
            {
                cache::Batch batch(fn,device.Get(),realPaths);
                for(const auto& path:realPaths){auto t=Load(fn,device.Get(),path,true,D3DUSAGE_AUTOGENMIPMAP);
                    Check(SUCCEEDED(t.result) && t.value,"parallel timing created every texture");}
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            std::printf("PARALLEL_ABBA mode=%c ms=%.6f\n",mode,ms);
        }
        SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY",nullptr);
        SetEnvironmentVariableW(L"MMS_PARALLEL_PREPARE_DISABLE",nullptr);
    }
    fs::remove(source);auto missing=Load(fn,device.Get(),source,true,0);Check(FAILED(missing.result) && !missing.value,"missing source never served from cache");
    std::puts("PASS corruption, mtime-preserving edits, unwritable directory, disable and missing source");
    for(int i=3;i<argc;++i){
        const fs::path path=argv[i];DWORD usage=D3DUSAGE_AUTOGENMIPMAP;
        auto original=Load(fn,device.Get(),path,false,usage);if(FAILED(original.result)){usage=0;original=Load(fn,device.Get(),path,false,usage);}
        Check(SUCCEEDED(original.result),"native real texture");auto seeded=Load(fn,device.Get(),path,true,usage);Equivalent(device.Get(),original,seeded);
        auto cached=Load(fn,device.Get(),path,true,usage);Equivalent(device.Get(),original,cached);
        std::fwprintf(stdout,L"REAL texture=%ls\n",path.c_str());
        for(char mode:std::string("ABBAABBA")){
            const auto start=std::chrono::steady_clock::now();auto texture=Load(fn,device.Get(),path,mode=='B',usage);
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            Check(SUCCEEDED(texture.result),"real texture timing load");std::printf("mode=%c ms=%.3f\n",mode,ms);
        }
    }
    const auto stats=cache::GetStatistics();std::printf("PASS texture cache hits=%llu native=%llu stores=%llu\n",stats.hits,stats.nativeCalls,stats.stores);
    device.Reset();DestroyWindow(window);return 0;
}
