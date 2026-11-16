#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "../../src/legacy_mmhack/tests/device_probe_base.hpp"
#include <MinHook.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace {
std::vector<int> events;
HRESULT observedResult;
const void* observedParameters;
const void* createdDevice;
bool stopInReset = false;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int Count(int event) { return int(std::count(events.begin(), events.end(), event)); }
struct Device : mme_re::mmhack::test::DeviceProbeBase {
    HRESULT result = D3DERR_DEVICELOST;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*) override { events.push_back(25); return result; }
};
struct StartupMessages {
    inline static StartupMessages* active = nullptr;
    HWND owner;
    const wchar_t* expected = nullptr;
    unsigned count = 0;
    bool unexpected = false;

    StartupMessages(HWND window, const std::wstring& mode) : owner(window) {
        if (mode == L"missing") expected = L"MMDPlugin.dll compatibility component is missing";
        if (mode == L"unsupported") expected = L"unsupported legacy MMDPlugin profile";
        if (mode == L"wrong-version") expected = L"version does not match";
        Check(MH_Initialize() == MH_OK, "initialize test-local notification hook");
        active = this;
        Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW),
            reinterpret_cast<void*>(&Observe), nullptr) == MH_OK, "capture startup notification");
        Check(MH_EnableHook(reinterpret_cast<void*>(&MessageBoxW)) == MH_OK, "enable startup notification capture");
    }
    ~StartupMessages() {
        MH_DisableHook(reinterpret_cast<void*>(&MessageBoxW));
        MH_RemoveHook(reinterpret_cast<void*>(&MessageBoxW));
        MH_Uninitialize();
        active = nullptr;
    }
    static int WINAPI Observe(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
        auto& self = *active;
        ++self.count;
        self.unexpected |= !self.expected || owner != self.owner || !text || !title ||
            wcscmp(title, L"MMDFacade：旧插件加载失败") != 0 || flags != (MB_OK | MB_ICONERROR) ||
            (self.expected && text && !wcsstr(text, self.expected));
        return IDOK;
    }
    void Verify() const {
        Check(!unexpected && count == (expected ? 1u : 0u), "exactly the expected startup error is reported");
    }
};
}
extern "C" __declspec(dllexport) void __cdecl MMSFacadeTestEvent(int value, HRESULT result, const void* pointer) {
    events.push_back(value);
    if (value == 20) createdDevice = pointer;
    if (value == 22) { observedResult = result; observedParameters = pointer; }
    if (value == 21 && stopInReset) legacy_mmd_plugin::Stop();
}
int wmain(int argc, wchar_t** argv) {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* real = nullptr;
    std::unique_ptr<MMDApp> app;
    std::unique_ptr<D3DRenderer> renderer;
    Device device;
    try {
        const std::wstring mode = argc > 1 ? argv[1] : L"enabled";
        window = CreateWindowW(L"STATIC", L"MMDFacade callback test", WS_OVERLAPPEDWINDOW,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "hidden window creation");
        StartupMessages messages(window, mode);
        app = std::make_unique<MMDApp>();
        renderer = std::make_unique<D3DRenderer>();
        app->MainWindow() = window;
        app->Renderer() = renderer.get();
        renderer->device = &device;
        legacy_mmd_plugin::Start(app.get());
        messages.Verify();
        if (mode == L"disabled" || mode == L"missing" || mode == L"unsupported" || mode == L"wrong-version") {
            Check(events.empty(), "rejected/disabled package executed a factory");
            legacy_mmd_plugin::Stop();
        } else {
            Check(Count(20) == 1 && createdDevice == &device, "ABI 2 factory / borrowed device");
            Check(Count(30) == 1 && Count(31) == 1, "ABI 3 factory / start");
            SendMessageW(window, WM_SHOWWINDOW, FALSE, 0);
            Check(Count(32) == 1, "ABI 3 window observer regression");
            D3DPRESENT_PARAMETERS parameters{};
            events.clear();
            if (mode == L"throw") SetEnvironmentVariableW(L"MMDFACADE_TEST_THROW", L"1");
            if (mode == L"post-throw") SetEnvironmentVariableW(L"MMDFACADE_TEST_POST_THROW", L"1");
            if (mode == L"stop") stopInReset = true;
            const auto result = legacy_mmd_plugin::ResetDevice(&device, &parameters);
            Check(result == D3DERR_DEVICELOST, "PostReset must not hide device failure");
            if (mode == L"throw") {
                Check(events == std::vector<int>({21,23,25}), "throw must destroy before unload, without post callback");
                SetEnvironmentVariableW(L"MMDFACADE_TEST_THROW", nullptr);
            } else {
                Check(events.size() >= 3 && events[0] == 21 && events[1] == 25 && events[2] == 22,
                    "pre / actual Reset / post order");
                Check(observedResult == result && observedParameters == &parameters, "post result and parameter identity");
            }
            legacy_mmd_plugin::Stop();
            SetEnvironmentVariableW(L"MMDFACADE_TEST_POST_THROW", nullptr);
            Check(Count(23) == 1 && Count(33) == 1 && Count(34) == 1, "exactly once destroy2 / stop / destroy3");
            const auto stoppedCount = events.size();
            SendMessageW(window, WM_SHOWWINDOW, FALSE, 0);
            Check(events.size() == stoppedCount, "observer called after stop");
            Check(!GetModuleHandleW(L"reset.dll") && !GetModuleHandleW(L"observer.dll") &&
                !GetModuleHandleW(L"MMDPlugin.dll"), "DLLs did not unload");
            if (mode == L"enabled") {
                d3d = Direct3DCreate9(D3D_SDK_VERSION);
                Check(d3d != nullptr, "Direct3DCreate9");
                parameters = {};
                parameters.Windowed = TRUE;
                parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
                parameters.hDeviceWindow = window;
                Check(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &real)), "real D3D9 device creation");
                renderer->device = real;
                events.clear();
                legacy_mmd_plugin::Start(app.get());
                messages.Verify();
                Check(createdDevice == real && Count(20) == 1, "real device borrowed by factory");
                events.clear();
                const auto reset = legacy_mmd_plugin::ResetDevice(real, &parameters);
                Check(SUCCEEDED(reset) && events == std::vector<int>({21,22}) && observedResult == reset,
                    "real D3D9 Reset callback pair");
                legacy_mmd_plugin::Stop();
                real->Release(); real = nullptr;
                d3d->Release(); d3d = nullptr;
            }
        }
        renderer->device = nullptr;
        app->Renderer() = nullptr;
        app->MainWindow() = nullptr;
        DestroyWindow(window); window = nullptr;
        std::puts("PASS MMDFacade callback lifecycle");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        legacy_mmd_plugin::Stop();
        if (renderer) renderer->device = nullptr;
        if (app) { app->Renderer() = nullptr; app->MainWindow() = nullptr; }
        if (real) real->Release();
        if (d3d) d3d->Release();
        if (window) DestroyWindow(window);
        return 1;
    }
}
