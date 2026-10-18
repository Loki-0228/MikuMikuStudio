// Guard the source against backwards reads while exercising the production resolver.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include "mikudancestudio/path_workspace.hpp"
#include "mikudancestudio/ported_funcs.hpp"
using namespace mikudancestudio;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
}
int main() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / (L"MMS-path-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(directory);
    struct RestoreDirectory {
        fs::path previous = fs::current_path();
        ~RestoreDirectory() { std::error_code ignored; fs::current_path(previous, ignored); }
    } restore;
    fs::current_path(directory);
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(memory != nullptr, "allocate guarded source string");
    if (!memory) return 1;
    DWORD previous = 0;
    Check(VirtualProtect(memory, system.dwPageSize, PAGE_NOACCESS, &previous) != FALSE,
        "guard page before source");
    auto* source = reinterpret_cast<wchar_t*>(memory + system.dwPageSize);
    PathResolutionWorkspace paths{};
    for (const auto* input : {L"", L"p", L"pm", L"pmd", L"wav", L"vac", L"WAV", L"file.pmd"}) {
        std::wcscpy(source, input);
        Check(ResolveUserFilePath(paths, source)[0] == L'\0',
            "missing short/basename path returns empty without a backwards read");
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    for (const auto* folder : {L"Model", L"Wave", L"Accessory"})
        fs::create_directories(directory / L"UserFile" / folder);
    std::wcscpy(paths.executableDirectory, (directory.wstring() + L"\\").c_str());
    for (const auto* name : {L"fixture.pmd", L"fixture.wav", L"fixture.vac"}) {
        const auto* folder = std::wcscmp(name, L"fixture.pmd") == 0 ? L"Model" :
            std::wcscmp(name, L"fixture.wav") == 0 ? L"Wave" : L"Accessory";
        const fs::path file = directory / L"UserFile" / folder / name;
        { std::ofstream stream(file); stream << "fixture"; }
        Check(std::wstring(ResolveUserFilePath(paths, (std::wstring(L"missing\\") + name).c_str())) == file.wstring(),
            "missing normal path keeps existing UserFile fallback");
        Check(std::wstring(ResolveUserFilePath(paths, file.c_str())) == file.wstring(),
            "existing absolute file remains directly resolvable");
        fs::remove(file);
    }
    for (const auto* folder : {L"Model", L"Wave", L"Accessory"}) fs::remove(directory / L"UserFile" / folder);
    fs::remove(directory / L"UserFile"); fs::current_path(restore.previous); fs::remove(directory);
    std::printf("path_resolve_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
