// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Field defects covered here (2026-09-22 report):
//   * the viewport operation axis showed the wrong colour on its Y and Z arms
//     because the axis.x materials were copied with the x86 D3DXMATERIAL
//     stride on x64;
//   * the coordinate-system caption (local / global / view) was sampled one
//     character per 16-pixel atlas cell, so the proportional - and in the
//     Chinese UI, wide - captions were split, measured with the English
//     advance and left the states at different offsets, with 局部 off centre.
// 2026-09-23 report: the caption's wording and face.  The three states are
// capitalized English words, the caption is painted with the sheet's serif face
// in every language, and the camera/light/accessory panel names its third state
// "Accessory" - a camera's own frame is the view, so the panel has no view
// state - while the model/bone panel keeps "View".
// 2026-09-23 report (second): the Japanese and English captions used
// different faces; the Chinese caption was set in SimSun; and all three were
// drawn with an outline whose weight increased with the HUD scale.
// 2026-09-23 report (third): the outline and white face still blended into a
// soft edge because the HUD scale magnified the 30-pixel atlas, and with
// no accessory loaded the selector still offered - and labelled itself with -
// the accessory state.
// 2026-09-23 report (fourth): a single click advanced the selector by two states (a lost
// capture re-armed the press edge), and a second click within the double-click
// interval arrived as WM_LBUTTONDBLCLK, which never reached the strip at all.
// 2026-09-26: the bone-scale HUD row.  The rotate and move rows moved up one
// 40-logical-pixel band (bottom-120..-90 and bottom-80..-50) to make room for
// the uniform/X/Y/Z scale row at bottom-40..-10, the coordinate caption
// follows the rows up by the same 40, and the scale row is inert while the
// camera/light/accessory panel is active, while no bone is selected or during
// playback.  Inside the model panel the Light/Camera edit modes are the bone
// move and bone rotate cursors and keep scaling enabled.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <utility>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_translation.hpp"

using namespace mikumikustudio;

namespace mikumikustudio {
// Panel-command dispatcher (src/window/command_control_500.cpp); the definition
// is in this namespace, so the declaration must be here as well.
void CmdControl500(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void CmdFileMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void ConsumeNumpadViewPresets(MMDApp*);
}  // namespace mikumikustudio

namespace {

// Leave enough room for the full-size FPS and operation blocks. Narrow-pane
// shrinking is covered by viewport_panel_checks and viewport_corner_layout_tests.
constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr float kTolerance = 0.002f;

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

bool Near(float value, float expected) {
    return std::fabs(value - expected) <= 0.002f;
}

void CheckDiffuse(const D3DMATERIAL9& material, float red, float green,
                  float blue, const char* what) {    if (!(std::fabs(material.Diffuse.r - red) <= kTolerance &&
          std::fabs(material.Diffuse.g - green) <= kTolerance &&
          std::fabs(material.Diffuse.b - blue) <= kTolerance)) {
        std::fprintf(stderr,
                     "FAIL: %s diffuse is (%.3f, %.3f, %.3f), expected "
                     "(%.3f, %.3f, %.3f)\n",
                     what, material.Diffuse.r, material.Diffuse.g,
                     material.Diffuse.b, red, green, blue);
        std::exit(1);
    }
}

struct Caption {
    int ink;
    float centerX;
    float centerY;
    int darkPixels;
    int lightPixels;
    int width;
    int height;
};

// Extents of the HUD text batch as emitted, used to check which atlas cell a
// caption samples and where it lands.
struct TextBatch {
    float minU = 0.0f;
    float maxU = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minX = 0.0f;
    float maxX = 0.0f;
};

TextBatch ReadTextBatch(MMDApp* app, int maxVertices) {
    TextBatch batch{};
    IDirect3DVertexBuffer9* vb = app->OverlayVertices();
    int count = static_cast<int>(app->TextOverlayPrimitiveCount()) * 3;
    if (maxVertices > 0 && count > maxVertices)
        count = maxVertices;
    if (vb == nullptr || count <= 0)
        return batch;
    void* data = nullptr;
    if (FAILED(vb->Lock(0, 0x6D60, &data, D3DLOCK_READONLY)) || data == nullptr)
        return batch;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (int i = 0; i < count; ++i) {
        float vertex[7];
        std::memcpy(vertex, bytes + static_cast<std::size_t>(i) * 28,
                    sizeof(vertex));
        const float x = vertex[0];
        const float y = vertex[1];
        const float u = vertex[5];
        if (i == 0) {
            batch.minU = batch.maxU = u;
            batch.minY = batch.maxY = y;
            batch.minX = batch.maxX = x;
            continue;
        }
        batch.minU = u < batch.minU ? u : batch.minU;
        batch.maxU = u > batch.maxU ? u : batch.maxU;
        batch.minY = y < batch.minY ? y : batch.minY;
        batch.maxY = y > batch.maxY ? y : batch.maxY;
        batch.minX = x < batch.minX ? x : batch.minX;
        batch.maxX = x > batch.maxX ? x : batch.maxX;
    }
    vb->Unlock();
    return batch;
}

// Quads of the HUD sprite batch that sample one atlas band, split by the
// disabled tint.  The scale row repeats its own UVs under a black
// semi-transparent diffuse (0xA0000000) while bone scaling is off, so the
// wash must share the row's u/v extents instead of covering it with a plate.
struct SpriteBand {
    int quads = 0;         // matching quads without the disabled tint
    int washQuads = 0;     // matching quads carrying 0xA0000000 on all corners
    float minU = 0.0f;
    float maxU = 0.0f;
    float washMinU = 0.0f;
    float washMaxU = 0.0f;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
};

// Tight match for the dyadic atlas literals: Near's 0.002 tolerance would
// blur cells one 512-sheet row (0.001953) apart.
bool Close(float a, float b) {
    return std::fabs(a - b) <= 1.0e-4f;
}

SpriteBand ReadSpriteBand(MMDApp* app, float bandV0, float bandV1) {
    SpriteBand band{};
    IDirect3DVertexBuffer9* vb = app->SpriteOverlayVertices();
    const int count = static_cast<int>(app->SpriteOverlayPrimitiveCount()) * 3;
    if (vb == nullptr || count <= 0)
        return band;
    void* data = nullptr;
    if (FAILED(vb->Lock(0, 0x445C0, &data, D3DLOCK_READONLY)) || data == nullptr)
        return band;
    const auto* bytes = static_cast<const unsigned char*>(data);
    band.minU = band.washMinU = band.minX = band.minY = 1.0e30f;
    band.maxU = band.washMaxU = band.maxX = band.maxY = -1.0e30f;
    for (int i = 0; i + 5 < count; i += 6) {
        float lowU = 1.0f, highU = 0.0f, lowV = 1.0f, highV = 0.0f;
        float lowX = 1.0e30f, highX = -1.0e30f;
        float lowY = 1.0e30f, highY = -1.0e30f;
        bool washed = true;
        for (int k = 0; k < 6; ++k) {
            float vertex[7];
            std::memcpy(vertex, bytes + static_cast<std::size_t>(i + k) * 28,
                        sizeof(vertex));
            DWORD color = 0;
            std::memcpy(&color, &vertex[4], sizeof(color));
            if (color != 0xA0000000u)
                washed = false;
            if (vertex[5] < lowU) lowU = vertex[5];
            if (vertex[5] > highU) highU = vertex[5];
            if (vertex[6] < lowV) lowV = vertex[6];
            if (vertex[6] > highV) highV = vertex[6];
            if (vertex[0] < lowX) lowX = vertex[0];
            if (vertex[0] > highX) highX = vertex[0];
            if (vertex[1] < lowY) lowY = vertex[1];
            if (vertex[1] > highY) highY = vertex[1];
        }
        // Band membership is exact on both v edges; the u filter drops the
        // bone-marker cells (u ~0.46..0.50) that can share the v range.
        if (!(Close(lowV, bandV0) && Close(highV, bandV1)) || highU > 0.32f)
            continue;
        if (lowU < band.minU) band.minU = lowU;
        if (highU > band.maxU) band.maxU = highU;
        if (lowX < band.minX) band.minX = lowX;
        if (highX > band.maxX) band.maxX = highX;
        if (lowY < band.minY) band.minY = lowY;
        if (highY > band.maxY) band.maxY = highY;
        if (washed) {
            if (lowU < band.washMinU) band.washMinU = lowU;
            if (highU > band.washMaxU) band.washMaxU = highU;
            ++band.washQuads;
        } else {
            ++band.quads;
        }
    }
    vb->Unlock();
    return band;
}

// Reads the selector rectangle back and reports where the caption's pixels sit.
Caption MeasureCaption(IDirect3DDevice9* device, IDirect3DSurface9* read,
                       const RECT& strip) {
    Caption caption{};
    IDirect3DSurface9* back = nullptr;
    Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)),
          "back buffer for the caption read-back");
    Check(SUCCEEDED(device->GetRenderTargetData(back, read)),
          "copy the rendered caption");
    back->Release();

    D3DLOCKED_RECT locked{};
    Check(SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY)),
          "lock the caption read-back");
    int firstX = 0;
    int lastX = 0;
    int firstY = 0;
    int lastY = 0;
    for (int y = strip.top; y < strip.bottom; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(
            static_cast<const char*>(locked.pBits) + y * locked.Pitch);
        for (int x = strip.left; x < strip.right; ++x) {
            const DWORD pixel = row[x];
            const int blue = static_cast<int>(pixel & 0xFF);
            const int green = static_cast<int>((pixel >> 8) & 0xFF);
            const int red = static_cast<int>((pixel >> 16) & 0xFF);
            const int brightest = red > green ? (red > blue ? red : blue)
                                              : (green > blue ? green : blue);
            const int darkest = red < green ? (red < blue ? red : blue)
                                            : (green < blue ? green : blue);
            // The viewport behind the caption is the cleared mid grey, so both
            // the black outline and the white face stand out from it.
            const int distance = std::abs(red - 0x80) + std::abs(green - 0x80) +
                                 std::abs(blue - 0x80);
            if (distance > 90) {
                ++caption.ink;
                if (caption.ink == 1 || x < firstX)
                    firstX = x;
                if (x > lastX)
                    lastX = x;
                if (caption.ink == 1 || y < firstY)
                    firstY = y;
                if (y > lastY)
                    lastY = y;
            }
            if (brightest < 80)
                ++caption.darkPixels;
            if (darkest > 200)
                ++caption.lightPixels;
        }
    }
    read->UnlockRect();
    if (caption.ink > 0) {
        // The painted box centre, not the ink-weighted centre: a serif face
        // carries heavier serifs along the baseline and "Accessory" adds a
        // descender, either of which pulls a weighted centre a few pixels below
        // the band all the states share (the reported caption looked low).
        caption.centerX =
            (static_cast<float>(firstX) + static_cast<float>(lastX)) * 0.5f;
        caption.centerY =
            (static_cast<float>(firstY) + static_cast<float>(lastY)) * 0.5f;
        caption.width = lastX - firstX + 1;
        caption.height = lastY - firstY + 1;
    }
    return caption;
}

// Reads one pixel back from the rendered frame.
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

// Forwards the mouse messages the operation buttons answer to the real main
// window procedure, so the press/duplicate-press/double-click path (including
// its mouse-position latch) runs exactly as it does in the application.
LRESULT CALLBACK ToolPressWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        return MainWndProc(hwnd, msg, wParam, lParam);
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int main() {
    viewport_corner_settings::ResetCacheForTest(viewport_tool_layout::Corner::BottomRight);
    HWND window = CreateWindowW(L"STATIC", L"Coordinate axis regression",
                                WS_POPUP, 0, 0, kWidth, kHeight + 192, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create the test window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp,
                                      &device)),
          "create a real GPU device");

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    // This fixture has no sidebar. Reserve the normal bottom panels in the
    // hidden host so its presentation frame ends at the test render target.
    app->SidebarWidth() = -9;
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    renderer->device = device;
    renderer->viewScale = 1.0f;

    // ---- the operation axis keeps the colours of the original axis.x ------
    // MMD's own mesh: material 0 red spans +X, material 1 blue spans -Z,
    // material 2 green spans +Y, material 3 yellow is the origin box and
    // materials 4/5 are the black outline shells.
    mdl::AccessoryRecord axisRecord{};
    app->state.axisMeshObject = &axisRecord;
    Check(InitAxisMesh(app.get()), "load axis.x from the embedded resource");
    auto* axis = mdl::Accessory(app->state.axisMeshObject);
    Check(axis != nullptr && axis->materials != nullptr,
          "axis.x materials were parsed");
    Check(axis->materialCount >= 5, "axis.x keeps all six material slots");
    auto* materials = static_cast<D3DMATERIAL9*>(axis->materials);
    CheckDiffuse(materials[0], 1.000f, 0.000f, 0.035f, "X arm");
    CheckDiffuse(materials[1], 0.000f, 0.043f, 1.000f, "Z arm");
    CheckDiffuse(materials[2], 0.039f, 1.000f, 0.000f, "Y arm");
    CheckDiffuse(materials[3], 0.996f, 1.000f, 0.000f, "origin box");

    // ---- the coordinate caption is one measured cell, centred -------------
    app->ViewportRect() = {0, 0, kWidth, kHeight};
    Check(SUCCEEDED(device->CreateVertexBuffer(0x6D60, D3DUSAGE_WRITEONLY,
                                               0x144, D3DPOOL_MANAGED,
                                               &app->OverlayVertices(),
                                               nullptr)),
          "allocate the HUD text batch");
    IDirect3DSurface9* read = nullptr;
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(
              kWidth, kHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &read,
              nullptr)),
          "allocate the caption read-back");

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    // ---- every caption is in the atlas, in the sheet's serif face ---------
    // The selector spells its three states in capitalized English, and the
    // camera/light/accessory panel needs a fourth word for its own third state.
    app->EnglishUI() = kUiEnglish;
    app->state.optflag[0] = 0;
    Check(InitSceneFontTexture(app.get()), "build the English HUD atlas");
    for (const char* key : {"Local", "Global", "View", "Accessory"}) {
        SceneFontCell cell{};
        if (!SceneFontLabelCell(key, renderer->viewScale, &cell)) {
            std::fprintf(stderr, "FAIL: the atlas has no \"%s\" caption\n", key);
            std::exit(1);
        }
    }
    Check(std::wcscmp(UiCaptionFaceName(kUiEnglish), L"Times New Roman") == 0,
          "the English caption uses the sheet's serif face");
    // The Japanese UI uses the same Latin words for the caption, so it must
    // use the same typeface: MS PMincho rendered the same word in a much
    // lighter typeface of its own, causing the reported "the Japanese and the
    // English captions do not use the same font".
    Check(std::wcscmp(UiCaptionFaceName(kUiJapanese),
                      UiCaptionFaceName(kUiEnglish)) == 0,
          "the Japanese and English captions share one face");
    // The Chinese UI shows the translated Han wording, where the label face
    // reads better than SimSun's hairlines.
    Check(std::wcscmp(UiCaptionFaceName(kUiChinese),
                      L"Microsoft YaHei UI") == 0,
          "the Chinese caption uses the sans-serif label face");

    // ---- the third state follows the active panel -------------------------
    // "Accessory" belongs to the camera/light/accessory panel: state 2 edits
    // the selected accessory there (ModeCameraAdjust / ModeAngleAdjust), while
    // the model/bone panel drags along the screen axes and keeps "View".
    const auto captionsCell = [&](const char* key) {
        SceneFontCell cell{};
        if (!SceneFontLabelCell(key, renderer->viewScale, &cell))
            return false;
        PrepareFrameTextOverlay(app.get());
        const TextBatch batch = ReadTextBatch(app.get(), -1);
        return Near(batch.minU, cell.u0) && Near(batch.maxU, cell.u1);
    };
    // The accessory state requires an accessory to edit; slot 0 holds the record
    // that the panel edits (CoordinateStateLimit reads the same slot).
    mdl::AccessoryRecord selectorAccessory{};
    app->AccessorySlot(0) = &selectorAccessory;
    app->SelectedAccessorySlot() = 0;
    app->EnglishUI() = kUiEnglish;
    app->state.fpsOverlayEnabled = 0;
    app->state.playbackActive = 0;
    app->state.coordinateSystem = 2;
    app->state.optflag[0] = 0;
    Check(captionsCell("View"),
          "the model/bone panel captions its third state View");
    app->state.optflag[0] = 1;
    Check(CoordinateStateLimit(app.get()) == 3,
          "an accessory keeps the third coordinate state reachable");
    Check(captionsCell("Accessory"),
          "the camera/accessory panel captions its third state Accessory");
    app->state.optflag[0] = 0;

    // ---- With an empty accessory list the selector stops at Local/Global --------
    // The third state on the camera/light/accessory panel edits the currently
    // selected accessory. With no accessory selected, the selector must not offer,
    // or be labelled with, a state that edits nothing.  CoordinateStateLimit is the
    // rule shared by the strip click (operation 21) and the 'L' key; the
    // caption clamps a stale state byte to the same range, so the selector can never
    // read "Accessory" while the list is empty.
    app->state.optflag[0] = 1;
    app->state.coordinateSystem = 2;
    app->AccessorySlot(0) = nullptr;
    Check(CoordinateStateLimit(app.get()) == 2,
          "an empty accessory list keeps the selector on Local/Global");
    Check(captionsCell("Local"),
          "the caption falls back to Local while no accessory exists");
    app->state.optflag[0] = 0;
    Check(CoordinateStateLimit(app.get()) == 3,
          "the model panel keeps View without any accessory");
    app->state.optflag[0] = 1;
    app->AccessorySlot(0) = &selectorAccessory;
    Check(CoordinateStateLimit(app.get()) == 3,
          "selecting an accessory restores the third state");

    // ---- an accessory anywhere in the list keeps the state reachable -------
    // Loaded accessories reside in the shared object list, and the selection
    // byte points to an empty slot until the user picks a row: checking only the
    // selected slot hid the accessory state when the panel was full, which is the
    // reported "no matter how often I click, the mode does not switch".
    mdl::AccessoryRecord otherAccessory{};
    app->AccessorySlot(0) = nullptr;
    app->SelectedAccessorySlot() = 0;
    app->AccessorySlot(5) = &otherAccessory;
    Check(CoordinateStateLimit(app.get()) == 3,
          "an accessory in another slot keeps the third state reachable");
    app->state.coordinateSystem = 2;
    Check(EffectiveCoordinateState(app.get()) == 2,
          "the third state stays reachable while the list is not empty");
    app->AccessorySlot(5) = nullptr;
    Check(CoordinateStateLimit(app.get()) == 2,
          "emptying the list hides the third state again");
    app->AccessorySlot(0) = &selectorAccessory;
    app->state.coordinateSystem = 0;

    // ---- Clamp a stale accessory state for every consumer ------------
    // Deleting the last accessory leaves 2 in the state byte. Cycling can no longer
    // reach that state, but the consumers read the byte directly: the
    // accessory drag handlers, the bone-handle axes, the operation-axis gizmo,
    // the mouse-wheel refresh gate, and the caption — so the clamp must use the
    // single shared helper, applied once per frame and at every read.
    app->state.optflag[0] = 1;
    app->AccessorySlot(0) = nullptr;
    app->state.coordinateSystem = 2;
    Check(EffectiveCoordinateState(app.get()) == 0,
          "a stale accessory state reads as Local while no accessory exists");
    NormalizeCoordinateState(app.get());
    Check(app->state.coordinateSystem == 0,
          "the frame tick clears the stale accessory state");
    app->AccessorySlot(0) = &selectorAccessory;
    app->state.coordinateSystem = 2;
    Check(EffectiveCoordinateState(app.get()) == 2,
          "the accessory state stays reachable while an accessory exists");
    app->state.optflag[0] = 0;
    Check(EffectiveCoordinateState(app.get()) == 2,
          "the model panel keeps its View state without an accessory");
    app->state.coordinateSystem = 0;
    app->state.optflag[0] = 0;
    app->state.coordinateSystem = 0;
    app->state.optflag[0] = 0;

    // ---- one press moves the selector exactly one state -------------------
    // The frame driver's input poll reads "idle -> down" as a press, and the
    // strip is a toggle, so a second press of the same physical click advances the
    // selector twice: the caption flashes the other state while the mode returns
    // unchanged (reported as "the switch is not smooth"). Only a
    // message-level press may advance it, and a lost capture while the button is
    // still held must not count as a fresh press.
    app->EnglishUI() = kUiEnglish;
    app->state.optflag[0] = 1;
    app->state.coordinateSystem = 0;
    app->state.playbackActive = 0;
    app->Hwnd() = window;
    app->MouseX() = app->ViewportRect().right - 70;
    app->MouseY() = app->ViewportRect().bottom - 145;
    Check(HandleViewportToolPress(app.get()),
          "the selector strip answers a press");
    Check(app->state.coordinateSystem == 1,
          "one press advances the coordinate selector once");
    Check(app->LeftMouseButtonState() == 3,
          "the press edge is consumed for the frame driver");
    ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/true);
    Check(app->LeftMouseButtonState() == 3,
          "a lost capture with the button held does not re-arm the press");
    Check(app->state.coordinateSystem == 1,
          "a lost capture does not advance the selector again");
    ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
    Check(app->LeftMouseButtonState() == 0,
          "the real mouse-up clears the tracked button state");

    // ---- the frame poll and the message must not both handle one press ------
    // The main loop calls FrameDriver only while the message queue is empty
    // (winmain.cpp), so a press during that window is first seen by the frame
    // driver's PollKey: its GetAsyncKeyState fallback reports the button while
    // WM_LBUTTONDOWN is still queued, and that path runs the strip once. When
    // the queued message is finally dispatched, it must not run the same press
    // again. Two runs advance the selector two states, and in a two-state cycle
    // that becomes "no matter how often I click, the mode
    // does not switch".
    app->state.coordinateSystem = 0;
    Check(HandleViewportToolPress(app.get()), "the strip answers the press");
    Check(app->state.coordinateSystem == 1, "the press advances the selector once");
    Check(HandleViewportToolPress(app.get()),
          "the delayed message still belongs to the tool");
    Check(app->state.coordinateSystem == 1,
          "the same press does not advance the selector a second time");
    ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
    Check(HandleViewportToolPress(app.get()),
          "the strip answers the next press");
    Check(app->state.coordinateSystem == 2,
          "a press after the mouse-up advances the selector again");
    ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
    app->state.coordinateSystem = 0;
    app->state.optflag[0] = 0;

    // ---- the second click of a double click also toggles ---------------------
    // The window class sets CS_DBLCLKS, so a second click within the
    // double-click time arrives as WM_LBUTTONDBLCLK instead of WM_LBUTTONDOWN.
    // The strip is a toggle and must still advance one state, while a click outside
    // the strip must remain with the other viewport tools.
    app->state.optflag[0] = 1;
    app->state.coordinateSystem = 0;
    app->MouseX() = app->ViewportRect().right - 70;
    app->MouseY() = app->ViewportRect().bottom - 145;
    Check(HandleCoordinateStripPress(app.get()),
          "the strip answers the double click that carries a second press");
    Check(app->state.coordinateSystem == 1,
          "the second click of a double click moves the selector one state");
    app->MouseX() = app->ViewportRect().left + 5;
    app->MouseY() = app->ViewportRect().top + 5;
    Check(!HandleCoordinateStripPress(app.get()),
          "a double click away from the strip is left to the other tools");
    Check(app->state.coordinateSystem == 1,
          "a double click away from the strip does not move the selector");
    app->state.coordinateSystem = 0;
    app->state.optflag[0] = 0;

    // ---- a press message must not overwrite a centred drag's origin --------
    // The six operation buttons run as centred drags: their press warps the
    // pointer to the viewport centre and stores that centre as *both* the
    // current and the previous mouse position.  The original writes the stored
    // position only from its mouse-move handler (x64 0x381A4..0x3821D) -
    // WM_LBUTTONDOWN there only takes capture (0xA4B38) and
    // WM_LBUTTONDBLCLK only calls its double-click handler (0xA4A86) - and a
    // store that follows a cursor warp moves the previous position with it, so
    // no button message can ever replace the drag's origin with the physical
    // button's coordinates.  This port latches a press's own coordinates to fix
    // the stale-position click (see the WM_LBUTTONDOWN comment); that latch
    // must leave the origin alone while the warp it would overwrite is still
    // pending, because otherwise the next frame applies the whole
    // centre-to-button distance as one delta - the reported "the view jumps
    // after a fast double click on the operation buttons".
    {
        const HWND previousHwnd = static_cast<HWND>(app->Hwnd());
        MMDApp* previousApp = g_Block;
        g_Block = app.get();
        WNDPROC previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ToolPressWndProc)));
        app->Hwnd() = nullptr;         // WarpCursor must not move the real pointer
        app->state.optflag[0] = 1;     // camera / light / accessory panel
        app->state.playbackActive = 0;
        app->state.v32c = 0;
        app->state.coordinateSystem = 0;
        // Start from an idle button: the strip checks above leave the tracked
        // state held.
        ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
        const RECT view = app->ViewportRect();
        const int buttonX = view.right - 70;    // move row, middle column
        const int buttonY = view.bottom - 65;   // camera translate Y (op 16)
        app->MouseX() = app->PreviousMouseX() = buttonX;
        app->MouseY() = app->PreviousMouseY() = buttonY;
        Check(HandleViewportToolPress(app.get()),
              "an operation button answers the press");
        const int originX = app->MouseX();
        const int originY = app->MouseY();
        const bool centred =
            originX == (view.left + view.right) / 2 &&
            originY == (view.top + view.bottom) / 2;
        // The press warped the pointer to that centre; the warp is in flight, as
        // it is when the frame driver's poll handled the press and the queued
        // message is dispatched afterwards.
        app->state.separateWindowMouseSeen = 1;
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(buttonX, buttonY));
        const int afterPressedX = app->MouseX();
        const int afterPressedY = app->MouseY();
        SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON,
                     MAKELPARAM(buttonX, buttonY));
        const int afterDoubleX = app->MouseX();
        const int afterDoubleY = app->MouseY();
        const int previousX = app->PreviousMouseX();
        const int previousY = app->PreviousMouseY();
        // A move queued before SetCursorPos can arrive after the press or a
        // later edge recenter. Neither docked nor detached input may treat it
        // as movement away from the new origin.
        app->state.a06B5 = 0;
        app->state.separateWindowMouseSeen = 0;
        HandleMouseMove(buttonX, buttonY);
        Check(app->MouseX() == originX && app->MouseY() == originY &&
              app->PreviousMouseX() == originX && app->PreviousMouseY() == originY,
              "a queued docked mouse move cannot move a centred drag origin");
        MicWndProc(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(buttonX, buttonY));
        Check(app->MouseX() == originX && app->MouseY() == originY &&
              app->PreviousMouseX() == originX && app->PreviousMouseY() == originY,
              "a queued detached mouse move cannot move a centred drag origin");
        ApplyCenteredDragCursorSample(app.get(), originX + 3, originY - 7);
        Check(app->MouseX() - app->PreviousMouseX() == 3 &&
              app->MouseY() - app->PreviousMouseY() == -7,
              "the live cursor sample keeps the first real drag delta");
        MouseInteractionEnd(app.get());
        ApplyCenteredDragCursorSample(app.get(), originX + 3, originY - 7);
        Check(app->MouseX() == app->PreviousMouseX() && app->MouseY() == app->PreviousMouseY(),
              "a repeated cursor sample cannot apply movement twice");
        HandleViewportToolRelease(app.get());
        SetWindowLongPtrW(window, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(previousProc));
        app->Hwnd() = previousHwnd;
        g_Block = previousApp;
        app->state.optflag[0] = 0;

        Check(app->ViewToolDragOperation() == ViewportToolAction::None &&
              app->InteractionDragMode() == ViewportDragMode::None,
              "the operation button owns a centred drag that the release ends");
        Check(centred, "the operation button recentres the drag origin");
        Check(afterPressedX == originX && afterPressedY == originY,
              "a repeated press keeps the centred drag origin");
        Check(afterDoubleX == originX && afterDoubleY == originY,
              "the double-click message keeps the centred drag origin");
        Check(previousX == originX && previousY == originY,
              "the centred drag still reports a zero delta");
    }

    // ---- a running centred drag re-centres a pointer that leaves the viewport
    // The original warps the pointer back to the middle of the viewport bounds
    // once per frame while the drag runs (x64 0x323DA/0x326B4/0x32761/0x36BB6),
    // so the gesture has no vertical limit; the warp stores the middle as both
    // the current and the previous position, so it adds no delta of its own.
    {
        const HWND previousHwnd = static_cast<HWND>(app->Hwnd());
        MMDApp* previousApp = g_Block;
        g_Block = app.get();
        app->Hwnd() = nullptr;         // the warp itself must not move the pointer
        app->state.optflag[0] = 1;
        app->state.playbackActive = 0;
        app->state.coordinateSystem = 0;
        const RECT view = app->ViewportRect();
        app->MouseX() = app->PreviousMouseX() = view.right - 70;
        app->MouseY() = app->PreviousMouseY() = view.bottom - 65;
        Check(HandleViewportToolPress(app.get()),
              "an operation button answers the press for the recentring check");
        // Drag far below the viewport: the vertical axis must come back to the
        // middle, and the horizontal one must stay where the user left it.
        app->MouseX() = view.right - 70;
        app->PreviousMouseX() = view.right - 70;
        app->MouseY() = view.bottom + 40;
        app->PreviousMouseY() = view.bottom + 10;
        app->state.mouseJumped = 0;
        RecentreCenteredDragCursor(app.get());
        const int recentredX = app->MouseX();
        const int recentredY = app->MouseY();
        const int recentredPreviousY = app->PreviousMouseY();
        const int middleX = (view.left + view.right) / 2;
        const int middleY = (view.top + view.bottom) / 2;
        Check(recentredY == middleY && recentredPreviousY == middleY,
              "a pointer that leaves the viewport comes back to the middle");
        Check(recentredX == view.right - 70,
              "the axis inside the viewport is left alone");
        // A stored position that follows a cursor warp must not be re-centred:
        // it is the warp's value, not the user's.
        app->MouseY() = view.bottom + 40;
        app->state.mouseJumped = 1;
        RecentreCenteredDragCursor(app.get());
        const int jumpedY = app->MouseY();
        app->state.mouseJumped = 0;
        // Without a running drag there is nothing to re-centre either.
        HandleViewportToolRelease(app.get());
        app->MouseY() = view.bottom + 40;
        RecentreCenteredDragCursor(app.get());
        const int releasedY = app->MouseY();
        app->Hwnd() = previousHwnd;
        g_Block = previousApp;
        app->state.optflag[0] = 0;
        app->MouseX() = app->PreviousMouseX() = 0;
        app->MouseY() = app->PreviousMouseY() = 0;
        Check(jumpedY == view.bottom + 40,
              "a position that follows a cursor warp is not re-centred");
        Check(releasedY == view.bottom + 40,
              "no recentring happens once the drag has ended");
    }

    // The latch itself still has to work while no centred drag owns the press:
    // that is the stale-position case it was added for (a press whose move was
    // never stored, e.g. the first click after the window is re-activated).
    {
        const HWND previousHwnd = static_cast<HWND>(app->Hwnd());
        MMDApp* previousApp = g_Block;
        g_Block = app.get();
        WNDPROC previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ToolPressWndProc)));
        app->Hwnd() = nullptr;
        app->state.optflag[0] = 1;
        app->state.separateWindowMouseSeen = 1;   // another warp is in flight
        app->ViewToolDragOperation() = ViewportToolAction::None;
        app->LeftMouseButtonState() = 0;
        app->MouseX() = 0;
        app->MouseY() = 0;
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 60));
        const int latchedX = app->MouseX();
        const int latchedY = app->MouseY();
        SetWindowLongPtrW(window, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(previousProc));
        app->Hwnd() = previousHwnd;
        g_Block = previousApp;
        app->state.optflag[0] = 0;
        app->state.separateWindowMouseSeen = 0;
        Check(latchedX == 40 && latchedY == 60,
              "a press outside a centred drag still latches its position");
    }

    // The atlas stores a 30-pixel and a 60-pixel copy of every caption.  At a
    // HUD scale of 2.0 (a 2560-wide desktop) the 60-pixel copy is sampled almost
    // one-to-one rather than magnifying the 30-pixel copy twofold; that
    // magnification is what made the outline and the white face merge into a soft
    // edge.  Both tiers must draw the caption at the same size, so the switch trades
    // only sharpness, never the layout.
    SceneFontCell smallCell{};
    SceneFontCell largeCell{};
    Check(SceneFontLabelCell("Global", 1.0f, &smallCell) &&
              smallCell.band == kSceneFontRowHeight,
          "a small HUD scale samples the 30-pixel caption");
    Check(SceneFontLabelCell("Global", 2.0f, &largeCell) &&
              largeCell.band == 2.0f * kSceneFontRowHeight,
          "a 2.0 HUD scale samples the 60-pixel caption");
    const auto paintedWidth = [](const SceneFontCell& cell, float hudScale) {
        return (cell.u1 - cell.u0) * kSceneFontAtlasSize *
               (kSceneFontRowHeight * hudScale / cell.band);
    };
    const float smallPainted = paintedWidth(smallCell, 2.0f);
    const float largePainted = paintedWidth(largeCell, 2.0f);
    if (!(std::fabs(smallPainted - largePainted) <= 4.0f)) {
        std::fprintf(stderr,
                     "FAIL: the two caption tiers paint %.1f and %.1f px wide "
                     "at HUD scale 2.0\n",
                     smallPainted, largePainted);
        std::exit(1);
    }

    const unsigned char languages[] = {kUiJapanese, kUiEnglish, kUiChinese};

    // Both panels share the selector, so every language/panel combination
    // must land on the same centre. Each panel's own atlas is built first:
    // the atlas is per language, but the caption word it samples is per panel.
    const int panels[] = {0, 1};
    for (unsigned char language : languages) {
        for (int panel : panels) {
            app->EnglishUI() = language;
            app->state.coordinateSystem = 0;
            app->state.optflag[0] = static_cast<unsigned char>(panel);
            Check(InitSceneFontTexture(app.get()),
                  "build the HUD font atlas for every UI language");
            // The toolbar can compress to a different height in each panel.
            // HUD drawing and hit testing use the remaining presentation area,
            // which can extend below the scene's aspect-ratio rectangle.
            const RECT overlay = FrameOverlayRect(app.get());
            Check(FrameOverlayScale(app.get()) == 1.0f,
                  "caption fixture has room for the normal HUD size");
            const RECT strip{overlay.right - 130, overlay.bottom - 165,
                             overlay.right - 10, overlay.bottom - 135};
            const float stripCenterX = float(overlay.right) - 70.0f;
            const float stripCenterY = float(overlay.bottom) - 150.0f;
            float previousCenterX = -1.0f;
            for (int state = 0; state < 3; ++state) {
                app->state.coordinateSystem = state;
                PrepareFrameTextOverlay(app.get());
                Check(app->TextOverlayPrimitiveCount() > 0,
                      "the coordinate caption emits HUD geometry");
                device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF808080, 1.0f, 0);
                device->BeginScene();
                device->SetTexture(0, app->TextOverlayTexture());
                device->SetStreamSource(0, app->OverlayVertices(), 0, 28);
                device->SetFVF(0x144);
                device->DrawPrimitive(D3DPT_TRIANGLELIST, 0,
                                      app->TextOverlayPrimitiveCount());
                device->EndScene();
                const Caption caption = MeasureCaption(device, read, strip);
                Check(caption.ink >= 12, "the caption draws visible pixels");
                Check(caption.width >= 24 && caption.height >= 12,
                      "the caption keeps its own size inside the selector");
                Check(caption.darkPixels >= 4,
                      "the caption keeps its black outline");
                Check(caption.lightPixels >= 4, "the caption face is white");
                if (!(std::fabs(caption.centerX - stripCenterX) <= 3.0f)) {
                    std::fprintf(stderr,
                                 "FAIL: language %u panel %d state %d caption "
                                 "centre x is %.1f, selector centre is %.1f\n",
                                 language, panel, state, caption.centerX,
                                 stripCenterX);
                    std::exit(1);
                }
                if (!(std::fabs(caption.centerY - stripCenterY) <= 4.0f)) {
                    std::fprintf(stderr,
                                 "FAIL: language %u panel %d state %d caption "
                                 "centre y is %.1f, selector centre is %.1f\n",
                                 language, panel, state, caption.centerY,
                                 stripCenterY);
                    std::exit(1);
                }
                if (previousCenterX >= 0.0f)
                    Check(std::fabs(caption.centerX - previousCenterX) <= 3.0f,
                          "the three captions share one centre");
                previousCenterX = caption.centerX;
            }
        }
    }
    app->state.optflag[0] = 0;
    app->state.coordinateSystem = 0;

    // ---- the view axis aligns with the screen ------------------------------
    // View mode must hand the drag handlers screen-aligned world directions:
    // with the camera yawed 90 degrees the screen-right axis is world +Z, and
    // the vertical axis stays world +Y.  A stale/transposed basis is what made
    // the "view" state unusable.
    app->CameraPitch() = 0.0f;
    app->CameraRoll() = 0.0f;
    app->CameraYaw() = 0.0f;
    float screenAxis[3]{};
    for (int index = 0; index < 3; ++index) {
        ViewWorldAxis(app.get(), index, screenAxis);
        const float expected[3][3] = {
            {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        for (int component = 0; component < 3; ++component)
            Check(Near(screenAxis[component], expected[index][component]),
                  "an unrotated camera keeps the world axes in view mode");
    }
    app->CameraYaw() = 1.5707964f;                       // 90 degrees
    ViewWorldAxis(app.get(), 0, screenAxis);
    Check(Near(screenAxis[0], 0.0f) && Near(screenAxis[1], 0.0f) &&
              Near(screenAxis[2], 1.0f),
          "the view X axis follows the camera yaw");
    ViewWorldAxis(app.get(), 1, screenAxis);
    Check(Near(screenAxis[1], 1.0f), "the view Y axis stays vertical");
    app->CameraYaw() = 0.0f;

    // ---- the "1fps or less" caption follows the UI language ---------------
    // Row 224 holds both original captions; DrawUiGlyph substitutes the
    // Chinese wording at the English anchor, so only Japanese may take the
    // second cell (the defect showed "1fps 未満" in the Chinese UI).
    app->state.fpsOverlayEnabled = 1;
    app->state.framesPerSecond = 0;
    app->state.playbackActive = 0;
    for (unsigned char language : languages) {
        app->EnglishUI() = language;
        Check(InitSceneFontTexture(app.get()),
              "rebuild the HUD atlas for the fps caption");
        PrepareFrameTextOverlay(app.get());
        // The fps caption is emitted first: two quads, outline pass included.
        const TextBatch batch = ReadTextBatch(app.get(), 12);
        const bool japanese = language == kUiJapanese;
        const float wantMinU = japanese ? 0.375f : 0.0f;
        const float wantMaxU = japanese ? 0.68554688f : 0.37304688f;
        if (!(Near(batch.minU, wantMinU) && Near(batch.maxU, wantMaxU))) {
            std::fprintf(stderr,
                         "FAIL: language %u fps caption samples u %.4f..%.4f, "
                         "expected %.4f..%.4f\n",
                         language, batch.minU, batch.maxU, wantMinU, wantMaxU);
            std::exit(1);
        }
    }
    app->state.fpsOverlayEnabled = 0;

    // The caption sits one button-row band above the top operation row,
    // measured from the HUD area after the toolbar has been laid out.
    PrepareFrameTextOverlay(app.get());
    const TextBatch lifted = ReadTextBatch(app.get(), -1);
    const float captionCentreY = (lifted.minY + lifted.maxY) * 0.5f;
    const float wantCentreY =
        static_cast<float>(FrameOverlayRect(app.get()).bottom) - 150.0f;
    if (!(std::fabs(captionCentreY - wantCentreY) <= 2.0f)) {
        std::fprintf(stderr,
                     "FAIL: caption centre y is %.1f, expected %.1f\n",
                     captionCentreY, wantCentreY);
        std::exit(1);
    }

    // ---- the caption outline stays one device pixel wide at any HUD scale ------
    // The ring must not change with the HUD scale. On a 2560-wide desktop the HUD
    // scale is 2.0, and the old 1.25 * scale offset produced a 2.5-pixel halo that
    // closed the counters of the 30-pixel glyphs - the reported "the caption
    // is drawn unclearly in every language". The HUD batch already carries the
    // geometry, so this check needs no second render.
    app->EnglishUI() = kUiEnglish;
    app->state.optflag[0] = 0;
    app->state.coordinateSystem = 1;                       // "Global"
    renderer->viewScale = 2.0f;
    Check(InitSceneFontTexture(app.get()),
          "rebuild the HUD atlas for the outline weight check");
    PrepareFrameTextOverlay(app.get());
    Check(app->TextOverlayPrimitiveCount() == 18,
          "the caption draws one white face over an eight-direction outline");
    const TextBatch outlined = ReadTextBatch(app.get(), -1);
    const float glyphHeight = kSceneFontRowHeight * FrameOverlayScale(app.get());
    const float outlineGrowth =
        (outlined.maxY - outlined.minY) - glyphHeight;
    if (!(outlineGrowth >= 0.0f && outlineGrowth <= 2.5f)) {
        std::fprintf(stderr,
                     "FAIL: the caption box is %.1f px tall at HUD scale 2.0, "
                     "expected %.1f plus a one-pixel outline on each side\n",
                     outlined.maxY - outlined.minY, glyphHeight);
        std::exit(1);
    }
    renderer->viewScale = 1.0f;
    app->state.coordinateSystem = 0;

    // ---- a UI language switch repaints the HUD atlas ----------------------
    // The captions are painted with the language's font and wording, so
    // LocalizeUI (menu 260) must rebuild the texture; otherwise the viewport
    // keeps the previous language's glyphs.  The camera/accessory panel avoids
    // the model-slot reload this harness does not provide.
    app->Hwnd() = window;
    app->state.optflag[0] = 1;
    app->EnglishUI() = kUiEnglish;
    LocalizeUI(app.get());
    SceneFontCell englishCell{};
    Check(SceneFontLabelCell("Global", renderer->viewScale, &englishCell),
          "the English atlas carries the coordinate caption");
    app->EnglishUI() = kUiChinese;
    LocalizeUI(app.get());
    SceneFontCell chineseCell{};
    Check(SceneFontLabelCell("Global", renderer->viewScale, &chineseCell),
          "the Chinese atlas carries the coordinate caption");
    const float englishWidth =
        (englishCell.u1 - englishCell.u0) * kSceneFontAtlasSize;
    const float chineseWidth =
        (chineseCell.u1 - chineseCell.u0) * kSceneFontAtlasSize;
    if (!(std::fabs(englishWidth - chineseWidth) >= 4.0f)) {
        std::fprintf(stderr,
                     "FAIL: atlas caption width stayed %.1f px across a "
                     "language switch (English %.1f, Chinese %.1f)\n",
                     englishWidth, englishWidth, chineseWidth);
        std::exit(1);
    }

    // ---- rotation follows the coordinate system ---------------------------
    // The model panel's rotate handles must rotate about the local, world or
    // screen axes; before this they always used the bone's local axes, so
    // selecting "view" changed nothing.
    {
        auto model = std::make_unique<mdl::ModelRecord>();
        static mdl::BoneRecord bones[1]{};
        static unsigned char selection[1] = {1};
        model->boneTable = bones;
        model->boneCount = 1;
        model->selectedBone = 0;
        model->boneSelection = selection;
        model->physicsMode = 1;                       // no PMX local axes
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get());
        app->SetSelectedModelSlot(0);
        bones[0].type = mdl::BoneType::RotateMove;
        bones[0].flags = mdl::kBoneFlagVisible;
        bones[0].tailBone = 0;
        // Rest world basis rotated 45 degrees about Y, parent basis identity:
        // the local X axis then differs from the world X axis.
        D3DMATRIX rest{};
        const float c = 0.70710678f;
        rest._11 = c; rest._13 = -c; rest._31 = c; rest._33 = c;
        rest._22 = 1.0f; rest._44 = 1.0f;
        D3DMATRIX parent{};
        parent._11 = parent._22 = parent._33 = parent._44 = 1.0f;
        std::memcpy(bones[0].matInit, &rest, sizeof(rest));
        std::memcpy(bones[0].matWorld, &parent, sizeof(parent));

        app->CameraPitch() = 0.0f;
        app->CameraRoll() = 0.0f;
        app->CameraYaw() = 1.5707964f;                // view X == world Z
        app->InteractionDragMode() = static_cast<ViewportDragMode>(4);         // rotate about handle X
        app->state.previousMouseY = 100;
        app->state.mouseY = 90;

        float quat[3][4]{};
        const int states[3] = {0, 1, 2};
        for (int i = 0; i < 3; ++i) {
            app->state.coordinateSystem = states[i];
            bones[0].rotQuat[0] = bones[0].rotQuat[1] = bones[0].rotQuat[2] =
                0.0f;
            bones[0].rotQuat[3] = 1.0f;
            BoneEditModes(app.get());
            std::memcpy(quat[i], bones[0].rotQuat, sizeof(quat[i]));
        }
        const auto differs = [](const float* a, const float* b) {
            for (int k = 0; k < 4; ++k)
                if (std::fabs(a[k] - b[k]) > 0.01f)
                    return true;
            return false;
        };
        Check(differs(quat[0], quat[1]),
              "local and global rotation about a handle differ");
        Check(differs(quat[1], quat[2]),
              "global and view rotation about a handle differ");
        // With the camera yawed 90 degrees the screen X axis is world +Z, so
        // view-X rotation must equal global-Z rotation.
        app->state.coordinateSystem = 1;
        bones[0].rotQuat[0] = bones[0].rotQuat[1] = bones[0].rotQuat[2] = 0.0f;
        bones[0].rotQuat[3] = 1.0f;
        app->InteractionDragMode() = static_cast<ViewportDragMode>(6);
        BoneEditModes(app.get());
        Check(!differs(bones[0].rotQuat, quat[2]),
              "view-X rotation matches global-Z rotation at 90 degrees yaw");
        app->ModelSlot(0) = nullptr;
        app->CameraYaw() = 0.0f;
        app->InteractionDragMode() = ViewportDragMode::None;
    }

    // ---- camera-panel switching restores the current camera-key view -----
    // Model editing renders the free view: the world stays at the origin and the
    // entire viewpoint is carried by the eye, which the frame builds as
    // (viewOffsetX, viewOffsetY, cameraDistance).  This matches the original's
    // translation for reference mode None (x64 0x14002797A).  The camera panel
    // renders the world translated by cameraPos with the same eye, so the two
    // panels rotate the world about different pivots; the switch carries the eye
    // by R(old pivot - new pivot) through CarryEyeForFocusChange instead of
    // overwriting the camera key's position as the earlier port did.  Only
    // camera playback overwrites the stop-time viewpoint with the track.
    {
        // The rendered image is world * view (D3D row-vector convention).
        auto composed = [&](D3DMATRIX* out) {
            SetupFrameWorldTransform(app.get());
            D3DMATRIX world{};
            D3DMATRIX view{};
            device->GetTransform(D3DTS_WORLD, &world);
            device->GetTransform(D3DTS_VIEW, &view);
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        sum += world.m[row][k] * view.m[k][column];
                    out->m[row][column] = sum;
                }
            }
        };
        auto sameMatrix = [](const D3DMATRIX& a, const D3DMATRIX& b) {
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    if (std::fabs(a.m[row][column] - b.m[row][column]) > 1.0e-3f)
                        return false;
            return true;
        };
        auto pivot = [&](float focus[3]) {
            ResolveFrameFocus(app.get(),
                              static_cast<int>(app->CameraReferenceMode()), focus);
        };
        auto switchPanel = [&](int panel) {
            float focusOld[3] = {0.0f, 0.0f, 0.0f};
            float focusNew[3] = {0.0f, 0.0f, 0.0f};
            pivot(focusOld);
            app->CameraMode() = panel;
            if (panel != 0) {
                ReloadModels(app.get());
                // Use the same camera-state application as the panel command;
                // hand-clearing the eye loses a parented camera's offset.
                ApplyCurrentCameraView(app.get());
            } else {
                pivot(focusNew);
                CarryEyeForFocusChange(app.get(), focusOld, focusNew);
            }
        };
        const float kCameraX = 12.0f;
        const float kCameraY = 34.0f;
        const float kCameraZ = -56.0f;

        app->CameraMode() = 0;
        app->state.followCameraEnabled = 0;
        app->state.playbackActive = 0;
        app->CameraReferenceMode() = CameraAttachmentReference::None;
        app->CameraParentModel() = -1;
        app->CameraYaw() = app->CameraPitch() = app->CameraRoll() = 0.0f;
        app->CameraPositionX() = kCameraX;
        app->CameraPositionY() = kCameraY;
        app->CameraPositionZ() = kCameraZ;
        app->ViewOffsetX() = -3.0f;
        app->ViewOffsetY() = 4.0f;
        app->CameraDistance() = -45.0f;
        D3DMATRIX freeWorld{};
        SetupFrameWorldTransform(app.get());
        device->GetTransform(D3DTS_WORLD, &freeWorld);
        Check(std::fabs(freeWorld.m[3][0]) < 1.0e-3f &&
                  std::fabs(freeWorld.m[3][1]) < 1.0e-3f &&
                  std::fabs(freeWorld.m[3][2]) < 1.0e-3f,
              "model editing pivots on the world origin, not on the view offsets");

        // The model / bone reference pivots the untracked model view (the view
        // buttons then show that model or bone from a fixed angle); tracking
        // overrides it. Keep deliberately displaced bones so each rule is seen.
        {
            auto referenceModel = std::make_unique<mdl::ModelRecord>();
            static mdl::BoneRecord referenceBones[2]{};
            referenceModel->boneTable = referenceBones;
            referenceModel->boneCount = 2;
            referenceModel->selectedBone = 1;
            referenceBones[0].position[0] = 17.0f;
            referenceBones[0].position[1] = 29.0f;
            referenceBones[0].position[2] = -11.0f;
            referenceBones[1].position[0] = -5.0f;
            referenceBones[1].position[1] = 41.0f;
            referenceBones[1].position[2] = 12.0f;
            D3DMATRIX identity{};
            identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
            for (auto& bone : referenceBones)
                std::memcpy(bone.matInit, &identity, sizeof(identity));
            app->ModelSlot(0) = reinterpret_cast<unsigned char*>(
                referenceModel.get());
            app->SetSelectedModelSlot(0);
            for (const auto& expected : {
                     std::pair{CameraAttachmentReference::ModelRoot,
                               std::array<float, 3>{17.0f, 29.0f, -11.0f}},
                     std::pair{CameraAttachmentReference::SelectedBone,
                               std::array<float, 3>{-5.0f, 41.0f, 12.0f}}}) {
                app->CameraReferenceMode() = expected.first;
                float focus[3]{};
                Check(ResolveFrameFocus(
                          app.get(), static_cast<int>(expected.first), focus) ==
                          FrameFocusSource::ModelReference &&
                          std::fabs(focus[0] - expected.second[0]) < 1.0e-3f &&
                          std::fabs(focus[1] - expected.second[1]) < 1.0e-3f &&
                          std::fabs(focus[2] - expected.second[2]) < 1.0e-3f,
                      "untracked model view pivots on its model / bone reference");
            }
            // Tracking covers the reference: the camera's parent (bone 0 here,
            // while the selected-bone reference names bone 1) wins.
            app->state.followCameraEnabled = 1;
            app->CameraParentModel() = 0;
            app->CameraParentBone() = 0;
            for (CameraAttachmentReference reference : {
                     CameraAttachmentReference::ModelRoot,
                     CameraAttachmentReference::SelectedBone}) {
                app->CameraReferenceMode() = reference;
                float focus[3]{};
                Check(ResolveFrameFocus(
                          app.get(), static_cast<int>(reference), focus) ==
                          FrameFocusSource::AttachmentBone &&
                          std::fabs(focus[0] - 17.0f) < 1.0e-3f &&
                          std::fabs(focus[1] - 29.0f) < 1.0e-3f &&
                          std::fabs(focus[2] + 11.0f) < 1.0e-3f,
                      "tracking overrides the model / bone reference");
            }
            // The camera panel follows the chosen parent bone even with the
            // follow switch off; bone 0 (master) must not stand in for it.
            app->state.followCameraEnabled = 0;
            app->CameraMode() = 1;
            app->CameraParentBone() = 1;
            {
                float focus[3]{};
                Check(ResolveFrameFocus(app.get(), 0, focus) ==
                          FrameFocusSource::AttachmentBone &&
                          std::fabs(focus[0] + 5.0f) < 1.0e-3f &&
                          std::fabs(focus[1] - 41.0f) < 1.0e-3f &&
                          std::fabs(focus[2] - 12.0f) < 1.0e-3f,
                      "parented camera view follows its chosen bone");
            }
            app->CameraMode() = 0;
            app->state.followCameraEnabled = 0;
            app->CameraParentModel() = -1;
            app->CameraParentBone() = 0;
            app->ModelSlot(0) = nullptr;
            app->CameraReferenceMode() = CameraAttachmentReference::None;
        }

        // A pan moves only the eye, so the image follows the drag
        // one to one.  Translating the world by the same offsets, as the port
        // previously did, doubles every pan.
        D3DMATRIX panBefore{};
        D3DMATRIX panAfter{};
        composed(&panBefore);
        app->ViewOffsetX() += 7.0f;
        composed(&panAfter);
        Check(std::fabs((panAfter.m[3][0] - panBefore.m[3][0]) + 7.0f) < 1.0e-3f,
              "a pan moves the rendered image by the drag delta, not twice it");
        app->ViewOffsetX() -= 7.0f;

        // Entering camera mode loads the camera key for the current frame and
        // drops the model editor's free-view offsets. Returning to model mode
        // carries that camera view across the pivot change.
        mdl::CameraKey cameraKey{};
        cameraKey.frame = 0;
        cameraKey.next = 0;
        cameraKey.distance = -63.0f;
        cameraKey.eye[0] = kCameraX;
        cameraKey.eye[1] = kCameraY;
        cameraKey.eye[2] = kCameraZ;
        cameraKey.fov = 45;
        cameraKey.parentModel = 0;
        cameraKey.parentBone = 0;
        auto cameraParent = std::make_unique<mdl::ModelRecord>();
        static mdl::BoneRecord cameraParentBones[1]{};
        cameraParent->boneTable = cameraParentBones;
        cameraParent->boneCount = 1;
        cameraParentBones[0].position[0] = 75.0f;
        cameraParentBones[0].position[1] = 24.0f;
        cameraParentBones[0].position[2] = -31.0f;
        D3DMATRIX cameraParentIdentity{};
        cameraParentIdentity._11 = cameraParentIdentity._22 =
            cameraParentIdentity._33 = cameraParentIdentity._44 = 1.0f;
        std::memcpy(cameraParentBones[0].matInit, &cameraParentIdentity,
                    sizeof(cameraParentIdentity));
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(cameraParent.get());
        app->SetSelectedModelSlot(0);
        auto* oldCameraKeys = app->CameraKeys();
        app->CameraKeys() = &cameraKey;
        app->state.currentFrame = 0;
        static const float kYaws[3] = {0.0f, 0.7f, 3.14159f};
        for (float yaw : kYaws) {
            app->CameraMode() = 0;
            app->CameraYaw() = yaw;
        cameraKey.target[1] = yaw;
            app->CameraPositionX() = kCameraX;
            app->CameraPositionY() = kCameraY;
            app->CameraPositionZ() = kCameraZ;
            app->ViewOffsetX() = -3.0f;
            app->ViewOffsetY() = 4.0f;
            app->CameraDistance() = -45.0f;
            D3DMATRIX modelSide{};
            D3DMATRIX cameraSide{};
            D3DMATRIX modelAgain{};
            composed(&modelSide);
            switchPanel(1);
            composed(&cameraSide);
            Check(!sameMatrix(modelSide, cameraSide),
                  "selecting the camera panel leaves the free model view");
            Check(app->CameraPositionX() == kCameraX &&
                      app->CameraPositionY() == kCameraY &&
                      app->CameraPositionZ() == kCameraZ &&
                      app->CameraDistance() == kCameraZ &&
                      app->ViewOffsetX() == kCameraX &&
                      app->ViewOffsetY() == kCameraY,
                  "selecting the parented camera panel restores its relative eye");
            switchPanel(0);
            composed(&modelAgain);
            Check(sameMatrix(cameraSide, modelAgain),
                  "returning to the model panel carries the camera view");
        }
        app->CameraKeys() = oldCameraKeys;
        app->CameraParentModel() = -1;  // the restored keys carry no parent
        app->CameraYaw() = 0.0f;

        // The camera panel renders from its own camera key, which is what lets the
        // viewport's pan, dolly and orbit gestures move the rendered image.
        switchPanel(1);
        D3DMATRIX cameraStopped{};
        SetupFrameWorldTransform(app.get());
        device->GetTransform(D3DTS_WORLD, &cameraStopped);
        Check(std::fabs(cameraStopped.m[3][0] + kCameraX) < 1.0e-3f &&
                  std::fabs(cameraStopped.m[3][1] + kCameraY) < 1.0e-3f &&
                  std::fabs(cameraStopped.m[3][2] + kCameraZ) < 1.0e-3f,
              "the camera panel renders its camera key");

        // Playback leaves the free view unchanged while the follow switch is off,
        // and forces the camera view while it is on: the two behaviours
        // requested by the field report.
        switchPanel(0);
        D3DMATRIX idleView{};
        D3DMATRIX playingView{};
        composed(&idleView);
        app->state.playbackActive = 1;
        composed(&playingView);
        Check(sameMatrix(idleView, playingView),
              "playback keeps the free view while the follow switch is off");
        app->state.followCameraEnabled = 1;
        D3DMATRIX trackedView{};
        composed(&trackedView);
        Check(!sameMatrix(idleView, trackedView),
              "the follow switch forces the camera view during playback");
        // Unchecking the follow switch resets the pivot from cameraPos to the
        // world origin, so the eye must follow it to avoid a jump in the view. CmdControl500
        // reads the checkbox, which the test window lacks, so this exercises
        // the unchecked branch; 535 is the follow switch (kFollowCameraCheckbox).
        app->state.playbackActive = 0;
        app->CameraMode() = 0;
        app->state.followCameraEnabled = 1;
        app->CameraPositionX() = kCameraX;
        app->CameraPositionY() = kCameraY;
        app->CameraPositionZ() = kCameraZ;
        app->ViewOffsetX() = -3.0f;
        app->ViewOffsetY() = 4.0f;
        app->CameraDistance() = -45.0f;
        D3DMATRIX trackedSide{};
        D3DMATRIX freeSide{};
        composed(&trackedSide);
        CmdControl500(app.get(), window, 535, 0);
        Check(app->state.followCameraEnabled == 0,
              "the follow switch can be switched off");
        composed(&freeSide);
        Check(sameMatrix(trackedSide, freeSide),
              "switching the follow switch off leaves the rendered image alone");

        // Enabling follow must recover the current camera, rather than carry
        // the displaced model/bone editing view over its higher-priority pivot.
        HWND follow = CreateWindowW(L"BUTTON", L"Follow", WS_CHILD|BS_AUTOCHECKBOX,
            0,0,20,20,window,reinterpret_cast<HMENU>(INT_PTR(535)),GetModuleHandleW(nullptr),nullptr);
        Check(follow != nullptr,"create hidden follow checkbox");
        MMDApp* trackingPreviousApp=g_Block;g_Block=app.get();
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        app->CameraKeys() = &cameraKey;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(cameraParent.get());
        for(int parent : {-1,0})for(auto reference : {CameraAttachmentReference::ModelRoot,CameraAttachmentReference::SelectedBone})for(bool menu : {false,true}){
            std::fprintf(stderr,"tracking parent=%d reference=%d menu=%d\n",parent,int(reference),menu);
            cameraKey.parentModel=parent;cameraKey.target[0]=0.3f;cameraKey.target[1]=0.7f;cameraKey.target[2]=-0.2f;
            app->CameraReferenceMode()=reference;app->CameraMode()=1;app->state.followCameraEnabled=1;
            ReloadModels(app.get());ApplyCurrentCameraView(app.get());
            D3DMATRIX expected{},restored{};composed(&expected);
            app->CameraMode()=0;app->state.followCameraEnabled=0;
            app->ViewOffsetX()=83;app->ViewOffsetY()=-21;app->CameraDistance()=-19;
            app->CameraYaw()=-1.3f;app->CameraPitch()=0.9f;
            SendMessageW(follow,BM_SETCHECK,BST_CHECKED,0);
            if(menu)CmdFileMenu(app.get(),window,247,0);else CmdControl500(app.get(),window,535,0);
            composed(&restored);
            Check(app->state.followCameraEnabled==1&&sameMatrix(expected,restored),"enabling tracking restores the current camera for model and bone references");
            for(int id : {402,403,404,405,406,407}){
                app->CameraMode()=1;ReloadModels(app.get());ApplyCurrentCameraView(app.get());CmdControl400(app.get(),window,id,0);composed(&expected);
                app->CameraMode()=0;ReloadModels(app.get());ApplyCurrentCameraView(app.get());CmdControl400(app.get(),window,id,0);composed(&restored);
                Check(sameMatrix(expected,restored),"tracked view presets match camera-mode presets");
            }
            app->ViewportInputActive()=1;
            for(int key : {2,4,6,8,5,1,0}){
                std::memset(app->state.numpadKeyState,0,sizeof app->state.numpadKeyState);app->state.numpadKeyState[key]=1;
                app->CameraMode()=1;ReloadModels(app.get());ApplyCurrentCameraView(app.get());ConsumeNumpadViewPresets(app.get());composed(&expected);
                app->CameraMode()=0;ReloadModels(app.get());ApplyCurrentCameraView(app.get());ConsumeNumpadViewPresets(app.get());composed(&restored);
                Check(sameMatrix(expected,restored),"tracked numpad presets match camera-mode presets");
            }
            std::memset(app->state.numpadKeyState,0,sizeof app->state.numpadKeyState);
        }
        DestroyWindow(follow);app->CameraKeys()=oldCameraKeys;app->ModelSlot(0)=nullptr;
        g_Block=trackingPreviousApp;
        app->CameraParentModel()=-1;app->CameraReferenceMode()=CameraAttachmentReference::None;

        app->state.followCameraEnabled = 0;
        app->state.playbackActive = 0;
        app->CameraMode() = 0;
        app->CameraPositionX() = 0.0f;
        app->CameraPositionY() = 0.0f;
        app->CameraPositionZ() = 0.0f;
        app->ViewOffsetX() = 0.0f;
        app->ViewOffsetY() = 0.0f;
        app->CameraDistance() = -45.0f;
    }

    // ---- the accessory state swaps the camera plate for the ".x" mark -----
    // While the camera/light/accessory panel edits the camera it samples the
    // camera rows of the HUD sheet (rotate v 0.412109..0.46875, move
    // 0.177734..0.234375); the accessory state takes the rows carrying the
    // ".x" mark (y 341..370 and 371..400).  This runs after the input blocks
    // above, which measure the tool state a press leaves behind.  The row
    // bands are matched per quad now that the scale row shares the batch.
    Check(SUCCEEDED(device->CreateVertexBuffer(0x445C0, D3DUSAGE_WRITEONLY,
                                               0x144, D3DPOOL_MANAGED,
                                               &app->SpriteOverlayVertices(),
                                               nullptr)),
          "allocate the HUD sprite batch");
    app->state.optflag[0] = 1;
    app->state.playbackActive = 0;
    app->state.coordinateSystem = 0;
    app->ViewportToolOperation() = ViewportToolAction::None;
    PrepareFrameSpriteOverlay(app.get());
    Check(ReadSpriteBand(app.get(), 0.41210899f, 0.46875f).quads >= 1,
          "the camera state samples the camera-plate rotate row");
    app->state.coordinateSystem = 2;
    PrepareFrameSpriteOverlay(app.get());
    Check(ReadSpriteBand(app.get(), 0.666015625f, 0.72265625f).quads >= 1,
          "the accessory state samples the .x rotate row");
    Check(ReadSpriteBand(app.get(), 0.41210899f, 0.46875f).quads == 0,
          "the accessory state stops sampling the camera-plate rows");
    app->state.coordinateSystem = 0;

    // ---- the scale row: four buttons, disabled states, every HUD scale ----
    // Third HUD row: uniform/X/Y/Z at right-170..-130 / -130..-90 /
    // -90..-50 / -50..-10 on the bottom-40..-10 band.  Every bone scales, so
    // the row is inert exactly while the camera/accessory/light state, a
    // missing bone selection or playback disables bone scaling, and a
    // disabled button keeps its UVs under the black tint instead of dragging.
    {
        auto scaleModel = std::make_unique<mdl::ModelRecord>();
        static mdl::BoneRecord scaleBones[1]{};
        static unsigned char scaleSelection[1] = {1};
        scaleModel->boneTable = scaleBones;
        scaleModel->boneCount = 1;
        scaleModel->selectedBone = 0;
        scaleModel->boneSelection = scaleSelection;
        scaleModel->physicsMode = 1;               // no PMX local axes
        scaleBones[0].type = mdl::BoneType::RotateMove;
        scaleBones[0].flags = mdl::kBoneFlagVisible;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(scaleModel.get());
        app->SetSelectedModelSlot(0);
        app->Hwnd() = nullptr;     // BeginCenteredDrag must not move the pointer
        app->state.optflag[0] = 0;
        app->state.v32c = 0;
        app->state.playbackActive = 0;
        app->state.coordinateSystem = 0;
        app->EditMode() = ViewportEditMode::Bone;
        app->ViewportToolCenterX() = 0;
        app->ViewportToolCenterY() = 0;
        const RECT wideView{0, 0, 640, 480};   // every HUD scale fits on screen
        app->ViewportRect() = wideView;
        static constexpr float kHudScales[3] = {1.0f, 1.5f, 2.0f};
        static constexpr int kScaleCentreX[4] = {150, 110, 70, 30};
        // The four buttons answer at any HUD scale, one drag mode each.
        for (float sceneScale : kHudScales) {
            renderer->viewScale = sceneScale;
            const float hudScale = FrameOverlayScale(app.get());
            Check(hudScale == 1.0f, "HUD holds its normal size across scene scales");
            for (int column = 0; column < 4; ++column) {
                app->MouseX() = app->PreviousMouseX() =
                    wideView.right -
                    static_cast<int>(kScaleCentreX[column] * hudScale);
                app->MouseY() = app->PreviousMouseY() =
                    wideView.bottom - static_cast<int>(25.0f * hudScale);
                Check(HandleViewportToolPress(app.get()),
                      "the scale row answers a press at any HUD scale");
                Check(app->ViewToolDragOperation() ==
                          static_cast<ViewportToolAction>(22 + column),
                      "the pressed scale button owns the centred drag");
                Check(app->InteractionDragMode() ==
                          static_cast<ViewportDragMode>(19 + column),
                      "the scale buttons map to the scale drag modes");
                Check(app->state.a06B5 == 1,
                      "the scale press flags the bone-edit refresh byte");
                ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
                Check(app->ViewToolDragOperation() == ViewportToolAction::None,
                      "the mouse-up ends the scale drag");
                Check(app->state.a06B5 == 0,
                      "the mouse-up clears the bone-edit refresh byte");
            }
            // The seam between the uniform and X hit bands answers nothing.
            app->MouseX() = app->PreviousMouseX() =
                wideView.right - static_cast<int>(130.0f * hudScale);
            app->MouseY() = app->PreviousMouseY() =
                wideView.bottom - static_cast<int>(25.0f * hudScale);
            Check(!HandleViewportToolPress(app.get()),
                  "the seam between the scale buttons stays inert");
        }
        Check(scaleModel->undoDirty == 1,
              "a scale press captures the bone-edit undo first");
        renderer->viewScale = 1.0f;

        // ViewportEditMode::Light and ::Camera inside the model panel are the
        // bone move (radio 492) and bone rotate (radio 493) cursors, not the
        // light and camera editors, so scaling stays enabled under both.
        const auto pressUniformEnabled = [&](const char* what) {
            app->MouseX() = app->PreviousMouseX() = wideView.right - 150;
            app->MouseY() = app->PreviousMouseY() = wideView.bottom - 25;
            Check(HandleViewportToolPress(app.get()), what);
            Check(app->ViewToolDragOperation() ==
                      ViewportToolAction::ScaleUniform,
                  "the model-panel bone cursors keep the scale drag");
            ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
        };
        app->EditMode() = ViewportEditMode::Light;
        pressUniformEnabled(
            "the model-panel bone move cursor still scales");
        app->EditMode() = ViewportEditMode::Camera;
        pressUniformEnabled(
            "the model-panel bone rotate cursor still scales");
        app->EditMode() = ViewportEditMode::Bone;

        // Disabled states: the real camera/light/accessory panel
        // (optflag[0] != 0), a missing bone selection and playback never
        // start a drag.
        const auto pressUniform = [&](const char* what) {
            app->MouseX() = app->PreviousMouseX() = wideView.right - 150;
            app->MouseY() = app->PreviousMouseY() = wideView.bottom - 25;
            Check(!HandleViewportToolPress(app.get()), what);
            Check(app->ViewToolDragOperation() == ViewportToolAction::None &&
                      app->InteractionDragMode() == ViewportDragMode::None,
                  "a disabled scale row owns no drag");
        };
        app->state.optflag[0] = 1;
        pressUniform("the camera panel leaves the scale row inert");
        app->state.coordinateSystem = 2;
        pressUniform("the accessory state leaves the scale row inert");
        app->state.coordinateSystem = 0;
        app->EditMode() = ViewportEditMode::Light;
        pressUniform("the light editor leaves the scale row inert");
        app->EditMode() = ViewportEditMode::Bone;
        app->state.optflag[0] = 0;
        scaleSelection[0] = 0;
        scaleModel->selectedBone = -1;
        pressUniform("a missing bone selection leaves the scale row inert");
        scaleSelection[0] = 1;
        scaleModel->selectedBone = 0;
        app->state.playbackActive = 1;
        pressUniform("playback leaves the scale row inert");
        app->state.playbackActive = 0;

        // A valid selectedBone with an unsynced selection array must still get
        // its undo record: the scale branch syncs boneSelection[selectedBone]
        // before PushBoneEditUndo, whose snapshot needs a selected entry.
        scaleSelection[0] = 0;
        scaleModel->selectedBone = 0;
        scaleModel->undoDirty = 0;
        app->MouseX() = app->PreviousMouseX() = wideView.right - 150;
        app->MouseY() = app->PreviousMouseY() = wideView.bottom - 25;
        Check(HandleViewportToolPress(app.get()),
              "a selected bone answers the scale press with a stale selection");
        Check(scaleSelection[0] == 1,
              "the scale press syncs the selected bone into the selection");
        Check(scaleModel->undoDirty == 1,
              "the synced selection keeps the undo capture alive");
        ReleaseLeftButtonState(app.get(), /*physicallyHeld=*/false);
        scaleSelection[0] = 1;

        // The drawing shares the input's gate: four normal cells while bone
        // scaling is on, the same four UVs under the 0xA0000000 tint (never a
        // plate) while it is off.
        const float kScaleV0 = 0.783203125f;       // y 401..430 of the sheet
        const float kScaleV1 = 0.83984375f;
        for (float sceneScale : kHudScales) {
            renderer->viewScale = sceneScale;
            const float hudScale = FrameOverlayScale(app.get());
            Check(hudScale == 1.0f, "HUD holds its normal size across scene scales");
            PrepareFrameSpriteOverlay(app.get());
            const SpriteBand row = ReadSpriteBand(app.get(), kScaleV0, kScaleV1);
            Check(row.quads == 4 && row.washQuads == 0,
                  "the enabled scale row draws four untinted cells");
            Check(Close(row.minU, 0.001953125f) &&
                      Close(row.maxU, 0.314453125f),
                  "the scale row samples the four x 1/41/81/121 cells");
            Check(Close(row.minX, wideView.right - 170.0f * hudScale) &&
                      Close(row.maxX, wideView.right - 10.0f * hudScale) &&
                      Close(row.minY, wideView.bottom - 40.0f * hudScale) &&
                      Close(row.maxY, wideView.bottom - 10.0f * hudScale),
                  "the scale row lands on its screen band at any HUD scale");

            // Hover highlight: the ScaleY cell from the y 431..460 band, on
            // the same geometry as the button it marks.
            app->ViewportToolOperation() = ViewportToolAction::ScaleY;
            PrepareFrameSpriteOverlay(app.get());
            const SpriteBand glow =
                ReadSpriteBand(app.get(), 0.841796875f, 0.8984375f);
            Check(glow.quads == 1 && glow.washQuads == 0,
                  "one highlight cell marks the hovered scale button");
            Check(Close(glow.minU, 0.158203125f) &&
                      Close(glow.maxU, 0.236328125f),
                  "the Y highlight samples its own atlas cell");
            Check(Close(glow.minX, wideView.right - 90.0f * hudScale) &&
                      Close(glow.maxX, wideView.right - 50.0f * hudScale) &&
                      Close(glow.minY, wideView.bottom - 40.0f * hudScale) &&
                      Close(glow.maxY, wideView.bottom - 10.0f * hudScale),
                  "the hover highlight tracks the button at any HUD scale");
            app->ViewportToolOperation() = ViewportToolAction::None;
        }

        // Disabled wash: same UVs, tinted corners, no opaque plate.  A stale
        // hover operation left over from before the switch must not brighten
        // the washed row - the highlight carries the same Enabled gate.
        scaleSelection[0] = 0;
        scaleModel->selectedBone = -1;
        renderer->viewScale = 1.0f;
        app->ViewportToolOperation() = ViewportToolAction::ScaleY;
        PrepareFrameSpriteOverlay(app.get());
        app->ViewportToolOperation() = ViewportToolAction::None;
        const SpriteBand washed = ReadSpriteBand(app.get(), kScaleV0, kScaleV1);
        Check(washed.quads == 4 && washed.washQuads == 4,
              "a disabled scale row washes its four cells");
        Check(Close(washed.washMinU, washed.minU) &&
                  Close(washed.washMaxU, washed.maxU),
              "the disabled wash reuses the cells' own UVs");
        Check(ReadSpriteBand(app.get(), 0.841796875f, 0.8984375f).quads == 0,
              "a stale hover never brightens a disabled scale row");

        // The caption follows the rows up 40 and keeps the band at every
        // HUD scale.
        app->EnglishUI() = kUiEnglish;
        app->state.optflag[0] = 0;
        app->state.coordinateSystem = 0;
        for (float sceneScale : kHudScales) {
            renderer->viewScale = sceneScale;
            const float hudScale = FrameOverlayScale(app.get());
            Check(hudScale == 1.0f, "HUD holds its normal size across scene scales");
            Check(InitSceneFontTexture(app.get()),
                  "rebuild the HUD atlas for the caption position check");
            PrepareFrameTextOverlay(app.get());
            const TextBatch caption = ReadTextBatch(app.get(), -1);
            const float centreX = (caption.minX + caption.maxX) * 0.5f;
            const float centreY = (caption.minY + caption.maxY) * 0.5f;
            Check(std::fabs(centreX - (wideView.right - 70.0f * hudScale)) <=
                      3.0f,
                  "the caption keeps its horizontal centre at any HUD scale");
            Check(std::fabs(centreY - (wideView.bottom - 150.0f * hudScale)) <=
                      3.0f,
                  "the caption keeps its new band at any HUD scale");
        }

        renderer->viewScale = 1.0f;
        app->ViewportRect() = RECT{0, 0, kWidth, kHeight};
        app->ModelSlot(0) = nullptr;
        app->state.playbackActive = 0;
        app->state.optflag[0] = 0;
        app->state.coordinateSystem = 0;
    }

    // ---- the disabled wash renders semi-transparent and texture-clipped ----
    // DrawSpriteOverlay pins COLOR/ALPHA to TEXTURE*DIFFUSE and restores the
    // six stage values, so starting from D3D9's default alpha SELECTARG1 - the
    // state that would otherwise drop the 0xA0 diffuse alpha and paint the
    // cell opaque black - the wash must still come out semi-transparent.  A
    // synthetic sheet opaque only in the uniform cell makes the verdict
    // readable: over the white button the wash darkens to (1 - 0xA0/255) of
    // white (about 95 per channel, not 0), while the transparent X cell keeps
    // the background, so the wash follows the texture's own alpha and paints
    // no plate.
    {
        IDirect3DTexture9* sheet = nullptr;
        Check(SUCCEEDED(device->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &sheet,
                                              nullptr)),
              "allocate the synthetic sprite sheet");
        D3DLOCKED_RECT locked{};
        Check(SUCCEEDED(sheet->LockRect(0, &locked, nullptr, 0)),
              "lock the synthetic sprite sheet");
        for (int y = 0; y < 512; ++y) {
            auto* row = reinterpret_cast<DWORD*>(
                static_cast<char*>(locked.pBits) + y * locked.Pitch);
            for (int x = 0; x < 512; ++x)
                row[x] = 0;                 // transparent outside the cell
        }
        for (int y = 401; y <= 430; ++y) {  // uniform cell, x 1..41
            auto* row = reinterpret_cast<DWORD*>(
                static_cast<char*>(locked.pBits) + y * locked.Pitch);
            for (int x = 1; x <= 41; ++x)
                row[x] = 0xFFFFFFFFu;
        }
        sheet->UnlockRect(0);
        app->OverlayTexture() = sheet;

        app->state.optflag[0] = 1;          // camera panel: scaling off
        app->state.playbackActive = 0;
        app->state.coordinateSystem = 0;
        app->ViewportToolOperation() = ViewportToolAction::None;
        app->EditMode() = ViewportEditMode::Bone;
        PrepareFrameSpriteOverlay(app.get());

        // Start the draw from the D3D9 default texture-stage state, exactly
        // the state the frame is in before DrawSpriteOverlay pins it.
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);

        device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF406080, 1.0f, 0);
        device->BeginScene();
        app->TextOverlayPrimitiveCount() = 0;
        app->LineOverlayPrimitiveCount() = 0;
        DrawFrameOverlayBatches(app.get());
        device->EndScene();

        const auto channel = [](DWORD pixel, int shift) {
            return static_cast<int>((pixel >> shift) & 0xFF);
        };
        // Uniform cell centre: the white button under the
        // wash ends at 255 * (255 - 160) / 255 = 95 per channel.
        const DWORD washed = ReadPixel(device, read, kWidth - 150, kHeight - 25);
        for (int c = 0; c < 3; ++c) {
            const int got = channel(washed, 16 - 8 * c);
            if (std::abs(got - 95) > 8) {
                std::fprintf(stderr,
                             "FAIL: the disabled wash channel %d is %d, "
                             "expected ~95 (semi-transparent, neither the "
                             "unwashed button nor opaque black)\n",
                             c, got);
                std::exit(1);
            }
        }
        // X cell centre: transparent texels must leave the
        // background untouched.
        const DWORD clipped = ReadPixel(device, read, kWidth - 110, kHeight - 25);
        for (int c = 0; c < 3; ++c)
            Check(std::abs(channel(clipped, 16 - 8 * c) -
                           channel(0xFF406080u, 16 - 8 * c)) <= 2,
                  "the disabled wash follows the texture alpha, not a plate");

        // The draw left the caller's stage state as it found it.
        DWORD alphaOp = 0;
        device->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOp);
        Check(alphaOp == D3DTOP_SELECTARG1,
              "the sprite draw restores the texture stage state");

        app->OverlayTexture() = nullptr;
        sheet->Release();
    }

    read->Release();
    app->OverlayVertices()->Release();
    if (app->SpriteOverlayVertices() != nullptr)
        app->SpriteOverlayVertices()->Release();
    if (axis->materials != nullptr)
        ::operator delete(axis->materials);
    if (axis->mesh != nullptr)
        reinterpret_cast<IUnknown*>(axis->mesh)->Release();
    if (app->TextOverlayTexture() != nullptr)
        app->TextOverlayTexture()->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    std::puts("PASS axis.x colours, caption centring and outline");
    return 0;
}
