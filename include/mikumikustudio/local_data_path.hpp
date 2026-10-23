#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <filesystem>
#include <vector>

namespace mikumikustudio::local_data {
// Installation scope survives EXE rename and never depends on process cwd or
// a shared Windows profile. Failure returns empty; callers must not fall back
// to another installation's settings.
inline std::filesystem::path ExecutableDirectory() {
    // Called from nested MME/D3DX callbacks, where little stack can remain.
    std::vector<wchar_t> executable(32768);
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    return size && size < executable.size() ? std::filesystem::path(executable.data()).parent_path() : std::filesystem::path();
}
inline std::filesystem::path File(const wchar_t* name) {
    const auto directory = ExecutableDirectory();
    return directory.empty() ? std::filesystem::path() : directory / L"data" / name;
}
}
