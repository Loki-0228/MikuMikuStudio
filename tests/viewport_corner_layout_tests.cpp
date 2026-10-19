// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// ===========================================================================
// 操作柄四角停靠（viewport-control-corner）回归
//
// 目标名 viewport_corner_layout_tests（隐藏目标，主端在 CMakeLists.txt 注册）。
// 沿用 tests/coordinate_axis_tests.cpp 的做法：后台 WS_POPUP 窗口 + 真
// D3D9 设备 + 顶点回读，不截图、不前台、不弹窗。
//
// 覆盖：
//   1) 角落->锚点映射：四角 x scale{1,1.5,2} x 非零原点视窗，块包围盒四边
//      留白 10s、贴合对应角；右下角锚点严格等于移植现状。
//   2) 绘制位置：PrepareFrameSpriteOverlay 顶点回读，四角下旋转 / 位移 /
//      缩放行的四边形坐标与映射值一致（绘制与命中同源）。
//   3) 命中：HandleViewportToolPress 在各角返回对应操作号，缩放行在
//      bone_scale::Enabled 为假时整行不命中。
//   4) 设置往返：临时 ini 四值读写一致、坏值回退 br、默认 br。
//
// 坐标系文字条同时验顶点回读与命中；缩放格额外核对纹理列与操作号。
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "mikumikustudio/frame_presentation.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using mikumikustudio::MMDApp;
using mikumikustudio::MMDAppState;
using mikumikustudio::D3DRenderer;
namespace layout = mikumikustudio::viewport_tool_layout;
namespace settings = mikumikustudio::viewport_corner_settings;
using Corner = layout::Corner;

// MMDAppState is 0xA55E0 bytes on x64 (661 KB).  `app->state = MMDAppState{}`
// materialises that whole blob as a temporary on the test thread's stack, and
// a frame that holds it next to another one overflows the 1 MB default stack
// (tests/channel_bake_tests.cpp documents the same trap).  Zero the blob in
// place instead: it holds only scalars, arrays and pointers, so a zero fill is
// exactly what value-initialisation produces.
void ResetAppState(MMDApp& app) { std::memset(&app.state, 0, sizeof(app.state)); }

// The production paths under test resolve the app through the g_Block global:
// RefreshRequest (src/window/ui_refresh.cpp) dereferences it without a null
// check, and the camera-panel row press reaches it through
// BeginOrEndViewportToolDrag.  The harness must therefore publish the app the
// way every other test driving these paths does (coordinate_axis_tests,
// bone_registration_tests); the global is restored on scope exit.
struct BlockGuard {
    MMDApp* previous;
    explicit BlockGuard(MMDApp* app) : previous(mikumikustudio::g_Block) {
        mikumikustudio::g_Block = app;
    }
    ~BlockGuard() { mikumikustudio::g_Block = previous; }
};

constexpr Corner kCorners[] = {Corner::TopLeft, Corner::TopRight,
                               Corner::BottomLeft, Corner::BottomRight};
constexpr float kScales[] = {1.0f, 1.5f, 2.0f};

// ---- 1) 角落->锚点映射 ---------------------------------------------------
void MappingChecks() {
    struct Case { int left, top, right, bottom; };
    const Case views[] = {{0, 0, 1280, 720}, {100, 50, 1500, 950},
                          {-40, 20, 600, 400}};
    for (const Case& viewCase : views) {
        RECT view{viewCase.left, viewCase.top, viewCase.right, viewCase.bottom};
        for (float scale : kScales) {
            for (Corner corner : kCorners) {
                const layout::Anchor a = layout::Make(view, scale, corner);
                if (corner == Corner::BottomRight) {
                    Check(a.x == static_cast<float>(view.right) &&
                              a.y == static_cast<float>(view.bottom),
                          "bottom-right anchor keeps the original numbers");
                }
                const float margin = 10.0f * scale;
                const bool leftSide = corner == Corner::TopLeft || corner == Corner::BottomLeft;
                const bool topSide = corner == Corner::TopLeft || corner == Corner::TopRight;
                const float left = a.X(leftSide ? 130.0f : 170.0f);
                const float right = a.X(leftSide ? -30.0f : 10.0f);
                const float top = a.Y(topSide ? 120.0f : 167.0f);
                const float bottom = a.Y(topSide ? -37.0f : 10.0f);
                const bool rightSide = corner == Corner::TopRight ||
                                       corner == Corner::BottomRight;
                const bool bottomSide = corner == Corner::BottomLeft ||
                                        corner == Corner::BottomRight;
                Check(rightSide ? right == static_cast<float>(view.right) -
                                          margin
                                : left == static_cast<float>(view.left) +
                                          margin,
                      "block hugs the viewport edge it docks to");
                Check(bottomSide ? bottom == static_cast<float>(view.bottom) -
                                             margin
                                 : top == static_cast<float>(view.top) +
                                          margin,
                      "block hugs the viewport edge it docks to (vertical)");
                Check(right - left > 0.0f && bottom - top > 0.0f,
                      "block extent stays positive");
            }
        }
    }
    // 选择器格子下标直接换算成 Corner。
    Check(static_cast<int>(Corner::TopLeft) == 0 &&
              static_cast<int>(Corner::TopRight) == 1 &&
              static_cast<int>(Corner::BottomLeft) == 2 &&
              static_cast<int>(Corner::BottomRight) == 3,
          "corner order matches the 2x2 picker cells");
}

// ---- 2) 绘制位置（顶点回读） ---------------------------------------------
// 每个 28 字节 SpriteVertex：x,y,z,rhw,color,u,v。一个四边形 6 个顶点。
struct QuadBox { float left, top, right, bottom; float u = -1.0f; };

bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) < 0.05f;
}

bool HasQuad(const std::vector<QuadBox>& quads, const QuadBox& want) {
    for (const QuadBox& quad : quads) {
        if (NearlyEqual(quad.left, want.left) &&
            NearlyEqual(quad.top, want.top) &&
            NearlyEqual(quad.right, want.right) &&
            NearlyEqual(quad.bottom, want.bottom) &&
            (want.u < 0.0f || std::fabs(quad.u - want.u) < 0.00001f))
            return true;
    }
    return false;
}

void DrawChecks(IDirect3DDevice9* device) {
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    BlockGuard blockGuard(app.get());
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    renderer->device = device;

    IDirect3DVertexBuffer9* vb = nullptr;
    Check(SUCCEEDED(device->CreateVertexBuffer(0x445C0, D3DUSAGE_WRITEONLY,
                                               0, D3DPOOL_MANAGED, &vb,
                                               nullptr)),
          "allocate the HUD sprite batch");
    if (vb == nullptr)
        return;
    app->SpriteOverlayVertices() = vb;
    app->state.playbackActive = 0;
    app->state.optflag[0] = 1;   // 相机面板：跳过骨骼投影路径
    app->state.coordinateSystem = 0;
    app->EnglishUI() = 1;

    Check(SUCCEEDED(device->CreateVertexBuffer(0x6D60, D3DUSAGE_WRITEONLY,
        0, D3DPOOL_MANAGED, &app->OverlayVertices(), nullptr)), "allocate caption batch");
    Check(mikumikustudio::InitSceneFontTexture(app.get()), "build caption atlas");

    for (Corner corner : kCorners) {
        settings::ResetCacheForTest(corner);
        for (float sceneScale : kScales) {
            renderer->viewScale = sceneScale;
            RECT view{100, 50, 1380, 770};
            app->ViewportRect() = view;
            const float scale = FrameOverlayScale(app.get());
            Check(scale == 1.0f, "HUD size is independent of scene scale when it fits");
            const layout::Anchor a = layout::Make(view, scale, corner);
            app->SpriteOverlayPrimitiveCount() = 0;
            mikumikustudio::PrepareFrameSpriteOverlay(app.get());

            std::vector<QuadBox> quads;
            void* data = nullptr;
            if (SUCCEEDED(vb->Lock(0, 0x445C0, &data, D3DLOCK_READONLY)) &&
                data != nullptr) {
                const float* vertex = static_cast<const float*>(data);
                const int quadCount =
                    static_cast<int>(app->SpriteOverlayPrimitiveCount()) / 2;
                for (int i = 0; i < quadCount; ++i) {
                    const float* v = vertex + i * 6 * 7;
                    QuadBox box{v[0], v[1], v[0], v[1], v[5]};
                    for (int k = 0; k < 6; ++k) {
                        const float x = v[k * 7];
                        const float y = v[k * 7 + 1];
                        if (x < box.left) box.left = x;
                        if (x > box.right) box.right = x;
                        if (y < box.top) box.top = y;
                        if (y > box.bottom) box.bottom = y;
                        if (v[k * 7 + 5] < box.u) box.u = v[k * 7 + 5];
                    }
                    quads.push_back(box);
                }
                vb->Unlock();
            }
            Check(!quads.empty(), "the sprite batch produced quads");
            // 旋转行、位移行：右-130..-10，y 120..90 / 80..50。
            Check(HasQuad(quads, {a.X(130.0f), a.Y(120.0f), a.X(10.0f),
                                  a.Y(90.0f)}),
                  "rotate row quad sits at the docked anchor");
            Check(HasQuad(quads, {a.X(130.0f), a.Y(80.0f), a.X(10.0f),
                                  a.Y(50.0f)}),
                  "move row quad sits at the docked anchor");
            // 缩放行四格：x0 = X(170-40*col)，y 40..10。
            for (int col = 0; col < 4; ++col) {
                const bool leftSide = corner == Corner::TopLeft || corner == Corner::BottomLeft;
                const float left = col == 0 ? (leftSide ? 10.0f : 170.0f) : 170.0f - 40.0f * col;
                Check(HasQuad(quads, {a.X(left), a.Y(40.0f),
                                      a.X(left - 40.0f), a.Y(10.0f),
                                      0.001953125f + 0.078125f * col}),
                      "scale row cell sits at the docked anchor");
            }
            mikumikustudio::PrepareFrameTextOverlay(app.get());
            void* textData = nullptr;
            const int count = app->TextOverlayPrimitiveCount() * 3;
            if (app->OverlayVertices() && count >= 54 && SUCCEEDED(
                    app->OverlayVertices()->Lock(0, 0, &textData, D3DLOCK_READONLY))) {
                const float* vertex = static_cast<const float*>(textData) + (count - 54) * 7;
                float left = vertex[0], right = left, top = vertex[1], bottom = top;
                for (int i = 0; i < 54; ++i) {
                    if (vertex[i * 7] < left) left = vertex[i * 7];
                    if (vertex[i * 7] > right) right = vertex[i * 7];
                    if (vertex[i * 7 + 1] < top) top = vertex[i * 7 + 1];
                    if (vertex[i * 7 + 1] > bottom) bottom = vertex[i * 7 + 1];
                }
                app->OverlayVertices()->Unlock();
                const bool topSide = corner == Corner::TopLeft || corner == Corner::TopRight;
                Check(NearlyEqual((left + right) * 0.5f, a.X(70.0f)) &&
                      NearlyEqual((top + bottom) * 0.5f,
                          topSide ? view.top + 150.0f * scale : view.bottom - 150.0f * scale),
                      "caption vertices follow top/bottom docking without overlap");
            } else {
                Check(false, "read back the complete caption vertex batch");
            }

        }
    }
    settings::ResetCacheForTest(layout::kDefaultCorner);
    if (app->OverlayVertices()) app->OverlayVertices()->Release();
    app->OverlayVertices() = nullptr;
    if (app->state.sceneFontTexture)
        static_cast<IDirect3DTexture9*>(app->state.sceneFontTexture)->Release();
    app->state.sceneFontTexture = nullptr;
    vb->Release();
    app->SpriteOverlayVertices() = nullptr;
}

// ---- 3) 命中 -------------------------------------------------------------
void HitChecks(IDirect3DDevice9* device) {
    auto app = std::make_unique<MMDApp>();
    ResetAppState(*app);
    BlockGuard blockGuard(app.get());
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    renderer->device = device;
    app->EnglishUI() = 1;
    app->state.playbackActive = 0;
    app->state.optflag[0] = 0;   // 模型面板
    app->state.v32c = 0;         // 旋转 / 位移行都可用
    app->Hwnd() = nullptr;       // 拖拽分支不得移动真实指针

    struct Row { int xUnits, yUnits, modelOp, cameraOp; };
    const Row rows[] = {{110, 105, 9, 18}, {70, 105, 10, 19}, {30, 105, 11, 20},
                        {110, 65, 12, 15}, {70, 65, 13, 16}, {30, 65, 14, 17}};

    for (Corner corner : kCorners) {
        settings::ResetCacheForTest(corner);
        for (float sceneScale : kScales) {
            renderer->viewScale = sceneScale;
            RECT view{100, 50, 1380, 770};
            app->ViewportRect() = view;
            const float scale = FrameOverlayScale(app.get());
            Check(scale == 1.0f, "HUD size is independent of scene scale when it fits");
            const layout::Anchor a = layout::Make(view, scale, corner);
            for (const Row& row : rows) {
                const int x = static_cast<int>(a.X(
                    static_cast<float>(row.xUnits)));
                const int y = static_cast<int>(a.Y(
                    static_cast<float>(row.yUnits)));
                app->MouseX() = app->PreviousMouseX() = x;
                app->MouseY() = app->PreviousMouseY() = y;
                app->state.optflag[0] = 0;
                Check(mikumikustudio::HandleViewportToolPress(app.get()),
                      "row cell answers a press");
                Check(static_cast<int>(app->ViewportToolOperation()) ==
                          row.modelOp,
                      "model-panel row cell maps to its own axis operation");
                mikumikustudio::ReleaseLeftButtonState(app.get(), false);

                app->MouseX() = app->PreviousMouseX() = x;
                app->MouseY() = app->PreviousMouseY() = y;
                app->state.optflag[0] = 1;
                Check(mikumikustudio::HandleViewportToolPress(app.get()),
                      "row cell answers a press on the camera panel");
                Check(static_cast<int>(app->ViewportToolOperation()) ==
                          row.cameraOp,
                      "camera-panel row cell maps to its own axis operation");
                mikumikustudio::ReleaseLeftButtonState(app.get(), false);
            }

            // 坐标条（操作 21）：格心 (70, 145) 在命中带 (130..10, 167..125)。
            app->state.optflag[0] = 1;
            app->state.coordinateSystem = 0;
            app->MouseX() = app->PreviousMouseX() =
                static_cast<int>(a.X(70.0f));
            app->MouseY() = app->PreviousMouseY() =
                static_cast<int>(a.Y(corner == Corner::TopLeft || corner == Corner::TopRight ? -20.0f : 145.0f));
            Check(mikumikustudio::HandleViewportToolPress(app.get()),
                  "coordinate strip answers a press at every corner");
            Check(app->state.coordinateSystem == 1,
                  "one press advances the coordinate selector once");
            mikumikustudio::ReleaseLeftButtonState(app.get(), false);

            // 缩放行无选中骨骼：bone_scale::Enabled 为假，格心 (110, 25) 不命中。
            app->MouseX() = app->PreviousMouseX() =
                static_cast<int>(a.X(110.0f));
            app->MouseY() = app->PreviousMouseY() =
                static_cast<int>(a.Y(25.0f));
            Check(!mikumikustudio::HandleViewportToolPress(app.get()),
                  "scale row stays inert while bone scaling is disabled");
            mikumikustudio::ReleaseLeftButtonState(app.get(), false);
            // Real enabled scale operations must still address uniform/X/Y/Z
            // in that logical order, including the moved uniform button.
            auto model = std::make_unique<mikumikustudio::mdl::ModelRecord>();
            mikumikustudio::mdl::BoneRecord bone{};
            unsigned char selected = 1;
            model->boneTable = &bone;
            model->boneCount = 1;
            model->selectedBone = 0;
            model->boneSelection = &selected;
            model->physicsMode = 1;
            app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
            app->SetSelectedModelSlot(0);
            app->state.optflag[0] = 0;
            app->EditMode() = mikumikustudio::ViewportEditMode::Bone;
            for (int column = 0; column < 4; ++column) {
                const bool leftSide = corner == Corner::TopLeft || corner == Corner::BottomLeft;
                const float units = column == 0 ? (leftSide ? -10.0f : 150.0f)
                                                : 150.0f - 40.0f * column;
                app->MouseX() = app->PreviousMouseX() = static_cast<int>(a.X(units));
                app->MouseY() = app->PreviousMouseY() = static_cast<int>(a.Y(25.0f));
                Check(mikumikustudio::HandleViewportToolPress(app.get()) &&
                          static_cast<int>(app->ViewportToolOperation()) == 22 + column,
                      "enabled scale cell retains its own axis operation");
                mikumikustudio::ReleaseLeftButtonState(app.get(), false);
            }
            app->ModelSlot(0) = nullptr;

        }
    }
    settings::ResetCacheForTest(layout::kDefaultCorner);
}

// ---- 4) 设置往返与严格解析 -----------------------------------------------
void SettingsChecks() {
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring path = std::wstring(tempDir) +
                        L"viewport_corner_settings_test.ini";
    DeleteFileW(path.c_str());

    const auto writeRaw = [&](const char* text) {
        const size_t size = std::strlen(text);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(file);
    };
    const auto expectToken = [&](const char* text, bool valid, Corner corner,
                                 const char* what) {
        writeRaw(text);
        Corner loaded = Corner::TopLeft;
        const bool ok = settings::LoadFrom(path, &loaded);
        Check(ok == valid && loaded == corner, what);
    };

    Check(!settings::LoadFrom(path, nullptr), "null out pointer is refused");
    Corner loaded = Corner::TopLeft;
    Check(!settings::LoadFrom(path, &loaded) &&
              loaded == layout::kDefaultCorner,
          "missing file falls back to the default corner");
    for (Corner corner : kCorners) {
        Check(settings::SaveTo(path, corner), "save the corner token");
        Check(settings::LoadFrom(path, &loaded) && loaded == corner,
              "round trip keeps every corner value");
    }

    // 严格解析：值必须整行恰好是 tl/tr/bl/br，且只认 [viewport] 节。
    expectToken("[viewport]\r\ncorner=tl\r\n", true, Corner::TopLeft,
                "corner=tl under [viewport] reads TopLeft");
    expectToken("[viewport]\r\nCorner=BL\r\n", true, Corner::BottomLeft,
                "key and value are ASCII case-insensitive");
    expectToken("[viewport]\r\ncorner=tr \r\n", true, Corner::TopRight,
                "trailing spaces around the token are tolerated");
    expectToken("[viewport]\r\ncorner=tl1\r\n", false,
                layout::kDefaultCorner,
                "a token with a tail (corner=tl1) is rejected, not read as tl");
    expectToken("[viewport]\r\ncorner=tl x\r\n", false,
                layout::kDefaultCorner,
                "a token with a trailing word is rejected");
    expectToken("[viewport]\r\ncorner=\r\n", false, layout::kDefaultCorner,
                "an empty value is rejected");
    expectToken("[viewport]\r\ncorner=garbage\r\n", false,
                layout::kDefaultCorner,
                "a corrupt token falls back to the default corner");
    expectToken("[other]\r\ncorner=tl\r\n", false, layout::kDefaultCorner,
                "corner= outside the [viewport] section is ignored");
    expectToken("[other]\r\ncorner=tl\r\n[viewport]\r\ncorner=br\r\n", true,
                Corner::BottomRight,
                "the [viewport] section is the one that counts");

    // 原子写入不留 .tmp 残骸。
    Check(settings::SaveTo(path, Corner::TopLeft), "save before the check");
    Check(GetFileAttributesW((path + L".tmp").c_str()) ==
              INVALID_FILE_ATTRIBUTES,
          "no .tmp file is left behind");
    DeleteFileW(path.c_str());
}

}  // namespace

int main() {
    MappingChecks();
    SettingsChecks();

    HWND window = CreateWindowW(L"STATIC", L"Viewport corner layout",
                                WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create the hidden test window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                      &pp, &device)),
          "create a real GPU device");
    if (device != nullptr) {
        DrawChecks(device);
        HitChecks(device);
    }
    if (device != nullptr) device->Release();
    if (d3d != nullptr) d3d->Release();
    if (window != nullptr) DestroyWindow(window);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("viewport_corner_layout_tests: all checks passed\n");
    return 0;
}
