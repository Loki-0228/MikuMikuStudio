// MAINTENANCE: integration-manual; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void CheckMatrices(const mdl::BoneRecord& bone) {
    for (const auto* matrix : {bone.matLocal, bone.matInit, bone.matExtra})
        for (unsigned i = 0; i < 16; ++i)
            Check(std::isfinite(matrix[i]), "invalid inheritance source must not produce nonfinite matrices");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t scene[256]{};
    const auto length = GetFullPathNameW(argv[1], 256, scene, nullptr);
    Check(length > 0 && length < 256, "resolve input scene path");
    const int cycles = argc > 2 ? (std::max)(1, _wtoi(argv[2])) : 3;
    auto* app = new MMDApp();
    g_Block = app; app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE) != 0, "initialize hidden Windows renderer");
    ShowWindow(app->MainWindow(), SW_HIDE);
    int expectedModels = -1, expectedAccessories = -1, expectedInvalid = -1;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        ResetAppState(app);
        wcscpy_s(app->EnvFileName(), 256, scene);
        std::printf("cycle=%d load PMM\n", cycle);
        const auto start = std::chrono::steady_clock::now();
        LoadSceneFile();
        const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        int models = 0, accessories = 0, invalid = 0;
        for (int slot = 0; slot < kModelSlotCount; ++slot) if (auto* raw = app->ModelSlot(slot)) {
            ++models;
            const auto& model = *mdl::Mdl(raw);
            for (unsigned i = 0; i < model.boneCount; ++i) {
                const auto& bone = model.boneTable[i];
                if ((bone.flags & mdl::kBoneFlagInheritMask) &&
                    (bone.tailIdx < 0 || static_cast<unsigned>(bone.tailIdx) >= model.boneCount)) {
                    ++invalid;
                    std::printf("model=%d bone=%u count=%u flags=0x%x source=%d hasFlag=%u layer=%d\n",
                        slot, i, model.boneCount, bone.flags, bone.tailIdx, bone.hasFlag, bone.layer);
                    CheckMatrices(bone);
                }
            }
        }
        for (int slot = 0; slot < 255; ++slot) if (app->AccessorySlot(slot)) ++accessories;
        std::printf("load_ms=%.3f models=%d accessories=%d invalid_inheritance=%d\n", loadMs, models, accessories, invalid);
        Check(models > 0 && invalid > 0, "fixture actually loads a model with an invalid inheritance source");
        if (cycle == 0) { expectedModels = models; expectedAccessories = accessories; expectedInvalid = invalid; }
        Check(models == expectedModels && accessories == expectedAccessories && invalid == expectedInvalid,
            "repeat loads preserve the complete scene object counts");
        for (unsigned frame = 0; frame < 12; ++frame) {
            app->DeltaTime() = 1.f / 30.f; app->state.messageSeen = 10;
            FrameDriver(app);
            Check(HeapValidate(GetProcessHeap(), 0, nullptr) != FALSE, "heap remains valid after frame evaluation");
        }
        Check(!IsWindowVisible(app->MainWindow()), "integration host remains hidden");
        std::printf("cycle=%d PASS 12 frames\n", cycle);
    }
    ResetAppState(app); ShutdownCleanup(app); delete app; g_Block = nullptr;
    std::puts("PASS Stage inheritance load, repeat evaluation and shutdown");
    return 0;
}
