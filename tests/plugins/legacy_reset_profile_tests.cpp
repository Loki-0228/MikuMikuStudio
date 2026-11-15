#include "manifest.hpp"
#include <fstream>
#include <iostream>

using namespace mikumikustudio::plugins;
int main() {
    const auto root = fs::temp_directory_path() / (L"mmdfacade-reset-" + std::to_wstring(GetCurrentProcessId()));
    try {
        fs::create_directories(root);
        std::ofstream(root / "reset.dll") << "fixture";
        std::ofstream(root / "plugin.json") << R"({"schemaVersion":1,"id":"test.reset","name":"Reset","version":"1.0.0","kind":"legacy_mmdplugin","profile":"device_reset_v2","entry":"reset.dll","sha256":")"
            << std::string(64, '0') << R"(","architecture":"x64","minWindows":"6.1.0","reloadPolicy":"restart"})";
        const auto manifest = ReadManifest(root / "plugin.json", {});
        if (manifest.major != 2) throw std::runtime_error("reset profile did not select ABI 2");
        fs::remove_all(root);
        std::cout << "PASS device_reset_v2 selects ABI 2\n";
        return 0;
    } catch (const std::exception& e) {
        fs::remove_all(root);
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
