#include "manifest.hpp"
#include "json.hpp"

#include <iostream>
#include <fstream>

using namespace mikumikustudio::plugins;
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Installation {
    static unsigned serial;
    fs::path root = fs::temp_directory_path() / (L"mms-auto-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
    Installation() { Check(fs::create_directory(root), "isolated installation already exists"); }
    ~Installation() { std::error_code error; fs::remove_all(root, error); }
};
unsigned Installation::serial = 0;
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "expected original camera");
        Installation install;
        const auto plugins = install.root / L"PLUGINS";
        const auto folder = plugins / L"LEGACY" / L"group" / L"camera";
        fs::create_directories(folder);
        fs::copy_file(argv[1], folder / L"MMDFirstPersonCamera.dll");
        const auto discovered = Discover(plugins);
        Check(discovered.packages.size() == 1, "camera DLL without a manifest was not discovered");
        Check(discovered.packages.front().legacyProfile == "first_person_camera_v4", "wrong automatic camera profile");
        Check(!fs::exists(folder / L"plugin.json"), "discovery must not write manifests into user packages");
        Check(!GetModuleHandleW(L"MMDFirstPersonCamera.dll"), "discovery executed the plugin");
        Check(discovered.inspectedDlls == 1 && discovered.cachedDlls == 0, "cold recognition counters");
        auto warm = Discover(plugins);
        Check(warm.inspectedDlls == 0 && warm.cachedDlls == 1 && warm.packages.size() == 1, "persistent warm cache");
        const auto id = discovered.packages.front().id;
        std::ofstream(plugins / L"config.json") << "{\"schemaVersion\":1,\"maxDepth\":2,\"disabledPlugins\":[\"" << id << "\"]}";
        auto disabled = Discover(plugins);
        Check(Resolve(disabled, {}).empty() && disabled.cachedDlls == 1, "blacklist must apply without reinspection");
        std::ofstream(plugins / L"config.json") << "{\"schemaVersion\":1,\"maxDepth\":1,\"disabledPlugins\":[]}";
        Check(Discover(plugins).packages.empty(), "maxDepth must bound DLL discovery");
        fs::remove(plugins / L"config.json");
        Check(Discover(plugins).packages.size() == 1, "restore depth");
        std::ofstream(folder / L"plugin.json") << "{}";
        auto invalid = Discover(plugins);
        Check(invalid.packages.empty() && !invalid.diagnostics.empty() && invalid.inspectedDlls == 0,
            "invalid manifest must prevent automatic fallback");
        fs::remove(folder / L"plugin.json");
        Check(Discover(plugins).packages.size() == 1, "remove explicit manifest");
        std::ofstream(folder / L"dependency.dll") << "ordinary invalid binary";
        auto dependency = Discover(plugins);
        Check(dependency.packages.size() == 1 && dependency.inspectedDlls == 1 && dependency.cachedDlls == 1,
            "new dependency must not re-inspect unchanged camera");
        auto negative = Discover(plugins);
        Check(negative.inspectedDlls == 0 && negative.cachedDlls == 2, "cache negative results too");
        auto bytes = ReadFile(folder / L"MMDFirstPersonCamera.dll", 32 * 1024 * 1024);
        bytes.back() ^= 1; // Change identity without changing the export table.
        { std::ofstream output(folder / L"MMDFirstPersonCamera.dll", std::ios::binary); output.write(bytes.data(), bytes.size()); }
        auto changed = Discover(plugins);
        Check(changed.packages.empty() && changed.inspectedDlls == 1 && changed.cachedDlls == 1,
            "changed DLL must invalidate only its own record");
        Check(!changed.diagnostics.empty(), "unsupported ABI 4 must be reported");
        fs::copy_file(argv[1], folder / L"MMDFirstPersonCamera.dll", fs::copy_options::overwrite_existing);
        std::ofstream(install.root / L"data/plugins/legacy-discovery.json") << "broken cache";
        auto repaired = Discover(plugins);
        Check(repaired.packages.size() == 1 && repaired.inspectedDlls == 2, "corrupt cache must rebuild");
        fs::rename(folder, folder.parent_path() / L"自由分组");
        auto renamed = Discover(plugins);
        Check(renamed.packages.size() == 1 && renamed.packages.front().id == id, "stable ID across directory changes");
        fs::create_directory(plugins / L"ordinary");
        fs::copy_file(argv[1], plugins / L"ordinary/MMDFirstPersonCamera.dll");
        const auto bounded = Discover(plugins);
        Check(bounded.packages.size() == 1 && bounded.cachedDlls == 2 && bounded.inspectedDlls == 0,
            "raw DLL outside LEGACY must never be inspected or activated");
        std::cout << "PASS automatic legacy discovery, cache, invalidation and LEGACY boundary\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
