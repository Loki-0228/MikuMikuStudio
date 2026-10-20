#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include <atomic>
#include <cstring>
#include <string>
#include <filesystem>
#include <mutex>
#include <vector>
#include <algorithm>

namespace mikumikustudio {
namespace {
namespace fs = std::filesystem;
struct RelativeAssignment {
    fs::path legacy; // MME expands relative entries against its installation.
    fs::path local;
};
std::mutex assignmentMutex;
std::vector<RelativeAssignment> assignments;
std::vector<RelativeAssignment> projectAliases;

bool ProjectAlias(const wchar_t* filename, std::wstring& result) {
    if (!filename || !*filename) return false;
    const auto path = fs::absolute(filename).lexically_normal();
    std::lock_guard<std::mutex> lock(assignmentMutex);
    for (const auto& entry : projectAliases) {
        if (_wcsicmp(path.c_str(), entry.legacy.c_str()) == 0) {
            result = entry.local.wstring(); return true;
        }
    }
    return false;
}

bool RegularFile(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
fs::path ExecutableDirectory() {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return count && count < 32768 ? fs::path(path).parent_path() : fs::path{};
}
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    return text;
}

void ReadAssignments(const fs::path& emm) {
    std::vector<RelativeAssignment> next;
    const auto executable = ExecutableDirectory();
    HANDLE file = CreateFileW(emm.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
        LARGE_INTEGER size{};
        // Bound the compatibility parser; MME retains ownership of its format.
        if (GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 16 * 1024 * 1024) {
            std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()) {
                for (std::size_t offset = 0; offset < bytes.size();) {
                    const auto end = bytes.find('\n', offset);
                    auto line = Trim(bytes.substr(offset, end == std::string::npos ? end : end - offset));
                    offset = end == std::string::npos ? bytes.size() : end + 1;
                    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
                    const auto equal = line.find('=');
                    if (equal == std::string::npos) continue;
                    const auto value = Trim(line.substr(equal + 1));
                    if (value.empty() || value.find('\0') != std::string::npos) continue;
                    // Decode before looking for separators: CP932 can contain
                    // 0x5c inside a character, not just as a directory separator.
                    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
                        std::wstring decoded;
                        if (!text_encoding::Decode(value.c_str(), cp, decoded)) continue;
                        const fs::path relative(decoded);
                        if (relative.empty() || relative.has_root_path() || !relative.has_extension()) continue;
                        const auto legacy = (executable / relative).lexically_normal();
                        const auto local = (emm.parent_path() / relative).lexically_normal();
                        const auto same = [&](const RelativeAssignment& entry) {
                            return entry.legacy == legacy && entry.local == local;
                        };
                        if (std::none_of(next.begin(), next.end(), same)) next.push_back({legacy, local});
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(assignmentMutex);
    assignments.swap(next); // A project without EMM must not retain old aliases.
}

bool RelativePath(const wchar_t* filename, std::wstring& resolved) {
    if (!filename || !*filename) return false;
    auto path = fs::path(filename).lexically_normal();
    if (!path.has_root_path()) path = (ExecutableDirectory() / path).lexically_normal();
    std::lock_guard<std::mutex> lock(assignmentMutex);
    // All project-relative candidates precede MME's expanded absolute path.
    for (const auto& entry : assignments) {
        if (_wcsicmp(path.c_str(), entry.legacy.c_str()) == 0 && RegularFile(entry.local)) {
            resolved = entry.local.native();
            return true;
        }
    }
    return false;
}

HANDLE TrackAssignmentRead(HANDLE file, bool readOnly) noexcept {
    if (file == INVALID_HANDLE_VALUE || !readOnly) return file;
    const DWORD error = GetLastError();
    try {
        // Also cover EMM files opened directly through MME's own Load menu.
        // The handle names the exact file chosen by Windows, independent of ACP.
        wchar_t filename[32768]{};
        const DWORD size = GetFinalPathNameByHandleW(file, filename, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size && size < 32768) {
            std::wstring path(filename, size);
            if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) path = L"\\\\" + path.substr(8);
            else if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
            if (_wcsicmp(fs::path(path).extension().c_str(), L".emm") == 0) ReadAssignments(path);
        }
    } catch (...) { /* Preserve the native file operation on compatibility failure. */ }
    SetLastError(error);
    return file;
}

// MME keeps EMM paths as bytes and calls ANSI Win32 APIs. A file saved on
// Japanese Windows can therefore point at an existing, different Unicode
// spelling on Chinese Windows. Prefer the current ACP and only fall back to
// another encoding when that exact decoded file really exists.
bool ExistingPath(const char* bytes, std::wstring& path, UINT& codePage) {
    if (!bytes || !*bytes) return false;
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded;
        if (text_encoding::Decode(bytes, cp, decoded) && ProjectAlias(decoded.c_str(), path)) {
            codePage = cp; return true;
        }
        if (text_encoding::Decode(bytes, cp, decoded) && RelativePath(decoded.c_str(), path)) {
            codePage = cp;
            return true;
        }
    }
    bool nonAscii = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes); *p; ++p)
        nonAscii |= *p >= 0x80;
    if (!nonAscii || (!std::strchr(bytes, '\\') && !std::strchr(bytes, '/')))
        return false;
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        if (!text_encoding::Decode(bytes, cp, path)) continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            codePage = cp;
            return true;
        }
    }
    return false;
}

bool MissingPath(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_INVALID_NAME || error == ERROR_NO_UNICODE_TRANSLATION;
}

DWORD WINAPI EffectFileAttributes(LPCSTR filename) {
    std::wstring preferred;
    UINT preferredCp = 0;
    if (ExistingPath(filename, preferred, preferredCp))
        return GetFileAttributesW(preferred.c_str());
    const DWORD attributes = GetFileAttributesA(filename);
    if (attributes != INVALID_FILE_ATTRIBUTES) return attributes;
    const DWORD error = GetLastError();
    std::wstring path;
    UINT cp = 0;
    if (MissingPath(error) && ExistingPath(filename, path, cp))
        return GetFileAttributesW(path.c_str());
    SetLastError(error);
    return attributes;
}

HANDLE WINAPI OpenEffectFile(LPCSTR filename, DWORD access, DWORD share,
                            LPSECURITY_ATTRIBUTES security, DWORD creation,
                            DWORD attributes, HANDLE templateFile) {
    const bool readOnly = creation == OPEN_EXISTING && (access == GENERIC_READ || access == 0);
    // Only our explicitly registered notification aliases can redirect writes.
    // In particular, missing effect/texture files never redirect a write.
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
        std::wstring decoded, mapped;
        if (text_encoding::Decode(filename, cp, decoded) && ProjectAlias(decoded.c_str(), mapped))
            return TrackAssignmentRead(CreateFileW(mapped.c_str(), access, share, security,
                creation, attributes, templateFile), readOnly);
    }
    std::wstring preferred;
    UINT preferredCp = 0;
    if (readOnly && ExistingPath(filename, preferred, preferredCp)) {
        HANDLE file = CreateFileW(preferred.c_str(), access, share, security, creation, attributes, templateFile);
        if (file != INVALID_HANDLE_VALUE) return TrackAssignmentRead(file, readOnly);
    }
    HANDLE file = CreateFileA(filename, access, share, security, creation,
                              attributes, templateFile);
    if (file != INVALID_HANDLE_VALUE) return TrackAssignmentRead(file, readOnly);
    const DWORD error = GetLastError();
    std::wstring path;
    UINT cp = 0;
    // A compatibility lookup must never redirect writes or create a new file.
    if (creation == OPEN_EXISTING && (access == GENERIC_READ || access == 0) &&
        MissingPath(error) && ExistingPath(filename, path, cp))
        return TrackAssignmentRead(CreateFileW(path.c_str(), access, share, security, creation,
                           attributes, templateFile), readOnly);
    SetLastError(error);
    return file;
}

int WINAPI DecodeEffectPath(UINT codePage, DWORD flags, LPCCH bytes, int byteCount,
                           LPWSTR output, int capacity) {
    if ((codePage == CP_ACP || codePage == CP_THREAD_ACP || codePage == GetACP()) &&
        bytes && (byteCount == -1 || byteCount > 0) &&
        (flags == 0 || flags == MB_ERR_INVALID_CHARS)) {
        std::string bounded = byteCount == -1 ? std::string(bytes)
                                              : std::string(bytes, byteCount);
        const bool terminated = byteCount == -1 || bounded.back() == '\0';
        if (!bounded.empty() && bounded.back() == '\0') bounded.pop_back();
        std::wstring path;
        UINT resolvedCodePage = 0;
        if (bounded.find('\0') == std::string::npos &&
            ExistingPath(bounded.c_str(), path, resolvedCodePage)) {
            const int needed = static_cast<int>(path.size()) + (terminated ? 1 : 0);
            if (capacity == 0) return needed;
            if (!output || capacity < 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return 0;
            }
            if (capacity < needed) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            std::memcpy(output, path.c_str(), needed * sizeof(wchar_t));
            return needed;
        }
    }
    return MultiByteToWideChar(codePage, flags, bytes, byteCount, output, capacity);
}

using CreateEffectFile = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR, const void*, const void*,
    DWORD, void*, void**, void**);
std::atomic<CreateEffectFile> originalEffectFile{nullptr};

// Read COM dispatch addresses without calling the object. A non-null device or
// pool alone does not establish that its vtable is still valid at FX creation.
bool ReadDispatchPointer(const void* address, void*& value) noexcept {
    value = nullptr;
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, &value,
        sizeof(value), &read) && read == sizeof(value);
}
void TraceEffectDispatch(const char* label, const void* object, bool device) noexcept {
    if (!object) {
        runtime_log::Write("MME_EFFECT_DISPATCH %s object=null", label);
        return;
    }
    void* table = nullptr;
    if (!ReadDispatchPointer(object, table) || !table) {
        runtime_log::Write("MME_EFFECT_DISPATCH %s object=%p vtable=unreadable", label, object);
        return;
    }
    // IDirect3DDevice9 shader creation is used by the FX compiler; the effect
    // pool exposes IUnknown. Log targets only, retaining the normal call path.
    const unsigned indices[] = {0, 1, 2, 7, 23, 86, 91, 106};
    for (unsigned i = 0; i < (device ? _countof(indices) : 3); ++i) {
        const auto slot = reinterpret_cast<ULONG_PTR>(table) + indices[i] * sizeof(void*);
        void* target = nullptr;
        const bool readable = ReadDispatchPointer(reinterpret_cast<void*>(slot), target);
        MEMORY_BASIC_INFORMATION region{};
        const bool mapped = readable && target && VirtualQuery(target, &region, sizeof(region));
        runtime_log::Write("MME_EFFECT_DISPATCH %s vtable=%p slot=%u target=%p readable=%u base=%p protection=0x%lX",
            label, table, indices[i], target, unsigned(readable), mapped ? region.AllocationBase : nullptr,
            mapped ? region.Protect : 0);
    }
}
HRESULT WINAPI TraceEffectFile(IDirect3DDevice9* device, LPCWSTR filename, const void* macros,
        const void* include, DWORD flags, void* pool, void** effect, void** errors) {
    const DWORD incomingError = GetLastError();
    const auto create = originalEffectFile.load(std::memory_order_acquire);
    runtime_log::WritePath("MME_EFFECT_FILE", filename);
    runtime_log::Write("MME_EFFECT_CREATE function=%p device=%p include=%p pool=%p macros=%p flags=0x%08lX",
        create, device, include, pool, macros, flags);
    TraceEffectDispatch("device", device, true);
    TraceEffectDispatch("pool", pool, false);
    SetLastError(incomingError);
    const auto result = create ? create(device, filename, macros, include, flags, pool, effect, errors)
                               : E_NOINTERFACE;
    const auto error = GetLastError();
    runtime_log::Write("MME_EFFECT_RESULT hr=0x%08lX effect=%p", static_cast<unsigned long>(result),
        effect ? *effect : nullptr);
    SetLastError(error);
    return result;
}

struct ImportReplacement { const char* name; void* original; void* replacement; };

// Restrict compatibility and effect diagnostics to MMEffect's own imports.
// Other plugins and the host keep their normal ANSI API behavior.
void PatchImports(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= size ||
        imports.Size > size - imports.VirtualAddress) return;
    const ImportReplacement replacements[] = {
        {"GetFileAttributesA", reinterpret_cast<void*>(&GetFileAttributesA),
                              reinterpret_cast<void*>(&EffectFileAttributes)},
        {"CreateFileA", reinterpret_cast<void*>(&CreateFileA),
                        reinterpret_cast<void*>(&OpenEffectFile)},
        {"MultiByteToWideChar", reinterpret_cast<void*>(&MultiByteToWideChar),
                                reinterpret_cast<void*>(&DecodeEffectPath)},
    };
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (std::size_t i = 0; i < imports.Size / sizeof(*descriptors); ++i) {
        const auto& descriptor = descriptors[i];
        if (!descriptor.Name) break;
        if (descriptor.Name >= size ||
            strnlen_s(reinterpret_cast<const char*>(base + descriptor.Name), size - descriptor.Name) == size - descriptor.Name ||
            !descriptor.OriginalFirstThunk || descriptor.OriginalFirstThunk >= size ||
            !descriptor.FirstThunk || descriptor.FirstThunk >= size) continue;
        const char* library = reinterpret_cast<const char*>(base + descriptor.Name);
        const bool kernel = _stricmp(library, "kernel32.dll") == 0;
        const auto compiler = _strnicmp(library, "d3dx9_", 6) == 0
            ? reinterpret_cast<CreateEffectFile>(GetProcAddress(GetModuleHandleA(library), "D3DXCreateEffectFromFileW"))
            : nullptr;
        if (!kernel && !compiler) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor.OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor.FirstThunk);
        const auto nameCount = (size - descriptor.OriginalFirstThunk) / sizeof(*names);
        const auto slotCount = (size - descriptor.FirstThunk) / sizeof(*slots);
        for (std::size_t j = 0; j < nameCount && j < slotCount && names[j].u1.AddressOfData; ++j) {
            if (IMAGE_SNAP_BY_ORDINAL(names[j].u1.Ordinal)) continue;
            const auto rva = names[j].u1.AddressOfData;
            if (rva >= size || size - rva <= sizeof(WORD)) continue;
            const char* name = reinterpret_cast<const char*>(base + rva + sizeof(WORD));
            if (strnlen_s(name, size - rva - sizeof(WORD)) == size - rva - sizeof(WORD)) continue;
            if (compiler && std::strcmp(name, "D3DXCreateEffectFromFileW") == 0 &&
                slots[j].u1.Function == reinterpret_cast<ULONG_PTR>(compiler)) {
                DWORD protection = 0;
                auto* slot = &slots[j].u1.Function;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
                    originalEffectFile.store(compiler, std::memory_order_release);
                    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                        reinterpret_cast<void*>(&TraceEffectFile), reinterpret_cast<void*>(compiler));
                    DWORD ignored = 0; VirtualProtect(slot, sizeof(void*), protection, &ignored);
                }
            }
            if (!kernel) continue;
            for (const auto& entry : replacements) {
                if (std::strcmp(name, entry.name) != 0 ||
                    slots[j].u1.Function != reinterpret_cast<ULONG_PTR>(entry.original)) continue;
                DWORD protection = 0;
                auto* slot = &slots[j].u1.Function;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) continue;
                InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                                  entry.replacement, entry.original);
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), protection, &ignored);
            }
        }
    }
}
}  // namespace

void InstallEffectPathCompatibility() {
    const HMODULE effect = GetModuleHandleW(L"MMEffect.dll");
    if (effect) PatchImports(effect);
}

void PrepareEffectProjectPaths(const wchar_t* project) {
    if (!project || !*project) return;
    try {
        const auto emm = pmm_paths::Effect(fs::absolute(project));
        ReadAssignments(emm);
    } catch (...) {
        std::lock_guard<std::mutex> lock(assignmentMutex);
        assignments.clear();
    }
}

std::wstring EffectProjectNotificationPath(const wchar_t* project) {
    const auto path = fs::absolute(project).lexically_normal();
    if (!pmm_paths::IsProject(path) || path.extension().wstring().size() == 4) return path.wstring();
    const auto local = pmm_paths::Effect(path);
    std::lock_guard<std::mutex> lock(assignmentMutex);
    for (const auto& entry : projectAliases) {
        if (_wcsicmp(entry.local.c_str(), local.c_str()) == 0) {
            auto notification = entry.legacy; notification.replace_extension(L".pmm");
            return notification.wstring();
        }
    }
    // A unique virtual filename avoids colliding with a real scene.pmm1.pmm.
    // MMHack queues notifications before checking whether CreateFile succeeds.
    static unsigned sequence = 0;
    for (;;) {
        auto notification = path.parent_path() / (L"~mms-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(++sequence) + L".pmm");
        auto legacy = notification; legacy.replace_extension(L".emm");
        if (GetFileAttributesW(notification.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        projectAliases.push_back({legacy, local});
        return notification.wstring();
    }
}
}  // namespace mikumikustudio
