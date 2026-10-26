// Exercise render retirement through the real Manager and a loaded DLL. The
// lifecycle fixture deliberately leaves its render callbacks/lease registered.
#include "manager.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)

static void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    if (!file) throw std::runtime_error("fixture write failed");
}
static std::string FixtureManifest(const std::string& id) {
    return "{\"schemaVersion\":1,\"id\":\"" + id +
        "\",\"name\":\"render retirement test\",\"version\":\"1.0.0\","
        "\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},"
        "\"entry\":\"lifecycle.dll\",\"architecture\":\"x64\","
        "\"minWindows\":\"6.1.7601\",\"capabilities\":[\"ui.command\"],"
        "\"requiresServices\":[{\"id\":\"mms.ui\",\"major\":1,\"minMinor\":0}],"
        "\"dependencies\":[],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
}
static size_t Count(const std::vector<std::string>& logs, const char* marker) {
    return size_t(std::count_if(logs.begin(), logs.end(), [&](const auto& text) {
        return text.find(marker) != std::string::npos;
    }));
}
static void Notify(RenderService& render) {
    render.FrameBegin();
    render.FramePresented(640, 480, 21);
    render.FrameEnd();
}
static void CheckRetired(Manager& manager, const std::vector<std::string>& logs) {
    const auto frames = Count(logs, "FRAME_CALLBACK frame=");
    const auto begins = Count(logs, "FRAME_PHASE_BEGIN");
    const auto ends = Count(logs, "FRAME_PHASE_END");
    const auto releases = Count(logs, "RENDER_LEASE_RELEASED");
    Notify(manager.Render());
    manager.Render().DeviceLost(2);
    manager.Render().DeviceRestored(3);
    Notify(manager.Render());
    CHECK(Count(logs, "FRAME_CALLBACK frame=") == frames);
    CHECK(Count(logs, "FRAME_PHASE_BEGIN") == begins);
    CHECK(Count(logs, "FRAME_PHASE_END") == ends);
    CHECK(Count(logs, "RENDER_LEASE_RELEASED") == releases);
}
static void LifecycleCase(const fs::path& root, const fs::path& fixture,
                          const std::string& mode) {
    const auto base = root / mode;
    const auto package = base / "PLUGINS" / "test";
    CHECK(fs::create_directories(package));
    const auto dll = package / "lifecycle.dll";
    CHECK(fs::copy_file(fixture, dll));
    const std::string id = "org.example.render-" + mode;
    Write(package / "plugin.json", FixtureManifest(id));
    std::vector<std::string> logs;
    Manager manager(base / "data", [&](const std::string& text) {
        logs.push_back(text);
        std::cout << mode << ": " << text << '\n';
    });
    manager.Load(package.parent_path());
    CHECK(Count(logs, "NEGOTIATION_PASS") == 1);
    CHECK(Count(logs, "FRAME_CALLBACK_REGISTERED") == 1);
    CHECK(Count(logs, "FRAME_PHASE_REGISTERED") == 1);
    CHECK(Count(logs, "RENDER_LEASE_ACQUIRED") == 1);
    const auto state = manager.Status();
    CHECK(state.size() == 1 && state.front().id == id);
    if (mode == "failstart") {
        // Start registers render state before the fixture returns failure.
        CHECK(state.front().stage == "failed");
        CHECK(state.front().error.find("start failed") != std::string::npos);
        CHECK(Count(logs, "DESTROY") == 1);
        CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
        CheckRetired(manager, logs);
        CHECK(manager.Stop());
    } else {
        CHECK(state.front().stage == "started");
        CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
        Notify(manager.Render());
        CHECK(Count(logs, "FRAME_CALLBACK frame=") == 1);
        CHECK(Count(logs, "FRAME_PHASE_BEGIN") == 1);
        CHECK(Count(logs, "FRAME_PHASE_END") == 1);
        if (mode == "busy") {
            CHECK(!manager.Stop());
            CHECK(Count(logs, "STOP_BUSY") == 1);
            CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
            CHECK(Count(logs, "DESTROY") == 0);
            CheckRetired(manager, logs);
            CHECK(!manager.Stop());
            CHECK(Count(logs, "STOP_BUSY") == 2);
            CheckRetired(manager, logs);
        }
        CHECK(manager.Stop());
        CHECK(Count(logs, "DESTROY") == 1);
        CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
        CheckRetired(manager, logs);
    }
    CHECK(Count(logs, "FRAME_CALLBACK_FAIL") == 0);
    CHECK(Count(logs, "FRAME_PHASE_FAIL") == 0);
    std::cout << mode << " PASS\n";
}
struct ReentryProbe {
    Manager* manager = nullptr;
    fs::path dll;
    unsigned calls = 0;
    bool sawCallback = false;
    bool stopSucceeded = true;
    bool dllStillLoaded = false;
};
static void AttemptStop(ReentryProbe& probe) {
    // Never assert/throw inside a host callback: the render boundary catches
    // exceptions, so an assertion there could falsely pass the outer test.
    ++probe.calls;
    probe.sawCallback = probe.manager->InCallback();
    probe.stopSucceeded = probe.manager->Stop();
    probe.dllStillLoaded = GetModuleHandleW(probe.dll.c_str()) != nullptr;
}
static void MMS_CALL StopFromFrame(void* context, const MmsRenderInfoV1*) {
    AttemptStop(*static_cast<ReentryProbe*>(context));
}
static void MMS_CALL StopFromPhase(void* context, const MmsRenderInfoV1*, uint32_t) {
    AttemptStop(*static_cast<ReentryProbe*>(context));
}
static void MMS_CALL StopFromRelease(void* context) {
    AttemptStop(*static_cast<ReentryProbe*>(context));
}
static void ReentryCase(const fs::path& root, const fs::path& fixture,
                        const std::string& mode) {
    const auto base = root / ("reentry-" + mode);
    const auto package = base / "PLUGINS" / "test";
    CHECK(fs::create_directories(package));
    const auto dll = package / "lifecycle.dll";
    CHECK(fs::copy_file(fixture, dll));
    Write(package / "plugin.json", FixtureManifest("org.example.render-reentry-" + mode));
    std::vector<std::string> logs;
    // These objects outlive Manager even if an assertion fails.
    RenderService::Client observer;
    observer.id = "org.example.render-host-observer";
    ReentryProbe probe;
    probe.dll = dll;
    Manager manager(base / "data", [&](const std::string& text) {
        logs.push_back(text);
        std::cout << "reentry-" << mode << ": " << text << '\n';
    });
    probe.manager = &manager;
    manager.Load(package.parent_path());
    CHECK(Count(logs, "FRAME_CALLBACK_REGISTERED") == 1);
    CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
    const auto api = manager.Render().Api12(observer);
    if (mode == "frame") {
        CHECK(api.setFrameCallback(api.context, StopFromFrame, &probe) == MMS_OK);
        manager.Render().FramePresented(640, 480, 21);
    } else if (mode == "phase" || mode == "phase-end") {
        CHECK(api.setPhaseCallback(api.context, StopFromPhase, &probe) == MMS_OK);
        if (mode == "phase") manager.Render().FrameBegin();
        else manager.Render().FrameEnd();
    } else {
        MmsRenderLeaseDescV1 desc{};
        desc.h = Header<MmsRenderLeaseDescV1>();
        desc.name = Text(observer.id);
        desc.release = StopFromRelease;
        desc.context = &probe;
        desc.flags = MMS_RENDER_LEASE_DEVICE;
        MmsHandle lease = 0;
        CHECK(api.acquireLease(api.context, &desc, &lease) == MMS_OK && lease);
        manager.Render().DeviceLost(2);
    }
    CHECK(probe.calls == 1);
    CHECK(probe.sawCallback);
    CHECK(!probe.stopSucceeded);
    CHECK(probe.dllStillLoaded);
    CHECK(!manager.InCallback());
    CHECK(Count(logs, "DESTROY") == 0);
    CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
    manager.Render().DropClient(observer.id);
    CHECK(manager.Stop());
    CHECK(Count(logs, "DESTROY") == 1);
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    std::cout << "reentry-" << mode << " PASS\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: render_lifecycle_tests <output-parent> <lifecycle.dll>\n");
        return 2;
    }
    try {
        // A fresh run directory is mandatory. Keep all prior evidence intact.
        const auto root = fs::absolute(argv[1]) /
            ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root));
        CHECK(fs::create_directories(root));
        CHECK(SetEnvironmentVariableW(L"MMS_ABI_PROBE_LOG", nullptr));
        const auto fixture = fs::absolute(argv[2]);
        CHECK(fs::is_regular_file(fixture));
        LifecycleCase(root, fixture, "normal");
        LifecycleCase(root, fixture, "failstart");
        LifecycleCase(root, fixture, "busy");
        ReentryCase(root, fixture, "frame");
        ReentryCase(root, fixture, "phase");
        ReentryCase(root, fixture, "phase-end");
        ReentryCase(root, fixture, "release");
        std::cout << "render lifecycle PASS checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
