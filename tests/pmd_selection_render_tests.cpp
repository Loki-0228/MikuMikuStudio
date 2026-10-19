// MAINTENANCE: regression-keep - real PMD texture rendering across Select/camera transitions.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/mmv4mms.hpp"

using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); ExitProcess(7); }
}
LRESULT CALLBACK Guard(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        auto window = reinterpret_cast<HWND>(wp);
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (wcscmp(name, L"#32770") == 0) {
            EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
                wchar_t text[512]{}; GetWindowTextW(child, text, 512);
                std::printf("DIALOG %ls\n", text); return TRUE;
            }, 0);
            Require(false, "unexpected dialog in isolated test");
        }
        return 1;
    }
    if (code == HCBT_CREATEWND) {
        auto* created = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(created->style & WS_CHILD)) {
            created->style &= ~WS_VISIBLE;
            created->dwExStyle |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            created->x = created->y = -32000;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void Capture(MMDApp* app, const fs::path& output, int step, bool fixture) {
    for (int i = 0; i != 3; ++i) { app->state.messageSeen = 1; RenderFrameScene(app); }
    auto* device = app->Renderer()->device;
    ComPtr<IDirect3DSurface9> target, resolved, readback;
    Require(SUCCEEDED(device->GetRenderTarget(0, &target)), "get render target");
    D3DSURFACE_DESC desc{}; Require(SUCCEEDED(target->GetDesc(&desc)), "describe target");
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        Require(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)), "resolve target");
        Require(SUCCEEDED(device->StretchRect(target.Get(), nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)), "resolve pixels");
        target = resolved;
    }
    Require(SUCCEEDED(d3dx::Get().saveSurfaceToFileW((output / (std::to_wstring(step) + L".png")).c_str(),
        3, target.Get(), nullptr, nullptr)), "save rendered pixels");
    Require(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
        D3DPOOL_SYSTEMMEM, &readback, nullptr)), "allocate readback");
    Require(SUCCEEDED(device->GetRenderTargetData(target.Get(), readback.Get())), "read pixels");
    D3DLOCKED_RECT lock{}; Require(SUCCEEDED(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock pixels");
    unsigned nonwhite = 0, colored = 0;
    const auto view = app->ViewportRect();
    for (int y = view.top; y < view.bottom && y < int(desc.Height); ++y) {
        if (y < 0) continue;
        auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        for (int x = view.left; x < view.right && x < int(desc.Width); ++x) {
            if (x < 0) continue;
            const int r = (row[x] >> 16) & 255, g = (row[x] >> 8) & 255, b = row[x] & 255;
            nonwhite += r < 240 || g < 240 || b < 240;
            colored += std::abs(r-g) > 20 || std::abs(r-b) > 20 || std::abs(g-b) > 20;
        }
    }
    if (fixture) {
        std::printf("SAMPLES step=%d", step);
        bool textured = true;
        for (const int side : {-1, 1}) for (const int rowIndex : {-1, 1}) {
            const int x = (view.left + view.right) / 2 + side * (view.right - view.left) * 7 / 100;
            const int y = (view.top + view.bottom) / 2 + rowIndex * (view.bottom - view.top) / 10;
            Require(x >= 0 && y >= 0 && x < int(desc.Width) && y < int(desc.Height), "fixture sample in target");
            const auto* row = reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits) + y * lock.Pitch);
            const DWORD pixel = row[x] & 0xffffff;
            const int r = pixel >> 16, g = (pixel >> 8) & 255, b = pixel & 255;
            textured &= side < 0 ? r > g + 30 && r > b + 60 : b > r + 60 && g > r + 50;
            std::printf(" %06lx", pixel);
        }
        std::printf("\n");
        readback->UnlockRect();
        Require(textured, "both authored texture colours survive Select/camera transition");
    } else readback->UnlockRect();
    std::printf("CAPTURE step=%d panel=%d mode=%d effect=%d mmv=%d size=%ux%u nonwhite=%u colored=%u\n",
        step, int(app->CameraMode()), int(app->EditMode()), UseEffectModelRenderer(app), mmv4mms::Active(), desc.Width, desc.Height, nonwhite, colored);
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 3) return 2;
    bool hidden = false, fixture = false;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hidden-default") == 0) hidden = true;
        else if (wcscmp(argv[i], L"--fixture") == 0) fixture = true;
        else return 2;
    }
    const auto modelPath = fs::absolute(argv[1]), output = fs::absolute(argv[2]);
    Require(fs::is_directory(output), "existing owned output directory");
    wchar_t desktop[256]{}; DWORD needed = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
        desktop, sizeof(desktop), &needed) != FALSE, "read test desktop");
    const bool isPrivate = _wcsnicmp(desktop, L"MMS_REPAIR_", 11) == 0;
    std::printf("%s name=%ls\n", isPrivate ? "PRIVATE_DESKTOP" : "HIDDEN_DESKTOP", desktop);
    Require(isPrivate || hidden, "default desktop needs explicit hidden flag");
    auto hook = SetWindowsHookExW(WH_CBT, Guard, nullptr, GetCurrentThreadId());
    Require(hook != nullptr, "install hidden-window guard");
    auto app = std::make_unique<MMDApp>(); g_Block = app.get();
    app->state = MMDAppState{}; app->InitDefaults();
    Require(InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE), "initialize MMS");
    std::printf("PLUGINS mme=%d mmhack=%d\n", GetModuleHandleW(L"MMEffect.dll") != nullptr, GetModuleHandleW(L"MMHack.dll") != nullptr);
    LoadModelFile(app.get(), modelPath.c_str(), false);
    auto* model = mdl::Mdl(app->SelectedModel());
    Require(model && model->loadComplete, "load model through production loader");
    std::printf("MODEL vertices=%u materials=%u format=%u\n", model->vertexCount, model->materialCount, model->physicsMode);
    for (int i = 0; i != 10; ++i) std::printf("TOON index=%d file=%s shared=%p\n", i, model->pmdToonFileNames[i], app->ToonTexture(i+1));
    for (unsigned i = 0; i < model->materialCount; ++i) {
        const auto& m = mdl::Materials(app->SelectedModel())[i];
        std::printf("MATERIAL index=%u toon=%d texture=%ls diffuse=%.3f,%.3f,%.3f\n", i, int(m.toonReference),
            m.texturePath, m.diffuse[0], m.diffuse[1], m.diffuse[2]);
    }
    const auto chooseModel = [&](int item) {
        const auto combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
        Require(SendMessageW(combo, CB_SETCURSEL, item, 0) != CB_ERR, "choose model/camera combo");
        SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kMainComboModel, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    };
    const auto toggleSelect = [&]() {
        SendMessageW(app->MainWindow(), WM_COMMAND, MAKEWPARAM(panel::kBoneSelectRadio, BN_CLICKED),
            reinterpret_cast<LPARAM>(GetDlgItem(app->MainWindow(), panel::kBoneSelectRadio)));
    };
    chooseModel(model->comboSelIndex);
    app->PlaybackActive() = 0;
    for (int step = 0; step != 8; ++step) {
        if (step == 1 || step == 4 || step == 6) chooseModel(0);
        if (step == 2 || step == 5 || step == 7) chooseModel(model->comboSelIndex);
        if (step == 3) toggleSelect();
        app->state.messageSeen = 1; app->DeltaTime() = 1.f/30.f; FrameDriver(app.get());
        Capture(app.get(), output, step, fixture);
    }
    Require(!IsWindowVisible(app->MainWindow()), "test main window stays hidden");
    ShutdownCleanup(app.get()); g_Block = nullptr; UnhookWindowsHookEx(hook);
}
