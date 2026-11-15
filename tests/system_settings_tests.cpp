#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#include "mikumikustudio/system_settings.hpp"
#include "../src/plugins/json.hpp"
#include <cstdio>
#include <atomic>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
namespace settings = mikumikustudio::system_settings;
using settings::PluginResult;
using settings::Result;
using mikumikustudio::plugins::Json;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Put(const fs::path& path, const std::string& value) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), value.size());
    Check(bool(file), "write fixture");
}
void PluginSettings(const fs::path& root) {
    const auto path = root / L"data" / L"settings.json";
    settings::Store store(path);
    std::string output = "unchanged";
    Check(store.ReadPlugin("demo.a", output) == PluginResult::Missing && output == "unchanged" &&
        !fs::exists(path.parent_path()), "missing plugin read changes no output or directory");
    Check(store.CompareMergePlugin("demo.a", {}, "{}") == PluginResult::Ok && !fs::exists(path),
        "empty patch cannot create a settings document");
    const auto seed = R"({ "schemaVersion":1,"futureRoot":{"keep":true},"sections":{
        "ui":{"future":2},"pluginSettings":{"demo.a.b":{"foreign":3},"opaquePeer":99}}})";
    Put(path, seed);
    Check(store.CompareMergePlugin("demo.a", {}, R"({"d":{"x":1,"unknown":8},"n":2})") == PluginResult::Ok,
        "initialize only the manifest ID slot");
    Check(store.ReadPlugin("demo.a", output) == PluginResult::Ok &&
        output == R"({"d":{"unknown":8,"x":1},"n":2})", "canonical read owns only the selected plugin object");
    const auto baseline = output;
    Check(store.WriteSection("ui", R"({"current":4})") == Result::Ok &&
        store.CompareMergePlugin("demo.a.b", R"({"foreign":3})", R"({"newPeer":7})") == PluginResult::Ok,
        "ordinary and peer updates share the settings store");
    Check(store.CompareMergePlugin("demo.a", baseline, R"({"d":{"x":4}})") == PluginResult::Ok,
        "merge succeeds after peer and ordinary settings updates");
    const auto afterMerge = Bytes(path);
    const auto afterTime = fs::last_write_time(path);
    const std::string equivalent = R"({ "n" : 2, "d" : { "x" : 4, "unknown" : 8 } })";
    Check(store.CompareMergePlugin("demo.a", equivalent, R"({"d":{"x":4}})") == PluginResult::Ok &&
        Bytes(path) == afterMerge && fs::last_write_time(path) == afterTime, "no-op leaves bytes and write time intact");
    Check(store.CompareMergePlugin("demo.a", baseline, R"({"n":5})") == PluginResult::Conflict &&
        Bytes(path) == afterMerge, "stale baseline never publishes");
    Check(store.CompareMergePlugin("demo.a", equivalent, R"({"n":5,"d":null})") == PluginResult::Invalid &&
        Bytes(path) == afterMerge, "invalid object replacement never publishes a valid sibling");
    const auto document = Json::Parse(afterMerge);
    const auto& sections = document.At("sections");
    Check(document.At("futureRoot").At("keep").text == "true" && sections.At("ui").At("current").UInt() == 4 &&
        sections.At("pluginSettings").At("demo.a").At("d").At("unknown").UInt() == 8 &&
        sections.At("pluginSettings").At("demo.a.b").At("newPeer").UInt() == 7 &&
        sections.At("pluginSettings").At("opaquePeer").UInt() == 99,
        "unknown sibling, peer, section and root values survive");
    Check(store.WriteSection("pluginSettings", "{}") == Result::Invalid &&
        store.CompareExchangeSection("pluginSettings", nullptr, "{}") == Result::Invalid &&
        store.WriteSections(R"({"ui":{"current":0},"pluginSettings":{}})") == Result::Invalid &&
        Bytes(path) == afterMerge, "generic Store writes cannot bypass reserved registry");

    // Load can still read this handle's file, but replacement is prohibited.
    auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "test replacement failure handle");
    Check(store.CompareMergePlugin("demo.a", equivalent, R"({"n":5})") == PluginResult::Io &&
        Bytes(path) == afterMerge, "replacement failure keeps original settings bytes");
    CloseHandle(file);
    for (const auto& entry : fs::directory_iterator(path.parent_path()))
        Check(entry.path().filename().wstring().find(L".tmp-") == std::wstring::npos,
            "replacement failure leaves no temporary file");

    const auto lockPath = path.wstring() + L".lock";
    file = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    OVERLAPPED overlap{};
    Check(file != INVALID_HANDLE_VALUE && LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap),
        "hold existing settings file lock");
    const auto start = GetTickCount64();
    output = "unchanged";
    const auto read = store.ReadPlugin("demo.a", output);
    const auto write = store.CompareMergePlugin("demo.a", equivalent, R"({"n":5})");
    const auto elapsed = GetTickCount64() - start;
    UnlockFileEx(file, 0, 1, 0, &overlap);
    CloseHandle(file);
    Check(read == PluginResult::Busy && write == PluginResult::Busy && elapsed < 1500 &&
        output == "unchanged" && Bytes(path) == afterMerge, "nonblocking plugin paths use the same settings lock");

    // Protect the existing DACL without changing its entries. A staging file
    // inherits normally and cannot stand in for this manually protected file.
    auto nativePath = path.wstring();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Check(GetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor) == ERROR_SUCCESS && dacl && descriptor,
        "read fixture security metadata");
    Check(SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS, "use protected DACL on fixture");
    Check(store.CompareMergePlugin("demo.a", equivalent, R"({"n":5})") == PluginResult::Io &&
        Bytes(path) == afterMerge, "new publisher cannot widen an existing explicit access policy");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    PSECURITY_DESCRIPTOR protectedDescriptor = nullptr;
    Check(GetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &protectedDescriptor) == ERROR_SUCCESS &&
        GetSecurityDescriptorControl(protectedDescriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED), "original protected DACL still in place");
    LocalFree(protectedDescriptor);
    LocalFree(descriptor);

    for (const auto* bad : {R"({"schemaVersion":3,"sections":{}})",
        R"({"schemaVersion":1,"sections":{"pluginSettings":[]}})",
        R"({"schemaVersion":1,"sections":{"pluginSettings":{"demo.a":null}}})"}) {
        Put(path, bad);
        Check(store.ReadPlugin("demo.a", output) == PluginResult::Corrupt && output == "unchanged" &&
            store.CompareMergePlugin("demo.a", {}, R"({"n":5})") == PluginResult::Corrupt &&
            Bytes(path) == bad, "corrupt plugin store is untouched");
    }
}
int Worker(const fs::path& path, const std::string& section) {
    settings::Store store(path);
    for (unsigned i = 0; i < 24; ++i) {
        const auto object = "{\"value\":" + std::to_string(i) + "}";
        const auto result = i % 2 ? store.WriteSections("{\"" + section + "\":" + object +
            ",\"batchWorkers\":{\"" + section + "\":" + std::to_string(i) + "}}") : store.WriteSection(section, object);
        if (result != Result::Ok) return 2;
    }
    return 0;
}
void Processes(const fs::path& path) {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    std::vector<PROCESS_INFORMATION> children;
    for (unsigned i = 0; i < 4; ++i) {
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --worker \"" + path.wstring() + L"\" worker" + std::to_wstring(i);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "launch concurrent settings writer");
        children.push_back(process);
    }
    for (auto& process : children) {
        const auto waited = WaitForSingleObject(process.hProcess, 30000);
        if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 3);
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Check(waited == WAIT_OBJECT_0 && code == 0, "concurrent process writer completed");
    }
    settings::Store store(path);
    for (unsigned i = 0; i < 4; ++i) {
        std::string value;
        Check(store.ReadSection("worker" + std::to_string(i), value) == Result::Ok &&
            Json::Parse(value).At("value").UInt() == 23, "cross-process sections preserved");
        Check(store.ReadSection("batchWorkers", value) == Result::Ok &&
            Json::Parse(value).At(("worker" + std::to_string(i)).c_str()).UInt() == 23,
            "concurrent process batch writes preserve other writers");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--worker")
        return Worker(fs::path(argv[2]), mikumikustudio::plugins::Utf8(argv[3]));
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto root = fs::path(executable).parent_path() /
        (L"settings-store-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const auto path = root / L"data" / L"settings.json";
    settings::Store store(path);
    std::string output = "unchanged";
    Check(store.ReadSection("ui", output) == Result::Missing && output == "unchanged", "missing read preserves output");
    Check(!fs::exists(path.parent_path()), "missing read does not create data folder");
    Check(store.WriteSection("ui", "{\"中文\":\"测试\\n文本\",\"nested\":{\"future\":7},\"enabled\":true}") == Result::Ok,
        "create valid UTF-8 settings");
    Check(store.ReadSection("ui", output) == Result::Ok && Json::Parse(output).At("enabled").text == "true", "read section");
    Check(store.WriteSection("ui", "{\"nested\":{\"known\":2}}") == Result::Ok, "merge section update");
    Check(store.ReadSection("ui", output) == Result::Ok && Json::Parse(output).At("nested").At("future").UInt() == 7,
        "recursive unknown field preservation");
    Check(store.WriteSection("render", "{\"cpu\":true}") == Result::Ok, "write unrelated section");
    Check(store.WriteSection("pluginUi", "{\"layout\":1}") == Result::Ok &&
        store.ReadSection("pluginUi", output) == Result::Ok, "mixed-case section names read and write");
    const auto mixedCaseExpected = output;
    Check(store.CompareExchangeSection("pluginUi", &mixedCaseExpected, "{\"layout\":2}") == Result::Ok &&
        store.ReadSection("pluginUi", output) == Result::Ok && Json::Parse(output).At("layout").UInt() == 2,
        "mixed-case section supports CAS");
    const auto original = Bytes(path);
    FILETIME marker{}; marker.dwHighDateTime = 0x01db0000;
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &marker), "set no-op timestamp marker");
    CloseHandle(file);
    const auto timestamp = fs::last_write_time(path);
    Check(store.WriteSection("render", "{ \"cpu\" : true }") == Result::Ok && Bytes(path) == original &&
        fs::last_write_time(path) == timestamp, "same settings do not rewrite file");

    Check(store.CompareExchangeSection("new", nullptr, "{}") == Result::Ok, "CAS inserts missing section");
    Check(store.CompareExchangeSection("new", nullptr, "{}") == Result::Conflict, "CAS rejects existing section");
    Check(store.ReadSection("render", output) == Result::Ok, "read CAS baseline");
    const auto expected = output;
    Check(store.WriteSection("unrelated", "{\"future\":true}") == Result::Ok, "concurrent unrelated change");
    Check(store.CompareExchangeSection("render", &expected, "{\"cpu\":false}") == Result::Ok, "CAS merges over other sections");
    const auto beforeConflict = Bytes(path);
    Check(store.CompareExchangeSection("render", &expected, "{\"cpu\":true}") == Result::Conflict &&
        Bytes(path) == beforeConflict, "CAS rejects stale same-section state without writes");
    const std::string noncanonical = "{ \"cpu\" : false }";
    Check(store.CompareExchangeSection("render", &noncanonical, "{\"new\":2}") == Result::Ok,
        "CAS ignores insignificant formatting");
    Check(store.ReadSection("render", output) == Result::Ok && Json::Parse(output).object.size() == 1 &&
        Json::Parse(output).object.count("cpu") == 0, "CAS replaces owned section and can remove keys");
    Check(store.ReadSection("unrelated", output) == Result::Ok && Json::Parse(output).At("future").text == "true",
        "CAS preserves unrelated section");
    const std::string invalidExpected = "[]";
    Check(store.CompareExchangeSection("render", &invalidExpected, "{}") == Result::Invalid, "CAS validates expected type");
    Check(settings::Store("relative.json").WriteSection("ui", "{}") == Result::Invalid, "reject relative store path");
    Check(store.WriteSection("bad/name", "{}") == Result::Invalid && store.WriteSection("ui", "[]") == Result::Invalid,
        "validate section name and object type");

    Put(path, "{\"schemaVersion\":1,\"futureRoot\":{\"n\":1e2},\"sections\":{\"ui\":{\"future\":[1,2,null]}}}");
    Check(store.WriteSection("ui", "{\"current\":3}") == Result::Ok, "accept unknown root metadata");
    auto rootValue = Json::Parse(Bytes(path));
    Check(rootValue.At("futureRoot").At("n").text == "1e2" && rootValue.At("sections").At("ui").At("future").array.size() == 3,
        "unknown root and section data survive save");
    Check(store.WriteSections(R"({"history":{"future":{"keep":7},"steps":12},"resources":{"future":{"keep":9},"backgroundPictureSourcesMiB":128}})") == Result::Ok,
        "create history and resources in one update");
    Check(store.WriteSections(R"({"history":{"future":{"new":8},"steps":24},"resources":{"backgroundPictureSourcesMiB":64}})") == Result::Ok,
        "merge two sections together");
    rootValue = Json::Parse(Bytes(path));
    Check(rootValue.At("sections").At("history").At("steps").UInt() == 24 &&
        rootValue.At("sections").At("history").At("future").At("keep").UInt() == 7 &&
        rootValue.At("sections").At("history").At("future").At("new").UInt() == 8 &&
        rootValue.At("sections").At("resources").At("backgroundPictureSourcesMiB").UInt() == 64 &&
        rootValue.At("sections").At("resources").At("future").At("keep").UInt() == 9 &&
        rootValue.At("futureRoot").At("n").text == "1e2" && rootValue.At("sections").At("ui").At("current").UInt() == 3,
        "batch preserves nested unknown members, other sections and root metadata");
    const auto beforeBatch = Bytes(path);
    const auto batchTime = fs::last_write_time(path);
    Check(store.WriteSections(R"({"resources":{"backgroundPictureSourcesMiB":64},"history":{"steps":24}})") == Result::Ok &&
        Bytes(path) == beforeBatch && fs::last_write_time(path) == batchTime, "batch no-op preserves file and timestamp");
    for (const auto* invalid : {"{}", "[]", R"({"history":{"steps":1},"resources":[]})",
        R"({"history":{"steps":1},"resources":null})", R"({"history":{"steps":1},"z/bad":{}})",
        R"({"history":{"steps":1},"history":{}})"})
        Check(store.WriteSections(invalid) == Result::Invalid && Bytes(path) == beforeBatch,
            "invalid batch never commits its valid first section");
    Check(settings::Store("relative.json").WriteSections(R"({"history":{}})") == Result::Invalid,
        "batch rejects relative store path");
    for (const std::string& bad : std::vector<std::string>{
        "{", "{\"schemaVersion\":2,\"sections\":{}}", "{\"schemaVersion\":1.0,\"sections\":{}}",
        "{\"schemaVersion\":1,\"sections\":[]}", "{\"schemaVersion\":1,\"sections\":{\"ui\":[]}}",
        "{\"schemaVersion\":1,\"sections\":{},\"sections\":{}}", std::string(1024 * 1024 + 1, ' ')}) {
        Put(path, bad); output = "unchanged";
        Check(store.ReadSection("ui", output) == Result::Invalid && output == "unchanged", "invalid read preserves output");
        Check(store.WriteSection("ui", "{}") == Result::Invalid && Bytes(path) == bad,
            "corrupt oversized or unsupported file is never overwritten");
        Check(store.WriteSections(R"({"history":{"steps":1},"resources":{"backgroundPictureSourcesMiB":16}})") == Result::Invalid &&
            Bytes(path) == bad, "batch never overwrites corrupt oversized or unsupported settings");
    }
    Put(path, "{\"schemaVersion\":1,\"sections\":{}}");
    const auto beforeLarge = Bytes(path);
    Check(store.WriteSection("ui", "{\"large\":\"" + std::string(1024 * 1024, 'x') + "\"}") == Result::Invalid &&
        Bytes(path) == beforeLarge, "oversized update leaves original unchanged");
    Check(store.WriteSections("{\"history\":{\"steps\":1},\"resources\":{\"large\":\"" + std::string(1024 * 1024, 'x') + "\"}}") == Result::Invalid &&
        Bytes(path) == beforeLarge, "oversized batch leaves every section unchanged");
    std::string deep = "{}";
    for (unsigned i = 0; i < 47; ++i) deep = "{\"n\":" + deep + "}";
    const auto deepBatch = "{\"deep\":" + deep + "}";
    Check(Json::Parse(deepBatch).kind == Json::Object && store.WriteSections(deepBatch) == Result::Invalid &&
        Bytes(path) == beforeLarge, "batch validates combined document depth before publication");
    std::string many = "{\"items\":[0";
    for (unsigned i = 1; i < 17000; ++i) many += ",0";
    many += "]}";
    Check(store.WriteSection("nodes", many) == Result::Ok, "prepare individually valid node budget fixture");
    const auto beforeNodes = Bytes(path);
    Check(store.WriteSections("{\"history\":{\"steps\":1},\"newNodes\":" + many + "}") == Result::Invalid &&
        Bytes(path) == beforeNodes, "batch rejects combined node budget without partial history update");
    Put(path, "{\"schemaVersion\":1,\"sections\":{}}");
    Check(store.WriteSection("first", "{\"large\":\"" + std::string(600000, 'x') + "\"}") == Result::Ok, "large valid section");
    const auto beforeCombined = Bytes(path);
    Check(store.WriteSection("second", "{\"large\":\"" + std::string(600000, 'x') + "\"}") == Result::Invalid &&
        Bytes(path) == beforeCombined, "combined file size limit preserves original");
    Check(store.WriteSections("{\"history\":{\"steps\":1},\"second\":{\"large\":\"" + std::string(600000, 'x') + "\"}}") == Result::Invalid &&
        Bytes(path) == beforeCombined, "batch combined file size rejects all sections");

    // Holding a readable handle without delete-sharing permits Load but forces
    // the atomic replacement to fail, without modifying the old settings.
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open replacement fault fixture");
    Check(store.WriteSection("failed", "{\"value\":1}") == Result::Io && Bytes(path) == beforeCombined,
        "atomic replacement failure preserves old contents");
    Check(store.WriteSections(R"({"history":{"steps":1},"resources":{"backgroundPictureSourcesMiB":16}})") == Result::Io &&
        Bytes(path) == beforeCombined, "batch replacement failure preserves both sections");
    CloseHandle(file);
    unsigned temporaryCount = 0;
    for (const auto& entry : fs::directory_iterator(path.parent_path()))
        if (entry.path().filename().wstring().find(L".tmp-") != std::wstring::npos) ++temporaryCount;
    Check(temporaryCount == 0, "failed save removes own temporary file");

    const auto lockPath = path.wstring() + L".lock";
    file = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    OVERLAPPED overlap{};
    Check(file != INVALID_HANDLE_VALUE && LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap), "hold store lock");
    output = "unchanged";
    Check(store.ReadSection("ui", output) == Result::Busy && output == "unchanged", "bounded lock wait reports busy");
    UnlockFileEx(file, 0, 1, 0, &overlap); CloseHandle(file);
    Put(path, "{\"schemaVersion\":1,\"sections\":{}}");
    Processes(path);
    std::vector<std::thread> threads;
    std::atomic<unsigned> failures{0};
    for (unsigned worker = 0; worker < 4; ++worker) threads.emplace_back([&, worker] {
        for (unsigned i = 0; i < 24; ++i) {
            const auto object = "{\"field" + std::to_string(worker * 24 + i) + "\":true}";
            const auto result = worker % 2 ? store.WriteSections("{\"shared\":" + object + ",\"paired\":" + object + "}") :
                store.WriteSection("shared", object);
            if (result != Result::Ok) ++failures;
        }
    });
    for (auto& thread : threads) thread.join();
    Check(failures == 0 && store.ReadSection("shared", output) == Result::Ok && Json::Parse(output).object.size() == 96,
        "threads preserve merged same-section fields");
    Check(store.ReadSection("paired", output) == Result::Ok && Json::Parse(output).object.size() == 48,
        "mixed single and batch writers preserve all paired fields");
    PluginSettings(root / L"plugin-fixture");
    std::printf("PASS system settings checks=%u artifacts=%ls\n", checks, root.c_str());
    return 0;
}
