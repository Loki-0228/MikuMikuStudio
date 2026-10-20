#include "mikumikustudio/pmm_paths.hpp"
// MAINTENANCE: regression-keep; hidden real-MME display, EMD and material-order checks.
// Exercise the real mapping dialog without displaying or activating a window.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/perf_timing.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/effect_display.hpp"
#include "mikumikustudio/effect_file_watch.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "../src/render/antialias_shader.hpp"
#include "../src/render/outline_multisample.hpp"
#include <atomic>
#include <MinHook.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <wrl/client.h>
#include <set>
#include <wincodec.h>
#include "mikumikustudio/image_sequence_export.hpp"
#include <cwctype>

using namespace mikumikustudio;
namespace mikumikustudio { void RegisterCameraState(MMDApp*, int); void RegisterSelfShadowState(MMDApp*, int); void RefreshAfterFrameApply(MMDApp*); void LayoutViewportPanels(MMDApp*); int ViewportToolAtPointForTest(MMDApp*); }
extern "C" char* ExpGetPmdFilename(int);
namespace {
void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(2);
}

unsigned emdEffectCompiles = 0;
bool projectImportAudit = false;
unsigned projectImportWarnings = 0;
unsigned effectPathDiagnostics = 0;
decltype(&runtime_log::WritePath) originalWritePath = nullptr;
void ObserveEffectPath(const char* label, const wchar_t* path) noexcept {
    if (std::strcmp(label, "MME_EFFECT_FILE") == 0 && path && *path) ++effectPathDiagnostics;
    originalWritePath(label, path);
}
struct ShaderMacro { const char* name; const char* definition; };
using CreateEffectFile = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR,
    const ShaderMacro*, void*, DWORD, void*, void**, void**);
CreateEffectFile originalCompile = nullptr;
unsigned fileEffectCompiles = 0;
std::vector<IUnknown*> sequenceClockEffects;
HRESULT WINAPI ObserveCompile(IDirect3DDevice9* device, LPCWSTR path,
    const ShaderMacro* macros, void* include, DWORD flags, void* pool,
    void** effect, void** errors) {
    const HRESULT result = originalCompile(device, path, macros, include, flags, pool, effect, errors);
    std::string utf8; text_encoding::EncodeExact(path, CP_UTF8, utf8);
    std::printf("COMPILE path=%s result=%08lx\n", utf8.c_str(), static_cast<unsigned long>(result));
    if (std::filesystem::path(path).filename() == L"assigned.fx" && SUCCEEDED(result)) ++emdEffectCompiles;
    if (SUCCEEDED(result)) ++fileEffectCompiles;
    if (SUCCEEDED(result) && std::filesystem::path(path).filename() == L"sequence-clock.fx") {
        auto* object = static_cast<IUnknown*>(*effect); object->AddRef();
        sequenceClockEffects.push_back(object);
    }
    return result;
}
decltype(&ShowWindow) originalShow;
BOOL WINAPI Hide(HWND window, int) { return originalShow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->x = create->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI Unexpected(HWND, LPCWSTR text, LPCWSTR, UINT type) {
    if (projectImportAudit) {
        ++projectImportWarnings;
        std::fwprintf(stderr, L"IMPORT warning: %ls\n", text);
        return IDOK;
    }
    if (std::getenv("MMS_CAMERA_FIXTURE") && (type & MB_ICONMASK) == MB_ICONINFORMATION) {
        std::puts("CAMERA acknowledged project compatibility information"); return IDOK;
    }
    std::fwprintf(stderr, L"FAIL unexpected dialog: %ls\n", text); std::exit(2);
}
int WINAPI UnexpectedAnsi(HWND, LPCSTR text, LPCSTR, UINT) {
    std::string utf8; text_encoding::EncodeExact(text_encoding::Display(text).c_str(), CP_UTF8, utf8);
    std::fprintf(stderr, "FAIL unexpected dialog: %s\n", utf8.c_str()); std::exit(2);
}
int tick = 0, rowsSeen = 0;
bool done = false;
bool bilingualFixture = false;
HWND editor = nullptr;
using DrawIndexed = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
DrawIndexed originalDraw = nullptr;
std::vector<unsigned> drawnMaterials;
using Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Present originalPresent = nullptr;
bool inspectOutlines = false;
unsigned outlineDraws = 0, outlineStateErrors = 0;
DWORD expectedOutlineAA = TRUE, observedOutlineAA = 0, observedOutlineSamples = 0;
bool captureRequested = false;
std::vector<std::uint32_t> capturedPixels;
unsigned captureWidth = 0, captureHeight = 0;
std::filesystem::path captureDirectory;
HRESULT WINAPI ObservePresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                              HWND window, const RGNDATA* region) {
    if (captureRequested) {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> back, resolved, cpu;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)), "get actual pre-Present backbuffer");
        D3DSURFACE_DESC desc{}; Check(SUCCEEDED(back->GetDesc(&desc)), "read backbuffer description");
        Check(desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8, "BGRA comparison format");
        auto* surface = back.Get();
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE,
                0, FALSE, &resolved, nullptr)) && SUCCEEDED(device->StretchRect(surface, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)),
                "resolve actual backbuffer without rebinding");
            surface = resolved.Get();
        }
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && SUCCEEDED(device->GetRenderTargetData(surface, cpu.Get())), "capture actual frame pixels");
        D3DLOCKED_RECT lock{}; Check(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock comparison pixels");
        captureWidth = desc.Width; captureHeight = desc.Height;
        capturedPixels.resize(std::size_t(desc.Width) * desc.Height);
        for (unsigned y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch);
            for (unsigned x = 0; x < desc.Width; ++x) capturedPixels[std::size_t(y) * desc.Width + x] = row[x] & 0xFFFFFF;
        }
        cpu->UnlockRect(); captureRequested = false;
    }
    return originalPresent(device, source, destination, window, region);
}
HRESULT WINAPI ObserveDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT min,
                            UINT vertices, UINT start, UINT primitives) {
    if (inspectOutlines && g_Block && g_Block->ActiveRenderPass() == AccessoryRenderPass::ModelOutline) {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> target;
        D3DSURFACE_DESC desc{}; DWORD aa = 0, mask = 0;
        const bool queried = SUCCEEDED(device->GetRenderTarget(0, &target)) &&
            SUCCEEDED(target->GetDesc(&desc)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &aa)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_MULTISAMPLEMASK, &mask));
        ++outlineDraws; observedOutlineAA = aa; observedOutlineSamples = desc.MultiSampleType;
        if (!queried || aa != expectedOutlineAA || desc.MultiSampleType == D3DMULTISAMPLE_NONE || mask != 0xFFFFFFFFu)
            ++outlineStateErrors;
    }
    if (g_Block && g_Block->ActiveRenderObject() == g_Block->SelectedModel() &&
        (g_Block->ActiveRenderPass() == AccessoryRenderPass::FixedFunction ||
         g_Block->ActiveRenderPass() == AccessoryRenderPass::Effect)) {
        const auto material = mdl::Mdl(g_Block->SelectedModel())->toonShared;
        if (drawnMaterials.empty() || drawnMaterials.back() != material) drawnMaterials.push_back(material);
    }
    return originalDraw(device, type, base, min, vertices, start, primitives);
}
void CheckOutlineAntialias(MMDApp* app) {
    app->EditMode() = ViewportEditMode::None;
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[82], &ObserveDraw, reinterpret_cast<void**>(&originalDraw)) == MH_OK &&
          MH_EnableHook(vtable[82]) == MH_OK, "observe outline draw states");
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe actual outline presentation");
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    app->CameraPositionX() = app->ViewOffsetX() = 0.f;
    app->CameraPositionY() = app->ViewOffsetY() = 18.f;
    app->CameraPositionZ() = 0.f; app->CameraDistance() = -16.f;
    app->CameraYaw() = app->CameraPitch() = app->CameraRoll() = 0.f;
    inspectOutlines = true;
    const auto frame = [&](const char* label, bool enabled) {
        const UINT menu = GetMenuState(GetMenu(app->MainWindow()), 277, MF_BYCOMMAND);
        Check(menu != UINT(-1) && ((menu & MF_CHECKED) != 0) == enabled, "AA menu matches requested state");
        expectedOutlineAA = enabled; outlineDraws = outlineStateErrors = 0;
        captureRequested = true;
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        std::printf("OUTLINE stage=%s aa=%lu samples=%lu draws=%u errors=%u size=%ux%u\n",
            label, observedOutlineAA, observedOutlineSamples, outlineDraws, outlineStateErrors, captureWidth, captureHeight);
        Check(outlineDraws > 0 && outlineStateErrors == 0, "every real outline draw retains AA selection and multisample target");
        Check(!captureRequested && !capturedPixels.empty(), "outline pixels captured at real Present");
        if (!captureDirectory.empty()) {
            std::filesystem::create_directories(captureDirectory);
            std::ofstream file(captureDirectory / (std::string(label) + ".bgra"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
            Check(bool(file), "write outline comparison pixels");
        }
    };
    frame("initial", true);
    CommandDispatch(nullptr, MAKEWPARAM(277, 0)); frame("disabled", false);
    Check(PostDeviceReset(app), "reset with AA disabled"); frame("disabled-reset", false);
    CommandDispatch(nullptr, MAKEWPARAM(277, 0)); frame("enabled", true);
    Check(PostDeviceReset(app), "reset with AA enabled"); frame("enabled-reset", true);
    for (int shadow : {0, 1, 2}) {
        app->SelfShadowMode() = shadow;
        const auto label = std::string("shadow-") + std::to_string(shadow);
        frame(label.c_str(), true);
    }
    for (auto precision : {TransparentSortPrecision::Low, TransparentSortPrecision::Off}) {
        app->TransparentMaterialSortPrecision() = precision;
        frame(precision == TransparentSortPrecision::Off ? "sort-off" : "sort-low", true);
    }
    app->state.englishUI = 1; RefreshMenuLanguage(app); frame("language", true);
    SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1200, 850,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Check(PostDeviceReset(app), "reset after resize"); frame("resized", true);
    inspectOutlines = false;
    MH_DisableHook(vtable[82]); MH_DisableHook(vtable[17]);
    Check(!IsWindowVisible(app->MainWindow()), "outline host stays hidden");
}
void CheckAlphaOrder(MMDApp* app) {
    const bool mmeLoaded = GetModuleHandleW(L"MMEffect.dll") != nullptr;
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    auto* target = vtable[82]; // IDirect3DDevice9::DrawIndexedPrimitive
    Check(MH_CreateHook(target, &ObserveDraw, reinterpret_cast<void**>(&originalDraw)) == MH_OK &&
          MH_EnableHook(target) == MH_OK, "observe actual D3D material draws");
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe actual pre-Present pixels");
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    for (int shadow : {0, 2}) {
        app->SelfShadowMode() = shadow;
        std::vector<std::uint32_t> firstOff;
        unsigned run = 0;
        for (auto precision : {TransparentSortPrecision::Off, TransparentSortPrecision::Low, TransparentSortPrecision::Off}) {
            app->TransparentMaterialSortPrecision() = precision;
            drawnMaterials.clear();
            captureRequested = true;
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
            std::printf("DRAW shadow=%d precision=%d:", shadow, int(precision));
            for (auto material : drawnMaterials) std::printf(" %u", material);
            std::puts("");
            const bool recordOrder = MMD_ORIGINAL_PROFILE ||
                (mmeLoaded && precision == TransparentSortPrecision::Off);
            const std::vector<unsigned> expected = recordOrder
                ? std::vector<unsigned>{0, 1} : std::vector<unsigned>{1, 0};
            Check(drawnMaterials == expected, "MME Off preserves authored material order; native and sorted passes retain their order");
            Check(!captureRequested && !capturedPixels.empty(), "capture came from a real Present");
            if (run == 0) firstOff = capturedPixels;
            if (run == 1 && mmeLoaded && !MMD_ORIGINAL_PROFILE)
                Check(firstOff != capturedPixels, "fixture visibly distinguishes authored and alpha-grouped material order");
            if (run == 2) Check(firstOff == capturedPixels, "On to Off preserves every frame RGB pixel");
            std::uint64_t hash = 14695981039346656037ull;
            for (auto pixel : capturedPixels) hash = (hash ^ pixel) * 1099511628211ull;
            std::printf("PIXELS shadow=%d run=%u precision=%d width=%u height=%u hash=%016llx\n",
                shadow, run, int(precision), captureWidth, captureHeight, static_cast<unsigned long long>(hash));
            if (!captureDirectory.empty()) {
                std::filesystem::create_directories(captureDirectory);
                const auto name = L"shadow-" + std::to_wstring(shadow) + L"-run-" + std::to_wstring(run) + L".bgra";
                std::ofstream file(captureDirectory / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
                Check(bool(file), "write actual comparison pixels");
            }
            ++run;
        }
    }
    MH_DisableHook(target);
    MH_DisableHook(vtable[17]);
    Check(!IsWindowVisible(app->MainWindow()), "material-order host stays hidden");
}
DrawIndexed surfaceDraw = nullptr;
std::set<std::pair<void*, int>> surfaceShaders;
std::set<unsigned> hiddenSurfaceMaterials;
unsigned blockedSurfaceDx12=0;
decltype(&LoadLibraryW) surfaceLoadLibrary=nullptr;
HMODULE WINAPI SurfaceLoadLibrary(LPCWSTR path){
    const wchar_t* name=std::wcsrchr(path,L'\\');name=name?name+1:path;
    if(_wcsicmp(name,L"d3d12.dll")==0){++blockedSurfaceDx12;SetLastError(ERROR_MOD_NOT_FOUND);return nullptr;}
    return surfaceLoadLibrary(path);
}
HRESULT WINAPI InspectSurfaceDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base,
    UINT min, UINT vertices, UINT start, UINT primitives) {
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> shader;
    device->GetPixelShader(&shader);
    const int pass = int(g_Block->ActiveRenderPass());
    const auto* activeModel=mdl::Mdl(g_Block->SelectedModel());
    if(activeModel && hiddenSurfaceMaterials.count(activeModel->toonShared))return S_OK;
    static bool dumped=false;
    if(pass==2 && !dumped) {
        dumped=true;
        Microsoft::WRL::ComPtr<IDirect3DVertexBuffer9> vb;
        Microsoft::WRL::ComPtr<IDirect3DIndexBuffer9> ib;
        UINT offset=0,stride=0;device->GetStreamSource(0,&vb,&offset,&stride);device->GetIndices(&ib);
        D3DVERTEXBUFFER_DESC vd{};D3DINDEXBUFFER_DESC id{};vb->GetDesc(&vd);ib->GetDesc(&id);
        void* data=nullptr;
        if(SUCCEEDED(vb->Lock(0,0,&data,D3DLOCK_READONLY))) {
            std::ofstream f(captureDirectory/"vertices.bin",std::ios::binary);f.write(static_cast<char*>(data),vd.Size);vb->Unlock();
        }
        if(SUCCEEDED(ib->Lock(0,0,&data,D3DLOCK_READONLY))) {
            std::ofstream f(captureDirectory/"indices.bin",std::ios::binary);f.write(static_cast<char*>(data),id.Size);ib->Unlock();
        }
        std::ofstream meta(captureDirectory/"geometry.txt");meta<<stride<<" "<<offset<<" "<<vertices<<" "<<int(id.Format)<<"\n";
        float constants[256][4]{};device->GetVertexShaderConstantF(0,constants[0],256);
        std::ofstream f(captureDirectory/"vs-constants.bin",std::ios::binary);f.write(reinterpret_cast<char*>(constants),sizeof(constants));
        D3DMATRIX m[3]{};device->GetTransform(D3DTS_WORLD,&m[0]);device->GetTransform(D3DTS_VIEW,&m[1]);device->GetTransform(D3DTS_PROJECTION,&m[2]);
        std::ofstream matrices(captureDirectory/"transforms.bin",std::ios::binary);matrices.write(reinterpret_cast<char*>(m),sizeof(m));
    }
    if(captureRequested) {
        const auto* model=mdl::Mdl(g_Block->SelectedModel());
        std::ofstream log(captureDirectory/"draws.csv",std::ios::app);
        log<<g_Block->SelfShadowMode()<<","<<g_Block->CameraYaw()<<","<<pass<<","<<model->toonShared<<","<<start<<","<<primitives<<"\n";
    }
    if (surfaceShaders.emplace(shader.Get(), pass).second) {
        DWORD z=0,write=0,cull=0,aa=0,blend=0,fill=0;
        device->GetRenderState(D3DRS_ZENABLE,&z);device->GetRenderState(D3DRS_ZWRITEENABLE,&write);
        device->GetRenderState(D3DRS_CULLMODE,&cull);device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS,&aa);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);device->GetRenderState(D3DRS_FILLMODE,&fill);
        std::printf("SURFACE DRAW shader=%zu pass=%d shadow=%d start=%u faces=%u z=%lu write=%lu cull=%lu aa=%lu blend=%lu fill=%lu\n",
            surfaceShaders.size(),pass,g_Block->SelfShadowMode(),start,primitives,z,write,cull,aa,blend,fill);
        if (shader) {
            UINT size=0;shader->GetFunction(nullptr,&size);std::vector<DWORD> bytes((size+3)/4);shader->GetFunction(bytes.data(),&size);
            using Disassemble=HRESULT(WINAPI*)(const DWORD*,BOOL,LPCSTR,void**);
            auto disassemble=reinterpret_cast<Disassemble>(GetProcAddress(d3dx::Get().module,"D3DXDisassembleShader"));
            void* assembly=nullptr;
            if (disassemble && SUCCEEDED(disassemble(bytes.data(),FALSE,nullptr,&assembly))) {
                using Data=const char*(WINAPI*)(void*);using Size=DWORD(WINAPI*)(void*);
                std::ofstream file(captureDirectory/("shader-"+std::to_string(surfaceShaders.size())+".txt"));
                file.write(d3dx::EffectMethod<Data,3>(assembly)(assembly),d3dx::EffectMethod<Size,4>(assembly)(assembly));
                static_cast<IUnknown*>(assembly)->Release();
            }
        }
    }
    DWORD previousCull=0;
    const bool singleSided=pass==2 && std::getenv("MMS_SURFACE_SINGLE_SIDED");
    if(singleSided){device->GetRenderState(D3DRS_CULLMODE,&previousCull);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW);}
    const HRESULT result=surfaceDraw(device,type,base,min,vertices,start,primitives);
    if(singleSided)device->SetRenderState(D3DRS_CULLMODE,previousCull);
    return result;
}
void CheckSurfaceGrid(MMDApp* app) {
    unsigned width=3840,height=2160;
    if(const char* size=std::getenv("MMS_SURFACE_SIZE"))Check(std::sscanf(size,"%u,%u",&width,&height)==2,"read test resolution");
    const auto* testModel=mdl::Mdl(app->SelectedModel());
    for(unsigned i=0;i<testModel->materialCount;++i){
        auto name=text_encoding::MaterialName(*testModel,i,false);
        auto english=text_encoding::MaterialName(*testModel,i,true);
        std::string utf8;text_encoding::EncodeExact((name+L" / "+english).c_str(),CP_UTF8,utf8);
        std::printf("MATERIAL %u %s\n",i,utf8.c_str());
        std::transform(english.begin(),english.end(),english.begin(),[](wchar_t ch){return std::towlower(ch);});
        if(std::getenv("MMS_SURFACE_HIDE_HAIR") && (name.find(L"髪")!=std::wstring::npos || english.find(L"hair")!=std::wstring::npos))hiddenSurfaceMaterials.insert(i);
    }
    auto* renderer=app->Renderer();
    app->RenderWidth()=width;app->RenderHeight()=height;
    renderer->presentParameters.BackBufferWidth=renderer->screenWidth=width;
    renderer->presentParameters.BackBufferHeight=renderer->screenHeight=height;
    if(const char* quality=std::getenv("MMS_SURFACE_AA_QUALITY"))renderer->presentParameters.MultiSampleQuality=std::atoi(quality);
    Check(PostDeviceReset(app),"create 3840x2160 production backbuffer");
    std::printf("SURFACE AA type=%u quality=%lu depth=%u\n",unsigned(renderer->presentParameters.MultiSampleType),renderer->presentParameters.MultiSampleQuality,unsigned(renderer->presentParameters.AutoDepthStencilFormat));
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe surface presentation");
    app->EditMode() = ViewportEditMode::Camera;
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    app->CameraPositionX() = app->ViewOffsetX() = 0.f;
    app->CameraPositionY() = app->ViewOffsetY() = 10.f;
    app->CameraPositionZ() = 0.f; app->CameraDistance() = -28.f;
    app->CameraPitch() = app->CameraRoll() = 0.f;
    if (const char* camera = std::getenv("MMS_SURFACE_CAMERA")) {
        float x=0,y=10,z=0,d=-28;
        Check(std::sscanf(camera,"%f,%f,%f,%f",&x,&y,&z,&d)==4,"read test camera");
        app->CameraPositionX()=app->ViewOffsetX()=x;
        app->CameraPositionY()=app->ViewOffsetY()=y;
        app->CameraPositionZ()=z;app->CameraDistance()=d;
    }
    if (std::getenv("MMS_SURFACE_NO_EDGE")) mdl::Mdl(app->SelectedModel())->edgeScale=0;
    if (std::getenv("MMS_SURFACE_NO_AA")) CommandDispatch(nullptr,MAKEWPARAM(277,0));
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    std::filesystem::create_directories(captureDirectory);
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    auto library=LoadLibraryW((std::filesystem::path(system)/L"d3d9.dll").c_str());
    auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(library,"Direct3DCreate9"));
    Microsoft::WRL::ComPtr<IDirect3D9> raw;raw.Attach(create(D3D_SDK_VERSION));
    Microsoft::WRL::ComPtr<IDirect3DDevice9> dummy;
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=app->MainWindow();
    pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(raw->CreateDevice(0,D3DDEVTYPE_HAL,app->MainWindow(),D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dummy)),"obtain native surface draw entry");
    auto** native=*reinterpret_cast<void***>(dummy.Get());
    Check(MH_CreateHook(native[82],&InspectSurfaceDraw,reinterpret_cast<void**>(&surfaceDraw))==MH_OK && MH_EnableHook(native[82])==MH_OK,"inspect actual surface shaders");
    for (int shadow : {0, 2}) for (int yaw : {0, 1, 2}) {
        app->SelfShadowMode() = shadow;
        const char* baseYaw=std::getenv("MMS_SURFACE_YAW");
        app->CameraYaw() = (baseYaw?float(std::atof(baseYaw)):0.f) + yaw * .035f;
        for (int i = 0; i < 3; ++i) {
            const bool legacy=std::getenv("MMS_SURFACE_LEGACY_VIEWPORT")!=nullptr;
            D3DVIEWPORT9 viewport=legacy?D3DVIEWPORT9{312,25,618,464,0,1}:D3DVIEWPORT9{0,0,width,height,0,1};
            app->ViewportRect()={LONG(viewport.X),LONG(viewport.Y),LONG(viewport.X+viewport.Width),LONG(viewport.Y+viewport.Height)};
            renderer->aspectRatio=float(viewport.Width)/viewport.Height;
            Check(SUCCEEDED(renderer->device->SetViewport(&viewport)),"install 4K viewport");
            app->EditMode()=ViewportEditMode::Camera;
            app->state.selfShadowEnabled=shadow!=0;
            captureRequested = i == 2;
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        }
        Check(!captureRequested && !capturedPixels.empty(), "capture surface pixels");
        const auto name = "surface-" + std::to_string(shadow) + "-" + std::to_string(yaw);
        std::ofstream file(captureDirectory / (name + ".bgra"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
        std::ofstream size(captureDirectory / (name + ".txt"));
        size << captureWidth << " " << captureHeight;
        std::printf("SURFACE shadow=%d yaw=%d width=%u height=%u\n", shadow, yaw, captureWidth, captureHeight);
    }
    MH_DisableHook(vtable[17]);
    MH_DisableHook(native[82]);
    const auto status=skinning::GetStatus();
    std::printf("SURFACE SKINNING cpu=%llu gpu=%llu blockedDx12=%u\n",static_cast<unsigned long long>(status.cpuUpdates),static_cast<unsigned long long>(status.gpuUpdates),blockedSurfaceDx12);
    if(std::getenv("MMS_SURFACE_MISSING_DX12"))Check(blockedSurfaceDx12>0 && status.gpuUpdates==0 && status.cpuUpdates>0,"real missing-DX12 CPU fallback rendered model");
    dummy.Reset();raw.Reset();FreeLibrary(library);
}
std::uint64_t PaintHash(HWND list) {
    RECT rect{}; GetClientRect(list, &rect);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr); void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Check(bitmap && pixels, "allocate control readback");
    HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    std::uint64_t hash = 14695981039346656037ull;
    const auto* bytes = static_cast<unsigned char*>(pixels);
    for (int i = 0; i < rect.right * rect.bottom * 4; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    return hash;
}
void CheckDisplay(HWND list) {
    wchar_t original[1024]{};
    LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = 2;
    item.pszText = original; item.cchTextMax = 1024;
    Check(SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)), "read original material item");
    const auto identity = item.lParam;
    const std::wstring stored = original;
    auto* model = mdl::Mdl(g_Block->SelectedModel());
    const auto native = text_encoding::MaterialName(*model, 0, false);
    const auto english = text_encoding::MaterialName(*model, 0, true);
    SetModelInfoLanguagePreference(kModelInfoLanguageEnglish);
    SetModelNameAutoTranslation(false);
    Check(EffectListDisplayLabel(g_Block, list, 1) == L"Display Model", "MME model label uses metadata");
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: CustomArm", "English material metadata is displayed");
    const auto rawHash = PaintHash(list);
    SendMessageW(g_Block->MainWindow(), WM_COMMAND, 311, 0);
    Check(gModelNameAutoTranslation, "enable translation through actual menu");
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: arm_L", "MME displays dictionary translation");
    const auto translatedHash = PaintHash(list);
    Check(rawHash != translatedHash, "actual MME control pixels change with translated labels");
    SendMessageW(g_Block->MainWindow(), WM_COMMAND, 311, 0);
    Check(!gModelNameAutoTranslation, "translation remains switchable with PMX and MME loaded");
    Check(PaintHash(list) == rawHash, "disabling translation restores original display pixels");
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
    g_Block->EnglishUI() = kUiChinese;
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: 左腕", "Chinese auto preference uses Japanese material name");
    wchar_t finalText[1024]{}; item.pszText = finalText;
    SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    Check(stored == finalText && item.lParam == identity, "MME stored text and assignment identity remain unchanged");
    Check(text_encoding::MaterialName(*model, 0, false) == native &&
          text_encoding::MaterialName(*model, 0, true) == english,
          "raw model material names remain unchanged");
    Check((GetMenuState(GetMenu(g_Block->MainWindow()), 311, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0,
          "translation preference is enabled after loading PMX with MME");
    std::printf("DISPLAY hashes original=%llx translated=%llx\n", rawHash, translatedHash);
}
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    ++tick;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        HMENU menu = GetMenu(window);
        if (menu && GetMenuState(menu, 40028, MF_BYCOMMAND) != UINT(-1) &&
            GetMenuState(menu, 40007, MF_BYCOMMAND) != UINT(-1)) editor = window;
        return TRUE;
    }, 0);
    if (editor) {
        EnumChildWindows(editor, [](HWND child, LPARAM) -> BOOL {
            wchar_t cls[64]{}; GetClassNameW(child, cls, 64);
            if (_wcsicmp(cls, WC_LISTVIEWW)) return TRUE;
            const int count = ListView_GetItemCount(child);
            std::printf("LIST tick=%d id=%d rows=%d\n", tick, GetDlgCtrlID(child), count);
            for (int row = 0; row < count; ++row) {
                wchar_t text[1024]{};
                LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_INDENT;
                item.iItem = row; item.pszText = text; item.cchTextMax = 1024;
                SendMessageW(child, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
                std::string utf8; text_encoding::EncodeExact(text, CP_UTF8, utf8);
                std::printf("ROW %d indent=%d data=%llx text=%s\n", row, item.iIndent,
                    static_cast<unsigned long long>(item.lParam), utf8.c_str());
                ++rowsSeen;
            }
            if (tick == 1 && count > 1) ListView_SetItemState(child, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            if (tick == 2 && bilingualFixture) { Check(count == 3, "fixture has one expanded material"); CheckDisplay(child); }
            return TRUE;
        }, 0);
        if (tick == 1) PostMessageW(editor, WM_COMMAND, 40006, 0);
        else { PostMessageW(editor, WM_COMMAND, 40011, 0); done = true; KillTimer(nullptr, timer); }
    }
    if (tick > 10) { std::puts("FAIL mapping dialog not found"); std::exit(2); }
}
#include "viewport_render_scale_scene_checks.hpp"
#include "viewport_scale_performance_checks.hpp"
#include "viewport_scale_dialog_checks.hpp"
#include "viewport_panel_checks.hpp"
#include "antialias_scene_checks.hpp"
#include "antialias_startup_checks.hpp"
#include "outline_coverage_scene_checks.hpp"
#include "mme_ground_shadow_checks.hpp"
#include "effect_file_watch_checks.hpp"
#include "pmm_version_effect_checks.hpp"
#include "camera_motion_scene_checks.hpp"
#include "mme_model_adapt_checks.hpp"
#include "project_import_checks.hpp"
#include "image_visibility_checks.hpp"
ULONGLONG startupClock = 0;
void StartupLog(const char* format, ...) noexcept {
    std::printf("STARTUP %llu ", GetTickCount64() - startupClock);
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args);
    std::putchar('\n');
}
}
#include "mme_sequence_clock_checks.hpp"
#include "model_edit_controls_checks.hpp"
#include "mme_scene_capture.hpp"
int wmain(int argc, wchar_t** argv) {
    Check(argc >= 2 && argc <= 4, "model path supplied");
    bilingualFixture = argc == 3 && std::wcscmp(argv[2], L"--bilingual") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize hidden-window hook");
    const bool shaderStartup = argc == 3 && std::wcscmp(argv[2], L"--shader-startup") == 0;
    if(shaderStartup)InstallAntialiasCompilerGate();
    const bool startupProbe = argc == 3 && std::wcscmp(argv[2], L"--startup-probe") == 0;
    startupClock = GetTickCount64();
    if (startupProbe) Check(MH_CreateHook(&runtime_log::Write, &StartupLog, nullptr) == MH_OK,
        "observe startup stages");
    Check(MH_CreateHook(&ShowWindow, &Hide, reinterpret_cast<void**>(&originalShow)) == MH_OK, "hook ShowWindow");
    Check(MH_CreateHook(&MessageBoxW, &Unexpected, nullptr) == MH_OK, "guard unexpected dialogs");
    Check(MH_CreateHook(&MessageBoxA, &UnexpectedAnsi, nullptr) == MH_OK, "guard legacy dialogs");
    if (argc == 4 && std::wcscmp(argv[2], L"--emm-load") == 0)
        Check(MH_CreateHook(&runtime_log::WritePath, &ObserveEffectPath,
            reinterpret_cast<void**>(&originalWritePath)) == MH_OK, "observe native effect path diagnostics");
    if(std::getenv("MMS_SURFACE_MISSING_DX12"))Check(MH_CreateHook(&LoadLibraryW,&SurfaceLoadLibrary,reinterpret_cast<void**>(&surfaceLoadLibrary))==MH_OK,"simulate missing DX12 in test process");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable hidden-window guards");
    if(std::getenv("MMS_VIEWPORT_PERF"))runtime_log::Initialize();
    HHOOK guard = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(guard != nullptr, "prevent window activation");
    auto* app = new MMDApp; g_Block = app;
    app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "start production host");
    if (argc == 4 && std::wcscmp(argv[2], L"--project-import") == 0) {
        CheckProjectImport(app, argv[1], argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--import-template") == 0) {
        const auto directory = std::filesystem::path(argv[3]);
        auto path = directory / L"empty.pmm";
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str()); SaveSceneFile(app);
        LoadModelFile(app, argv[1], false);
        Check(app->SelectedModel() != nullptr, "load import fixture model");
        path = directory / L"model-cold.pmm";
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str()); SaveSceneFile(app);
        for (int i = 0; i < 3; ++i) { app->state.messageSeen = 10; FrameDriver(app); }
        path = directory / L"model.pmm";
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str()); SaveSceneFile(app);
        app->state.messageSeen = 10; FrameDriver(app);
        ResetAppState(app);
        LoadAccessoryFile((directory / L"fixture.x").c_str());
        Check(app->AccessorySlot(0) != nullptr, "load real accessory import fixture");
        for (int i = 0; i < 3; ++i) { app->state.messageSeen = 10; FrameDriver(app); }
        path = directory / L"accessory.pmm";
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str()); SaveSceneFile(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if(shaderStartup) {
        CheckAntialiasStartup(app);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (startupProbe) {
        StartupLog("UI ready");
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        StartupLog("First empty frame");
        LoadModelFile(app, argv[1], false);
        StartupLog("Model loaded");
        for (int i = 0; i < 20; ++i) {
            const auto begin = GetTickCount64();
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
            StartupLog("Frame %d took %llu ms", i, GetTickCount64() - begin);
            MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(16);
        }
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-preference") == 0) {
        Check(app->antialiasing.algorithm == antialias::Algorithm::Fxaa && app->antialiasing.enabled,
            "restart restores selected algorithm and enable state");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-menu") == 0) {
        CheckAntialiasMenus(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-temporal") == 0) {
        CheckTemporalResolveShader();
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    const bool emdAutoload = argc == 3 && std::wcscmp(argv[2], L"--emd-autoload") == 0;
    const bool fileWatch = argc == 4 && std::wcscmp(argv[2], L"--file-watch") == 0;
    const bool emmLoad = argc == 4 && std::wcscmp(argv[2], L"--emm-load") == 0;
    const bool aaScene = argc == 4 && std::wcscmp(argv[2], L"--antialias-scene") == 0;
    const bool mmeSceneCapture = argc == 4 && std::wcscmp(argv[2], L"--mme-scene-capture") == 0;
    const bool imageVisibility = argc == 4 && std::wcscmp(argv[2], L"--image-visibility") == 0;
    const bool groundShadow = argc == 4 && std::wcscmp(argv[2], L"--ground-shadow") == 0;
    const bool sequenceClock = argc == 4 && std::wcscmp(argv[2], L"--sequence-clock") == 0;
    if (sequenceClock || emdAutoload || fileWatch || emmLoad || aaScene || mmeSceneCapture || groundShadow || (argc == 4 && std::wcscmp(argv[2], L"--surface-grid") == 0)) {
        auto* compile = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
        Check(compile && MH_CreateHook(compile, &ObserveCompile, reinterpret_cast<void**>(&originalCompile)) == MH_OK &&
              MH_EnableHook(compile) == MH_OK, "observe actual MME effect compilation");
    }
    wchar_t native[4]{};
    const bool nativeOrder = GetEnvironmentVariableW(L"MMS_MME_ORDER_NATIVE", native, 4) == 1 && native[0] == L'1';
    Check(nativeOrder ? !GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll") :
        GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        nativeOrder ? "native baseline excludes MME DLLs" : "real MME DLLs loaded");
    if(std::getenv("MMS_SURFACE_FORCE_GPU")){
        const auto& adapters=skinning::AvailableAdapters();Check(!adapters.empty(),"GPU skinning adapter available");
        skinning::SetPreference({skinning::Mode::Adapter,adapters.front().id,adapters.front().name});
    }
    if ((aaScene || mmeSceneCapture || (argc == 4 && std::wcscmp(argv[2], L"--viewport-scale") == 0)) &&
        std::filesystem::path(argv[1]).extension() == L".pmm") {
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, argv[1]);
        LoadSceneFile();
        app->SetSelectedModelSlot(0);
        std::set<const void*> loadedModels;
        for (int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot))
            Check(loadedModels.insert(model).second,"loaded project model slots retain distinct ownership");
        const auto* effects = mmeSceneCapture ? _wgetenv(L"MMS_MME_CAPTURE_EMM") : nullptr;
        if (!effects) effects = _wgetenv(L"MMS_AA_EMM");
        if (effects) {
            if (mmeSceneCapture) std::printf("MME_CAPTURE explicit effects=%ls\n", effects);
            NotifyEffectFileOpen(effects);
        }
    } else LoadModelFile(app, argv[1], false);
    if (argc == 4 && std::wcscmp(argv[2], L"--model-edit-controls") == 0) {
        CheckModelEditControls(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard);
        return 0;
    }
    // Test-only second model and accessory slots: insert them into the scene
    // instead of replacing the base model.
    if (imageVisibility) PrepareImageVisibilityScene(app);
    if (const wchar_t* extra = _wgetenv(L"MMS_EXTRA_MODEL")) {
        LoadModelFile(app, extra, false);
        for (int i = 0; i < 3; ++i) {
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
    }
    if (const wchar_t* accessory = _wgetenv(L"MMS_EXTRA_ACCESSORY")) {
        LoadAccessoryFile(accessory);
        for (int i = 0; i < 3; ++i) {
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
    }
    Check(app->SelectedModel() != nullptr, "load textured PMX");
    if (imageVisibility) {
        PrepareImageVisibilityScene(app);
        if (const auto* project = _wgetenv(L"MMS_IMAGE_EFFECT_PROJECT")) {
            wcscpy_s(app->EnvFileName(), kPathFieldCapacity, project);
            SaveSceneFile(app);
            for (int i = 0; i < 3; ++i) { app->state.messageSeen = 1; FrameDriver(app); }
            auto fixture = std::filesystem::path(project); fixture.replace_extension(L".fixture.emm");
            auto emm = std::filesystem::path(project); emm.replace_extension(L".emm");
            std::filesystem::copy_file(fixture, emm, std::filesystem::copy_options::overwrite_existing);
            ApplyImageVisibilitySelfShadow(app);
            NotifyEffectFileOpen(project);
            for (int i = 0; i < 3; ++i) { app->state.messageSeen = 1; FrameDriver(app); }
        }
        CheckImageVisibility(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--camera-motion") == 0) {
        CheckCameraMotionScene(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    for (int i = 0; i < 3; ++i) { app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app); }
    if (mmeSceneCapture) {
        int result = 0;
        try { mme_scene_capture::Run(app, argv[1], argv[3]); }
        catch (const std::exception& error) { std::fprintf(stderr, "FAIL MME_CAPTURE %s\n", error.what()); result = 2; }
        mme_scene_capture::Release(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard);
        std::printf("MME_CAPTURE cleanup complete result=%d\n", result);
        return result;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--model-adapt") == 0) {
        CheckMmeModelAdapt(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (sequenceClock) {
        CheckMmeSequenceClock(app, argv[3]);
        for (auto* effect : sequenceClockEffects) effect->Release();
        sequenceClockEffects.clear();
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (fileWatch) {
        CheckEffectFileWatch(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (aaScene) {
        CheckAntialiasScene(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--viewport-scale") == 0) {
        if(std::getenv("MMS_VIEWPORT_PANELS")) CheckViewportPanels(app,argv[3]);
        else if(std::getenv("MMS_VIEWPORT_PERF")) CheckViewportScalePerformance(app,argv[3]);
        else if(std::getenv("MMS_VIEWPORT_DIALOG")) CheckViewportScaleDialog(app,argv[3]);
        else CheckViewportRenderScale(app, argv[3]);
        ResetAppState(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (emmLoad) {
        Check(std::filesystem::is_regular_file(argv[3]), "EMM project notification target exists");
        NotifyEffectFileOpen(argv[3]);
        if (const auto* working = _wgetenv(L"MMS_EMM_WORKING_DIRECTORY"))
            Check(SetCurrentDirectoryW(working) != FALSE, "simulate a file picker changing the working directory");
        for (int i = 0; i < 4; ++i) {
            app->DeltaTime() = 1.f / 30;
            app->state.messageSeen = 10;
            FrameDriver(app);
        }
        Check(emdEffectCompiles > 0, "EMM loads and compiles its assigned effect");
        void** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
        Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
              MH_EnableHook(vtable[17]) == MH_OK, "observe actual EMM presentation");
        captureRequested = true;
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        Check(!captureRequested && std::count(capturedPixels.begin(), capturedPixels.end(), 0xFF00FFu) > 50,
              "assigned EMM effect renders magenta pixels on the real model");
        Check(effectPathDiagnostics > 0, "effect file is recorded before entering native D3DX compilation");
        MH_DisableHook(vtable[17]);
          if (std::getenv("MMS_PMM_VERSION_TEST")) CheckVersionEffectSave(app, argv[3]);
        Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (emdAutoload) {
        const auto* filename = reinterpret_cast<const unsigned char*>(ExpGetPmdFilename(0));
        std::printf("EXPORTED_PATH acp=%u bytes=", GetACP());
        for (; filename && *filename; ++filename) std::printf("%02x", *filename);
        std::puts("");
        Check(emdEffectCompiles > 0, "same-name EMD automatically loads and compiles its assigned effect");
        Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (groundShadow) {
        if (!nativeOrder) Check(fileEffectCompiles > 0, "ground-shadow MME effects compiled");
        captureDirectory = argv[3];
        CheckGroundShadow(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--surface-grid") == 0) {
        captureDirectory = argv[3];
        CheckSurfaceGrid(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--outline-coverage") == 0) {
        CheckOutlineCoverage(app,argv[3]);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--outline-stability") == 0) {
        CheckOutlineStability(app,argv[3]);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (argc >= 3 && std::wcscmp(argv[2], L"--outline-antialias") == 0) {
        if (argc == 4) captureDirectory = argv[3];
        CheckOutlineAntialias(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc >= 3 && std::wcscmp(argv[2], L"--alpha-order") == 0) {
        if (argc == 4) captureDirectory = argv[3];
        CheckAlphaOrder(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    Check(SetTimer(nullptr, 0, 100, Probe) != 0, "inspect dialog asynchronously");
    SendMessageW(app->MainWindow(), WM_COMMAND, 40005, 0);
    while (!done) { MSG message{}; if (GetMessageW(&message, nullptr, 0, 0) <= 0) break; TranslateMessage(&message); DispatchMessageW(&message); }
    Check(done && rowsSeen > 1, "inspect actual expanded material rows");
    Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    UnhookWindowsHookEx(guard);
    return 0;
}
