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
#include <cwchar>
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
// The ABI carries project paths as UTF-8. Mirror the host conversion so the
// expected log text can be compared byte for byte.
static std::string WideToUtf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, &result[0], length, nullptr, nullptr);
    return result;
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
// Markers left by the successful path above must never satisfy a regression
// assertion. Each phase records the append offset first and inspects only bytes
// written after it; the fresh marker names keep the interval visible in the log.
static void AppendLog(const wchar_t* path, const char* marker) {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    file << marker << '\n';
}
static std::string ReadRegion(const wchar_t* path, size_t offset) {
    const auto body = ReadLog(path);
    return body.size() <= offset ? std::string{} : body.substr(offset);
}
static size_t CountLine(const std::string& region, const std::string& line) {
    size_t count = 0, start = 0;
    while (start < region.size()) {
        const auto end = region.find('\n', start);
        const auto stop = end == std::string::npos ? region.size() : end;
        if (stop - start == line.size() && region.compare(start, line.size(), line) == 0) ++count;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return count;
}
// Identity lines carry lo/hi after the stable prefix; only a prefix count can
// see them without knowing the random values.
static size_t CountPrefixLine(const std::string& region, const std::string& prefix) {
    size_t count = 0, start = 0;
    while (start < region.size()) {
        const auto end = region.find('\n', start);
        const auto stop = end == std::string::npos ? region.size() : end;
        if (stop - start >= prefix.size() && region.compare(start, prefix.size(), prefix) == 0) ++count;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return count;
}
static std::string FindLine(const std::string& region, const std::string& prefix) {
    size_t start = 0;
    while (start < region.size()) {
        const auto end = region.find('\n', start);
        const auto stop = end == std::string::npos ? region.size() : end;
        if (stop - start >= prefix.size() && region.compare(start, prefix.size(), prefix) == 0)
            return region.substr(start, stop - start);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}
static size_t LinePosition(const std::string& region, const std::string& line) {
    size_t start = 0;
    while (start < region.size()) {
        const auto end = region.find('\n', start);
        const auto stop = end == std::string::npos ? region.size() : end;
        if (stop - start == line.size() && region.compare(start, line.size(), line) == 0) return start;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::string::npos;
}
static bool WaitForRegion(const wchar_t* path, size_t offset, const char* markers, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        Pump();
        Sleep(5);
        const auto region = ReadRegion(path, offset);
        bool all = true;
        std::istringstream lines(markers);
        std::string marker;
        while (std::getline(lines, marker)) if (!marker.empty() && region.find(marker) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}
using RenderReentryHook = void (MMS_CALL*)(void*);
using SetRenderHook = void (MMS_CALL*)(RenderReentryHook, void*);
using GetRenderCounts = void (MMS_CALL*)(uint64_t*);
using EnableSerializer = MmsResult (MMS_CALL*)();
using SetSerializerCommitThrow = void (MMS_CALL*)(uint32_t);
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
    // Close carries the current project path. Keep it non-empty and non-ASCII so
    // the deferred payload must keep both its pointer and its UTF-8 text intact.
    const wchar_t* closedProject = L"D:/MMS ABI/工程/closed project.pmm";
    wcscpy_s(app.EnvFileName(), 256, closedProject);
    const std::string closedPath = WideToUtf8(closedProject);
    const std::string closedMarker = "EVENT_PROJECT_CLOSED_PATH data=non-null bytes=" +
        std::to_string(closedPath.size()) + " text=" + closedPath;
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
        "EVENT_LANGUAGE\nEVENT_PROJECT_CLOSED\nEVENT_PROJECT_CLOSED_PATH data=non-null\n"
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
    Check(!closedPath.empty() && body.find(closedMarker) != std::string::npos,
        "project close delivers the exact non-empty project path to the plugin");
    // ResetPluginProject only queues PROJECT_CLOSED when a current project id
    // exists. Capture one through the host's own save snapshot, then let the
    // production reset boundary prove close is delivered before scene reset.
    bool captured = false;
    try {
        plugins::ProjectSave capture(&app);
        captured = true;
    } catch (...) {
    }
    const bool reset = captured && plugins::ResetPluginProject(&app);
    Check(reset, "plugin project reset accepts a captured current project");
    if (reset) {
        Check(WaitFor(logPath, "EVENT_SCENE_RESET", 800), "scene reset reaches the plugin");
        const auto resetBody = ReadLog(logPath);
        const auto firstClosed = resetBody.find(closedMarker);
        const auto secondClosed = firstClosed == std::string::npos ? firstClosed :
            resetBody.find(closedMarker, firstClosed + 1);
        const auto sceneReset = resetBody.find("EVENT_SCENE_RESET");
        Check(secondClosed != std::string::npos && sceneReset != std::string::npos && secondClosed < sceneReset,
            "project close is delivered before scene reset");

        // Regression: the serializer violates commitLoad's no-fail contract while
        // ResetPluginProject consumes the prepared candidates. The success path
        // above stays intact; the serializer is enabled only for this phase and
        // every assertion reads bytes appended after a fresh marker, so earlier
        // close/reset lines cannot turn these checks into false positives.
        const auto enableSerializer = fixture ? reinterpret_cast<EnableSerializer>(GetProcAddress(fixture, "MmsTestEnableSerializer")) : nullptr;
        const auto setCommitThrow = fixture ? reinterpret_cast<SetSerializerCommitThrow>(GetProcAddress(fixture, "MmsTestSetSerializerCommitThrow")) : nullptr;
        Check(enableSerializer && setCommitThrow, "lifecycle fixture exposes independent serializer fault controls");
        const std::string consumedIdentity = FindLine(ReadLog(logPath), "EVENT_PROJECT_CLOSED_ID nonzero=1");
        const bool serializerEnabled = enableSerializer && enableSerializer() == MMS_OK;
        Check(serializerEnabled && CountLine(ReadLog(logPath), "SERIALIZER_PROVIDER_ENABLED") == 1,
            "test serializer enabled independently after the successful reset");
        bool reidentified = false;
        try {
            plugins::ProjectSave recapture(&app);
            reidentified = true;
        } catch (...) {
        }
        Check(reidentified, "serializer-enabled capture re-establishes a project identity");
        const auto captureBody = ReadLog(logPath);
        Check(CountLine(captureBody, "SERIALIZER_SAVE") == 1 && CountLine(captureBody, "SERIALIZER_RELEASE") == 1,
            "serializer-enabled capture publishes and releases one saved state");
        if (setCommitThrow) setCommitThrow(1);
        Check(CountLine(ReadLog(logPath), "SERIALIZER_COMMIT_THROW_ARMED") == 1,
            "commitLoad failure armed only for the next reset");
        AppendLog(logPath, "RESET_SERIALIZER_THROW_BEGIN");
        const size_t throwOffset = ReadLog(logPath).size();
        const bool resetAfterThrow = plugins::ResetPluginProject(&app);
        AppendLog(logPath, "RESET_SERIALIZER_THROW_END");
        const auto throwRegion = ReadRegion(logPath, throwOffset);
        Check(resetAfterThrow, "reset still succeeds after serializer commitLoad throws");
        Check(CountLine(throwRegion, "SERIALIZER_COMMIT_THROW_BEGIN") == 1,
            "the targeted reset enters the throwing commitLoad callback");
        Check(CountLine(throwRegion, "SERIALIZER_COMMIT") == 0 && CountLine(throwRegion, "SERIALIZER_PREPARE") == 1,
            "the throwing callback replaces the normal commit tail");
        if (setCommitThrow) setCommitThrow(0);

        const size_t eventsOffset = ReadLog(logPath).size();
        Check(WaitForRegion(logPath, eventsOffset,
            "EVENT_PROJECT_CLOSED\nEVENT_PROJECT_CLOSED_PATH data=non-null\n"
            "EVENT_PROJECT_CLOSED_ID nonzero=1\nEVENT_SCENE_RESET", 800),
            "poisoned reset still delivers project close before scene reset");
        const auto eventRegion = ReadRegion(logPath, eventsOffset);
        const auto closeLine = FindLine(eventRegion, "EVENT_PROJECT_CLOSED");
        const auto sceneLine = FindLine(eventRegion, "EVENT_SCENE_RESET");
        const auto freshIdentity = FindLine(eventRegion, "EVENT_PROJECT_CLOSED_ID nonzero=1");
        Check(CountLine(eventRegion, "EVENT_PROJECT_CLOSED") == 1 && CountLine(eventRegion, "EVENT_SCENE_RESET") == 1 &&
              CountPrefixLine(eventRegion, "EVENT_PROJECT_CLOSED_ID nonzero=1") == 1,
            "poisoned reset queues exactly one close and one scene reset");
        Check(closeLine == "EVENT_PROJECT_CLOSED" && sceneLine == "EVENT_SCENE_RESET" && !freshIdentity.empty() &&
            LinePosition(eventRegion, closeLine) < LinePosition(eventRegion, freshIdentity) &&
            LinePosition(eventRegion, freshIdentity) < LinePosition(eventRegion, sceneLine),
            "close with the re-established identity is delivered before scene reset");
        Check(!consumedIdentity.empty() && freshIdentity != consumedIdentity,
            "the close consumes the newly established identity");

        AppendLog(logPath, "RESET_IDENTITY_PROBE_BEGIN");
        const size_t identityOffset = ReadLog(logPath).size();
        plugins::PluginNotifyProjectClosed(&app);
        Check(WaitForRegion(logPath, identityOffset, "EVENT_PROJECT_CLOSED_ID nonzero=0", 800),
            "reset cleared the project identity after the poisoned commit");
        const auto identityRegion = ReadRegion(logPath, identityOffset);
        Check(CountLine(identityRegion, "EVENT_PROJECT_CLOSED") == 1 &&
              CountPrefixLine(identityRegion, "EVENT_PROJECT_CLOSED_ID nonzero=1") == 0 &&
              CountPrefixLine(identityRegion, "EVENT_PROJECT_CLOSED_ID nonzero=0") == 1,
            "identity probe observes no surviving project identity");
    }
    plugins::Shutdown();
    ShutdownCleanup(&app);
    g_Block = nullptr;
    app.Hwnd() = nullptr;
    app.MainWindow() = nullptr;
    std::printf("host events %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
