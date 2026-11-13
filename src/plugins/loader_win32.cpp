#include "loader_win32.hpp"
#include <bcrypt.h>
#include <fstream>
#include <set>
#include <string_view>

namespace mikumikustudio::plugins {
namespace {
class Pe {
    std::string bytes_;
    IMAGE_NT_HEADERS64 nt_{};
    std::vector<IMAGE_SECTION_HEADER> sections_;
    template<class T> T Read(size_t offset) const {
        if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset) throw std::invalid_argument("truncated PE");
        T result; std::memcpy(&result, bytes_.data() + offset, sizeof(T)); return result;
    }
    size_t Offset(uint32_t rva, size_t size) const {
        if (rva < nt_.OptionalHeader.SizeOfHeaders && rva <= bytes_.size() && size <= bytes_.size() - rva) return rva;
        for (const auto& s : sections_) if (rva >= s.VirtualAddress && uint64_t(rva) - s.VirtualAddress < s.SizeOfRawData) {
            size_t delta = rva - s.VirtualAddress; uint64_t offset = uint64_t(s.PointerToRawData) + delta;
            if (size <= s.SizeOfRawData - delta && offset <= bytes_.size() && size <= bytes_.size() - offset) return size_t(offset);
        }
        throw std::invalid_argument("invalid PE RVA");
    }
    std::string Name(uint32_t rva) const {
        std::string name;
        for (unsigned i = 0; i < 260; ++i) {
            if (rva > UINT32_MAX - i) break;
            char c = bytes_[Offset(rva + i, 1)]; if (!c) {
                if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) break;
                for (char& ch : name) { if (static_cast<unsigned char>(ch) > 127 || ch < 32) throw std::invalid_argument("non-ASCII DLL import"); if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A'; }
                return name;
            }
            name += c;
        }
        throw std::invalid_argument("invalid DLL import name");
    }
    int CompareExportName(uint32_t rva, std::string_view name) const {
        for (size_t i = 0; i <= name.size(); ++i) {
            if (i > UINT32_MAX - rva) throw std::invalid_argument("export name overflow");
            const auto actual = static_cast<unsigned char>(bytes_[Offset(rva + static_cast<uint32_t>(i), 1)]);
            const auto expected = i < name.size() ? static_cast<unsigned char>(name[i]) : 0;
            if (actual != expected) return actual < expected ? -1 : 1;
        }
        return 0;
    }
public:
    explicit Pe(const fs::path& file): bytes_(ReadFile(file, 256u * 1024u * 1024u)) {
        auto dos = Read<IMAGE_DOS_HEADER>(0); if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) throw std::invalid_argument("invalid DOS header");
        nt_ = Read<IMAGE_NT_HEADERS64>(size_t(dos.e_lfanew));
        if (nt_.Signature != IMAGE_NT_SIGNATURE || nt_.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt_.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !(nt_.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            nt_.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt_.OptionalHeader.NumberOfRvaAndSizes < 14 ||
            nt_.FileHeader.NumberOfSections > 96) throw std::invalid_argument("expected valid x64 DLL");
        const size_t base = size_t(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt_.FileHeader.SizeOfOptionalHeader;
        for (unsigned i = 0; i < nt_.FileHeader.NumberOfSections; ++i) sections_.push_back(Read<IMAGE_SECTION_HEADER>(base + i * sizeof(IMAGE_SECTION_HEADER)));
    }
    void VerifyLegacyObserverExports() const {
        const auto dir = nt_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            throw std::invalid_argument("legacy MMDPlugin has no valid export directory");
        const auto table = Read<IMAGE_EXPORT_DIRECTORY>(Offset(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
        if (!table.NumberOfNames || !table.NumberOfFunctions || !table.AddressOfNames ||
            !table.AddressOfNameOrdinals || !table.AddressOfFunctions)
            throw std::invalid_argument("legacy MMDPlugin has no named factory exports");
        const auto names = Offset(table.AddressOfNames, size_t(table.NumberOfNames) * sizeof(DWORD));
        const auto ordinals = Offset(table.AddressOfNameOrdinals, size_t(table.NumberOfNames) * sizeof(WORD));
        const auto functions = Offset(table.AddressOfFunctions, size_t(table.NumberOfFunctions) * sizeof(DWORD));
        auto hasDirectExport = [&](std::string_view name) {
            uint32_t begin = 0, end = table.NumberOfNames;
            while (begin < end) {
                const auto index = begin + (end - begin) / 2;
                const auto nameRva = Read<DWORD>(names + size_t(index) * sizeof(DWORD));
                const auto order = CompareExportName(nameRva, name);
                if (order < 0) begin = index + 1;
                else if (order > 0) end = index;
                else {
                    const auto ordinal = Read<WORD>(ordinals + size_t(index) * sizeof(WORD));
                    if (ordinal >= table.NumberOfFunctions) return false;
                    const auto entry = Read<DWORD>(functions + size_t(ordinal) * sizeof(DWORD));
                    // An RVA in the export directory names a forwarded symbol.
                    if (!entry || (entry >= dir.VirtualAddress && entry - dir.VirtualAddress < dir.Size))
                        return false;
                    Offset(entry, 1);
                    return std::any_of(sections_.begin(), sections_.end(), [entry](const auto& section) {
                        return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                            entry >= section.VirtualAddress && entry - section.VirtualAddress < section.SizeOfRawData;
                    });
                }
            }
            return false;
        };
        for (const char* name : {"version", "create3", "destroy3"}) {
            if (!hasDirectExport(name))
                throw std::invalid_argument(std::string("legacy MMDPlugin missing direct export ") + name);
        }
    }
    PeInfo Info() const {
        PeInfo result; result.subsystemMajor = nt_.OptionalHeader.MajorSubsystemVersion; result.subsystemMinor = nt_.OptionalHeader.MinorSubsystemVersion;
        for (const unsigned directory : {IMAGE_DIRECTORY_ENTRY_IMPORT, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT}) {
            auto d = nt_.OptionalHeader.DataDirectory[directory]; if (!d.VirtualAddress && !d.Size) continue;
            const unsigned stride = directory == IMAGE_DIRECTORY_ENTRY_IMPORT ? sizeof(IMAGE_IMPORT_DESCRIPTOR) : 32;
            if (!d.VirtualAddress || d.Size < stride || d.Size > 1024 * 1024) throw std::invalid_argument("invalid import directory");
            bool terminated = false;
            for (uint32_t n = 0; n <= d.Size - stride; n += stride) {
                if (d.VirtualAddress > UINT32_MAX - n) throw std::invalid_argument("import overflow");
                size_t off = Offset(d.VirtualAddress + n, stride); uint32_t name;
                if (directory == IMAGE_DIRECTORY_ENTRY_IMPORT) {
                    auto desc = Read<IMAGE_IMPORT_DESCRIPTOR>(off); name = desc.Name;
                    if (!name) { if (desc.FirstThunk || desc.OriginalFirstThunk) throw std::invalid_argument("invalid import terminator"); terminated = true; break; }
                } else {
                    auto attr = Read<uint32_t>(off); name = Read<uint32_t>(off + 4);
                    if (!name) { terminated = true; break; } if (attr != 1) throw std::invalid_argument("delay import must use RVAs");
                }
                result.imports.push_back(Name(name));
            }
            if (!terminated) throw std::invalid_argument("unterminated import table");
        }
        std::sort(result.imports.begin(), result.imports.end()); result.imports.erase(std::unique(result.imports.begin(), result.imports.end()), result.imports.end()); return result;
    }
};
fs::path ModulePath(HMODULE module) {
    std::wstring value(32768, 0); const DWORD n = GetModuleFileNameW(module, value.data(), DWORD(value.size()));
    if (!n || n >= value.size()) throw std::runtime_error("cannot identify loaded module"); value.resize(n); return value;
}
bool ApiSet(const std::string& s) { return s.rfind("api-ms-", 0) == 0 || s.rfind("ext-ms-", 0) == 0; }
}
PeInfo InspectPe(const fs::path& file) { return Pe(file).Info(); }
Version WindowsVersion() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*); auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)}; if (!fn || fn(&v) != 0) throw std::runtime_error("cannot query Windows version"); return {v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber};
}
std::string Sha256(const fs::path& path) {
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } state;
    if (BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA-256 provider");
    DWORD size = 0, copied = 0;
    if (BCryptGetProperty(state.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &copied, 0) < 0) throw std::runtime_error("SHA-256 object size");
    state.object.resize(size);
    if (BCryptCreateHash(state.algorithm, &state.hash, state.object.data(), size, nullptr, 0, 0) < 0) throw std::runtime_error("SHA-256 create");
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("hash open failed");
    char buffer[65536]; while (input.read(buffer, sizeof(buffer)) || input.gcount())
        if (BCryptHashData(state.hash, reinterpret_cast<PUCHAR>(buffer), ULONG(input.gcount()), 0) < 0) throw std::runtime_error("SHA-256 update");
    if (!input.eof()) throw std::runtime_error("hash read failed");
    unsigned char digest[32]; if (BCryptFinishHash(state.hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("SHA-256 finish");
    constexpr char hex[] = "0123456789abcdef"; std::string out; for (unsigned char b : digest) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
}
void LoadedPackage::Pin() noexcept {
    for (const auto& module : modules) if (module->handle) {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module->handle), &pinned);
    }
}
NativeLoader::NativeLoader(bool forceFallback) {
    safeSearch_ = !forceFallback && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory") != nullptr;
    wchar_t system[32768]; UINT n = GetSystemDirectoryW(system, 32768);
    if (!n || n >= 32768) throw std::runtime_error("cannot locate system directory"); system_ = std::wstring(system, n);
}
LoadedPackage NativeLoader::Load(const Manifest& manifest, const Log& log, const fs::path& legacyMmdProvider) {
    if (manifest.kind == Manifest::Kind::LegacyMme) throw std::invalid_argument("legacy MME is loaded only during graphics startup");
    const bool legacyMmd = manifest.kind == Manifest::Kind::LegacyMmdPlugin;
    if (legacyMmd != !legacyMmdProvider.empty()) throw std::invalid_argument("legacy MMDPlugin provider mismatch");
    if (WindowsVersion() < manifest.minWindows) throw std::invalid_argument("minimum Windows version is not met");
    std::map<std::wstring, fs::path> declared;
    for (auto path : manifest.nativeDependencies) if (!declared.emplace(Lower(path.filename().native()), path).second) throw std::invalid_argument("duplicate native DLL basename");
    if (!declared.emplace(Lower(manifest.entry.filename().native()), manifest.entry).second) throw std::invalid_argument("entry basename collision");
    if (legacyMmd && (!manifest.nativeDependencies.empty() ||
        Lower(legacyMmdProvider.filename().native()) != L"mmdplugin.dll" ||
        !declared.emplace(L"mmdplugin.dll", legacyMmdProvider).second))
        throw std::invalid_argument("invalid legacy MMDPlugin provider or dependency");
    auto verifyPath = [&](const fs::path& path) {
        if (legacyMmd && path == legacyMmdProvider)
            PackageFile(path.parent_path(), "MMDPlugin.dll");
        else
            PackageFile(manifest.package, Utf8(path.lexically_relative(manifest.package).native()));
    };
    struct Files {
        std::vector<HANDLE> values;
        ~Files() { for (HANDLE h : values) CloseHandle(h); }
    } locked;
    locked.values.reserve(declared.size());
    for (const auto& [name, path] : declared) {
        verifyPath(path);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot hold plugin binary for verification: " + Utf8(path.native()));
        locked.values.push_back(h);
    }
    if (legacyMmd && Sha256(manifest.entry) != manifest.legacyEntrySha256)
        throw std::invalid_argument("legacy MMDPlugin entry SHA-256 mismatch");
    std::map<std::wstring, unsigned> state;
    std::vector<fs::path> order; std::set<fs::path> systems;
    std::function<void(const fs::path&)> visit = [&](const fs::path& path) {
        const auto key = Lower(path.filename().native()); auto& status = state[key];
        if (status == 1) throw std::invalid_argument("native DLL dependency cycle"); if (status == 2) return; status = 1;
        // Repeat the no-junction check immediately before inspecting any code.
        verifyPath(path);
        const Pe pe(path);
        if (legacyMmd && path == manifest.entry) pe.VerifyLegacyObserverExports();
        for (const auto& imported : pe.Info().imports) {
            auto w = Wide(imported); auto dep = declared.find(Lower(w));
            if (dep != declared.end()) { visit(dep->second); continue; }
            if (ApiSet(imported)) {
                if (!safeSearch_) throw std::invalid_argument("API-set import needs safe system search on this Windows installation");
                continue;
            }
            auto system = system_ / w;
            if (!fs::is_regular_file(system)) throw std::invalid_argument("undeclared non-system dependency: " + imported);
            if (auto loaded = GetModuleHandleW(w.c_str()); loaded && !SamePath(ModulePath(loaded), system))
                throw std::invalid_argument("system DLL basename already bound outside System32: " + imported);
            systems.insert(system);
        }
        status = 2; order.push_back(path);
    };
    if (legacyMmd) visit(legacyMmdProvider);
    visit(manifest.entry);
    for (const auto& path : manifest.nativeDependencies) visit(path);
    LoadedPackage result;
    auto load = [&](const fs::path& path, bool system) {
        const auto key = Lower(path.filename().native()); auto hash = Sha256(path);
        if (auto previous = modules_[key].lock()) {
            if (previous->hash != hash) throw std::invalid_argument("loaded DLL basename/content conflict: " + Utf8(key));
            result.modules.push_back(previous); return previous->handle;
        }
        if (auto present = GetModuleHandleW(path.filename().c_str())) {
            auto actual = ModulePath(present);
            if (!SamePath(actual, path)) throw std::invalid_argument("DLL basename already loaded from another path: " + Utf8(actual.native()));
        }
        auto module = std::make_shared<LoadedModule>(); module->path = path; module->hash = hash;
        DWORD flags = safeSearch_ ? (system ? LOAD_LIBRARY_SEARCH_SYSTEM32 : LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : LOAD_WITH_ALTERED_SEARCH_PATH;
        module->handle = LoadLibraryExW(path.c_str(), nullptr, flags);
        if (!module->handle) throw std::runtime_error("LoadLibraryEx failed " + std::to_string(GetLastError()) + ": " + Utf8(path.native()));
        if (!SamePath(ModulePath(module->handle), path)) throw std::runtime_error("loaded module path differs from preflight");
        if (Sha256(path) != hash) throw std::runtime_error("DLL changed while loading");
        result.modules.push_back(module); modules_[key] = module;
        if (log && !system) log(manifest.id + " loaded " + Utf8(path.native()) + " sha256=" + hash);
        return module->handle;
    };
    // Even the Win7 fallback resolves imported system modules absolutely before
    // invoking the legacy loader. It never changes the process search directory.
    for (const auto& p : systems) load(p, true);
    for (const auto& p : order) { auto h = load(p, false); if (SamePath(p, manifest.entry)) result.entry = h; }
    return result;
}
}
