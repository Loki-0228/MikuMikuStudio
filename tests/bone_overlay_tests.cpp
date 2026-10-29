// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_projection.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "bone_multiselect_hud_checks.hpp"

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
DWORD ReadPixel(IDirect3DDevice9* device, IDirect3DSurface9* read, int x,
                int y) {
    IDirect3DSurface9* back = nullptr;
    Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                          &back)),
          "back buffer for the pixel read-back");
    Check(SUCCEEDED(device->GetRenderTargetData(back, read)),
          "copy the rendered frame");
    back->Release();
    D3DLOCKED_RECT locked{};
    Check(SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY)),
          "lock the pixel read-back");
    const auto* row = reinterpret_cast<const DWORD*>(
        static_cast<const char*>(locked.pBits) + y * locked.Pitch);
    const DWORD pixel = row[x];
    read->UnlockRect();
    return pixel;
}

// Issue #37: the bottom-right axis preview must originate at the same pixel
// as the selected bone, including after its pose or the camera has moved.
// Read the final yellow origin square, not just a separately computed matrix.
void CheckHudAxisOrigin() {
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    HWND window = CreateWindowW(L"STATIC", L"HUD axis origin regression", WS_POPUP,
        0, 0, kWidth, kHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden gizmo regression window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create gizmo regression D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)),
        "create actual gizmo regression GPU device");
    IDirect3DSurface9* read = nullptr;
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(kWidth, kHeight,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &read, nullptr)),
        "allocate gizmo regression readback");
    Check(d3dx::Get().Load(), "load D3DX for gizmo transforms");
    auto fixture = std::make_unique<MMDApp>();
    // MMDApp's user-defined constructor does not initialize state. Keep this
    // large value-initialized object off the stack shared with the caller.
    const auto initialState = std::make_unique<MMDAppState>();
    fixture->state = *initialState;
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    fixture->Renderer() = renderer.get();
    renderer->device = device;
    renderer->viewScale = 1.0f;
    fixture->ViewportRect() = {40, 25, kWidth - 40, kHeight - 25};
    fixture->CameraFov() = 30;
    fixture->CameraDistance() = -6;
    fixture->CameraRoll() = -0.3f;
    auto model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bone{};
    unsigned char selected = 1;
    model->boneTable = &bone;
    model->boneCount = 1;
    model->selectedBone = 0;
    model->boneSelection = &selected;
    fixture->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
    fixture->SetSelectedModelSlot(0);
    bone.type = mdl::BoneType::RotateMove;
    bone.flags = mdl::kBoneFlagVisible;
    bone.position[0] = 0.35f;
    bone.position[1] = -0.2f;
    bone.position[2] = 2.0f;
    // PMX custom local axes deliberately differ from the global axes.
    bone.localAxes[0] = 0.8f;
    bone.localAxes[1] = 0.6f;
    bone.localAxes[5] = 1.0f;
    bone.matWorld[0] = bone.matWorld[5] = bone.matWorld[10] = bone.matWorld[15] = 1.0f;
    D3DMATRIX pose{};
    pose._11 = 1.2f * std::cos(0.4f);
    pose._12 = 1.2f * std::sin(0.4f);
    pose._21 = -0.8f * std::sin(0.4f);
    pose._22 = 0.8f * std::cos(0.4f);
    pose._33 = pose._44 = 1.0f;
    pose._41 = 0.25f;
    pose._42 = 0.35f;
    pose._43 = 0.4f;
    D3DMATRIX frame{};
    frame._11 = frame._22 = std::cos(-0.3f);
    frame._12 = std::sin(-0.3f);
    frame._21 = -std::sin(-0.3f);
    frame._33 = frame._44 = 1.0f;
    frame._41 = -0.1f;
    frame._42 = 0.15f;
    D3DMATRIX view{};
    view._11 = view._22 = view._33 = view._44 = 1.0f;
    view._41 = 0.1f;
    view._42 = -0.2f;
    view._43 = 1.0f;
    const D3DVIEWPORT9 viewport{40, 25, kWidth - 80, kHeight - 50, 0, 1};
    Check(SUCCEEDED(device->SetViewport(&viewport)), "set an inset gizmo viewport");
    device->SetTransform(D3DTS_WORLD, &frame);
    device->SetTransform(D3DTS_VIEW, &view);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    const int hoverStarts[] = {9, 12, 23};
    const int dragStarts[] = {4, 10, 20};
    unsigned cases = 0;
    for (int pmx : {0, 1}) {
        model->physicsMode = pmx ? 2 : 1;
        for (int perspective : {0, 1}) {
            D3DMATRIX projection{};
            projection._11 = 0.75f;
            projection._22 = 1.0f;
            projection._33 = 0.1f;
            if (perspective)
                projection._34 = 1.0f;
            else
                projection._44 = 1.0f;
            device->SetTransform(D3DTS_PROJECTION, &projection);
            for (int coordinate = 0; coordinate < 3; ++coordinate) {
                fixture->state.coordinateSystem = coordinate;
                for (int kind = 0; kind < (build_profile::original ? 2 : 3); ++kind) {
                    for (int axis = 0; axis < 3; ++axis) {
                        for (int dragging : {0, 1}) {
                            // Moving the pose between hover and drag catches a
                            // preview that retains the old bone screen position.
                            pose._41 = dragging ? -0.2f : 0.25f;
                            std::memcpy(bone.matInit, &pose, sizeof(pose));
                            int x = 0, y = 0;
                            float w = 0;
                            Check(ProjectBonePoint(bone, BoneOverlayPoint::Origin,
                                frame, view, projection, fixture->ViewportRect(), &x, &y, &w),
                                "project the transformed bone for the gizmo reference");
                            Check(x > 3 && x < kWidth - 3 && y > 3 && y < kHeight - 3,
                                  "gizmo fixture origin lies inside the readback");
                            fixture->ViewportToolOperation() = dragging ? ViewportToolAction::None
                                : static_cast<ViewportToolAction>(hoverStarts[kind] + axis);
                            fixture->InteractionDragMode() = dragging
                                ? static_cast<ViewportDragMode>(dragStarts[kind] + axis)
                                : ViewportDragMode::None;
                            Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                0xFF406080u, 1, 0)), "clear the axis preview frame");
                            Check(SUCCEEDED(device->BeginScene()), "begin the axis preview frame");
                            DrawBoneOperationAxis(fixture.get(), reinterpret_cast<const float*>(&frame));
                            Check(SUCCEEDED(device->EndScene()), "finish the axis preview frame");
                            const DWORD pixel = ReadPixel(device, read, x, y) & 0xFFFFFFu;
                            if (pixel != 0xFFE000u) {
                                std::fprintf(stderr,
                                    "FAIL: gizmo origin PMX=%d perspective=%d coordinate=%d kind=%d axis=%d drag=%d at (%d,%d) pixel=%06lX\n",
                                    pmx, perspective, coordinate, kind, axis, dragging, x, y, pixel);
                                std::exit(1);
                            }
                            ++cases;
                        }
                    }
                }
            }
        }
    }
    // Do not leave a drag-sector pointer or published geometry referring to
    // this temporary model in the process-wide renderer state.
    fixture->ViewportToolOperation() = ViewportToolAction::None;
    fixture->InteractionDragMode() = ViewportDragMode::None;
    DrawBoneOperationAxis(fixture.get(), reinterpret_cast<const float*>(&frame));
    bone_scale::gizmo::Publish(fixture.get(), {});
    read->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    std::printf("PASS %u transformed HUD axis origins\n", cases);
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
    CheckHudAxisOrigin();
    bone_multiselect_hud_tests::Run();
    std::puts("PASS bone direction GPU pixels, PMX endpoints, selection batch, and textured markers");
}
