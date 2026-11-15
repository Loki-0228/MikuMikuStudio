// MAINTENANCE: Keep with the legacy observer PE and provider loading contract.
#include "loader_win32.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace mikumikustudio::plugins;

namespace {
constexpr wchar_t attachMarker[] = L"MMS_TEST_LEGACY_OBSERVER_ATTACHED";
const auto windowMarker = reinterpret_cast<HWND>(UINT_PTR{1});
constexpr int menuIdMarker = 12345;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Attached() {
    return GetEnvironmentVariableW(attachMarker, nullptr, 0) != 0;
}

Manifest Entry(const fs::path& file) {
    Manifest manifest;
    manifest.kind = Manifest::Kind::LegacyMmdPlugin;
    manifest.major = 3;
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
struct TemporaryInstall {
    const fs::path root = fs::absolute(fs::temp_directory_path()) /
        (L"mms-legacy-provider-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    TemporaryInstall() { Check(fs::create_directory(root), "temporary install already exists"); }
    ~TemporaryInstall() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

void CopyProvider(const fs::path& provider, const fs::path& destination) {
    fs::create_directories(destination.parent_path());
    fs::copy_file(provider, destination);
}

void CheckProvider(NativeLoader& loader, const fs::path& install, const Manifest& entry, const fs::path& expected) {
    const auto selected = LegacyMmdProviderFile(install);
    Check(SamePath(selected, expected), "unexpected legacy MMDPlugin provider selection");
    SetEnvironmentVariableW(attachMarker, nullptr);
    {
        const auto loaded = loader.Load(entry, {}, selected);
        Check(loaded.entry && Attached(), "provider layout did not load the observer");
        const auto provider = GetModuleHandleW(L"MMDPlugin.dll");
        Check(provider && std::any_of(loaded.modules.begin(), loaded.modules.end(), [&](const auto& module) {
            return module->handle == provider && SamePath(module->path, expected);
        }), "wrong provider was loaded");
        const auto getWindow = reinterpret_cast<HWND(__cdecl*)()>(GetProcAddress(provider, "getHWND"));
        const auto allocate = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(provider, "createWM_APP_ID"));
        Check(getWindow && getWindow() == windowMarker && allocate && allocate() == menuIdMarker,
              "provider exports did not bind to the renamed host");
    }
    Check(!GetModuleHandleW(L"MMDPlugin.dll"), "provider layout test left DLL loaded");
}

void CheckProviderLayouts(NativeLoader& loader, const fs::path& provider, const fs::path& direct) {
    Check(!GetModuleHandleW(L"MikuMikuDance.exe"), "layout test requires a differently named host");
    const TemporaryInstall install;
    const auto plugins = install.root / L"PLUGINS";
    const auto fallback = install.root / L"legacy-mmdplugin" / L"MMDPlugin.dll";
    const auto previousPreferred = plugins / L"legacy-mmdplugin" / L"MMDPlugin.dll";
    const auto preferred = plugins / L"LEGACY" / L"MMDPlugin.dll";
    const auto entry = Entry(direct);

    auto rejectedPath = [&](const fs::path& root) {
        try {
            LegacyMmdProviderFile(root);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CopyProvider(provider, install.root / L"MMDPlugin.dll");
    CopyProvider(provider, plugins / L"another-package" / L"MMDPlugin.dll");
    Check(rejectedPath(install.root), "unrecognized provider location was accepted");
    Check(rejectedPath(L"."), "relative installation was accepted");

    CopyProvider(provider, fallback);
    CheckProvider(loader, install.root, entry, fallback);
    CopyProvider(provider, previousPreferred);
    CheckProvider(loader, install.root, entry, previousPreferred);
    CopyProvider(provider, preferred);
    CheckProvider(loader, install.root, entry, preferred);
    fs::remove(fallback);
    CheckProvider(loader, install.root, entry, preferred);

    CopyProvider(provider, fallback);
    fs::remove(preferred);
    CheckProvider(loader, install.root, entry, previousPreferred);
    fs::remove(previousPreferred);
    CheckProvider(loader, install.root, entry, fallback); // Preferred directory exists, DLL absent.
    fs::create_directory(preferred);
    Check(rejectedPath(install.root), "provider directory was treated as absent");
    fs::remove_all(preferred.parent_path());
    {
        std::ofstream blocker(preferred.parent_path());
        Check(bool(blocker), "could not block provider parent with a file");
    }
    Check(rejectedPath(install.root), "non-directory provider parent was treated as absent");
    fs::remove(preferred.parent_path());

    fs::create_directory(preferred.parent_path());
    {
        std::ofstream invalid(preferred, std::ios::binary);
        const char bytes[128]{};
        invalid.write(bytes, sizeof bytes);
        Check(bool(invalid), "could not write invalid provider fixture");
    }
    const auto selected = LegacyMmdProviderFile(install.root);
    Check(SamePath(selected, preferred), "bad preferred DLL was treated as absent");
    SetEnvironmentVariableW(attachMarker, nullptr);
    bool rejected = false;
    try {
        loader.Load(entry, {}, selected);
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "invalid DOS header";
    }
    Check(rejected && !Attached() && !GetModuleHandleW(L"MMDPlugin.dll"),
          "bad preferred DLL did not fail before any provider or observer ran");
}
} // namespace

extern "C" __declspec(dllexport) HWND __cdecl MMSLegacyMmdGetWindow() { return windowMarker; }
extern "C" __declspec(dllexport) int __cdecl MMSLegacyMmdAllocateMenuId() { return menuIdMarker; }

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 5, "expected provider, direct, missing and forwarded DLLs");
        SetEnvironmentVariableW(attachMarker, nullptr);
        const auto provider = fs::absolute(argv[1]);
        const auto direct = fs::absolute(argv[2]);
        NativeLoader loader;
        {
            auto wrongAbi = Entry(direct);
            wrongAbi.major = 2;
            bool rejected = false;
            try { loader.Load(wrongAbi, {}, provider); }
            catch (const std::invalid_argument& error) {
                rejected = std::string_view(error.what()) == "legacy MMDPlugin missing direct export create2";
            }
            Check(rejected && !Attached(), "ABI 2 must not use ABI 3 factories or run DllMain");
        }
        CheckRejected(loader, provider, fs::absolute(argv[3]));
        CheckRejected(loader, provider, fs::absolute(argv[4]));
        {
            const auto loaded = loader.Load(Entry(direct), {}, provider);
            Check(loaded.entry && Attached(), "direct exports did not load or fixture marker failed");
            Check(GetProcAddress(loaded.entry, "destroy3") != nullptr, "direct destroy3 is missing");
        }
        Check(!GetModuleHandleW(direct.filename().c_str()), "direct entry was not unloaded");
        Check(!GetModuleHandleW(provider.filename().c_str()), "provider was not unloaded");
        CheckProviderLayouts(loader, provider, direct);
        std::cout << "PASS legacy observer export preflight\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
