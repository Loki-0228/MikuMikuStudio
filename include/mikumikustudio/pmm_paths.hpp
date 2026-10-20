#pragma once
#include <filesystem>
#include <cwchar>

namespace mikumikustudio::pmm_paths {
// Routing only. The PMM loader still validates the header and complete body.
inline bool IsProject(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return extension.size() >= 4 && _wcsnicmp(extension.c_str(), L".pmm", 4) == 0;
}
inline std::filesystem::path Effect(const std::filesystem::path& project) {
    const auto extension = project.extension().wstring();
    auto result = project;
    result.replace_extension(IsProject(project) ? L".emm" + extension.substr(4) : L".emm");
    return result;
}
}
