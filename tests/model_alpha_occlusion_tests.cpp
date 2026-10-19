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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/transparent_geometry.hpp"
#endif
#include "mikumikustudio/ported_funcs.hpp"

using namespace mikumikustudio;

namespace {

constexpr int kWidth = 96;
constexpr int kHeight = 96;
constexpr DWORD kModelFvf = 274;  // XYZ | NORMAL | TEX1, the body vertex format
constexpr UINT kModelStride = 32;
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
// morph pools the effect pass indexes per material.
std::unique_ptr<Model> BuildModel(IDirect3DDevice9* device,
                                  const std::vector<ModelVertex>& vertices,
                                  const std::vector<WORD>& indices,
                                  const std::vector<MaterialSpec>& specs,
                                  bool pmx = false) {
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
    record.pmxAdditionalUvCount = 0;  // keeps the 274/32 vertex format
    if (pmx) {
        mdl::MaterialMorphAdd(reinterpret_cast<unsigned char*>(&record)) =
            model->morphAdd.data();
        mdl::MaterialMorphMul(reinterpret_cast<unsigned char*>(&record)) =
            model->morphMul.data();
    }

    const UINT vertexBytes =
        static_cast<UINT>(vertices.size() * sizeof(ModelVertex));
    Check(SUCCEEDED(device->CreateVertexBuffer(vertexBytes, 0, kModelFvf,
                                               D3DPOOL_MANAGED,
                                               &model->vertexBuffer, nullptr)),
          "create model vertices");
    void* vertexData = nullptr;
    Check(SUCCEEDED(model->vertexBuffer->Lock(0, 0, &vertexData, 0)),
          "lock model vertices");
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
void SampleTwoLayerFrame(const char* submitted, const D3DLOCKED_RECT& lock) {
    const auto within = [](int value, int expected) {
        return value >= expected - 1 && value <= expected + 1;
    };
    for (int x : {24, 48, 72}) {
        const DWORD pixel = Row(lock, 16)[x];
        const int red = static_cast<int>(pixel >> 16 & 0xFF);
        const int green = static_cast<int>(pixel >> 8 & 0xFF);
        const int blue = static_cast<int>(pixel & 0xFF);
        ExpectPixel(within(red, kTwoLayerExpectedRed) &&
                        within(green, kTwoLayerExpectedGreen) &&
                        within(blue, kTwoLayerExpectedBlue),
                    x, 16, pixel, submitted, "two half-transparent layers",
                    "the rear layer then the front layer source-over "
                    "(about 0x90A88F); a single layer or the clear colour "
                    "proves the draw order or the depth test went wrong");
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
#if MMD_ORIGINAL_PROFILE
        Check(forward == frontOnly, "original profile retains one authored material walk");
        Check(reversed != frontOnly, "Off preserves authored material order instead of normalizing both orders");
        Check(pair(front.get(), rear.get()) == frontOnly,
              "Off does not move the later opaque model ahead of translucent foreground");
#else
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
#endif
        Check(pair(rear.get(), front.get()) == reversed,
              "Off follows native model order as well as material order");

        // The near-first CPU order must be restored after a sort has rewritten
        // this single material's GPU buffer into far-first triangle order.
        auto stack = BuildModel(device, bothVertices, nearFirst,
            {{kTwoLayerFrontPath, 1, 0, 12}}, pmx);
        Check(one(stack.get()) == frontOnly, "Off retains authored triangle order inside a material");
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Low;
        const auto sorted = one(stack.get());
        if (app->AutoTransparentMaterialSortingEnabled())
            Check(sorted != frontOnly, "enabled sorting changes the near-first overlap fixture");
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

}  // namespace

int main(int argc, char** argv) {
    // --msaa combines with any of the scene switches, so every argument is
    // read instead of only the first one.
    bool intersectionsOnly = false;
    bool lowAlphaOnly = false;
    bool peelBudgetOnly = false;
    bool maximumLimitOnly = false;
    bool maximumFullSize = false;
    bool msaaRequested = false;
    bool sortOffOnly = false;
    bool precisionRequested = false;
    TransparentSortPrecision precision = TransparentSortPrecision::Standard;
    const char* precisionName = "standard";
    HMODULE mmeMarker = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--precision") == 0) {
            if (++i == argc) {
                std::fputs("--precision needs low, standard, high or maximum\n", stderr);
                return 2;
            }
            precisionName = argv[i];
            if (std::strcmp(precisionName, "low") == 0) precision = TransparentSortPrecision::Low;
            else if (std::strcmp(precisionName, "standard") == 0) precision = TransparentSortPrecision::Standard;
            else if (std::strcmp(precisionName, "high") == 0) precision = TransparentSortPrecision::High;
            else if (std::strcmp(precisionName, "maximum") == 0) precision = TransparentSortPrecision::Maximum;
            else {
                std::fputs("--precision needs low, standard, high or maximum; use --sort-off for Off\n", stderr);
                return 2;
            }
            precisionRequested = true;
        } else if (std::strcmp(argv[i], "--mme-marker") == 0) {
            Check(i + 1 < argc, "MME presence marker requires a DLL path");
            mmeMarker = LoadLibraryA(argv[++i]);
            Check(mmeMarker && GetModuleHandleW(L"MMEffect.dll") == mmeMarker,
                  "load inert MMEffect.dll presence marker");
        } else if (std::strcmp(argv[i], "--sort-off") == 0)
            sortOffOnly = true;
        else if (std::strcmp(argv[i], "--intersections") == 0)
            intersectionsOnly = true;
        else if (std::strcmp(argv[i], "--low-alpha") == 0)
            lowAlphaOnly = true;
        else if (std::strcmp(argv[i], "--peel-budget") == 0)
            peelBudgetOnly = true;
        else if (std::strcmp(argv[i], "--maximum-limit") == 0)
            maximumLimitOnly = true;
        else if (std::strcmp(argv[i], "--maximum-full-size") == 0) {
            // Same mode and same scenes, on the original 96x96 stress target.
            maximumLimitOnly = true;
            maximumFullSize = true;
        } else if (std::strcmp(argv[i], "--msaa") == 0)
            msaaRequested = true;
    }
    if (int(intersectionsOnly) + int(lowAlphaOnly) + int(peelBudgetOnly) +
            int(maximumLimitOnly) + int(sortOffOnly) > 1) {
        std::fputs("Choose a single scene mode\n", stderr);
        return 2;
    }
    const bool ordinary = !intersectionsOnly && !lowAlphaOnly && !peelBudgetOnly &&
        !maximumLimitOnly && !sortOffOnly;
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // One local target size drives the window, the device, the readback surface
    // and the MSAA resolve surface. Maximum defaults to 16x16; the other modes
    // and the explicit full-size stress mode keep the original 96x96.
    const int targetWidth = maximumLimitOnly && !maximumFullSize ? 16 : kWidth;
    const int targetHeight =
        maximumLimitOnly && !maximumFullSize ? 16 : kHeight;
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
                            lock);
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
                            lock);
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
                            lock);
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
    if (targetWidth >= 64 && targetHeight >= 64)
#if !MMD_ORIGINAL_PROFILE
        CheckBatchedPixels(device, backbuffer, readback, targetWidth, targetHeight);
#endif
    app->ModelSlot(0) = nullptr;
    app->ModelSlot(1) = nullptr;
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
    std::puts("Semi-transparent material occlusion pixel regressions passed.");
    return 0;
}
