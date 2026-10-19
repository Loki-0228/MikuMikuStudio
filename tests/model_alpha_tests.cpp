// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/transparent_triangles.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <random>
#include <limits>
#include <thread>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
template<class Index>
void CheckSortParity() {
    std::mt19937 random(932);
    D3DMATRIX matrix{};
    matrix._13 = .25f; matrix._23 = -.5f; matrix._33 = 1.f; matrix._43 = 2.f;
    for (UINT count : {0u, 1u, 255u, 256u, 257u, 2047u, 2048u, 4096u, 16384u}) {
        std::vector<Vertex> vertices(count * 3);
        std::vector<Index> actual(count * 3), expected;
        struct Ref { Index indices[3]; float depth; };
        std::vector<Ref> reference;
        for (UINT i=0; i<count; ++i) {
            Ref triangle{};
            for (UINT j=0; j<3; ++j) {
                const UINT index=i*3+j;
                auto& v=vertices[index];
                v.x=static_cast<float>(int(random()%33)-16);
                v.y=static_cast<float>(int(random()%33)-16);
                v.z=static_cast<float>(int(random()%33)-16);
                actual[index]=static_cast<Index>(index);
                triangle.indices[j]=actual[index];
                triangle.depth+=(v.x*matrix._13+v.y*matrix._23+v.z*matrix._33+matrix._43)/3.f;
            }
            reference.push_back(triangle);
        }
        std::stable_sort(reference.begin(), reference.end(),
            [](const Ref& a,const Ref& b){return a.depth>b.depth;});
        for (const auto& t:reference) for(auto i:t.indices) expected.push_back(i);
        if (count==0) continue; // null empty vectors use the existing rejection contract
        auto source = actual;
        Check(mikumikustudio::SortTransparentTriangles(vertices.data(),sizeof(Vertex),
            static_cast<UINT>(vertices.size()),actual.data(),static_cast<UINT>(actual.size()),matrix),
            "sort randomized triangles");
        Check(actual==expected,"radix matches stable reference including equal and negative depths");
        mikumikustudio::TransparentTriangleOrder cache;
        for (int repeat = 0; repeat < 3; ++repeat) {
            actual = source;
            Check(mikumikustudio::SortTransparentTriangles(vertices.data(), sizeof(Vertex),
                static_cast<UINT>(vertices.size()), actual.data(), static_cast<UINT>(actual.size()), matrix, &cache)
                && actual == expected, "cached order matches stable reference on repeated passes");
        }
        auto changedMatrix = matrix;
        for (int change = 0; change < 3; ++change) {
            if (change == 0) changedMatrix._33 = -1.f;
            if (change == 1) vertices[source[0]].z += 4096.f;
            if (change == 2) std::rotate(source.begin(), source.begin() + 3, source.end());
            actual = source; expected = source;
            Check(mikumikustudio::SortTransparentTriangles(vertices.data(), sizeof(Vertex),
                static_cast<UINT>(vertices.size()), expected.data(), static_cast<UINT>(expected.size()), changedMatrix),
                "sort changed reference inputs");
            Check(mikumikustudio::SortTransparentTriangles(vertices.data(), sizeof(Vertex),
                static_cast<UINT>(vertices.size()), actual.data(), static_cast<UINT>(actual.size()), changedMatrix, &cache)
                && actual == expected, "cache revalidates camera, deformed positions and source order");
        }
        const auto unchanged=actual;
        vertices[actual[0]].z=std::numeric_limits<float>::quiet_NaN();
        Check(!mikumikustudio::SortTransparentTriangles(vertices.data(),sizeof(Vertex),
            static_cast<UINT>(vertices.size()),actual.data(),static_cast<UINT>(actual.size()),matrix, &cache)
            && actual==unchanged,"nonfinite input leaves indices unchanged");
    }
}
void Quad(IDirect3DDevice9* device, float z, DWORD color) {
    const Vertex v[] = {
        {-0.5f,-0.5f,z,1,color,0,0}, {15.5f,-0.5f,z,1,color,1,0},
        {-0.5f,15.5f,z,1,color,0,1}, {15.5f,15.5f,z,1,color,1,1}};
    Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex))), "draw quad");
}
}

int main() {
    {
        using Cache = mikumikustudio::TransparentSortFrameCache;
        Cache frame;
        Check(Cache::active == &frame, "render scope installs sort cache");
        const auto entry = frame.EntryFor(&frame, &frame, 0, 300);
        Check(entry != Cache::missing && frame.EntryFor(&frame, &frame, 0, 300) == entry,
              "same material reuses entry");
        Check(frame.EntryFor(&frame, &frame, 0, 303) == Cache::missing,
              "changed material range falls back");
        Check(frame.EntryFor(&frame, &frame, 300, 16000000) == Cache::missing,
              "oversized cache request falls back without allocation");
        { Cache disabled(false); Check(Cache::active == nullptr, "native rendering disables cache"); }
        Check(Cache::active == &frame, "nested scope restores parent");
    }
    Check(mikumikustudio::TransparentSortFrameCache::active == nullptr,
          "cache lifetime ends with frame");
    std::thread workers[4];
    for (auto& worker:workers) worker=std::thread([] { CheckSortParity<unsigned int>(); });
    for (auto& worker:workers) worker.join();
    CheckSortParity<unsigned short>();
    CheckSortParity<unsigned int>();
    HWND window = CreateWindowW(L"STATIC", L"Alpha regression", WS_POPUP,
                                0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window != nullptr, "create hidden render target window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth=16; pp.BackBufferHeight=16; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D16;
    IDirect3DDevice9* device=nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)), "create real render device");
    device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);
    device->SetRenderState(D3DRS_ALPHAREF,1);
    device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATEREQUAL);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    IDirect3DTexture9* texture=nullptr;
    Check(SUCCEEDED(device->CreateTexture(4,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)), "alpha texture");
    D3DLOCKED_RECT lock{};
    texture->LockRect(0,&lock,nullptr,0);
    auto* pixels=static_cast<DWORD*>(lock.pBits);
    pixels[0]=0x00000000; pixels[1]=0x40000000; pixels[2]=0x80000000; pixels[3]=0xFF000000;
    texture->UnlockRect(0);
    unsigned char tag[4]{};
    mikumikustudio::CacheTextureColorAndAlpha(texture, tag);
    Check((tag[3] & mikumikustudio::kTextureUsesAlpha) != 0, "texture alpha classification");
    device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    IDirect3DSurface9* back=nullptr; device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    IDirect3DSurface9* read=nullptr;
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)), "readback");
    for (bool split : {false,true}) {
        device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFFFFFFFF,1,0);
        device->BeginScene();
        int frontDraws=0, rearDraws=0;
        const auto draw = [&](mikumikustudio::ModelAlphaPass pass) {
            if (mikumikustudio::ModelMaterialMatchesPass(pass,true)) {
                device->SetTexture(0,texture); Quad(device,0.25f,0xFFFFFFFF);
                ++frontDraws;
            }
            if (mikumikustudio::ModelMaterialMatchesPass(pass,false)) {
                device->SetTexture(0,nullptr); Quad(device,0.75f,0xFF0000FF);
                ++rearDraws;
            }
        };
        if (split) {
            draw(mikumikustudio::ModelAlphaPass::Opaque);
            draw(mikumikustudio::ModelAlphaPass::Translucent);
        } else draw(mikumikustudio::ModelAlphaPass::Original);
        Check(frontDraws==1 && rearDraws==1, "each material is submitted only once");
        // An inverted-hull outline behind the front hair must not paint over
        // its soft alpha pixels. With translucent ZWRITE disabled this is black.
        device->SetTexture(0,nullptr); Quad(device,0.5f,0xFF000000);
        device->EndScene();
        Check(SUCCEEDED(device->GetRenderTargetData(back,read)), "read rendered pixels");
        read->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        const auto* row=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+8*lock.Pitch);
        Check((row[1]&0xFFFFFF)==0, "zero-alpha pixels do not write depth over rear geometry");
        Check((row[14]&0xFFFFFF)==0, "opaque black surface still occludes rear surface");
        if (split) {
            Check((row[6]&0xFFFF00)==0 && (row[10]&0xFFFF00)==0,
                  "partial alpha blends with rear blue instead of white clear color");
            Check((row[6]&255)>170 && (row[10]&255)>110,
                  "soft alpha coverage retained and rear outline remains occluded");
        } else Check((row[6]&0xFF0000)!=0, "old single pass reproduces white fringe");
        read->UnlockRect();
        DWORD value=0; device->GetRenderState(D3DRS_ZWRITEENABLE,&value);
        Check(value==TRUE,"depth-write state preserved");
        device->GetRenderState(D3DRS_ALPHAREF,&value); Check(value==1,"alpha threshold preserved");
    }
    // The clothing's front and rear surfaces share one material/texture. A
    // material-only partition cannot fix this: its near triangles may be first.
    texture->LockRect(0,&lock,nullptr,0);
    static_cast<DWORD*>(lock.pBits)[3]=0xFFFFFFFF;
    texture->UnlockRect(0);
    const Vertex layers[] = {
        {-.5f,-.5f,.25f,1,0xFFFFFFFF,.375f,0}, {15.5f,-.5f,.25f,1,0xFFFFFFFF,.375f,0},
        {-.5f,15.5f,.25f,1,0xFFFFFFFF,.375f,0}, {15.5f,15.5f,.25f,1,0xFFFFFFFF,.375f,0},
        {-.5f,-.5f,.75f,1,0xFF0000FF,.875f,0}, {15.5f,-.5f,.75f,1,0xFF0000FF,.875f,0},
        {-.5f,15.5f,.75f,1,0xFF0000FF,.875f,0}, {15.5f,15.5f,.75f,1,0xFF0000FF,.875f,0}};
    D3DMATRIX worldView{};
    worldView._11=worldView._22=worldView._33=worldView._44=1;
    for (bool sort : {false,true}) {
        unsigned short indices[]={0,1,2,2,1,3,4,5,6,6,5,7};
        if (sort) Check(mikumikustudio::SortTransparentTriangles(layers,sizeof(Vertex),8,
            indices,12,worldView), "sort transparent triangles");
        device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xFFFFFFFF,1,0);
        device->BeginScene();
        device->SetTexture(0,texture);
        Check(SUCCEEDED(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,8,4,
            indices,D3DFMT_INDEX16,layers,sizeof(Vertex))), "draw overlapping surfaces in one material");
        device->SetTexture(0,nullptr); Quad(device,.5f,0xFF000000);
        device->EndScene();
        Check(SUCCEEDED(device->GetRenderTargetData(back,read)), "read same-material alpha result");
        read->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        const auto* row=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+8*lock.Pitch);
        if (sort) Check((row[8]&0xFFFF00)==0 && (row[8]&255)>170,
            "same-material white fringe removed without exposing rear black outline");
        else Check((row[8]&0xFF0000)!=0, "unsorted same-material triangles reproduce white fringe");
        read->UnlockRect();
    }
    unsigned int wideIndices[]={0,1,2,4,5,6};
    Check(mikumikustudio::SortTransparentTriangles(layers,sizeof(Vertex),8,wideIndices,6,worldView)
          && wideIndices[0]==4, "32-bit indices sort far surface first");
    worldView._33=-1;
    Check(mikumikustudio::SortTransparentTriangles(layers,sizeof(Vertex),8,wideIndices,6,worldView)
          && wideIndices[0]==0, "camera reversal changes triangle order");
    wideIndices[0]=8;
    Check(!mikumikustudio::SortTransparentTriangles(layers,sizeof(Vertex),8,wideIndices,6,worldView),
          "invalid vertex index rejected before reading vertex data");
    texture->LockRect(0,&lock,nullptr,0);
    pixels=static_cast<DWORD*>(lock.pBits);
    for (int i=0;i<4;++i) pixels[i]=0xFF112233;
    texture->UnlockRect(0);
    mikumikustudio::CacheTextureColorAndAlpha(texture,tag);
    Check(tag[0]==0x11 && tag[1]==0x22 && tag[2]==0x33, "toon color metadata preserved");
    Check(tag[3]==mikumikustudio::kTextureAlphaKnown, "reload updates opaque texture classification");
    DWORD paddedPixels[]={0xFFFFFFFF,0xFFFFFFFF,0,0xFFFFFFFF,0x80FFFFFF,0};
    D3DLOCKED_RECT padded{3*sizeof(DWORD),paddedPixels};
    Check(mikumikustudio::TexturePixelsUseAlpha(padded,2,2), "alpha scan respects pitch and later rows");
    paddedPixels[4]=0xFFFFFFFF;
    Check(!mikumikustudio::TexturePixelsUseAlpha(padded,2,2), "padding is not mistaken for alpha texels");
    // Material classification: transparent ordering only covers materials
    // that can emit visible non-opaque fragments. An authored alpha of exactly
    // zero emits nothing - the alpha test above keeps it from writing depth -
    // so hidden helper materials stay out of the sort and peel sets even when
    // their texture carries alpha.
    Check(!mikumikustudio::MaterialAlphaVisible(
              mikumikustudio::EffectiveMaterialAlpha(0.0f, false)),
          "authored alpha 0 needs no transparent ordering");
    Check(mikumikustudio::MaterialAlphaVisible(
              mikumikustudio::EffectiveMaterialAlpha(1.0f / 255.0f, false)),
          "very low non-zero alpha still needs transparent ordering");
    Check(mikumikustudio::MaterialAlphaVisible(
              mikumikustudio::EffectiveMaterialAlpha(0.8f, false)),
          "material alpha below one needs transparent ordering");
    Check(mikumikustudio::MaterialAlphaVisible(
              mikumikustudio::EffectiveMaterialAlpha(1.0f, false)),
          "opaque material alpha still defers to the texture alpha term");
    // The translucent-display override forces 0.5 alpha on every material, so
    // it lifts even an authored alpha-0 material back into the translucent set.
    Check(mikumikustudio::MaterialAlphaVisible(
              mikumikustudio::EffectiveMaterialAlpha(0.0f, true)),
          "translucent display keeps alpha-0 materials translucent");
    // The hidden material keeps its draw: the opaque pass submits it exactly
    // once, so an MME effect intercepting that draw still runs.
    Check(mikumikustudio::ModelMaterialMatchesPass(
              mikumikustudio::ModelAlphaPass::Opaque, false) &&
              !mikumikustudio::ModelMaterialMatchesPass(
                  mikumikustudio::ModelAlphaPass::Translucent, false),
          "hidden material draw submits once through the opaque pass");
    read->Release(); back->Release(); texture->Release(); device->Release(); d3d->Release();
    DestroyWindow(window);
    std::puts("Transparent model pixel regressions passed");
}
