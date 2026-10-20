#pragma once
#include <filesystem>
#include <cwchar>

namespace mikumikustudio::pmm_paths {
// Filename routing only. The loader validates the PMM header and body; a
// retained suffix such as .pmm1 does not select a different file format.
inline bool IsProject(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return extension.size() >= 4 && _wcsnicmp(extension.c_str(), L".pmm", 4) == 0;
}
inline bool IsEffect(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return extension.size() >= 4 && _wcsnicmp(extension.c_str(), L".emm", 4) == 0;
}
inline std::filesystem::path Effect(const std::filesystem::path& project) {
    if (IsEffect(project)) return project; // Keep an explicitly chosen EMM suffix.
    const auto extension = project.extension().wstring();
    auto result = project;
    result.replace_extension(IsProject(project) ? L".emm" + extension.substr(4) : L".emm");
    return result;
}
}
