// Run the production temporal shader on controlled GPU inputs. This verifies
// the lookup and rejection rules independently of scene-dependent screenshots.
void CheckTemporalResolveShader() {
    using Microsoft::WRL::ComPtr;
    constexpr unsigned side=32;
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    HMODULE runtime=LoadLibraryW((std::wstring(system)+L"\\d3d9.dll").c_str());
    Check(runtime!=nullptr,"load native runtime for temporal shader checks");
    const auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime,"Direct3DCreate9"));
    ComPtr<IDirect3D9> factory;factory.Attach(create?create(D3D_SDK_VERSION):nullptr);
    Check(factory!=nullptr,"create temporal shader test factory");
    HWND window=CreateWindowW(L"STATIC",L"Hidden temporal shader regression",WS_POPUP,0,0,side,side,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=side;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    Check(SUCCEEDED(factory->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)),
        "create hidden GPU temporal shader device");
    ComPtr<ID3DBlob> code,error;
    Check(SUCCEEDED(graphics_runtime::Compile(antialias::kShader,sizeof(antialias::kShader)-1,
        "antialias-temporal-test",nullptr,nullptr,"temporal","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)),
        "compile production motion-vector temporal shader");
    ComPtr<IDirect3DPixelShader9> shader;
    Check(SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&shader)),
        "create production temporal shader");
    ComPtr<IDirect3DTexture9> scene,history,motion,depth;
    for(auto texture:{scene.GetAddressOf(),history.GetAddressOf()})Check(SUCCEEDED(device->CreateTexture(side,side,1,0,
        D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture,nullptr)),"allocate color test input");
    for(auto texture:{motion.GetAddressOf(),depth.GetAddressOf()})Check(SUCCEEDED(device->CreateTexture(side,side,1,0,
        D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,texture,nullptr)),"allocate motion/depth test input");
    const auto colors=[&](IDirect3DTexture9* texture,bool current,bool flat) {
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(texture->LockRect(0,&lock,nullptr,0)),"write temporal color input");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            unsigned value=current?(flat?51:(x==16?128:x<16?0:255)):x*8;
            auto* row=reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch);
            row[x]=D3DCOLOR_ARGB(255,value,value,value);
        }
        texture->UnlockRect(0);
    };
    const auto vectors=[&](float displacement,float previous,bool occluded) {
        for(auto* texture:{motion.Get(),depth.Get()}) {
            D3DLOCKED_RECT lock{};Check(SUCCEEDED(texture->LockRect(0,&lock,nullptr,0)),"write temporal geometry input");
            for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
                auto* at=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+x*4;
                at[0]=texture==motion.Get()?displacement:0;at[1]=0;
                at[2]=texture==motion.Get()?5.f:occluded?1.f:3.f;at[3]=previous;
            }
            texture->UnlockRect(0);
        }
    };
    ComPtr<IDirect3DSurface9> target,cpu;
    Check(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&target)) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(side,side,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr)),
        "allocate temporal shader readback");
    const auto render=[&](float weight=.875f,float currentJitter=0,float previousJitter=0) {
        const float constants[]={1.f/side,1.f/side,weight,0,.5f/side,.5f/side,
            (side-.5f)/side,(side-.5f)/side,currentJitter/side,0,.002f,.002f,previousJitter/side,0,0,0};
        device->SetPixelShaderConstantF(0,constants,4);
        device->SetTexture(0,scene.Get());device->SetTexture(1,history.Get());
        device->SetTexture(2,motion.Get());device->SetTexture(3,depth.Get());
        for(unsigned sampler=0;sampler<4;++sampler) {
            device->SetSamplerState(sampler,D3DSAMP_MINFILTER,sampler<2?D3DTEXF_LINEAR:D3DTEXF_POINT);
            device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,sampler<2?D3DTEXF_LINEAR:D3DTEXF_POINT);
            device->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
            device->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        }
        device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        device->SetVertexShader(nullptr);device->SetPixelShader(shader.Get());
        device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
        struct Vertex{float x,y,z,w,u,v;};
        const Vertex quad[]={{-.5f,-.5f,0,1,0,0},{side-.5f,-.5f,0,1,1,0},
            {-.5f,side-.5f,0,1,0,1},{side-.5f,side-.5f,0,1,1,1}};
        Check(SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex))) &&
            SUCCEEDED(device->EndScene()),"execute production temporal shader");
        Check(SUCCEEDED(device->GetRenderTargetData(target.Get(),cpu.Get())),"read actual temporal result");
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY)),"inspect temporal result");
        const auto value=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+16*lock.Pitch)[16]&255;
        cpu->UnlockRect();return int(value);
    };
    colors(scene.Get(),true,false);colors(history.Get(),false,false);
    vectors(-4.f/side,3,false);
    Check(std::abs(render()-100)<=1,"motion vector reprojects history four pixels to the previous object location");
    vectors(4.f/side,3,false);
    Check(std::abs(render()-156)<=1,"reverse motion reprojects in the opposite direction");
    vectors(-4.f/side,3,false);
    D3DLOCKED_RECT adjacent{};
    Check(SUCCEEDED(motion->LockRect(0,&adjacent,nullptr,0)),"write adjacent surface with unrelated motion");
    auto* nearSurface=reinterpret_cast<float*>(static_cast<char*>(adjacent.pBits)+16*adjacent.Pitch)+15*4;
    nearSurface[0]=4.f/side;nearSurface[2]=2;
    motion->UnlockRect(0);
    Check(std::abs(render()-100)<=1,"visible surface retains its own motion beside a nearer surface");
    vectors(-4.f/side,3,false);
    Check(std::abs(render(.96875f)-100)<=1,"moving geometry does not inherit the longer stationary history weight");
    vectors(-4.f/side,3,true);
    Check(std::abs(render()-128)<=1,"previous-depth mismatch rejects an occluder's stale history");
    vectors(-4.f/side,0,false);
    Check(std::abs(render()-128)<=1,"new geometry without previous positions rejects history");
    vectors(-4.f/side,3,false);
    const auto edgeCoverage=[&](float centerDepth) {
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(motion->LockRect(0,&lock,nullptr,0)),"write temporal silhouette coverage");
        auto* center=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+16*lock.Pitch)+16*4;
        center[2]=centerDepth;center[3]=0;
        auto* neighbor=center-4;neighbor[2]=2;neighbor[3]=3;
        motion->UnlockRect(0);
    };
    edgeCoverage(5);
    Check(std::abs(render()-128)<=1,"transparent fragments cannot borrow another surface's valid history");
    edgeCoverage(0);
    Check(std::abs(render()-100)<=1,"subpixel silhouette coverage retains history using tracked neighboring geometry");
    vectors(-2.f,3,false);
    Check(std::abs(render()-128)<=1,"history outside the prior viewport is rejected");
    vectors(-4.f/side,3,false);
    Check(std::abs(render(0)-128)<=1,"first frame does not sample uninitialized history");
    Check(std::abs(render(0,.5f)-192)<=1,"first frame removes current positive jitter before display");
    Check(std::abs(render(0,-.5f)-64)<=1,"first frame removes current negative jitter before display");
    vectors(-4.f/side,0,false);
    Check(std::abs(render(.875f,.5f)-192)<=1,"history rejection also removes current jitter");
    vectors(-4.f/side,3,false);
    D3DLOCKED_RECT prior{};
    Check(SUCCEEDED(depth->LockRect(0,&prior,nullptr,0)),"write shifted previous-depth fixture");
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
        reinterpret_cast<float*>(static_cast<char*>(prior.pBits)+y*prior.Pitch)[x*4+2]=x==18?3.f:1.f;
    depth->UnlockRect(0);
    Check(std::abs(render()-128)<=1,"unaligned previous depth rejects unrelated geometry");
    Check(std::abs(render(.875f,0,6)-100)<=1,"previous jitter aligns depth while resolved history stays on the fixed grid");
    vectors(-4.f/side,3,false);
    colors(scene.Get(),true,true);
    Check(std::abs(render()-51)<=1,"neighborhood clamp rejects obsolete material colors");
    colors(scene.Get(),true,false);
    Check(SUCCEEDED(history->LockRect(0,&prior,nullptr,0)),"write stationary history color");
    reinterpret_cast<DWORD*>(static_cast<char*>(prior.pBits)+16*prior.Pitch)[16]=D3DCOLOR_ARGB(255,64,64,64);
    history->UnlockRect(0);
    vectors(0,3,false);
    Check(std::abs(render(.96875f)-66)<=1,"stationary surface converges with the longer history weight");
    const auto previousPlane=[&](float z) {
        Check(SUCCEEDED(depth->LockRect(0,&prior,nullptr,0)),"write prior coverage plane");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
            reinterpret_cast<float*>(static_cast<char*>(prior.pBits)+y*prior.Pitch)[x*4+2]=z;
        depth->UnlockRect(0);
    };
    previousPlane(8);
    Check(std::abs(render()-72)<=1,"stationary thin edge includes farther background from the previous phase");
    previousPlane(0);
    Check(std::abs(render()-72)<=1,"stationary thin edge includes an uncovered previous phase");
    previousPlane(1);
    Check(std::abs(render()-128)<=1,"stationary newly exposed surface still rejects a nearer previous occluder");
    Check(SUCCEEDED(motion->LockRect(0,&adjacent,nullptr,0)),"write stationary occluder at neighboring coverage");
    nearSurface=reinterpret_cast<float*>(static_cast<char*>(adjacent.pBits)+16*adjacent.Pitch)+15*4;
    nearSurface[0]=0;nearSurface[2]=1;nearSurface[3]=1;
    motion->UnlockRect(0);
    Check(std::abs(render()-72)<=1,"stationary occluder still present beside the pixel contributes jitter coverage");
    Check(SUCCEEDED(motion->LockRect(0,&adjacent,nullptr,0)),"move the neighboring occluder away");
    nearSurface=reinterpret_cast<float*>(static_cast<char*>(adjacent.pBits)+16*adjacent.Pitch)+15*4;
    nearSurface[0]=1.f/side;
    motion->UnlockRect(0);
    Check(std::abs(render()-128)<=1,"moving occluder cannot be mistaken for stationary jitter coverage");
    const auto fringeMotion=[&](unsigned distance,float displacement,bool visibleUntracked=false) {
        Check(SUCCEEDED(motion->LockRect(0,&adjacent,nullptr,0)),"write softened silhouette motion footprint");
        for(unsigned y=0;y<side;++y)std::memset(static_cast<char*>(adjacent.pBits)+y*adjacent.Pitch,0,side*4*sizeof(float));
        auto* tracked=reinterpret_cast<float*>(static_cast<char*>(adjacent.pBits)+16*adjacent.Pitch)+(16-distance)*4;
        tracked[0]=displacement;tracked[2]=5;tracked[3]=3;
        if(visibleUntracked)reinterpret_cast<float*>(static_cast<char*>(adjacent.pBits)+16*adjacent.Pitch)[16*4+2]=5;
        motion->UnlockRect(0);
    };
    previousPlane(8);fringeMotion(2,0);
    Check(std::abs(render()-72)<=1,"two-pixel softened fringe retains stationary silhouette history");
    fringeMotion(3,0);
    Check(std::abs(render()-128)<=1,"background beyond the softened footprint cannot borrow distant history");
    fringeMotion(2,0,true);
    Check(std::abs(render()-128)<=1,"visible untracked fragment rejects the outer silhouette neighbor");
    previousPlane(1);fringeMotion(2,-4.f/side);
    Check(std::abs(render()-128)<=1,"moving softened fringe still rejects occluded history");
    for(unsigned sampler=0;sampler<4;++sampler)device->SetTexture(sampler,nullptr);
    device->SetPixelShader(nullptr);
    cpu.Reset();target.Reset();scene.Reset();history.Reset();motion.Reset();depth.Reset();shader.Reset();
    device.Reset();factory.Reset();DestroyWindow(window);FreeLibrary(runtime);
}
