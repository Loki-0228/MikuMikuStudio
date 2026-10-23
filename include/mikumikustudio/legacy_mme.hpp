#pragma once
// MMEffect/MMHack module detection for legacy compatibility. The historical
// code compares the loaded base name with GetModuleHandleW("MMEffect.dll"),
// which misses a renamed copy (for example MMEffectJP.dll) even when it sits in
// the executable directory or a subdirectory. These helpers keep the exact-name
// fast path and fall back to a throttled module snapshot that matches the
// MMEffect/MMHack base-name prefix.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <cwchar>
#include <cstdint>

namespace mikumikustudio {
namespace legacy_mme_detail {
inline bool IsLoaded(HMODULE module) noexcept {
    if (module == nullptr) return false;
    HMODULE current = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(module), &current) != FALSE && current != nullptr;
}
inline const wchar_t* BaseName(const wchar_t* path) noexcept {
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    return name;
}
inline bool Prefix(const wchar_t* name, const wchar_t* prefix) noexcept {
    return _wcsnicmp(name, prefix, std::wcslen(prefix)) == 0;
}
inline HMODULE Find(const wchar_t* prefix, HMODULE& cache, ULONGLONG& nextScan) noexcept {
    if (IsLoaded(cache)) return cache;
    cache = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (nextScan != 0 && now < nextScan) return nullptr;
    nextScan = now + 500; // do not walk the process snapshot every frame
    using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, DWORD*);
    using GetModuleBaseNameFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    static const EnumProcessModulesFn enumModules = []() -> EnumProcessModulesFn {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (auto* fn = kernel ? GetProcAddress(kernel, "K32EnumProcessModules") : nullptr)
            return reinterpret_cast<EnumProcessModulesFn>(fn);
        if (const HMODULE psapi = LoadLibraryW(L"psapi.dll"))
            return reinterpret_cast<EnumProcessModulesFn>(GetProcAddress(psapi, "EnumProcessModules"));
        return nullptr;
    }();
    static const GetModuleBaseNameFn moduleBaseName = []() -> GetModuleBaseNameFn {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (auto* fn = kernel ? GetProcAddress(kernel, "K32GetModuleBaseNameW") : nullptr)
            return reinterpret_cast<GetModuleBaseNameFn>(fn);
        if (const HMODULE psapi = LoadLibraryW(L"psapi.dll"))
            return reinterpret_cast<GetModuleBaseNameFn>(GetProcAddress(psapi, "GetModuleBaseNameW"));
        return nullptr;
    }();
    if (!enumModules || !moduleBaseName) return nullptr;
    HMODULE modules[2048]{};
    DWORD needed = 0;
    const HANDLE process = GetCurrentProcess();
    if (!enumModules(process, modules, sizeof(modules), &needed)) return nullptr;
    const DWORD count = needed / sizeof(HMODULE);
    HMODULE found = nullptr;
    for (DWORD i = 0; i < count && i < 2048; ++i) {
        wchar_t name[MAX_PATH]{};
        if (moduleBaseName(process, modules[i], name, MAX_PATH) && Prefix(name, prefix)) {
            found = modules[i];
            break;
        }
    }
    cache = found;
    if (found) nextScan = 0;
    return found;
}
} // namespace legacy_mme_detail

inline HMODULE LegacyMmeModule() noexcept {
    static HMODULE cache = nullptr;
    static ULONGLONG nextScan = 0;
    HMODULE exact = GetModuleHandleW(L"MMEffect.dll");
    if (exact != nullptr) return exact;
    return legacy_mme_detail::Find(L"MMEffect", cache, nextScan);
}
inline bool LegacyMmeLoaded() noexcept { return LegacyMmeModule() != nullptr; }

inline HMODULE LegacyMmeHackModule() noexcept {
    static HMODULE cache = nullptr;
    static ULONGLONG nextScan = 0;
    HMODULE exact = GetModuleHandleW(L"MMHack.dll");
    if (exact != nullptr) return exact;
    return legacy_mme_detail::Find(L"MMHack", cache, nextScan);
}
inline bool LegacyMmeHackLoaded() noexcept { return LegacyMmeHackModule() != nullptr; }
} // namespace mikumikustudio
