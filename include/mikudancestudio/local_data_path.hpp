#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <filesystem>

namespace mikudancestudio::local_data {
// Installation scope survives EXE rename and never depends on process cwd or
// a shared Windows profile. Failure returns empty; callers must not fall back
// to another installation's settings.
inline std::filesystem::path ExecutableDirectory() {
    wchar_t executable[32768]{};
    const auto size = GetModuleFileNameW(nullptr, executable, 32768);
    return size && size < 32768 ? std::filesystem::path(executable).parent_path() : std::filesystem::path();
}
inline std::filesystem::path File(const wchar_t* name) {
    const auto directory = ExecutableDirectory();
    return directory.empty() ? std::filesystem::path() : directory / L"data" / name;
}
}
