// Guard the actual source string against backwards reads, then verify the
// existing UserFile model fallback still resolves a normal missing path.
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
    ++checks; if (!ok) { ++failures; std::fprintf(stderr,"FAIL: %s\n",text); }
}
}
int main() {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, system.dwPageSize * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(memory != nullptr,"allocate guarded source string");
    if (!memory) return 1;
    DWORD previous = 0;
    Check(VirtualProtect(memory,system.dwPageSize,PAGE_NOACCESS,&previous) != FALSE,"guard page before source");
    auto* source = reinterpret_cast<wchar_t*>(memory + system.dwPageSize);
    PathResolutionWorkspace paths{};
    for (const auto* input : {L"",L"p",L"pm",L"pmd",L"wav",L"vac",L"file.pmd"}) {
        std::wcscpy(source,input);
        Check(ResolveUserFilePath(paths,source)[0] == L'\0',"missing short/basename path returns empty without a backwards read");
    }
    VirtualFree(memory,0,MEM_RELEASE);
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / (L"MMS-path-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(directory / L"UserFile" / L"Model");
    const fs::path model = directory / L"UserFile" / L"Model" / L"fixture.pmd";
    { std::ofstream stream(model); stream << "fixture"; }
    std::wcscpy(paths.executableDirectory,(directory.wstring() + L"\\").c_str());
    Check(std::wstring(ResolveUserFilePath(paths,L"missing\\fixture.pmd")) == model.wstring(),
          "missing normal model path keeps the existing UserFile Model fallback");
    fs::remove(model); fs::remove(directory / L"UserFile" / L"Model");
    fs::remove(directory / L"UserFile"); fs::remove(directory);
    std::printf("path_resolve_tests: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
