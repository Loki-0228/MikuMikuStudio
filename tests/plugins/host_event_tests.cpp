// Real host-event probe: initializes the production plugin runtime on a hidden
// window, queues lifecycle notifications through the same entries the host uses
// and verifies the events reach a real plugin with valid payloads.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace mikumikustudio;

static int failures = 0;
static void Check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
static std::string ReadLog(const wchar_t* path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream body;
    body << file.rdbuf();
    return body.str();
}
static void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
static bool WaitFor(const wchar_t* path, const char* markers, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        Pump();
        Sleep(5);
        const auto body = ReadLog(path);
        bool all = true;
        std::istringstream lines(markers);
        std::string marker;
        while (std::getline(lines, marker)) if (!marker.empty() && body.find(marker) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}
int wmain() {
    const wchar_t* logPath = _wgetenv(L"MMS_ABI_PROBE_LOG");
    if (!logPath || !*logPath) { std::fprintf(stderr, "MMS_ABI_PROBE_LOG missing\n"); return 2; }
    // MMDApp is close to 670 KiB; keep it off the reserved thread stack.
    auto appOwner = std::make_unique<MMDApp>();
    auto& app = *appOwner;
    app.state = MMDAppState{};
    app.InitDefaults();
    HWND window = CreateWindowExW(0, L"STATIC", L"host event test",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 400, 300,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "hidden production window");
    if (!window) return 1;
    app.MainWindow() = window;
    app.Hwnd() = window;
    app.WindowLayoutReady() = 1;
    plugins::Initialize(&app);
    Check(WaitFor(logPath, "NEGOTIATION_PASS", 600), "real lifecycle plugin started");

    plugins::PluginNotifyLanguageChanged(&app);
    plugins::PluginNotifyDeviceLost(&app);
    plugins::PluginNotifyDeviceRestored(&app);
    plugins::PluginNotifyProjectClosed(&app);
    const void* removedToken = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x5150));
    plugins::SceneObjectIdentity(removedToken);
    plugins::PluginNotifyObjectRemoved(&app, removedToken);
    // The runtime window subclass handles WM_DPICHANGED in production; this
    // focused probe calls the same host entry deterministically.
    plugins::PluginNotifyDpiChanged(&app, 144);
    Check(WaitFor(logPath,
        "EVENT_LANGUAGE\nEVENT_DEVICE_LOST\nEVENT_DEVICE_RESTORED\nEVENT_PROJECT_CLOSED\n"
        "EVENT_OBJECT_REMOVED\nEVENT_DPI", 800),
        "queued host events reach the plugin with valid payloads");

    const auto body = ReadLog(logPath);
    Check(body.find("CODE_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DEVICE_PAYLOAD_FAIL") == std::string::npos &&
          body.find("PROJECT_CLOSED_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DEVICE_GENERATION_FAIL") == std::string::npos &&
          body.find("OBJECT_REMOVED_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DPI_VALUE_FAIL") == std::string::npos,
        "no event payload validation failures");
    plugins::Shutdown();
    DestroyWindow(window);
    app.Hwnd() = nullptr;
    app.MainWindow() = nullptr;
    std::printf("host events %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
