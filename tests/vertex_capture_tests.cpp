// MAINTENANCE: regression-keep; hidden-window GPU test for the D3D9 vertex
// shader output capture (include/mikumikustudio/vertex_capture.hpp).
//
// What is verified, on a real HAL device with a hidden window:
//   * the captured float4 is the vertex shader's POSITION output and not the
//     input vertex data (vs_2_0 and vs_3_0, with measurable scale + offset and
//     two matrix multiplies in between)
//   * a non-zero firstVertex and a non-zero stream offset address the right
//     vertices
//   * vertex shader / pixel shader / declaration / stream / viewport /
//     render target / depth stencil / render state / constants are unchanged
//     across a capture
//   * a shader-written point size does not corrupt the one-pixel-per-vertex
//     mapping
//   * the documented failure cases report an error instead of returning data
//
// The test needs no project model and no MME DLL.  It loads d3dcompiler_47.dll
// dynamically, so the test target needs no extra import library.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/vertex_capture.hpp"
#include "../src/render/antialias_motion_cache.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <d3dcommon.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

// ---- tiny check harness ---------------------------------------------------
int gChecks = 0;

void Check(bool ok, const char* message) {
    ++gChecks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void Check(bool ok, const std::string& message) {
    Check(ok, message.c_str());
}

void Note(const std::string& message) {
    std::printf("note: %s\n", message.c_str());
    std::fflush(stdout);
}

std::string HResultText(HRESULT result) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(result));
    return buffer;
}

DWORD FloatBits(float value) {
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string ShaderVersionText(DWORD version) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u", (version >> 8) & 0xFF,
                  version & 0xFF);
    return buffer;
}

// ---- d3dcompiler, loaded so the test target needs no import library -------
typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR,
                                   const D3D_SHADER_MACRO*, ID3DInclude*,
                                   LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                   ID3DBlob**);

CompileFn gCompile = nullptr;

void LoadCompiler() {
    HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
    Check(module != nullptr, "d3dcompiler_47.dll must be available");
    gCompile = reinterpret_cast<CompileFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
    Check(gCompile != nullptr, "D3DCompile must be exported");
}

std::vector<DWORD> CompileVertexShader(const char* source, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result =
        gCompile(source, std::strlen(source) + 1, "vertex_capture_tests",
                 nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    if (FAILED(result)) {
        std::string text = "compile ";
        text += target;
        text += " failed";
        if (errors != nullptr) {
            text += ": ";
            text += static_cast<const char*>(errors->GetBufferPointer());
        }
        Check(false, text);
    }
    if (errors != nullptr) errors->Release();
    const DWORD* words = static_cast<const DWORD*>(code->GetBufferPointer());
    std::vector<DWORD> bytecode(words, words + code->GetBufferSize() / 4);
    code->Release();
    return bytecode;
}

// ---- test geometry --------------------------------------------------------
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

const Vertex kVertices[8] = {
    {{-1.00f, 0.50f, -0.25f}, {0.0f, 0.0f, 1.0f}, {0.00f, 0.00f}},
    {{-0.50f, -1.00f, 0.75f}, {0.0f, 1.0f, 0.0f}, {0.25f, 0.50f}},
    {{0.00f, 1.50f, 0.50f}, {1.0f, 0.0f, 0.0f}, {0.50f, 0.25f}},
    {{0.75f, -0.25f, 1.00f}, {0.0f, 0.0f, 1.0f}, {0.75f, 1.00f}},
    {{1.50f, 0.25f, -0.50f}, {0.5f, 0.5f, 0.5f}, {1.00f, 0.50f}},
    {{-1.50f, -0.75f, 0.25f}, {0.3f, 0.6f, 0.9f}, {0.10f, 0.90f}},
    {{0.25f, 0.75f, -1.00f}, {0.9f, 0.2f, 0.4f}, {0.60f, 0.20f}},
    {{-0.25f, -0.50f, 0.00f}, {0.2f, 0.8f, 0.1f}, {0.90f, 0.70f}},
};

// Constant registers the shader multiplies with; the CPU reference below
// mirrors the dp4 chains the compiler emits for `mul(v, matrix)`.
const float kWorld[4][4] = {
    {1.50f, 0.25f, -0.50f, 0.00f},
    {0.00f, 2.00f, 0.50f, 0.00f},
    {-0.25f, 0.00f, 1.25f, 0.00f},
    {0.50f, -0.75f, 0.25f, 1.00f},
};
const float kViewProjection[4][4] = {
    {0.90f, 0.10f, 0.00f, 0.00f},
    {-0.20f, 0.80f, 0.15f, 0.00f},
    {0.00f, -0.10f, 1.10f, 0.00f},
    {0.30f, 0.40f, -0.60f, 1.00f},
};

// Expected POSITION for `p = pos * 2 + 1; mul(mul(float4(p, 1), World),
// ViewProjection)`.  The compiler turns `mul(v, matrix)` into one dp4 per
// component against the matrix' constant registers, so the result component j
// is dot(v, register j) - the mirror below follows that order exactly.
void ExpectedClip(const float position[3], float out[4]) {
    const float p[4] = {position[0] * 2.0f + 1.0f, position[1] * 2.0f + 1.0f,
                        position[2] * 2.0f + 1.0f, 1.0f};
    float world[4];
    for (int row = 0; row < 4; ++row) {
        world[row] = p[0] * kWorld[row][0] + p[1] * kWorld[row][1] +
                     p[2] * kWorld[row][2] + p[3] * kWorld[row][3];
    }
    for (int row = 0; row < 4; ++row) {
        out[row] = world[0] * kViewProjection[row][0] +
                   world[1] * kViewProjection[row][1] +
                   world[2] * kViewProjection[row][2] +
                   world[3] * kViewProjection[row][3];
    }
}

const char* kHlslSource =
    "float4x4 gWorld;\n"
    "float4x4 gViewProjection;\n"
    "struct VsOut {\n"
    "  float4 position : POSITION;\n"
    "  float2 uv : TEXCOORD0;\n"
    "};\n"
    "VsOut main(float3 pos : POSITION, float3 nrm : NORMAL,\n"
    "           float2 uv : TEXCOORD0) {\n"
    "  VsOut output;\n"
    "  float3 p = pos * 2.0 + 1.0;\n"
    "  output.position = mul(mul(float4(p, 1.0), gWorld), gViewProjection);\n"
    "  output.uv = uv + nrm.xy;\n"
    "  return output;\n"
    "}\n";

// ---- hand-assembled shaders ----------------------------------------------
std::vector<DWORD> Tokens(std::initializer_list<DWORD> words) {
    return std::vector<DWORD>(words);
}

// dcl_position v0 (0x0200001F, usage POSITION, dest v0)
const DWORD kDclPositionV0[] = {0x0200001F, 0x80000000, 0x900F0000};

IDirect3DVertexShader9* TryCreateShader(IDirect3DDevice9* device,
                                        const std::vector<DWORD>& code,
                                        const char* what) {
    IDirect3DVertexShader9* shader = nullptr;
    const HRESULT result = device->CreateVertexShader(code.data(), &shader);
    if (FAILED(result)) {
        Note(std::string("device rejected the ") + what + " shader (" +
             HResultText(result) + "); case skipped");
        return nullptr;
    }
    return shader;
}

// ---- fixtures -------------------------------------------------------------
struct Fixture {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DVertexShader9* shader2 = nullptr;
    IDirect3DVertexShader9* shader3 = nullptr;
    IDirect3DPixelShader9* appPixelShader = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* depthStencil = nullptr;
    std::vector<DWORD> code2;
    std::vector<DWORD> code3;
};

void CreateFixture(Fixture& fixture) {
    // WS_POPUP and no ShowWindow: the window is never visible, so the test
    // never touches the desktop.
    fixture.window = CreateWindowW(L"STATIC", L"vertex capture regression",
                                   WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    Check(fixture.window != nullptr, "create the hidden device window");

    fixture.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(fixture.d3d != nullptr, "Direct3DCreate9");

    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.hDeviceWindow = fixture.window;
    parameters.EnableAutoDepthStencil = FALSE;

    const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING |
                        D3DCREATE_FPU_PRESERVE;
    HRESULT result = fixture.d3d->CreateDevice(
        0, D3DDEVTYPE_HAL, fixture.window, flags, &parameters, &fixture.device);
    Check(SUCCEEDED(result), "CreateDevice(HAL, hardware vertex processing) " +
                                 HResultText(result));

    D3DADAPTER_IDENTIFIER9 identifier{};
    D3DCAPS9 caps{};
    fixture.device->GetDeviceCaps(&caps);
    if (SUCCEEDED(fixture.d3d->GetAdapterIdentifier(0, 0, &identifier))) {
        char text[512]{};
        std::snprintf(text, sizeof(text),
                      "device: %s, vs %s ps %s, MaxTexture %ux%u, "
                      "MaxStreams %lu",
                      identifier.Description,
                      ShaderVersionText(caps.VertexShaderVersion).c_str(),
                      ShaderVersionText(caps.PixelShaderVersion).c_str(),
                      caps.MaxTextureWidth, caps.MaxTextureHeight,
                      static_cast<unsigned long>(caps.MaxStreams));
        Note(text);
    }

    IDirect3DTexture9* probeTarget = nullptr;
    const HRESULT format = fixture.device->CreateTexture(
        4, 4, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
        &probeTarget, nullptr);
    Note(std::string("A32B32G32R32F render target support check: ") +
         HResultText(format));
    if (probeTarget != nullptr) probeTarget->Release();

    // Vertex data.
    result = fixture.device->CreateVertexBuffer(
        sizeof(kVertices), 0, 0, D3DPOOL_MANAGED, &fixture.vertexBuffer,
        nullptr);
    Check(SUCCEEDED(result), "CreateVertexBuffer");
    void* data = nullptr;
    Check(SUCCEEDED(fixture.vertexBuffer->Lock(0, sizeof(kVertices), &data, 0)),
          "lock the vertex buffer");
    std::memcpy(data, kVertices, sizeof(kVertices));
    fixture.vertexBuffer->Unlock();

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
         0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,
         0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    result = fixture.device->CreateVertexDeclaration(elements,
                                                     &fixture.declaration);
    Check(SUCCEEDED(result), "CreateVertexDeclaration");

    fixture.device->GetRenderTarget(0, &fixture.backBuffer);
    result = fixture.device->CreateDepthStencilSurface(
        64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
        &fixture.depthStencil, nullptr);
    Check(SUCCEEDED(result), "CreateDepthStencilSurface");

    fixture.code2 = CompileVertexShader(kHlslSource, "vs_2_0");
    fixture.code3 = CompileVertexShader(kHlslSource, "vs_3_0");
    result = fixture.device->CreateVertexShader(fixture.code2.data(),
                                                &fixture.shader2);
    Check(SUCCEEDED(result), "CreateVertexShader(vs_2_0)");
    result = fixture.device->CreateVertexShader(fixture.code3.data(),
                                                &fixture.shader3);
    Check(SUCCEEDED(result), "CreateVertexShader(vs_3_0)");

    const std::vector<DWORD> pixelCode =
        CompileVertexShader("float4 main() : COLOR { return float4(1,1,1,1); }",
                            "ps_2_0");
    result = fixture.device->CreatePixelShader(pixelCode.data(),
                                               &fixture.appPixelShader);
    Check(SUCCEEDED(result), "CreatePixelShader(ps_2_0)");

    // Bind the state a real draw would have bound.
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, 0, sizeof(Vertex));
    fixture.device->SetVertexDeclaration(fixture.declaration);
    fixture.device->SetVertexShader(fixture.shader2);
    fixture.device->SetPixelShader(fixture.appPixelShader);
    fixture.device->SetVertexShaderConstantF(0, &kWorld[0][0], 4);
    fixture.device->SetVertexShaderConstantF(4, &kViewProjection[0][0], 4);
    fixture.device->SetRenderTarget(0, fixture.backBuffer);
    fixture.device->SetDepthStencilSurface(fixture.depthStencil);
    Check(SUCCEEDED(fixture.device->BeginScene()), "BeginScene");
}

void DestroyFixture(Fixture& fixture) {
    if (fixture.device != nullptr) fixture.device->EndScene();
    if (fixture.shader2 != nullptr) fixture.shader2->Release();
    if (fixture.shader3 != nullptr) fixture.shader3->Release();
    if (fixture.appPixelShader != nullptr) fixture.appPixelShader->Release();
    if (fixture.declaration != nullptr) fixture.declaration->Release();
    if (fixture.vertexBuffer != nullptr) fixture.vertexBuffer->Release();
    if (fixture.depthStencil != nullptr) fixture.depthStencil->Release();
    if (fixture.backBuffer != nullptr) fixture.backBuffer->Release();
    if (fixture.device != nullptr) fixture.device->Release();
    if (fixture.d3d != nullptr) fixture.d3d->Release();
    if (fixture.window != nullptr) DestroyWindow(fixture.window);
}

// ---- captures -------------------------------------------------------------
// Captures vertexCount vertices from firstVertex and compares each with the
// CPU reference, also proving the result is not the input vertex data.
// `expectedFirstVertex` is the index into kVertices that the first captured
// vertex must correspond to: it equals firstVertex, plus any vertex offset the
// bound stream adds.
void CheckCaptureMatches(const char* label, Fixture& fixture,
                         unsigned firstVertex, unsigned vertexCount,
                         unsigned expectedFirstVertex) {
    std::vector<std::array<float, 4>> captured;
    std::string error;
    const bool ok = mikumikustudio::vertex_capture::Capture(
        fixture.device, firstVertex, vertexCount, captured, error);
    Check(ok, std::string(label) + ": capture failed: " + error);
    Check(captured.size() == vertexCount,
          std::string(label) + ": captured vertex count");

    float worst = 0.0f;
    for (unsigned index = 0; index < vertexCount; ++index) {
        const Vertex& vertex = kVertices[expectedFirstVertex + index];
        float expected[4]{};
        ExpectedClip(vertex.position, expected);
        float difference = 0.0f;
        float inputDifference = 0.0f;
        for (int component = 0; component < 4; ++component) {
            difference = std::max(
                difference,
                std::fabs(captured[index][component] - expected[component]));
            const float input = component < 3 ? vertex.position[component] : 1.0f;
            inputDifference = std::max(
                inputDifference,
                std::fabs(captured[index][component] - input));
        }
        worst = std::max(worst, difference);
        if (difference > 2.0e-3f) {
            char text[256]{};
            std::snprintf(text, sizeof(text),
                          "%s: vertex %u captured (%g, %g, %g, %g) but "
                          "expected (%g, %g, %g, %g)",
                          label, expectedFirstVertex + index, captured[index][0],
                          captured[index][1], captured[index][2],
                          captured[index][3], expected[0], expected[1],
                          expected[2], expected[3]);
            Check(false, text);
        }
        if (inputDifference < 0.25f) {
            char text[256]{};
            std::snprintf(text, sizeof(text),
                          "%s: vertex %u captured value equals the input "
                          "vertex data (max diff %g)",
                          label, expectedFirstVertex + index, inputDifference);
            Check(false, text);
        }
    }
    char summary[256]{};
    std::snprintf(summary, sizeof(summary),
                  "%s: %u vertices, worst |captured - expected| = %g", label,
                  vertexCount, worst);
    Note(summary);
}

void CheckCaptureMatches(const char* label, Fixture& fixture,
                         unsigned firstVertex, unsigned vertexCount) {
    CheckCaptureMatches(label, fixture, firstVertex, vertexCount, firstVertex);
}

// ---- tests ----------------------------------------------------------------
void TestBasicCaptures(Fixture& fixture) {
    fixture.device->SetVertexShader(fixture.shader2);
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, 0,
                                    sizeof(Vertex));
    CheckCaptureMatches("vs_2_0 all 8 vertices", fixture, 0, 8);

    fixture.device->SetVertexShader(fixture.shader3);
    CheckCaptureMatches("vs_3_0 all 8 vertices", fixture, 0, 8);
    CheckCaptureMatches("vs_3_0 3 vertices", fixture, 2, 3);

    // Non-zero firstVertex, for both shader models.
    fixture.device->SetVertexShader(fixture.shader2);
    CheckCaptureMatches("vs_2_0 firstVertex 5 count 3", fixture, 5, 3);
    fixture.device->SetVertexShader(fixture.shader3);
    CheckCaptureMatches("vs_3_0 firstVertex 5 count 3", fixture, 5, 3);

    // A non-zero stream offset plus a non-zero firstVertex: the capture has to
    // shift the real streams by firstVertex on top of the app's offset, so the
    // first captured vertex is the one at (offset/stride + firstVertex).
    fixture.device->SetVertexShader(fixture.shader3);
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, sizeof(Vertex),
                                    sizeof(Vertex));
    CheckCaptureMatches("vs_3_0 stream offset 1 + firstVertex 2", fixture, 2, 4,
                        3);
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, 0,
                                    sizeof(Vertex));

    // A single vertex, to cover the 1x1 render target shape.
    CheckCaptureMatches("vs_3_0 single vertex", fixture, 7, 1);

    // Repeated captures must not drift.
    CheckCaptureMatches("vs_3_0 all 8 vertices (repeat)", fixture, 0, 8);
}

void TestTemporalVertexCache(Fixture& fixture) {
    using mikumikustudio::antialias::motion::detail::VertexCache;
    VertexCache cache;
    fixture.device->SetVertexShader(fixture.shader3);
    fixture.device->SetVertexDeclaration(fixture.declaration);
    fixture.device->SetStreamSource(0,fixture.vertexBuffer,0,sizeof(Vertex));
    std::vector<std::array<float,4>> first,second,changed;
    std::string error;
    const auto capture=[&](unsigned start,unsigned count,std::vector<std::array<float,4>>& out) {
        Check(cache.Capture(fixture.device,fixture.code3,0,0,8,start,count,out,error),
              "temporal cache capture: "+error);
    };
    capture(0,3,first);capture(3,3,second);
    Check(cache.captures==1&&cache.hits==1,"two materials reuse one full-range GPU capture");
    for(unsigned i=0;i<second.size();++i) {
        float expected[4]{};ExpectedClip(kVertices[i+3].position,expected);
        for(unsigned c=0;c<4;++c)Check(std::fabs(second[i][c]-expected[c])<2e-3f,
            "cached subset uses the actual source vertex indices");
    }
    float saved[4]{};fixture.device->GetVertexShaderConstantF(0,saved,1);
    float altered[4];std::memcpy(altered,saved,sizeof(altered));altered[0]+=2;
    fixture.device->SetVertexShaderConstantF(0,altered,1);capture(0,3,changed);
    Check(cache.captures==2&&cache.hits==1&&first!=changed,"changed float constants recompute final shader output");
    fixture.device->SetVertexShaderConstantF(0,saved,1);
    int oldInteger[4]{};fixture.device->GetVertexShaderConstantI(0,oldInteger,1);
    const int integer[4]={17,18,19,20};fixture.device->SetVertexShaderConstantI(0,integer,1);
    capture(0,3,changed);Check(cache.captures==3,"integer constants participate in the cache key");
    fixture.device->SetVertexShaderConstantI(0,oldInteger,1);
    BOOL oldBoolean=FALSE;fixture.device->GetVertexShaderConstantB(0,&oldBoolean,1);
    const BOOL boolean=!oldBoolean;fixture.device->SetVertexShaderConstantB(0,&boolean,1);
    capture(0,3,changed);Check(cache.captures==4,"boolean constants participate in the cache key");
    fixture.device->SetVertexShaderConstantB(0,&oldBoolean,1);
    void* bytes=nullptr;Check(SUCCEEDED(fixture.vertexBuffer->Lock(0,sizeof(kVertices),&bytes,0)),"rewrite cached source buffer");
    static_cast<Vertex*>(bytes)[0].position[0]+=3;fixture.vertexBuffer->Unlock();
    capture(0,3,changed);
    Check(cache.captures==5&&first!=changed,"same buffer with changed vertex contents is not reused");
    fixture.vertexBuffer->Lock(0,sizeof(kVertices),&bytes,0);std::memcpy(bytes,kVertices,sizeof(kVertices));fixture.vertexBuffer->Unlock();
    capture(0,3,changed);Check(cache.captures==5&&cache.hits==2&&changed==first,"exact restored input can reuse the earlier result");
    fixture.device->SetStreamSource(0,fixture.vertexBuffer,sizeof(Vertex),sizeof(Vertex));
    Check(cache.Capture(fixture.device,fixture.code3,0,0,7,0,3,changed,error),"capture changed stream offset");
    Check(cache.captures==6&&changed!=first,"stream offset and declared vertex range cannot alias cached input");
    fixture.device->SetStreamSource(0,fixture.vertexBuffer,0,sizeof(Vertex));
    fixture.device->SetVertexShader(fixture.shader2);
    Check(cache.Capture(fixture.device,fixture.code2,0,0,8,0,3,changed,error),"capture different vertex shader");
    Check(cache.captures==7,"vertex shader bytecode participates in the cache key");
    fixture.device->SetVertexShader(fixture.shader3);
    IDirect3DTexture9* texture=nullptr;
    Check(SUCCEEDED(fixture.device->CreateTexture(1,1,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&texture,nullptr)),
          "create vertex-texture cache exclusion fixture");
    Check(SUCCEEDED(fixture.device->SetTexture(D3DVERTEXTEXTURESAMPLER0,texture)),"bind vertex texture");
    const auto oldHits=cache.hits;capture(0,3,changed);capture(0,3,changed);
    Check(cache.captures==9&&cache.hits==oldHits,"vertex textures force fresh shader capture on every draw");
    fixture.device->SetTexture(D3DVERTEXTEXTURESAMPLER0,nullptr);texture->Release();
    cache.Clear();capture(0,3,changed);
    Check(cache.captures==1&&cache.hits==0,"a new frame never reuses the previous frame's deformed positions");
    Note("temporal vertex cache: exact state/content reuse and all invalidation checks passed");
}

void TestStateRestored(Fixture& fixture) {
    // Distinctive state that the capture must put back exactly.
    fixture.device->SetVertexShader(fixture.shader2);
    fixture.device->SetPixelShader(fixture.appPixelShader);
    fixture.device->SetVertexDeclaration(fixture.declaration);
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, sizeof(Vertex),
                                    sizeof(Vertex));
    fixture.device->SetRenderTarget(0, fixture.backBuffer);
    fixture.device->SetDepthStencilSurface(fixture.depthStencil);
    D3DVIEWPORT9 viewport{};
    viewport.X = 3;
    viewport.Y = 5;
    viewport.Width = 40;
    viewport.Height = 20;
    viewport.MinZ = 0.1f;
    viewport.MaxZ = 0.9f;
    Check(SUCCEEDED(fixture.device->SetViewport(&viewport)), "SetViewport");
    fixture.device->SetRenderState(D3DRS_ZENABLE, TRUE);
    fixture.device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    fixture.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    fixture.device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    fixture.device->SetRenderState(D3DRS_CLIPPING, TRUE);
    fixture.device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    fixture.device->SetRenderState(D3DRS_POINTSIZE, FloatBits(3.0f));
    fixture.device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    const float marker[4] = {11.0f, 22.0f, 33.0f, 44.0f};
    fixture.device->SetVertexShaderConstantF(8, marker, 1);
    // D3D9 reports an FVF equivalent for declarations that one can express as
    // an FVF, so only "unchanged" is meaningful here.
    DWORD fvfBefore = 0;
    fixture.device->GetFVF(&fvfBefore);

    std::vector<std::array<float, 4>> captured;
    std::string error;
    Check(mikumikustudio::vertex_capture::Capture(fixture.device, 1, 6,
                                                   captured, error),
          "state restore: capture failed: " + error);
    Check(captured.size() == 6, "state restore: vertex count");
    mikumikustudio::vertex_capture::GpuCapture gpuCapture;
    std::shared_ptr<const mikumikustudio::vertex_capture::GpuPositions> gpuPositions;
    Check(gpuCapture.Capture(fixture.device,1,6,gpuPositions,error),"GPU capture restores every probed device state: "+error);

    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DPixelShader9* pixelShader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DVertexBuffer9* buffer = nullptr;
    IDirect3DSurface9* renderTarget = nullptr;
    IDirect3DSurface9* depthStencil = nullptr;
    UINT offset = 0;
    UINT stride = 0;
    D3DVIEWPORT9 restored{};
    DWORD value = 0;
    float constants[4]{};

    Check(SUCCEEDED(fixture.device->GetVertexShader(&vertexShader)) &&
              vertexShader == fixture.shader2,
          "state restore: vertex shader");
    Check(SUCCEEDED(fixture.device->GetPixelShader(&pixelShader)) &&
              pixelShader == fixture.appPixelShader,
          "state restore: pixel shader");
    Check(SUCCEEDED(fixture.device->GetVertexDeclaration(&declaration)) &&
              declaration == fixture.declaration,
          "state restore: vertex declaration");
    Check(SUCCEEDED(fixture.device->GetStreamSource(0, &buffer, &offset,
                                                    &stride)) &&
              buffer == fixture.vertexBuffer && offset == sizeof(Vertex) &&
              stride == sizeof(Vertex),
          "state restore: stream 0 offset and stride");
    Check(SUCCEEDED(fixture.device->GetRenderTarget(0, &renderTarget)) &&
              renderTarget == fixture.backBuffer,
          "state restore: render target");
    Check(SUCCEEDED(fixture.device->GetDepthStencilSurface(&depthStencil)) &&
              depthStencil == fixture.depthStencil,
          "state restore: depth stencil");
    Check(SUCCEEDED(fixture.device->GetViewport(&restored)),
          "state restore: GetViewport");
    if (restored.X != viewport.X || restored.Y != viewport.Y ||
        restored.Width != viewport.Width || restored.Height != viewport.Height ||
        restored.MinZ != viewport.MinZ || restored.MaxZ != viewport.MaxZ) {
        char text[256]{};
        std::snprintf(text, sizeof(text),
                      "state restore: viewport is (%lu, %lu, %lu, %lu, %g, %g) "
                      "but was set to (%lu, %lu, %lu, %lu, %g, %g)",
                      static_cast<unsigned long>(restored.X),
                      static_cast<unsigned long>(restored.Y),
                      static_cast<unsigned long>(restored.Width),
                      static_cast<unsigned long>(restored.Height), restored.MinZ,
                      restored.MaxZ, static_cast<unsigned long>(viewport.X),
                      static_cast<unsigned long>(viewport.Y),
                      static_cast<unsigned long>(viewport.Width),
                      static_cast<unsigned long>(viewport.Height), viewport.MinZ,
                      viewport.MaxZ);
        Check(false, text);
    }
    fixture.device->GetRenderState(D3DRS_ZENABLE, &value);
    Check(value == TRUE, "state restore: D3DRS_ZENABLE");
    fixture.device->GetRenderState(D3DRS_ZWRITEENABLE, &value);
    Check(value == TRUE, "state restore: D3DRS_ZWRITEENABLE");
    fixture.device->GetRenderState(D3DRS_CULLMODE, &value);
    Check(value == D3DCULL_CW, "state restore: D3DRS_CULLMODE");
    fixture.device->GetRenderState(D3DRS_ALPHABLENDENABLE, &value);
    Check(value == TRUE, "state restore: D3DRS_ALPHABLENDENABLE");
    fixture.device->GetRenderState(D3DRS_CLIPPING, &value);
    Check(value == TRUE, "state restore: D3DRS_CLIPPING");
    fixture.device->GetRenderState(D3DRS_SCISSORTESTENABLE, &value);
    Check(value == TRUE, "state restore: D3DRS_SCISSORTESTENABLE");
    fixture.device->GetRenderState(D3DRS_COLORWRITEENABLE, &value);
    Check(value == 0x7, "state restore: D3DRS_COLORWRITEENABLE");
    fixture.device->GetRenderState(D3DRS_POINTSIZE, &value);
    float pointSize = 0.0f;
    std::memcpy(&pointSize, &value, sizeof(pointSize));
    Check(pointSize == 3.0f, "state restore: D3DRS_POINTSIZE");
    Check(SUCCEEDED(fixture.device->GetVertexShaderConstantF(8, constants, 1)) &&
              constants[0] == 11.0f && constants[1] == 22.0f &&
              constants[2] == 33.0f && constants[3] == 44.0f,
          "state restore: vertex shader constants");

    if (vertexShader != nullptr) vertexShader->Release();
    if (pixelShader != nullptr) pixelShader->Release();
    if (declaration != nullptr) declaration->Release();
    if (buffer != nullptr) buffer->Release();
    if (renderTarget != nullptr) renderTarget->Release();
    if (depthStencil != nullptr) depthStencil->Release();
    DWORD fvfAfter = 0;
    fixture.device->GetFVF(&fvfAfter);
    Check(fvfAfter == fvfBefore,
          "state restore: the reported FVF state is unchanged");

    // The device must still accept a normal draw after the capture.
    fixture.device->SetStreamSource(0, fixture.vertexBuffer, 0, sizeof(Vertex));
    Check(SUCCEEDED(fixture.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2)),
          "a real triangle draw still succeeds after the capture");
    Check(SUCCEEDED(fixture.device->SetViewport(&viewport)),
          "restore the test viewport");
}

void TestHandAssembledShaders(Fixture& fixture) {
    // A float4 POSITION buffer so hand-written shaders can forward v0 unchanged.
    float positions[4][4] = {
        {0.25f, -0.50f, 0.75f, 1.0f},
        {-1.25f, 0.25f, 0.50f, 1.0f},
        {0.75f, 1.00f, -0.25f, 1.0f},
        {-0.50f, -0.75f, -1.00f, 1.0f},
    };
    IDirect3DVertexBuffer9* buffer = nullptr;
    Check(SUCCEEDED(fixture.device->CreateVertexBuffer(
              sizeof(positions), 0, 0, D3DPOOL_MANAGED, &buffer, nullptr)),
          "CreateVertexBuffer(float4 positions)");
    void* data = nullptr;
    Check(SUCCEEDED(buffer->Lock(0, sizeof(positions), &data, 0)),
          "lock the float4 buffer");
    std::memcpy(data, positions, sizeof(positions));
    buffer->Unlock();

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
         0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9* declaration = nullptr;
    Check(SUCCEEDED(fixture.device->CreateVertexDeclaration(elements,
                                                            &declaration)),
          "CreateVertexDeclaration(float4 position)");

    fixture.device->SetStreamSource(0, buffer, 0, 4 * sizeof(float));
    fixture.device->SetVertexDeclaration(declaration);

    std::vector<std::array<float, 4>> captured;
    std::string error;

    // 1. `mov oPos, v0` forwards the input unchanged: the capture must return
    //    exactly the input float4.
    {
        std::vector<DWORD> code = Tokens({0xFFFE0200});
        code.insert(code.end(), std::begin(kDclPositionV0),
                    std::end(kDclPositionV0));
        code.insert(code.end(), {0x02000001, 0xC00F0000, 0x90E40000});
        code.push_back(0x0000FFFF);
        IDirect3DVertexShader9* shader =
            TryCreateShader(fixture.device, code, "mov oPos, v0");
        if (shader != nullptr) {
            fixture.device->SetVertexShader(shader);
            captured.clear();
            error.clear();
            Check(mikumikustudio::vertex_capture::Capture(
                      fixture.device, 0, 4, captured, error),
                  "forwarded position: capture failed: " + error);
            for (unsigned index = 0; index < 4; ++index) {
                for (int component = 0; component < 4; ++component) {
                    Check(std::fabs(captured[index][component] -
                                    positions[index][component]) < 1.0e-4f,
                          "forwarded position: value mismatch");
                }
            }
            shader->Release();
        }
    }

    // 2. A compiled vs_3_0 shader that declares a PSIZE output of 64: a
    //    shader-written point size must not stretch the capture points over
    //    neighbouring pixels, so the values still have to come back exactly.
    //    (Without the neutralisation each point would cover 64x64 pixels.)
    {
        const std::vector<DWORD> code = CompileVertexShader(
            "struct Out { float4 position : POSITION; float size : PSIZE; };\n"
            "Out main(float4 pos : POSITION) {\n"
            "  Out output;\n"
            "  output.position = pos;\n"
            "  output.size = 64.0;\n"
            "  return output;\n"
            "}\n",
            "vs_3_0");
        IDirect3DVertexShader9* shader = nullptr;
        const HRESULT created =
            fixture.device->CreateVertexShader(code.data(), &shader);
        Check(SUCCEEDED(created),
              "CreateVertexShader(psize) failed with " + HResultText(created));
        fixture.device->SetVertexShader(shader);
        captured.clear();
        error.clear();
        Check(mikumikustudio::vertex_capture::Capture(fixture.device, 0, 4,
                                                       captured, error),
              "point size neutralised: capture failed: " + error);
        float worst = 0.0f;
        for (unsigned index = 0; index < 4; ++index) {
            for (int component = 0; component < 4; ++component) {
                worst = std::max(worst,
                                 std::fabs(captured[index][component] -
                                           positions[index][component]));
            }
        }
        Check(worst < 1.0e-4f,
              "point size neutralised: values changed (evidence that the "
              "shader-declared point size leaked into the capture)");
        Note("point size neutralised: a dcl_psize output of 64 still gave one "
             "pixel per vertex");
        shader->Release();
    }

    // 3. A shader that writes no POSITION output must be refused.
    {
        std::vector<DWORD> code = Tokens({0xFFFE0200});
        code.insert(code.end(), std::begin(kDclPositionV0),
                    std::end(kDclPositionV0));
        code.insert(code.end(), {0x02000001, 0x800F0000, 0x90E40000});
        code.push_back(0x0000FFFF);
        IDirect3DVertexShader9* shader =
            TryCreateShader(fixture.device, code, "no position output");
        if (shader != nullptr) {
            fixture.device->SetVertexShader(shader);
            captured.clear();
            error.clear();
            Check(!mikumikustudio::vertex_capture::Capture(
                      fixture.device, 0, 2, captured, error),
                  "no position output: capture must fail");
            Check(!error.empty(), "no position output: error message");
            Check(captured.empty(),
                  "no position output: output vector must be cleared");
            Note("no position output: " + error);
            shader->Release();
        }
    }

    // 4. A shader that writes every oT register leaves nowhere to put the
    //    captured POSITION and must be refused.
    {
        std::vector<DWORD> code = Tokens({0xFFFE0200});
        code.insert(code.end(), std::begin(kDclPositionV0),
                    std::end(kDclPositionV0));
        for (DWORD reg = 0; reg < 8; ++reg) {
            code.insert(code.end(),
                        {0x02000001, 0xE00F0000 | reg, 0x90E40000});
        }
        code.insert(code.end(), {0x02000001, 0xC00F0000, 0x90E40000});
        code.push_back(0x0000FFFF);
        IDirect3DVertexShader9* shader =
            TryCreateShader(fixture.device, code, "all oT written");
        if (shader != nullptr) {
            fixture.device->SetVertexShader(shader);
            captured.clear();
            error.clear();
            Check(!mikumikustudio::vertex_capture::Capture(
                      fixture.device, 0, 2, captured, error),
                  "all oT written: capture must fail");
            Check(error.find("texture coordinate") != std::string::npos,
                  "all oT written: error names the exhausted oT registers");
            Note("all oT written: " + error);
            shader->Release();
        }
    }

    declaration->Release();
    buffer->Release();
}

void TestChunkedCapture(Fixture& fixture) {
    // More vertices than one capture chunk (262144 pixels) to exercise the
    // multi-chunk draw + readback path with an identity shader.
    const unsigned vertexCount = 300000;
    std::vector<float> positions(static_cast<std::size_t>(vertexCount) * 4);
    for (unsigned index = 0; index < vertexCount; ++index) {
        positions[index * 4 + 0] = static_cast<float>(index % 97) * 0.125f - 6.0f;
        positions[index * 4 + 1] = static_cast<float>(index % 31) * 0.25f - 4.0f;
        positions[index * 4 + 2] = static_cast<float>(index % 13) * 0.5f - 3.0f;
        positions[index * 4 + 3] = 1.0f;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    Check(SUCCEEDED(fixture.device->CreateVertexBuffer(
              static_cast<UINT>(positions.size() * sizeof(float)), 0, 0,
              D3DPOOL_MANAGED, &buffer, nullptr)),
          "CreateVertexBuffer(chunking)");
    void* data = nullptr;
    Check(SUCCEEDED(buffer->Lock(0, static_cast<UINT>(positions.size() *
                                                     sizeof(float)),
                                 &data, 0)),
          "lock the chunking buffer");
    std::memcpy(data, positions.data(), positions.size() * sizeof(float));
    buffer->Unlock();

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
         0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9* declaration = nullptr;
    Check(SUCCEEDED(fixture.device->CreateVertexDeclaration(elements,
                                                            &declaration)),
          "CreateVertexDeclaration(chunking)");

    std::vector<DWORD> code = Tokens({0xFFFE0200});
    code.insert(code.end(), std::begin(kDclPositionV0), std::end(kDclPositionV0));
    code.insert(code.end(), {0x02000001, 0xC00F0000, 0x90E40000});
    code.push_back(0x0000FFFF);
    IDirect3DVertexShader9* shader =
        TryCreateShader(fixture.device, code, "chunking");
    if (shader == nullptr) {
        declaration->Release();
        buffer->Release();
        return;
    }

    fixture.device->SetStreamSource(0, buffer, 0, 4 * sizeof(float));
    fixture.device->SetVertexDeclaration(declaration);
    fixture.device->SetVertexShader(shader);

    std::vector<std::array<float, 4>> captured;
    std::string error;
    Check(mikumikustudio::vertex_capture::Capture(fixture.device, 0,
                                                   vertexCount, captured,
                                                   error),
          "chunked capture failed: " + error);
    Check(captured.size() == vertexCount, "chunked capture: vertex count");
    float worst = 0.0f;
    unsigned worstIndex = 0;
    for (unsigned index = 0; index < vertexCount; ++index) {
        for (int component = 0; component < 4; ++component) {
            const float difference =
                std::fabs(captured[index][component] -
                          positions[index * 4 + component]);
            if (difference > worst) {
                worst = difference;
                worstIndex = index;
            }
        }
    }
    char text[256]{};
    std::snprintf(text, sizeof(text),
                  "chunked capture: %u vertices in 2 chunks, worst error %g "
                  "(at vertex %u)",
                  vertexCount, worst, worstIndex);
    Note(text);
    Check(worst < 1.0e-4f, "chunked capture: value mismatch");

    mikumikustudio::vertex_capture::GpuCapture gpuCapture;
    std::shared_ptr<const mikumikustudio::vertex_capture::GpuPositions> gpu;
    Check(gpuCapture.Capture(fixture.device,0,vertexCount,gpu,error),"large GPU capture: "+error);
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface,system;
    Check(SUCCEEDED(gpu->texture->GetSurfaceLevel(0,&surface))&&
        SUCCEEDED(fixture.device->CreateOffscreenPlainSurface(gpu->width,gpu->height,D3DFMT_A32B32G32R32F,
            D3DPOOL_SYSTEMMEM,&system,nullptr))&&
        SUCCEEDED(fixture.device->GetRenderTargetData(surface.Get(),system.Get())),"explicitly inspect multi-row GPU capture in test");
    D3DLOCKED_RECT locked{};Check(SUCCEEDED(system->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"lock large GPU test capture");
    bool equal=true;
    for(unsigned i=0;i<vertexCount;++i)equal&=std::memcmp(positions.data()+std::size_t(i)*4,
        static_cast<const char*>(locked.pBits)+(i/gpu->width)*locked.Pitch+(i%gpu->width)*16,16)==0;
    system->UnlockRect();
    Check(equal,"all 300000 GPU positions match across texture rows and the old chunk boundary");

    shader->Release();
    declaration->Release();
    buffer->Release();
}

void TestGpuCapture(Fixture& fixture) {
    using namespace mikumikustudio::vertex_capture;
    using Microsoft::WRL::ComPtr;
    fixture.device->SetVertexDeclaration(fixture.declaration);
    fixture.device->SetStreamSource(0,fixture.vertexBuffer,0,sizeof(Vertex));
    GpuCapture capture;std::string error;
    const auto read=[&](const std::shared_ptr<const GpuPositions>& gpu) {
        ComPtr<IDirect3DSurface9> surface,system;
        Check(SUCCEEDED(gpu->texture->GetSurfaceLevel(0,&surface)),"GPU capture texture has a surface");
        Check(SUCCEEDED(fixture.device->CreateOffscreenPlainSurface(gpu->width,gpu->height,
            D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&system,nullptr)),"allocate explicit test readback");
        Check(SUCCEEDED(fixture.device->GetRenderTargetData(surface.Get(),system.Get())),"read GPU positions only in regression test");
        D3DLOCKED_RECT locked{};Check(SUCCEEDED(system->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"lock test positions");
        std::vector<std::array<float,4>> result(gpu->vertexCount);
        for(unsigned i=0;i<gpu->vertexCount;++i)std::memcpy(result[i].data(),
            static_cast<const char*>(locked.pBits)+(i/gpu->width)*locked.Pitch+(i%gpu->width)*16,16);
        system->UnlockRect();return result;
    };
    for(auto* shader:{fixture.shader2,fixture.shader3}) {
        fixture.device->SetVertexShader(shader);
        std::vector<std::array<float,4>> cpu;
        Check(Capture(fixture.device,1,6,cpu,error),"capture reference positions: "+error);
        std::shared_ptr<const GpuPositions> first,second,third;
        Check(capture.Capture(fixture.device,1,6,first,error),"capture GPU positions: "+error);
        Check(first&&first->firstVertex==1&&read(first)==cpu,"GPU shader 2/3 positions equal export capture byte for byte");
        float saved[4]{};fixture.device->GetVertexShaderConstantF(0,saved,1);
        float changed[4];std::memcpy(changed,saved,sizeof(saved));changed[0]+=2;
        fixture.device->SetVertexShaderConstantF(0,changed,1);
        Check(capture.Capture(fixture.device,1,6,second,error),"capture changed GPU positions: "+error);
        Check(first->texture!=second->texture&&read(first)==cpu&&read(second)!=cpu,
            "a later capture cannot overwrite referenced previous-frame positions");
        auto* released=first->texture.Get();first.reset();
        fixture.device->SetVertexShaderConstantF(0,saved,1);
        Check(capture.Capture(fixture.device,1,6,third,error),"reuse unreferenced GPU capture target: "+error);
        Check(third->texture.Get()==released&&read(third)==cpu,"GPU capture pool safely reuses released storage");
        ComPtr<IDirect3DVertexShader9> restored;fixture.device->GetVertexShader(&restored);
        Check(restored.Get()==shader,"GPU capture restores the original vertex shader");
    }
    mikumikustudio::antialias::motion::detail::VertexCache cache;
    fixture.device->SetVertexShader(fixture.shader3);
    std::shared_ptr<const GpuPositions> first,second;
    Check(cache.CaptureGpu(fixture.device,fixture.code3,0,0,8,0,3,first,error),"cache GPU full declared range");
    Check(cache.CaptureGpu(fixture.device,fixture.code3,0,0,8,3,3,second,error),"reuse GPU range across materials");
    Check(first==second&&first->vertexCount==8&&cache.gpuCaptures==1&&cache.hits==1&&cache.cpuReadbacks==0,
        "two material ranges share one GPU capture without CPU readback");
    cache.Clear();
    Check(cache.CaptureGpu(fixture.device,fixture.code3,0,0,8,0,3,second,error),"capture next GPU frame");
    Check(first!=second&&cache.gpuCaptures==1,"new frame preserves referenced history and captures fresh geometry");
    Check(!capture.Capture(nullptr,0,1,second,error)&&!second&&!error.empty(),"GPU capture failure clears stale output");
    Check(capture.Capture(fixture.device,0,0,second,error)&&!second,"empty GPU capture succeeds without a resource");
}

void TestFailures(Fixture& fixture) {
    std::vector<std::array<float, 4>> captured;
    std::string error;

    captured.push_back({1.0f, 1.0f, 1.0f, 1.0f});
    error.clear();
    Check(!mikumikustudio::vertex_capture::Capture(nullptr, 0, 4, captured,
                                                   error),
          "null device must fail");
    Check(!error.empty(), "null device must report a reason");
    Check(captured.empty(), "null device must clear the output");

    error.clear();
    Check(!mikumikustudio::vertex_capture::Capture(fixture.device, 0xFFFFFFFFu,
                                                   2, captured, error),
          "overflowing vertex range must fail");
    Check(!error.empty(), "overflowing vertex range must report a reason");

    // Zero vertices is a no-op, not a failure.
    captured.push_back({1.0f, 1.0f, 1.0f, 1.0f});
    error.clear();
    Check(mikumikustudio::vertex_capture::Capture(fixture.device, 4, 0,
                                                   captured, error),
          "zero vertices must succeed");
    Check(captured.empty(), "zero vertices must yield an empty result");

    // No vertex shader bound: the caller's fixed-function path handles it.
    fixture.device->SetVertexShader(nullptr);
    captured.clear();
    error.clear();
    Check(!mikumikustudio::vertex_capture::Capture(fixture.device, 0, 4,
                                                    captured, error),
          "no vertex shader must fail");
    Check(error.find("no vertex shader") != std::string::npos,
          "no vertex shader: error names the missing shader");
    Note("no vertex shader: " + error);

    // Vertex shader bound but no declaration: an FVF layout cannot be extended.
    fixture.device->SetVertexShader(fixture.shader2);
    fixture.device->SetVertexDeclaration(nullptr);
    fixture.device->SetFVF(D3DFVF_XYZ);
    IDirect3DVertexDeclaration9* probe = nullptr;
    const HRESULT probeResult = fixture.device->GetVertexDeclaration(&probe);
    if (probe == nullptr) {
        captured.clear();
        error.clear();
        Check(!mikumikustudio::vertex_capture::Capture(fixture.device, 0, 4,
                                                       captured, error),
              "no vertex declaration must fail");
        Check(!error.empty(), "no vertex declaration must report a reason");
        Note("no vertex declaration: " + error);
    } else {
        Note("runtime exposes an FVF-backed vertex declaration (" +
             HResultText(probeResult) +
             "); the no-declaration case is not reachable this way");
        probe->Release();
    }
    fixture.device->SetFVF(0);
    fixture.device->SetVertexDeclaration(fixture.declaration);
    fixture.device->SetVertexShader(fixture.shader2);
}

}  // namespace

int main() {
    LoadCompiler();

    Fixture fixture;
    CreateFixture(fixture);

    TestBasicCaptures(fixture);
    TestTemporalVertexCache(fixture);
    TestStateRestored(fixture);
    TestHandAssembledShaders(fixture);
    TestChunkedCapture(fixture);
    TestGpuCapture(fixture);
    TestFailures(fixture);

    Check(SUCCEEDED(fixture.device->EndScene()), "EndScene still succeeds");
    DestroyFixture(fixture);

    std::printf("PASS: vertex capture regression (%d checks)\n", gChecks);
    return 0;
}
