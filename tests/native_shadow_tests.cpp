// MAINTENANCE: regression-keep; hidden D3D9 native 9.32 parity and coverage checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/native_shadow_projection.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"

using namespace mikumikustudio::d3dx;
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned kSize=128;
void CheckVertexBounds() {
    using namespace mikumikustudio;
    unsigned checks = 0;
    const auto compare = [&](const void* data, std::size_t stride, std::size_t count) {
        NativeShadowBounds reference;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i=0; i<count; ++i) {
            float point[3]; std::memcpy(point,bytes+i*stride,sizeof(point));
            reference.Include(point);
        }
        const auto actual=NativeShadowVertexBounds(data,stride,count);
        if (std::memcmp(reference.minimum.data(),actual.minimum.data(),12) ||
            std::memcmp(reference.maximum.data(),actual.maximum.data(),12))
            throw std::runtime_error("vertex bounds differ from full scalar scan");
        ++checks;
    };
    for (std::size_t stride : {12u,16u,20u,32u,48u,96u}) for (unsigned offset : {0u,1u,3u}) {
        std::vector<unsigned char> data(stride*1025+offset);
        unsigned random=12345;
        for (unsigned i=0; i<1025; ++i) {
            float point[3];
            for (float& value : point) {random=random*1664525u+1013904223u;value=float(int(random%20001)-10000)/17;}
            if(i%13==0)point[i%3]=std::numeric_limits<float>::quiet_NaN();
            if(i%17==0)point[i%3]=INFINITY;
            if(i%19==0)point[i%3]=-INFINITY;
            std::memcpy(data.data()+offset+i*stride,point,12);
        }
        for(std::size_t count : {0u,1u,2u,7u,1024u,1025u})compare(data.data()+offset,stride,count);
        const float negative[4]={-0.f,-0.f,-0.f,INFINITY},positive[4]={0.f,0.f,0.f,NAN};
        std::memcpy(data.data()+offset,negative,(std::min)(stride,std::size_t(16)));
        std::memcpy(data.data()+offset+stride,positive,(std::min)(stride,std::size_t(16)));
        compare(data.data()+offset,stride,2);
        std::memcpy(data.data()+offset,positive,(std::min)(stride,std::size_t(16)));
        std::memcpy(data.data()+offset+stride,negative,(std::min)(stride,std::size_t(16)));
        compare(data.data()+offset,stride,2);
    }
    if(NativeShadowVertexBounds(nullptr,32,1).Valid() ||
       NativeShadowVertexBounds("",4,1).Valid() ||
       NativeShadowVertexBounds("",32,(std::numeric_limits<std::size_t>::max)()).Valid())
        throw std::runtime_error("invalid vertex span produced bounds");
    std::printf("PASS full vertex bounds: %u scalar parity checks\n",checks);
}
void CheckReferenceFit() {
    using namespace mikumikustudio;
    NativeShadowBounds bounds;
    const float low[3]={-6,0,-3}, high[3]={6,20,8};
    bounds.Include(low);bounds.Include(high);
    std::array<float,16> inside{};
    inside[0]=inside[5]=inside[10]=.01f;inside[14]=.25f;inside[15]=1;
    if (FitReferenceShadowProjection(inside,bounds)!=inside)
        throw std::runtime_error("unclipped native sampling grid changed");
    unsigned checked=0;
    for (float scale : {.01f,.1f,1.f,10.f}) for (float warp : {-1.f,0.f,.5f,1.f}) {
        auto matrix=inside;matrix[0]=matrix[5]=scale;matrix[10]=scale*.15f;
        matrix[7]=warp;matrix[12]=2;matrix[13]=-1;matrix[14]=-.2f;
        const auto fitted=FitReferenceShadowProjection(matrix,bounds);
        for (unsigned corner=0;corner<8;++corner) {
            const auto p=bounds.Corner(corner);double q[4]{};
            for (int a=0;a<4;++a)
                q[a]=fitted[12+a]+p[0]*double(fitted[a])+p[1]*double(fitted[4+a])+p[2]*double(fitted[8+a]);
            if (!(q[3]>0) || std::abs(q[0]/q[3])>=1 || std::abs(q[1]/q[3])>=1 ||
                !(q[2]/q[3]>0 && q[2]/q[3]<1))
                throw std::runtime_error("native projective coverage or positive W failed");
            ++checked;
        }
        if (!std::isfinite(MinimumShadowScale(fitted,bounds)))
            throw std::runtime_error("native projective sampling density is not finite");
    }
    std::printf("PASS native grid preservation and %u projective coverage corners\n",checked);
}
void CheckProjection() {
    CheckVertexBounds();
    CheckReferenceFit();
    using namespace mikumikustudio;
    if (ShadowSamplingFactor(.1f) != 4.f || ShadowSamplingFactor(.05f) != 1.f ||
        ShadowSamplingFactor(.00001f) != 1.f)
        throw std::runtime_error("shared finest range lost its 4x sampling demand");
    for (unsigned base : {2048u, 4096u}) {
        if (FittedShadowResolutionLimit(base, .2f, .2f, 16384) != base ||
            FittedShadowResolutionLimit(base, .2f, .1f, 16384) != base * 2 ||
            FittedShadowResolutionLimit(base, .2f, .001f, 16384) != base * 2 ||
            FittedShadowResolutionLimit(base, .2f, .001f, 2048) != 2048)
            throw std::runtime_error("fitted coverage density compensation or hardware limit failed");
    }
    std::puts("PASS finest sampling demand, coverage density compensation and hardware limits");
    const float focus[3] = {2, 10, -3};
    for (const auto& direction : std::vector<std::array<float, 3>>{
             {-.5f, -1, .5f}, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 0}}) {
        for (int mode : {1, 2}) {
            const auto matrix = mikumikustudio::NativeShadowProjection(focus, direction.data(), .01784f, mode);
            const auto project = [&](const std::array<float, 3>& point) {
                std::array<float, 3> result{};
                for (int axis = 0; axis < 3; ++axis)
                    result[axis] = matrix[12+axis] + point[0]*matrix[axis] +
                        point[1]*matrix[4+axis] + point[2]*matrix[8+axis];
                return result;
            };
            // The centre and all sides of a small object must be covered,
            // including lights parallel to either candidate up direction.
            for (int axis = 0; axis < 3; ++axis) for (float offset : {-4.f, 0.f, 4.f}) {
                std::array<float, 3> point{focus[0], focus[1], focus[2]}; point[axis] += offset;
                const auto p = project(point);
                if (!std::isfinite(p[0]+p[1]+p[2]) || std::abs(p[0]) >= 1 || std::abs(p[1]) >= 1 || p[2] <= 0 || p[2] >= 1)
                    throw std::runtime_error("shadow projection clips the focused object");
            }
            if (direction[0] == 0 && direction[1] == 0 && direction[2] == 0) continue;
            const auto front = project({focus[0], focus[1], focus[2]});
            const auto back = project({focus[0]+direction[0], focus[1]+direction[1], focus[2]+direction[2]});
            if (std::abs(front[0]-back[0]) > 1e-6f || std::abs(front[1]-back[1]) > 1e-6f || front[2] >= back[2])
                throw std::runtime_error("occluder and receiver disagree along a light ray");
        }
    }
    std::puts("PASS focused coverage and directional occluder/receiver projection");
    mikumikustudio::NativeShadowBounds bounds;
    const float lower[3]{-20, -5, -130}, upper[3]{35, 48, 160};
    bounds.Include(lower); bounds.Include(upper);
    const float panned[3]{300, -120, 90};
    unsigned covered = 0;
    for (const auto& direction : std::vector<std::array<float, 3>>{
             {-.5f, -1, .5f}, {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 0}}) {
        for (int mode : {1, 2}) for (float range : {.1f, .01125f, .00001f}) {
            const auto projection = mikumikustudio::NativeShadowProjection(focus, direction.data(), range, mode, &bounds);
            if (projection != mikumikustudio::NativeShadowProjection(panned, direction.data(), range, mode, &bounds))
                throw std::runtime_error("camera pan moved fitted shadow coverage");
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto point = bounds.Corner(corner);
                float p[3]{};
                for (int axis = 0; axis < 3; ++axis)
                    p[axis] = projection[12+axis] + point[0]*projection[axis] +
                        point[1]*projection[4+axis] + point[2]*projection[8+axis];
                if (!std::isfinite(p[0]+p[1]+p[2]) || std::abs(p[0]) >= .99f || std::abs(p[1]) >= .99f || p[2] <= .01f || p[2] >= .99f)
                    throw std::runtime_error("fitted shadow projection clips geometry or filter margin");
                ++covered;
            }
        }
    }
    std::printf("PASS %u fitted coverage corners, both modes, depth, extreme precision and camera pan\n", covered);
}
void Check(HRESULT hr,const char* operation) {
    if(FAILED(hr)){std::printf("%s: 0x%08lx\n",operation,static_cast<unsigned long>(hr));throw std::runtime_error(operation);}
}
struct Vertex {float p[3],n[3],uv[2];};
struct CoverageProbe {
    float offset[3];
    bool specular = false;
};
struct Effect {
    void* value=nullptr;
    Effect()=default;
    Effect(const Effect&)=delete;
    Effect(Effect&& other)noexcept:value(other.value){other.value=nullptr;}
    ~Effect(){if(value)static_cast<IUnknown*>(value)->Release();}
};
Effect Load(IDirect3DDevice9* device,const std::filesystem::path& file) {
    std::ifstream stream(file,std::ios::binary);
    if(!stream)throw std::runtime_error("Cannot read production shader");
    const std::string source{std::istreambuf_iterator<char>(stream),{}};
    static HMODULE module=LoadLibraryW(L"d3dx9_43.dll");
    using Create=HRESULT(WINAPI*)(IDirect3DDevice9*,const void*,UINT,const void*,const void*,DWORD,void*,void**,void**);
    const auto create=reinterpret_cast<Create>(GetProcAddress(module,"D3DXCreateEffect"));
    if(!create)throw std::runtime_error("D3DXCreateEffect unavailable");
    Effect fx;void* errors=nullptr;
    const auto hr=create(device,source.data(),UINT(source.size()),nullptr,nullptr,0,nullptr,&fx.value,&errors);
    if(errors){using Data=void*(WINAPI*)(void*);std::printf("%s\n",static_cast<const char*>(EffectMethod<Data,3>(errors)(errors)));static_cast<IUnknown*>(errors)->Release();}
    Check(hr,"Compile production shader");return fx;
}
std::vector<DWORD> Draw(IDirect3DDevice9* device,void* effect,unsigned mapSize,int mode,float slope,bool split,bool legacy,float depthGap=0,int pattern=0,float phase=0,const CoverageProbe* coverage=nullptr,float facing=1) {
    ComPtr<IDirect3DTexture9> shadow;
    Check(device->CreateTexture(mapSize,mapSize,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&shadow,nullptr),"Create shadow texture");
    D3DLOCKED_RECT pixels{};Check(shadow->LockRect(0,&pixels,nullptr,0),"Lock shadow texture");
    for(unsigned y=0;y<mapSize;++y)for(unsigned x=0;x<mapSize;++x){
        auto* row=reinterpret_cast<float*>(static_cast<char*>(pixels.pBits)+y*pixels.Pitch);
        row[x]=split?(x<mapSize/2?.2f:.8f):.5f+slope*(2.f*x/mapSize-1.f)-depthGap;
        if(pattern==1)row[x]=float(x)<mapSize*.5f+(float(y)-mapSize*.5f)*.35f?.2f:.8f;
        if(pattern==2)row[x]=(x==mapSize/2)?.2f:.8f;
        if(coverage)row[x]=1; // No occluder: only material lighting may shade this receiver.
    }
    Check(shadow->UnlockRect(0),"Unlock shadow texture");
    D3DXMATRIXF identity{};for(unsigned i=0;i<4;++i)identity.m[i][i]=1;
    Check(FxSetMatrix(effect,"matWorldViewProj",&identity),"Set camera projection");
    auto lightProjection=identity;lightProjection.m[3][0]=2*phase/mapSize;
    if(coverage)for(unsigned i=0;i<3;++i)lightProjection.m[3][i]+=coverage->offset[i];
    Check(FxSetMatrix(effect,"matLightViewProj",&lightProjection),"Set light projection");
    const float white[4]={1,1,1,.37f},dark[4]={.2f,.2f,.2f,1},light[4]={std::sqrt(1-facing*facing),0,-facing,1},zero[4]={0,0,0,1},eye[4]={0,0,4,1};
    Check(FxSetFloatArray(effect,"EgColor",white,4),"Set material");
    Check(FxSetFloatArray(effect,"ToonColor",dark,4),"Set toon");
    Check(FxSetFloatArray(effect,"LightDir",light,4),"Set light");
    Check(FxSetFloatArray(effect,"SpcColor",zero,4),"Set specular");
    Check(FxSetFloatArray(effect,"Place",eye,4),"Set eye");
    if(coverage){
        const float backlight[4]={0,.9797959f,.2f,1},gloss[4]={.3f,.3f,.3f,1};
        Check(FxSetFloatArray(effect,"LightDir",backlight,4),"Set backlit material");
        if(coverage->specular)Check(FxSetFloatArray(effect,"SpcColor",gloss,4),"Set authored specular");
    }
    Check(FxSetInt(effect,"parthf",mode==2),"Set shadow mode");
    Check(FxSetInt(effect,"transp",0),"Set material opacity");
    Check(FxSetTechnique(effect,"BufferShadowTec"),"Select native receiver");
    const Vertex vertices[]={{{-1,1,.5f-slope},{0,0,1},{0,0}},{{1,1,.5f+slope},{0,0,1},{1,0}},
                             {{-1,-1,.5f-slope},{0,0,1},{0,1}},{{1,-1,.5f+slope},{0,0,1},{1,1}}};
    Check(device->SetTexture(0,shadow.Get()),"Bind shadow map");
    Check(device->SetRenderState(D3DRS_ZENABLE,FALSE),"Disable depth testing");
    Check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Set culling");
    Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"Disable blending");
    Check(device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1),"Set vertex format");
    Check(device->BeginScene(),"Begin scene");
    UINT passes=0;Check(FxBegin(effect,&passes),"Begin effect");
    for(UINT p=0;p<passes;++p){Check(FxBeginPass(effect,p),"Begin pass");
        Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)),"Draw receiver");
        Check(FxEndPass(effect),"End pass");}
    Check(FxEnd(effect),"End effect");Check(device->EndScene(),"End scene");
    ComPtr<IDirect3DSurface9> target,readback;Check(device->GetRenderTarget(0,&target),"Get render target");
    Check(device->CreateOffscreenPlainSurface(kSize,kSize,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr),"Create readback");
    Check(device->GetRenderTargetData(target.Get(),readback.Get()),"Read GPU pixels");
    Check(readback->LockRect(&pixels,nullptr,D3DLOCK_READONLY),"Lock readback");
    std::vector<DWORD> output(kSize*kSize);
    for(unsigned y=0;y<kSize;++y)std::copy_n(reinterpret_cast<const DWORD*>(static_cast<const char*>(pixels.pBits)+y*pixels.Pitch),kSize,output.data()+y*kSize);
    Check(readback->UnlockRect(),"Unlock readback");Check(device->SetTexture(0,nullptr),"Unbind map");
    return output;
}
void Save(const std::filesystem::path& path,const std::vector<DWORD>& pixels) {
    std::ofstream out(path,std::ios::binary);out<<"P6\n"<<kSize<<" "<<kSize<<"\n255\n";
    for(auto pixel:pixels){const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)};out.write(rgb,3);}
    if(!out)throw std::runtime_error("Cannot write GPU readback image");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>4){std::fputs("usage: native_shadow_tests <production shader> [--legacy|--coverage-only] [output-directory]\n",stderr);return 2;}
    const bool legacy=argc>=3&&!wcscmp(argv[2],L"--legacy");
    const bool coverageOnly=argc>=3&&!wcscmp(argv[2],L"--coverage-only");
    const std::filesystem::path output=argc>((legacy||coverageOnly)?3:2)?argv[(legacy||coverageOnly)?3:2]:L"";
    if(!output.empty())std::filesystem::create_directories(output);
    HWND window=CreateWindowExW(0,L"STATIC",L"native shadow test",WS_OVERLAPPED,0,0,kSize,kSize,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    try {
        CheckProjection();
        if(!wcscmp(argv[1],L"--bounds-only")){DestroyWindow(window);return 0;}
        ComPtr<IDirect3D9> d3d;d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if(!d3d)throw std::runtime_error("D3D9 unavailable");
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
        pp.BackBufferWidth=kSize;pp.BackBufferHeight=kSize;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        ComPtr<IDirect3DDevice9> device;
        Check(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device),"Create hidden HAL device");
        auto effect=Load(device.Get(),argv[1]);unsigned failures=0;
        for(int mode:{1,2})for(bool specular:{false,true}){
            // Camera-dependent shadow coverage must not toggle toon lighting
            // or uncover specular on a backlit surface. Cross all six bounds.
            for(auto offset:std::vector<CoverageProbe>{{{0,0,0},specular},{{-3,0,0},specular},
                    {{3,0,0},specular},{{0,-3,0},specular},{{0,3,0},specular},
                    {{0,0,-1},specular},{{0,0,1},specular}}){
                const auto image=Draw(device.Get(),effect.value,32,mode,0,false,legacy||coverageOnly,0,0,0,&offset);
                unsigned wrong=0;
                for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x){
                    const auto pixel=image[y*kSize+x];
                    wrong+=std::abs(int((pixel>>16)&255)-51)>1||std::abs(int(pixel>>24)-94)>1;
                }
                std::printf("material_coverage mode=%d specular=%d offset=%.0f,%.0f,%.0f wrong=%u\n",
                    mode,specular,offset.offset[0],offset.offset[1],offset.offset[2],wrong);
                failures+=wrong!=0;
            }
        }
        if(coverageOnly){DestroyWindow(window);std::printf("material_coverage failures=%u\n",failures);return failures?1:0;}
        auto reference=Load(device.Get(),legacy ? std::filesystem::path(argv[1]) :
            std::filesystem::path(argv[1]).parent_path()/"original"/"skin_effect_sm3.fx");
        unsigned comparisons=0, partialDepths=0;
        const auto compare=[&](unsigned resolution,int mode,float slope,float gap,int pattern,float phase,float facing=1.f){
            const auto actual=Draw(device.Get(),effect.value,resolution,mode,slope,false,false,gap,pattern,phase,nullptr,facing);
            const auto original=Draw(device.Get(),reference.value,resolution,mode,slope,false,true,gap,pattern,phase,nullptr,facing);
            unsigned difference=0, different=0;
            for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x){
                const DWORD a=actual[y*kSize+x], b=original[y*kSize+x];
                unsigned error=0;
                for(unsigned shift:{0u,8u,16u,24u})
                    error=(std::max)(error,unsigned(std::abs(int((a>>shift)&255)-int((b>>shift)&255))));
                difference=(std::max)(difference,error);different+=error>1;
            }
            const unsigned centre=(actual[64*kSize+64]>>16)&255;
            if(gap>0&&slope==0&&pattern==0&&facing==1)partialDepths+=centre>55&&centre<250;
            std::printf("native_parity size=%u mode=%d slope=%.3f gap=%.5f pattern=%d phase=%.3f facing=%.2f max_difference=%u different=%u\n",
                resolution,mode,slope,gap,pattern,phase,facing,difference,different);
            failures+=different!=0;++comparisons;
            if(pattern==1&&phase==0&&!output.empty()){
                const auto name="diagonal-mode"+std::to_string(mode);
                Save(output/(name+".ppm"),actual);Save(output/(name+"-932.ppm"),original);
            }
        };
        for(int mode:{1,2}){
            // Magnified and minified maps; coplanar slopes; thin and diagonal
            // occluders; fractional map positions. Compare real GPU RGBA with
            // the checked-in 9.32 shader instead of imposing a new PCF filter.
            for(unsigned resolution:{16u,32u,64u,192u}){
                for(float slope:{-.15f,0.f,.15f})compare(resolution,mode,slope,0,0,0);
                for(float gap:{.00015f,.00025f,.0004f,.0006f,.001f})compare(resolution,mode,0,gap,0,0);
                compare(resolution,mode,0,0,2,0);
            }
            for(float phase:{0.f,.09375f,.1875f,.28125f,.375f,.46875f,.5625f,.65625f})
                compare(16,mode,0,0,1,phase);
            // Native toon interpolation is linear, including partially lit
            // normals. A smoothstep here changes the body gradient even if
            // the depth sampler and both mode thresholds are correct.
            for(float facing:{.05f,.1f,.2f,.3f,.4f})compare(32,mode,0,.00025f,0,0,facing);
        }
        failures+=partialDepths==0;
        std::printf("native_parity comparisons=%u partial_depth_cases=%u\n",comparisons,partialDepths);
        DestroyWindow(window);std::printf("native_shadow failures=%u\n",failures);return failures?1:0;
    } catch(const std::exception& e){DestroyWindow(window);std::fprintf(stderr,"%s\n",e.what());return 1;}
}
