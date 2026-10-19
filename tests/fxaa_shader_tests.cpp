// MAINTENANCE: regression-keep; real GPU edge reconstruction and transparent coverage.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr unsigned kWidth = 192, kHeight = 144, kReferenceGrid = 16;
struct Pixel { float r, g, b, a; float* data() { return &r; } const float* data() const { return &r; } };
using Image = std::vector<Pixel>;
struct Rect { unsigned left=0, top=0, right=kWidth, bottom=kHeight; };
enum class Shape { HalfPlane, Strip, Corner, Solid };
struct Case {
    std::string name;
    Shape shape = Shape::HalfPlane;
    float nx = 1, ny = 0, phase = .23f, width = 1;
    Pixel foreground{0,0,0,1}, background{1,1,1,1};
    bool transparent=false, randomAlpha=false, poison=false, quality=true;
    Rect viewport;
};
struct Metrics {
    double pointMse=0, outputMse=0, pointEdgeMse=0, outputEdgeMse=0;
    double premultError=0, maxFlatError=0;
    unsigned edgePixels=0, softPixels=0, alphaSoftPixels=0, changedPixels=0;
    unsigned nonfinite=0, outOfRange=0, premultViolations=0, flatChanged=0;
};
unsigned failures=0, assertions=0;
void Need(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
void Verify(bool ok, const std::string& message) {
    ++assertions;
    if(!ok) { ++failures; std::fprintf(stderr,"FAIL %s\n",message.c_str()); }
}
float Clamp(float v) { return std::clamp(v,0.f,1.f); }
std::string JsonString(const std::string& text) {
    std::string result="\"";
    for(unsigned char c:text) {
        if(c=='\\' || c=='\"') result+='\\';
        if(c=='\n') result+="\\n"; else if(c=='\r') result+="\\r";
        else if(c=='\t') result+="\\t"; else result+=char(c);
    }
    return result+'\"';
}
unsigned Byte(float v) { return std::isfinite(v)?unsigned(std::lround(Clamp(v)*255.f)):0; }
Pixel Quantize(Pixel v) { for(unsigned c=0;c<4;++c) v.data()[c]=Byte(v.data()[c])/255.f; return v; }
Pixel Mix(Pixel a, Pixel b, float t) {
    Pixel p{}; for(unsigned c=0;c<4;++c) p.data()[c]=a.data()[c]+(b.data()[c]-a.data()[c])*t; return p;
}
float SignedDistance(const Case& c, float x, float y) {
    x-=kWidth*.5f; y-=kHeight*.5f;
    const float d=c.nx*x+c.ny*y-c.phase;
    if(c.shape==Shape::Solid) return -1000000;
    if(c.shape==Shape::Strip) return std::abs(d)-c.width*.5f;
    if(c.shape==Shape::Corner) return std::abs(x)*.65f+y-c.phase;
    return d;
}
bool Inside(const Case& c,float x,float y) { return SignedDistance(c,x,y)<=0; }
bool InRect(const Rect& r,unsigned x,unsigned y) { return x>=r.left && x<r.right && y>=r.top && y<r.bottom; }
Image Generate(const Case& c,bool reference=false) {
    Image image(kWidth*kHeight);
    const Pixel fg=Quantize(c.foreground), bg=Quantize(c.background);
    for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
        float coverage=0;
        const float d=SignedDistance(c,x+.5f,y+.5f);
        const float footprint=c.shape==Shape::Corner?.825f:.5f*(std::abs(c.nx)+std::abs(c.ny));
        if(reference && std::abs(d)>=footprint) coverage=d<=0?1.f:0.f;
        else if(reference) {
            // 16x16 spatial integration is an independent geometric reference,
            // never a reference produced by this FXAA shader.
            for(unsigned sy=0;sy<kReferenceGrid;++sy) for(unsigned sx=0;sx<kReferenceGrid;++sx)
                coverage+=Inside(c,x+(sx+.5f)/kReferenceGrid,y+(sy+.5f)/kReferenceGrid)?1.f:0.f;
            coverage/=float(kReferenceGrid*kReferenceGrid);
        } else coverage=Inside(c,x+.5f,y+.5f)?1.f:0.f;
        auto p=Mix(bg,fg,coverage);
        if(c.randomAlpha) p.a=((x*37+y*19+x*y*3)%256)/255.f;
        if(!InRect(c.viewport,x,y)) p=c.poison?Pixel{1,0,1,0}:Pixel{0,1,0,1};
        image[y*kWidth+x]=p;
    }
    return image;
}
void SaveFloat(const fs::path& path,const Image& image) {
    std::ofstream out(path,std::ios::binary); Need(bool(out),"create float image");
    out.write(reinterpret_cast<const char*>(image.data()),std::streamsize(image.size()*sizeof(Pixel)));
}
void SavePng(IWICImagingFactory* factory,const fs::path& path,const Image& source,
             bool transparent,bool checker=false,bool alphaView=false) {
    std::vector<unsigned char> bytes(source.size()*4);
    for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
        auto p=source[y*kWidth+x];
        for(unsigned c=0;c<4;++c) if(!std::isfinite(p.data()[c])) p.data()[c]=1;
        if(alphaView) p={p.a,p.a,p.a,1};
        else if(transparent && checker) {
            const float bg=((x/8+y/8)&1)?.8f:.4f;
            p={p.r+bg*(1-p.a),p.g+bg*(1-p.a),p.b+bg*(1-p.a),1};
        } else if(transparent) {
            // PNG stores straight RGB; the .rgba32f companion preserves the
            // actual premultiplied render target without conversion.
            if(p.a>1e-8f) { p.r/=p.a; p.g/=p.a; p.b/=p.a; } else p.r=p.g=p.b=0;
        } else p.a=1;
        auto* dst=bytes.data()+(y*kWidth+x)*4;
        dst[0]=static_cast<unsigned char>(Byte(p.b)); dst[1]=static_cast<unsigned char>(Byte(p.g));
        dst[2]=static_cast<unsigned char>(Byte(p.r)); dst[3]=static_cast<unsigned char>(Byte(p.a));
    }
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    Need(SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE)),"create PNG stream");
    Need(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(kWidth,kHeight)),"create PNG encoder");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    Need(SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"PNG BGRA format");
    Need(SUCCEEDED(frame->WritePixels(kHeight,kWidth*4,UINT(bytes.size()),bytes.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()),"write PNG");
}
struct Gpu {
    HMODULE runtime=nullptr,compiler=nullptr;
    HWND window=nullptr;
    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DPixelShader9> shader;
    ComPtr<IDirect3DTexture9> input,maskTexture,target;
    ComPtr<IDirect3DSurface9> surface,cpu;
    std::string adapter;
    explicit Gpu(const fs::path& hlsl) {
        wchar_t system[MAX_PATH]{}; Need(GetSystemDirectoryW(system,MAX_PATH)>0,"system directory");
        runtime=LoadLibraryW((fs::path(system)/L"d3d9.dll").c_str());
        compiler=LoadLibraryW((fs::path(system)/L"d3dcompiler_47.dll").c_str());
        Need(runtime && compiler,"load native D3D9 and HLSL compiler");
        const auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime,"Direct3DCreate9"));
        const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));
        d3d.Attach(create?create(D3D_SDK_VERSION):nullptr); Need(d3d && compile,"native D3D9 exports");
        D3DADAPTER_IDENTIFIER9 id{}; Need(SUCCEEDED(d3d->GetAdapterIdentifier(0,0,&id)),"adapter identity"); adapter=id.Description;
        window=CreateWindowW(L"STATIC",L"Hidden FXAA edge regression",WS_POPUP,0,0,kWidth,kHeight,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Need(window!=nullptr,"hidden GPU window");
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
        pp.BackBufferWidth=kWidth; pp.BackBufferHeight=kHeight; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        Need(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)),"create hardware D3D9 device");
        std::ifstream f(hlsl,std::ios::binary); Need(bool(f),"open candidate HLSL");
        std::string text{std::istreambuf_iterator<char>(f),{}};
        ComPtr<ID3DBlob> code,error;
        const auto hr=compile(text.data(),text.size(),hlsl.string().c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "fxaa","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr) && error) std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
        Need(SUCCEEDED(hr),"compile candidate fxaa ps_3_0");
        Need(SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&shader)),"create candidate shader");
        Need(SUCCEEDED(device->CreateTexture(kWidth,kHeight,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&input,nullptr)) &&
            SUCCEEDED(device->CreateTexture(kWidth,kHeight,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&maskTexture,nullptr)) &&
            SUCCEEDED(device->CreateTexture(kWidth,kHeight,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&target,nullptr)) &&
            SUCCEEDED(target->GetSurfaceLevel(0,&surface)) &&
            SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth,kHeight,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&cpu,nullptr)),"allocate float output and readback");
        Need(SUCCEEDED(device->SetDepthStencilSurface(nullptr)) && SUCCEEDED(device->SetRenderTarget(0,surface.Get())),"bind float output");
        const D3DVIEWPORT9 viewport{0,0,kWidth,kHeight,0,1}; device->SetViewport(&viewport);
        device->SetTexture(0,input.Get()); device->SetVertexShader(nullptr); device->SetPixelShader(shader.Get());
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
        for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,
            D3DRS_LIGHTING,D3DRS_FOGENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_DITHERENABLE}) device->SetRenderState(state,FALSE);
        device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        for(unsigned sampler:{0u,1u}) {
            for(auto state:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER}) device->SetSamplerState(sampler,state,D3DTEXF_LINEAR);
            device->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE); device->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE);
            device->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); device->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        }
        std::printf("GPU %s shader_bytes=%zu output=RGBA32F\n",adapter.c_str(),code->GetBufferSize());
    }
    ~Gpu() {
        if(device) { device->SetTexture(0,nullptr); device->SetTexture(1,nullptr); }
        cpu.Reset(); surface.Reset(); target.Reset(); maskTexture.Reset(); input.Reset(); shader.Reset(); device.Reset(); d3d.Reset();
        if(window) DestroyWindow(window); if(compiler) FreeLibrary(compiler); if(runtime) FreeLibrary(runtime);
    }
    Image Render(const Image& image,const Case& c,const Image* mask=nullptr,bool lineOnly=false,bool protectMsaa=false) {
        const auto upload=[&](IDirect3DTexture9* texture,const Image& pixels) {
            Need(pixels.size()==kWidth*kHeight,"upload dimensions");
            D3DLOCKED_RECT uploadLock{}; Need(SUCCEEDED(texture->LockRect(0,&uploadLock,nullptr,0)),"upload texture");
            for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
                const auto p=pixels[y*kWidth+x]; auto* row=reinterpret_cast<DWORD*>(static_cast<char*>(uploadLock.pBits)+y*uploadLock.Pitch);
                row[x]=D3DCOLOR_ARGB(Byte(p.a),Byte(p.r),Byte(p.g),Byte(p.b));
            }
            Need(SUCCEEDED(texture->UnlockRect(0)),"finish texture upload");
        };
        upload(input.Get(),image); if(mask)upload(maskTexture.Get(),*mask);
        Need(SUCCEEDED(device->SetTexture(1,mask?maskTexture.Get():nullptr)),"bind optional native coverage mask");
        const auto r=c.viewport;
        const float constants[]={1.f/kWidth,1.f/kHeight,0,c.transparent?1.f:0.f,
            (r.left+.5f)/kWidth,(r.top+.5f)/kHeight,(r.right-.5f)/kWidth,(r.bottom-.5f)/kHeight,0,0,0,0};
        Need(SUCCEEDED(device->SetPixelShaderConstantF(0,constants,3)),"bind c0 dimensions/transparent c1 viewport c2 jitter");
        const float outlineFlags[]={mask?1.f:0.f,protectMsaa?1.f:0.f,lineOnly?1.f:0.f,0};
        Need(SUCCEEDED(device->SetPixelShaderConstantF(4,outlineFlags,1)),"bind native mask, MSAA protection and line-only flags");
        struct Vertex { float x,y,z,w,u,v; };
        const Vertex quad[]={{-.5f,-.5f,0,1,0,0},{kWidth-.5f,-.5f,0,1,1,0},
            {-.5f,kHeight-.5f,0,1,0,1},{kWidth-.5f,kHeight-.5f,0,1,1,1}};
        Need(SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex))) &&
            SUCCEEDED(device->EndScene()),"execute candidate shader");
        D3DLOCKED_RECT lock{};
        Need(SUCCEEDED(device->GetRenderTargetData(surface.Get(),cpu.Get())) &&
            SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"read actual float GPU output");
        Image result(kWidth*kHeight);
        for(unsigned y=0;y<kHeight;++y) std::memcpy(result.data()+y*kWidth,static_cast<char*>(lock.pBits)+y*lock.Pitch,kWidth*sizeof(Pixel));
        cpu->UnlockRect(); return result;
    }
};
Metrics Analyze(const Case& c,const Image& input,const Image& output,const Image& reference) {
    Metrics m; double sumPoint=0,sumOutput=0,edgePoint=0,edgeOutput=0; unsigned components=0;
    const Pixel fg=Quantize(c.foreground), bg=Quantize(c.background);
    const unsigned signal=c.transparent?3:0;
    for(unsigned y=c.viewport.top;y<c.viewport.bottom;++y) for(unsigned x=c.viewport.left;x<c.viewport.right;++x) {
        const unsigned at=y*kWidth+x; const auto p=input[at], q=output[at], r=reference[at];
        bool edge=false;
        for(int dy=-2;dy<=2;++dy) for(int dx=-2;dx<=2;++dx) {
            const unsigned xx=unsigned(std::clamp(int(x)+dx,int(c.viewport.left),int(c.viewport.right)-1));
            const unsigned yy=unsigned(std::clamp(int(y)+dy,int(c.viewport.top),int(c.viewport.bottom)-1));
            const auto s=input[yy*kWidth+xx]; edge=edge || std::abs(s.data()[signal]-p.data()[signal])>1.f/255;
        }
        edge=edge || (r.data()[signal]>std::min(fg.data()[signal],bg.data()[signal])+1.f/255 &&
            r.data()[signal]<std::max(fg.data()[signal],bg.data()[signal])-1.f/255);
        double ep=0,eq=0; bool changed=false;
        for(unsigned channel=0;channel<(c.transparent?4u:3u);++channel) {
            const double a=p.data()[channel]-r.data()[channel], b=q.data()[channel]-r.data()[channel]; ep+=a*a; eq+=b*b; ++components;
        }
        sumPoint+=ep; sumOutput+=eq;
        if(edge) { edgePoint+=ep; edgeOutput+=eq; ++m.edgePixels; }
        for(unsigned channel=0;channel<4;++channel) {
            const float v=q.data()[channel]; m.nonfinite+=!std::isfinite(v); m.outOfRange+=v<-.00001f || v>1.00001f;
            changed=changed || Byte(v)!=Byte(p.data()[channel]);
            if(c.shape==Shape::Solid) {
                m.maxFlatError=std::max(m.maxFlatError,double(std::abs(v-p.data()[channel])));
                m.flatChanged+=Byte(v)!=Byte(p.data()[channel]);
            }
        }
        m.changedPixels+=changed;
        const float lo=std::min(fg.data()[signal],bg.data()[signal]), hi=std::max(fg.data()[signal],bg.data()[signal]);
        m.softPixels+=edge && q.data()[signal]>lo+2.f/255 && q.data()[signal]<hi-2.f/255;
        m.alphaSoftPixels+=edge && q.a>2.f/255 && q.a<fg.a-2.f/255;
        if(c.transparent) for(unsigned channel=0;channel<3;++channel) {
            const float expected=q.a*fg.data()[channel]/fg.a;
            m.premultError=std::max(m.premultError,double(std::abs(q.data()[channel]-expected)));
            m.premultViolations+=q.data()[channel]>q.a+.00001f;
        }
    }
    m.pointMse=sumPoint/components; m.outputMse=sumOutput/components;
    const double edgeComponents=std::max(1u,m.edgePixels)*(c.transparent?4u:3u);
    m.pointEdgeMse=edgePoint/edgeComponents; m.outputEdgeMse=edgeOutput/edgeComponents;
    return m;
}
std::vector<Case> Cases() {
    std::vector<Case> cases;
    for(float slope:{.08f,.25f,.5f,1.f,4.f,12.f,-.5f,-4.f}) {
        const float normalizer=1/std::sqrt(1+slope*slope);
        for(float phase:{.23f,.61f}) {
            Case c; std::ostringstream name; name<<"edge-s"<<slope<<"-p"<<phase;
            c.name=name.str(); c.nx=normalizer; c.ny=-slope*normalizer; c.phase=phase; cases.push_back(c);
        }
    }
    for(unsigned width=1;width<=4;++width) for(float slope:{.25f,3.f}) {
        Case c; c.name="line-w"+std::to_string(width)+"-s"+std::to_string(int(slope*100));
        c.shape=Shape::Strip; c.width=float(width); c.nx=1/std::sqrt(1+slope*slope); c.ny=-slope*c.nx; cases.push_back(c);
    }
    Case corner; corner.name="sharp-corner"; corner.shape=Shape::Corner; cases.push_back(corner);
    Case colors; colors.name="red-blue-material"; colors.nx=.8f; colors.ny=-.6f;
    colors.foreground={1,0,0,1}; colors.background={0,0,1,1}; cases.push_back(colors);
    Case black=colors; black.name="transparent-black-edge"; black.transparent=true;
    black.foreground={0,0,0,1}; black.background={0,0,0,0}; cases.push_back(black);
    for(unsigned width=1;width<=4;++width) {
        auto c=black; c.name="transparent-black-line-w"+std::to_string(width); c.shape=Shape::Strip; c.width=float(width); cases.push_back(c);
    }
    Case premult=black; premult.name="premultiplied-color-edge"; premult.foreground={.8f,.3f,.1f,.8f}; cases.push_back(premult);
    Case flat; flat.name="flat-opaque"; flat.shape=Shape::Solid; flat.foreground={.2f,.4f,.6f,1}; flat.quality=false; cases.push_back(flat);
    flat.name="flat-premultiplied"; flat.foreground={.1f,.2f,.3f,.7f}; flat.transparent=true; cases.push_back(flat);
    Case noise=colors; noise.name="mme-alpha-control"; noise.quality=false; cases.push_back(noise);
    noise.name="mme-alpha-noise"; noise.randomAlpha=true; cases.push_back(noise);
    Case clamp=colors; clamp.viewport={17,13,kWidth-19,kHeight-15}; clamp.quality=false;
    clamp.name="viewport-edge-control"; cases.push_back(clamp); clamp.name="viewport-edge-poison"; clamp.poison=true; cases.push_back(clamp);
    clamp.shape=Shape::Solid; clamp.foreground={.25f,.5f,.75f,1}; clamp.name="viewport-flat-control"; clamp.poison=false; cases.push_back(clamp);
    clamp.name="viewport-flat-poison"; clamp.poison=true; cases.push_back(clamp);
    return cases;
}
void CompareRgb(const Image& a,const Image& b,const Rect& viewport,bool includeAlpha,const char* message) {
    double error=0;
    for(unsigned y=viewport.top;y<viewport.bottom;++y) for(unsigned x=viewport.left;x<viewport.right;++x)
        for(unsigned c=0;c<(includeAlpha?4u:3u);++c) error=std::max(error,double(std::abs(a[y*kWidth+x].data()[c]-b[y*kWidth+x].data()[c])));
    Verify(error<.00001,message); std::printf("PAIR %s max_error=%.9f\n",message,error);
}
struct NativeLine { Case test; Image input,mask,ideal; };
Pixel InkPixel(const Case& c,float coverage) {
    return c.transparent?Quantize(Pixel{.25f*coverage,.5f*coverage,.75f*coverage,coverage}):
        Pixel{1-coverage,1-coverage,1-coverage,1};
}
// Production supplies a minimum-width hull: an authored outline thinner than a
// pixel is widened to exactly one pixel and its alpha carries the authored
// ink. The line therefore never disappears, but its spatial coverage stays a
// hard single-sample step instead of the fractional ramp MSAA would resolve.
// This is the profile FXAA actually receives, so it is measured separately
// from the already resolved coverage that must merely survive the filter.
float WidenedCoverage(const Case& c,float x,float y) {
    const float d=c.nx*(x+.5f-kWidth*.5f)+c.ny*(y+.5f-kHeight*.5f)-c.phase;
    const float half=c.width<1?.5f:c.width*.5f;
    if(std::abs(d)>half) return 0;
    return c.width<1?c.width:1;
}
NativeLine NativeCoverage(Case c,int variant) {
    Case coverage=c; coverage.transparent=true; coverage.foreground={1,1,1,1}; coverage.background={0,0,0,0};
    coverage.poison=false;
    // The 16x16 integrated coverage is the independent reference: it is what
    // MSAA resolves for the same authored outline, and never this filter's own
    // output. Variant 0 is that resolved coverage, variant 1 is the widened
    // single-sample hull production rasterises, and variant 2 is the same hull
    // for a half-transparent authored outline.
    const auto reference=Generate(coverage,true);
    const float authoredAlpha=variant==2?.5f:1.f;
    NativeLine line{c,Image(kWidth*kHeight),Image(kWidth*kHeight),Image(kWidth*kHeight)};
    for(unsigned y=0;y<kHeight;++y) for(unsigned x=0;x<kWidth;++x) {
        const unsigned at=y*kWidth+x;
        const float resolved=Byte(reference[at].r)/255.f;
        // The scene and the mask travel through 8-bit targets, so the fixture
        // quantizes them too; otherwise the analysis measures upload rounding
        // instead of filtering.
        const float value=variant==0?resolved:
            Byte(WidenedCoverage(c,float(x),float(y))*authoredAlpha)/255.f;
        line.ideal[at]=InkPixel(c,resolved*authoredAlpha);
        line.mask[at]={value,value,value,value};
        line.input[at]=InkPixel(c,value);
        if(!InRect(c.viewport,x,y)) {
            line.mask[at]=c.poison?Pixel{1,1,1,1}:Pixel{0,0,0,0};
            line.input[at]=c.poison?Pixel{1,0,1,0}:c.transparent?Pixel{0,0,0,0}:Pixel{1,1,1,1};
            line.ideal[at]=line.input[at];
        }
    }
    return line;
}
double Ink(const Pixel& pixel,bool transparent,unsigned channel) {
    return transparent?pixel.data()[channel]:1.0-pixel.data()[channel];
}
struct NativeMetrics {
    double basePeak=0,outputPeak=0,idealPeak=0,minimumTracePeak=std::numeric_limits<double>::infinity();
    double peakRetention=0,varianceGain=0,maxMassError=0,maxOutsideError=0;
    double maxCentroidShift=0,maxCentroidErrorInput=0,maxCentroidErrorOutput=0;
    double inputIdealRmse=0,outputIdealRmse=0;
    unsigned missing=0,nonfinite=0,outOfRange=0,premultViolations=0,changed=0;
};
NativeMetrics AnalyzeNative(const NativeLine& line,const Image& output,const fs::path& tracePath) {
    NativeMetrics m; const auto& c=line.test;
    const unsigned signal=c.transparent?3u:0u;
    std::ofstream trace(tracePath); Need(bool(trace),"create native line cross-section trace");
    trace<<std::setprecision(12)<<"normal_distance,base_ink,output_ink,mask_r,base_r,base_g,base_b,base_a,output_r,output_g,output_b,output_a\n";
    for(unsigned y=c.viewport.top;y<c.viewport.bottom;++y) for(unsigned x=c.viewport.left;x<c.viewport.right;++x) {
        const unsigned at=y*kWidth+x; const auto p=line.input[at],q=output[at];
        const double baseInk=Ink(p,c.transparent,signal), outputInk=Ink(q,c.transparent,signal);
        m.missing+=baseInk>0 && outputInk<=1e-6;
        bool changed=false;
        for(unsigned channel=0;channel<4;++channel) {
            const auto value=q.data()[channel];
            m.nonfinite+=!std::isfinite(value); m.outOfRange+=value<-.00001f || value>1.00001f;
            changed=changed || std::abs(value-p.data()[channel])>1e-5;
        }
        m.changed+=changed;
        if(c.transparent) for(unsigned channel=0;channel<3;++channel)
            m.premultViolations+=q.data()[channel]>q.a+.00001f;
        bool nearMask=false;
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
            const unsigned xx=unsigned(std::clamp(int(x)+dx,int(c.viewport.left),int(c.viewport.right)-1));
            const unsigned yy=unsigned(std::clamp(int(y)+dy,int(c.viewport.top),int(c.viewport.bottom)-1));
            nearMask=nearMask || line.mask[yy*kWidth+xx].r>0;
        }
        if(!nearMask) for(unsigned channel=0;channel<4;++channel)
            m.maxOutsideError=std::max(m.maxOutsideError,double(std::abs(q.data()[channel]-p.data()[channel])));
    }
    // Each of these 33 interior slices contains the entire line cross-section.
    // Straight lines are translation-invariant along their tangent, so no
    // viewport-edge flux is hidden in a whole-image or average-ink comparison.
    const bool scanY=std::abs(c.nx)<.1f;
    double inputIdealSquares=0,outputIdealSquares=0; unsigned idealSamples=0;
    for(int section=-16;section<=16;++section) {
        std::array<double,4> before{},after{};
        double basePeak=0,outputPeak=0,idealPeak=0,baseFirst=0,outputFirst=0,baseSecond=0,outputSecond=0;
        double idealFirst=0,idealMass=0;
        for(unsigned i=0;i<(scanY?kHeight:kWidth);++i) {
            const unsigned x=scanY?unsigned(int(kWidth/2)+section):i;
            const unsigned y=scanY?i:unsigned(int(kHeight/2)+section);
            const unsigned at=y*kWidth+x; const auto p=line.input[at],q=output[at];
            const double distance=c.nx*(x+.5-kWidth*.5)+c.ny*(y+.5-kHeight*.5)-c.phase;
            const double a=Ink(p,c.transparent,signal),b=Ink(q,c.transparent,signal);
            const double idealInk=Ink(line.ideal[at],c.transparent,signal);
            idealPeak=std::max(idealPeak,idealInk);
            // Both distances are measured against the resolved coverage, so a
            // filtered profile closer to MSAA scores lower regardless of how
            // much ink it moved.
            const double inputError=a-idealInk, outputError=b-idealInk;
            inputIdealSquares+=inputError*inputError; outputIdealSquares+=outputError*outputError;
            ++idealSamples;
            basePeak=std::max(basePeak,a); outputPeak=std::max(outputPeak,b);
            baseFirst+=distance*a; outputFirst+=distance*b;
            baseSecond+=distance*distance*a; outputSecond+=distance*distance*b;
            idealFirst+=distance*idealInk; idealMass+=idealInk;
            for(unsigned channel=0;channel<4;++channel) {
                before[channel]+=Ink(p,c.transparent,channel); after[channel]+=Ink(q,c.transparent,channel);
            }
            if(section==0) {
                trace<<distance<<','<<a<<','<<b<<','<<line.mask[at].r;
                for(unsigned channel=0;channel<4;++channel)trace<<','<<p.data()[channel];
                for(unsigned channel=0;channel<4;++channel)trace<<','<<q.data()[channel];
                trace<<'\n';
            }
        }
        Need(before[signal]>0 && basePeak>0,"nonempty independent coverage cross-section");
        for(unsigned channel=0;channel<4;++channel)m.maxMassError=std::max(m.maxMassError,std::abs(after[channel]-before[channel]));
        const double baseMean=baseFirst/before[signal],outputMean=outputFirst/after[signal];
        // The first moment is the line's position along its normal. MSAA and
        // TAA reconstruct the same line from coverage that never leaves its
        // authored place, so a spatial filter that keeps the position is the
        // one that matches them; a filter that moves it is a visible offset
        // however smooth the transition looks.
        m.maxCentroidShift=std::max(m.maxCentroidShift,std::abs(outputMean-baseMean));
        const double idealMean=idealFirst/idealMass;
        m.maxCentroidErrorInput=std::max(m.maxCentroidErrorInput,std::abs(baseMean-idealMean));
        m.maxCentroidErrorOutput=std::max(m.maxCentroidErrorOutput,std::abs(outputMean-idealMean));
        m.varianceGain+=outputSecond/after[signal]-outputMean*outputMean-
            (baseSecond/before[signal]-baseMean*baseMean);
        m.basePeak+=basePeak; m.outputPeak+=outputPeak; m.idealPeak+=idealPeak;
        m.peakRetention+=outputPeak/basePeak;
        m.minimumTracePeak=std::min(m.minimumTracePeak,outputPeak);
    }
    m.basePeak/=33; m.outputPeak/=33; m.idealPeak/=33; m.peakRetention/=33; m.varianceGain/=33;
    m.inputIdealRmse=std::sqrt(inputIdealSquares/std::max(1u,idealSamples));
    m.outputIdealRmse=std::sqrt(outputIdealSquares/std::max(1u,idealSamples));
    return m;
}
unsigned CheckNativeMask(Gpu& gpu,IWICImagingFactory* wic,const fs::path& directory) {
    const auto root=directory/L"native-mask"; fs::create_directories(root);
    std::ofstream csv(root/L"metrics.csv"); Need(bool(csv),"create native mask metrics");
    csv<<std::setprecision(12)<<"case,input,base_peak,ideal_peak,output_peak,minimum_trace_peak,peak_retention,normal_variance_gain,"
        "max_cross_section_rgba_mass_error,centroid_shift,centroid_error_input,centroid_error_output,"
        "input_ideal_rmse,output_ideal_rmse,changed_pixels,missing_pixels,max_outside_mask_error\n";
    // [transparent][input class][width][direction]; class 0 is already resolved
    // coverage, class 1 is the widened single-sample hull production supplies,
    // class 2 is that hull for a half-transparent authored outline.
    std::array<std::array<std::array<std::array<NativeMetrics,3>,3>,3>,2> measurements{};
    const float widths[]={.25f,1.f,2.f};
    const char* directions[]={"horizontal","vertical","diagonal45"};
    const char* inputs[]={"resolved","widened","widened50"};
    unsigned count=0;
    for(unsigned transparent=0;transparent<2;++transparent) for(unsigned width=0;width<3;++width)
    for(unsigned direction=0;direction<3;++direction) for(unsigned variant=0;variant<3;++variant) {
        Case c; c.shape=Shape::Strip; c.width=widths[width]; c.transparent=transparent!=0;
        c.nx=direction==0?0.f:direction==1?1.f:std::sqrt(.5f);
        c.ny=direction==0?1.f:direction==1?0.f:-std::sqrt(.5f);
        c.name="native-"+std::string(c.transparent?"premult-":"opaque-")+directions[direction]+"-w"+
            std::to_string(widths[width])+"-"+inputs[variant];
        const auto line=NativeCoverage(c,variant);
        const auto output=gpu.Render(line.input,c,&line.mask,true),repeat=gpu.Render(line.input,c,&line.mask,true);
        const auto folder=root/c.name; fs::create_directories(folder);
        const auto m=AnalyzeNative(line,output,folder/L"cross-section.csv");
        measurements[transparent][variant][width][direction]=m;
        SavePng(wic,folder/L"input.png",line.input,c.transparent);
        SavePng(wic,folder/L"ideal.png",line.ideal,c.transparent);
        SavePng(wic,folder/L"mask.png",line.mask,false); SavePng(wic,folder/L"output.png",output,c.transparent);
        SaveFloat(folder/L"input.rgba32f",line.input); SaveFloat(folder/L"output.rgba32f",output);
        if(c.transparent)SavePng(wic,folder/L"output-checker.png",output,true,true);
        csv<<c.name<<','<<inputs[variant]<<','<<m.basePeak<<','<<m.idealPeak<<','<<m.outputPeak<<','<<m.minimumTracePeak<<','
           <<m.peakRetention<<','<<m.varianceGain<<','<<m.maxMassError<<','<<m.maxCentroidShift<<','
           <<m.maxCentroidErrorInput<<','<<m.maxCentroidErrorOutput<<','<<m.inputIdealRmse<<','<<m.outputIdealRmse<<','
           <<m.changed<<','<<m.missing<<','<<m.maxOutsideError<<'\n';
        Verify(m.nonfinite==0 && m.outOfRange==0 && m.premultViolations==0,c.name+": finite normalized premultiplied RGBA");
        Verify(m.missing==0 && m.minimumTracePeak>0,c.name+": every covered pixel and every line cross-section survives");
        // The filter conserves ink by construction on the hulls it softens.
        // Resolved coverage is returned untouched, so the only residual there is
        // a rim whose stencil cannot tell a resolved ramp from a hull alpha.
        Verify(m.maxMassError<(variant==0?.0005:.0001),c.name+": each interior cross-section conserves every RGBA ink channel in float output");
        Verify(m.maxOutsideError<.00002,c.name+": line-only pass preserves pixels beyond the native mask neighborhood");
        // MSAA and TAA never move the outline they resolve; the FXAA outline
        // resolve has to match that, including at the strongest strength.
        Verify(m.maxCentroidShift<.002,c.name+": filter keeps the measured line position");
        Verify(m.maxCentroidErrorOutput<=m.maxCentroidErrorInput+.002,
            c.name+": filter does not move the line away from its MSAA position");
        Verify(std::memcmp(output.data(),repeat.data(),output.size()*sizeof(Pixel))==0,c.name+": static input repeats bit for bit");
        if(variant!=0) {
            // The widened hull is a hard step. Filtering it may never move it
            // away from the resolved coverage MSAA produces, and a line at
            // least a pixel wide must gain a transition.
            if(widths[width]>=1) {
                Verify(m.outputIdealRmse<m.inputIdealRmse*.95,
                    c.name+": filter visibly reduces distance to the MSAA coverage");
                Verify(m.changed>16 && m.varianceGain>.0001,
                    c.name+": widened hull receives a measurable spatial spread");
            } else {
                // A quarter-pixel hull carries too little ink for a spread to
                // recover, so the property that matters is that it keeps its
                // density instead of fading out.
                Verify(m.peakRetention>=.85,
                    c.name+": thin widened hull keeps at least 85 percent of its peak density");
            }
        } else {
            // This input already equals what MSAA resolves, so any change is
            // distance from the reference. It must stay near identity and keep
            // most of its density.
            Verify(m.outputIdealRmse<.02,c.name+": already resolved coverage stays close to the MSAA reference");
            Verify(m.peakRetention>=.85,c.name+": already resolved coverage keeps at least 85 percent of its peak density");
        }
        std::printf("NATIVE %-48s peak %.6f->%.6f ideal %.6f retention=%.4f variance=%.6f shift=%+.5f rmse %.6f->%.6f missing=%u\n",
            c.name.c_str(),m.basePeak,m.outputPeak,m.idealPeak,m.peakRetention,m.varianceGain,m.maxCentroidShift,
            m.inputIdealRmse,m.outputIdealRmse,m.missing);
        ++count;
    }
    for(unsigned transparent=0;transparent<2;++transparent) for(unsigned variant=0;variant<3;++variant) {
        for(unsigned width=0;width<3;++width) {
            // Quarter-pixel resolved coverage is byte-identical to the widened
            // hull of the same width, so it is softened like one and its peak
            // follows direction as the hull's does. Wider resolved coverage is
            // left untouched, so only it can hold this directional agreement.
            if(variant==0 && width==0)continue;
            double low=1,high=0;
            for(const auto& m:measurements[transparent][variant][width]) { low=std::min(low,m.peakRetention); high=std::max(high,m.peakRetention); }
            Verify(high-low<.12,"native "+std::string(transparent?"premult":"opaque")+" "+inputs[variant]+" width "+
                std::to_string(widths[width])+": horizontal, vertical and 45-degree peak retention differ by less than 12 points");
        }
        for(unsigned direction=0;direction<3;++direction) {
            const auto& thin=measurements[transparent][variant][0][direction];
            const auto& one=measurements[transparent][variant][1][direction];
            const auto& thick=measurements[transparent][variant][2][direction];
            // Resolved coverage is left alone, so this ordering only applies to
            // the hard single-sample hull that still needs a transition. The
            // filter reads the rasterised width, so after the minimum-width hull
            // a quarter-pixel and a one-pixel authored line share one width and
            // must therefore share their spread. Only a wider hull earns more.
            if(variant==0)continue;
            Verify(std::abs(thin.varianceGain-one.varianceGain)<=1e-3,
                "native "+std::string(transparent?"premult-":"opaque-")+inputs[variant]+"-"+directions[direction]+
                ": quarter-pixel and one-pixel authored lines rasterise one width and share one spread");
            Verify(one.varianceGain<thick.varianceGain*.9,
                "native "+std::string(transparent?"premult-":"opaque-")+inputs[variant]+"-"+directions[direction]+
                ": a two-pixel authored line receives more normal blur than a one-pixel line");
        }
    }
    Case textureCase; textureCase.shape=Shape::Strip; textureCase.width=1;
    auto texture=NativeCoverage(textureCase,0);
    for(unsigned y=16;y<kHeight-16;++y)for(unsigned x=16;x<64;++x) {
        const bool bright=((x+2*y)/3)&1;
        texture.input[y*kWidth+x]=bright?Pixel{1,.2f,0,1}:Pixel{0,.2f,1,1};
        texture.input[y*kWidth+x]=Quantize(texture.input[y*kWidth+x]);
    }
    const auto textureOutput=gpu.Render(texture.input,texture.test,&texture.mask,true);
    double outsideError=0; unsigned outsideByteChanges=0;
    for(unsigned y=16;y<kHeight-16;++y)for(unsigned x=16;x<64;++x)for(unsigned channel=0;channel<4;++channel) {
        const auto at=y*kWidth+x; const auto a=texture.input[at].data()[channel],b=textureOutput[at].data()[channel];
        outsideError=std::max(outsideError,double(std::abs(a-b))); outsideByteChanges+=Byte(a)!=Byte(b);
    }
    Verify(outsideError<.00002 && outsideByteChanges==0,"native line-only mode leaves unrelated high-contrast material texture unchanged");
    SavePng(wic,root/L"line-only-texture-input.png",texture.input,false);
    SavePng(wic,root/L"line-only-texture-output.png",textureOutput,false); ++count;
    const auto protectedOutput=gpu.Render(texture.input,texture.test,&texture.mask,true,true);
    CompareRgb(texture.input,protectedOutput,texture.test.viewport,true,"MSAA-covered native layer is protected from a second FXAA softening"); ++count;
    Case edge; edge.shape=Shape::Strip; edge.width=1; edge.transparent=true; edge.nx=std::sqrt(.5f); edge.ny=-edge.nx;
    edge.viewport={17,13,kWidth-19,kHeight-15};
    const auto control=NativeCoverage(edge,0); edge.poison=true; const auto poison=NativeCoverage(edge,0);
    const auto controlOutput=gpu.Render(control.input,control.test,&control.mask,true);
    const auto poisonOutput=gpu.Render(poison.input,poison.test,&poison.mask,true);
    CompareRgb(controlOutput,poisonOutput,edge.viewport,true,"native tensor and flux taps clamp both source and mask viewport poison");
    SavePng(wic,root/L"viewport-poison-output.png",poisonOutput,true,true); ++count;
    return count;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        Need(argc==3 || (argc==4 && std::wstring(argv[3])==L"--strict-quality"),"usage: fxaa_probe.exe candidate.hlsl output-directory [--strict-quality]");
        const fs::path hlsl=fs::absolute(argv[1]), directory=fs::absolute(argv[2]); fs::create_directories(directory);
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
        Need(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"initialize image encoder");
        ComPtr<IWICImagingFactory> wic; Need(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))),"WIC encoder");
        Gpu gpu(hlsl);
        std::ofstream csv(directory/L"metrics.csv"); csv<<std::setprecision(10)
            <<"case,transparent,edge_pixels,point_rmse,output_rmse,point_edge_rmse,output_edge_rmse,soft_pixels,alpha_soft_pixels,changed_pixels,premult_error,flat_changed,nonfinite,out_of_range\n";
        Image alphaControl, edgeControl, flatControl;
        double aggregatePoint=0,aggregateOutput=0; unsigned qualityCases=0;
        const auto cases=Cases();
        for(const auto& c:cases) {
            const auto input=Generate(c), reference=Generate(c,true), output=gpu.Render(input,c); const auto m=Analyze(c,input,output,reference);
            const auto folder=directory/c.name; fs::create_directories(folder);
            SavePng(wic.Get(),folder/L"input.png",input,c.transparent); SavePng(wic.Get(),folder/L"ideal-16x16.png",reference,c.transparent);
            SavePng(wic.Get(),folder/L"output.png",output,c.transparent); SaveFloat(folder/L"output.rgba32f",output);
            if(c.transparent) { SavePng(wic.Get(),folder/L"output-checker.png",output,true,true); SavePng(wic.Get(),folder/L"output-alpha.png",output,true,false,true); }
            Image difference(output.size());
            for(unsigned i=0;i<output.size();++i) {
                double error=0; for(unsigned channel=0;channel<(c.transparent?4u:3u);++channel) error+=std::abs(output[i].data()[channel]-reference[i].data()[channel]);
                difference[i]={Clamp(float(error)*2),0,0,1};
            }
            SavePng(wic.Get(),folder/L"error-heat.png",difference,false);
            csv<<c.name<<','<<c.transparent<<','<<m.edgePixels<<','<<std::sqrt(m.pointMse)<<','<<std::sqrt(m.outputMse)<<','
               <<std::sqrt(m.pointEdgeMse)<<','<<std::sqrt(m.outputEdgeMse)<<','<<m.softPixels<<','<<m.alphaSoftPixels<<','<<m.changedPixels<<','
               <<m.premultError<<','<<m.flatChanged<<','<<m.nonfinite<<','<<m.outOfRange<<'\n';
            Verify(m.nonfinite==0 && m.outOfRange==0,c.name+": finite normalized RGBA output");
            if(c.shape==Shape::Solid) Verify(m.flatChanged==0,c.name+": flat RGBA8 pixels remain exact");
            if(c.transparent) {
                Verify(m.premultError<.00002 && m.premultViolations==0,c.name+": RGB and alpha use the same premultiplied weights");
                if(c.shape!=Shape::Solid) Verify(m.alphaSoftPixels>8,c.name+": transparent black/color edge receives alpha antialiasing");
            }
            if(c.quality) { aggregatePoint+=m.pointEdgeMse; aggregateOutput+=m.outputEdgeMse; ++qualityCases; }
            if(c.name=="mme-alpha-control") alphaControl=output;
            if(c.name=="mme-alpha-noise") CompareRgb(alphaControl,output,c.viewport,false,"opaque RGB ignores arbitrary MME alpha");
            if(c.name=="viewport-edge-control") edgeControl=output;
            if(c.name=="viewport-edge-poison") CompareRgb(edgeControl,output,c.viewport,true,"edge filter clamps viewport poison");
            if(c.name=="viewport-flat-control") flatControl=output;
            if(c.name=="viewport-flat-poison") CompareRgb(flatControl,output,c.viewport,true,"flat filter clamps viewport poison");
            std::printf("CASE %-34s edge_RMSE %.6f -> %.6f soft=%u alpha_soft=%u\n",c.name.c_str(),std::sqrt(m.pointEdgeMse),std::sqrt(m.outputEdgeMse),m.softPixels,m.alphaSoftPixels);
        }
        const auto nativeCases=CheckNativeMask(gpu,wic.Get(),directory);
        const double inputRmse=std::sqrt(aggregatePoint/qualityCases), outputRmse=std::sqrt(aggregateOutput/qualityCases);
        if(argc==4) Verify(outputRmse<inputRmse*.98,"aggregate geometric edge RMSE improves by at least 2 percent");
        std::ofstream summary(directory/L"summary.json"); summary<<std::setprecision(12)
            <<"{\n  \"gpu\": "<<JsonString(gpu.adapter)<<",\n  \"hlsl_source\": "<<JsonString(hlsl.u8string())
            <<",\n  \"width\": "<<kWidth<<", \"height\": "<<kHeight<<", \"reference_grid\": "<<kReferenceGrid
            <<",\n  \"cases\": "<<cases.size()+nativeCases<<", \"unmasked_cases\": "<<cases.size()<<", \"native_mask_cases\": "<<nativeCases
            <<", \"assertions\": "<<assertions<<", \"failures\": "<<failures
            <<",\n  \"point_edge_rmse\": "<<inputRmse<<", \"output_edge_rmse\": "<<outputRmse
            <<",\n  \"edge_rmse_ratio\": "<<(outputRmse/inputRmse)<<",\n  \"reference\": \"CPU independent ideal 16x16 geometry coverage; not MSAA or FXAA\"\n}\n";
        std::printf("%s cases=%zu assertions=%u failures=%u ideal_grid=%ux%u edge_RMSE %.6f -> %.6f ratio=%.5f\n",
            failures?"FAIL":"PASS",cases.size()+nativeCases,assertions,failures,kReferenceGrid,kReferenceGrid,inputRmse,outputRmse,outputRmse/inputRmse);
        return failures?1:0;
    } catch(const std::exception& e) { std::fprintf(stderr,"ERROR %s\n",e.what()); return 2; }
}
