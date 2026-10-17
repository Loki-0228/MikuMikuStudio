#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "mikudancestudio/system_settings.hpp"
#include "../src/plugins/json.hpp"
#include <cstdio>
#include <atomic>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
namespace settings = mikudancestudio::system_settings;
using settings::Result;
using mikudancestudio::plugins::Json;
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
int Worker(const fs::path& path, const std::string& section) {
    settings::Store store(path);
    for (unsigned i = 0; i < 24; ++i)
        if (store.WriteSection(section, "{\"value\":" + std::to_string(i) + "}") != Result::Ok) return 2;
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
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--worker")
        return Worker(fs::path(argv[2]), mikudancestudio::plugins::Utf8(argv[3]));
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
    for (const std::string& bad : std::vector<std::string>{
        "{", "{\"schemaVersion\":2,\"sections\":{}}", "{\"schemaVersion\":1.0,\"sections\":{}}",
        "{\"schemaVersion\":1,\"sections\":[]}", "{\"schemaVersion\":1,\"sections\":{\"ui\":[]}}",
        "{\"schemaVersion\":1,\"sections\":{},\"sections\":{}}", std::string(1024 * 1024 + 1, ' ')}) {
        Put(path, bad); output = "unchanged";
        Check(store.ReadSection("ui", output) == Result::Invalid && output == "unchanged", "invalid read preserves output");
        Check(store.WriteSection("ui", "{}") == Result::Invalid && Bytes(path) == bad,
            "corrupt oversized or unsupported file is never overwritten");
    }
    Put(path, "{\"schemaVersion\":1,\"sections\":{}}");
    const auto beforeLarge = Bytes(path);
    Check(store.WriteSection("ui", "{\"large\":\"" + std::string(1024 * 1024, 'x') + "\"}") == Result::Invalid &&
        Bytes(path) == beforeLarge, "oversized update leaves original unchanged");
    Check(store.WriteSection("first", "{\"large\":\"" + std::string(600000, 'x') + "\"}") == Result::Ok, "large valid section");
    const auto beforeCombined = Bytes(path);
    Check(store.WriteSection("second", "{\"large\":\"" + std::string(600000, 'x') + "\"}") == Result::Invalid &&
        Bytes(path) == beforeCombined, "combined file size limit preserves original");

    // Holding a readable handle without delete-sharing permits Load but forces
    // the atomic replacement to fail, without modifying the old settings.
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open replacement fault fixture");
    Check(store.WriteSection("failed", "{\"value\":1}") == Result::Io && Bytes(path) == beforeCombined,
        "atomic replacement failure preserves old contents");
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
        for (unsigned i = 0; i < 24; ++i)
            if (store.WriteSection("shared", "{\"field" + std::to_string(worker * 24 + i) + "\":true}") != Result::Ok) ++failures;
    });
    for (auto& thread : threads) thread.join();
    Check(failures == 0 && store.ReadSection("shared", output) == Result::Ok && Json::Parse(output).object.size() == 96,
        "threads preserve merged same-section fields");
    std::printf("PASS system settings checks=%u artifacts=%ls\n", checks, root.c_str());
    return 0;
}
