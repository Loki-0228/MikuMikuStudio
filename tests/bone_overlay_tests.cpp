// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Vertex { float x, y, z, w; DWORD color; float u, v; };
void Quad(IDirect3DVertexBuffer9* vb, float x, float y) {
    const Vertex vertices[] = {
        {x,y,0,1,0xFFFFFFFF,0,0}, {x+8,y,0,1,0xFFFFFFFF,1,0}, {x,y+8,0,1,0xFFFFFFFF,0,1},
        {x,y+8,0,1,0xFFFFFFFF,0,1}, {x+8,y,0,1,0xFFFFFFFF,1,0}, {x+8,y+8,0,1,0xFFFFFFFF,1,1}};
    void* data = nullptr;
    Check(SUCCEEDED(vb->Lock(0, sizeof vertices, &data, 0)), "lock quad fixture");
    std::memcpy(data, vertices, sizeof vertices); vb->Unlock();
}
}
int main() {
    HWND window = CreateWindowW(L"STATIC", L"Bone overlay regression", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create test render window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "create real GPU device");
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get(); renderer->device = device; renderer->viewScale = 1;
    app->ViewportRect() = {0, 0, 64, 64}; app->state.optflag[0] = 0;
    auto model = std::make_unique<mdl::ModelRecord>();
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); app->SetSelectedModelSlot(0);
    mdl::BoneRecord bones[2]{};
    model->boneTable = bones; model->boneCount = 2; model->selectedBone = 0; model->physicsMode = 2;
    unsigned char selection[2] = {1,0}; model->boneSelection = selection;
    D3DMATRIX identity{}; identity._11 = identity._22 = identity._33 = identity._44 = 1;
    for (auto& bone : bones) {
        std::memcpy(bone.matInit, &identity, sizeof identity);
        bone.flags = mdl::kBoneFlagVisible | mdl::kBoneFlagTailIsBone;
        bone.tailBone = -1;
    }
    bones[0].position[0] = -.5f; bones[0].position[1] = -.5f;
    bones[1].position[0] = .5f; bones[1].position[1] = .5f;
    bones[0].tailBone = 1;
    device->SetTransform(D3DTS_WORLD, &identity); device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);
    Check(SUCCEEDED(device->CreateVertexBuffer(200000, 0, 0, D3DPOOL_MANAGED,
        &renderer->lineVertexBuffer, nullptr)), "allocate line batch");
    Check(SUCCEEDED(device->CreateVertexBuffer(0x445C0, 0, 0, D3DPOOL_MANAGED,
        &app->SpriteOverlayVertices(), nullptr)), "allocate sprite batch");
    Check(SUCCEEDED(device->CreateVertexBuffer(6 * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED,
        &app->OverlayVertices(), nullptr)), "allocate text batch");
    auto texture = [&](DWORD color) {
        IDirect3DTexture9* result = nullptr;
        Check(SUCCEEDED(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&result,nullptr)), "create atlas fixture");
        D3DLOCKED_RECT locked{}; result->LockRect(0,&locked,nullptr,0);
        *static_cast<DWORD*>(locked.pBits) = color; result->UnlockRect(0);
        return result;
    };
    app->TextOverlayTexture() = texture(0x00000000); // transparent atlas pixel, as between text glyphs
    app->OverlayTexture() = texture(0xFF00FF00);
    device->SetRenderState(D3DRS_LIGHTING, FALSE); device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    IDirect3DSurface9 *back = nullptr, *read = nullptr;
    device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)), "allocate readback");
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 4, "linked bone emits normal and selected direction Vs");
    Quad(app->OverlayVertices(), 2, 2); app->TextOverlayPrimitiveCount() = 2;
    Quad(app->SpriteOverlayVertices(), 2, 2); app->SpriteOverlayPrimitiveCount() = 2;
    device->Clear(0,nullptr,D3DCLEAR_TARGET,0xFFFFFFFF,1,0);
    device->BeginScene(); DrawFrameOverlayBatches(app.get()); device->EndScene();
    Check(SUCCEEDED(device->GetRenderTargetData(back, read)), "read final rendered overlays");
    D3DLOCKED_RECT locked{}; read->LockRect(&locked,nullptr,D3DLOCK_READONLY);
    int coloredLinePixels = 0;
    for (int y = 12; y < 56; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(locked.pBits) + y * locked.Pitch);
        for (int x = 10; x < 56; ++x)
            if ((row[x] & 0xFFFFFF) != 0xFFFFFF) ++coloredLinePixels;
    }
    const auto* spriteRow = reinterpret_cast<const DWORD*>(static_cast<const char*>(locked.pBits) + 5 * locked.Pitch);
    Check((spriteRow[5] & 0xFFFFFF) == 0x00FF00, "textured markers still draw after bone lines");
    read->UnlockRect();
    std::printf("bone direction pixels=%d\n", coloredLinePixels);
    Check(coloredLinePixels >= 40, "bone direction lines remain visible after a transparent text atlas");
    // Cursor mode 3 draws the selected bone's direction rather than all bones.
    app->EditMode() = ViewportEditMode::Camera; app->LineOverlayPrimitiveCount() = 0;
    bones[0].flags = mdl::kBoneFlagVisible;
    bones[0].tailBone = -1;
    std::memcpy(bones[0].tailOffset, bones[1].position, sizeof bones[0].tailOffset);
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 2, "selected PMX offset tail draws its direction in cursor mode 3");
    app->LineOverlayPrimitiveCount() = 0; model->selectedBone = 1;
    bones[1].tailBone = 0;
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 2, "selected PMX bone can point to index zero");
    // Selection-box lines precede bone lines and must only be counted once.
    app->LineOverlayPrimitiveCount() = 4;
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 6, "selection box and bone line counts do not double");
    app->LineOverlayPrimitiveCount() = 4998;
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 5000, "last complete bone V fits in the line buffer");
    app->LineOverlayPrimitiveCount() = 4999;
    PrepareFrameSpriteOverlay(app.get());
    Check(app->LineOverlayPrimitiveCount() == 4999, "partial V never writes past the line buffer");
    // Dense skeletons must leave space for the scale row without overrunning
    // the fixed six-vertices-per-quad sprite buffer.
    std::vector<mdl::BoneRecord> crowded(4000, bones[0]);
    std::vector<unsigned char> crowdSelection(crowded.size(), 0);
    for (auto& bone : crowded) {
        bone.type=mdl::BoneType::RotateMove; bone.tailBone=-1;
        bone.flags=mdl::kBoneFlagVisible;
    }
    model->boneTable=crowded.data(); model->boneCount=static_cast<unsigned>(crowded.size());
    model->boneSelection=crowdSelection.data(); model->selectedBone=0;
    app->EditMode()=static_cast<ViewportEditMode>(0); app->LineOverlayPrimitiveCount()=0;
    PrepareFrameSpriteOverlay(app.get());
    Check(app->SpriteOverlayPrimitiveCount()*3u*28u<=0x445C0u,"dense bone markers never overrun sprite buffer");
    Check(app->SpriteOverlayPrimitiveCount()>3200u,"dense fixture fills marker budget and still emits HUD");
    model->boneTable=bones; model->boneCount=2; model->boneSelection=selection;
    read->Release(); back->Release();
    app->TextOverlayTexture()->Release(); app->OverlayTexture()->Release();
    app->OverlayVertices()->Release(); app->SpriteOverlayVertices()->Release();
    renderer->lineVertexBuffer->Release(); device->Release(); d3d->Release(); DestroyWindow(window);
    std::puts("PASS bone direction GPU pixels, PMX endpoints, selection batch, and textured markers");
}
