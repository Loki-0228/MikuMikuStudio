// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 1e-7f) {
        std::fprintf(stderr, "%s: actual=%.9g expected=%.9g\n", message, actual, expected);
        std::exit(1);
    }
}
}

int main() {
    HWND window = CreateWindowW(L"STATIC", L"Model edge regression", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden test window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "create device");
    IDirect3DVertexBuffer9* mainBuffer = nullptr;
    IDirect3DVertexBuffer9* edgeBuffer = nullptr;
    Check(SUCCEEDED(device->CreateVertexBuffer(sizeof(mdl::SkinnedVertexBase), 0,
        0, D3DPOOL_MANAGED, &mainBuffer, nullptr)), "allocate body vertex");
    Check(SUCCEEDED(device->CreateVertexBuffer(sizeof(mdl::EdgeVertex), 0,
        0, D3DPOOL_MANAGED, &edgeBuffer, nullptr)), "allocate outline vertex");

    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    auto model = std::make_unique<mdl::ModelRecord>();
    auto* bytes = reinterpret_cast<unsigned char*>(model.get());
    mdl::BoneRecord bones[2]{};
    mdl::PmdVertex pmd{};
    mdl::PmxVertex pmx{};
    mdl::ModelMaterialRecord material{};
    model->vertexCount = model->materialCount = model->boneCount = 1;
    model->loadComplete = 1; model->edgeScale = 1;
    model->boneTable = bones; model->materials = &material;
    model->rawVertices = &pmd; model->pmxVertices = &pmx;
    model->vertexBuffer = mainBuffer; model->vertexBuffer2 = edgeBuffer;
    float identity[16]{}; identity[0] = identity[5] = identity[10] = identity[15] = 1;
    for (auto& bone : bones) std::memcpy(bone.matInit, identity, sizeof identity);
    pmd.normal[0] = 1; pmd.weightPercent = 100;
    pmx.normal[0] = 1; pmx.weightType = mdl::PmxWeightType::bdef1;
    pmx.edgeScale = .5f; pmx.hasPmxEdgeData = 1;
    material.edgeSize = 3;
    app->CameraFov() = 30;

    // MMD 9.26 passes the view eye to its edge worker (0x14002A213..247),
    // separately from the orbit target already included in frameWorld.
    for (bool isPmx : {false, true}) {
        model->physicsMode = isPmx ? 2 : 0;
        model->boneCount = 1;
        bones[0].position[1] = 0;
        auto verify = [&](float* world, float expected, bool enabled = true) {
            GpuModelInput input;
            Check(PrepareGpuModelInput(app.get(), bytes, world, input), "prepare GPU input");
            Near(input.parameters.edgeDistance, expected, "GPU frame edge distance");
            Near(input.materials[0].edgeDistance, expected, "GPU batched material distance");
            UpdateModelVertexBuffers(app.get(), bytes, world);
            mdl::EdgeVertex* edge = nullptr;
            Check(SUCCEEDED(edgeBuffer->Lock(0, 0, reinterpret_cast<void**>(&edge), 0)), "read CPU edge");
            Near(edge->position[0], enabled ? expected * (isPmx ? 1.5f : 1.f) : -.005f,
                "CPU outline expansion");
            edgeBuffer->Unlock();
        };

        // Camera close to a head at y=16; translating the scene and target
        // together must preserve line thickness (the old code thickened it).
        app->CameraPositionY() = 0; app->CameraDistance() = -2;
        app->ViewOffsetX() = app->ViewOffsetY() = 0;
        verify(identity, .00171f);
        float shifted[16]; std::memcpy(shifted, identity, sizeof shifted);
        app->CameraPositionY() = bones[0].position[1] = 16;
        shifted[13] = -16;
        verify(shifted, .00171f);
        model->boneCount = 2; bones[1].position[1] = 16;
        bones[0].position[1] = 100; // More than one bone uses bone 1.
        verify(shifted, .00171f);

        // Rotation, pan and distance all belong to the same frame space.
        float rotated[16]{};
        rotated[0] = rotated[6] = rotated[15] = 1; rotated[9] = -1;
        rotated[14] = -16;
        app->ViewOffsetX() = 3; app->ViewOffsetY() = 4; app->CameraDistance() = 0;
        verify(rotated, .004275f); // A 3-4-5 distance, independent of orbit target.
        app->ViewOffsetX() = app->ViewOffsetY() = 0;
        app->CameraDistance() = -20;
        verify(shifted, .0171f);
        model->edgeScale = .25f;
        verify(shifted, .004275f);
        model->edgeScale = 0;
        verify(shifted, 0);
        model->edgeScale = 1;
        pmd.edgeDisabled = 1; pmx.hasPmxEdgeData = 0;
        verify(shifted, .0171f, false);
        pmd.edgeDisabled = 0; pmx.hasPmxEdgeData = 1;
    }
    mainBuffer->Release(); edgeBuffer->Release(); device->Release(); d3d->Release();
    DestroyWindow(window);
    std::puts("PASS PMD/PMX outline camera space, width controls and GPU inputs");
}
