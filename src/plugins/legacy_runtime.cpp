#include "mikumikustudio/legacy_mme_bootstrap.hpp"
#include "mikumikustudio/legacy_mme.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "legacy_image.hpp"
#include "loader_win32.hpp"
#include "../io/cache_files_crypto.hpp"
#include <array>
#include <fstream>

namespace mikumikustudio::plugins {
namespace {
using Bytes = std::vector<std::uint8_t>;
LegacyMmeStartupStatus status;
bool attempted = false, safeToContinue = true;
struct Handles {
    std::vector<HANDLE> values;
    ~Handles() { for (auto h : values) CloseHandle(h); }
    HANDLE Hold(HANDLE h) {
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("legacy file access error " + std::to_string(GetLastError()));
        try { values.push_back(h); } catch (...) { CloseHandle(h); throw; }
        return h;
    }
} retainedFiles;
void LogMessage(const std::string& message) noexcept {
    try {
        OutputDebugStringA(("MME bootstrap: " + message + "\n").c_str());
        // Diagnostic storage uses the same installation scope as the cache.
        if (!status.directory.empty()) {
            const auto file = status.directory.parent_path() / L"startup.log";
            HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION info{};
                if (GetFileInformationByHandle(h, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    DWORD written = 0; const auto text = message + '\n';
                    WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr);
                }
                CloseHandle(h);
            }
        }
    } catch (...) {}
}
void HoldDirectory(const fs::path& path, Handles& handles, bool create) {
    if (create && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw std::runtime_error("cannot create legacy cache directory");
    auto h = handles.Hold(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("legacy cache requires ordinary directories");
}
Bytes ReadImage(const fs::path& path, Handles& handles) {
    auto h = handles.Hold(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
        throw std::runtime_error("invalid legacy image file or size");
    Bytes bytes(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
    if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
        throw std::runtime_error("incomplete legacy image read");
    return bytes;
}
std::string CacheKey(const std::array<Bytes, 3>& images) {
    cache_files::crypto::Sha256 hash;
    if (!hash.Start()) throw std::runtime_error("legacy cache SHA-256 unavailable");
    const char version[] = "MMS legacy import adapter v1";
    if (!hash.Update(version, sizeof(version))) throw std::runtime_error("legacy cache hash failed");
    for (const auto& bytes : images) {
        const uint64_t count = bytes.size();
        if (!hash.Update(&count, sizeof(count)) || !hash.Update(bytes.data(), bytes.size()))
            throw std::runtime_error("legacy cache hash failed");
    }
    cache_files::Digest digest{};
    if (!hash.Finish(digest)) throw std::runtime_error("legacy cache hash failed");
    constexpr char hex[] = "0123456789abcdef"; std::string key;
    for (auto b : digest) { key += hex[b >> 4]; key += hex[b & 15]; }
    return key;
}
void Publish(const fs::path& path, const Bytes& bytes) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        auto temp = path; temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot stage legacy cache image");
        DWORD written = 0;
        const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(h);
        CloseHandle(h);
        const bool moved = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
        const DWORD error = GetLastError();
        if (!moved) {
            DeleteFileW(temp.c_str()); // Only this invocation's CREATE_NEW staging file.
            if (!ok || (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS))
                throw std::runtime_error("cannot publish legacy cache image");
        }
    }
    // Retain a read-only, non-delete-shared handle through process shutdown.
    // A cached DLL must equal the freshly adapted source, even on a cache hit.
    if (ReadImage(path, retainedFiles) != bytes) throw std::runtime_error("legacy cache image content differs");
}
void InitializeSettings(const Manifest& manifest, const fs::path& cache) {
    const Bytes identity(manifest.id.begin(), manifest.id.end());
    const std::string domain = "MME preferences identity v1";
    cache_files::crypto::Sha256 hash;
    cache_files::Digest digest{};
    if (!hash.Start() || !hash.Update(domain.data(), domain.size()) || !hash.Update(identity.data(), identity.size()) || !hash.Finish(digest))
        throw std::runtime_error("legacy preferences identity unavailable");
    constexpr char hex[] = "0123456789abcdef"; std::string key;
    for (auto value : digest) { key += hex[value >> 4]; key += hex[value & 15]; }
    auto directory = cache.parent_path() / L"settings";
    HoldDirectory(directory, retainedFiles, true);
    directory /= Wide(key); HoldDirectory(directory, retainedFiles, true);
    status.settingsFile = directory / L"MMEffect.ini";
    // Serialize writers across all cache generations for this plugin identity.
    auto lock = retainedFiles.Hold(CreateFileW((directory / L"preferences.lock").c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1)
        throw std::runtime_error("invalid legacy preferences lock");
    Publish(directory / L"plugin.id", identity);
    if (GetFileAttributesW(status.settingsFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Handles check;
        auto file = check.Hold(CreateFileW(status.settingsFile.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION existing{};
        if (!GetFileInformationByHandle(file, &existing) || existing.nNumberOfLinks != 1 ||
            (existing.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw std::runtime_error("invalid legacy preferences file");
        return;
    }
    // Older caches have no package identity. Never guess which user's settings
    // to adopt from a same-DLL cache shared by multiple possible package IDs.
    for (const auto& entry : fs::directory_iterator(cache.parent_path())) {
        const auto name = entry.path().filename().wstring();
        if (name.size() != 64 || name.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) continue;
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("legacy preferences cache is a reparse point");
        if (GetFileAttributesW((entry.path() / L"MMEffect.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("unattributed old MMEffect.ini: copy the chosen preferences into " + Utf8(status.settingsFile.native()));
    }
    const auto original = manifest.entry.parent_path() / L"MMEffect.ini";
    Bytes bytes;
    Handles inputs;
    if (fs::exists(original)) {
        PackageFile(manifest.package, Utf8(original.lexically_relative(manifest.package).native()));
        if (fs::file_size(original) > 1024 * 1024) throw std::runtime_error("legacy settings file exceeds limit");
        if (fs::file_size(original)) bytes = ReadImage(original, inputs);
        if (bytes.size() > 1024 * 1024) throw std::runtime_error("legacy settings file exceeds limit");
    }
    if (bytes.empty()) {
        const std::string defaults = "[System]\nSkipValidation=true\nEMMAutoSave=true\n";
        bytes.assign(defaults.begin(), defaults.end());
    }
    auto temp = directory / (L"initial.ini.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()));
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot stage legacy preferences");
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), status.settingsFile.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); throw std::runtime_error("cannot publish legacy preferences");
    }
}
fs::path ModulePath(HMODULE module) {
    std::vector<wchar_t> name(32768);
    const DWORD n = GetModuleFileNameW(module, name.data(), DWORD(name.size()));
    if (!n || n >= name.size()) throw std::runtime_error("cannot identify loaded legacy module");
    return fs::path(std::wstring(name.data(), n));
}
void Pin(HMODULE module) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned)) throw std::runtime_error("cannot retain legacy hook module");
}
// Preload imports from their explicit paths even on unpatched Windows 7. The
// legacy MMHack/MMEffect pair is intentionally cyclic; NativeLoader is for
// acyclic native C ABI packages and must not be used for this pair.
void PreloadSystemImports(const fs::path& directory) {
    std::vector<wchar_t> name(32768); const UINT n = GetSystemDirectoryW(name.data(), UINT(name.size()));
    if (!n || n >= name.size()) throw std::runtime_error("system directory unavailable");
    const fs::path system(std::wstring(name.data(), n));
    const UINT windowsLength = GetWindowsDirectoryW(name.data(), UINT(name.size()));
    if (!windowsLength || windowsLength >= name.size()) throw std::runtime_error("Windows directory unavailable");
    const auto sideBySide = Lower((fs::path(std::wstring(name.data(), windowsLength)) / L"WinSxS").native()) + L"\\";
    const auto systemModule = [&](HMODULE module, const fs::path& expected) {
        if (!module) return false;
        const auto actual = ModulePath(module);
        // Activation contexts may select a Windows-serviced GDI+ assembly.
        // Respect that binding; never accept an app-local substitute here.
        return SamePath(actual, expected) ||
            (Lower(actual.filename().native()) == Lower(expected.filename().native()) &&
             Lower(actual.native()).rfind(sideBySide, 0) == 0);
    };
    std::set<std::wstring> imports;
    for (const auto* file : {L"MMSAdapter.dll", L"MMHack.dll", L"MMEffect.dll"})
        for (const auto& imported : InspectPe(directory / file).imports) {
            const auto lower = Lower(Wide(imported));
            if (lower != L"mmsadapter.dll" && lower != L"mmhack.dll" && lower != L"mmeffect.dll") imports.insert(lower);
        }
    const bool safeSearch = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory") != nullptr;
    for (const auto& imported : imports) {
        const auto path = PackageFile(system, Utf8(imported));
        HMODULE module = GetModuleHandleW(imported.c_str());
        if (module && !systemModule(module, path)) throw std::runtime_error("legacy dependency " + Utf8(imported) + " already loaded from " + Utf8(ModulePath(module).native()) + "; expected " + Utf8(path.native()));
        if (!module) module = LoadLibraryExW(path.c_str(), nullptr, safeSearch ? LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!systemModule(module, path)) throw std::runtime_error("legacy system dependency load failed: " + Utf8(imported));
        Pin(module);
    }
}
}

const LegacyMmeStartupStatus& LegacyMmeStatus() noexcept { return status; }
bool BootstrapLegacyMme() noexcept {
    if (attempted) return safeToContinue;
    attempted = true;
    try {
        const auto install = local_data::ExecutableDirectory();
        if (install.empty()) throw std::runtime_error("installation directory unavailable");
        const auto discovered = Discover(install / L"PLUGINS");
        for (const auto& d : discovered.diagnostics) LogMessage(d);
        const auto order = Resolve(discovered, LogMessage);
        const Manifest* legacy = nullptr;
        for (auto index : order) if (discovered.packages[index].kind == Manifest::Kind::LegacyMme) {
            if (legacy) throw std::runtime_error("enable only one legacy MME package per process");
            legacy = &discovered.packages[index];
        }
        if (!legacy) return true;
        status.id = legacy->id;
        if (WindowsVersion() < legacy->minWindows) throw std::runtime_error("legacy package minimum Windows version is not met");
        if (LegacyMmeModule() || LegacyMmeHackModule() || GetModuleHandleW(L"MMSAdapter.dll"))
            throw std::runtime_error("legacy modules already loaded; remove the old startup proxy before using a package");
        Handles source;
        LegacyImagePatch hack;
        if (legacy->legacyHack.empty()) {
            // The built-in bridge resolves the current host directly; it has no
            // MikuMikuDance.exe import to rewrite. Validate its PE before loading.
            const auto bridge = PackageFile(install, "MMHack.dll");
            InspectPe(bridge);
            hack.image = ReadImage(bridge, source);
        } else {
            hack = RedirectLegacyHostImport(ReadImage(legacy->legacyHack, source));
        }
        auto effect = RedirectLegacyHostImport(ReadImage(legacy->entry, source));
        if (!hack.ok()) throw std::runtime_error("MMHack: " + std::string(LegacyImageErrorName(hack.error)) + " " + hack.message);
        if (!effect.ok()) throw std::runtime_error("MMEffect: " + std::string(LegacyImageErrorName(effect.error)) + " " + effect.message);
        std::array<Bytes, 3> images{std::move(hack.image), std::move(effect.image),
            ReadImage(PackageFile(install, "MMSAdapter.dll"), source)};
        auto directory = install;
        HoldDirectory(directory, retainedFiles, false);
        for (const auto& part : {std::wstring(L"data"), std::wstring(L"plugins"), std::wstring(L"legacy_mme"), Wide(CacheKey(images))}) {
            directory /= part; HoldDirectory(directory, retainedFiles, true);
        }
        status.directory = directory;
        const wchar_t* names[] = {L"MMHack.dll", L"MMEffect.dll", L"MMSAdapter.dll"};
        for (size_t i = 0; i < images.size(); ++i) Publish(directory / names[i], images[i]);
        InitializeSettings(*legacy, directory);
        PreloadSystemImports(directory);
        const bool safeSearch = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory") != nullptr;
        const DWORD flags = safeSearch ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_WITH_ALTERED_SEARCH_PATH;
        HMODULE adapter = LoadLibraryExW((directory / L"MMSAdapter.dll").c_str(), nullptr, flags);
        if (!adapter || !SamePath(ModulePath(adapter), directory / L"MMSAdapter.dll"))
            throw std::runtime_error("MMSAdapter cannot bind this executable's host exports, error " + std::to_string(GetLastError()));
        Pin(adapter);
        // From here the foreign initializer may install irreversible hooks.
        // If it fails, abort graphics initialization rather than entering D3D
        // through potentially stale pointers. Successful modules stay pinned.
        safeToContinue = false;
        HMODULE hackModule = LoadLibraryExW((directory / L"MMHack.dll").c_str(), nullptr, flags);
        const DWORD loadError = GetLastError();
        HMODULE effectModule = LegacyMmeModule();
        if (!hackModule || !effectModule || !SamePath(ModulePath(hackModule), directory / L"MMHack.dll") ||
            !SamePath(ModulePath(effectModule), directory / L"MMEffect.dll"))
            throw std::runtime_error("legacy hook initialization failed, error " + std::to_string(loadError));
        Pin(hackModule); Pin(effectModule);
        status.loaded = true; safeToContinue = true;
        LogMessage(status.id + " loaded from " + Utf8(directory.native()));
    } catch (const std::exception& e) {
        try { status.error = e.what(); LogMessage(status.error); } catch (...) {}
    } catch (...) {
        try { status.error = "legacy MME startup failed"; LogMessage(status.error); } catch (...) {}
    }
    return safeToContinue;
}
}
