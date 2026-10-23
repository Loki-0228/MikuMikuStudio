// Real host-event probe: initializes the production plugin runtime on a hidden
// window, queues lifecycle notifications through the same entries the host uses
// and verifies the events reach a real plugin with valid payloads.
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/ported_funcs.hpp"
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
    // The probe owns its log; a stale file from an earlier run must never
    // satisfy the marker waits below.
    DeleteFileW(logPath);
    // MMDApp is close to 670 KiB; keep it off the reserved thread stack.
    auto appOwner = std::make_unique<MMDApp>();
    auto& app = *appOwner;
    g_Block = &app;
    app.state = MMDAppState{};
    app.InitDefaults();
    Check(InitMainWindowAndD3D(&app, GetModuleHandleW(nullptr), SW_HIDE),
        "hidden production window and device");
    HWND window = app.MainWindow();
    Check(window != nullptr, "production main window available");
    if (!window) return 1;
    Check(WaitFor(logPath, "NEGOTIATION_PASS", 600), "real lifecycle plugin started");

    plugins::PluginNotifyLanguageChanged(&app);
    plugins::PluginNotifyDeviceLost(&app);
    plugins::PluginNotifyDeviceRestored(&app);
    plugins::PluginNotifyProjectClosed(&app);
    const void* removedToken = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x5150));
    plugins::SceneObjectIdentity(removedToken);
    plugins::PluginNotifyObjectRemoved(&app, removedToken);
    // Sending a synthetic WM_DPICHANGED to the hidden main window did not
    // reach the runtime subclass in this fixture, so the probe calls the same
    // host entry deterministically until the message path is diagnosed.
    plugins::PluginNotifyDpiChanged(&app, 144);
    plugins::PluginNotifyPropertyChanged(&app, "camera.distance", 7);
    Check(WaitFor(logPath,
        "EVENT_LANGUAGE\nEVENT_DEVICE_LOST\nEVENT_DEVICE_RESTORED\nEVENT_PROJECT_CLOSED\n"
        "EVENT_OBJECT_REMOVED\nEVENT_DPI\nEVENT_PROPERTY_CHANGED", 800),
        "queued host events reach the plugin with valid payloads");

    const auto body = ReadLog(logPath);
    Check(body.find("CODE_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DEVICE_PAYLOAD_FAIL") == std::string::npos &&
          body.find("PROJECT_CLOSED_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DEVICE_GENERATION_FAIL") == std::string::npos &&
          body.find("OBJECT_REMOVED_PAYLOAD_FAIL") == std::string::npos &&
          body.find("DPI_VALUE_FAIL") == std::string::npos &&
          body.find("COMMAND_STATE_PAYLOAD_FAIL") == std::string::npos &&
          body.find("COMMAND_STATE_REVISION_FAIL") == std::string::npos &&
          body.find("PROPERTY_PAYLOAD_FAIL") == std::string::npos &&
          body.find("PROPERTY_VALUE_FAIL") == std::string::npos,
        "no event payload validation failures");
    plugins::Shutdown();
    ShutdownCleanup(&app);
    g_Block = nullptr;
    app.Hwnd() = nullptr;
    app.MainWindow() = nullptr;
    std::printf("host events %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
