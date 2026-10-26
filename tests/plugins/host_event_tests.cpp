// Real host-event probe: initializes the production plugin runtime on a hidden
// window, queues lifecycle notifications through the same entries the host uses
// and verifies the events reach a real plugin with valid payloads.
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <mms/plugin.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
using RenderReentryHook = void (MMS_CALL*)(void*);
using SetRenderHook = void (MMS_CALL*)(RenderReentryHook, void*);
using GetRenderCounts = void (MMS_CALL*)(uint64_t*);
struct RenderReentryProbe { MMDApp* app; unsigned calls = 0; };
static void MMS_CALL ReenterRender(void* context) {
    auto& probe = *static_cast<RenderReentryProbe*>(context);
    ++probe.calls;
    plugins::PluginFrameBegin(probe.app);
    plugins::PluginFramePresented(probe.app);
    plugins::PluginFrameEnd(probe.app);
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

    plugins::PluginFrameBegin(&app);
    plugins::PluginFramePresented(&app);
    plugins::PluginFrameEnd(&app);
    const wchar_t* pluginsRoot = _wgetenv(L"MMS_PLUGIN_TEST_DIR");
    const auto fixturePath = pluginsRoot ? std::filesystem::path(pluginsRoot) / L"test" / L"lifecycle.dll" : std::filesystem::path{};
    HMODULE fixture = fixturePath.empty() ? nullptr : GetModuleHandleW(fixturePath.c_str());
    const auto setHook = fixture ? reinterpret_cast<SetRenderHook>(GetProcAddress(fixture, "MmsTestSetRenderReentryHook")) : nullptr;
    const auto getCounts = fixture ? reinterpret_cast<GetRenderCounts>(GetProcAddress(fixture, "MmsTestGetRenderProbeCounts")) : nullptr;
    Check(setHook && getCounts, "loaded lifecycle fixture exposes reverse-reentry probe");
    RenderReentryProbe reentry{&app};
    if (setHook && getCounts) setHook(ReenterRender, &reentry);
    plugins::PluginNotifyLanguageChanged(&app);
    if (setHook && getCounts) {
        Check(WaitFor(logPath, "RENDER_REENTRY_END", 800), "ordinary language event executes reverse-reentry hook");
        const auto reentryBody = ReadLog(logPath);
        Check(reentry.calls == 1 && reentryBody.find("RENDER_REENTRY_PASS") != std::string::npos &&
              reentryBody.find("RENDER_REENTRY_FAIL") == std::string::npos,
              "ordinary plugin callback cannot dispatch nested render callbacks or advance frame index");
        uint64_t before[3]{}, after[3]{};
        getCounts(before);
        plugins::PluginFrameBegin(&app);
        plugins::PluginFramePresented(&app);
        plugins::PluginFrameEnd(&app);
        getCounts(after);
        Check(after[0] == before[0] + 1 && after[1] == before[1] + 1 && after[2] == before[2] + 1,
              "all render notifications resume after ordinary plugin callback returns");
        setHook(nullptr, nullptr);
    }
    plugins::PluginNotifyDeviceLost(&app);
    Check(WaitFor(logPath, "EVENT_DEVICE_LOST\nRENDER_LOST_QUERY", 800),
        "device lost reaches the plugin before restoration");
    plugins::PluginNotifyDeviceRestored(&app);
    Check(WaitFor(logPath, "EVENT_DEVICE_RESTORED\nRENDER_RESTORED_QUERY", 800),
        "device restoration refreshes the render generation");
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
        "EVENT_LANGUAGE\nEVENT_PROJECT_CLOSED\n"
        "EVENT_OBJECT_REMOVED\nEVENT_DPI\nEVENT_PROPERTY_CHANGED\n"
        "RENDER_INFO\nRENDER_LEASE_ACQUIRED\nRENDER_LEASE_RELEASED\n"
        "RENDER_LOST_QUERY\nRENDER_RESTORED_QUERY\nRENDER_FRAME_SEEN\nLAYOUT_HISTORY_PASS\n"
        "FRAME_CALLBACK_REGISTERED\nFRAME_CALLBACK frame=\n"
        "FRAME_PHASE_REGISTERED\nFRAME_PHASE_BEGIN\nFRAME_PHASE_END", 1200),
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
          body.find("PROPERTY_VALUE_FAIL") == std::string::npos &&
          body.find("RENDER_QUERY_FAIL") == std::string::npos &&
          body.find("RENDER_LEASE_FAIL") == std::string::npos &&
          body.find("RENDER_SERVICE_MISSING") == std::string::npos &&
          body.find("LAYOUT_HISTORY_MISSING") == std::string::npos &&
          body.find("RENDER_QUERY_FAIL") == std::string::npos &&
          body.find("FRAME_CALLBACK_FAIL") == std::string::npos &&
          body.find("FRAME_PHASE_FAIL") == std::string::npos,
        "no event payload validation failures");
    Check(ReadLog(logPath).find("frame=1 ") != std::string::npos,
        "presented frame reaches the render service");
    plugins::Shutdown();
    ShutdownCleanup(&app);
    g_Block = nullptr;
    app.Hwnd() = nullptr;
    app.MainWindow() = nullptr;
    std::printf("host events %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
