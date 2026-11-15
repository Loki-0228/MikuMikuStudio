#include "loader_win32.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace mikumikustudio::plugins;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 4, "expected provider, GDI+ fixture and private fixture");
        Manifest entry;
        entry.id = "test.sxs"; entry.kind = Manifest::Kind::LegacyMmdPlugin; entry.major = 3;
        entry.entry = fs::absolute(argv[2]); entry.package = entry.entry.parent_path();
        entry.legacyEntrySha256 = Sha256(entry.entry);
        for (bool fallback : {false, true}) {
            NativeLoader loader(fallback);
            auto code = loader.Load(entry, {}, fs::absolute(argv[1]));
            auto version = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(code.entry, "version"));
            Check(version && version() == 3, "system-default SxS fixture did not execute");
        }
        const auto root = fs::temp_directory_path() / (L"mms-system-import-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        const auto manifestPath = root / L"sxs.manifest";
        std::ofstream(manifestPath) << R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><assemblyIdentity name="MmsSxsTest" version="1.0.0.0" processorArchitecture="amd64" type="win32"/><dependency><dependentAssembly><assemblyIdentity type="win32" name="Microsoft.Windows.GdiPlus" version="1.1.0.0" processorArchitecture="amd64" publicKeyToken="6595b64144ccf1df" language="*"/></dependentAssembly></dependency></assembly>)";
        ACTCTXW settings{sizeof(settings)};
        settings.lpSource = manifestPath.c_str();
        const auto context = CreateActCtxW(&settings);
        Check(context != INVALID_HANDLE_VALUE, "cannot create Windows GDI+ activation context");
        ULONG_PTR cookie = 0;
        Check(ActivateActCtx(context, &cookie), "cannot activate Windows GDI+");
        auto gdiplus = LoadLibraryW(L"gdiplus.dll");
        Check(gdiplus != nullptr, "cannot load Windows GDI+");
        wchar_t loadedPath[32768]{};
        GetModuleFileNameW(gdiplus, loadedPath, 32768);
        Check(Lower(loadedPath).find(L"\\winsxs\\") != std::wstring::npos, "test did not select a real Windows SxS DLL");
        std::wcout << L"Windows binding: " << loadedPath << L'\n';
        for (bool fallback : {false, true}) {
            NativeLoader loader(fallback);
            auto code = loader.Load(entry, {}, fs::absolute(argv[1]));
            auto version = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(code.entry, "version"));
            Check(version && version() == 3, "Windows SxS fixture did not execute");
        }
        FreeLibrary(gdiplus);
        DeactivateActCtx(0, cookie);
        ReleaseActCtx(context);
        Check(!GetModuleHandleW(L"gdiplus.dll"), "SxS references leaked");
        const auto privatePath = root / L"gdiplus.dll";
        fs::copy_file(argv[3], privatePath);
        const auto privateManifest = root / L"private.manifest";
        std::ofstream(privateManifest) << R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><assemblyIdentity name="PrivateGdiSubstitute" version="1.0.0.0" processorArchitecture="amd64" type="win32"/><file name="gdiplus.dll"/></assembly>)";
        settings.lpSource = privateManifest.c_str();
        const auto privateContext = CreateActCtxW(&settings);
        Check(privateContext != INVALID_HANDLE_VALUE && ActivateActCtx(privateContext, &cookie), "private redirect fixture activation");
        auto substitute = LoadLibraryExW(privatePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(substitute != nullptr, "cannot preload private fixture");
        GetModuleFileNameW(substitute, loadedPath, 32768);
        Check(SamePath(loadedPath, privatePath), "private preload was redirected back to Windows");
        bool rejected = false;
        try { NativeLoader loader; auto code = loader.Load(entry, {}, fs::absolute(argv[1])); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "private same-name DLL was accepted as a system assembly");
        FreeLibrary(substitute);
        DeactivateActCtx(0, cookie);
        ReleaseActCtx(privateContext);
        fs::remove_all(root);
        std::cout << "PASS Windows SxS binding and private substitute rejection\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
