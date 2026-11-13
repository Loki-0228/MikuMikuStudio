#include "loader_win32.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace mikumikustudio::plugins;

namespace {
constexpr wchar_t attachMarker[] = L"MMS_TEST_LEGACY_OBSERVER_ATTACHED";

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Attached() {
    return GetEnvironmentVariableW(attachMarker, nullptr, 0) != 0;
}

Manifest Entry(const fs::path& file) {
    Manifest manifest;
    manifest.kind = Manifest::Kind::LegacyMmdPlugin;
    manifest.package = file.parent_path();
    manifest.entry = file;
    manifest.id = "test.legacy.observer";
    manifest.legacyEntrySha256 = Sha256(file);
    return manifest;
}

void CheckRejected(NativeLoader& loader, const fs::path& provider, const fs::path& entry) {
    bool rejected = false;
    try {
        loader.Load(Entry(entry), {}, provider);
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "legacy MMDPlugin missing direct export destroy3";
    }
    Check(rejected, "invalid destroy3 export was not rejected by PE preflight");
    Check(!Attached(), "DllMain ran before PE rejection");
    Check(!GetModuleHandleW(entry.filename().c_str()), "rejected entry remains loaded");
    Check(!GetModuleHandleW(provider.filename().c_str()), "provider was left loaded by rejection");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 5, "expected provider, direct, missing and forwarded DLLs");
        SetEnvironmentVariableW(attachMarker, nullptr);
        const auto provider = fs::absolute(argv[1]);
        const auto direct = fs::absolute(argv[2]);
        NativeLoader loader;
        CheckRejected(loader, provider, fs::absolute(argv[3]));
        CheckRejected(loader, provider, fs::absolute(argv[4]));
        {
            const auto loaded = loader.Load(Entry(direct), {}, provider);
            Check(loaded.entry && Attached(), "direct exports did not load or fixture marker failed");
            Check(GetProcAddress(loaded.entry, "destroy3") != nullptr, "direct destroy3 is missing");
        }
        Check(!GetModuleHandleW(direct.filename().c_str()), "direct entry was not unloaded");
        Check(!GetModuleHandleW(provider.filename().c_str()), "provider was not unloaded");
        std::cout << "PASS legacy observer export preflight\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
