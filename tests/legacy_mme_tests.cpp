// Legacy MMEffect/MMHack module detection: a renamed copy and a copy in a
// subdirectory must be recognized even though GetModuleHandleW("MMEffect.dll")
// cannot see them. This test only checks the detection seam; real MME plugins
// remain external acceptance.
#include "mikumikustudio/legacy_mme.hpp"
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
static unsigned checks = 0;
static unsigned failures = 0;
static void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}
static fs::path CopyMarker(const fs::path& source, const fs::path& target) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    Check(!ec, "copy the inert MMEffect marker");
    return target;
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3) { std::fprintf(stderr, "usage: legacy_mme_tests <scratch> <MMEffect marker.dll>\n"); return 2; }
    const fs::path root = fs::absolute(argv[1]);
    const fs::path marker = fs::absolute(argv[2]);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    Check(!mikumikustudio::LegacyMmeLoaded(), "no MMEffect module before loading");
    Check(!mikumikustudio::LegacyMmeHackLoaded(), "no MMHack module before loading");
    // The module scan is throttled to avoid walking the process on every frame.
    Sleep(600);
    const auto renamed = CopyMarker(marker, root / L"renamed" / L"MMEffectJP.dll");
    HMODULE module = LoadLibraryW(renamed.c_str());
    Check(module != nullptr, "load the renamed MMEffect marker");
    Check(mikumikustudio::LegacyMmeLoaded(), "renamed MMEffect module is detected");
    Check(mikumikustudio::LegacyMmeModule() == module, "renamed module handle is reported");
    if (module) FreeLibrary(module);
    module = nullptr;
    Check(!mikumikustudio::LegacyMmeLoaded(), "unloaded renamed module is not detected");
    Sleep(600);
    const auto subdir = CopyMarker(marker, root / L"plugin" / L"mme" / L"MMEffectSub.dll");
    module = LoadLibraryW(subdir.c_str());
    Check(module != nullptr, "load the subdirectory MMEffect marker");
    Check(mikumikustudio::LegacyMmeLoaded(), "subdirectory MMEffect module is detected");
    if (module) FreeLibrary(module);
    module = nullptr;
    Check(!mikumikustudio::LegacyMmeLoaded(), "unloaded subdirectory module is not detected");
    Sleep(600);
    const auto unrelated = CopyMarker(marker, root / L"unrelated" / L"NotMMEffect.dll");
    module = LoadLibraryW(unrelated.c_str());
    Check(module != nullptr, "load the unrelated marker");
    Check(!mikumikustudio::LegacyMmeLoaded(), "unrelated module name is ignored");
    if (module) FreeLibrary(module);
    std::printf("legacy mme detection %s checks=%u failures=%u\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
