#include "manager.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "settings_fixture_plugin.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>

namespace {
using namespace mikumikustudio::plugins;
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
constexpr char owner[] = "org.example.settings-owner";
constexpr char other[] = "org.example.settings-other";

struct TestRoot {
    fs::path path;
    explicit TestRoot(const fs::path& parent)
        : path(fs::absolute(parent) / ("run-" + std::to_string(GetCurrentProcessId()) + "-" +
              std::to_string(GetTickCount64()))) {
        CHECK(fs::create_directories(path));
    }
    ~TestRoot() { std::error_code error; fs::remove_all(path, error); }
};
void Write(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    CHECK(bool(output));
}
void WriteManifest(const fs::path& package, const char* id, const char* entry, bool optIn) {
    const auto capability = optIn ? "settings.global" : "project.storage";
    const auto required = optIn ? R"({"id":"mms.settings","major":1,"minMinor":0})" : "";
    const std::string manifest = std::string(R"({"schemaVersion":1,"id":")") + id +
        R"(","name":"Global settings fixture","version":"1.0.0","kind":"native",)" +
        R"("abi":{"major":1,"minMinor":0},"entry":")" + entry +
        R"(","architecture":"x64","minWindows":"6.1.7601","capabilities":[")" + capability +
        R"("],"requiresServices":[)" + required +
        R"(],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})";
    Write(package / "plugin.json", manifest);
}
void Package(const fs::path& package, const fs::path& source, const char* id,
             const char* entry, bool optIn) {
    CHECK(fs::create_directories(package));
    CHECK(fs::copy_file(source, package / entry));
    WriteManifest(package, id, entry, optIn);
}
void LogMessage(const std::string& message) { std::printf("manager: %s\n", message.c_str()); }
PluginStatus FindStatus(const Manager& manager, const char* id) {
    const auto statuses = manager.Status();
    const auto found = std::find_if(statuses.begin(), statuses.end(), [&](auto& s) { return s.id == id; });
    CHECK(found != statuses.end());
    return *found;
}
MmsString TextView(std::string_view text) { return {text.data(), text.size()}; }
struct Plugin {
    decltype(&MmsSettingsFixtureObserve) observe;
    decltype(&MmsSettingsFixtureQueue) queue;
    decltype(&MmsSettingsFixtureDirect) direct;
    explicit Plugin(const fs::path& dll) {
        const auto module = GetModuleHandleW(dll.c_str());
        CHECK(module);
        observe = reinterpret_cast<decltype(observe)>(GetProcAddress(module, "MmsSettingsFixtureObserve"));
        queue = reinterpret_cast<decltype(queue)>(GetProcAddress(module, "MmsSettingsFixtureQueue"));
        direct = reinterpret_cast<decltype(direct)>(GetProcAddress(module, "MmsSettingsFixtureDirect"));
        CHECK(observe && queue && direct);
    }
    void Run(Manager& manager, SettingsFixtureRequest& request) const {
        CHECK(queue(&request) == MMS_OK);
        CHECK(!request.completed);
        manager.Drain();
        CHECK(request.completed);
    }
    MmsResult Merge(Manager& manager, std::string_view baseline, std::string_view patch) const {
        SettingsFixtureRequest request{};
        request.op = SettingsFixtureRequest::Op::Merge;
        request.expected = TextView(baseline);
        request.patch = TextView(patch);
        Run(manager, request);
        return request.result;
    }
    std::string Read(Manager& manager) const {
        SettingsFixtureRequest measure{};
        Run(manager, measure);
        CHECK(measure.result == MMS_OK && measure.bytes > 1 && measure.bytes <= MMS_SETTINGS_MAX_JSON_BYTES);
        SettingsFixtureRequest request{};
        std::string buffer(static_cast<size_t>(measure.bytes) + 1, '#');
        request.buffer = buffer.data();
        request.capacity = measure.bytes - 1;
        Run(manager, request);
        CHECK(request.result == MMS_E_MEMORY && request.bytes == measure.bytes);
        CHECK(std::all_of(buffer.begin(), buffer.end(), [](char c) { return c == '#'; }));
        request = {};
        request.buffer = buffer.data();
        request.capacity = measure.bytes;
        Run(manager, request);
        CHECK(request.result == MMS_OK && request.bytes == measure.bytes && buffer.back() == '#');
        buffer.resize(static_cast<size_t>(request.bytes));
        return buffer;
    }
};
void Test(const fs::path& root, const fs::path& source) {
    const auto plugins = root / "PLUGINS";
    const auto data = root / "plugin-data";
    const auto settings = root / "profile" / "settings.json";
    const auto project = root / "project" / "fixture.pmm";
    const auto sidecar = fs::path(project.native() + mikumikustudio::pmm_paths::PluginSidecarSuffix);
    CHECK(fs::create_directories(settings.parent_path()));
    CHECK(fs::create_directories(project.parent_path()));
    Write(settings, R"({"schemaVersion":1,"futureRoot":{"keep":true},"sections":{)"
        R"("pluginUi":{"sessionSentinel":["keep",77]},"hostFixture":{"value":42},)"
        R"("pluginSettings":{"org.example.settings-other":{"onlyOther":13}}}})");
    Write(project, "synthetic PMM sentinel");
    Write(sidecar, "synthetic plugin sidecar sentinel");
    Package(plugins / "owner", source, owner, "settings-owner.dll", true);
    Package(plugins / "other", source, other, "settings-other.dll", false);
    const auto ownerDll = plugins / "owner" / "settings-owner.dll";
    const auto otherDll = plugins / "other" / "settings-other.dll";
    const auto initial = ReadFile(settings);
    {
        Manager manager(data, LogMessage);
        manager.Load(plugins);
        const auto status = FindStatus(manager, owner);
        CHECK(status.stage == "failed");
        CHECK(status.error.find("required service unavailable: mms.settings result=" +
            std::to_string(MMS_E_NOT_FOUND)) != std::string::npos);
        CHECK(!GetModuleHandleW(ownerDll.c_str()));
        CHECK(FindStatus(manager, other).stage == "started");
        CHECK(Plugin(otherDll).observe()->query == MMS_E_NOT_FOUND);
        CHECK(manager.Stop());
        CHECK(ReadFile(settings) == initial);
    }
    {
        Manager manager(data, LogMessage, {}, false, {}, false, {}, settings);
        manager.Load(plugins);
        CHECK(FindStatus(manager, owner).stage == "started");
        CHECK(FindStatus(manager, other).stage == "started");
        const Plugin plugin(ownerDll), optedOut(otherDll);
        CHECK(plugin.observe()->query == MMS_OK && optedOut.observe()->query == MMS_E_NOT_FOUND);
        CHECK(plugin.observe()->createRead == MMS_E_NOT_FOUND);
        CHECK(plugin.observe()->startMerge == MMS_E_BUSY && plugin.observe()->eventRead == MMS_E_BUSY);
        // Dispatch queued plugin-startup registry notifications before exercising settings.
        manager.Drain();
        const auto ownerEvents = plugin.observe()->unexpectedEvents;
        const auto optedOutEvents = optedOut.observe()->unexpectedEvents;
        const auto prefsRevision = manager.Ui().PreferencesRevision();
        CHECK(!manager.Storage().Prepared() && manager.Storage().Records().empty());
        CHECK(ReadFile(settings) == initial);

        char untouched = '#';
        SettingsFixtureRequest absent{};
        absent.buffer = &untouched;
        absent.capacity = 1;
        absent.bytes = 73;
        plugin.Run(manager, absent);
        CHECK(absent.result == MMS_E_NOT_FOUND && absent.bytes == 73 && untouched == '#');
        CHECK(optedOut.queue(&absent) == MMS_E_NOT_FOUND);
        CHECK(plugin.Merge(manager, {}, R"({"view":{"future":true,"mode":"light"},"count":7})") == MMS_OK);
        const auto first = Json::Parse(plugin.Read(manager));
        CHECK(first.At("view").At("future").text == "true" && first.At("count").UInt() == 7);
        CHECK(first.At("view").At("mode").String() == "light");
        CHECK(!first.object.count("onlyOther") && !first.object.count("outsideTask"));
        const auto beforeConflict = ReadFile(settings);
        SettingsFixtureRequest offThread{};
        offThread.bytes = 74;
        MmsResult offThreadResult = MMS_OK;
        std::thread worker([&] { offThreadResult = plugin.direct(&offThread); });
        worker.join();
        CHECK(offThreadResult == MMS_E_THREAD && offThread.bytes == 74);

        const auto lockPath = settings.wstring() + L".lock";
        HANDLE held = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED overlap{};
        CHECK(held != INVALID_HANDLE_VALUE && LockFileEx(held, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap));
        SettingsFixtureRequest blocked{};
        blocked.bytes = 75;
        plugin.Run(manager, blocked);
        const auto blockedWrite = plugin.Merge(manager, R"({"count":7,"view":{"mode":"light","future":true}})", R"({"count":9})");
        UnlockFileEx(held, 0, 1, 0, &overlap);
        CloseHandle(held);
        CHECK(blocked.result == MMS_E_BUSY && blocked.bytes == 75 && blockedWrite == MMS_E_BUSY);
        CHECK(ReadFile(settings) == beforeConflict);
        CHECK(plugin.Merge(manager, {}, R"({"count":9})") == MMS_E_CONFLICT);
        CHECK(ReadFile(settings) == beforeConflict);
        CHECK(plugin.Merge(manager, R"({ "count":7, "view":{"mode":"light","future":true} })",
            R"({"view":{"mode":"dark"}})") == MMS_OK);
        CHECK(plugin.observe()->unexpectedEvents == ownerEvents &&
            optedOut.observe()->unexpectedEvents == optedOutEvents);
        CHECK(manager.Ui().PreferencesRevision() == prefsRevision);
        CHECK(!manager.Storage().Prepared() && manager.Storage().Records().empty());
        SettingsFixtureRequest cancelled{};
        cancelled.op = SettingsFixtureRequest::Op::Merge;
        cancelled.patch = TextView(R"({"cancelled":true})");
        CHECK(plugin.queue(&cancelled) == MMS_OK);
        CHECK(manager.Stop());
        CHECK(cancelled.completed && cancelled.result == MMS_E_NOT_READY);
        CHECK(!GetModuleHandleW(ownerDll.c_str()) && !GetModuleHandleW(otherDll.c_str()));
    }
    {
        // On restart the same second package opts in, proving the settings
        // table actually selects each client's own entry for both readers.
        WriteManifest(plugins / "other", other, "settings-other.dll", true);
        Manager manager(data, LogMessage, {}, false, {}, false, {}, settings);
        manager.Load(plugins);
        CHECK(FindStatus(manager, owner).stage == "started" && FindStatus(manager, other).stage == "started");
        const Plugin plugin(ownerDll), second(otherDll);
        CHECK(plugin.observe()->createRead == MMS_OK && second.observe()->createRead == MMS_OK);
        manager.Drain();
        const auto ownerEvents = plugin.observe()->unexpectedEvents;
        const auto secondEvents = second.observe()->unexpectedEvents;
        const auto loaded = Json::Parse(plugin.Read(manager));
        CHECK(loaded.At("view").At("mode").String() == "dark");
        CHECK(loaded.At("view").At("future").text == "true" && loaded.At("count").UInt() == 7);
        CHECK(!loaded.object.count("onlyOther"));
        const auto otherValue = Json::Parse(second.Read(manager));
        CHECK(otherValue.object.size() == 1 && otherValue.At("onlyOther").UInt() == 13);
        const auto ownerBaseline = plugin.Read(manager);
        CHECK(second.Merge(manager, R"({"onlyOther":13})", R"({"secondWrite":9})") == MMS_OK);
        CHECK(plugin.Merge(manager, ownerBaseline, R"({"count":8})") == MMS_OK);
        CHECK(Json::Parse(second.Read(manager)).At("secondWrite").UInt() == 9 &&
            !Json::Parse(second.Read(manager)).object.count("count"));
        CHECK(plugin.observe()->unexpectedEvents == ownerEvents &&
            second.observe()->unexpectedEvents == secondEvents);
        CHECK(manager.Stop());
    }

    const auto final = Json::Parse(ReadFile(settings));
    const auto& sections = final.At("sections");
    CHECK(sections.At("hostFixture").At("value").UInt() == 42);
    CHECK(sections.At("pluginUi").At("sessionSentinel").Items().at(0).String() == "keep");
    CHECK(sections.At("pluginUi").At("sessionSentinel").Items().at(1).UInt() == 77);
    CHECK(final.At("futureRoot").At("keep").text == "true");
    CHECK(sections.At("pluginSettings").object.size() == 2);
    CHECK(ReadFile(project) == "synthetic PMM sentinel");
    CHECK(ReadFile(sidecar) == "synthetic plugin sidecar sentinel");
    CHECK(std::distance(fs::directory_iterator(project.parent_path()), fs::directory_iterator()) == 2);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: settings_manager_tests <output-parent> <fixture.dll>\n");
        return 2;
    }
    try {
        const auto source = fs::absolute(argv[2]);
        CHECK(fs::is_regular_file(source));
        const TestRoot root(argv[1]);
        Test(root.path, source);
        std::printf("settings manager PASS: %u checks\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "settings manager FAIL: %s\n", e.what());
        return 1;
    }
}
