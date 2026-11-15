#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "../../src/legacy_mmhack/tests/device_probe_base.hpp"
#include "../../src/plugins/original_camera_memory.hpp"
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace mikumikustudio;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a - b) < .0001f; }
unsigned errorDialogs = 0;
std::wstring errorText;
BOOL CALLBACK CloseError(HWND window, LPARAM) {
    wchar_t title[256]{}, text[16384]{};
    GetWindowTextW(window, title, 256);
    if (std::wcsstr(title, L"MMDFacade") && !GetPropW(window, L"TestSeen")) {
        SetPropW(window, L"TestSeen", reinterpret_cast<HANDLE>(1));
        ++errorDialogs; GetDlgItemTextW(window, 0xffff, text, 16384); errorText += text;
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
void CALLBACK ErrorTimer(HWND, UINT, UINT_PTR, DWORD) { EnumThreadWindows(GetCurrentThreadId(), CloseError, 0); }

}
int main(int argc, char** argv) {
    auto app = std::make_unique<MMDApp>();
    auto renderer = std::make_unique<D3DRenderer>();
    mme_re::mmhack::test::DeviceProbeBase device;
    const auto window = CreateWindowW(L"STATIC", L"Original camera input test", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto edit = CreateWindowW(L"EDIT", L"text", WS_CHILD, 0, 0, 100, 20, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    BYTE keys[256]{}, savedKeys[256]{}; GetKeyboardState(savedKeys);
    int result = 1;
    try {
        Check(window && edit, "test windows");
        Check(SetKeyboardState(keys), "deterministic test keyboard state");
        app->MainWindow() = window; app->Renderer() = renderer.get(); renderer->device = &device;
        app->CameraMode() = 1; app->ViewportRect() = {0, 30, 400, 300}; app->CameraDistance() = -10;
        const auto errorTimer = argc > 1 ? SetTimer(nullptr, 0, 25, ErrorTimer) : 0;
        legacy_mmd_plugin::Start(app.get());
        if (argc > 1) {
            if (errorTimer) KillTimer(nullptr, errorTimer);
            Check(!GetModuleHandleW(L"MMDFirstPersonCamera.dll"), "failed or disabled plugin was executed");
            const bool disabled = std::string(argv[1]) == "disabled";
            Check(disabled ? errorDialogs == 0 : errorDialogs == 1 && !errorText.empty(), "visible startup error policy");
            std::printf("PASS startup %s; error dialogs=%u\n", argv[1], errorDialogs);
            if (!errorText.empty()) std::wprintf(L"%ls\n", errorText.c_str());
            legacy_mmd_plugin::Stop(); SetKeyboardState(savedKeys);
            renderer->device = nullptr; app->Renderer() = nullptr; app->MainWindow() = nullptr;
            DestroyWindow(window); return 0;
        }
        const auto module = GetModuleHandleW(L"MMDFirstPersonCamera.dll");
        Check(module != nullptr, "original camera factory/start");
        Check(!GetProcAddress(module, "MMSLegacyCameraAdapterVersion"), "test substituted a source adapter");
        const auto getter = reinterpret_cast<void*(__cdecl*)()>(reinterpret_cast<unsigned char*>(module) + 0x1060);
        const auto* view = static_cast<const plugins::OriginalCameraFields926*>(getter());
        Check(view && view != reinterpret_cast<void*>(app.get()), "independent original memory view");
        Check(view->cameraMode == 1 && Near(view->distance, -10), "factory sees real initial state");
        MEMORY_BASIC_INFORMATION gap{};
        Check(VirtualQuery(reinterpret_cast<const unsigned char*>(view) + 0x10000, &gap, sizeof(gap)) && gap.State == MEM_RESERVE,
            "unsupported memory pages must remain inaccessible");
        SetFocus(window);
        Check(legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'W', 0), "original W callback");
        // v0.01 uses GetKeyState(VK_SHIFT)>0, not its high bit. At state zero it
        // chooses speed 4, and does not filter key release. Preserve these facts.
        Check(Near(app->CameraPositionZ(), 4) && Near(view->position[2], 4), "original memory writes reach MMS");
        legacy_mmd_plugin::HandleInput(window, WM_KEYUP, 'W', 0);
        Check(Near(app->CameraPositionZ(), 8), "original key-up behavior differs from adapter");
        app->CameraPositionX() = 10; app->CameraPositionY() = 20; app->CameraPositionZ() = 30;
        legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'D', 0);
        Check(Near(app->CameraPositionX(), 12) && Near(view->position[1], 20) && Near(view->position[2], 30), "host changes refresh before callback");
        Check(getter() == view, "retained pointer must stay stable");
        SetFocus(edit);
        Check(!legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'W', 0) && Near(app->CameraPositionZ(), 30), "text focus gate");
        SetFocus(window); app->CameraMode() = 0;
        Check(!legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'W', 0) && Near(app->CameraPositionZ(), 30), "model mode gate");
        Check(view->cameraMode == 0, "mode view follows real host state");
        app->CameraMode() = 1;
        CURSORINFO before{sizeof(before)}, after{sizeof(after)}; Check(GetCursorInfo(&before), "cursor state");
        POINT cursor{}; Check(GetCursorPos(&cursor), "cursor position");
        Check(legacy_mmd_plugin::HandleInput(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100)), "original mouse capture");
        POINT end{150, 125}; ClientToScreen(window, &end);
        legacy_mmd_plugin::HandleInput(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(150, 125));
        Check(Near(app->CameraYaw(), -(end.x - cursor.x) / 500.f) && Near(app->CameraPitch(), -(end.y - cursor.y) / 500.f), "original mouse rotation");
        Check(Near(app->CameraDistance(), 0) && Near(view->distance, 0) && Near(app->CameraPositionZ(), 20), "original distance/position conversion");
        legacy_mmd_plugin::HandleInput(window, WM_KILLFOCUS, 0, 0); GetCursorInfo(&after);
        Check(!GetCapture() && before.flags == after.flags, "original focus-loss cleanup");
        Check(legacy_mmd_plugin::HandleInput(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100)), "capture before mode transition");
        app->CameraMode() = 0;
        legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'W', 0); GetCursorInfo(&after);
        Check(!GetCapture() && before.flags == after.flags && view->cameraMode == 0, "release old gesture before mode sync");
        app->CameraMode() = 1;
        Check(legacy_mmd_plugin::HandleInput(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100)), "capture before stop");
        legacy_mmd_plugin::Stop(); GetCursorInfo(&after);
        Check(!GetCapture() && before.flags == after.flags, "stop restores original cursor/capture");
        Check(!GetModuleHandleW(L"MMDFirstPersonCamera.dll"), "original DLL unload");
        MEMORY_BASIC_INFORMATION released{};
        Check(VirtualQuery(view, &released, sizeof(released)) && released.State == MEM_FREE, "view released after object destruction");
        const auto z = app->CameraPositionZ();
        Check(!legacy_mmd_plugin::HandleInput(window, WM_KEYDOWN, 'W', 0) && app->CameraPositionZ() == z, "input after stop");
        std::puts("PASS ORIGINAL DLL: factory, stable memory, two-way camera state, keyboard, mouse, mode, focus, stop and destruction");
        result = 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); }
    legacy_mmd_plugin::Stop(); SetKeyboardState(savedKeys);
    renderer->device = nullptr; app->Renderer() = nullptr; app->MainWindow() = nullptr;
    DestroyWindow(window); return result;
}
