#pragma once
#include "nvidia_antialias_driver.hpp"

namespace mikumikustudio::nvidia_antialias_fix::detail {
constexpr wchar_t kHelperName[] = L"MMSNvidiaAAFix.dll";
constexpr wchar_t kApplicationName[] = L"MikuMikuDance.exe";

inline std::wstring ModulePath(HMODULE module = nullptr) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? std::wstring(path, length) : std::wstring{};
}
inline bool CanonicalFile(const std::wstring& path, std::wstring& canonical) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    wchar_t buffer[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(file, buffer, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool regular = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(file);
    if (!regular || !count || count >= 32768) return false;
    canonical.assign(buffer, count);
    if (canonical.compare(0, 4, L"\\\\?\\") == 0) canonical.erase(0, 4);
    return canonical.size() > 3 && canonical[1] == L':' && canonical[2] == L'\\';
}
inline std::wstring CanonicalModulePath() {
    std::wstring path;
    return CanonicalFile(ModulePath(), path) ? path : std::wstring{};
}
inline bool IsSiblingApplication(const std::wstring& helper, const std::wstring& application) {
    const auto h = NormalizePath(helper), a = NormalizePath(application);
    if (h.size() >= kText || a.size() >= kText || h.size() < 4 || a.size() < 4 ||
        h[1] != L':' || a[1] != L':' || h[2] != L'/' || a[2] != L'/' ||
        h.find(L"/../") != std::wstring::npos || a.find(L"/../") != std::wstring::npos ||
        h.find(L"/./") != std::wstring::npos || a.find(L"/./") != std::wstring::npos)
        return false;
    const auto hp = h.find_last_of(L'/'), ap = a.find_last_of(L'/');
    return h.substr(hp + 1) == NormalizePath(kHelperName) &&
        a.substr(ap + 1) == NormalizePath(kApplicationName) &&
        h.substr(0, hp) == a.substr(0, ap);
}
inline bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

}
