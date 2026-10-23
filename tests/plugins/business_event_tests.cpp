// Runtime business event delivery: the manager coalesces UI-registry and
// layout mutations into deferred events so a plugin never observes a partial
// layout transaction. The lifecycle fixture records the payload contract.
#include "manager.hpp"
#include "native_ui.hpp"
#include <Windows.h>
#include <native_ui/layout.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace mikumikustudio::plugins;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
static MmsString S(const char* s) { return {s, std::strlen(s)}; }
static std::string Wide(const std::string& text) { return text; }
static void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary); f << bytes; if (!f) throw std::runtime_error("fixture write failed");
}
static std::string ManifestText(std::string id = "org.example.gui-state-events", std::string entry = "lifecycle.dll") {
    return "{\"schemaVersion\":1,\"id\":\"" + id + "\",\"name\":\"test\",\"version\":\"1.0.0\",\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},\"entry\":\"" + entry + "\",\"architecture\":\"x64\",\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
}
static void BusinessEventChecks(const fs::path& root, const fs::path& fixture) {
    const auto base = root / "events";
    const auto package = base / "PLUGINS" / "test";
    fs::create_directories(package);
    fs::copy_file(fixture, package / "lifecycle.dll");
    Write(package / "plugin.json", ManifestText());
    std::vector<std::string> logs;
    Manager manager(base / "userdata", [&](const auto& s) { logs.push_back(s); }, std::function<void()>{});
    manager.Load(package.parent_path());
    auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(marker) != std::string::npos; }); };
    CHECK(count("NEGOTIATION_PASS") == 1);
    manager.Drain();
    const auto layoutOnly = count("EVENT_LAYOUT");
    UiClient client{&manager.Ui(), "org.example.events"};
    auto api = manager.Ui().Api(client);
    MmsHandle layout = 0;
    CHECK(api.beginLayout(api.context, manager.Ui().Revision(), &layout) == MMS_OK);
    CHECK(api.commitLayout(api.context, layout) == MMS_OK);
    manager.Drain();
    CHECK(count("EVENT_UI_REGISTRY") >= 1);
    CHECK(count("EVENT_LAYOUT") == layoutOnly + 1);
    CHECK(count("REVISION_PAYLOAD_FAIL") == 0 && count("REVISION_ZERO_FAIL") == 0);
    // A real NativeUi layout observes command states; the lifecycle probe flips
    // its checked bit on a 50 ms cadence for ids containing gui-state.
    HWND window = CreateWindowExW(0, L"STATIC", L"event host ui", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        0, 0, 640, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr && SetMenu(window, CreateMenu()));
    NativeUi ui(manager.Ui(), window, [&](const std::string& s) { logs.push_back(s); });
    ui.Refresh();
    unsigned stateChanged = 0;
    ui.SetCommandStateChanged([&](uint64_t revision) {
        ++stateChanged;
        MmsRevisionEventV1 payload{Header<MmsRevisionEventV1>(), revision};
        manager.Event(MMS_EVENT_COMMAND_STATE_CHANGED, {&payload, sizeof(payload)}, 1);
    });
    ui.DetectCommandStateChanges();
    {
        UiClient visitor{&manager.Ui(), "example.visitor"};
        auto invokeApi = manager.Ui().Api(visitor);
        CHECK(invokeApi.invoke(invokeApi.context, S("org.example.gui-state-events.command"), {}, {}) == MMS_OK);
    }
    for (int attempt = 0; attempt < 12 && !stateChanged; ++attempt) {
        Sleep(10);
        ui.DetectCommandStateChanges();
    }
    CHECK(stateChanged >= 1);
    manager.Drain();
    CHECK(count("EVENT_COMMAND_STATE") >= 1);
    CHECK(count("COMMAND_STATE_PAYLOAD_FAIL") == 0 && count("COMMAND_STATE_REVISION_FAIL") == 0);
    ui.SetCommandStateChanged({});
    DestroyWindow(window);
    CHECK(manager.Stop());
    std::cout << "business events PASS checks=" << checks << "\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: business_event_tests <output-parent> <lifecycle.dll>\n"); return 2; }
    const fs::path parent = fs::absolute(argv[1]);
    const fs::path root = parent / ("run-" + std::to_string(GetCurrentProcessId()));
    try {
        if (fs::exists(root)) fs::remove_all(root);
        fs::create_directories(root);
        BusinessEventChecks(root, fs::absolute(argv[2]));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
