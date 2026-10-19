// MAINTENANCE: regression-keep — native bone scale behavior.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <cmath>
using namespace mikumikustudio;
namespace mikumikustudio { void DrawFrameOverlayBatches(MMDApp*); }
// MMS: the top status bar's XYZ bone-scale readout (src/window/ui_palette.cpp).
namespace mikumikustudio {
bool SelectedBoneScaleReadout(MMDApp*, float[3]);
bool SetSelectedBoneScaleAxis(MMDApp*, int, float);
bool BoneScaleReadoutText(MMDApp*, char*, std::size_t);
void BoneScaleReadoutTick(MMDApp*);
void HandlePaletteChanged2(HDC);
}

// ---- Refresh-transport spy -------------------------------------------------
// A hidden window keeps no update region at all (measured: even an
// InvalidateRect of the whole window leaves GetUpdateRect, GetUpdateRgn and
// BeginPaint all empty on a never-shown window), so the refresh cannot be
// observed through the update region.  The spy patches this EXE's import of
// user32!InvalidateRect instead, recording the exact call the production tick
// makes - window, rectangle and count - which verifies the transport without
// showing any window.
namespace {
int (WINAPI* RealInvalidateRect)(HWND, const RECT*, BOOL) = nullptr;
struct InvalidateCall {
    HWND hwnd;
    RECT rect;
    BOOL hasRect;
};
constexpr int kMaxInvalidateCalls = 64;
InvalidateCall g_invalidateCalls[kMaxInvalidateCalls];
int g_invalidateCallCount = 0;

int WINAPI SpyInvalidateRect(HWND window, const RECT* rect, BOOL erase) {
    if (g_invalidateCallCount < kMaxInvalidateCalls) {
        InvalidateCall& call = g_invalidateCalls[g_invalidateCallCount++];
        call.hwnd = window;
        call.hasRect = rect != nullptr;
        call.rect = rect != nullptr ? *rect : RECT{0, 0, 0, 0};
    }
    return RealInvalidateRect(window, rect, erase);
}

void InstallInvalidateRectSpy() {
    // InvalidateRect is a neutral Win32 API (WinUser.h: WINUSERAPI BOOL WINAPI
    // InvalidateRect(HWND, CONST RECT*, BOOL) - no A/W variants and no macro),
    // so the export name is exactly "InvalidateRect" and the EXE imports it
    // under that name.  Match the IAT slot by resolved address first (covers
    // any binding style) and fall back to the import name; the validated
    // diag_invalidate_spy reproduces this installer against a cross-TU
    // dllimport call.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
        user32 = LoadLibraryW(L"user32.dll");
    FARPROC target = GetProcAddress(user32, "InvalidateRect");
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(exe) + directory.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(exe) + imports->FirstThunk);
        auto* names = imports->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                  reinterpret_cast<BYTE*>(exe) + imports->OriginalFirstThunk)
            : nullptr;
        for (; thunk->u1.Function != 0; ++thunk) {
            // Two ways to recognise the slot: the resolved address (covers
            // every binding style) or the import name.  Only the neutral name
            // exists for this API - there is no A/W variant to match.
            bool match = target != nullptr &&
                thunk->u1.Function == reinterpret_cast<ULONG_PTR>(target);
            if (!match && names != nullptr &&
                !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    reinterpret_cast<BYTE*>(exe) + names->u1.AddressOfData);
                const char* entry = reinterpret_cast<const char*>(import->Name);
                match = std::strcmp(entry, "InvalidateRect") == 0;
            }
            if (names != nullptr)
                ++names;
            if (!match)
                continue;
            DWORD previous = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(void*),
                           PAGE_READWRITE, &previous);
            RealInvalidateRect = reinterpret_cast<decltype(RealInvalidateRect)>(
                static_cast<ULONG_PTR>(thunk->u1.Function));
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(
                reinterpret_cast<void*>(&SpyInvalidateRect));
            VirtualProtect(&thunk->u1.Function, sizeof(void*), previous,
                           &previous);
            return;
        }
    }
}
}  // namespace
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
int wmain(int argc, wchar_t** argv) {
    Check(argc == 2, "output directory argument");
    std::filesystem::create_directories(argv[1]);
    HWND window = CreateWindowW(L"STATIC", L"Hidden HUD readback", WS_POPUP,
        0, 0, 512, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr && !IsWindowVisible(window), "hidden window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "Direct3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 512; pp.BackBufferHeight = 320;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "GPU device");
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    app->Renderer() = renderer.get(); renderer->device = device;
    app->ViewportRect() = {0, 0, 512, 320};
    app->state.optflag[0] = 1; app->CameraParentModel() = 0;
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{}; unsigned char selected=1, dirty=0;
    bone.parent=-1; bone.matInit[0]=bone.matInit[5]=bone.matInit[10]=bone.matInit[15]=1;
    model->boneCount=1; model->boneTable=&bone; model->selectedBone=0;
    model->boneSelection=&selected; model->bonePhysicsState=&dirty;
    app->state.modelSlots[0]=reinterpret_cast<unsigned char*>(model.get());
    app->state.slotIdx=0;
    mdl::AccessoryRecord accessory{};
    app->AccessorySlot(0) = &accessory;
    app->SelectedAccessorySlot() = 0;
    Check(SUCCEEDED(device->CreateVertexBuffer(0x445C0, D3DUSAGE_WRITEONLY,
        0x144, D3DPOOL_MANAGED, &app->SpriteOverlayVertices(), nullptr)), "sprite buffer");
    auto& api = d3dx::Get(); Check(api.Load(), "D3DX");
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC info = FindResourceA(module, MAKEINTRESOURCEA(102), "PNG");
    Check(info != nullptr, "embedded HUD resource");
    const void* bytes = LockResource(LoadResource(module, info));
    Check(SUCCEEDED(api.fromMemEx(device, bytes, SizeofResource(module, info),
        UINT(-1), UINT(-1), 1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, DWORD(-1),
        DWORD(-1), 0, nullptr, nullptr, &app->OverlayTexture())), "production PNG load");
    D3DSURFACE_DESC desc{}; app->OverlayTexture()->GetLevelDesc(0, &desc);
    Check(desc.Width == 512 && desc.Height == 512, "texture dimensions");
    IDirect3DSurface9* target = nullptr;
    Check(SUCCEEDED(device->GetRenderTarget(0, &target)), "render target");
    IDirect3DSurface9* read = nullptr;
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(512, 320,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &read, nullptr)), "readback surface");
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    const float scales[] = {1.f, 1.5f, 2.f};
    const DWORD backgrounds[] = {0xFF303030, 0xFFE8E8E8};
    const auto capture = [&](int state, float scale, DWORD background) {
        app->state.optflag[0]=state == 0 ? 0 : 1;
        app->SelectedAccessorySlot()=state == 2 ? 0 : -1;
        renderer->viewScale=scale;
        Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,background,1,0)),"clear");
        Check(SUCCEEDED(device->BeginScene()),"begin");
        PrepareFrameSpriteOverlay(app.get()); DrawFrameOverlayBatches(app.get());
        Check(SUCCEEDED(device->EndScene()),"end");
        DWORD alphaOp=0; device->GetTextureStageState(0,D3DTSS_ALPHAOP,&alphaOp);
        Check(alphaOp==D3DTOP_SELECTARG1,"sprite alpha state restored");
        Check(SUCCEEDED(device->GetRenderTargetData(target,read)),"GPU readback");
        std::vector<DWORD> pixels(512*320);
        D3DLOCKED_RECT locked{}; Check(SUCCEEDED(read->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"lock readback");
        for(int y=0;y<320;++y) std::memcpy(pixels.data()+y*512,static_cast<unsigned char*>(locked.pBits)+y*locked.Pitch,512*4);
        read->UnlockRect();
        return pixels;
    };
    int captures=0;
    for (int s=0;s<3;++s) for(int b=0;b<2;++b) {
        const float scale=scales[s]; const DWORD background=backgrounds[b]&0xFFFFFF;
        const auto alternate=capture(0,scale,backgrounds[1-b]);
        const auto enabled=capture(0,scale,backgrounds[b]);
        const auto save=[&](int state) {
            const auto path=std::wstring(argv[1])+L"/scale-"+std::to_wstring(state)+L"-"+std::to_wstring(s)+L"-"+std::to_wstring(b)+L".png";
            Check(SUCCEEDED(api.saveSurfaceToFileW(path.c_str(),3,read,nullptr,nullptr)),"PNG capture"); ++captures;
        };
        save(0);
        for(int state:{1,2}) {
            const auto disabled=capture(state,scale,backgrounds[b]); save(state);
            for(int button=0;button<4;++button) {
                int transparent=0,dimmed=0,partial=0;
                const int x0=int(512-(170-button*40)*scale),x1=int(512-(130-button*40)*scale);
                const int y0=int(320-40*scale),y1=int(320-10*scale);
                for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
                    const DWORD a=enabled[y*512+x]&0xFFFFFF,d=disabled[y*512+x]&0xFFFFFF;
                    const int ar=(a>>16)&255,ag=(a>>8)&255,ab=a&255;
                    const int dr=(d>>16)&255,dg=(d>>8)&255,db=d&255;
                    // An opaque gray texel may equal one background by
                    // coincidence. Remaining unchanged on both contrasting
                    // backgrounds identifies actual transparent texels.
                    if(a==background&&(alternate[y*512+x]&0xFFFFFF)==(backgrounds[1-b]&0xFFFFFF)) {
                        Check(d==background,"transparent area keeps background without rectangular wash"); ++transparent;
                    }
                    if(x<x0+2*scale||x>=x1-2*scale)
                        Check(a==background&&d==background,"clear icon margins remain exactly transparent");
                    if(a!=background&&dr<=ar&&dg<=ag&&db<=ab&&(ar+ag+ab)-(dr+dg+db)>20) {
                        ++dimmed; if(dr+dg+db>30) ++partial;
                    }
                }
                Check(transparent>10,"each icon includes transparent pixels");
                Check(dimmed>20,"each disabled icon is visibly dimmed");
                Check(partial>10,"disabled wash is semi-transparent, not opaque black");
            }
        }
    }
    // ---- MMS: the top status bar's XYZ bone-scale readout ------------------
    // src/window/ui_palette.cpp.  The three factors must follow the axis rule
    // of the HUD rotate and scale rows (bone_scale.hpp Drag: X/Y/Z in the
    // existing coordinate selector) - local / global / view, and in local mode
    // the bone's own fixed-axis / local-axis definition - and must never be
    // the raw diagonal of the stored matrix, whose axes are the bone's unposed
    // model-space basis.
    {
        const auto approx = [](float a, double b) {
            return std::fabs(static_cast<double>(a) - b) < 1e-3;
        };
        const auto diagonal = [](float x, float y, float z) {
            bone_scale::Matrix m;
            m.v = {x, 0, 0, 0, y, 0, 0, 0, z};
            return m;
        };
        // The readout shares the status band with the window-top button row and
        // hides when the free band cannot hold it, so give the hidden window
        // the width of a normal main window.
        SetWindowPos(window, nullptr, 0, 0, 1024, 320, SWP_NOMOVE | SWP_NOZORDER);
        app->ViewportRect() = {0, 60, 1024, 300};
        app->state.hwnd = window;
        g_Block = app.get();
        app->state.englishUI = 1;
        app->state.optflag[0] = 0;          // bone panel
        app->state.coordinateSystem = 1;    // global
        app->PlaybackActive() = 0;
        app->FrameStepPlayback() = 0;
        unsigned char* raw = reinterpret_cast<unsigned char*>(model.get());
        model->physicsMode = 0;
        for (float& v : bone.localAxes) v = 0.0f;
        model->selectedBone = 0;
        for (int i = 0; i < 16; ++i) bone.matInit[i] = 0.0f;
        bone.matInit[0] = bone.matInit[5] = bone.matInit[10] = bone.matInit[15] = 1.0f;
        bone_scale::Forget(raw);

        float values[3] = {};
        char text[64] = {};
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 1) && approx(values[1], 1) && approx(values[2], 1),
              "an unscaled bone reads 1,1,1");
        Check(BoneScaleReadoutText(app.get(), text, sizeof(text)) &&
                  std::strcmp(text, "X 1.00 Y 1.00 Z 1.00") == 0,
              "the default readout line is X 1.00 Y 1.00 Z 1.00");

        Check(bone_scale::SetPose(raw, 0, diagonal(2, 3, 4)),
              "install a 2,3,4 scale pose");
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 2) && approx(values[1], 3) && approx(values[2], 4),
              "global mode reports the world-axis factors");
        app->state.coordinateSystem = 0;    // local
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 2) && approx(values[1], 3) && approx(values[2], 4),
              "local mode matches for identity bone axes");

        // Local-axis bone (PMX local axes, physicsMode gate as in
        // BoneLocalAxes): axis 0 becomes world Y, so the same stored scale must
        // read permuted - the axis definition, not the matrix diagonal.
        model->physicsMode = 2;
        bone.localAxes[0] = 0.0f; bone.localAxes[1] = 1.0f; bone.localAxes[2] = 0.0f;
        bone.localAxes[3] = 0.0f; bone.localAxes[4] = 0.0f; bone.localAxes[5] = 1.0f;
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 3) && approx(values[1], 2) && approx(values[2], 4),
              "local mode follows the bone's own axis definition");
        app->state.coordinateSystem = 1;
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 2) && approx(values[1], 3) && approx(values[2], 4),
              "global mode ignores the bone's local axes");
        model->physicsMode = 0;
        for (float& v : bone.localAxes) v = 0.0f;

        // Edit consistency: a scale edit along the global X axis must read back
        // on that axis even when the bone's rigid basis rotates the stored
        // axes, and the raw matrix diagonal then names a different axis.
        Check(bone_scale::SetPose(raw, 0, bone_scale::Matrix{}),
              "reset the pose to identity");
        for (int i = 0; i < 16; ++i) bone.matInit[i] = 0.0f;
        bone.matInit[1] = 1.0f;    // rigid basis: 90 degrees about Z
        bone.matInit[4] = -1.0f;
        bone.matInit[10] = 1.0f;
        bone.matInit[15] = 1.0f;
        bone_scale::Matrix rigid{};
        rigid.v = {0, 1, 0, -1, 0, 0, 0, 0, 1};
        const float worldX[3] = {1.0f, 0.0f, 0.0f};
        Check(bone_scale::Scale(raw, 0, rigid, worldX, 2.0f, false),
              "apply one global-X factor-2 increment through the edit path");
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 2) && approx(values[1], 1) && approx(values[2], 1),
              "the readout reports the edited factor on the edited axis");
        const bone_scale::Matrix edited = bone_scale::Pose(raw, 0);
        Check(std::fabs(static_cast<double>(edited.v[0]) - 1.0) < 1e-3 &&
                  std::fabs(static_cast<double>(edited.v[4]) - 2.0) < 1e-3,
              "the raw matrix diagonal names a different axis and is never shown as XYZ");
        Check(SetSelectedBoneScaleAxis(app.get(), 0, 3.0f),
              "the bottom scale field accepts an absolute global-X factor");
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 3) && approx(values[1], 1) && approx(values[2], 1),
              "numeric scaling edits the selected coordinate axis in place");

        // Empty and hidden states.
        model->selectedBone = -1;
        Check(!SelectedBoneScaleReadout(app.get(), values),
              "no selected bone has no value");
        Check(BoneScaleReadoutText(app.get(), text, sizeof(text)) &&
                  std::strcmp(text, "-----") == 0,
              "no selected bone shows the clear empty marker");
        model->selectedBone = 0;
        app->state.optflag[0] = 1;
        Check(!BoneScaleReadoutText(app.get(), text, sizeof(text)) &&
                  text[0] == '\0',
              "the camera panel hides the bone scale readout");
        app->state.optflag[0] = 0;
        app->PlaybackActive() = 1;
        Check(!BoneScaleReadoutText(app.get(), text, sizeof(text)) &&
                  text[0] == '\0',
              "playback hides the readout with the Playing line");
        app->PlaybackActive() = 0;

        // ---- Refresh transport --------------------------------------------
        // The production tick must request one repaint of the status band per
        // value change and none while unchanged.  A hidden window keeps no
        // update region at all (see the spy note at the top), so the requests
        // are recorded at this EXE's user32 import instead of GetUpdateRect.
        InstallInvalidateRectSpy();
        Check(RealInvalidateRect != nullptr, "the InvalidateRect import is hooked");
        BoneScaleReadoutTick(app.get());            // prime the cache
        const int primeCalls = g_invalidateCallCount;
        BoneScaleReadoutTick(app.get());
        Check(g_invalidateCallCount == primeCalls,
              "an unchanged readout requests no repaint");
        Check(bone_scale::SetPose(raw, 0, diagonal(2, 3, 5)),
              "change the scale pose");
        BoneScaleReadoutTick(app.get());
        Check(g_invalidateCallCount == primeCalls + 1,
              "a changed readout requests exactly one repaint");
        {
            const InvalidateCall& call = g_invalidateCalls[primeCalls];
            Check(call.hwnd == window && call.hasRect &&
                      call.rect.left == 19 && call.rect.top == 35 &&
                      call.rect.right == 1024 && call.rect.bottom == 59,
                  "the repaint request covers the top status band of this window");
        }
        BoneScaleReadoutTick(app.get());
        Check(g_invalidateCallCount == primeCalls + 1,
              "the tick is idempotent once the values settled");

        // ---- Painted result -----------------------------------------------
        // The band must repaint with new numbers on a value change and on a
        // coordinate-mode change (memory-DC readback of the same painter
        // WM_PAINT runs; no window is shown and no desktop capture is used).
        const auto captureBand = [&](std::vector<DWORD>& pixels, int& ink) {
            HDC windowDc = GetDC(window);
            HDC memory = CreateCompatibleDC(windowDc);
            Check(memory != nullptr, "memory DC");
            HBITMAP bitmap = CreateCompatibleBitmap(windowDc, 1024, 320);
            Check(bitmap != nullptr, "memory bitmap");
            HGDIOBJ selected = SelectObject(memory, bitmap);
            HandlePaletteChanged2(memory);
            pixels.assign(1024 * 25, 0);
            ink = 0;
            for (int y = 35; y < 60; ++y)
                for (int x = 0; x < 1024; ++x) {
                    const COLORREF pixel = GetPixel(memory, x, y);
                    pixels[(y - 35) * 1024 + x] = static_cast<DWORD>(pixel);
                    if (x >= 19 && x < 560 && pixel == RGB(0, 0, 0))
                        ++ink;
                }
            SelectObject(memory, selected);
            DeleteObject(bitmap);
            DeleteDC(memory);
            ReleaseDC(window, windowDc);
        };
        bone_scale::Forget(raw);                    // identity rigid basis
        for (int i = 0; i < 16; ++i) bone.matInit[i] = 0.0f;
        bone.matInit[0] = bone.matInit[5] = bone.matInit[10] = bone.matInit[15] = 1.0f;
        model->physicsMode = 0;
        for (float& v : bone.localAxes) v = 0.0f;
        app->state.coordinateSystem = 1;
        Check(bone_scale::SetPose(raw, 0, diagonal(2, 3, 4)), "pose 2,3,4");
        int ink = 0;
        std::vector<DWORD> base, changed, local;
        BoneScaleReadoutTick(app.get());
        captureBand(base, ink);
        Check(ink > 20, "the scale readout paints on the top status band");

        const int beforeValueChange = g_invalidateCallCount;
        Check(bone_scale::SetPose(raw, 0, diagonal(2, 3, 5)), "pose 2,3,5");
        BoneScaleReadoutTick(app.get());
        Check(g_invalidateCallCount == beforeValueChange + 1,
              "a value change requests a repaint");
        captureBand(changed, ink);
        Check(changed == base && ink > 20,
              "bone scale values are no longer painted in the top status band");

        // Coordinate-mode change: the local-axis bone reads the same stored
        // scale along its own axes (3,2,5), not the global numbers (2,3,5).
        model->physicsMode = 2;
        bone.localAxes[0] = 0.0f; bone.localAxes[1] = 1.0f; bone.localAxes[2] = 0.0f;
        bone.localAxes[3] = 0.0f; bone.localAxes[4] = 0.0f; bone.localAxes[5] = 1.0f;
        const int beforeModeChange = g_invalidateCallCount;
        app->state.coordinateSystem = 0;
        Check(SelectedBoneScaleReadout(app.get(), values) &&
                  approx(values[0], 3) && approx(values[1], 2) && approx(values[2], 5),
              "the local axis definition drives the painted numbers");
        BoneScaleReadoutTick(app.get());
        Check(g_invalidateCallCount == beforeModeChange + 1,
              "a coordinate-mode change requests a repaint");
        captureBand(local, ink);
        Check(local == changed && ink > 20,
              "coordinate changes do not paint scale values in the top status band");
        g_Block = nullptr;
    }
    std::printf("PASS: %d GPU captures; all four icons, camera/accessory masks, transparent margins, 1/1.5/2 DPI, the XYZ scale readout and its repaint transport\n",captures);
    read->Release(); target->Release();
    app->state.modelSlots[0]=nullptr;
    app->OverlayTexture()->Release(); app->SpriteOverlayVertices()->Release();
    device->Release(); d3d->Release(); DestroyWindow(window);
}
