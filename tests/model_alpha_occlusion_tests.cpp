// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Hidden D3D9 readback for solid coverage and two half-transparent layers,
// across both model orders and material orders, using fixed and embedded effects.
// The cross-model pair also covers a rear layer whose material exceeds the
// 4096-triangle reorder cap, kept large with offscreen filler triangles that
// contribute no pixel.
//
// --intersections adds a sandwich scene: an opaque surface between two
// transparent ones, where the far transparent fragment has to be rejected by
// the opaque surface's depth before it can blend in, in both submission orders
// and both model ABIs. The mode explicitly selects Maximum.
//
// Scenes that need peeling select Maximum explicitly. Ordinary occlusion uses
// Standard; --precision low|standard|high|maximum selects the same assertions
// at another sorting tier. The product's default remains Off.
// --sort-off checks native material/model pass order and On -> Off index restore,
// using real draw/readback on PMD and PMX with fixed, SM2 and SM3 rendering.
//
// --low-alpha runs only the low-alpha accumulation scenes, on the
// shader-model-3 resource (118): one PMX material holding a 256-layer stack of
// 1/255-alpha texels and one holding a 512-layer stack of 2/255-alpha texels,
// each checked against 255 * pow(1 - alpha, layers) computed in double.
//
// --maximum-limit runs only the top-precision accumulation scenes, on the same
// resource (118), reusing the low-alpha fixture: one PMX material holding a
// 1152-layer stack of 1/255-alpha black texels and one holding a 4608-layer
// stack of the same texels, both selected at TransparentSortPrecision::Maximum.
// 1152 and 4608 exceed the historical fixed budgets of 1024 and 4096 layers,
// so only a build that peels the whole stack at the top tier reaches the
// reference pixel. The assertions stay on the composite pixel; how many layers
// the peel ran is never read back. This mode alone also creates its device with
// hardware vertex processing, matching the production cloud benchmark; the
// other modes keep software.
//
// --maximum-limit renders into a 16x16 target by default: the deep stacks cost
// one full pass per layer, and the nine interior samples are what the
// regression needs. The optional --maximum-full-size selects the same mode and
// the same scenes on the original 96x96 stress target instead.
//
// --peel-budget retains the historical deep-stack fixture name. High now
// orders complete triangles directly; its nearest green layer must survive.
//
// --msaa combines with any of the scene switches: the same scenes render into a
// 4x multisampled back buffer which is resolved into a same-size single-sample
// surface before the readback. When 4x is unavailable the run exits non-zero
// instead of reporting a single-sampled pass.
//
// --sphere-only --capture-dir <absolute directory> is the native PMX
// sphereMode=1 pixel regression: one two-material model whose empty base
// texture path makes the sphere texel's colour and alpha the whole surface.
// It renders both authoring orders at Off -> Low -> Standard -> High ->
// Maximum -> Off on the fixed, SM2 and SM3 resources, checks two 5x5 interior
// regions per frame against a double-precision source-over reference, and
// saves every frame as BGRA and BMP with a JSONL record. The capture directory
// must be new or empty; a write failure fails the run.
//
// --sphere-mode 1|2|3 selects the native PMX sphereMode the --sphere-only
// fixture drives; it is valid only together with --sphere-only and defaults to
// 1, which keeps the original matrix and its 60 frames. Mode 2 uses the
// additive sphere cascade, so the fixture zeroes the base colour of the
// materials that really sample a sphere while keeping their alpha at the
// sphere texel's. Mode 3 samples the sphere texture through TEXCOORD1, so the
// fixture builds real 48-byte PMX vertices (FVF 524818, pmxAdditionalUvCount 1)
// and adds a 2x1 UV-source probe of three frames per resource. Modes 1 and 2
// keep the original 60-frame matrix; mode 3 renders 69 frames.
//
// --cross-object-only --capture-dir <absolute directory> is the independent
// cross-object transparent-sort capture. It builds two genuinely separate
// models, one inset quad each: the near model holds the half-transparent front
// texel (ARGB 0x8080A040) at z 0.25 and the far model the half-transparent rear
// texel (ARGB 0x804060C0) at z 0.75. On the fixed, SM2 and SM3 resources it
// renders both author orders through TransparentSortCrossObjects false and true
// at every precision tier Off -> Low -> Standard -> High -> Maximum -> Off, then
// a Standard false -> true -> false scope toggle. Every frame is checked against
// an independent source-over reference computed from the known texels, and the
// full BGRA and BMP plus a JSONL record are written to the capture directory.
// The mode requires that directory and rejects every other scene switch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <shellapi.h>  // CommandLineToArgvW (excluded by WIN32_LEAN_AND_MEAN)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/transparent_geometry.hpp"
#include "mikumikustudio/transparent_sort_cache.hpp"
#endif
#include "mikumikustudio/ported_funcs.hpp"

using namespace mikumikustudio;

namespace {

constexpr int kWidth = 96;
constexpr int kHeight = 96;
constexpr DWORD kModelFvf = 274;  // XYZ | NORMAL | TEX1, the body vertex format
constexpr UINT kModelStride = 32;
// --sphere-mode 3 samples the sphere texture through TEXCOORD1, which needs the
// real PMX additional-UV vertex ModelVertexFormat selects for
// pmxAdditionalUvCount 1: XYZ | NORMAL | TEX2 with a float4 TEXCOORD1.
constexpr DWORD kModelFvfUv1 = 524818;
constexpr UINT kModelStrideUv1 = 48;
constexpr float kNearDepth = 0.25f;
constexpr float kFarDepth = 0.75f;
// The clear colour the half-transparent texel must not blend with; a grey or
// white pixel proves it did.
constexpr DWORD kBackground = 0xFFFFFFFF;
constexpr DWORD kBlue = 0xFF0000FFu;

// Front texture: a transparent texel, a half-transparent one and a solid one.
// They are the three cases a transparent texture edge is made of.
constexpr DWORD kNearTexels[3] = {0x00000000u, 0x80000000u, 0xFF000000u};
// Rear texture: the opaque blue texel the front surface must blend with, plus a
// transparent texel elsewhere in the same texture. That second texel is what
// classifies the texture as using alpha, so the opaque blue surface is
// submitted in the translucent pass as well.
constexpr DWORD kFarTexels[2] = {kBlue, 0x00000000u};

// Two coloured layers distinguish correct compositing from a missing rear layer.
constexpr DWORD kTwoLayerRearTexel = 0x804060C0u;    // a=0x80 r=0x40 g=0x60 b=0xC0
constexpr DWORD kTwoLayerFrontTexel = 0x8080A040u;   // a=0x80 r=0x80 g=0xA0 b=0x40
// u=.25 samples texel 0; u=.5 would sample the fully transparent texel.
constexpr DWORD kTwoLayerRearTexels[2] = {kTwoLayerRearTexel, 0x00000000u};
constexpr DWORD kTwoLayerFrontTexels[2] = {kTwoLayerFrontTexel, 0x00000000u};
constexpr float kTwoLayerSamplerU = 0.25f;
// Source-over of the rear layer painted first and the front layer over it,
// evaluated in 8-bit channels with alpha = 128/255 at each step, starting from
// the cleared white background:
//   after rear:  R = 0x40*a + 255*(1-a) = 159.13, G = 175.19, B = 223.38
//   after front: R = 0x80*a + 159.13*(1-a) = 143.5, G = 167.6, B = 143.4
// The destination blend is INVSRCALPHA and the source is SRCALPHA, so the two
// steps above are exactly what the hardware evaluates. The tolerance below
// absorbs the one-unit rounding difference between the reference arithmetic and
// the fixed-point blend the device performs.
constexpr int kTwoLayerExpectedRed = 144;
constexpr int kTwoLayerExpectedGreen = 168;
constexpr int kTwoLayerExpectedBlue = 143;
// PMX material-alpha scene: the front material's Diffuse.a is 0.5 and its
// texture is opaque (0xFF20C0E0), so the front layer blends at alpha 0.5; the
// rear layer is the ordinary half-transparent texel. Starting from white:
//   after rear:  R = 0x40*0x80/255 + 255*(1-0x80/255)     = 159.13, 175.19, 223.38
//   after front: R = 0x20*0.5 + 159.13*0.5                =  95.56, 183.59, 223.69
constexpr int kMaterialAlphaExpectedRed = 96;
constexpr int kMaterialAlphaExpectedGreen = 184;
constexpr int kMaterialAlphaExpectedBlue = 224;

const wchar_t kNearPath[] = L"alpha_occlusion_near.bmp";
const wchar_t kFarPath[] = L"alpha_occlusion_far.bmp";
const wchar_t kTwoLayerFrontPath[] = L"alpha_occlusion_two_layer_front.bmp";
const wchar_t kTwoLayerRearPath[] = L"alpha_occlusion_two_layer_rear.bmp";
const wchar_t kPmxAlphaPath[] = L"alpha_occlusion_pmx_alpha.bmp";

// Sample grid shared by the full-viewport fixtures, kept inside the quad and
// away from its edges.
constexpr int kSampleFirst = 32;
constexpr int kSampleLast = 64;
constexpr int kSampleStep = 16;

// Only Maximum peels after the lower three strengths were reduced again.
constexpr TransparentSortPrecision kPeelTier = TransparentSortPrecision::Maximum;

// --low-alpha fixture. One PMX material holds a stack of parallel black quads
// at distinct depths, so a sampled pixel is the source-over accumulation of
// every layer behind it. The texel alpha is 1/255 in the 256-layer case and
// 2/255 in the 512-layer case: both are below the reference the opaque coverage
// pre-pass uses, so losing them - to the alpha test, to a quantisation to zero,
// or to a dropped layer - leaves the white clear colour, or a far brighter
// pixel, in the frame.
const wchar_t kLowAlphaPath1[] = L"alpha_occlusion_low_alpha_1.bmp";
const wchar_t kLowAlphaPath2[] = L"alpha_occlusion_low_alpha_2.bmp";
constexpr int kLowAlphaTexelAlpha1 = 1;  // out of 255
constexpr int kLowAlphaTexelAlpha2 = 2;  // out of 255
constexpr int kLowAlphaLayers1 = 256;
constexpr int kLowAlphaLayers2 = 512;
// Every layer is a full-viewport quad, and the depth is the only thing that
// separates the layers. They are appended far to near, which is also the order
// a single material draws its own index range in, so the authored order is the
// blend order and the accumulation is the one the reference describes.
constexpr float kLowAlphaFarDepth = 0.9f;
constexpr float kLowAlphaNearDepth = 0.1f;
// The reference is the double-precision 255 * pow(1 - alpha, layers); a sample
// must land within this many units of it.
constexpr double kLowAlphaTolerance = 2.0;
// First free resourcePool slot: entries 0..4 belong to the other fixtures, and
// the pool scan stops at the first empty slot, so these must follow directly.
constexpr UINT kLowAlphaPoolBase = 5;

struct LowAlphaCase {
    const wchar_t* texturePath;
    int texelAlpha;  // out of 255
    int layers;
};

const LowAlphaCase kLowAlphaCases[] = {
    {kLowAlphaPath1, kLowAlphaTexelAlpha1, kLowAlphaLayers1},
    {kLowAlphaPath2, kLowAlphaTexelAlpha2, kLowAlphaLayers2},
};

// --maximum-limit fixture: the same black-layer stack as --low-alpha, rendered
// through the same shader-model-3 resource (118), with the top tier selected.
// The two depths exceed the historical fixed budgets, so the mode only
// reports the reference pixel on a build that peels the whole stack there. The
// texel alpha stays at 1/255, the value the low-alpha scenes already prove
// survives classification and blending.
const wchar_t kMaximumLimitPath1[] = L"alpha_occlusion_maximum_limit_1.bmp";
const wchar_t kMaximumLimitPath2[] = L"alpha_occlusion_maximum_limit_2.bmp";
constexpr int kMaximumLimitTexelAlpha = 1;  // out of 255
constexpr int kMaximumLimitLayers1 = 1152;
constexpr int kMaximumLimitLayers2 = 4608;
static_assert(kMaximumLimitLayers1 > 1024,
              "the first maximum-limit stack must be deeper than the 1024-layer "
              "historical peel budget");
static_assert(kMaximumLimitLayers2 > 4096,
              "the second maximum-limit stack must be deeper than the "
              "4096-layer peel budget");
// Every mode is a separate run that narrows to resource 118, so the
// maximum-limit cases take the same first free slot after entries 0..4 that the
// low-alpha and peel-budget cases take.
constexpr UINT kMaximumLimitPoolBase = kLowAlphaPoolBase;

const LowAlphaCase kMaximumLimitCases[] = {
    {kMaximumLimitPath1, kMaximumLimitTexelAlpha, kMaximumLimitLayers1},
    {kMaximumLimitPath2, kMaximumLimitTexelAlpha, kMaximumLimitLayers2},
};

// --peel-budget fixture: 1152 parallel quads, black behind one green quad.
// The final pixel must include the nearest green quad, at the same alpha.
const wchar_t kPeelBudgetPath[] = L"alpha_occlusion_peel_budget.bmp";
// The 2-wide texture the two quad colours are sampled from: u=.25 returns texel
// 0, u=.75 returns texel 1.
constexpr DWORD kPeelBudgetTexels[2] = {0x80000000u, 0x8000FF00u};
constexpr float kPeelBudgetBlackU = 0.25f;
constexpr float kPeelBudgetGreenU = 0.75f;
// Green at alpha 0.5 over the black the far layers leave: 0.5 * 255 = 127.5.
constexpr int kPeelBudgetExpectedRed = 0;
constexpr int kPeelBudgetExpectedGreen = 128;
constexpr int kPeelBudgetExpectedBlue = 0;
constexpr int kPeelBudgetTolerance = 2;
// Keep the historical stack size for before/after comparisons.
constexpr int kPeelBudgetLayers = 1152;
constexpr float kPeelBudgetFarDepth = 0.9f;
constexpr float kPeelBudgetNearDepth = 0.1f;
// This fixture used to exceed the former High tier's 1024-layer peel budget.
static_assert(kPeelBudgetLayers > 1024,
              "the peel-budget stack must exceed the 1024-layer peel budget");
// The low-alpha and peel-budget modes never run in the same process, so both
// take the first free pool slot after entries 0..4.
constexpr UINT kPeelBudgetPoolSlot = 5;

// --intersections sandwich fixture. Three parallel full-viewport layers, so
// every sample point has the same far / opaque / near order and no
// camera-dependent logic is needed:
//   near   kTwoLayerFrontPath at z 0.25, texel 0x8080A040 (alpha 0x80)
//   opaque kPmxAlphaPath     at z 0.50, texel 0xFF20C0E0 (alpha 0xFF)
//   far    kTwoLayerRearPath at z 0.75, texel 0x804060C0 (alpha 0x80)
// The opaque surface is the middle layer, so the far transparent fragment is
// behind it at every sampled pixel and must be rejected before it can blend its
// colour in. The pixel is then only the nearest transparent texel at its own
// alpha over the opaque texel at the remaining weight, with alpha = 0x80/255:
//   R = 0x80*a + 0x20*(1-a) = 80.19  -> 80  (0x50)
//   G = 0xA0*a + 0xC0*(1-a) = 175.94 -> 176 (0xB0)
//   B = 0x40*a + 0xE0*(1-a) = 143.69 -> 144 (0x90)
// The fixed-point blend can land one unit either side of that. A frame that
// blends the far layer in first measures 0x589888, and one that blends it in
// after the near layer measures 0x4888A8; both miss by far more than this.
constexpr int kSandwichExpectedRed = 80;
constexpr int kSandwichExpectedGreen = 176;
constexpr int kSandwichExpectedBlue = 144;
constexpr int kSandwichTolerance = 2;
constexpr float kSandwichNearDepth = 0.25f;
constexpr float kSandwichOpaqueDepth = 0.5f;
constexpr float kSandwichFarDepth = 0.75f;

// Translucent display ("表示->半透明显示", WM_COMMAND 0xD6) fixture. One PMX
// model holds two opaque full-viewport quads: the black one is material 0 at
// kNearDepth, the cyan PMX opaque texture is material 1 at kFarDepth, both at
// raw Diffuse.a 1.0 so the half-transparency alone makes them blend.
// The original x64 material pass (0x1400B6D70) walks the materials in record
// order in one pass without touching the depth state, so the near quad blends
// over the cleared white background and its depth rejects the far quad:
//   R = G = B = 0 * 0.5 + 255 * 0.5 = 127.5 -> 128 (0x80)
// A pass that splits or sorts the materials draws the far quad first and lets
// both quads blend ((72,112,120) here), so any other outcome means the model
// left the original-order pass. The second arrangement gives the near slot to
// the cyan texture and expects (144,224,240) from it alone:
//   R = (32 + 255) * 0.5 = 143.5 -> 144, G = (192 + 255) * 0.5 -> 224,
//   B = (224 + 255) * 0.5 -> 240; a sorted pass would give (80,160,176).
const wchar_t kDisplayStatePath[] = L"alpha_occlusion_display_state.bmp";
constexpr DWORD kDisplayStateTexel = 0xFF000000u;  // opaque black
constexpr int kDisplayStateBlackExpected[3] = {128, 128, 128};
constexpr int kDisplayStateCyanExpected[3] = {144, 224, 240};
constexpr int kDisplayStateTolerance = 2;
// The default run fills resourcePool slots 0..4 and the pool scan stops at the
// first empty slot, so the display-state texture takes the first free one.
constexpr UINT kDisplayStatePoolSlot = 5;

struct ModelVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(ModelVertex) == kModelStride, "model vertex ABI");

// PMX vertex with one additional float4 UV set, the layout ModelVertexFormat
// reports for pmxAdditionalUvCount 1. The first 32 bytes stay identical to
// ModelVertex, so positions, normals and UV0 keep their production offsets.
struct ModelVertexUv1 {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    float u1, v1, z1, w1;
};
static_assert(sizeof(ModelVertexUv1) == kModelStrideUv1,
              "PMX vertex with one additional UV set ABI");

int g_failures = 0;

// Non-null only in a --msaa run: the same-size single-sample surface the
// multisampled back buffer is resolved into before it can be read back.
IDirect3DSurface9* g_resolveTarget = nullptr;

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

// Direct fixture publication must honor the same lookup generation contract
// as LoadTextureShared. The renderer indexes names only after invalidation.
void PublishFixtureTexture(D3DRenderer* renderer, UINT slot, const wchar_t* path,
                           IDirect3DTexture9* texture, const unsigned char (&tag)[4]) {
    auto& entry = renderer->resourcePool[slot];
    Check(!entry.heapBuffer && !entry.comObject, "fixture texture slot is empty before publication");
    entry.heapBuffer = const_cast<wchar_t*>(path);
    entry.comObject = reinterpret_cast<IUnknown*>(texture);
    std::memcpy(&entry.tag, tag, sizeof(tag));
    InvalidateTextureLookup();
    const auto* found = FindTextureResource(renderer, path);
    Check(found == &entry && found->comObject == reinterpret_cast<IUnknown*>(texture),
          "published fixture path resolves to its exact pool slot and texture");
}

void ReleaseFixtureTexture(D3DRenderer* renderer, UINT slot, IDirect3DTexture9* texture) {
    auto& entry = renderer->resourcePool[slot];
    Check(entry.comObject == reinterpret_cast<IUnknown*>(texture),
          "fixture texture release matches the published pool entry");
    entry = {};
    InvalidateTextureLookup();
    texture->Release();
}

// Copies the rendered frame into the readback surface. GetRenderTargetData
// refuses a multisampled source, so the --msaa run resolves the frame with
// StretchRect first; the single-sample run keeps its direct read.
void ReadRenderedPixels(IDirect3DDevice9* device,
                        IDirect3DSurface9* backbuffer,
                        IDirect3DSurface9* readback, const char* message) {
    if (g_resolveTarget == nullptr) {
        Check(SUCCEEDED(device->GetRenderTargetData(backbuffer, readback)),
              message);
        return;
    }
    Check(SUCCEEDED(device->StretchRect(backbuffer, nullptr, g_resolveTarget,
                                        nullptr, D3DTEXF_NONE)),
          "resolve the multisampled frame");
    Check(SUCCEEDED(device->GetRenderTargetData(g_resolveTarget, readback)),
          message);
}

#if !MMD_ORIGINAL_PROFILE
void CheckBatchedPixels(IDirect3DDevice9* device, IDirect3DSurface9* backbuffer,
                        IDirect3DSurface9* readback, int width, int height) {
    using transparent_geometry::Triangle;
    std::vector<Triangle> source;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        for (int layer = 0; layer < 4; ++layer) {
            Triangle t{};
            t.draw = layer % 3;
            const double left = -0.95 + x * 0.48 + layer * 0.002;
            const double top = -0.95 + y * 0.48;
            for (int v = 0; v < 3; ++v) {
                t.v[v].p[0] = left + (v == 1 ? 0.45 : 0);
                t.v[v].p[1] = top + (v == 2 ? 0.45 : 0);
                t.v[v].p[2] = 0.8 - layer * 0.15;
                t.v[v].data[0] = float(source.size());
            }
            source.push_back(t);
        }
    auto batched = source;
    Check(transparent_geometry::BatchDraws(batched, width, height),
          "pixel fixture must actually use fewer transparent draws");
    struct Vertex { float x, y, z, w; DWORD colour; };
    const auto render = [&](const std::vector<Triangle>& triangles) {
        Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                      0xFFFFFFFF, 1, 0)), "clear batch comparison");
        Check(SUCCEEDED(device->BeginScene()), "begin batch comparison");
        device->SetPixelShader(nullptr); device->SetVertexShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        std::vector<Vertex> vertices;
        for (const auto& t : triangles) for (const auto& v : t.v) {
            const unsigned identity = unsigned(v.data[0]);
            const DWORD colour = 0x70000000 | ((identity * 67 % 256) << 16) |
                                  ((identity * 37 % 256) << 8) | (identity * 19 % 256);
            vertices.push_back({float((v.p[0] + 1) * width / 2 - 0.5),
                                float((v.p[1] + 1) * height / 2 - 0.5),
                                float(v.p[2]), 1, colour});
        }
        for (std::size_t first = 0; first < triangles.size();) {
            auto end = first + 1;
            while (end < triangles.size() && triangles[end].draw == triangles[first].draw) ++end;
            Check(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(end - first),
                                                    vertices.data() + first * 3, sizeof(Vertex))),
                  "draw batched translucent islands");
            first = end;
        }
        Check(SUCCEEDED(device->EndScene()), "end batch comparison");
        ReadRenderedPixels(device, backbuffer, readback, "read batch comparison");
        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock batch pixels");
        std::vector<DWORD> pixels(std::size_t(width) * height);
        for (int y = 0; y < height; ++y)
            std::memcpy(pixels.data() + std::size_t(y) * width,
                        static_cast<const char*>(lock.pBits) + y * lock.Pitch, width * 4);
        readback->UnlockRect();
        Check(std::any_of(pixels.begin(), pixels.end(), [](DWORD p) { return (p & 0xFFFFFF) != 0xFFFFFF; }),
              "batch fixture must draw visible translucent pixels");
        return pixels;
    };
    const auto reference = render(source);
    Check(reference == render(batched), "batched and original GPU pixels must match exactly");
    std::printf("PASS batch GPU readback: %d identical pixels, msaa=%d\n",
                width * height, g_resolveTarget ? 1 : 0);
}
#endif

// Reports every mismatching sample instead of stopping at the first one: on a
// regression the whole band table is what identifies which surface lost the one
// behind it.
void ExpectPixel(bool ok, int x, int y, DWORD pixel, const char* submitted,
                 const char* texel, const char* expected) {
    if (ok) return;
    std::fprintf(stderr,
                 "FAIL pixel (%d,%d)=%08lX submission=%s texel=%s expected %s\n",
                 x, y, static_cast<unsigned long>(pixel), submitted, texel,
                 expected);
    ++g_failures;
}

// One quad spanning [left,right] x [bottom,top] with a single constant texture
// coordinate, so point sampling returns exactly the texel at (u,v). The
// triangles wind (0,2,1)/(0,3,2): the model pass runs with D3DCULL_CCW, and the
// opposite winding is the one that pass culls.
void AppendQuad(std::vector<ModelVertex>& vertices, std::vector<WORD>& indices,
                float left, float top, float right, float bottom, float z,
                float u, float v) {
    const WORD base = static_cast<WORD>(vertices.size());
    const float xs[4] = {left, right, right, left};
    const float ys[4] = {bottom, bottom, top, top};
    for (int corner = 0; corner < 4; ++corner) {
        ModelVertex vertex{};
        vertex.x = xs[corner];
        vertex.y = ys[corner];
        vertex.z = z;
        vertex.nz = 1.0f;
        vertex.u = u;
        vertex.v = v;
        vertices.push_back(vertex);
    }
    const WORD order[6] = {base, static_cast<WORD>(base + 2),
                           static_cast<WORD>(base + 1), base,
                           static_cast<WORD>(base + 3),
                           static_cast<WORD>(base + 2)};
    indices.insert(indices.end(), order, order + 6);
}

IDirect3DTexture9* CreateTexture(IDirect3DDevice9* device, const DWORD* texels,
                                 UINT width, UINT height, const char* message) {
    IDirect3DTexture9* texture = nullptr;
    Check(SUCCEEDED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &texture, nullptr)),
          message);
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(texture->LockRect(0, &lock, nullptr, 0)), "lock fixture texture");
    for (UINT y = 0; y < height; ++y) {
        auto* row = static_cast<char*>(lock.pBits) +
                    static_cast<std::size_t>(y) * lock.Pitch;
        for (UINT x = 0; x < width; ++x)
            *reinterpret_cast<DWORD*>(row + x * 4) = texels[y * width + x];
    }
    texture->UnlockRect(0);
    return texture;
}

// D3DMATERIAL9 is copied straight out of this record by the model pass:
// Diffuse comes from diffuse[4], Ambient from diffuseMirror[4] and Emissive
// from ambient[3]. White ambient and emissive keep the lit vertex colour white,
// so the texture alone decides the rendered colour.
//
// diffuseAlpha drives the material's own Diffuse.a. The two-layer fixture keeps
// it at 1.0 and lets the texture supply the half transparency, which is the
// case MMD authors actually hit; passing anything below 1.0 would move the
// material into the alpha<1 branch for the wrong reason.
void FillMaterial(mdl::ModelMaterialRecord& material, const wchar_t* texturePath,
                  std::int32_t faceVertexCount, float diffuseAlpha = 1.0f) {
    std::memset(&material, 0, sizeof(material));
    for (int channel = 0; channel < 4; ++channel) {
        material.diffuse[channel] = 1.0f;
        material.diffuseMirror[channel] = 1.0f;
    }
    material.diffuse[3] = diffuseAlpha;
    material.ambient[0] = material.ambient[1] = material.ambient[2] = 1.0f;
    material.faceVertexCount = faceVertexCount;
    // -1 selects the shared toon table, which is the white texture the fixture
    // binds at app->ToonTexture(0).
    material.toonReference = 0xFF;
    wcscpy_s(material.texturePath, texturePath);
}

struct Model {
    std::unique_ptr<mdl::ModelRecord> record;
    std::vector<mdl::ModelMaterialRecord> materials;
    // PMD keeps two 16-bit words per index and the sorters read every second
    // one, so the odd words stay zero (see pmd_load.cpp). PMX stores plain
    // 32-bit indices instead, and only one of the two vectors is filled.
    std::vector<std::uint16_t> cpuIndices;
    std::vector<std::int32_t> cpuPmxIndices;
    // PMX only: the effect pass reads the additive/multiplicative material
    // morph pools once per material, so a PMX fixture must supply them or the
    // draw dereferences null. Index 0 is the base pool, index 1+ the per-entry
    // pools the loader builds.
    std::vector<mdl::MaterialMorphPool> morphAdd;
    std::vector<mdl::MaterialMorphPool> morphMul;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DIndexBuffer9* indexBuffer = nullptr;
    ~Model() { if (vertexBuffer) vertexBuffer->Release(); if (indexBuffer) indexBuffer->Release(); }
};

// One material of a fixture model: the texture it samples, the material-record
// fields the pass branches on, and the index range it owns.
struct MaterialSpec {
    const wchar_t* texturePath;
    float diffuseAlpha;
    std::size_t firstIndex;
    std::size_t indexCount;
    // PMX material flags bit 0 is the double-sided (cull-none) bit; bit 1 is
    // the projected-shadow bit. PMD ignores them.
    std::uint8_t flags = 0;
};

// Builds a model from per-material specs that each name their own index range.
// The specs must tile the index buffer in order: the pass walks materials in
// record order and accumulates firstIndex, so the fixture checks the tiling
// rather than assuming it.
//
// pmx selects the PMX ABI: physicsMode 2 (which makes ModelVertexFormat return
// the plain 274/32 format for pmxAdditionalUvCount 0), a 32-bit index buffer
// with a plain int32 CPU index array, and the additive/multiplicative material
// morph pools the effect pass indexes per material. additionalUv1 is null for
// every other fixture, which keeps the 274/32 vertex; the sphere-only mode-3
// builder passes one float4 per vertex and gets the real 48-byte PMX vertex
// (524818, pmxAdditionalUvCount 1) with the same positions, normals and UV0.
std::unique_ptr<Model> BuildModel(
    IDirect3DDevice9* device, const std::vector<ModelVertex>& vertices,
    const std::vector<WORD>& indices,
    const std::vector<MaterialSpec>& specs, bool pmx = false,
    const std::vector<std::array<float, 4>>* additionalUv1 = nullptr) {
    const bool withUv1 = additionalUv1 != nullptr;
    Check(!withUv1 || pmx,
          "the additional UV set belongs to the PMX model ABI");
    Check(!withUv1 || additionalUv1->size() == vertices.size(),
          "the additional UV set covers every vertex");
    std::size_t expected = 0;
    for (const MaterialSpec& spec : specs) {
        Check(spec.firstIndex == expected,
              "material specs tile the index buffer in record order");
        expected += spec.indexCount;
    }
    Check(expected == indices.size(),
          "material specs cover the whole index buffer");
    auto model = std::make_unique<Model>();
    model->record = std::make_unique<mdl::ModelRecord>();
    model->materials.resize(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        FillMaterial(model->materials[i], specs[i].texturePath,
                     static_cast<std::int32_t>(specs[i].indexCount),
                     specs[i].diffuseAlpha);
        model->materials[i].flags = specs[i].flags;
    }
    // PMX wants one morph pool per material plus the pool at index 0 that
    // ConfigureEffectMaterial never reads but the loader always allocates,
    // matching the loader's materialCount + 1 sizing.
    model->morphAdd.resize(specs.size() + 1);
    model->morphMul.resize(specs.size() + 1);
    for (auto& pool : model->morphMul) {
        // A multiplicative entry of zero would zero the material's colour, and
        // PMX material morph defaults encode identity as 1 for the tint and
        // colour channels. Only the channels ConfigureEffectMaterial actually
        // consumes need the identity value, but filling the whole record keeps
        // the fixture honest about the layout.
        std::fill(std::begin(pool.channels.diffuse),
                  std::end(pool.channels.diffuse), 1.0f);
        std::fill(std::begin(pool.channels.textureTint),
                  std::end(pool.channels.textureTint), 1.0f);
        std::fill(std::begin(pool.channels.sphereTint),
                  std::end(pool.channels.sphereTint), 1.0f);
        std::fill(std::begin(pool.channels.toonTint),
                  std::end(pool.channels.toonTint), 1.0f);
    }

    // The CPU index array: PMD interleaves two 16-bit words and the sorters
    // read every second one, PMX stores plain 32-bit indices.
    if (pmx) {
        model->cpuPmxIndices.assign(indices.begin(), indices.end());
    } else {
        model->cpuIndices.assign(indices.size() * 2, 0);
        for (std::size_t i = 0; i < indices.size(); ++i)
            model->cpuIndices[2 * i] = indices[i];
    }

    mdl::ModelRecord& record = *model->record;
    record.vertexCount = static_cast<std::uint32_t>(vertices.size());
    record.indexCount = static_cast<std::uint32_t>(indices.size());
    record.materialCount = static_cast<std::uint32_t>(model->materials.size());
    record.materials = model->materials.data();
    record.indices = pmx
        ? static_cast<void*>(model->cpuPmxIndices.data())
        : static_cast<void*>(model->cpuIndices.data());
    record.loadComplete = 1;
    record.toonFlag = 1;
    // No outline: the regression covers the material pass alone.
    record.edgeScale = 0.0f;
    record.physicsMode = pmx ? 2 : 0;
    // One extra float4 selects the 524818/48 PMX vertex format; zero keeps the
    // plain 274/32 body vertex every other fixture uses.
    record.pmxAdditionalUvCount =
        static_cast<std::uint8_t>(withUv1 ? 1 : 0);
    if (pmx) {
        mdl::MaterialMorphAdd(reinterpret_cast<unsigned char*>(&record)) =
            model->morphAdd.data();
        mdl::MaterialMorphMul(reinterpret_cast<unsigned char*>(&record)) =
            model->morphMul.data();
    }

    const DWORD vertexFvf = withUv1 ? kModelFvfUv1 : kModelFvf;
    const UINT vertexBytes = static_cast<UINT>(
        vertices.size() * (withUv1 ? sizeof(ModelVertexUv1) : sizeof(ModelVertex)));
    std::vector<ModelVertexUv1> combinedVertices;
    if (withUv1) {
        combinedVertices.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            std::memcpy(&combinedVertices[i], &vertices[i], sizeof(ModelVertex));
            combinedVertices[i].u1 = (*additionalUv1)[i][0];
            combinedVertices[i].v1 = (*additionalUv1)[i][1];
            combinedVertices[i].z1 = (*additionalUv1)[i][2];
            combinedVertices[i].w1 = (*additionalUv1)[i][3];
        }
    }
    Check(SUCCEEDED(device->CreateVertexBuffer(vertexBytes, 0, vertexFvf,
                                               D3DPOOL_MANAGED,
                                               &model->vertexBuffer, nullptr)),
          "create model vertices");
    void* vertexData = nullptr;
    Check(SUCCEEDED(model->vertexBuffer->Lock(0, 0, &vertexData, 0)),
          "lock model vertices");
    if (withUv1)
        std::memcpy(vertexData, combinedVertices.data(), vertexBytes);
    else
        std::memcpy(vertexData, vertices.data(), vertexBytes);
    model->vertexBuffer->Unlock();

    const UINT indexBytes = static_cast<UINT>(
        indices.size() * (pmx ? sizeof(std::int32_t) : sizeof(WORD)));
    Check(SUCCEEDED(device->CreateIndexBuffer(
              indexBytes, 0, pmx ? D3DFMT_INDEX32 : D3DFMT_INDEX16,
              D3DPOOL_MANAGED, &model->indexBuffer, nullptr)),
          "create model indices");
    void* indexData = nullptr;
    Check(SUCCEEDED(model->indexBuffer->Lock(0, 0, &indexData, 0)),
          "lock model indices");
    if (pmx) {
        auto* destination = static_cast<std::int32_t*>(indexData);
        for (std::size_t i = 0; i < indices.size(); ++i)
            destination[i] = indices[i];
    } else {
        std::memcpy(indexData, indices.data(), indexBytes);
    }
    model->indexBuffer->Unlock();

    record.vertexBuffer = model->vertexBuffer;
    record.indexBuffer = model->indexBuffer;
    Check(record.pmxAdditionalUvCount ==
              static_cast<std::uint8_t>(withUv1 ? 1 : 0),
          "the record states the same additional-UV count the vertices carry");
    Check(record.vertexBuffer == model->vertexBuffer &&
              record.indexBuffer == model->indexBuffer,
          "the model record names the buffers the Model owns");
    if (withUv1) {
        D3DVERTEXBUFFER_DESC vertexDescription{};
        Check(SUCCEEDED(model->vertexBuffer->GetDesc(&vertexDescription)) &&
                  vertexDescription.FVF == kModelFvfUv1 &&
                  vertexDescription.Size == vertexBytes,
              "the additional-UV model holds the real 48-byte PMX vertex "
              "format");
    }
    return model;
}

const DWORD* Row(const D3DLOCKED_RECT& lock, int y) {
    return reinterpret_cast<const DWORD*>(
        static_cast<const char*>(lock.pBits) +
        static_cast<std::size_t>(y) * lock.Pitch);
}

// The three bands are sampled away from every edge so point sampling returns
// one exact texel: the solid one across the top, the half-transparent one in
// the middle and the transparent one at the bottom.
void SampleFrame(const char* submitted, const D3DLOCKED_RECT& lock) {
    for (int x : {24, 48, 72}) {
        const DWORD solid = Row(lock, 16)[x];
        ExpectPixel((solid & 0xFFFFFFu) == 0, x, 16, solid, submitted, "opaque",
                    "the solid black texel to cover the blue surface (0x000000)");

        const DWORD half = Row(lock, 48)[x];
        const int red = static_cast<int>(half >> 16 & 0xFF);
        const int green = static_cast<int>(half >> 8 & 0xFF);
        const int blue = static_cast<int>(half & 0xFF);
        ExpectPixel(red == 0 && green == 0 && blue >= 100 && blue <= 160, x, 48,
                    half, submitted, "half transparent",
                    "the blue surface blended with the black texel (about "
                    "0x00007F), never the white background (0x808080)");

        const DWORD clear = Row(lock, 80)[x];
        ExpectPixel((clear & 0xFFFFFFu) == 0x0000FFu, x, 80, clear, submitted,
                    "transparent", "the untouched blue surface (0xFF0000FF)");
    }
}

// Samples the two-layer frame. Every layer is a single full-viewport quad at
// its own depth, so one sample per band is one exact texel of each layer, and
// the two layers overlap across the whole viewport.
//
// Each channel is compared with a +/-1 tolerance. The expected values come from
// float reference arithmetic while the device blends in fixed point, so a
// correct render can land one unit either side of them. Every wrong outcome
// this scenario exists to catch misses by far more than one unit: a single
// layer over the clear colour gives (159,175,223) for the rear one or
// (191,207,159) for the front one, an unblended front layer gives
// (128,160,64), and the cleared background is (255,255,255).
void SampleTwoLayerFrame(const char* submitted, const D3DLOCKED_RECT& lock,bool frontOnly=false) {
    const auto within = [](int value, int expected) {
        return value >= expected - 1 && value <= expected + 1;
    };
    for (int x : {24, 48, 72}) {
        const DWORD pixel = Row(lock, 16)[x];
        const int red = static_cast<int>(pixel >> 16 & 0xFF);
        const int green = static_cast<int>(pixel >> 8 & 0xFF);
        const int blue = static_cast<int>(pixel & 0xFF);
        ExpectPixel(within(red,frontOnly ? 191 : kTwoLayerExpectedRed) &&
                        within(green,frontOnly ? 207 : kTwoLayerExpectedGreen) &&
                        within(blue,frontOnly ? 159 : kTwoLayerExpectedBlue),
                    x, 16, pixel, submitted, "two half-transparent layers",
                    frontOnly ? "Object-local sorting keeps near-first objects (about 0xBFCF9F)" :
                    "the rear layer then the front layer source-over (about 0x90A88F)");
    }
}

// Samples the PMX material-alpha scene. The front material blends at its own
// Diffuse.a of 0.5 over an opaque texel, the rear material at its texel alpha,
// so the pair has its own expected value.
void SampleMaterialAlphaFrame(const char* submitted, const D3DLOCKED_RECT& lock) {
    const auto within = [](int value, int expected) {
        return value >= expected - 1 && value <= expected + 1;
    };
    for (int x : {24, 48, 72}) {
        const DWORD pixel = Row(lock, 16)[x];
        const int red = static_cast<int>(pixel >> 16 & 0xFF);
        const int green = static_cast<int>(pixel >> 8 & 0xFF);
        const int blue = static_cast<int>(pixel & 0xFF);
        ExpectPixel(within(red, kMaterialAlphaExpectedRed) &&
                        within(green, kMaterialAlphaExpectedGreen) &&
                        within(blue, kMaterialAlphaExpectedBlue),
                    x, 16, pixel, submitted, "material diffuse alpha",
                    "the rear layer then the 0.5-alpha front layer source-over "
                    "(about 0x60B8E0); an unblended front layer gives "
                    "0x20C0E0, and the rear layer alone gives 0x9FAFDF");
    }
}

// Draws one arrangement of a two-model pair and returns the locked readback
// rows for the caller to sample. The readback surface stays locked until
// UnlockRect, so the lock is returned through the out parameter.
void RenderArrangement(MMDApp* app, IDirect3DDevice9* device,
                       IDirect3DSurface9* backbuffer,
                       IDirect3DSurface9* readback, int resource,
                       const D3DMATRIX& frameMatrix, Model* first, Model* second,
                       D3DLOCKED_RECT* lock) {
    first->record->comboSelIndex = 0;
    second->record->comboSelIndex = 1;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(first->record.get());
    app->ModelSlot(1) = reinterpret_cast<unsigned char*>(second->record.get());

    Check(SUCCEEDED(device->Clear(0, nullptr,
                                  D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                  kBackground, 1.0f, 0)),
          "clear the model frame");
    Check(SUCCEEDED(device->BeginScene()), "begin the model frame");
    device->SetRenderState(D3DRS_ALPHAREF, 7);
    if (resource == 0) RenderModelsFixed(app);
    else RenderModelsEffect(app, reinterpret_cast<const float*>(&frameMatrix));
    DWORD restoredAlpha = 0;
    device->GetRenderState(D3DRS_ALPHAREF, &restoredAlpha);
    Check(restoredAlpha == 7, "restore caller alpha reference");
    Check(SUCCEEDED(device->EndScene()), "end the model frame");
    ReadRenderedPixels(device, backbuffer, readback,
                       "read the rendered model pixels");
    Check(SUCCEEDED(readback->LockRect(lock, nullptr, D3DLOCK_READONLY)),
          "lock the rendered model pixels");
}

// Renders a single model in slot 0 (slot 1 empty) and leaves the readback
// surface locked for the caller to sample. Used by the one-model PMD and PMX
// scenes, which only differ in the model ABI.
void RenderOneModelScene(MMDApp* app, IDirect3DDevice9* device,
                         IDirect3DSurface9* backbuffer,
                         IDirect3DSurface9* readback, int resource,
                         const D3DMATRIX& frameMatrix, Model* model,
                         D3DLOCKED_RECT* lock) {
    model->record->comboSelIndex = 0;
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model->record.get());
    app->ModelSlot(1) = nullptr;

    Check(SUCCEEDED(device->Clear(0, nullptr,
                                  D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                  kBackground, 1.0f, 0)),
          "clear the one-model frame");
    Check(SUCCEEDED(device->BeginScene()), "begin the one-model frame");
    device->SetRenderState(D3DRS_ALPHAREF, 7);
    if (resource == 0) RenderModelsFixed(app);
    else RenderModelsEffect(app, reinterpret_cast<const float*>(&frameMatrix));
    // The opaque coverage pre-pass raises D3DRS_ALPHAREF and must put the
    // caller's value back; a single model with two materials exercises that
    // restore on the material path.
    DWORD restoredAlpha = 0;
    device->GetRenderState(D3DRS_ALPHAREF, &restoredAlpha);
    Check(restoredAlpha == 7, "restore caller alpha reference (one model)");
    Check(SUCCEEDED(device->EndScene()), "end the one-model frame");
    ReadRenderedPixels(device, backbuffer, readback,
                       "read the one-model pixels");
    Check(SUCCEEDED(readback->LockRect(lock, nullptr, D3DLOCK_READONLY)),
          "lock the one-model pixels");
}

#if !MMD_ORIGINAL_PROFILE
void CheckTextureSortCandidates(MMDApp* app, IDirect3DDevice9* device,
    IDirect3DSurface9* backbuffer, IDirect3DSurface9* readback, int resource,
    const D3DMATRIX& identity, bool mmeMarkerLoaded) {
    const auto precision = app->TransparentMaterialSortPrecision();
    const auto scope = app->TransparentSortCrossObjects();
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Maximum;
    constexpr auto path = L"opaque-rgb-sort-regression.bmp";
    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    AppendQuad(vertices, indices, -1.f, 1.f, 1.f, -1.f, .3f, .5f, .5f);

    struct Case { const char* name; unsigned char flags; bool sorted; };
    const Case cases[] = {
        {"known opaque", kTextureAlphaKnown, false},
        {"known alpha", kTextureAlphaKnown | kTextureUsesAlpha, true},
        {"unknown", 0, mmeMarkerLoaded},
        {"uses without known", kTextureUsesAlpha, mmeMarkerLoaded},
    };
    for (const auto& test : cases) {
        IDirect3DTexture9* texture = nullptr;
        const bool softAlpha = test.flags == (kTextureAlphaKnown | kTextureUsesAlpha);
        if (softAlpha) {
            constexpr DWORD texel = 0x80808080u;
            texture = CreateTexture(device, &texel, 1, 1, "soft-alpha regression texture");
        } else {
            Check(SUCCEEDED(device->CreateTexture(4, 4, 1, 0, D3DFMT_X8R8G8B8,
                  D3DPOOL_MANAGED, &texture, nullptr)), "opaque RGB regression texture");
        }
        unsigned char metadata[4]{};
        CacheTextureColorAndAlpha(texture, metadata);
        Check(metadata[3] == (softAlpha ? (kTextureAlphaKnown | kTextureUsesAlpha) : kTextureAlphaKnown),
              "measured texture alpha matches the control");
        metadata[3] = test.flags;
        PublishFixtureTexture(app->Renderer(), 5, path, texture, metadata);
        for (bool pmx : {false, true}) {
            auto model = BuildModel(device, vertices, indices, {{path, 1.f, 0, indices.size()}}, pmx);
            for (bool cross : {false, true}) {
                app->TransparentSortCrossObjects() = cross;
                const auto before = transparent_sort_cache::GetStatistics();
                D3DLOCKED_RECT pixels{};
                RenderOneModelScene(app, device, backbuffer, readback, resource, identity, model.get(), &pixels);
                readback->UnlockRect();
                const auto after = transparent_sort_cache::GetStatistics();
                const bool sorted = after.sourceMisses > before.sourceMisses || after.sourceHits > before.sourceHits;
                if (sorted != test.sorted) {
                    std::fprintf(stderr, "FAIL: %s flags=%u resource=%d pmx=%d cross=%d sorted=%d expected=%d\n",
                                 test.name, unsigned(test.flags), resource, pmx, cross, sorted, test.sorted);
                    std::exit(1);
                }
                if (!test.sorted)
                    Check(after.planMisses == before.planMisses && after.planHits == before.planHits &&
                          after.allocations == before.allocations, "excluded texture incurs no sorting plan or allocation");
            }
            app->ModelSlot(0) = app->ModelSlot(1) = nullptr;
        }
        ReleaseFixtureTexture(app->Renderer(), 5, texture);
    }
    app->TransparentMaterialSortPrecision() = precision;
    app->TransparentSortCrossObjects() = scope;
    std::printf("PASS texture sort candidates, resource=%d\n", resource);
}
#endif

void CheckTransparentCache(MMDApp* app,IDirect3DDevice9* device,IDirect3DSurface9* backbuffer,
                           IDirect3DSurface9* readback,int resource,const D3DMATRIX& identity,Model* model) {
#if !MMD_ORIGINAL_PROFILE
    const auto precision=app->TransparentMaterialSortPrecision();
    app->TransparentMaterialSortPrecision()=TransparentSortPrecision::Standard;
    D3DMATRIX transform=identity;
    std::vector<ModelVertex> originalVertices(model->record->vertexCount);
    void* originalData=nullptr;
    Check(SUCCEEDED(model->vertexBuffer->Lock(0,0,&originalData,D3DLOCK_READONLY)),"snapshot fixture vertices");
    std::memcpy(originalVertices.data(),originalData,originalVertices.size()*sizeof(ModelVertex));model->vertexBuffer->Unlock();
    const float originalAlpha=model->materials[0].diffuse[3];
    const auto render=[&] {
        device->SetTransform(D3DTS_WORLD,&transform);
        D3DLOCKED_RECT locked{};
        RenderOneModelScene(app,device,backbuffer,readback,resource,transform,model,&locked);
        D3DSURFACE_DESC desc{};readback->GetDesc(&desc);
        std::vector<DWORD> pixels(std::size_t(desc.Width)*desc.Height);
        for(UINT y=0;y<desc.Height;++y)std::memcpy(pixels.data()+std::size_t(y)*desc.Width,
            static_cast<const char*>(locked.pBits)+std::size_t(y)*locked.Pitch,desc.Width*4);
        readback->UnlockRect();return pixels;
    };
    using namespace mikumikustudio;
    transparent_sort_cache::SetEnabledForTests(false);const auto expected=render();
    transparent_sort_cache::SetEnabledForTests(true);Check(expected==render(),"cold transparent cache equals original projection");
    const auto before=transparent_sort_cache::GetStatistics();
    Check(expected==render(),"warm transparent cache has identical pixels");
    const auto warm=transparent_sort_cache::GetStatistics();
    Check(warm.sourceHits>before.sourceHits && warm.gpuHits>before.gpuHits && warm.uploads==before.uploads &&
        warm.allocations==before.allocations,"warm cache skips projection, allocation and upload");
    const auto changed=[&](auto mutate,bool changesGeometry) {
        render();const auto old=transparent_sort_cache::GetStatistics();mutate();const auto actual=render();
        if(changesGeometry)Check(transparent_sort_cache::GetStatistics().sourceMisses>old.sourceMisses,
            "camera or source data change requires fresh projection");
        transparent_sort_cache::SetEnabledForTests(false);
        Check(actual==render(),"changed transparent inputs equal independent uncached rendering");
        transparent_sort_cache::SetEnabledForTests(true);render();
    };
    changed([&]{transform._33=-1;transform._43=1;},true);
    changed([&]{transform=identity;},true);
    changed([&]{void* data=nullptr;Check(SUCCEEDED(model->vertexBuffer->Lock(0,0,&data,0)),"edit original source buffer");
        auto* vertices=static_cast<ModelVertex*>(data);
        for(UINT i=0;i<model->record->vertexCount;++i)vertices[i].z=1-vertices[i].z;
        model->vertexBuffer->Unlock();},true);
    changed([&]{if(model->record->physicsMode==2)std::swap(model->cpuPmxIndices[1],model->cpuPmxIndices[4]);
        else std::swap(model->cpuIndices[2],model->cpuIndices[8]);},true);
    changed([&]{model->materials[0].diffuse[3]=.35f;},false);
    const auto crossObjects=app->TransparentSortCrossObjects();
    const auto oldPlan=transparent_sort_cache::GetStatistics().planMisses;
    changed([&]{app->TransparentSortCrossObjects()=!crossObjects;},false);
    Check(transparent_sort_cache::GetStatistics().planMisses>oldPlan,
        "scope toggle invalidates the cached ordering plan");
    changed([&]{app->TransparentSortCrossObjects()=crossObjects;},false);
    // Restore all fixture inputs before the ordinary alpha assertions below.
    model->materials[0].diffuse[3]=originalAlpha;
    if(model->record->physicsMode==2)std::swap(model->cpuPmxIndices[1],model->cpuPmxIndices[4]);
    else std::swap(model->cpuIndices[2],model->cpuIndices[8]);
    void* data=nullptr;Check(SUCCEEDED(model->vertexBuffer->Lock(0,0,&data,0)),"restore source buffer");
    std::memcpy(data,originalVertices.data(),originalVertices.size()*sizeof(ModelVertex));
    model->vertexBuffer->Unlock();
    transparent_sort_cache::Release(app);
    Check(!transparent_sort_cache::GetStatistics().sourceBytes && !transparent_sort_cache::GetStatistics().gpuBytes,
        "reset/close release hook drops snapshot and default-pool buffers");
    app->TransparentMaterialSortPrecision()=precision;device->SetTransform(D3DTS_WORLD,&identity);
    std::printf("PASS transparent cache exact pixels and invalidation, resource=%d pmx=%u\n",resource,model->record->physicsMode==2);
#endif
}

void CheckObjectCache(MMDApp* app,IDirect3DDevice9* device,IDirect3DSurface9* backbuffer,
                      IDirect3DSurface9* readback,int resource,const D3DMATRIX& identity,
                      Model* moving,Model* still) {
#if !MMD_ORIGINAL_PROFILE
    using namespace mikumikustudio;
    const auto savedPrecision=app->TransparentMaterialSortPrecision();
    std::vector<ModelVertex> original(moving->record->vertexCount);
    void* data=nullptr;
    Check(SUCCEEDED(moving->vertexBuffer->Lock(0,0,&data,D3DLOCK_READONLY)),"snapshot moving object");
    std::memcpy(original.data(),data,original.size()*sizeof(ModelVertex));moving->vertexBuffer->Unlock();
    const auto render=[&] {
        D3DLOCKED_RECT locked{};
        RenderArrangement(app,device,backbuffer,readback,resource,identity,moving,still,&locked);
        D3DSURFACE_DESC desc{};readback->GetDesc(&desc);
        std::vector<DWORD> pixels(std::size_t(desc.Width)*desc.Height);
        for(UINT y=0;y<desc.Height;++y)std::memcpy(pixels.data()+std::size_t(y)*desc.Width,
            static_cast<const char*>(locked.pBits)+std::size_t(y)*locked.Pitch,desc.Width*4);
        readback->UnlockRect();return pixels;
    };
    for(auto precision:{TransparentSortPrecision::Low,TransparentSortPrecision::Standard,TransparentSortPrecision::High}) {
        app->TransparentMaterialSortPrecision()=precision;
        transparent_sort_cache::SetEnabledForTests(true);render();
        const auto before=transparent_sort_cache::GetStatistics();
        Check(SUCCEEDED(moving->vertexBuffer->Lock(0,0,&data,0)),"move only one object");
        auto* vertices=static_cast<ModelVertex*>(data);
        for(UINT i=0;i<moving->record->vertexCount;++i)vertices[i].x+=.15f;
        moving->vertexBuffer->Unlock();
        const auto actual=render();
        const auto after=transparent_sort_cache::GetStatistics();
        Check(after.objectHits==before.objectHits+1 && after.objectMisses==before.objectMisses+1,
            "a moving object reprojects alone while the other object reuses its projection");
        transparent_sort_cache::SetEnabledForTests(false);
        Check(actual==render(),"partly reused objects exactly match fresh rendering");
        Check(SUCCEEDED(moving->vertexBuffer->Lock(0,0,&data,0)),"restore moving object");
        std::memcpy(data,original.data(),original.size()*sizeof(ModelVertex));moving->vertexBuffer->Unlock();
    }
    transparent_sort_cache::SetEnabledForTests(true);
    app->TransparentMaterialSortPrecision()=savedPrecision;
    std::printf("PASS object cache partial reuse, resource=%d pmx=%u\n",resource,moving->record->physicsMode==2);
#endif
}

void RunSortOffScene(MMDApp* app, IDirect3DDevice9* device,
                     IDirect3DSurface9* backbuffer, IDirect3DSurface9* readback,
                     int resource, const D3DMATRIX& identity) {
    const auto savedPrecision = app->TransparentMaterialSortPrecision();
    const auto sample = [&](const D3DLOCKED_RECT& lock) {
        std::array<DWORD, 9> pixels{};
        unsigned index = 0;
        for (int y : {16, 48, 80}) for (int x : {24, 48, 72})
            pixels[index++] = Row(lock, y)[x] & 0xffffff;
        readback->UnlockRect();
        return pixels;
    };
    const auto one = [&](Model* model) {
        D3DLOCKED_RECT lock{};
        RenderOneModelScene(app, device, backbuffer, readback, resource, identity, model, &lock);
        return sample(lock);
    };
    const auto pair = [&](Model* first, Model* second) {
        D3DLOCKED_RECT lock{};
        RenderArrangement(app, device, backbuffer, readback, resource, identity, first, second, &lock);
        return sample(lock);
    };
    for (bool pmx : {false, true}) {
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
        std::vector<ModelVertex> frontVertices, rearVertices, bothVertices;
        std::vector<WORD> frontIndices, rearIndices, nearFirst;
        AppendQuad(frontVertices, frontIndices, -1, 1, 1, -1, kNearDepth, kTwoLayerSamplerU, .5f);
        AppendQuad(rearVertices, rearIndices, -1, 1, 1, -1, kFarDepth, .25f, .5f);
        AppendQuad(bothVertices, nearFirst, -1, 1, 1, -1, kNearDepth, kTwoLayerSamplerU, .5f);
        AppendQuad(bothVertices, nearFirst, -1, 1, 1, -1, kFarDepth, .25f, .5f);
        std::vector<WORD> farFirst(nearFirst.begin() + 6, nearFirst.end());
        farFirst.insert(farFirst.end(), nearFirst.begin(), nearFirst.begin() + 6);
        auto front = BuildModel(device, frontVertices, frontIndices,
            {{kTwoLayerFrontPath, 1, 0, 6}}, pmx);
        auto rear = BuildModel(device, rearVertices, rearIndices,
            {{kPmxAlphaPath, 1, 0, 6}}, pmx);
        auto frontFirst = BuildModel(device, bothVertices, nearFirst,
            {{kTwoLayerFrontPath, 1, 0, 6}, {kPmxAlphaPath, 1, 6, 6}}, pmx);
        auto rearFirst = BuildModel(device, bothVertices, farFirst,
            {{kPmxAlphaPath, 1, 0, 6}, {kTwoLayerFrontPath, 1, 6, 6}}, pmx);
        const auto frontOnly = one(front.get());
        for (DWORD pixel : frontOnly) {
            Check(std::abs(int(pixel >> 16 & 255) - 191) <= 2 &&
                  std::abs(int(pixel >> 8 & 255) - 207) <= 2 &&
                  std::abs(int(pixel & 255) - 159) <= 2,
                  "Off draws the translucent foreground exactly once over white");
        }
        const auto forward = one(frontFirst.get());
        const auto reversed = one(rearFirst.get());
        if (MMD_ORIGINAL_PROFILE || GetModuleHandleW(L"MMEffect.dll") != nullptr) {
            Check(forward == frontOnly, "original order retains one authored material walk");
            Check(reversed != frontOnly, "Off preserves authored material order instead of normalizing both orders");
            Check(pair(front.get(), rear.get()) == frontOnly,
                  "Off does not move the later opaque model ahead of translucent foreground");
        } else {
            Check(forward == reversed && forward != frontOnly,
                  "full Off keeps native opaque/translucent passes independently of authored material order");
            for (DWORD pixel : forward) {
                Check(std::abs(int(pixel >> 16 & 255) - 80) <= 2 &&
                      std::abs(int(pixel >> 8 & 255) - 176) <= 2 &&
                      std::abs(int(pixel & 255) - 144) <= 2,
                      "full Off draws the opaque rear then the translucent foreground exactly once");
            }
            Check(pair(front.get(), rear.get()) == forward,
                  "full Off retains native opaque/translucent pass order across models");
        }
        Check(pair(rear.get(), front.get()) == reversed,
              "Off follows native model order as well as material order");

        // The near-first CPU order must be restored after a sort has rewritten
        // this single material's GPU buffer into far-first triangle order.
        auto stack = BuildModel(device, bothVertices, nearFirst,
            {{kTwoLayerFrontPath, 1, 0, 12}}, pmx);
        Check(one(stack.get()) == frontOnly, "Off retains authored triangle order inside a material");
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Low;
        const auto sorted = one(stack.get());
        if (app->AutoTransparentMaterialSortingEnabled()) {
            Check(sorted != frontOnly,
                  "enabled sorting changes the near-first overlap fixture");
            // The additive block below reuses this fixture and must keep its
            // authored near-first GPU order. Prove the non-additive control is
            // really rewritten to the far-first order first, so a sorter that
            // stopped touching model buffers could not make both sides pass.
            // The MME proxy can take the split path without the original
            // in-place sort, so only the native frame carries this direct GPU
            // evidence.
            if (GetModuleHandleW(L"MMEffect.dll") == nullptr) {
                void* sortedGpu = nullptr;
                Check(SUCCEEDED(stack->indexBuffer->Lock(0, 0, &sortedGpu,
                                                         D3DLOCK_READONLY)),
                      "read the sorted non-additive GPU index buffer");
                for (std::size_t i = 0; i < farFirst.size(); ++i)
                    Check((pmx ? static_cast<const std::int32_t*>(sortedGpu)[i]
                               : static_cast<const WORD*>(sortedGpu)[i]) == farFirst[i],
                          "Low rewrites the non-additive control to far-first GPU order");
                stack->indexBuffer->Unlock();
            }
        }
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
        Check(one(stack.get()) == frontOnly,
              "On to Off restores native GPU index order before drawing");
        void* restored = nullptr;
        Check(SUCCEEDED(stack->indexBuffer->Lock(0, 0, &restored, D3DLOCK_READONLY)),
              "read restored native index buffer");
        for (std::size_t i = 0; i < nearFirst.size(); ++i)
            Check((pmx ? static_cast<const std::int32_t*>(restored)[i]
                       : static_cast<const WORD*>(restored)[i]) == nearFirst[i],
                  "Off restores every original GPU triangle index");
        stack->indexBuffer->Unlock();
        // Additive (postLoadFlag2) models keep the authored triangle order at
        // every precision: the sort helper must return before it locks or
        // rewrites their GPU buffer. The non-additive stack above is rewritten
        // to far-first at Low, so the same sorter would corrupt this model
        // without that early return. Each tier gets a fresh model so one tier
        // never reads back a buffer another tier already sorted.
        for (const TransparentSortPrecision tier :
             {TransparentSortPrecision::Off, TransparentSortPrecision::Low,
              TransparentSortPrecision::Standard, TransparentSortPrecision::High,
              TransparentSortPrecision::Maximum}) {
            auto additive = BuildModel(device, bothVertices, nearFirst,
                {{kTwoLayerFrontPath, 1, 0, 12}}, pmx);
            additive->record->postLoadFlag2 = 1;
            Check(additive->record->postLoadFlag2 == 1,
                  "additive fixture selects postLoadFlag2 before its first draw");
            app->TransparentMaterialSortPrecision() = tier;
            one(additive.get());
            void* gpu = nullptr;
            Check(SUCCEEDED(additive->indexBuffer->Lock(0, 0, &gpu,
                                                        D3DLOCK_READONLY)),
                  "read additive GPU index order");
            for (std::size_t i = 0; i < nearFirst.size(); ++i)
                Check((pmx ? static_cast<const std::int32_t*>(gpu)[i]
                           : static_cast<const WORD*>(gpu)[i]) == nearFirst[i],
                      "additive model keeps every authored GPU index at every precision");
            additive->indexBuffer->Unlock();
            for (std::size_t i = 0; i < nearFirst.size(); ++i)
                Check((pmx ? additive->cpuPmxIndices[i]
                           : additive->cpuIndices[2 * i]) == nearFirst[i],
                      "additive model keeps every authored CPU source index");
            // Slot 0 still names this model; clear it before this tier's local
            // record is destroyed.
            app->ClearModelSlots();
        }
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
        std::printf("sort-off %s additive postLoadFlag2 GPU order preserved across all precisions\n",
                    pmx ? "PMX" : "PMD");
        app->ClearModelSlots();
        std::printf("sort-off %s native pass/triangle order and single draw verified\n", pmx ? "PMX" : "PMD");
    }
    app->TransparentMaterialSortPrecision() = savedPrecision;
}

// Translucent display (displayState != 0) keeps the original-order single
// pass: the half-transparency makes every material blend (the fixed path via
// the 0.5 displayState override on the material copy, the effect path via the
// shader's transp constant, which the global characterTransparentMode feeds
// and which the 0xD6 command writes together with displayState), and the walk
// submits the materials in record order with the depth state untouched,
// exactly like the x64 baseline pass at 0x1400B6D70. A pass that splits the
// model into opaque and translucent passes or sorts its materials paints the
// far quad into the frame behind the near one, so the sampled pixel must not
// change with the transparent sort tier.
void RunDisplayStateScene(MMDApp* app, IDirect3DDevice9* device,
                          IDirect3DSurface9* backbuffer,
                          IDirect3DSurface9* readback, int resource,
                          const D3DMATRIX& identity) {
    auto* renderer = app->Renderer();
    const DWORD texel = kDisplayStateTexel;
    IDirect3DTexture9* texture = CreateTexture(
        device, &texel, 1, 1, "create the display-state black texture");
    unsigned char tag[4]{};
    CacheTextureColorAndAlpha(texture, tag);
    Check((tag[3] & kTextureUsesAlpha) == 0,
          "the opaque black texel classifies the display-state texture as "
          "opaque");
    PublishFixtureTexture(renderer, kDisplayStatePoolSlot, kDisplayStatePath, texture, tag);

    const auto savedPrecision = app->TransparentMaterialSortPrecision();
    // The WM_COMMAND 0xD6 handler (command_dispatch.cpp case 0xD6) writes the
    // global characterTransparentMode and every model's displayState together.
    // The effect frame turns the global into the shader's transp constant,
    // which sets the output alpha to 0.5, so the fixture sets both fields
    // exactly like the command does and every renderer resource sees the
    // real state.
    const auto savedTransparentMode = app->state.characterTransparentMode;
    app->state.characterTransparentMode = 1;
    for (const TransparentSortPrecision tier :
         {TransparentSortPrecision::Off, TransparentSortPrecision::Low,
          TransparentSortPrecision::Standard, TransparentSortPrecision::High,
          TransparentSortPrecision::Maximum}) {
        app->TransparentMaterialSortPrecision() = tier;
        for (int arrangement = 0; arrangement < 2; ++arrangement) {
            // Both arrangements keep the black quad as the near one; the
            // arrangement only decides which material samples which texture.
            std::vector<ModelVertex> vertices;
            std::vector<WORD> indices;
            AppendQuad(vertices, indices, -1.0f, 1.0f, 1.0f, -1.0f, kNearDepth,
                       0.25f, 0.5f);
            AppendQuad(vertices, indices, -1.0f, 1.0f, 1.0f, -1.0f, kFarDepth,
                       0.25f, 0.5f);
            const wchar_t* nearPath =
                arrangement == 0 ? kDisplayStatePath : kPmxAlphaPath;
            const wchar_t* farPath =
                arrangement == 0 ? kPmxAlphaPath : kDisplayStatePath;
            auto model = BuildModel(
                device, vertices, indices,
                {{nearPath, 1.0f, 0, 6}, {farPath, 1.0f, 6, 6}}, true);
            model->record->displayState = 1;
            D3DLOCKED_RECT lock{};
            RenderOneModelScene(app, device, backbuffer, readback, resource,
                                identity, model.get(), &lock);
            const int* expected = arrangement == 0 ? kDisplayStateBlackExpected
                                                   : kDisplayStateCyanExpected;
            char submission[96]{};
            std::snprintf(submission, sizeof(submission),
                          "displayState tier=%d arrangement=%d",
                          static_cast<int>(tier), arrangement);
            for (int y : {16, 48, 80}) {
                for (int x : {24, 48, 72}) {
                    const DWORD pixel = Row(lock, y)[x];
                    const int red = static_cast<int>(pixel >> 16 & 0xFF);
                    const int green = static_cast<int>(pixel >> 8 & 0xFF);
                    const int blue = static_cast<int>(pixel & 0xFF);
                    ExpectPixel(
                        std::abs(red - expected[0]) <= kDisplayStateTolerance &&
                            std::abs(green - expected[1]) <=
                                kDisplayStateTolerance &&
                            std::abs(blue - expected[2]) <=
                                kDisplayStateTolerance,
                        x, y, pixel, submission,
                        "record-order single pass",
                        "only the near quad blends over the white background; "
                        "a split or sorted pass adds the far quad's colour");
                }
            }
            readback->UnlockRect();
        }
    }
    app->state.characterTransparentMode = savedTransparentMode;
    app->TransparentMaterialSortPrecision() = savedPrecision;
    ReleaseFixtureTexture(renderer, kDisplayStatePoolSlot, texture);
    std::printf("displayState scenes sampled 18 pixels per tier\n");
}

// The three sample positions of the stack fixture along one axis: one third,
// one half and two thirds of the axis. The fixture is a full-viewport quad, so
// these stay inside it at any target size, and on a 96x96 target they are the
// 32/48/64 grid the other scenes use.
struct StackSampleAxis {
    int first;
    int middle;
    int last;
};

StackSampleAxis StackSampleAxisOf(int extent) {
    return {extent / 3, extent / 2, 2 * extent / 3};
}

// One black-layer stack case, shared by --low-alpha and --maximum-limit. The
// fixture is a single PMX material whose texture is a 1x1 black texel at the
// case's alpha, stacked as one full-viewport quad per layer at a distinct
// depth, rendered once through the shader-model-3 effect (resource 118). Every
// sampled pixel must match the double-precision source-over reference of the
// whole stack, 255 * pow(1 - alpha, layers), within kLowAlphaTolerance, and
// must keep A=255 over the opaque white clear. A stack whose low alpha was
// alpha-tested away, quantised to zero or short of a layer lands on the white
// clear colour or on a far brighter value, and the printed measurement names
// which it was. The caller selects the precision; `mode` names the run in both
// the report line and any pixel failure. The nine sample positions and the
// reported centre come from the target the device actually has, so the one
// fixture serves both the 16x16 default and the 96x96 stress target.
void RunLayerStackCase(MMDApp* app, IDirect3DDevice9* device,
                       IDirect3DSurface9* backbuffer,
                       IDirect3DSurface9* readback, int resource,
                       const D3DMATRIX& identity, const LowAlphaCase& fixture,
                       UINT slot, const char* mode) {
    char submission[64]{};
    std::snprintf(submission, sizeof(submission), "%s layer stack", mode);
    auto* renderer = app->Renderer();
    const StackSampleAxis xSamples = StackSampleAxisOf(
        static_cast<int>(renderer->presentParameters.BackBufferWidth));
    const StackSampleAxis ySamples = StackSampleAxisOf(
        static_cast<int>(renderer->presentParameters.BackBufferHeight));
    // A black texel that carries nothing but its alpha, in a 1x1 texture
    // that point sampling returns exactly at any coordinate.
    const DWORD texel = static_cast<DWORD>(fixture.texelAlpha) << 24;
    IDirect3DTexture9* texture = CreateTexture(
        device, &texel, 1, 1, "create the low-alpha fixture texture");
    unsigned char tag[4]{};
    CacheTextureColorAndAlpha(texture, tag);
    Check((tag[3] & kTextureUsesAlpha) != 0,
          "a texel alpha below 255 classifies the low-alpha texture as "
          "translucent");
    // The same pool flow the other fixtures use: the material names the
    // path, and the pass resolves it back to this texture and its tag.
    PublishFixtureTexture(renderer, slot, fixture.texturePath, texture, tag);

    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    const float step = (kLowAlphaFarDepth - kLowAlphaNearDepth) /
                       static_cast<float>(fixture.layers);
    for (int layer = 0; layer < fixture.layers; ++layer) {
        const float z = kLowAlphaFarDepth -
                        (static_cast<float>(layer) + 0.5f) * step;
        AppendQuad(vertices, indices, -1.0f, 1.0f, 1.0f, -1.0f, z, 0.5f,
                   0.5f);
    }
    auto model = BuildModel(
        device, vertices, indices,
        std::vector<MaterialSpec>{{fixture.texturePath, 1.0f, 0,
                                   indices.size()}},
        true);
    Check(indices.size() == static_cast<std::size_t>(fixture.layers) * 6 &&
              model->materials[0].faceVertexCount ==
                  static_cast<std::int32_t>(indices.size()),
          "the low-alpha material owns one quad per layer");

    D3DLOCKED_RECT lock{};
    RenderOneModelScene(app, device, backbuffer, readback, resource, identity,
                        model.get(), &lock);
    const double alpha = static_cast<double>(fixture.texelAlpha) / 255.0;
    const double expected =
        255.0 * std::pow(1.0 - alpha, static_cast<double>(fixture.layers));
    // The destination alpha is checked along with the colour: the frame is
    // cleared to opaque white, so a stack that leaves the colour intact but
    // decays the alpha channel has still damaged the background.
    for (int y : {ySamples.first, ySamples.middle, ySamples.last}) {
        for (int x : {xSamples.first, xSamples.middle, xSamples.last}) {
            const DWORD pixel = Row(lock, y)[x];
            const int red = static_cast<int>(pixel >> 16 & 0xFF);
            const int green = static_cast<int>(pixel >> 8 & 0xFF);
            const int blue = static_cast<int>(pixel & 0xFF);
            const int alpha = static_cast<int>(pixel >> 24 & 0xFF);
            ExpectPixel(
                std::fabs(static_cast<double>(red) - expected) <=
                        kLowAlphaTolerance &&
                    std::fabs(static_cast<double>(green) - expected) <=
                        kLowAlphaTolerance &&
                    std::fabs(static_cast<double>(blue) - expected) <=
                        kLowAlphaTolerance &&
                    alpha == 255,
                x, y, pixel, submission,
                "black layers with a 1/255 or 2/255 texel alpha",
                "255 * pow(1 - alpha, layers) within 2 over the opaque "
                "white clear, whose A=255 must survive");
        }
    }
    const DWORD measured = Row(lock, ySamples.middle)[xSamples.middle];
    std::printf("%s resource=%d layers=%d alpha=%d/255 "
                "measured=(%d,%d,%d) alpha=%d expected=%.2f tolerance=%.0f "
                "pixel=%08lX\n",
                mode, resource, fixture.layers, fixture.texelAlpha,
                static_cast<int>(measured >> 16 & 0xFF),
                static_cast<int>(measured >> 8 & 0xFF),
                static_cast<int>(measured & 0xFF),
                static_cast<int>(measured >> 24 & 0xFF), expected,
                kLowAlphaTolerance, static_cast<unsigned long>(measured));
    readback->UnlockRect();
    ReleaseFixtureTexture(renderer, slot, texture);
}

// The --low-alpha scenes. Each case is the shared black-layer stack at its own
// depth and texel alpha, rendered through the shader-model-3 resource (118).
// Select Maximum explicitly so low-alpha accumulation still exercises peeling.
void RunLowAlphaScenes(MMDApp* app, IDirect3DDevice9* device,
                       IDirect3DSurface9* backbuffer,
                       IDirect3DSurface9* readback, int resource,
                       const D3DMATRIX& identity) {
    app->TransparentMaterialSortPrecision() = kPeelTier;
    for (std::size_t index = 0; index < std::size(kLowAlphaCases); ++index) {
        RunLayerStackCase(app, device, backbuffer, readback, resource, identity,
                          kLowAlphaCases[index],
                          kLowAlphaPoolBase,
                          "low-alpha");
    }
}

// The --maximum-limit scenes. The fixture is the --low-alpha black-layer stack;
// the layer counts and the selected precision differ. Both stacks exceed the
// historical fixed peel budgets. The assertion checks the final composite pixel,
// independently of the number of iterations that actually ran.
void RunMaximumLimitScenes(MMDApp* app, IDirect3DDevice9* device,
                           IDirect3DSurface9* backbuffer,
                           IDirect3DSurface9* readback, int resource,
                           const D3DMATRIX& identity) {
    // Maximum is the top tier in both builds, so the enumerator names it
    // directly and no stored byte is spelled out here.
    const auto precision = TransparentSortPrecision::Maximum;
    app->TransparentMaterialSortPrecision() = precision;
    // The mode's own line names the target it renders into, so a log always
    // shows whether this was the 16x16 default or the --maximum-full-size run.
    const auto& present = app->Renderer()->presentParameters;
    for (std::size_t index = 0; index < std::size(kMaximumLimitCases); ++index) {
        const LowAlphaCase& fixture = kMaximumLimitCases[index];
        std::printf("maximum-limit layers=%d precision=%d alpha=%d/255 "
                    "target=%ux%u\n",
                    fixture.layers, static_cast<int>(precision),
                    fixture.texelAlpha, present.BackBufferWidth,
                    present.BackBufferHeight);
        RunLayerStackCase(app, device, backbuffer, readback, resource, identity,
                          fixture,
                          kMaximumLimitPoolBase,
                          "maximum-limit");
    }
}

// The historical --peel-budget fixture now verifies High's direct geometry
// order: all black layers must draw, followed by the nearest half-green layer.
void RunPeelBudgetScene(MMDApp* app, IDirect3DDevice9* device,
                        IDirect3DSurface9* backbuffer,
                        IDirect3DSurface9* readback, int resource,
                        const D3DMATRIX& identity) {
    auto* renderer = app->Renderer();
    // Retain the legacy --peel-budget fixture invocation. High no longer peels;
    // this deep stack must now reach the same final green pixel directly.
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::High;

    IDirect3DTexture9* texture = CreateTexture(
        device, kPeelBudgetTexels, 2, 1,
        "create the peel-budget fixture texture");
    unsigned char tag[4]{};
    CacheTextureColorAndAlpha(texture, tag);
    Check((tag[3] & kTextureUsesAlpha) != 0,
          "the half-transparent texels classify the peel-budget texture as "
          "translucent");
    PublishFixtureTexture(renderer, kPeelBudgetPoolSlot, kPeelBudgetPath, texture, tag);

    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    const float step = (kPeelBudgetFarDepth - kPeelBudgetNearDepth) /
                       static_cast<float>(kPeelBudgetLayers);
    for (int layer = 0; layer < kPeelBudgetLayers; ++layer) {
        // One distinct depth per layer, the farthest appended first, so the
        // authored index order already is a valid far-to-near order.
        const bool nearest = layer + 1 == kPeelBudgetLayers;
        const float z = kPeelBudgetFarDepth -
                        (static_cast<float>(layer) + 0.5f) * step;
        AppendQuad(vertices, indices, -1.0f, 1.0f, 1.0f, -1.0f, z,
                   nearest ? kPeelBudgetGreenU : kPeelBudgetBlackU, 0.5f);
    }
    auto model = BuildModel(
        device, vertices, indices,
        std::vector<MaterialSpec>{{kPeelBudgetPath, 1.0f, 0, indices.size()}},
        true);
    Check(indices.size() ==
                  static_cast<std::size_t>(kPeelBudgetLayers) * 6 &&
              model->materials[0].faceVertexCount ==
                  static_cast<std::int32_t>(indices.size()),
          "the peel-budget material owns one quad per layer");

    D3DLOCKED_RECT lock{};
    RenderOneModelScene(app, device, backbuffer, readback, resource, identity,
                        model.get(), &lock);
    for (int y = kSampleFirst; y <= kSampleLast; y += kSampleStep) {
        for (int x = kSampleFirst; x <= kSampleLast; x += kSampleStep) {
            const DWORD pixel = Row(lock, y)[x];
            const int red = static_cast<int>(pixel >> 16 & 0xFF);
            const int green = static_cast<int>(pixel >> 8 & 0xFF);
            const int blue = static_cast<int>(pixel & 0xFF);
            ExpectPixel(std::abs(red - kPeelBudgetExpectedRed) <=
                                kPeelBudgetTolerance &&
                            std::abs(green - kPeelBudgetExpectedGreen) <=
                                kPeelBudgetTolerance &&
                            std::abs(blue - kPeelBudgetExpectedBlue) <=
                                kPeelBudgetTolerance,
                        x, y, pixel, "high deep stack",
                        "black far layers under one green layer at alpha 0x80",
                        "the complete draw: (0,128,0) "
                        "within 2; black loses the green layer, white means "
                        "nothing was drawn");
        }
    }
    const DWORD measured = Row(lock, 48)[48];
    std::printf("peel-budget resource=%d layers=%d precision=%d "
                "measured=(%d,%d,%d) expected=(%d,%d,%d) tolerance=%d "
                "pixel=%08lX\n",
                resource, kPeelBudgetLayers, static_cast<int>(app->TransparentMaterialSortPrecision()),
                static_cast<int>(measured >> 16 & 0xFF),
                static_cast<int>(measured >> 8 & 0xFF),
                static_cast<int>(measured & 0xFF), kPeelBudgetExpectedRed,
                kPeelBudgetExpectedGreen, kPeelBudgetExpectedBlue,
                kPeelBudgetTolerance, static_cast<unsigned long>(measured));
    readback->UnlockRect();
    ReleaseFixtureTexture(renderer, kPeelBudgetPoolSlot, texture);
}


// ---- --sphere-only: native PMX sphereMode GPU pixel regression -------------
//
// A PMX material whose base texture path is empty and whose sphereMode is 1
// samples its sphere texture as the surface colour on the fixed path and on
// both embedded shader paths, so the sphere texel's alpha reaches the blend
// stage. --sphere-mode 2 selects the additive cascade (BShadowSphiaTec with
// spadd on SM3, D3DTOP_ADD on the fixed path) and mode 3 the TEXCOORD1 cascade
// (BShadowTexCd2Tec). Both the fixed and the embedded paths keep the sphere
// texel's alpha as the blend alpha in all three modes. The fixture is one PMX
// model with two materials; each material owns
// one quad inside the same [-0.75,0.75] bounds, the near quad at z 0.25 and the
// far quad at z 0.75. The two authoring orders put the near or the far material
// first in both the material record and the index buffer.
//
// The main matrix renders resources 0 (fixed), 117 (SM2) and 118 (SM3), both
// author orders, each Off -> Low -> Standard -> High -> Maximum -> Off. The
// expected pixel is computed here from a double-precision source-over of the
// two texel colours over the white clear; the sorter, the texture classifier
// and every control frame are never consulted for it. Off keeps the authored
// order and the depth write, so near-first keeps only the near layer while
// far-first blends both; every enabled tier sorts far to near and must show
// both. The final Off frame must reproduce the first Off frame's RGB bytes and
// the authored CPU index array must never change.
//
// Controls on the same target cover single-layer sampling, sphereMode 0, an
// opaque sphere alpha and the identical half alpha moved to the base texture
// path. Mode 2 keeps its control scenes on the original white base: only the
// materials that actually add a sphere texel lose their base colour, so
// "a white base plus an additive sphere is saturated" cannot mask the layer
// order. Mode 3 builds the same scenes from a real 48-byte PMX vertex with one
// float4 TEXCOORD1 and adds a 2x1 UV-source probe: the two texels are different
// colours, UV0 points at the other texel than UV1, and a frame with the same
// UV1 but a moved UV0 must repeat the first probe frame's pixels.
// Every frame is written to an absolute, new-or-empty capture directory as raw
// BGRA and as a viewable 24-bit BMP, with one JSONL record per frame and per
// restore check. A capture or record write failure exits non-zero.
constexpr wchar_t kSphereNearPath[] = L"alpha_occlusion_sphere_near.bmp";
constexpr wchar_t kSphereFarPath[] = L"alpha_occlusion_sphere_far.bmp";
constexpr DWORD kSphereNearTexel = 0x80FF0000u;        // a=0x80 r=0xFF
constexpr DWORD kSphereFarTexel = 0x800000FFu;         // a=0x80 b=0xFF
constexpr DWORD kSphereNearOpaqueTexel = 0xFFFF0000u;  // a=0xFF r=0xFF
constexpr DWORD kSphereFarOpaqueTexel = 0xFF0000FFu;   // a=0xFF b=0xFF
constexpr UINT kSphereNearSlot = 5;
constexpr UINT kSphereFarSlot = 6;
// The mode-3 TEXCOORD1 probe publishes its own 2x1 texture after the main
// matrix, so it cannot disturb the near/far pair.
constexpr UINT kSphereUv1Slot = 7;
constexpr float kSphereLeft = -0.75f;
constexpr float kSphereTop = 0.75f;
constexpr float kSphereRight = 0.75f;
constexpr float kSphereBottom = -0.75f;
constexpr int kSphereTolerance = 2;
constexpr double kSphereTexelAlpha = 128.0 / 255.0;
constexpr int kSphereRoiRadius = 2;  // a 5x5 interior block
constexpr int kSphereMainStepCount = 6;
// --sphere-mode is a native PMX sphereMode selector, not a scene switch: it is
// valid only with --sphere-only. Mode 1 is the default and keeps the original
// 60-frame matrix; mode 2 runs the same matrix on the additive cascade; mode 3
// runs it on the TEXCOORD1 cascade and adds the UV-source probe.
constexpr int kSphereModeDefault = 1;
constexpr int kSphereModeMin = 1;
constexpr int kSphereModeMax = 3;
constexpr int kSphereResourceCount = 3;  // fixed, 117 (SM2) and 118 (SM3)
// Per resource: two author orders x Off -> ... -> Off, the eight controls
// (near-only, far-only, sphereMode 0 x2, base-alpha x2, opaque alpha x2), and
// the three mode-3 UV-source probe frames.
constexpr int kSphereMainFramesPerResource = 2 * kSphereMainStepCount;
constexpr int kSphereControlFramesPerResource = 8;
constexpr int kSphereUv1ProbeFramesPerResource = 3;

struct SphereRoi { int x; int y; };
// Both centres stay well inside the quad and away from its diagonal, which
// runs from the lower-left to the upper-right corner of the viewport.
constexpr SphereRoi kSphereRois[] = {{30, 30}, {66, 66}};

const char* const kSpherePrecisionNames[kSphereMainStepCount] = {
    "off", "low", "standard", "high", "maximum", "off"};
constexpr TransparentSortPrecision kSpherePrecisions[kSphereMainStepCount] = {
    TransparentSortPrecision::Off, TransparentSortPrecision::Low,
    TransparentSortPrecision::Standard, TransparentSortPrecision::High,
    TransparentSortPrecision::Maximum, TransparentSortPrecision::Off};
const char* const kSphereOrderNames[2] = {"near-first", "far-first"};

std::wstring g_sphereCaptureDir;
HANDLE g_sphereRecord = INVALID_HANDLE_VALUE;
int g_sphereMode = kSphereModeDefault;
int g_sphereFrameCount = 0;
int g_sphereFailedFrames = 0;

// The frame matrix each mode is contracted to render. The startup and end
// records and the JSONL summary all state these numbers, and main asserts the
// rendered count against them after the resource loop.
int SphereMainFrameCount() {
    return kSphereMainFramesPerResource * kSphereResourceCount;
}

int SphereControlFrameCount() {
    return kSphereControlFramesPerResource * kSphereResourceCount;
}

int SphereUv1ProbeFrameCount() {
    return (g_sphereMode == 3 ? kSphereUv1ProbeFramesPerResource : 0) *
           kSphereResourceCount;
}

int SphereExpectedFrameCount() {
    return SphereMainFrameCount() + SphereControlFrameCount() +
           SphereUv1ProbeFrameCount();
}

bool SphereIsAbsolutePath(const std::wstring& path) {
    const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
    return drive || unc;
}

bool SphereCreateDirectoryTree(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr) != 0)
        return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return true;
    const std::size_t split = path.find_last_of(L"\\/");
    if (split == std::wstring::npos)
        return false;
    const std::wstring parent = path.substr(0, split);
    if (!parent.empty() && !(parent.size() == 2 && parent[1] == L':') &&
        !SphereCreateDirectoryTree(parent))
        return false;
    if (CreateDirectoryW(path.c_str(), nullptr) != 0)
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool SphereDirectoryEmpty(const std::wstring& path) {
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = path + L"\\*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return false;
    bool empty = true;
    do {
        const std::wstring name(entry.cFileName);
        if (name == L"." || name == L"..")
            continue;
        empty = false;
        break;
    } while (FindNextFileW(search, &entry) != 0);
    FindClose(search);
    return empty;
}

// The capture directory is usage, not a scene input: a relative path, an
// existing file or a directory that already holds anything is rejected before
// a device exists, so a run can never overwrite user files.
bool SpherePrepareCaptureDirectory(std::wstring* path, std::string* error) {
    std::wstring normalized = *path;
    while (normalized.size() > 3 &&
           (normalized.back() == L'\\' || normalized.back() == L'/'))
        normalized.pop_back();
    if (normalized.empty()) {
        *error = "the directory path is empty";
        return false;
    }
    if (!SphereIsAbsolutePath(normalized)) {
        *error = "the directory path is not absolute";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            *error = "the path names an existing file";
            return false;
        }
        if (!SphereDirectoryEmpty(normalized)) {
            *error = "the directory must be new or empty";
            return false;
        }
    } else if (!SphereCreateDirectoryTree(normalized)) {
        *error = "the directory cannot be created";
        return false;
    }
    *path = normalized;
    return true;
}

// Builds one capture path inside a prepared capture directory. Both capture
// modes share the same validated new-or-empty directory rule and the same
// ASCII file-name conversion; only the directory differs.
std::wstring CapturePathIn(const std::wstring& directory, const char* name) {
    std::wstring wide;
    for (const char* cursor = name; cursor != nullptr && *cursor != '\0'; ++cursor)
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path += wide;
    return path;
}

std::wstring SphereCapturePath(const char* name) {
    return CapturePathIn(g_sphereCaptureDir, name);
}

void WriteSphereFile(const std::wstring& path, const void* data,
                     std::size_t bytes, const char* failure) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, failure);
    const auto* cursor = static_cast<const unsigned char*>(data);
    std::size_t remaining = bytes;
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, 0x100000u));
        DWORD written = 0;
        Check(WriteFile(file, cursor, chunk, &written, nullptr) != 0 &&
                  written == chunk,
              failure);
        cursor += written;
        remaining -= written;
    }
    Check(CloseHandle(file) != 0, failure);
}

void WriteSphereBmp(const std::wstring& path, const std::vector<DWORD>& pixels,
                    int width, int height,
                    const char* failure = "write the sphere-only BMP frame") {
    const int rowBytes = (width * 3 + 3) & ~3;
    const DWORD imageBytes = static_cast<DWORD>(rowBytes) *
        static_cast<DWORD>(height);
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
    BITMAPINFOHEADER info{};
    info.biSize = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
    info.biWidth = width;
    info.biHeight = height;  // bottom-up, the BMP storage order
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = imageBytes;
    std::vector<unsigned char> bytes(
        static_cast<std::size_t>(fileHeader.bfOffBits) + imageBytes, 0);
    std::memcpy(bytes.data(), &fileHeader, sizeof(fileHeader));
    std::memcpy(bytes.data() + sizeof(fileHeader), &info, sizeof(info));
    unsigned char* image = bytes.data() + fileHeader.bfOffBits;
    for (int y = 0; y < height; ++y) {
        const DWORD* row = pixels.data() +
            static_cast<std::size_t>(height - 1 - y) * width;
        unsigned char* out = image + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < width; ++x) {
            const DWORD pixel = row[x];
            out[3 * x + 0] = static_cast<unsigned char>(pixel & 0xFF);
            out[3 * x + 1] = static_cast<unsigned char>(pixel >> 8 & 0xFF);
            out[3 * x + 2] = static_cast<unsigned char>(pixel >> 16 & 0xFF);
        }
    }
    WriteSphereFile(path, bytes.data(), bytes.size(), failure);
}

void AppendCaptureRecord(HANDLE handle, const std::string& line,
                         const char* openFailure, const char* writeFailure) {
    Check(handle != INVALID_HANDLE_VALUE, openFailure);
    std::string text = line;
    text += '\n';
    const char* cursor = text.data();
    std::size_t remaining = text.size();
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, 0x100000u));
        DWORD written = 0;
        Check(WriteFile(handle, cursor, chunk, &written, nullptr) != 0 &&
                  written == chunk,
              writeFailure);
        cursor += written;
        remaining -= written;
    }
}

void AppendSphereRecord(const std::string& line) {
    AppendCaptureRecord(g_sphereRecord, line,
                        "the sphere-only frame record is open",
                        "write the sphere-only frame record");
}

struct SphereColor { int red; int green; int blue; };
constexpr SphereColor kSphereWhite{255, 255, 255};
constexpr SphereColor kSphereRed{255, 0, 0};
constexpr SphereColor kSphereBlue{0, 0, 255};
// Valid TEXCOORD1 for the mode-3 main matrix and its single-layer controls.
// The 1x1 sphere textures are point sampled, so any in-range coordinate reads
// the one texel; the probe below replaces it with two real coordinates.
constexpr std::array<float, 4> kSphereUv1Center = {0.5f, 0.5f, 0.0f, 1.0f};
// Mode-3 TEXCOORD1 source probe: a real 2x1 texture of two different
// half-transparent texels. UV0 points at the other texel than UV1, so only a
// pass that really samples TEXCOORD1 can produce the expected known colour. The
// source-over expectations are computed from these texel constants over the
// white clear, never from a rendered frame.
constexpr wchar_t kSphereUv1Path[] = L"alpha_occlusion_sphere_uv1_source.bmp";
constexpr DWORD kSphereUv1Texels[2] = {0x8040C060u, 0x80C04080u};
constexpr SphereColor kSphereUv1Color0{0x40, 0xC0, 0x60};
constexpr SphereColor kSphereUv1Color1{0xC0, 0x40, 0x80};

// Double-precision source-over, the independent reference the device's fixed
// point blend is checked against. Each case spells out its own layer stack.
SphereColor SphereAlphaOver(SphereColor source, double alpha,
                            SphereColor destination) {
    const double inverse = 1.0 - alpha;
    return {
        static_cast<int>(std::lround(source.red * alpha +
                                     destination.red * inverse)),
        static_cast<int>(std::lround(source.green * alpha +
                                     destination.green * inverse)),
        static_cast<int>(std::lround(source.blue * alpha +
                                     destination.blue * inverse))};
}

SphereColor SphereNearOnlyExpected() {
    return SphereAlphaOver(kSphereRed, kSphereTexelAlpha, kSphereWhite);
}

SphereColor SphereFarOnlyExpected() {
    return SphereAlphaOver(kSphereBlue, kSphereTexelAlpha, kSphereWhite);
}

// Far blue first, then near red, both at 0x80/255 over the white clear.
SphereColor SphereDoubleLayerExpected() {
    return SphereAlphaOver(
        kSphereRed, kSphereTexelAlpha,
        SphereAlphaOver(kSphereBlue, kSphereTexelAlpha, kSphereWhite));
}

struct SphereCheck {
    bool passed = true;
    bool backgroundWhite = true;
    int maxError[2] = {0, 0};
    int failedPixels = 0;
};

struct SphereFrameResult {
    std::vector<DWORD> pixels;
    SphereCheck check;
    std::string bgraName;
    std::string bmpName;
};

SphereCheck CheckSpherePixels(const std::vector<DWORD>& pixels, int width,
                              int height, const int expected[3],
                              const char* submission) {
    SphereCheck check;
    const int backgroundPoints[6][2] = {
        {4, 4}, {width - 5, 4}, {4, height - 5},
        {width - 5, height - 5}, {width / 2, 4}, {4, height / 2}};
    for (const auto& point : backgroundPoints) {
        const DWORD pixel =
            pixels[static_cast<std::size_t>(point[1]) * width + point[0]];
        if ((pixel & 0xFFFFFFu) != 0xFFFFFFu) {
            check.backgroundWhite = false;
            break;
        }
    }
    if (!check.backgroundWhite) {
        std::fprintf(stderr,
                     "FAIL: %s background is not the white sentinel\n",
                     submission);
        check.passed = false;
        ++g_failures;
    }
    for (int roi = 0; roi < 2; ++roi) {
        int worstPixel = 0;
        int worstX = kSphereRois[roi].x;
        int worstY = kSphereRois[roi].y;
        bool roiPassed = true;
        for (int dy = -kSphereRoiRadius; dy <= kSphereRoiRadius; ++dy) {
            for (int dx = -kSphereRoiRadius; dx <= kSphereRoiRadius; ++dx) {
                const int x = kSphereRois[roi].x + dx;
                const int y = kSphereRois[roi].y + dy;
                const DWORD pixel =
                    pixels[static_cast<std::size_t>(y) * width + x];
                const int red = static_cast<int>(pixel >> 16 & 0xFF);
                const int green = static_cast<int>(pixel >> 8 & 0xFF);
                const int blue = static_cast<int>(pixel & 0xFF);
                const int error = std::max(
                    std::abs(red - expected[0]),
                    std::max(std::abs(green - expected[1]),
                             std::abs(blue - expected[2])));
                if (error > check.maxError[roi]) {
                    check.maxError[roi] = error;
                    worstPixel = static_cast<int>(pixel);
                    worstX = x;
                    worstY = y;
                }
                if (error > kSphereTolerance) {
                    roiPassed = false;
                    ++check.failedPixels;
                    ++g_failures;
                }
            }
        }
        if (!roiPassed) {
            check.passed = false;
            std::fprintf(
                stderr,
                "FAIL: %s roi=%d center=(%d,%d) worst=(%d,%d) pixel=%08lX "
                "expected=(%d,%d,%d) max-error=%d tolerance=%d\n",
                submission, roi, kSphereRois[roi].x, kSphereRois[roi].y,
                worstX, worstY,
                static_cast<unsigned long>(static_cast<DWORD>(worstPixel)),
                expected[0], expected[1], expected[2], check.maxError[roi],
                kSphereTolerance);
        }
    }
    return check;
}

void RecordSphereFrame(const char* caseName, int resource,
                       const char* precision, const char* order, int frame,
                       const int expected[3], const SphereCheck& check,
                       const std::string& bgraName,
                       const std::string& bmpName) {
    char line[1024];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"case\":\"%s\",\"sphereMode\":%d,\"resource\":%d,"
        "\"precision\":\"%s\","
        "\"order\":\"%s\",\"frame\":%d,\"expectedRGB\":[%d,%d,%d],"
        "\"width\":%d,\"height\":%d,\"roiMaxError\":[%d,%d],"
        "\"backgroundWhite\":%s,\"failedPixels\":%d,\"pass\":%s,"
        "\"bgra\":\"%s\",\"bmp\":\"%s\"}",
        caseName, g_sphereMode, resource, precision, order, frame, expected[0],
        expected[1],
        expected[2], kWidth, kHeight, check.maxError[0], check.maxError[1],
        check.backgroundWhite ? "true" : "false", check.failedPixels,
        check.passed ? "true" : "false", bgraName.c_str(), bmpName.c_str());
    Check(length > 0 && static_cast<std::size_t>(length) < sizeof(line),
          "format the sphere-only frame record");
    AppendSphereRecord(line);
}

void RecordSphereRestore(int resource, const char* order, bool frameRestored,
                         bool cpuIndicesUnchanged, const std::string& firstOff,
                         const std::string& finalOff) {
    char line[640];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"case\":\"restore\",\"sphereMode\":%d,\"resource\":%d,"
        "\"order\":\"%s\","
        "\"frameRGB\":\"%s\",\"cpuIndices\":\"%s\",\"firstOff\":\"%s\","
        "\"finalOff\":\"%s\"}",
        g_sphereMode, resource, order,
        frameRestored ? "restored" : "mismatch",
        cpuIndicesUnchanged ? "unchanged" : "changed", firstOff.c_str(),
        finalOff.c_str());
    Check(length > 0 && static_cast<std::size_t>(length) < sizeof(line),
          "format the sphere-only restore record");
    AppendSphereRecord(line);
}

SphereFrameResult RunSphereFrame(MMDApp* app, IDirect3DDevice9* device,
                                 IDirect3DSurface9* backbuffer,
                                 IDirect3DSurface9* readback, int resource,
                                 const D3DMATRIX& identity, Model* model,
                                 const char* caseName, const char* precision,
                                 const char* order, const int expected[3]) {
    SphereFrameResult result;
    D3DLOCKED_RECT lock{};
    RenderOneModelScene(app, device, backbuffer, readback, resource, identity,
                        model, &lock);
    // Copy the complete readback before the surface is unlocked, so every
    // check and every saved image comes from the same frame.
    result.pixels.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y)
        std::memcpy(
            result.pixels.data() + static_cast<std::size_t>(y) * kWidth,
            static_cast<const char*>(lock.pBits) +
                static_cast<std::size_t>(y) * lock.Pitch,
            static_cast<std::size_t>(kWidth) * sizeof(DWORD));
    Check(SUCCEEDED(readback->UnlockRect()), "unlock the sphere-only frame");
    const int frame = g_sphereFrameCount++;
    char stem[192];
    std::snprintf(stem, sizeof(stem), "m%d_%s_r%d_%s_%s_f%03d", g_sphereMode,
                  caseName, resource, precision, order, frame);
    result.bgraName = std::string(stem) + ".bgra";
    result.bmpName = std::string(stem) + ".bmp";
    // Both files are written even when the pixel check below fails.
    WriteSphereFile(SphereCapturePath(result.bgraName.c_str()),
                    result.pixels.data(),
                    result.pixels.size() * sizeof(DWORD),
                    "write the sphere-only BGRA frame");
    WriteSphereBmp(SphereCapturePath(result.bmpName.c_str()),
                   result.pixels, kWidth, kHeight);
    result.check = CheckSpherePixels(result.pixels, kWidth, kHeight, expected,
                                     stem);
    if (!result.check.passed)
        ++g_sphereFailedFrames;
    RecordSphereFrame(caseName, resource, precision, order, frame, expected,
                      result.check, result.bgraName, result.bmpName);
    std::printf(
        "sphere-only frame=%d sphereMode=%d case=%s resource=%d precision=%s "
        "order=%s expected=(%d,%d,%d) roi0=%d roi1=%d background=%d pass=%d\n",
        frame, g_sphereMode, caseName, resource, precision, order, expected[0],
        expected[1],
        expected[2], result.check.maxError[0], result.check.maxError[1],
        result.check.backgroundWhite ? 1 : 0, result.check.passed ? 1 : 0);
    return result;
}

struct SphereTextures {
    // Named nearTexture/farTexture: Windows' minwindef.h defines the empty
    // macros near and far, so those two identifiers cannot be member names.
    IDirect3DTexture9* nearTexture = nullptr;
    IDirect3DTexture9* farTexture = nullptr;
};

// The tag comes from the production CacheTextureColorAndAlpha measurement of
// the uploaded texels, not from a hand-written classification. The 1x1
// textures are point sampled at every coordinate the sphere cascade produces.
SphereTextures PublishSphereTextures(MMDApp* app, IDirect3DDevice9* device,
                                     DWORD nearTexel, DWORD farTexel,
                                     bool softAlpha) {
    SphereTextures textures;
    textures.nearTexture = CreateTexture(device, &nearTexel, 1, 1,
                                         "create the sphere near texture");
    textures.farTexture = CreateTexture(device, &farTexel, 1, 1,
                                        "create the sphere far texture");
    unsigned char nearTag[4]{};
    unsigned char farTag[4]{};
    CacheTextureColorAndAlpha(textures.nearTexture, nearTag);
    CacheTextureColorAndAlpha(textures.farTexture, farTag);
    const unsigned char expectedFlags =
        static_cast<unsigned char>(kTextureAlphaKnown |
                                   (softAlpha ? kTextureUsesAlpha : 0));
    Check(nearTag[3] == expectedFlags,
          "the measured sphere near texel alpha classifies as expected");
    Check(farTag[3] == expectedFlags,
          "the measured sphere far texel alpha classifies as expected");
    Check(nearTag[0] == 0xFF && nearTag[1] == 0 && nearTag[2] == 0,
          "the measured sphere near texel colour is red");
    Check(farTag[0] == 0 && farTag[1] == 0 && farTag[2] == 0xFF,
          "the measured sphere far texel colour is blue");
    PublishFixtureTexture(app->Renderer(), kSphereNearSlot, kSphereNearPath,
                          textures.nearTexture, nearTag);
    PublishFixtureTexture(app->Renderer(), kSphereFarSlot, kSphereFarPath,
                          textures.farTexture, farTag);
    return textures;
}

void ReleaseSphereTextures(MMDApp* app, SphereTextures& textures) {
    auto* renderer = app->Renderer();
    app->ModelSlot(0) = nullptr;
    app->ModelSlot(1) = nullptr;
    ReleaseFixtureTexture(renderer, kSphereFarSlot, textures.farTexture);
    ReleaseFixtureTexture(renderer, kSphereNearSlot, textures.nearTexture);
    textures = {};
    Check(renderer->resourcePool[kSphereNearSlot].heapBuffer == nullptr &&
              renderer->resourcePool[kSphereNearSlot].comObject == nullptr &&
              renderer->resourcePool[kSphereFarSlot].heapBuffer == nullptr &&
              renderer->resourcePool[kSphereFarSlot].comObject == nullptr,
          "the sphere fixture releases both pool slots without a dangling path");
}

// The additive sphere cascade adds the sphere texel to the material colour. A
// white base would saturate every channel and hide the layer order, so the
// mode-2 fixture keeps the alpha at 1 but zeroes the base colour channels of
// the materials that really add a sphere. Modes 1 and 3 keep the white base
// their multiplicative path samples. SphereMode 0 controls never reach this
// helper, so they also keep the original white base and their white pixel.
void ApplySphereMaterialMode(mdl::ModelMaterialRecord& material,
                             int sphereMode) {
    if (sphereMode == 2) {
        for (int channel = 0; channel < 3; ++channel) {
            material.diffuse[channel] = 0.0f;
            material.diffuseMirror[channel] = 0.0f;
            material.ambient[channel] = 0.0f;
        }
        Check(material.diffuse[3] == 1.0f && material.specular[0] == 0.0f &&
                  material.specular[1] == 0.0f &&
                  material.specular[2] == 0.0f,
              "mode 2 changes the base colour only, not the alpha or specular");
        Check(material.diffuse[0] == 0.0f && material.diffuse[1] == 0.0f &&
                  material.diffuse[2] == 0.0f &&
                  material.diffuseMirror[0] == 0.0f &&
                  material.diffuseMirror[1] == 0.0f &&
                  material.diffuseMirror[2] == 0.0f &&
                  material.ambient[0] == 0.0f && material.ambient[1] == 0.0f &&
                  material.ambient[2] == 0.0f,
              "mode 2 zeroes the base colour so the additive sphere is not "
              "saturated by a white base");
        return;
    }
    Check(material.diffuse[0] == 1.0f && material.diffuse[1] == 1.0f &&
              material.diffuse[2] == 1.0f && material.diffuseMirror[0] == 1.0f &&
              material.diffuseMirror[1] == 1.0f &&
              material.diffuseMirror[2] == 1.0f && material.ambient[0] == 1.0f &&
              material.ambient[1] == 1.0f && material.ambient[2] == 1.0f,
          "the white base a multiplicative sphere samples is unchanged");
}

// One PMX model with two material quads. farFirst swaps both the material
// record order and the index ranges together, so the scene is identical and
// only the authoring order differs. baseTexture moves the same half alpha to
// the base texture path and leaves the sphere path empty. sphereMode is the
// selected native PMX mode; mode 3 also builds the 48-byte vertex with a
// float4 TEXCOORD1.
std::unique_ptr<Model> BuildSphereTwoMaterialModel(
    IDirect3DDevice9* device, const std::vector<ModelVertex>& vertices,
    const std::vector<WORD>& nearFirstIndices, bool farFirst,
    bool sphereEnabled, bool baseTexture, int sphereMode) {
    const bool usesSphere = !baseTexture && sphereEnabled;
    const int materialMode = usesSphere ? sphereMode : 0;
    Check(materialMode == 0 ||
              (materialMode >= kSphereModeMin && materialMode <= kSphereModeMax),
          "the sphere fixture material selects 0 or a supported sphereMode");
    std::vector<WORD> indices;
    std::vector<MaterialSpec> specs;
    const wchar_t* nearPath = baseTexture ? kSphereNearPath : L"";
    const wchar_t* farPath = baseTexture ? kSphereFarPath : L"";
    if (!farFirst) {
        indices = nearFirstIndices;
        specs = {{nearPath, 1.0f, 0, 6}, {farPath, 1.0f, 6, 6}};
    } else {
        indices.assign(nearFirstIndices.begin() + 6, nearFirstIndices.end());
        indices.insert(indices.end(), nearFirstIndices.begin(),
                       nearFirstIndices.begin() + 6);
        specs = {{farPath, 1.0f, 0, 6}, {nearPath, 1.0f, 6, 6}};
    }
    // Only the mode-3 scene needs the extra UV set; all other modes keep the
    // plain 274/32 vertices every other fixture builds.
    std::vector<std::array<float, 4>> additionalUv1;
    if (materialMode == 3) {
        additionalUv1.assign(vertices.size(), kSphereUv1Center);
    }
    auto model = BuildModel(device, vertices, indices, specs, true,
                            materialMode == 3 ? &additionalUv1 : nullptr);
    Check(model->materials.size() == 2, "the sphere fixture has two materials");
    const wchar_t* const paths[2] = {
        farFirst ? kSphereFarPath : kSphereNearPath,
        farFirst ? kSphereNearPath : kSphereFarPath};
    for (std::size_t i = 0; i < model->materials.size(); ++i) {
        mdl::ModelMaterialRecord& material = model->materials[i];
        Check(material.diffuse[3] == 1.0f,
              "the sphere fixture material diffuse alpha is 1");
        if (baseTexture) {
            Check(std::wstring(material.texturePath) == paths[i],
                  "the base-texture control names the same texel on the base path");
            Check(material.spherePath[0] == L'\0',
                  "the base-texture control leaves the sphere path empty");
            material.sphereMode = 0;
        } else {
            Check(material.texturePath[0] == L'\0',
                  "the sphere fixture keeps the base texture path empty");
            wcscpy_s(material.spherePath, paths[i]);
            material.sphereMode = static_cast<std::uint8_t>(materialMode);
            Check(std::wstring(material.spherePath) == paths[i],
                  "the sphere fixture material names the sphere texture");
        }
        Check(material.sphereMode == static_cast<std::uint8_t>(materialMode),
              "the sphere fixture material selects the requested sphereMode");
        if (usesSphere) ApplySphereMaterialMode(material, sphereMode);
    }
    Check(model->record->pmxAdditionalUvCount ==
              static_cast<unsigned>(materialMode == 3 ? 1 : 0),
          "the two-material scene uses the vertex format its mode needs");
    Check(model->record->postLoadFlag2 == 0,
          "the sphere fixture record has postLoadFlag2 0");
    Check(model->record->displayState == 0,
          "the sphere fixture record has displayState 0");
    Check(model->morphAdd.size() == model->materials.size() + 1 &&
              model->morphMul.size() == model->materials.size() + 1,
          "the PMX sphere fixture carries one morph pool per material");
    // BuildModel fills the multiplicative pools with 1 and leaves the additive
    // pools zeroed; read them back so the fixture proves both identities.
    for (const auto& pool : model->morphMul) {
        for (int channel = 0; channel < 4; ++channel)
            Check(pool.channels.diffuse[channel] == 1.0f &&
                      pool.channels.textureTint[channel] == 1.0f &&
                      pool.channels.sphereTint[channel] == 1.0f &&
                      pool.channels.toonTint[channel] == 1.0f,
                  "the sphere fixture multiplicative material morphs are 1");
    }
    for (const auto& pool : model->morphAdd) {
        for (int channel = 0; channel < 4; ++channel)
            Check(pool.channels.diffuse[channel] == 0.0f &&
                      pool.channels.textureTint[channel] == 0.0f &&
                      pool.channels.sphereTint[channel] == 0.0f &&
                      pool.channels.toonTint[channel] == 0.0f,
                  "the sphere fixture additive material morphs are 0");
    }
    return model;
}

// One quad that samples its sphere texture, for the single-layer controls and
// the mode-3 UV-source probe. u0/v0 drive the base texture coordinate set;
// uv1 carries the TEXCOORD1 value of the mode-3 vertex and is ignored by the
// other modes.
std::unique_ptr<Model> BuildSphereSingleLayerModel(
    IDirect3DDevice9* device, float z, const wchar_t* spherePath,
    int sphereMode, float u0 = 0.5f, float v0 = 0.5f,
    const std::array<float, 4>* uv1 = nullptr) {
    Check(sphereMode >= 0 && sphereMode <= kSphereModeMax,
          "the single-layer sphere control selects a supported sphereMode");
    Check(sphereMode == 3 || uv1 == nullptr,
          "only the mode-3 single-layer fixture carries TEXCOORD1");
    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    AppendQuad(vertices, indices, kSphereLeft, kSphereTop, kSphereRight,
               kSphereBottom, z, u0, v0);
    std::vector<std::array<float, 4>> additionalUv1;
    if (sphereMode == 3) {
        additionalUv1.assign(vertices.size(),
                             uv1 != nullptr ? *uv1 : kSphereUv1Center);
    }
    auto model = BuildModel(device, vertices, indices,
                            {{L"", 1.0f, 0, indices.size()}}, true,
                            sphereMode == 3 ? &additionalUv1 : nullptr);
    mdl::ModelMaterialRecord& material = model->materials[0];
    Check(material.texturePath[0] == L'\0',
          "the single-layer sphere control keeps the base texture path empty");
    wcscpy_s(material.spherePath, spherePath);
    material.sphereMode = static_cast<std::uint8_t>(sphereMode);
    Check(material.sphereMode == static_cast<std::uint8_t>(sphereMode),
          "the single-layer sphere control selects the requested sphereMode");
    // The helper checks the base colour the selected multiplicative or
    // additive mode expects.
    ApplySphereMaterialMode(material, sphereMode);
    Check(model->record->pmxAdditionalUvCount ==
              static_cast<unsigned>(sphereMode == 3 ? 1 : 0),
          "the single-layer scene uses the vertex format its mode needs");
    Check(model->record->postLoadFlag2 == 0 && model->record->displayState == 0,
          "the single-layer sphere control record state");
    return model;
}

void RunSphereMainMatrix(MMDApp* app, IDirect3DDevice9* device,
                         IDirect3DSurface9* backbuffer,
                         IDirect3DSurface9* readback, int resource,
                         const D3DMATRIX& identity,
                         const std::vector<ModelVertex>& vertices,
                         const std::vector<WORD>& nearFirstIndices) {
    std::unique_ptr<Model> models[2];
    models[0] = BuildSphereTwoMaterialModel(
        device, vertices, nearFirstIndices, false, true, false, g_sphereMode);
    models[1] = BuildSphereTwoMaterialModel(
        device, vertices, nearFirstIndices, true, true, false, g_sphereMode);
    for (int order = 0; order < 2; ++order) {
        Check(models[order]->record->pmxAdditionalUvCount ==
                  static_cast<unsigned>(g_sphereMode == 3 ? 1 : 0),
              "the main matrix builds the vertex format of the selected mode");
    }
    std::vector<std::int32_t> cpuBefore[2];
    for (int order = 0; order < 2; ++order)
        cpuBefore[order] = models[order]->cpuPmxIndices;
    for (int order = 0; order < 2; ++order) {
        std::string firstOff;
        std::string finalOff;
        std::vector<DWORD> firstOffPixels;
        bool restored = false;
        for (int step = 0; step < kSphereMainStepCount; ++step) {
            const bool off = step == 0 || step == kSphereMainStepCount - 1;
            const bool nearFirst = order == 0;
            // Off keeps the authored order and the depth write: near-first
            // leaves only the near layer, far-first blends both layers. Every
            // enabled tier sorts far to near, so both orders blend both.
            // All three modes reach the same independent reference: mode 1 and
            // mode 3 multiply a white base by the sphere texel, and mode 2's
            // base is black so its additive sphere texel is the whole colour.
            // The blend alpha is the sphere texel's in every mode, so the
            // source-over arithmetic below is the same.
            const SphereColor expectedColor =
                off && nearFirst ? SphereNearOnlyExpected()
                                 : SphereDoubleLayerExpected();
            const int expected[3] = {expectedColor.red, expectedColor.green,
                                     expectedColor.blue};
            app->TransparentMaterialSortPrecision() = kSpherePrecisions[step];
            const SphereFrameResult frame = RunSphereFrame(
                app, device, backbuffer, readback, resource, identity,
                models[order].get(), "main", kSpherePrecisionNames[step],
                kSphereOrderNames[order], expected);
            if (step == 0) {
                firstOffPixels = frame.pixels;
                firstOff = frame.bmpName;
            }
            if (step == kSphereMainStepCount - 1) {
                finalOff = frame.bmpName;
                restored = frame.pixels.size() == firstOffPixels.size();
                if (restored) {
                    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
                        if ((frame.pixels[i] & 0xFFFFFFu) !=
                            (firstOffPixels[i] & 0xFFFFFFu)) {
                            restored = false;
                            break;
                        }
                    }
                }
                if (!restored) {
                    std::fprintf(stderr,
                                 "FAIL: the final Off frame did not restore the "
                                 "first Off frame, resource=%d order=%s\n",
                                 resource, kSphereOrderNames[order]);
                    ++g_failures;
                }
            }
        }
        // The sorter rewrites the GPU index buffer only; the authored CPU
        // source array is the invariant compared here.
        const bool cpuUnchanged = models[order]->cpuPmxIndices == cpuBefore[order];
        if (!cpuUnchanged) {
            std::fprintf(stderr,
                         "FAIL: the sphere fixture CPU indices changed, "
                         "resource=%d order=%s\n",
                         resource, kSphereOrderNames[order]);
            ++g_failures;
        }
        RecordSphereRestore(resource, kSphereOrderNames[order], restored,
                            cpuUnchanged, firstOff, finalOff);
        std::printf(
            "sphere-only restore resource=%d order=%s frameRGB=%s "
            "cpuIndices=%s\n",
            resource, kSphereOrderNames[order],
            restored ? "restored" : "mismatch",
            cpuUnchanged ? "unchanged" : "changed");
    }
    app->ModelSlot(0) = nullptr;
    app->ModelSlot(1) = nullptr;
}

void RunSphereControlScenes(MMDApp* app, IDirect3DDevice9* device,
                            IDirect3DSurface9* backbuffer,
                            IDirect3DSurface9* readback, int resource,
                            const D3DMATRIX& identity,
                            const std::vector<ModelVertex>& vertices,
                            const std::vector<WORD>& nearFirstIndices) {
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    {
        const SphereColor color = SphereNearOnlyExpected();
        const int expected[3] = {color.red, color.green, color.blue};
        auto model = BuildSphereSingleLayerModel(device, kNearDepth,
                                                 kSphereNearPath, g_sphereMode);
        RunSphereFrame(app, device, backbuffer, readback, resource, identity,
                       model.get(), "near-only", "off", "single", expected);
        app->ModelSlot(0) = nullptr;
    }
    {
        const SphereColor color = SphereFarOnlyExpected();
        const int expected[3] = {color.red, color.green, color.blue};
        auto model = BuildSphereSingleLayerModel(device, kFarDepth,
                                                 kSphereFarPath, g_sphereMode);
        RunSphereFrame(app, device, backbuffer, readback, resource, identity,
                       model.get(), "far-only", "off", "single", expected);
        app->ModelSlot(0) = nullptr;
    }
    // sphereMode 0 gates the sphere cascade off: no texture reaches the
    // surface, so the white light and the white toon leave the clear colour.
    {
        const int expected[3] = {255, 255, 255};
        for (int order = 0; order < 2; ++order) {
            auto model = BuildSphereTwoMaterialModel(
                device, vertices, nearFirstIndices, order == 1, false, false,
                g_sphereMode);
            RunSphereFrame(app, device, backbuffer, readback, resource,
                           identity, model.get(), "sphere-mode0", "off",
                           kSphereOrderNames[order], expected);
            app->ModelSlot(0) = nullptr;
        }
    }
    // The same half alpha moved to the base texture path: classification and
    // the sorted far-to-near order must produce the identical two-layer pixel.
    {
        const SphereColor color = SphereDoubleLayerExpected();
        const int expected[3] = {color.red, color.green, color.blue};
        app->TransparentMaterialSortPrecision() =
            TransparentSortPrecision::Standard;
        for (int order = 0; order < 2; ++order) {
            auto model = BuildSphereTwoMaterialModel(
                device, vertices, nearFirstIndices, order == 1, false, true, 0);
            RunSphereFrame(app, device, backbuffer, readback, resource,
                           identity, model.get(), "base-alpha", "standard",
                           kSphereOrderNames[order], expected);
            app->ModelSlot(0) = nullptr;
        }
    }
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
}

void RunSphereOpaqueAlphaControl(MMDApp* app, IDirect3DDevice9* device,
                                 IDirect3DSurface9* backbuffer,
                                 IDirect3DSurface9* readback, int resource,
                                 const D3DMATRIX& identity,
                                 const std::vector<ModelVertex>& vertices,
                                 const std::vector<WORD>& nearFirstIndices) {
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    // Sphere alpha 255 makes each layer opaque, so the near red layer owns
    // every sampled pixel after the depth test in both authoring orders.
    const SphereColor color = SphereAlphaOver(kSphereRed, 1.0, kSphereRed);
    const int expected[3] = {color.red, color.green, color.blue};
    for (int order = 0; order < 2; ++order) {
        auto model = BuildSphereTwoMaterialModel(
            device, vertices, nearFirstIndices, order == 1, true, false,
            g_sphereMode);
        RunSphereFrame(app, device, backbuffer, readback, resource, identity,
                       model.get(), "sphere-alpha255", "off",
                       kSphereOrderNames[order], expected);
        app->ModelSlot(0) = nullptr;
    }
}

// --sphere-mode 3 samples the sphere texture through TEXCOORD1. A 1x1 texture
// cannot prove which coordinate set the pass really used, so this probe owns a
// real 2x1 texture with two different half-transparent texels, gives the quad a
// UV0 that points at the other texel than UV1, and renders three frames: one
// per TEXCOORD1 texel, then the first frame again with UV0 moved. The two known
// colours come from the texel constants with the same source-over reference the
// main matrix uses, never from a rendered frame; the moved-UV0 frame must be
// pixel-identical to the first one.
void RunSphereUv1SourceProbe(MMDApp* app, IDirect3DDevice9* device,
                             IDirect3DSurface9* backbuffer,
                             IDirect3DSurface9* readback, int resource,
                             const D3DMATRIX& identity) {
    auto* renderer = app->Renderer();
    IDirect3DTexture9* texture = CreateTexture(
        device, kSphereUv1Texels, 2, 1,
        "create the mode-3 TEXCOORD1 source texture");
    unsigned char tag[4]{};
    CacheTextureColorAndAlpha(texture, tag);
    Check((tag[3] & kTextureUsesAlpha) != 0,
          "the TEXCOORD1 source texture carries half-transparent texels");
    PublishFixtureTexture(renderer, kSphereUv1Slot, kSphereUv1Path, texture,
                          tag);
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    const SphereColor expectedForTexel[2] = {
        SphereAlphaOver(kSphereUv1Color0, kSphereTexelAlpha, kSphereWhite),
        SphereAlphaOver(kSphereUv1Color1, kSphereTexelAlpha, kSphereWhite)};
    // u0 deliberately names the other texel than u1 in the first two frames.
    const auto render = [&](float u0, float u1, const char* caseName,
                            int texel) {
        const int expected[3] = {expectedForTexel[texel].red,
                                 expectedForTexel[texel].green,
                                 expectedForTexel[texel].blue};
        const std::array<float, 4> uv1 = {u1, 0.5f, 0.0f, 1.0f};
        auto model = BuildSphereSingleLayerModel(
            device, kNearDepth, kSphereUv1Path, 3, u0, 0.5f, &uv1);
        const SphereFrameResult frame = RunSphereFrame(
            app, device, backbuffer, readback, resource, identity, model.get(),
            caseName, "off", "single", expected);
        app->ModelSlot(0) = nullptr;
        return frame;
    };
    const SphereFrameResult texel0 =
        render(0.75f, 0.25f, "uv1-source-texel0", 0);
    const SphereFrameResult texel1 =
        render(0.25f, 0.75f, "uv1-source-texel1", 1);
    const SphereFrameResult movedUv0 =
        render(0.25f, 0.25f, "uv1-source-uv0-moved", 0);
    // Both frames of the pair above sample a different known colour, so the
    // probe proves TEXCOORD1 is the sampled coordinate; the moved UV0 frame
    // repeats the first frame although its base coordinate changed.
    Check(texel0.pixels != texel1.pixels,
          "the two TEXCOORD1 texels really produce different colours");
    Check(texel0.pixels == movedUv0.pixels,
          "moving UV0 leaves the mode-3 sphere sample unchanged");
    ReleaseFixtureTexture(renderer, kSphereUv1Slot, texture);
    app->ModelSlot(0) = nullptr;
    std::printf("sphere-only TEXCOORD1 probe resource=%d frames=3 "
                "texel0=(%d,%d,%d) texel1=(%d,%d,%d) uv0-moved=identical\n",
                resource, expectedForTexel[0].red, expectedForTexel[0].green,
                expectedForTexel[0].blue, expectedForTexel[1].red,
                expectedForTexel[1].green, expectedForTexel[1].blue);
}

void RunSphereOnlyResource(MMDApp* app, IDirect3DDevice9* device,
                           IDirect3DSurface9* backbuffer,
                           IDirect3DSurface9* readback, int resource,
                           const D3DMATRIX& identity) {
    D3DSURFACE_DESC target{};
    Check(SUCCEEDED(backbuffer->GetDesc(&target)) && target.Width == kWidth &&
              target.Height == kHeight,
          "the sphere-only capture target is 96x96");
    D3DMATRIX world{};
    Check(SUCCEEDED(device->GetTransform(D3DTS_WORLD, &world)) &&
              std::memcmp(&world, &identity, sizeof(world)) == 0,
          "the sphere-only frames use the identity world matrix");
    const auto savedPrecision = app->TransparentMaterialSortPrecision();
    const bool savedCrossObjects = app->TransparentSortCrossObjects();
    app->TransparentSortCrossObjects() = false;
    Check(!app->TransparentSortCrossObjects(),
          "sphere-only frames do not sort across objects");
    Check(g_resolveTarget == nullptr,
          "sphere-only frames render without multisampling");
    // The capture pixels are defined by unorm reads and writes with point
    // sampling, so neither side of the blend may take an sRGB conversion.
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    for (DWORD stage = 0; stage < 3; ++stage) {
        device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    // The layers differ only by depth; Off relies on the near layer's depth
    // write to reject the far layer, so the depth state is fixed here.
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    AppendQuad(vertices, indices, kSphereLeft, kSphereTop, kSphereRight,
               kSphereBottom, kNearDepth, 0.5f, 0.5f);
    AppendQuad(vertices, indices, kSphereLeft, kSphereTop, kSphereRight,
               kSphereBottom, kFarDepth, 0.5f, 0.5f);
    Check(vertices.size() == 8 && indices.size() == 12,
          "the sphere fixture has two overlapping quads");

    SphereTextures soft = PublishSphereTextures(
        app, device, kSphereNearTexel, kSphereFarTexel, true);
    RunSphereMainMatrix(app, device, backbuffer, readback, resource, identity,
                        vertices, indices);
    RunSphereControlScenes(app, device, backbuffer, readback, resource,
                           identity, vertices, indices);
    // Mode 3 alone needs the TEXCOORD1 source evidence; its own slot and path
    // leave the near/far matrix above untouched.
    if (g_sphereMode == 3)
        RunSphereUv1SourceProbe(app, device, backbuffer, readback, resource,
                                identity);
    ReleaseSphereTextures(app, soft);

    SphereTextures opaque = PublishSphereTextures(
        app, device, kSphereNearOpaqueTexel, kSphereFarOpaqueTexel, false);
    RunSphereOpaqueAlphaControl(app, device, backbuffer, readback, resource,
                                identity, vertices, indices);
    ReleaseSphereTextures(app, opaque);

    app->TransparentMaterialSortPrecision() = savedPrecision;
    app->TransparentSortCrossObjects() = savedCrossObjects;
    app->ClearModelSlots();
    std::printf("sphere-only resource=%d frames=%d failures=%d\n", resource,
                g_sphereFrameCount, g_failures);
}


// ---- --cross-object-only: independent two-model cross-object capture --------
//
// Two genuinely separate ModelRecords, one inset quad each. The near model
// samples the two-layer front texel (ARGB 0x8080A040) at z .25 and the far model
// the rear texel (ARGB 0x804060C0) at z .75; both quads span [-.75,.75] and
// sample texel 0 of the already published 2-wide two-layer textures at u .25,
// v .5. Both materials keep Diffuse.a at 1, so the texel alpha alone decides
// the blend. The author order only decides which record is submitted first; the
// scene never stands in one two-material model for the pair.
//
// The expected pixel is an independent source-over over the white clear,
// evaluated per layer with SphereAlphaOver from the known texel colours and the
// texel alpha 128/255. With precision Off or cross-object sorting disabled the
// submitted order survives, so only near-first keeps just the near layer; every
// other combination blends the far layer and then the near layer. The sphere
// capture's two 5x5 ROIs, 2-unit tolerance and white background sentinel apply
// unchanged. Every frame is saved as BGRA and BMP and recorded in
// cross_object_frames.jsonl whether or not its pixels pass.
constexpr int kCrossObjectResourceCount = 3;  // fixed, 117 (SM2), 118 (SM3)
constexpr int kCrossObjectFormatCount = 2;    // PMD then PMX
constexpr int kCrossObjectOrderCount = 2;     // near-first then far-first
constexpr int kCrossObjectScopeCount = 2;     // cross-object sorting off then on
constexpr int kCrossObjectPrecisionStepCount = kSphereMainStepCount;
constexpr int kCrossObjectToggleStepCount = 3;  // false -> true -> false
static_assert(kSphereMainStepCount == 6,
              "the cross-object precision round is Off -> ... -> Off");
static_assert(kSphereResourceCount == kCrossObjectResourceCount,
              "the cross-object capture runs on the same three resources");
constexpr int kCrossObjectPrecisionFramesPerFormat =
    kCrossObjectOrderCount * kCrossObjectScopeCount *
    kCrossObjectPrecisionStepCount;  // 24
constexpr int kCrossObjectToggleFramesPerFormat =
    kCrossObjectOrderCount * kCrossObjectToggleStepCount;  // 6
constexpr int kCrossObjectFramesPerResource =
    kCrossObjectFormatCount * (kCrossObjectPrecisionFramesPerFormat +
                               kCrossObjectToggleFramesPerFormat);  // 60
constexpr int kCrossObjectExpectedFrameCount =
    kCrossObjectFramesPerResource * kCrossObjectResourceCount;  // 180
constexpr int kCrossObjectPrecisionRestoreCount =
    kCrossObjectFormatCount * kCrossObjectOrderCount *
    kCrossObjectScopeCount * kCrossObjectResourceCount;  // 24
constexpr int kCrossObjectScopeToggleRestoreCount =
    kCrossObjectFormatCount * kCrossObjectOrderCount *
    kCrossObjectResourceCount;  // 12

constexpr float kCrossObjectLeft = -0.75f;
constexpr float kCrossObjectTop = 0.75f;
constexpr float kCrossObjectRight = 0.75f;
constexpr float kCrossObjectBottom = -0.75f;
constexpr float kCrossObjectSamplerU = 0.25f;
constexpr float kCrossObjectSamplerV = 0.5f;
const char* const kCrossObjectFormatNames[kCrossObjectFormatCount] = {"pmd",
                                                                     "pmx"};

// The known texels come from the constants the published textures are built
// from; the expected colours are derived here, never read back from a frame.
constexpr DWORD kCrossObjectNearTexel = kTwoLayerFrontTexel;
constexpr DWORD kCrossObjectFarTexel = kTwoLayerRearTexel;
static_assert(((kCrossObjectNearTexel >> 24) & 0xFFu) == 0x80u,
              "the near cross-object texel keeps alpha 128");
static_assert(((kCrossObjectFarTexel >> 24) & 0xFFu) == 0x80u,
              "the far cross-object texel keeps alpha 128");
constexpr double kCrossObjectTexelAlpha = 128.0 / 255.0;

constexpr SphereColor CrossObjectTexelColor(DWORD texel) {
    return {static_cast<int>((texel >> 16) & 0xFFu),
            static_cast<int>((texel >> 8) & 0xFFu),
            static_cast<int>(texel & 0xFFu)};
}

SphereColor CrossObjectNearOnlyExpected() {
    return SphereAlphaOver(CrossObjectTexelColor(kCrossObjectNearTexel),
                           kCrossObjectTexelAlpha, kSphereWhite);
}

SphereColor CrossObjectDoubleLayerExpected() {
    return SphereAlphaOver(
        CrossObjectTexelColor(kCrossObjectNearTexel), kCrossObjectTexelAlpha,
        SphereAlphaOver(CrossObjectTexelColor(kCrossObjectFarTexel),
                        kCrossObjectTexelAlpha, kSphereWhite));
}

std::wstring g_crossObjectCaptureDir;
HANDLE g_crossObjectRecord = INVALID_HANDLE_VALUE;
int g_crossObjectFrameCount = 0;
int g_crossObjectFailedFrames = 0;
int g_crossObjectPrecisionRestores = 0;
int g_crossObjectScopeToggleRestores = 0;

bool CrossObjectFullRgbEqual(const std::vector<DWORD>& first,
                             const std::vector<DWORD>& final) {
    if (first.size() != final.size())
        return false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if ((first[i] & 0xFFFFFFu) != (final[i] & 0xFFFFFFu))
            return false;
    return true;
}

struct CrossObjectFrameResult {
    std::vector<DWORD> pixels;
    SphereCheck check;
    std::string bgraName;
    std::string bmpName;
};

void RecordCrossObjectFrame(const char* caseName, const char* formatName,
                            int resource, const char* precision,
                            bool crossObjects, const char* order, int step,
                            int frame, const int expected[3],
                            const SphereCheck& check,
                            const std::string& bgraName,
                            const std::string& bmpName) {
    char line[2048];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"case\":\"%s\",\"format\":\"%s\",\"resource\":%d,"
        "\"precision\":\"%s\",\"crossObjects\":%s,\"order\":\"%s\","
        "\"step\":%d,\"frame\":%d,\"width\":%d,\"height\":%d,"
        "\"expectedRGB\":[%d,%d,%d],\"roiMaxError\":[%d,%d],"
        "\"backgroundWhite\":%s,\"failedPixels\":%d,\"pass\":%s,"
        "\"bgra\":\"%s\",\"bmp\":\"%s\"}",
        caseName, formatName, resource, precision,
        crossObjects ? "true" : "false", order, step, frame, kWidth, kHeight,
        expected[0], expected[1], expected[2], check.maxError[0],
        check.maxError[1], check.backgroundWhite ? "true" : "false",
        check.failedPixels, check.passed ? "true" : "false",
        bgraName.c_str(), bmpName.c_str());
    Check(length > 0 && static_cast<std::size_t>(length) < sizeof(line),
          "format the cross-object frame record");
    AppendCaptureRecord(g_crossObjectRecord, line,
                        "the cross-object frame record is open",
                        "write the cross-object frame record");
}

// The cpuIndices field in both restore records reports the four-vector check:
// each model's cpuIndices and cpuPmxIndices compared with its own initial value.
void RecordCrossObjectPrecisionRestore(const char* formatName, int resource,
                                       const char* order, bool crossObjects,
                                       bool frameRestored,
                                       bool cpuIndicesUnchanged,
                                       const std::string& firstOff,
                                       const std::string& finalOff) {
    char line[2048];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"case\":\"precision-restore\",\"format\":\"%s\",\"resource\":%d,"
        "\"order\":\"%s\",\"crossObjects\":%s,\"precision\":\"off\","
        "\"frameRGB\":\"%s\",\"cpuIndices\":\"%s\",\"firstOff\":\"%s\","
        "\"finalOff\":\"%s\"}",
        formatName, resource, order, crossObjects ? "true" : "false",
        frameRestored ? "restored" : "mismatch",
        cpuIndicesUnchanged ? "unchanged" : "changed", firstOff.c_str(),
        finalOff.c_str());
    Check(length > 0 && static_cast<std::size_t>(length) < sizeof(line),
          "format the cross-object precision restore record");
    AppendCaptureRecord(g_crossObjectRecord, line,
                        "the cross-object frame record is open",
                        "write the cross-object frame record");
}

void RecordCrossObjectScopeRestore(const char* formatName, int resource,
                                   const char* order, bool frameRestored,
                                   bool cpuIndicesUnchanged,
                                   const std::string& firstFrame,
                                   const std::string& finalFrame) {
    char line[2048];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"case\":\"scope-restore\",\"format\":\"%s\",\"resource\":%d,"
        "\"order\":\"%s\",\"precision\":\"standard\","
        "\"scope\":\"false-true-false\",\"frameRGB\":\"%s\","
        "\"cpuIndices\":\"%s\",\"firstScope\":\"%s\","
        "\"finalScope\":\"%s\"}",
        formatName, resource, order,
        frameRestored ? "restored" : "mismatch",
        cpuIndicesUnchanged ? "unchanged" : "changed", firstFrame.c_str(),
        finalFrame.c_str());
    Check(length > 0 && static_cast<std::size_t>(length) < sizeof(line),
          "format the cross-object scope restore record");
    AppendCaptureRecord(g_crossObjectRecord, line,
                        "the cross-object frame record is open",
                        "write the cross-object frame record");
}

std::unique_ptr<Model> BuildCrossObjectModel(IDirect3DDevice9* device, float z,
                                             const wchar_t* texturePath,
                                             bool pmx) {
    std::vector<ModelVertex> vertices;
    std::vector<WORD> indices;
    AppendQuad(vertices, indices, kCrossObjectLeft, kCrossObjectTop,
               kCrossObjectRight, kCrossObjectBottom, z,
               kCrossObjectSamplerU, kCrossObjectSamplerV);
    Check(vertices.size() == 4 && indices.size() == 6,
          "the cross-object fixture model is one inset quad");
    auto model = BuildModel(device, vertices, indices,
                            {{texturePath, 1.0f, 0, indices.size()}}, pmx);
    Check(model->record != nullptr && model->materials.size() == 1,
          "the cross-object model record is independent with one material");
    Check(std::wstring(model->materials[0].texturePath) == texturePath,
          "the cross-object model names its own texel texture");
    Check(model->materials[0].diffuse[3] == 1.0f,
          "the cross-object material diffuse alpha is 1");
    Check(model->record->physicsMode == (pmx ? 2 : 0),
          "the cross-object model uses the requested ABI");
    Check(pmx ? model->cpuPmxIndices.size() == indices.size()
              : model->cpuIndices.size() == indices.size() * 2,
          "the cross-object model keeps its authored CPU source indices");
    return model;
}

CrossObjectFrameResult RunCrossObjectFrame(
    MMDApp* app, IDirect3DDevice9* device, IDirect3DSurface9* backbuffer,
    IDirect3DSurface9* readback, int resource, const D3DMATRIX& identity,
    Model* first, Model* second, const char* caseName, const char* formatName,
    const char* precision, bool crossObjects, const char* order, int step,
    const int expected[3]) {
    CrossObjectFrameResult result;
    D3DLOCKED_RECT lock{};
    RenderArrangement(app, device, backbuffer, readback, resource, identity,
                      first, second, &lock);
    result.pixels.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y)
        std::memcpy(
            result.pixels.data() + static_cast<std::size_t>(y) * kWidth,
            static_cast<const char*>(lock.pBits) +
                static_cast<std::size_t>(y) * lock.Pitch,
            static_cast<std::size_t>(kWidth) * sizeof(DWORD));
    Check(SUCCEEDED(readback->UnlockRect()), "unlock the cross-object frame");
    const int frame = g_crossObjectFrameCount++;
    char stem[256];
    std::snprintf(stem, sizeof(stem), "cross_%s_r%d_%s_c%d_%s_step%d_f%03d",
                  formatName, resource, order, crossObjects ? 1 : 0, precision,
                  step, frame);
    result.bgraName = std::string(stem) + ".bgra";
    result.bmpName = std::string(stem) + ".bmp";
    // Both files are written even when the pixel check below fails.
    WriteSphereFile(CapturePathIn(g_crossObjectCaptureDir,
                                  result.bgraName.c_str()),
                    result.pixels.data(),
                    result.pixels.size() * sizeof(DWORD),
                    "write the cross-object BGRA frame");
    WriteSphereBmp(CapturePathIn(g_crossObjectCaptureDir,
                                 result.bmpName.c_str()),
                   result.pixels, kWidth, kHeight,
                   "write the cross-object BMP frame");
    result.check = CheckSpherePixels(result.pixels, kWidth, kHeight, expected,
                                     stem);
    if (!result.check.passed)
        ++g_crossObjectFailedFrames;
    RecordCrossObjectFrame(caseName, formatName, resource, precision,
                           crossObjects, order, step, frame, expected,
                           result.check, result.bgraName, result.bmpName);
    std::printf(
        "cross-object frame=%d case=%s format=%s resource=%d precision=%s "
        "crossObjects=%d order=%s step=%d expected=(%d,%d,%d) roi0=%d roi1=%d "
        "background=%d pass=%d\n",
        frame, caseName, formatName, resource, precision,
        crossObjects ? 1 : 0, order, step, expected[0], expected[1],
        expected[2], result.check.maxError[0], result.check.maxError[1],
        result.check.backgroundWhite ? 1 : 0, result.check.passed ? 1 : 0);
    return result;
}

void RunCrossObjectResource(MMDApp* app, IDirect3DDevice9* device,
                            IDirect3DSurface9* backbuffer,
                            IDirect3DSurface9* readback, int resource,
                            const D3DMATRIX& identity) {
    D3DSURFACE_DESC target{};
    Check(SUCCEEDED(backbuffer->GetDesc(&target)) && target.Width == kWidth &&
              target.Height == kHeight,
          "the cross-object capture target is 96x96");
    D3DMATRIX world{};
    Check(SUCCEEDED(device->GetTransform(D3DTS_WORLD, &world)) &&
              std::memcmp(&world, &identity, sizeof(world)) == 0,
          "the cross-object frames use the identity world matrix");
    Check(g_resolveTarget == nullptr,
          "cross-object frames render without multisampling");
    const auto savedPrecision = app->TransparentMaterialSortPrecision();
    const bool savedCrossObjects = app->TransparentSortCrossObjects();
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    for (DWORD stage = 0; stage < 3; ++stage) {
        device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    // Off relies on the near layer's depth write to reject the far layer, so
    // the depth state is fixed here.
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    const int firstFrameOfResource = g_crossObjectFrameCount;
    for (int format = 0; format < kCrossObjectFormatCount; ++format) {
        const bool pmx = format == 1;
        const char* const formatName = kCrossObjectFormatNames[format];
        auto nearModel = BuildCrossObjectModel(device, kNearDepth,
                                               kTwoLayerFrontPath, pmx);
        auto farModel = BuildCrossObjectModel(device, kFarDepth,
                                              kTwoLayerRearPath, pmx);
        Check(nearModel->record.get() != farModel->record.get(),
              "the cross-object pair holds two independent model records");
        Check(nearModel->vertexBuffer != farModel->vertexBuffer &&
                  nearModel->indexBuffer != farModel->indexBuffer,
              "the cross-object pair holds independent GPU buffers");
        const void* const nearCpu = pmx
            ? static_cast<const void*>(nearModel->cpuPmxIndices.data())
            : static_cast<const void*>(nearModel->cpuIndices.data());
        const void* const farCpu = pmx
            ? static_cast<const void*>(farModel->cpuPmxIndices.data())
            : static_cast<const void*>(farModel->cpuIndices.data());
        Check(nearCpu != farCpu,
              "the cross-object pair does not share a CPU index array");
        // Snapshot all four authored CPU source vectors, not only the pair the
        // selected ABI fills: the other ABI's vectors must stay empty as well.
        const std::vector<std::uint16_t> nearPmdBefore = nearModel->cpuIndices;
        const std::vector<std::int32_t> nearPmxBefore =
            nearModel->cpuPmxIndices;
        const std::vector<std::uint16_t> farPmdBefore = farModel->cpuIndices;
        const std::vector<std::int32_t> farPmxBefore =
            farModel->cpuPmxIndices;
        const auto cpuIndicesUnchanged = [&] {
            return nearModel->cpuIndices == nearPmdBefore &&
                   nearModel->cpuPmxIndices == nearPmxBefore &&
                   farModel->cpuIndices == farPmdBefore &&
                   farModel->cpuPmxIndices == farPmxBefore;
        };
        const auto recordCpuState = [&](const char* stage) {
            const bool unchanged = cpuIndicesUnchanged();
            if (!unchanged) {
                std::fprintf(stderr,
                             "FAIL: the cross-object CPU source indices "
                             "changed, format=%s resource=%d stage=%s\n",
                             formatName, resource, stage);
                ++g_failures;
            }
            return unchanged;
        };
        for (int order = 0; order < kCrossObjectOrderCount; ++order) {
            const char* const orderName = kSphereOrderNames[order];
            Model* const first = order == 0 ? nearModel.get() : farModel.get();
            Model* const second = order == 0 ? farModel.get() : nearModel.get();
            for (int scope = 0; scope < kCrossObjectScopeCount; ++scope) {
                const bool crossObjects = scope == 1;
                app->TransparentSortCrossObjects() = crossObjects;
                Check(app->TransparentSortCrossObjects() == crossObjects,
                      "the cross-object frame sets the requested sort scope");
                bool restored = false;
                std::vector<DWORD> firstOffPixels;
                std::string firstOffName;
                std::string finalOffName;
                for (int step = 0; step < kCrossObjectPrecisionStepCount;
                     ++step) {
                    const bool off =
                        step == 0 ||
                        step == kCrossObjectPrecisionStepCount - 1;
                    // Off keeps the author order, and scope=false keeps it at
                    // every tier because each model owns one material and one
                    // triangle pair. Only then does near-first keep just the
                    // near layer; everything else blends far and then near.
                    const bool nearOnly =
                        order == 0 && (off || !crossObjects);
                    const SphereColor expectedColor =
                        nearOnly ? CrossObjectNearOnlyExpected()
                                 : CrossObjectDoubleLayerExpected();
                    const int expected[3] = {expectedColor.red,
                                             expectedColor.green,
                                             expectedColor.blue};
                    app->TransparentMaterialSortPrecision() =
                        kSpherePrecisions[step];
                    const CrossObjectFrameResult frame = RunCrossObjectFrame(
                        app, device, backbuffer, readback, resource, identity,
                        first, second, "precision", formatName,
                        kSpherePrecisionNames[step], crossObjects, orderName,
                        step, expected);
                    if (step == 0) {
                        firstOffPixels = frame.pixels;
                        firstOffName = frame.bmpName;
                    }
                    if (step == kCrossObjectPrecisionStepCount - 1) {
                        finalOffName = frame.bmpName;
                        restored = CrossObjectFullRgbEqual(frame.pixels,
                                                           firstOffPixels);
                        if (!restored) {
                            std::fprintf(
                                stderr,
                                "FAIL: the final Off frame did not restore the "
                                "first Off frame, format=%s resource=%d "
                                "order=%s crossObjects=%d\n",
                                formatName, resource, orderName, crossObjects);
                            ++g_failures;
                        }
                    }
                }
                app->ModelSlot(0) = nullptr;
                app->ModelSlot(1) = nullptr;
                const bool cpuUnchanged = recordCpuState("precision");
                ++g_crossObjectPrecisionRestores;
                RecordCrossObjectPrecisionRestore(
                    formatName, resource, orderName, crossObjects, restored,
                    cpuUnchanged, firstOffName, finalOffName);
                std::printf(
                    "cross-object restore kind=precision format=%s "
                    "resource=%d order=%s crossObjects=%d first=%s final=%s "
                    "frameRGB=%s cpuIndices=%s\n",
                    formatName, resource, orderName, crossObjects,
                    firstOffName.c_str(), finalOffName.c_str(),
                    restored ? "restored" : "mismatch",
                    cpuUnchanged ? "unchanged" : "changed");
            }
            // Standard precision scope toggle: false -> true -> false. The
            // first and last frames must be pixel-identical in RGB.
            app->TransparentMaterialSortPrecision() =
                TransparentSortPrecision::Standard;
            std::vector<DWORD> firstTogglePixels;
            std::vector<DWORD> finalTogglePixels;
            std::string firstToggleName;
            std::string finalToggleName;
            for (int step = 0; step < kCrossObjectToggleStepCount; ++step) {
                const bool crossObjects = step == 1;
                const bool nearOnly = order == 0 && !crossObjects;
                const SphereColor expectedColor =
                    nearOnly ? CrossObjectNearOnlyExpected()
                             : CrossObjectDoubleLayerExpected();
                const int expected[3] = {expectedColor.red,
                                         expectedColor.green,
                                         expectedColor.blue};
                app->TransparentSortCrossObjects() = crossObjects;
                const CrossObjectFrameResult frame = RunCrossObjectFrame(
                    app, device, backbuffer, readback, resource, identity,
                    first, second, "scope-toggle", formatName, "standard",
                    crossObjects, orderName, step, expected);
                if (step == 0) {
                    firstTogglePixels = frame.pixels;
                    firstToggleName = frame.bmpName;
                }
                if (step == kCrossObjectToggleStepCount - 1) {
                    finalTogglePixels = frame.pixels;
                    finalToggleName = frame.bmpName;
                }
            }
            app->ModelSlot(0) = nullptr;
            app->ModelSlot(1) = nullptr;
            const bool scopeRestored = CrossObjectFullRgbEqual(
                finalTogglePixels, firstTogglePixels);
            if (!scopeRestored) {
                std::fprintf(stderr,
                             "FAIL: the final scope=false frame did not "
                             "restore the first scope=false frame, format=%s "
                             "resource=%d order=%s\n",
                             formatName, resource, orderName);
                ++g_failures;
            }
            const bool cpuUnchanged = recordCpuState("scope-toggle");
            ++g_crossObjectScopeToggleRestores;
            RecordCrossObjectScopeRestore(formatName, resource, orderName,
                                          scopeRestored, cpuUnchanged,
                                          firstToggleName, finalToggleName);
            std::printf(
                "cross-object restore kind=scope format=%s resource=%d "
                "order=%s scope=false-true-false first=%s final=%s "
                "frameRGB=%s cpuIndices=%s\n",
                formatName, resource, orderName, firstToggleName.c_str(),
                finalToggleName.c_str(),
                scopeRestored ? "restored" : "mismatch",
                cpuUnchanged ? "unchanged" : "changed");
        }
        nearModel.reset();
        farModel.reset();
    }
    Check(g_crossObjectFrameCount - firstFrameOfResource ==
              kCrossObjectFramesPerResource,
          "the cross-object resource rendered its contracted frame count");
    app->TransparentMaterialSortPrecision() = savedPrecision;
    app->TransparentSortCrossObjects() = savedCrossObjects;
    app->ClearModelSlots();
    std::printf("cross-object resource=%d frames=%d failures=%d\n", resource,
                g_crossObjectFrameCount, g_failures);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // The capture directory has to accept Windows Unicode, so the command line
    // is read as UTF-16. Every existing switch stays ASCII and keeps its exact
    // UTF-8 comparison; only the directory value uses the wide text.
    int wideArgumentCount = 0;
    LPWSTR* wideArgumentList =
        CommandLineToArgvW(GetCommandLineW(), &wideArgumentCount);
    if (wideArgumentList == nullptr) {
        std::fputs("Cannot read the command line\n", stderr);
        return 2;
    }
    std::vector<std::string> arguments;
    std::vector<std::wstring> argumentTexts;
    arguments.reserve(static_cast<std::size_t>(wideArgumentCount));
    argumentTexts.reserve(static_cast<std::size_t>(wideArgumentCount));
    for (int i = 1; i < wideArgumentCount; ++i) {
        const std::wstring text(wideArgumentList[i]);
        std::string utf8(text.size() * 4 + 1, '\0');
        const int length = WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
        if (length <= 0) {
            std::fputs("Cannot convert a command-line argument\n", stderr);
            LocalFree(wideArgumentList);
            return 2;
        }
        utf8.resize(static_cast<std::size_t>(length));
        arguments.push_back(std::move(utf8));
        argumentTexts.push_back(text);
    }
    LocalFree(wideArgumentList);

    // Every switch is validated completely before a marker DLL is loaded or a
    // device is created. Old legal command lines keep their exact meaning;
    // unknown, duplicated, missing and conflicting arguments are usage errors.
    bool intersectionsOnly = false;
    bool lowAlphaOnly = false;
    bool peelBudgetOnly = false;
    bool maximumLimitOnly = false;
    bool maximumFullSize = false;
    bool msaaRequested = false;
    bool sortOffOnly = false;
    bool sphereOnly = false;
    bool crossObjectOnly = false;
    bool sphereModeSeen = false;
    int sphereMode = kSphereModeDefault;
    bool precisionRequested = false;
    bool crossObjects = false;
    std::wstring mmeMarkerPath;
    bool markerRequested = false;
    bool captureDirectoryRequested = false;
    std::wstring captureDirectory;
    bool precisionSeen = false;
    bool crossObjectsSeen = false;
    bool markerSeen = false;
    bool sortOffSeen = false;
    bool intersectionsSeen = false;
    bool lowAlphaSeen = false;
    bool peelBudgetSeen = false;
    bool maximumLimitSeen = false;
    bool maximumFullSizeSeen = false;
    bool msaaSeen = false;
    bool sphereSeen = false;
    bool crossObjectSeen = false;
    bool captureSeen = false;
    TransparentSortPrecision precision = TransparentSortPrecision::Standard;
    const char* precisionName = "standard";
    const auto duplicate = [](const char* option) {
        std::fprintf(stderr, "Duplicate option %s\n", option);
        return 2;
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const char* argument = arguments[i].c_str();
        if (std::strcmp(argument, "--precision") == 0) {
            if (precisionSeen) return duplicate(argument);
            precisionSeen = true;
            // A following option is a missing value, not a precision name.
            if (++i == arguments.size() ||
                arguments[i].compare(0, 2, "--") == 0) {
                std::fputs("--precision needs low, standard, high or maximum\n", stderr);
                return 2;
            }
            precisionName = arguments[i].c_str();
            if (std::strcmp(precisionName, "low") == 0) precision = TransparentSortPrecision::Low;
            else if (std::strcmp(precisionName, "standard") == 0) precision = TransparentSortPrecision::Standard;
            else if (std::strcmp(precisionName, "high") == 0) precision = TransparentSortPrecision::High;
            else if (std::strcmp(precisionName, "maximum") == 0) precision = TransparentSortPrecision::Maximum;
            else {
                std::fputs("--precision needs low, standard, high or maximum; use --sort-off for Off\n", stderr);
                return 2;
            }
            precisionRequested = true;
        } else if (std::strcmp(argument, "--cross-objects") == 0) {
            if (crossObjectsSeen) return duplicate(argument);
            crossObjectsSeen = true;
            crossObjects = true;
        } else if (std::strcmp(argument, "--mme-marker") == 0) {
            if (markerSeen) return duplicate(argument);
            markerSeen = true;
            // Reject a following option as a missing DLL path before any
            // LoadLibrary call can turn the next switch into a file name.
            if (++i == arguments.size() ||
                arguments[i].compare(0, 2, "--") == 0) {
                std::fputs("--mme-marker needs a DLL path\n", stderr);
                return 2;
            }
            mmeMarkerPath = argumentTexts[i];
            markerRequested = true;
        } else if (std::strcmp(argument, "--sort-off") == 0) {
            if (sortOffSeen) return duplicate(argument);
            sortOffSeen = true;
            sortOffOnly = true;
        } else if (std::strcmp(argument, "--intersections") == 0) {
            if (intersectionsSeen) return duplicate(argument);
            intersectionsSeen = true;
            intersectionsOnly = true;
        } else if (std::strcmp(argument, "--low-alpha") == 0) {
            if (lowAlphaSeen) return duplicate(argument);
            lowAlphaSeen = true;
            lowAlphaOnly = true;
        } else if (std::strcmp(argument, "--peel-budget") == 0) {
            if (peelBudgetSeen) return duplicate(argument);
            peelBudgetSeen = true;
            peelBudgetOnly = true;
        } else if (std::strcmp(argument, "--maximum-limit") == 0) {
            if (maximumLimitSeen) return duplicate(argument);
            maximumLimitSeen = true;
            maximumLimitOnly = true;
        } else if (std::strcmp(argument, "--maximum-full-size") == 0) {
            if (maximumFullSizeSeen) return duplicate(argument);
            maximumFullSizeSeen = true;
            maximumLimitOnly = true;
            maximumFullSize = true;
        } else if (std::strcmp(argument, "--msaa") == 0) {
            if (msaaSeen) return duplicate(argument);
            msaaSeen = true;
            msaaRequested = true;
        } else if (std::strcmp(argument, "--sphere-only") == 0) {
            if (sphereSeen) return duplicate(argument);
            sphereSeen = true;
            sphereOnly = true;
        } else if (std::strcmp(argument, "--cross-object-only") == 0) {
            if (crossObjectSeen) return duplicate(argument);
            crossObjectSeen = true;
            crossObjectOnly = true;
        } else if (std::strcmp(argument, "--sphere-mode") == 0) {
            if (sphereModeSeen) return duplicate(argument);
            sphereModeSeen = true;
            // A following option is a missing value, not a mode number.
            if (++i == arguments.size() ||
                arguments[i].compare(0, 2, "--") == 0) {
                std::fputs("--sphere-mode needs 1, 2 or 3\n", stderr);
                return 2;
            }
            const char* modeText = arguments[i].c_str();
            if (std::strcmp(modeText, "1") == 0) sphereMode = 1;
            else if (std::strcmp(modeText, "2") == 0) sphereMode = 2;
            else if (std::strcmp(modeText, "3") == 0) sphereMode = 3;
            else {
                std::fputs("--sphere-mode needs 1, 2 or 3\n", stderr);
                return 2;
            }
        } else if (std::strcmp(argument, "--capture-dir") == 0) {
            if (captureSeen) return duplicate(argument);
            captureSeen = true;
            // Absolute Windows paths never begin with '--', so this only
            // catches a following switch used where the directory belongs.
            if (++i == arguments.size() ||
                arguments[i].compare(0, 2, "--") == 0) {
                std::fputs("--capture-dir needs an absolute directory\n", stderr);
                return 2;
            }
            captureDirectory = argumentTexts[i];
            captureDirectoryRequested = true;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argument);
            return 2;
        }
    }
    // --sphere-mode is a sphereMode selector, not a scene switch: it is only
    // legal on the --sphere-only run and is rejected before any device or
    // capture directory exists.
    if (sphereModeSeen && !sphereOnly) {
        std::fputs("--sphere-mode is only valid with --sphere-only\n", stderr);
        return 2;
    }
    if (int(intersectionsOnly) + int(lowAlphaOnly) + int(peelBudgetOnly) +
            int(maximumLimitOnly) + int(sortOffOnly) + int(sphereOnly) +
            int(crossObjectOnly) > 1) {
        std::fputs("Choose a single scene mode\n", stderr);
        return 2;
    }
    const bool ordinary = !intersectionsOnly && !lowAlphaOnly && !peelBudgetOnly &&
        !maximumLimitOnly && !sortOffOnly && !sphereOnly && !crossObjectOnly;
    if (precisionRequested && !ordinary) {
        std::fputs("--precision applies only to ordinary occlusion; scene modes select their own tier\n", stderr);
        return 2;
    }
#if MMD_ORIGINAL_PROFILE
    if (!sortOffOnly) {
        std::fputs("The original profile supports this fixture's --sort-off mode only\n", stderr);
        return 2;
    }
#endif
    if (sphereOnly) {
        g_sphereMode = sphereMode;
        if (crossObjects || markerRequested || msaaRequested) {
            std::fputs("--sphere-only cannot be combined with --cross-objects, --mme-marker or --msaa\n", stderr);
            return 2;
        }
        if (!captureDirectoryRequested) {
            std::fputs("--sphere-only requires --capture-dir <absolute directory>\n", stderr);
            return 2;
        }
        std::string captureError;
        if (!SpherePrepareCaptureDirectory(&captureDirectory, &captureError)) {
            std::fprintf(stderr, "Invalid --capture-dir: %s\n", captureError.c_str());
            return 2;
        }
        g_sphereCaptureDir = captureDirectory;
    } else if (crossObjectOnly) {
        // --sphere-mode was rejected above because it needs --sphere-only, and
        // --precision was rejected because this is not the ordinary scene.
        if (crossObjects || markerRequested || msaaRequested) {
            std::fputs("--cross-object-only cannot be combined with --cross-objects, --mme-marker or --msaa\n", stderr);
            return 2;
        }
        if (!captureDirectoryRequested) {
            std::fputs("--cross-object-only requires --capture-dir <absolute directory>\n", stderr);
            return 2;
        }
        std::string captureError;
        if (!SpherePrepareCaptureDirectory(&captureDirectory, &captureError)) {
            std::fprintf(stderr, "Invalid --capture-dir: %s\n", captureError.c_str());
            return 2;
        }
        g_crossObjectCaptureDir = captureDirectory;
    } else if (captureDirectoryRequested) {
        std::fputs("--capture-dir is only valid with --sphere-only or --cross-object-only\n", stderr);
        return 2;
    }
    HMODULE mmeMarker = nullptr;
    if (markerRequested) {
        mmeMarker = LoadLibraryW(mmeMarkerPath.c_str());
        Check(mmeMarker && GetModuleHandleW(L"MMEffect.dll") == mmeMarker,
              "load inert MMEffect.dll presence marker");
    }
    if (sphereOnly) {
        const std::wstring recordPath =
            SphereCapturePath("sphere_only_frames.jsonl");
        g_sphereRecord = CreateFileW(recordPath.c_str(), GENERIC_WRITE, 0,
                                     nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(g_sphereRecord != INVALID_HANDLE_VALUE,
              "create the sphere-only frame record");
    }
    if (crossObjectOnly) {
        const std::wstring recordPath =
            CapturePathIn(g_crossObjectCaptureDir, "cross_object_frames.jsonl");
        g_crossObjectRecord = CreateFileW(recordPath.c_str(), GENERIC_WRITE, 0,
                                          nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(g_crossObjectRecord != INVALID_HANDLE_VALUE,
              "create the cross-object frame record");
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // One local target size drives the window, the device, the readback surface
    // and the MSAA resolve surface. Maximum defaults to 16x16; the other modes
    // and the explicit full-size stress mode keep the original 96x96.
    const int targetWidth = maximumLimitOnly && !maximumFullSize ? 16 : kWidth;
    const int targetHeight =
        maximumLimitOnly && !maximumFullSize ? 16 : kHeight;
    if (sphereOnly) {
        std::printf("SPHERE_ONLY BEGIN capture-dir=%ls target=%dx%d "
                    "sphereMode=%d resources=0,117,118 main-frames=%d "
                    "control-frames=%d uv1-probe-frames=%d total-frames=%d "
                    "tolerance=%d\n",
                    g_sphereCaptureDir.c_str(), targetWidth, targetHeight,
                    g_sphereMode, SphereMainFrameCount(),
                    SphereControlFrameCount(), SphereUv1ProbeFrameCount(),
                    SphereExpectedFrameCount(), kSphereTolerance);
    }
    if (crossObjectOnly) {
        std::printf("CROSS_OBJECT BEGIN capture-dir=%ls target=%dx%d "
                    "resources=0,117,118 formats=pmd,pmx orders=near-first,"
                    "far-first scopes=false,true precision-steps=%d "
                    "toggle-steps=%d expected-frames=%d "
                    "precision-restores=%d scope-restores=%d tolerance=%d\n",
                    g_crossObjectCaptureDir.c_str(), targetWidth, targetHeight,
                    kCrossObjectPrecisionStepCount,
                    kCrossObjectToggleStepCount,
                    kCrossObjectExpectedFrameCount,
                    kCrossObjectPrecisionRestoreCount,
                    kCrossObjectScopeToggleRestoreCount, kSphereTolerance);
    }
    HWND window = CreateWindowW(L"STATIC", L"Alpha occlusion regression",
                                WS_POPUP, 0, 0, targetWidth, targetHeight,
                                nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    Check(window != nullptr, "create hidden render window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = targetWidth;
    pp.BackBufferHeight = targetHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    // The rim only shows while the front surface's depth survives the frame:
    // without a depth buffer every translucent draw lands in submission order.
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;

    // --msaa renders the same scenes into a 4x multisampled back buffer. The
    // capability is a hard requirement, because silently keeping a
    // single-sampled device would report a pass that never exercised MSAA.
    DWORD msaaColourQuality = 0;
    DWORD msaaDepthQuality = 0;
    if (msaaRequested) {
        const bool colourSupported = SUCCEEDED(d3d->CheckDeviceMultiSampleType(
            0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
            D3DMULTISAMPLE_4_SAMPLES, &msaaColourQuality));
        const bool depthSupported = SUCCEEDED(d3d->CheckDeviceMultiSampleType(
            0, D3DDEVTYPE_HAL, D3DFMT_D16, TRUE,
            D3DMULTISAMPLE_4_SAMPLES, &msaaDepthQuality));
        if (!colourSupported || msaaColourQuality == 0 || !depthSupported ||
            msaaDepthQuality == 0) {
            std::fprintf(stderr,
                         "FAIL: --msaa needs 4x multisampling for an A8R8G8B8 "
                         "back buffer and a D16 depth buffer; this device "
                         "reports colour=%lu depth=%lu, so the run cannot "
                         "exercise MSAA and must not be read as a pass\n",
                         static_cast<unsigned long>(
                             colourSupported ? msaaColourQuality : 0),
                         static_cast<unsigned long>(
                             depthSupported ? msaaDepthQuality : 0));
            return 2;
        }
        pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
        pp.MultiSampleQuality = 0;
        std::printf("msaa=4x colour-quality=%lu depth-quality=%lu\n",
                    static_cast<unsigned long>(msaaColourQuality),
                    static_cast<unsigned long>(msaaDepthQuality));
    }

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    app->Hwnd() = window;
    app->EnglishUI() = 1;
    app->TransparentSortCrossObjects() = crossObjects || (!ordinary && !sphereOnly);
    app->PlaybackActive() = 1;
    if (ordinary) {
        app->TransparentMaterialSortPrecision() = precision;
        std::printf("occlusion precision=%s (%u)\n", precisionName, unsigned(precision));
    }
    auto& present = renderer->presentParameters;
    present = pp;
    IDirect3DDevice9* device = nullptr;
    // --maximum-limit creates its device the way the production cloud benchmark
    // does, with hardware vertex processing (0x40). Every other mode keeps the
    // software vertex processing path this regression has always used.
    const DWORD behaviorFlags = maximumLimitOnly
        ? D3DCREATE_HARDWARE_VERTEXPROCESSING
        : D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, behaviorFlags,
                                      &present, &device)),
          "create a real GPU device");
    // The flags the device really got, not the ones requested. A silent
    // fallback to software vertex processing would make the --maximum-limit
    // timing incomparable with the production benchmark, so it fails the run
    // instead of being reported as a pass.
    D3DDEVICE_CREATION_PARAMETERS creation{};
    Check(SUCCEEDED(device->GetCreationParameters(&creation)),
          "read the device creation parameters");
    std::printf("device requested-flags=0x%08lX actual-flags=0x%08lX\n",
                static_cast<unsigned long>(behaviorFlags),
                static_cast<unsigned long>(creation.BehaviorFlags));
    if (maximumLimitOnly) {
        Check((creation.BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) != 0 &&
                  (creation.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) == 0,
              "the --maximum-limit run needs a hardware vertex processing "
              "device; this device fell back to software");
    }
    renderer->device = device;

    // The production texture pool classifies both materials as translucent: the
    // tag is derived here from the actual texels with the same helper the
    // loaders use, so the fixture cannot fake the classification.
    IDirect3DTexture9* nearTexture =
        CreateTexture(device, kNearTexels, 3, 1, "create the front texture");
    IDirect3DTexture9* farTexture =
        CreateTexture(device, kFarTexels, 2, 1, "create the rear texture");
    // The two-layer textures exist twice, once per layer and once per model
    // arrangement, because each model binds its own material path.
    IDirect3DTexture9* twoLayerRearTexture = CreateTexture(
        device, kTwoLayerRearTexels, 2, 1, "create the two-layer rear texture");
    IDirect3DTexture9* twoLayerFrontTexture = CreateTexture(
        device, kTwoLayerFrontTexels, 2, 1, "create the two-layer front texture");
    unsigned char nearTag[4]{};
    unsigned char farTag[4]{};
    unsigned char twoLayerRearTag[4]{};
    unsigned char twoLayerFrontTag[4]{};
    CacheTextureColorAndAlpha(nearTexture, nearTag);
    CacheTextureColorAndAlpha(farTexture, farTag);
    CacheTextureColorAndAlpha(twoLayerRearTexture, twoLayerRearTag);
    CacheTextureColorAndAlpha(twoLayerFrontTexture, twoLayerFrontTag);
    Check((nearTag[3] & kTextureUsesAlpha) != 0,
          "the front texture has non-opaque texels");
    Check((farTag[3] & kTextureUsesAlpha) != 0,
          "the blue texture has a transparent texel elsewhere, so its solid "
          "surface is translucent too");
    // Texture alpha, rather than diffuse alpha, selects the translucent pass.
    Check((twoLayerRearTag[3] & kTextureUsesAlpha) != 0 &&
              (twoLayerFrontTag[3] & kTextureUsesAlpha) != 0,
          "both two-layer textures carry a transparent texel, so both materials "
          "are classified translucent from their own texels");
    // The pool scan stops at the first empty slot, so the two entries are
    // contiguous from index 0 and their names live as long as the renderer.
    PublishFixtureTexture(renderer.get(), 0, kNearPath, nearTexture, nearTag);
    PublishFixtureTexture(renderer.get(), 1, kFarPath, farTexture, farTag);
    PublishFixtureTexture(renderer.get(), 2, kTwoLayerRearPath, twoLayerRearTexture, twoLayerRearTag);
    PublishFixtureTexture(renderer.get(), 3, kTwoLayerFrontPath, twoLayerFrontTexture, twoLayerFrontTag);

    // Stage 0 samples the toon table. A null stage-0 texture lets the fixed
    // pipeline drop its cascade before stage 1 ever runs, so the fixture binds
    // an opaque white texel there and marches the scene light to a direction
    // the toon basis can be built from.
    const DWORD whiteTexel = 0xFFFFFFFFu;
    IDirect3DTexture9* toonTexture =
        CreateTexture(device, &whiteTexel, 1, 1, "create the toon texture");
    app->ToonTexture(0) = toonTexture;
    app->SceneLight().Direction.x = 0.0f;
    app->SceneLight().Direction.y = 0.0f;
    app->SceneLight().Direction.z = -1.0f;

    // The front surface (three bands at the near depth) and the rear surface
    // (one blue quad at the far depth) live in separate models and cover the
    // same pixels, so which one is submitted first decides the result.
    constexpr float kThird = 1.0f / 3.0f;
    std::vector<ModelVertex> frontVertices;
    std::vector<WORD> frontIndices;
    AppendQuad(frontVertices, frontIndices, -1.0f, 1.0f, 1.0f, kThird,
               kNearDepth, 5.0f / 6.0f, 0.5f);  // solid texel
    AppendQuad(frontVertices, frontIndices, -1.0f, kThird, 1.0f, -kThird,
               kNearDepth, 0.5f, 0.5f);  // half-transparent texel
    AppendQuad(frontVertices, frontIndices, -1.0f, -kThird, 1.0f, -1.0f,
               kNearDepth, 1.0f / 6.0f, 0.5f);  // transparent texel
    std::vector<ModelVertex> rearVertices;
    std::vector<WORD> rearIndices;
    AppendQuad(rearVertices, rearIndices, -1.0f, 1.0f, 1.0f, -1.0f, kFarDepth,
               0.25f, 0.5f);  // opaque blue texel

    // One material each, owning the whole index buffer: the single-layer
    // fixture keeps its original shape.
    auto front = BuildModel(device, frontVertices, frontIndices,
                            std::vector<MaterialSpec>{{kNearPath, 1.0f, 0,
                                                       frontIndices.size()}});
    auto rear = BuildModel(device, rearVertices, rearIndices,
                           std::vector<MaterialSpec>{{kFarPath, 1.0f, 0,
                                                      rearIndices.size()}});

    // Two-layer fixture. Each layer is one full-viewport quad at its own depth.
    // Both quads sample with kTwoLayerSamplerU, which is the coordinate that
    // lands on texel 0 of the two-wide textures.
    std::vector<ModelVertex> twoLayerFrontVertices;
    std::vector<WORD> twoLayerFrontIndices;
    AppendQuad(twoLayerFrontVertices, twoLayerFrontIndices, -1.0f, 1.0f, 1.0f,
               -1.0f, kNearDepth, kTwoLayerSamplerU,
               0.5f);  // half-transparent front texel
    std::vector<ModelVertex> twoLayerRearVertices;
    std::vector<WORD> twoLayerRearIndices;
    AppendQuad(twoLayerRearVertices, twoLayerRearIndices, -1.0f, 1.0f, 1.0f,
               -1.0f, kFarDepth, kTwoLayerSamplerU,
               0.5f);  // half-transparent rear texel

    // Swap index ranges together with materials so both orders describe the same scene.
    std::vector<ModelVertex> twoLayerSingleModelVertices;
    std::vector<WORD> twoLayerNearFirstIndices;
    AppendQuad(twoLayerSingleModelVertices, twoLayerNearFirstIndices,
               -1.0f, 1.0f, 1.0f, -1.0f, kNearDepth, kTwoLayerSamplerU, 0.5f);
    AppendQuad(twoLayerSingleModelVertices, twoLayerNearFirstIndices,
               -1.0f, 1.0f, 1.0f, -1.0f, kFarDepth, kTwoLayerSamplerU, 0.5f);
    // Same vertices and same two quads, with the far quad's indices moved to
    // the front of the buffer. The vertex data is untouched, so every quad
    // keeps its own depth and texture coordinate.
    std::vector<WORD> twoLayerFarFirstIndices(
        twoLayerNearFirstIndices.begin() + 6, twoLayerNearFirstIndices.end());
    twoLayerFarFirstIndices.insert(twoLayerFarFirstIndices.end(),
                                   twoLayerNearFirstIndices.begin(),
                                   twoLayerNearFirstIndices.begin() + 6);
    Check(twoLayerSingleModelVertices.size() == 8 &&
              twoLayerNearFirstIndices.size() == 12 &&
              twoLayerFarFirstIndices.size() == 12,
          "two-material model carries both layer quads");
    // Arrangement A pairs material 0 with the near quad and the front texture;
    // arrangement B pairs material 0 with the far quad and the rear texture.
    // The front texture is therefore on the near quad and the rear texture on
    // the far quad in both arrangements, so the two differ only in which of the
    // two materials the walk reaches first.
    const std::vector<MaterialSpec> twoLayerFrontFirstModel{
        {kTwoLayerFrontPath, 1.0f, 0, 6}, {kTwoLayerRearPath, 1.0f, 6, 6}};
    const std::vector<MaterialSpec> twoLayerRearFirstModel{
        {kTwoLayerRearPath, 1.0f, 0, 6}, {kTwoLayerFrontPath, 1.0f, 6, 6}};
    // The far-first index buffer must still resolve to the same two quads, just
    // in the other order, or the two arrangements are not the same scene.
    Check(twoLayerFarFirstIndices[0] == twoLayerNearFirstIndices[6] &&
              twoLayerFarFirstIndices[6] == twoLayerNearFirstIndices[0],
          "far-first index buffer reorders the quads without changing them");

    auto twoLayerFrontModel =
        BuildModel(device, twoLayerFrontVertices, twoLayerFrontIndices,
                   std::vector<MaterialSpec>{{kTwoLayerFrontPath, 1.0f, 0,
                                              twoLayerFrontIndices.size()}});
    auto twoLayerRearModel =
        BuildModel(device, twoLayerRearVertices, twoLayerRearIndices,
                   std::vector<MaterialSpec>{{kTwoLayerRearPath, 1.0f, 0,
                                              twoLayerRearIndices.size()}});
    // Arrangement A records the front material first over the near-first index
    // buffer; arrangement B records the rear material first over the far-first
    // index buffer. Both therefore put the front layer at kNearDepth and the
    // rear layer at kFarDepth, and differ only in which material the walk
    // reaches first.
    auto twoLayerFrontFirstAssembly = BuildModel(
        device, twoLayerSingleModelVertices, twoLayerNearFirstIndices,
        twoLayerFrontFirstModel);
    auto twoLayerRearFirstAssembly = BuildModel(
        device, twoLayerSingleModelVertices, twoLayerFarFirstIndices,
        twoLayerRearFirstModel);

    // PMX twins of the same three scenes. They reuse the identical vertices,
    // textures, depths and submissions and differ only in the model ABI, so the
    // expected pixels are the ones the PMD runs already established: 32-bit CPU
    // and GPU indices (PmxIndices / D3DFMT_INDEX32), physicsMode 2 to select the
    // PMX material path, and the per-material morph pools that path reads.
    auto pmxTwoLayerFrontModel =
        BuildModel(device, twoLayerFrontVertices, twoLayerFrontIndices,
                   std::vector<MaterialSpec>{{kTwoLayerFrontPath, 1.0f, 0,
                                              twoLayerFrontIndices.size()}},
                   true);
    auto pmxTwoLayerRearModel =
        BuildModel(device, twoLayerRearVertices, twoLayerRearIndices,
                   std::vector<MaterialSpec>{{kTwoLayerRearPath, 1.0f, 0,
                                              twoLayerRearIndices.size()}},
                   true);
    auto pmxTwoLayerFrontFirstAssembly = BuildModel(
        device, twoLayerSingleModelVertices, twoLayerNearFirstIndices,
        twoLayerFrontFirstModel, true);
    auto pmxTwoLayerRearFirstAssembly = BuildModel(
        device, twoLayerSingleModelVertices, twoLayerFarFirstIndices,
        twoLayerRearFirstModel, true);

    // PMX double-sided material: flags bit 0 set. Both layers carry it, so the
    // pass takes its cull-none branch for each (the PMX material path forces
    // D3DCULL_NONE when alpha < 1 as well, but the flag alone must not change
    // the blended result).
    const std::vector<MaterialSpec> pmxDoubleSidedSpecs{
        {kTwoLayerFrontPath, 1.0f, 0, 6, 1}, {kTwoLayerRearPath, 1.0f, 6, 6, 1}};
    auto reversedWinding = twoLayerNearFirstIndices;
    for (std::size_t i = 0; i < reversedWinding.size(); i += 3)
        std::swap(reversedWinding[i], reversedWinding[i + 1]);
    auto pmxDoubleSidedScene = BuildModel(
        device, twoLayerSingleModelVertices, reversedWinding,
        pmxDoubleSidedSpecs, true);

    // PMX material alpha: Diffuse.a drops from 1.0 to 0.5 on a material whose
    // texture is otherwise opaque. This is the branch where the pass blends
    // because of the material colour rather than an alpha texel, and on PMX it
    // also takes cull-none.
    constexpr DWORD kPmxOpaqueTexel = 0xFF20C0E0u;  // a=0xFF r=0x20 g=0xC0 b=0xE0
    IDirect3DTexture9* pmxOpaqueTexture =
        CreateTexture(device, &kPmxOpaqueTexel, 1, 1,
                      "create the PMX opaque texture");
    unsigned char pmxOpaqueTag[4]{};
    CacheTextureColorAndAlpha(pmxOpaqueTexture, pmxOpaqueTag);
    Check((pmxOpaqueTag[3] & kTextureUsesAlpha) == 0,
          "the PMX alpha fixture texture is opaque, so the material alpha is "
          "the only reason its surface blends");
    PublishFixtureTexture(renderer.get(), 4, kPmxAlphaPath, pmxOpaqueTexture, pmxOpaqueTag);
    const std::vector<MaterialSpec> pmxMaterialAlphaSpecs{
        {kPmxAlphaPath, 0.5f, 0, 6}, {kTwoLayerRearPath, 1.0f, 6, 6}};
    auto pmxMaterialAlphaScene = BuildModel(
        device, twoLayerSingleModelVertices, twoLayerNearFirstIndices,
        pmxMaterialAlphaSpecs, true);

    // Opposite local depth orders force the cross-material triangle path.
    std::vector<ModelVertex> interleavedVertices;
    std::vector<WORD> interleavedIndices;
    AppendQuad(interleavedVertices,interleavedIndices,-1,1,0,-1,kNearDepth,.25f,.5f);
    AppendQuad(interleavedVertices,interleavedIndices,0,1,1,-1,kFarDepth,.25f,.5f);
    AppendQuad(interleavedVertices,interleavedIndices,-1,1,0,-1,kFarDepth,.25f,.5f);
    AppendQuad(interleavedVertices,interleavedIndices,0,1,1,-1,kNearDepth,.25f,.5f);
    auto interleaved = BuildModel(device,interleavedVertices,interleavedIndices,
        {{kTwoLayerFrontPath,1,0,12},{kTwoLayerRearPath,1,12,12}},true);

    // A visible rear layer plus offscreen geometry exceeds the old 4096-triangle
    // material barrier without changing the expected two-layer pixel colors.
    constexpr std::size_t kLargeMaterialFillerQuads = 2048;  // 4096 triangles
    std::vector<ModelVertex> largeRearVertices;
    std::vector<WORD> largeRearIndices;
    AppendQuad(largeRearVertices, largeRearIndices, -1.0f, 1.0f, 1.0f, -1.0f,
               kFarDepth, kTwoLayerSamplerU, 0.5f);  // half-transparent rear texel
    for (std::size_t i = 0; i < kLargeMaterialFillerQuads; ++i) {
        // Small, distinct, fully offscreen patches to the right of the clip box.
        const float left = 2.0f + 0.0005f * static_cast<float>(i);
        AppendQuad(largeRearVertices, largeRearIndices, left, 2.0005f,
                   left + 0.0005f, 2.0f, kFarDepth, kTwoLayerSamplerU, 0.5f);
    }
    auto largeRearModel = BuildModel(
        device, largeRearVertices, largeRearIndices,
        std::vector<MaterialSpec>{{kTwoLayerRearPath, 1.0f, 0,
                                   largeRearIndices.size()}});
    Check(largeRearModel->materials[0].faceVertexCount / 3 > 4096,
          "the large material holds more than 4096 triangles");

    // Render state the model pass assumes (InitRenderStates) plus the identity
    // transforms that keep the vertices in the viewport's clip box. The
    // submission order is decided in world-view space, where identity makes the
    // measure equal to the vertex z: 0.25 in front, 0.75 behind.
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetRenderState(D3DRS_AMBIENT, 0xFFFFFFFF);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    // Translucent surfaces keep writing depth on purpose - the outlines behind
    // them depend on it - which is exactly why their submission order matters.
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAREF, 1);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                 D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &identity);

    IDirect3DSurface9* backbuffer = nullptr;
    IDirect3DSurface9* readback = nullptr;
    Check(SUCCEEDED(device->GetRenderTarget(0, &backbuffer)),
          "get the render target");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(
              targetWidth, targetHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
              &readback, nullptr)),
          "create the readback surface");
    // The back buffer has to keep the target size, and it has to agree with the
    // present parameters the stack fixture derives its sample positions from: a
    // silently resized target would move every sample.
    D3DSURFACE_DESC targetDesc{};
    Check(SUCCEEDED(backbuffer->GetDesc(&targetDesc)) &&
              targetDesc.Width == static_cast<UINT>(targetWidth) &&
              targetDesc.Height == static_cast<UINT>(targetHeight) &&
              present.BackBufferWidth == targetDesc.Width &&
              present.BackBufferHeight == targetDesc.Height,
          "the back buffer is the target size the samples come from");
    std::printf("target actual=%ux%u requested=%dx%d\n", targetDesc.Width,
                targetDesc.Height, targetWidth, targetHeight);
    if (msaaRequested) {
        // The device must really hold a multisampled back buffer, and the
        // frame has to be resolved into a single-sample surface of the same
        // size before it can be read.
        Check(targetDesc.MultiSampleType != D3DMULTISAMPLE_NONE,
              "the --msaa back buffer is multisampled, not single-sampled");
        Check(SUCCEEDED(device->CreateRenderTarget(
                  targetWidth, targetHeight, D3DFMT_A8R8G8B8,
                  D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolveTarget, nullptr)),
              "create the resolve target");
        std::printf("msaa resolve surface=%dx%d plain\n", targetWidth,
                    targetHeight);
    }

    auto& api = d3dx::Get();
    Check(api.Load(), "load D3DX for embedded effects");
    app->LightDirection()[2] = -1.0f;
    app->SceneLight().Ambient = {1, 1, 1, 1};
    app->CameraDistance() = -2.0f;
    std::memcpy(&app->ViewRotationTransform(), &identity, sizeof(identity));
    std::memcpy(&app->LightViewProjection(), &identity, sizeof(identity));
    // Run fixed-function and both actual embedded shader variants. --low-alpha,
    // --peel-budget and --maximum-limit narrow the run to the shader-model-3
    // resource (118) and to their own scene, so no other scene runs in those
    // modes.
    for (int resource : {0, 117, 118}) {
        if ((lowAlphaOnly || peelBudgetOnly || maximumLimitOnly) &&
            resource != 118)
            continue;
        if (resource != 0) {
            void* errors = nullptr;
            const HRESULT hr = api.createEffectFromResA(device, nullptr,
                MAKEINTRESOURCEA(resource), nullptr, nullptr, 0, nullptr,
                reinterpret_cast<void**>(&renderer->effect), &errors);
            if (errors) reinterpret_cast<IUnknown*>(errors)->Release();
            Check(SUCCEEDED(hr) && renderer->effect, "compile embedded model effect");
            renderer->shaderModel3 = resource == 118;
            renderer->hdrTexture = toonTexture;
        }
        std::printf("renderer resource=%d\n", resource);
        if (sphereOnly) {
            RunSphereOnlyResource(app.get(), device, backbuffer, readback,
                                  resource, identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
        if (crossObjectOnly) {
            RunCrossObjectResource(app.get(), device, backbuffer, readback,
                                   resource, identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
#if !MMD_ORIGINAL_PROFILE
        if (ordinary)
            CheckTextureSortCandidates(app.get(), device, backbuffer, readback, resource, identity, mmeMarker != nullptr);
#endif
        if (sortOffOnly) {
            RunSortOffScene(app.get(), device, backbuffer, readback, resource, identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
        if (lowAlphaOnly) {
            RunLowAlphaScenes(app.get(), device, backbuffer, readback, resource,
                              identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
        if (peelBudgetOnly) {
            RunPeelBudgetScene(app.get(), device, backbuffer, readback, resource,
                               identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
        if (maximumLimitOnly) {
            RunMaximumLimitScenes(app.get(), device, backbuffer, readback,
                                  resource, identity);
            if (renderer->effect) {
                reinterpret_cast<IUnknown*>(renderer->effect)->Release();
                renderer->effect = nullptr;
                renderer->hdrTexture = nullptr;
            }
            continue;
        }
        if (intersectionsOnly) {
            // Exercise the unchanged Maximum peeling path explicitly.
            app->TransparentMaterialSortPrecision() = kPeelTier;
            for (bool pmx : {false, true}) {
                std::vector<ModelVertex> a, b;
                std::vector<WORD> ai, bi;
                AppendQuad(a, ai, -1, 1, 1, -1, .5f, kTwoLayerSamplerU, .5f);
                AppendQuad(b, bi, -1, 1, 1, -1, .5f, kTwoLayerSamplerU, .5f);
                for (auto& v : a) v.z += .4f * v.x;
                for (auto& v : b) v.z -= .4f * v.x;
                auto am = BuildModel(device, a, ai, {{kTwoLayerFrontPath, 1.f, 0, ai.size()}}, pmx);
                auto bm = BuildModel(device, b, bi, {{kTwoLayerRearPath, 1.f, 0, bi.size()}}, pmx);
                auto joined = a;
                joined.insert(joined.end(), b.begin(), b.end());
                auto ji = ai;
                for (auto v : bi) ji.push_back(WORD(v + a.size()));
                auto both = BuildModel(device, joined, ji,
                    {{kTwoLayerFrontPath, 1.f, 0, ai.size()},
                     {kTwoLayerRearPath, 1.f, ai.size(), bi.size()}}, pmx);
                std::vector<ModelVertex> ov; std::vector<WORD> oi;
                AppendQuad(ov, oi, -.3f, 1, .3f, -1, .05f, .25f, .5f);
                auto occluder = BuildModel(device, ov, oi, {{kPmxAlphaPath, 1.f, 0, oi.size()}}, pmx);
                occluder->record->comboSelIndex = 2;
                occluder->materials[0].flags = 1;
                for (int scene = 0; scene < 3; ++scene) {
                    for (int camera = 0; camera < 2; ++camera) {
                        D3DMATRIX transform = identity;
                        if (camera) transform._11 = -.8f; // Reflect, preserve both material sides.
                        am->materials[0].flags = bm->materials[0].flags = 1;
                        both->materials[0].flags = both->materials[1].flags = 1;
                        // PMD single-sided paths use a positive scale for the second view.
                        if (camera && !pmx) transform._11 = .8f;
                        device->SetTransform(D3DTS_WORLD, &transform);
                        app->ModelSlot(2) = reinterpret_cast<unsigned char*>(occluder->record.get());
                        D3DLOCKED_RECT lock{};
                        if (scene == 2) RenderOneModelScene(app.get(), device, backbuffer, readback,
                            resource, transform, both.get(), &lock);
                        else RenderArrangement(app.get(), device, backbuffer, readback, resource,
                            transform, scene ? bm.get() : am.get(), scene ? am.get() : bm.get(), &lock);
                        for (int y : {16, 48, 80}) for (int x : {24, 72}) {
                            bool aFront = (x == 24) != (camera && pmx);
                            const int expected[3] = {aFront ? 144 : 127, aFront ? 168 : 151, aFront ? 143 : 176};
                            const DWORD pixel = Row(lock, y)[x];
                            bool correct = std::abs(int(pixel>>16&255)-expected[0]) <= 2 &&
                                std::abs(int(pixel>>8&255)-expected[1]) <= 2 && std::abs(int(pixel&255)-expected[2]) <= 2;
                            ExpectPixel(correct, x, y, pixel, "intersecting materials/models", "half alpha", "per-pixel far-to-near colour");
                        }
                        const DWORD center = Row(lock, 16)[48];
                        ExpectPixel((center & 0xffffff) == 0x20c0e0, 48, 16, center,
                                    "opaque occluder", "solid", "opaque material hides both transparent planes");
                        readback->UnlockRect();
                        app->ModelSlot(2) = nullptr;
                    }
                }
                // Geometry sorting must never rewrite the author's CPU indices.
                Check(pmx ? both->cpuPmxIndices == std::vector<std::int32_t>(ji.begin(), ji.end())
                          : both->cpuIndices.size() == ji.size() * 2, "preserve source indices");

                // Sandwich: the opaque surface is the middle layer instead of
                // the front one, so the frame can no longer come out right just
                // because the last depth comparison rejects the far plane. At
                // every sampled pixel the far transparent fragment sits behind
                // the opaque surface and has to be dropped before it can blend
                // its colour in; the pixel is only the nearest transparent
                // texel over the opaque texel. Both submission orders must
                // reach that same pixel, which is what makes a leak visible: a
                // far fragment blended in first lands on 0x589888, and one
                // blended in after the near layer lands on 0x4888A8.
                auto makeSandwichLayer = [&](float z, const wchar_t* path) {
                    std::vector<ModelVertex> vertices;
                    std::vector<WORD> indices;
                    AppendQuad(vertices, indices, -1.0f, 1.0f, 1.0f, -1.0f, z,
                               kTwoLayerSamplerU, 0.5f);
                    return BuildModel(device, vertices, indices,
                                      {{path, 1.0f, 0, indices.size()}}, pmx);
                };
                auto sandwichNear =
                    makeSandwichLayer(kSandwichNearDepth, kTwoLayerFrontPath);
                auto sandwichOpaque =
                    makeSandwichLayer(kSandwichOpaqueDepth, kPmxAlphaPath);
                auto sandwichFar =
                    makeSandwichLayer(kSandwichFarDepth, kTwoLayerRearPath);
                sandwichOpaque->record->comboSelIndex = 2;
                // The camera loop above leaves its own WORLD matrix behind, so
                // the sandwich is submitted from the identity transform the
                // other scenes use.
                device->SetTransform(D3DTS_WORLD, &identity);
                for (int order = 0; order < 2; ++order) {
                    Model* const first = order == 0 ? sandwichNear.get()
                                                    : sandwichFar.get();
                    Model* const second = order == 0 ? sandwichFar.get()
                                                     : sandwichNear.get();
                    app->ModelSlot(2) = reinterpret_cast<unsigned char*>(
                        sandwichOpaque->record.get());
                    D3DLOCKED_RECT lock{};
                    RenderArrangement(app.get(), device, backbuffer, readback,
                                      resource, identity, first, second, &lock);
                    for (int y = kSampleFirst; y <= kSampleLast;
                         y += kSampleStep) {
                        for (int x = kSampleFirst; x <= kSampleLast;
                             x += kSampleStep) {
                            const DWORD pixel = Row(lock, y)[x];
                            const int red = static_cast<int>(pixel >> 16 & 0xFF);
                            const int green = static_cast<int>(pixel >> 8 & 0xFF);
                            const int blue = static_cast<int>(pixel & 0xFF);
                            ExpectPixel(
                                std::abs(red - kSandwichExpectedRed) <=
                                        kSandwichTolerance &&
                                    std::abs(green - kSandwichExpectedGreen) <=
                                        kSandwichTolerance &&
                                    std::abs(blue - kSandwichExpectedBlue) <=
                                        kSandwichTolerance,
                                x, y, pixel,
                                order == 0
                                    ? "sandwich, near transparent submitted first"
                                    : "sandwich, far transparent submitted first",
                                "half-transparent near layer over the opaque "
                                "middle layer",
                                "nearest transparent over the opaque (0x50B090); "
                                "0x589888 or 0x4888A8 mean the far transparent "
                                "layer reached the frame");
                        }
                    }
                    const DWORD measured = Row(lock, 48)[48];
                    std::printf("sandwich pmx=%d order=%d resource=%d "
                                "measured=(%d,%d,%d) expected=(%d,%d,%d) "
                                "pixel=%08lX\n",
                                pmx ? 1 : 0, order, resource,
                                static_cast<int>(measured >> 16 & 0xFF),
                                static_cast<int>(measured >> 8 & 0xFF),
                                static_cast<int>(measured & 0xFF),
                                kSandwichExpectedRed, kSandwichExpectedGreen,
                                kSandwichExpectedBlue,
                                static_cast<unsigned long>(measured));
                    readback->UnlockRect();
                    app->ModelSlot(2) = nullptr;
                }
            }
            device->SetTransform(D3DTS_WORLD, &identity);
            if (renderer->effect) { reinterpret_cast<IUnknown*>(renderer->effect)->Release(); renderer->effect = nullptr; renderer->hdrTexture = nullptr; }
            app->ModelSlot(0) = app->ModelSlot(1) = nullptr;
            continue;
        }

    CheckTransparentCache(app.get(),device,backbuffer,readback,resource,identity,twoLayerFrontFirstAssembly.get());
    CheckTransparentCache(app.get(),device,backbuffer,readback,resource,identity,pmxTwoLayerFrontFirstAssembly.get());
    CheckObjectCache(app.get(),device,backbuffer,readback,resource,identity,twoLayerFrontModel.get(),twoLayerRearModel.get());
    CheckObjectCache(app.get(),device,backbuffer,readback,resource,identity,pmxTwoLayerFrontModel.get(),pmxTwoLayerRearModel.get());

    // Both arrangements place the two models in opposite slots and give them
    // opposite combo indexes, so the model walk submits them in the opposite
    // order. The far surface first is the order that already renders correctly
    // and stays in as the positive control.
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const first = arrangement == 0 ? front.get() : rear.get();
        Model* const second = arrangement == 0 ? rear.get() : front.get();
        first->record->comboSelIndex = 0;
        second->record->comboSelIndex = 1;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(first->record.get());
        app->ModelSlot(1) = reinterpret_cast<unsigned char*>(second->record.get());

        Check(SUCCEEDED(device->Clear(0, nullptr,
                                      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                      kBackground, 1.0f, 0)),
              "clear the model frame");
        Check(SUCCEEDED(device->BeginScene()), "begin the model frame");
        device->SetRenderState(D3DRS_ALPHAREF, 7);
        if (resource == 0) RenderModelsFixed(app.get());
        else RenderModelsEffect(app.get(), reinterpret_cast<const float*>(&identity));
        DWORD restoredAlpha = 0;
        device->GetRenderState(D3DRS_ALPHAREF, &restoredAlpha);
        Check(restoredAlpha == 7, "restore caller alpha reference");
        Check(SUCCEEDED(device->EndScene()), "end the model frame");
        ReadRenderedPixels(device, backbuffer, readback,
                           "read the rendered model pixels");
        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)),
              "lock the rendered model pixels");
        SampleFrame(arrangement == 0 ? "front model first"
                                     : "rear model first (old-path control)",
                    lock);
        readback->UnlockRect();
        std::printf("arrangement %d sampled 9 pixels\n", arrangement);
    }

    // Scenario 1 and 2: two genuinely half-transparent layers as two separate
    // models, both submission orders. Both layers are always assigned to the
    // translucent pass, so combo index alone decides which of the two is drawn
    // first and the depth buffer must not let the front layer hide the rear one.
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const first =
            arrangement == 0 ? twoLayerFrontModel.get() : twoLayerRearModel.get();
        Model* const second =
            arrangement == 0 ? twoLayerRearModel.get() : twoLayerFrontModel.get();
        D3DLOCKED_RECT lock{};
        RenderArrangement(app.get(), device, backbuffer, readback, resource,
                          identity, first, second, &lock);
        SampleTwoLayerFrame(arrangement == 0
                                ? "two models, front layer first"
                                : "two models, rear layer first",
                            lock,arrangement==0 && !app->TransparentSortCrossObjects());
        readback->UnlockRect();
        std::printf("two-model arrangement %d sampled 3 pixels\n", arrangement);
    }

    // Both model submission orders must retain the rear transparent layer.
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const first = arrangement == 0 ? largeRearModel.get()
                                              : twoLayerFrontModel.get();
        Model* const second = arrangement == 0 ? twoLayerFrontModel.get()
                                               : largeRearModel.get();
        D3DLOCKED_RECT lock{};
        RenderArrangement(app.get(), device, backbuffer, readback, resource,
                          identity, first, second, &lock);
        SampleTwoLayerFrame(arrangement == 0
                                ? "large material model first, front model second"
                                : "front model first, large material model second",
                            lock,arrangement==1 && !app->TransparentSortCrossObjects());
        readback->UnlockRect();
        std::printf("large-material arrangement %d sampled 3 pixels\n",
                    arrangement);
    }

    // Scenario 3: both half-transparent layers inside one model, one material
    // each. Each arrangement puts the front layer at kNearDepth and the rear
    // layer at kFarDepth and differs only in which material the walk reaches
    // first, so the same source-over expectation applies to both.
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const model = arrangement == 0 ? twoLayerFrontFirstAssembly.get()
                                              : twoLayerRearFirstAssembly.get();
        model->record->comboSelIndex = 0;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model->record.get());
        app->ModelSlot(1) = nullptr;

        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(device->Clear(0, nullptr,
                                      D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                      kBackground, 1.0f, 0)),
              "clear the one-model frame");
        Check(SUCCEEDED(device->BeginScene()), "begin the one-model frame");
        device->SetRenderState(D3DRS_ALPHAREF, 7);
        if (resource == 0) RenderModelsFixed(app.get());
        else RenderModelsEffect(app.get(), reinterpret_cast<const float*>(&identity));
        // The same restore check the two-model scenarios make: the opaque
        // coverage pre-pass raises D3DRS_ALPHAREF and must put the caller's
        // value back. Only the multi-material model can exercise it on the
        // material path a second time, so it is asserted here as well.
        DWORD restoredAlpha = 0;
        device->GetRenderState(D3DRS_ALPHAREF, &restoredAlpha);
        Check(restoredAlpha == 7, "restore caller alpha reference (one model)");
        Check(SUCCEEDED(device->EndScene()), "end the one-model frame");
        ReadRenderedPixels(device, backbuffer, readback,
                           "read the one-model pixels");
        Check(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)),
              "lock the one-model pixels");
        SampleTwoLayerFrame(
            arrangement == 0 ? "one model, front material first"
                             : "one model, rear material first",
            lock);
        readback->UnlockRect();
        std::printf("one-model arrangement %d sampled 3 pixels\n", arrangement);
    }

    // The PMX twins of the same three scenes. Geometry, textures, depths and
    // submissions are identical to the PMD runs above; only the model ABI
    // differs (physicsMode 2, 32-bit indices, morph pools), so the same
    // source-over expectation must hold and any divergence is a PMX-path
    // defect rather than a fixture difference.
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const first = arrangement == 0 ? pmxTwoLayerFrontModel.get()
                                              : pmxTwoLayerRearModel.get();
        Model* const second = arrangement == 0 ? pmxTwoLayerRearModel.get()
                                               : pmxTwoLayerFrontModel.get();
        D3DLOCKED_RECT lock{};
        RenderArrangement(app.get(), device, backbuffer, readback, resource,
                          identity, first, second, &lock);
        SampleTwoLayerFrame(arrangement == 0
                                ? "PMX two models, front layer first"
                                : "PMX two models, rear layer first",
                            lock,arrangement==0 && !app->TransparentSortCrossObjects());
        readback->UnlockRect();
        std::printf("pmx two-model arrangement %d sampled 3 pixels\n",
                    arrangement);
    }
    for (int arrangement = 0; arrangement < 2; ++arrangement) {
        Model* const model = arrangement == 0
            ? pmxTwoLayerFrontFirstAssembly.get()
            : pmxTwoLayerRearFirstAssembly.get();
        D3DLOCKED_RECT lock{};
        RenderOneModelScene(app.get(), device, backbuffer, readback, resource,
                            identity, model, &lock);
        SampleTwoLayerFrame(arrangement == 0
                                ? "PMX one model, front material first"
                                : "PMX one model, rear material first",
                            lock);
        readback->UnlockRect();
        std::printf("pmx one-model arrangement %d sampled 3 pixels\n",
                    arrangement);
    }

    // PMX double-sided material (flags bit 0). The material record asks for no
    // culling, so the pass draws both windings. The two layers still blend in
    // reverse-depth order, so the result is the same source-over pair.
    {
        D3DLOCKED_RECT lock{};
        RenderOneModelScene(app.get(), device, backbuffer, readback, resource,
                            identity, pmxDoubleSidedScene.get(), &lock);
        SampleTwoLayerFrame("PMX double-sided material", lock);
        readback->UnlockRect();
        std::printf("pmx double-sided scene sampled 3 pixels\n");
    }

    // PMX material alpha: Diffuse.a is 0.5 on an opaque texture, so the surface
    // blends because of the material colour instead of an alpha texel. The rear
    // layer keeps its half-transparent texel. Expected result is the rear layer
    // at full strength first (its own texel alpha 0x80 over white), then the
    // front layer with alpha 0.5 over that.
    {
        D3DLOCKED_RECT lock{};
        pmxMaterialAlphaScene->materials[0].diffuse[3] = 1.0f;
        RenderOneModelScene(app.get(), device, backbuffer, readback, resource,
                            identity, pmxMaterialAlphaScene.get(), &lock);
        Check((Row(lock,16)[24] & 0xffffff) == 0x20c0e0, "opaque material before alpha change");
        readback->UnlockRect();
        pmxMaterialAlphaScene->materials[0].diffuse[3] = 0.5f;
        RenderOneModelScene(app.get(), device, backbuffer, readback, resource,
                            identity, pmxMaterialAlphaScene.get(), &lock);
        SampleMaterialAlphaFrame("PMX material alpha 1.0 -> 0.5", lock);
        readback->UnlockRect();
        std::printf("pmx material-alpha scene sampled 3 pixels\n");
    }

    // Translucent display: displayState forces every material translucent, yet
    // the model must still render as one record-order pass at every transparent
    // sort tier.
    RunDisplayStateScene(app.get(), device, backbuffer, readback, resource,
                         identity);

        for (int camera = 0; camera < 2; ++camera) {
            D3DMATRIX shifted = identity;
            shifted._41 = camera ? .01f : 0;
            device->SetTransform(D3DTS_WORLD, &shifted);
            D3DLOCKED_RECT lock{};
            RenderOneModelScene(app.get(),device,backbuffer,readback,resource,shifted,interleaved.get(),&lock);
            for (int x : {24,72}) {
                const DWORD pixel = Row(lock,16)[x];
                const int expected[3] = {x == 24 ? 144 : 127, x == 24 ? 168 : 151, x == 24 ? 143 : 176};
                const bool correct = std::abs(int(pixel>>16&255)-expected[0])<=1 &&
                    std::abs(int(pixel>>8&255)-expected[1])<=1 && std::abs(int(pixel&255)-expected[2])<=1;
                ExpectPixel(correct,x,16,pixel,"interleaved PMX / camera change","half alpha","both local layer orders");
            }
            readback->UnlockRect();
        }
        device->SetTransform(D3DTS_WORLD,&identity);

        // Native ZValuePlotTec writes projected depth without clipping texel
        // alpha. The former shadow-alpha experiment was withdrawn; material
        // caster gates live above this shader and are not exercised here.
        if (resource != 0) {
            using namespace mikumikustudio::d3dx;
            for (const float depthShift : {0.0f, 0.125f}) {
                D3DMATRIX lightProjection = identity;
                lightProjection._43 = depthShift;
                Check(SUCCEEDED(FxSetTechnique(renderer->effect, "ZValuePlotTec")),
                      "select the existing native depth technique");
                Check(SUCCEEDED(FxSetMatrix(renderer->effect, "matLightViewProj",
                            reinterpret_cast<const D3DXMATRIXF*>(&lightProjection))),
                      "set the native light projection");
                Check(SUCCEEDED(device->SetTexture(1, nearTexture)), "bind the three-alpha-band texture");
                Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                              kBackground, 1, 0)), "clear native depth fixture");
                Check(SUCCEEDED(device->BeginScene()), "begin native depth fixture");
                UINT passes = 0;
                Check(SUCCEEDED(FxBegin(renderer->effect, &passes)) && passes == 1,
                      "begin the single native depth effect pass");
                Check(SUCCEEDED(FxBeginPass(renderer->effect, 0)), "begin native depth pass");
                Check(SUCCEEDED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)), "set depth fixture culling");
                Check(SUCCEEDED(device->SetFVF(kModelFvf)), "set depth fixture vertex format");
                Check(SUCCEEDED(device->SetStreamSource(0, front->vertexBuffer, 0, kModelStride)), "bind depth fixture vertices");
                Check(SUCCEEDED(device->SetIndices(front->indexBuffer)), "bind depth fixture indices");
                Check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                    front->record->vertexCount, 0, front->record->indexCount / 3)), "draw native depth fixture");
                Check(SUCCEEDED(FxEndPass(renderer->effect)), "end native depth pass");
                Check(SUCCEEDED(FxEnd(renderer->effect)), "end native depth effect");
                Check(SUCCEEDED(device->EndScene()), "end native depth fixture");
                ReadRenderedPixels(device, backbuffer, readback, "read native depth fixture");
                D3DLOCKED_RECT lock{};
                Check(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock native depth pixels");
                const int expectedDepth = static_cast<int>(std::lround(255.0f * (kNearDepth + depthShift)));
                for (int band = 0; band < 3; ++band) {
                    const int x = 48, y = 80 - band * 32;
                    const DWORD pixel = Row(lock,y)[x];
                    const bool correct = int(pixel >> 16 & 255) == expectedDepth &&
                        (pixel & 0xffff) == 0 && (pixel >> 24) == 255;
                    ExpectPixel(correct,x,y,pixel,"native projected shadow depth","transparent/half/opaque",
                                "round(255 * projected z/w), G=B=0 and A=255 for all texel alpha bands");
                }
                Check(SUCCEEDED(readback->UnlockRect()), "unlock native depth pixels");
                std::printf("native-shadow resource=%d depth-shift=%.3f expected-red=%d bands=3\n",
                            resource, depthShift, expectedDepth);
            }
        }
        if (renderer->effect) {
            reinterpret_cast<IUnknown*>(renderer->effect)->Release();
            renderer->effect = nullptr;
            renderer->hdrTexture = nullptr;
        }
    }
#if !MMD_ORIGINAL_PROFILE
    if (!sphereOnly && !crossObjectOnly && targetWidth >= 64 &&
        targetHeight >= 64)
        CheckBatchedPixels(device, backbuffer, readback, targetWidth, targetHeight);
#endif
    app->ModelSlot(0) = nullptr;
    app->ModelSlot(1) = nullptr;
#if !MMD_ORIGINAL_PROFILE
    mikumikustudio::transparent_sort_cache::Release(app.get());
#endif
    readback->Release();
    if (g_resolveTarget) {
        g_resolveTarget->Release();
        g_resolveTarget = nullptr;
    }
    backbuffer->Release();
    ReleaseFixtureTexture(renderer.get(), 4, pmxOpaqueTexture);
    ReleaseFixtureTexture(renderer.get(), 3, twoLayerFrontTexture);
    ReleaseFixtureTexture(renderer.get(), 2, twoLayerRearTexture);
    ReleaseFixtureTexture(renderer.get(), 1, farTexture);
    ReleaseFixtureTexture(renderer.get(), 0, nearTexture);
    toonTexture->Release();
    device->Release();
    renderer->device = nullptr;
    d3d->Release();
    DestroyWindow(window);

    if (sphereOnly) {
        Check(g_sphereFrameCount == SphereExpectedFrameCount(),
              "the sphere-only run rendered the frame matrix of its mode");
        if (g_sphereRecord != INVALID_HANDLE_VALUE) {
            char summary[512];
            const int length = std::snprintf(
                summary, sizeof(summary),
                "{\"summary\":true,\"sphereMode\":%d,\"frames\":%d,"
                "\"expectedFrames\":%d,\"failedFrames\":%d,"
                "\"failures\":%d,\"mainFrames\":%d,"
                "\"controlFrames\":%d,\"uv1ProbeFrames\":%d,"
                "\"tolerance\":%d}",
                g_sphereMode, g_sphereFrameCount, SphereExpectedFrameCount(),
                g_sphereFailedFrames, g_failures, SphereMainFrameCount(),
                SphereControlFrameCount(), SphereUv1ProbeFrameCount(),
                kSphereTolerance);
            Check(length > 0 && static_cast<std::size_t>(length) < sizeof(summary),
                  "format the sphere-only summary record");
            AppendSphereRecord(summary);
            Check(CloseHandle(g_sphereRecord) != 0,
                  "close the sphere-only frame record");
            g_sphereRecord = INVALID_HANDLE_VALUE;
        }
        std::printf("SPHERE_ONLY END sphereMode=%d frames=%d "
                    "expected_frames=%d failed_frames=%d failures=%d "
                    "tolerance=%d\n",
                    g_sphereMode, g_sphereFrameCount,
                    SphereExpectedFrameCount(), g_sphereFailedFrames,
                    g_failures, kSphereTolerance);
    }
    if (crossObjectOnly) {
        // Write the summary and the END line before the fatal frame-matrix
        // assertions, so a contract violation still leaves its numbers on disk
        // instead of vanishing with the exit.
        if (g_crossObjectRecord != INVALID_HANDLE_VALUE) {
            char summary[1024];
            const int length = std::snprintf(
                summary, sizeof(summary),
                "{\"summary\":true,\"frames\":%d,\"expectedFrames\":%d,"
                "\"failedFrames\":%d,\"failures\":%d,"
                "\"precisionRestores\":%d,\"expectedPrecisionRestores\":%d,"
                "\"scopeRestores\":%d,\"expectedScopeRestores\":%d,"
                "\"tolerance\":%d}",
                g_crossObjectFrameCount, kCrossObjectExpectedFrameCount,
                g_crossObjectFailedFrames, g_failures,
                g_crossObjectPrecisionRestores,
                kCrossObjectPrecisionRestoreCount,
                g_crossObjectScopeToggleRestores,
                kCrossObjectScopeToggleRestoreCount, kSphereTolerance);
            Check(length > 0 &&
                      static_cast<std::size_t>(length) < sizeof(summary),
                  "format the cross-object summary record");
            AppendCaptureRecord(g_crossObjectRecord, summary,
                                "the cross-object frame record is open",
                                "write the cross-object summary record");
            Check(CloseHandle(g_crossObjectRecord) != 0,
                  "close the cross-object frame record");
            g_crossObjectRecord = INVALID_HANDLE_VALUE;
        }
        std::printf("CROSS_OBJECT END capture-dir=%ls frames=%d "
                    "expected_frames=%d failed_frames=%d failures=%d "
                    "precision_restores=%d/%d scope_restores=%d/%d "
                    "tolerance=%d\n",
                    g_crossObjectCaptureDir.c_str(), g_crossObjectFrameCount,
                    kCrossObjectExpectedFrameCount,
                    g_crossObjectFailedFrames, g_failures,
                    g_crossObjectPrecisionRestores,
                    kCrossObjectPrecisionRestoreCount,
                    g_crossObjectScopeToggleRestores,
                    kCrossObjectScopeToggleRestoreCount, kSphereTolerance);
        Check(g_crossObjectFrameCount == kCrossObjectExpectedFrameCount,
              "the cross-object run rendered the frame matrix of its mode");
        Check(g_crossObjectPrecisionRestores ==
                  kCrossObjectPrecisionRestoreCount,
              "the cross-object run recorded every precision Off restore");
        Check(g_crossObjectScopeToggleRestores ==
                  kCrossObjectScopeToggleRestoreCount,
              "the cross-object run recorded every scope toggle restore");
        if (g_crossObjectFailedFrames != 0) {
            std::fprintf(stderr, "FAIL: %d cross-object frames failed\n",
                         g_crossObjectFailedFrames);
            return 1;
        }
    }
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "FAIL: %d alpha-occlusion pixel samples\n",
                     g_failures);
        return 1;
    }
    if (lowAlphaOnly) {
        std::puts("Low-alpha layer accumulation pixel regression passed.");
        return 0;
    }
    if (peelBudgetOnly) {
        std::puts("Peel-budget fallback pixel regression passed.");
        return 0;
    }
    if (maximumLimitOnly) {
        std::puts("Maximum-limit layer accumulation pixel regression passed.");
        return 0;
    }
    if (sphereOnly) {
        std::printf("Native PMX sphereMode=%d sphere-alpha GPU pixel regression "
                    "passed.\n",
                    g_sphereMode);
        return 0;
    }
    if (crossObjectOnly) {
        std::puts("Cross-object transparent sort capture passed.");
        return 0;
    }
    std::puts("Semi-transparent material occlusion pixel regressions passed.");
    return 0;
}
