#include "loader_win32.hpp"
#include <iostream>
using namespace mikumikustudio::plugins;
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) throw std::runtime_error("expected original camera DLL and provider");
        Manifest manifest;
        manifest.kind = Manifest::Kind::LegacyMmdPlugin;
        manifest.legacyProfile = "first_person_camera_v4";
        manifest.major = 4; manifest.minWindows = {6, 1, 0};
        manifest.id = "test.original-camera";
        manifest.entry = fs::absolute(argv[1]); manifest.package = manifest.entry.parent_path();
        manifest.legacyEntrySha256 = Sha256(manifest.entry);
        if (manifest.legacyEntrySha256 != "d3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef")
            throw std::runtime_error("expected unmodified FirstPersonCamera v0.01 x64");
        NativeLoader loader;
        auto loaded = loader.Load(manifest, {}, fs::absolute(argv[2]));
        if (!loaded.entry) throw std::runtime_error("original camera rejected before its factory");
        std::cout << "PASS original DLL preflight only; input/memory tests are separate\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
