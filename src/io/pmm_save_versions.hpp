#pragma once
#include "mikumikustudio/pmm_paths.hpp"
#include <Windows.h>
#include <vector>
#include <utility>

namespace mikumikustudio::pmm_versions {
namespace fs = std::filesystem;
inline std::vector<fs::path> Pair(const fs::path& project) {
    return {project, pmm_paths::Effect(project)};
}
inline bool Exists(const fs::path& path, bool& exists) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (!exists) return GetLastError() == ERROR_FILE_NOT_FOUND;
    return !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
// The new PMM is already flushed. Reserve previous generations first, then
// atomically replace the active file. Copying the old pair keeps the active
// project readable until publication and leaves it intact on any failure.
inline bool Publish(const fs::path& temporary, const fs::path& target) {
    bool exists = false;
    if (!Exists(target, exists)) return false;
    if (!exists) return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    std::vector<std::vector<fs::path>> levels{Pair(target)};
    for (;;) {
        const fs::path next(levels.back().front().wstring() + L"1");
        // Keep within the host's path field and leave room for MME's alias.
        if (next.wstring().size() >= 250) return false;
        auto pair = Pair(next);
        bool occupied = false;
        for (const auto& path : pair) {
            if (!Exists(path, exists)) return false;
            occupied |= exists;
        }
        levels.push_back(std::move(pair));
        if (!occupied) break;
    }
    struct Rollback {
        std::vector<std::pair<fs::path, fs::path>> moved;
        std::vector<fs::path> copied;
        bool committed = false;
        ~Rollback() {
            if (committed) return;
            const DWORD error = GetLastError();
            for (auto it = copied.rbegin(); it != copied.rend(); ++it) {
                const DWORD attributes = GetFileAttributesW(it->c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                    SetFileAttributesW(it->c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                DeleteFileW(it->c_str());
            }
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                MoveFileExW(it->second.c_str(), it->first.c_str(), MOVEFILE_WRITE_THROUGH);
            SetLastError(error);
        }
    } rollback;
    // Allocate all journal storage before touching files.
    rollback.moved.reserve(levels.size() * 2);
    rollback.copied.reserve(2);
    for (std::size_t i = levels.size() - 1; i > 1; --i) {
        for (std::size_t j = 0; j < levels[i].size(); ++j) {
            if (!Exists(levels[i-1][j], exists)) return false;
            if (!exists) continue;
            rollback.moved.emplace_back(levels[i-1][j], levels[i][j]);
            if (!MoveFileExW(levels[i-1][j].c_str(), levels[i][j].c_str(), MOVEFILE_WRITE_THROUGH)) {
                rollback.moved.pop_back(); return false;
            }
        }
    }
    for (std::size_t j = 0; j < levels[0].size(); ++j) {
        if (!Exists(levels[0][j], exists)) return false;
        if (!exists) continue;
        rollback.copied.push_back(levels[1][j]);
        if (!CopyFileW(levels[0][j].c_str(), levels[1][j].c_str(), TRUE)) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                rollback.copied.pop_back();
            // CopyFile may leave a partially written destination on failure.
            // The name was verified absent before entering this transaction.
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    rollback.committed = true;
    return true;
}
}
