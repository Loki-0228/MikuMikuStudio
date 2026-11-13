#include "manager.hpp"
#include <mms/render_image.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
static size_t Count(const std::vector<std::string>& logs, const char* value) {
    return size_t(std::count_if(logs.begin(), logs.end(), [&](const std::string& s) { return s.find(value) != std::string::npos; }));
}
static void WriteManifest(const fs::path& path, const std::string& mode, const std::string& filename,
                          const std::string& dependency = {}) {
    std::ofstream out(path, std::ios::binary);
    out << "{\"schemaVersion\":1,\"id\":\"org.example.image-" << mode
        << "\",\"name\":\"CPU image lifecycle\",\"version\":\"1.0.0\","
        "\"kind\":\"native\",\"abi\":{\"major\":1,\"minMinor\":0},"
        "\"entry\":\"" << filename << "\",\"architecture\":\"x64\","
        "\"minWindows\":\"6.1.7601\",\"capabilities\":[\"render.image\"],"
        "\"requiresServices\":[{\"id\":\"mms.render.image\",\"major\":1,\"minMinor\":0}],"
        "\"dependencies\":[";
    if (!dependency.empty()) out << "{\"id\":\"" << dependency
        << "\",\"minVersion\":\"1.0.0\",\"maxVersionExclusive\":\"2.0.0\"}";
    out << "],\"nativeDependencies\":[],\"reloadPolicy\":\"restart\"}";
    out.close();
    CHECK(bool(out));
}
static void CheckRetired(const MmsRenderImageApiV1& api, MmsHandle image) {
    const unsigned char pixels[4] = {1, 2, 3, 4};
    const MmsRenderImageDescV1 desc{Header<MmsRenderImageDescV1>(), 1, 1,
        MMS_IMAGE_BGRA8, MMS_IMAGE_COLOR_SRGB, MMS_IMAGE_ALPHA_STRAIGHT, 0, 4};
    MmsHandle out = 12345;
    CHECK(api.createCpu(api.context, &desc, {pixels, sizeof(pixels)}, &out) == MMS_E_NOT_READY);
    CHECK(out == 12345);
    CHECK(api.retain(api.context, image, &out) == MMS_E_NOT_READY);
    CHECK(out == 12345);
    MmsRenderImageInfoV1 info;
    std::memset(&info, 0x5a, sizeof(info));
    const auto oldInfo = info;
    CHECK(api.describe(api.context, image, &info) == MMS_E_NOT_READY);
    CHECK(std::memcmp(&info, &oldInfo, sizeof(info)) == 0);
    MmsRenderImageMapV1 map;
    std::memset(&map, 0x5a, sizeof(map));
    const auto oldMap = map;
    CHECK(api.mapRead(api.context, image, &map) == MMS_E_NOT_READY);
    CHECK(std::memcmp(&map, &oldMap, sizeof(map)) == 0);
    MmsRenderImageCapsV1 caps;
    std::memset(&caps, 0x5a, sizeof(caps));
    const auto oldCaps = caps;
    CHECK(api.getCapabilities(api.context, &caps) == MMS_E_NOT_READY);
    CHECK(std::memcmp(&caps, &oldCaps, sizeof(caps)) == 0);
    CHECK(api.release(api.context, image) == MMS_E_NOT_FOUND);
}
static void WorkerOperations(const MmsRenderImageApiV1& api) {
    struct Results { MmsResult create = MMS_E_FAILED, map = MMS_E_FAILED, release = MMS_E_FAILED, unmap = MMS_E_FAILED; bool bytes = false; } result;
    std::thread worker([&] {
        unsigned char pixel[4] = {19, 29, 39, 49};
        const MmsRenderImageDescV1 desc{Header<MmsRenderImageDescV1>(), 1, 1,
            MMS_IMAGE_RGBA8, MMS_IMAGE_COLOR_SRGB, MMS_IMAGE_ALPHA_STRAIGHT, 0, 4};
        MmsHandle image = 0;
        result.create = api.createCpu(api.context, &desc, {pixel, sizeof(pixel)}, &image);
        if (result.create != MMS_OK) return;
        MmsRenderImageMapV1 mapped{};
        result.map = api.mapRead(api.context, image, &mapped);
        result.release = api.release(api.context, image);
        if (result.map == MMS_OK) {
            result.bytes = mapped.byteCount == 4 && std::memcmp(mapped.pixels, pixel, sizeof(pixel)) == 0;
            result.unmap = api.unmap(api.context, mapped.mapping);
        }
    });
    worker.join();
    CHECK(result.create == MMS_OK);
    CHECK(result.map == MMS_OK);
    CHECK(result.release == MMS_OK);
    CHECK(result.bytes);
    CHECK(result.unmap == MMS_OK);
}
static void RunCase(const fs::path& root, const fs::path& fixture, const std::string& mode) {
    const auto base = root / mode;
    const auto package = base / "PLUGINS" / "test";
    CHECK(fs::create_directories(package));
    // Distinct filenames also prevent the loader's equal-hash DLL reuse from
    // sharing fixture globals between cases.
    const std::string filename = "image-" + mode + ".dll";
    const auto dll = package / filename;
    CHECK(fs::copy_file(fixture, dll));
    WriteManifest(package / "plugin.json", mode, filename);
    std::vector<std::string> logs;
    Manager manager(base / "data", [&](const std::string& text) { logs.push_back(text); std::cout << mode << ": " << text << '\n'; });
    manager.Load(package.parent_path());
    CHECK(Count(logs, "IMAGE_NEGOTIATION_PASS") == 1);
    CHECK(Count(logs, "IMAGE_PREPARED") == 1);
    const auto statuses = manager.Status();
    CHECK(statuses.size() == 1);
    const bool failedCreate = mode == "failcreate";
    CHECK(statuses[0].stage == (failedCreate ? "stopping" : "started"));
    const auto module = GetModuleHandleW(dll.c_str());
    CHECK(module != nullptr);
    using Get = MmsResult(MMS_CALL*)(MmsRenderImageApiV1*, MmsHandle*, MmsRenderImageMapV1*);
    const auto get = reinterpret_cast<Get>(GetProcAddress(module, "MmsImageTestGet"));
    CHECK(get != nullptr);
    MmsRenderImageApiV1 api{};
    MmsHandle image = 0;
    MmsRenderImageMapV1 map{};
    CHECK(get(&api, &image, &map) == MMS_OK);
    CHECK(image != 0);
    if (mode == "nomap") {
        CHECK(map.mapping == 0);
        WorkerOperations(api);
        CHECK(manager.Stop());
        CHECK(Count(logs, "IMAGE_STOP_OK") == 1);
        CHECK(Count(logs, "IMAGE_DESTROY") == 1);
        CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
        return;
    }
    CHECK(map.mapping != 0);
    const unsigned char expected[8] = {17, 34, 51, 255, 68, 85, 102, 127};
    CHECK(map.byteCount == sizeof(expected));
    CHECK(map.rowStride == 8);
    CHECK(std::memcmp(map.pixels, expected, sizeof(expected)) == 0);
    if (!failedCreate) {
        WorkerOperations(api);
        MmsRenderImageInfoV1 info{};
        CHECK(api.describe(api.context, image, &info) == MMS_OK);
        CHECK(info.deviceGeneration == 0);
        manager.Render().DeviceLost(10);
        manager.Render().DeviceRestored(11);
        CHECK(api.describe(api.context, image, &info) == MMS_OK);
        CHECK(info.deviceGeneration == 0);
    }
    CHECK(!manager.Stop());
    CHECK(GetModuleHandleW(dll.c_str()) != nullptr);
    CHECK(Count(logs, "IMAGE_DESTROY") == 0);
    CHECK(Count(logs, "IMAGE_STOP_OK") == (failedCreate ? 0u : 1u));
    CheckRetired(api, image);
    CHECK(std::memcmp(map.pixels, expected, sizeof(expected)) == 0);
    MmsResult unmap = MMS_E_FAILED;
    std::thread releaser([&] { unmap = api.unmap(api.context, map.mapping); });
    releaser.join();
    CHECK(unmap == MMS_OK);
    CHECK(api.unmap(api.context, map.mapping) == MMS_E_NOT_FOUND);
    CHECK(manager.Stop());
    CHECK(Count(logs, "IMAGE_DESTROY") == (failedCreate ? 0u : 1u));
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    CHECK(manager.Stop());
    std::cout << mode << " PASS\n";
}
static void DependencyCase(const fs::path& root, const fs::path& fixture) {
    const auto base = root / "dependency";
    const auto plugins = base / "PLUGINS";
    const auto basePackage = plugins / "base";
    const auto dependentPackage = plugins / "dependent";
    CHECK(fs::create_directories(basePackage));
    CHECK(fs::create_directories(dependentPackage));
    const auto baseDll = basePackage / "image-dependency-base.dll";
    const auto dependentDll = dependentPackage / "image-dependency-dependent.dll";
    CHECK(fs::copy_file(fixture, baseDll));
    CHECK(fs::copy_file(fixture, dependentDll));
    WriteManifest(basePackage / "plugin.json", "dependency-base", "image-dependency-base.dll");
    WriteManifest(dependentPackage / "plugin.json", "dependency-dependent", "image-dependency-dependent.dll", "org.example.image-dependency-base");
    std::vector<std::string> logs;
    Manager manager(base / "data", [&](const std::string& text) { logs.push_back(text); std::cout << "dependency: " << text << '\n'; });
    manager.Load(plugins);
    CHECK(Count(logs, "IMAGE_NEGOTIATION_PASS") == 2);
    CHECK(Count(logs, "IMAGE_PREPARED") == 2);
    const auto statuses = manager.Status();
    CHECK(statuses.size() == 2);
    CHECK(std::all_of(statuses.begin(), statuses.end(), [](const auto& item) { return item.stage == "started"; }));
    const auto baseModule = GetModuleHandleW(baseDll.c_str());
    const auto dependentModule = GetModuleHandleW(dependentDll.c_str());
    CHECK(baseModule != nullptr && dependentModule != nullptr && baseModule != dependentModule);
    using Get = MmsResult(MMS_CALL*)(MmsRenderImageApiV1*, MmsHandle*, MmsRenderImageMapV1*);
    using Stops = uint32_t(MMS_CALL*)();
    const auto baseGet = reinterpret_cast<Get>(GetProcAddress(baseModule, "MmsImageTestGet"));
    const auto dependentGet = reinterpret_cast<Get>(GetProcAddress(dependentModule, "MmsImageTestGet"));
    const auto baseStops = reinterpret_cast<Stops>(GetProcAddress(baseModule, "MmsImageTestStopCount"));
    const auto dependentStops = reinterpret_cast<Stops>(GetProcAddress(dependentModule, "MmsImageTestStopCount"));
    CHECK(baseGet != nullptr && dependentGet != nullptr && baseStops != nullptr && dependentStops != nullptr);
    MmsRenderImageApiV1 baseApi{}, dependentApi{};
    MmsRenderImageMapV1 baseMap{}, dependentMap{};
    MmsHandle baseImage = 0, dependentImage = 0;
    CHECK(baseGet(&baseApi, &baseImage, &baseMap) == MMS_OK);
    CHECK(dependentGet(&dependentApi, &dependentImage, &dependentMap) == MMS_OK);
    CHECK(baseImage != 0 && dependentImage != 0 && baseMap.mapping != 0 && dependentMap.mapping != 0);
    CHECK(baseStops() == 0 && dependentStops() == 0);
    CHECK(!manager.Stop());
    // The dependent still owns its mapping, so dependency protection skips
    // base's StopOne. All clients must nevertheless have been retired first.
    CHECK(baseStops() == 0);
    CHECK(dependentStops() == 1);
    CHECK(Count(logs, "IMAGE_DESTROY") == 0);
    CHECK(GetModuleHandleW(baseDll.c_str()) != nullptr);
    CHECK(GetModuleHandleW(dependentDll.c_str()) != nullptr);
    CheckRetired(baseApi, baseImage);
    CheckRetired(dependentApi, dependentImage);
    const unsigned char expected[8] = {17, 34, 51, 255, 68, 85, 102, 127};
    CHECK(baseMap.byteCount == sizeof(expected) && dependentMap.byteCount == sizeof(expected));
    CHECK(std::memcmp(baseMap.pixels, expected, sizeof(expected)) == 0);
    CHECK(std::memcmp(dependentMap.pixels, expected, sizeof(expected)) == 0);
    MmsResult baseUnmap = MMS_E_FAILED, dependentUnmap = MMS_E_FAILED;
    std::thread releaser([&] {
        baseUnmap = baseApi.unmap(baseApi.context, baseMap.mapping);
        dependentUnmap = dependentApi.unmap(dependentApi.context, dependentMap.mapping);
    });
    releaser.join();
    CHECK(baseUnmap == MMS_OK && dependentUnmap == MMS_OK);
    CHECK(manager.Stop());
    CHECK(Count(logs, "IMAGE_DESTROY") == 2);
    CHECK(Count(logs, "IMAGE_STOP_OK") == 2);
    CHECK(GetModuleHandleW(baseDll.c_str()) == nullptr);
    CHECK(GetModuleHandleW(dependentDll.c_str()) == nullptr);
    std::cout << "dependency PASS\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: image_lifecycle_tests <output-parent> <fixture.dll>\n"); return 2; }
    try {
        const auto root = fs::absolute(argv[1]) / ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        CHECK(!fs::exists(root));
        CHECK(fs::create_directories(root));
        const auto fixture = fs::absolute(argv[2]);
        CHECK(fs::is_regular_file(fixture));
        RunCase(root, fixture, "mapped");
        RunCase(root, fixture, "nomap");
        RunCase(root, fixture, "failcreate");
        DependencyCase(root, fixture);
        std::cout << "CPU image lifecycle PASS: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
