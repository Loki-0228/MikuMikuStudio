#pragma once
#include "mikumikustudio/pmm_paths.hpp"
#include <Windows.h>
#include <vector>
#include <utility>

namespace mikumikustudio::pmm_versions {
namespace fs = std::filesystem;
inline std::vector<fs::path> Pair(const fs::path& project, bool plugins = false) {
    std::vector<fs::path> paths{project, pmm_paths::Effect(project)};
    if (plugins) paths.emplace_back(project.native() + L".mms-plugins.json");
    return paths;
}
inline bool Exists(const fs::path& path, bool& exists) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (!exists) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetLastError(ERROR_ACCESS_DENIED); return false;
    }
    return true;
}

// The caller holds the active project's save lock. Older versions are locked
// before examination too: another MMS may have opened one as its active file.
// Preparation never removes the current files. An ordinary publication failure
// restores the version chain; a published/recoverable pair keeps its archive.
class Retention {
    std::vector<std::pair<fs::path, fs::path>> moved_;
    std::vector<fs::path> copied_;
    std::vector<HANDLE> locks_;
    bool committed_ = false;
public:
    Retention() = default;
    Retention(const Retention&) = delete;
    Retention& operator=(const Retention&) = delete;
    ~Retention() {
        const DWORD error = GetLastError();
        if (!committed_) {
            for (auto it = copied_.rbegin(); it != copied_.rend(); ++it) {
                const DWORD attributes = GetFileAttributesW(it->c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                    SetFileAttributesW(it->c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                DeleteFileW(it->c_str());
            }
            for (auto it = moved_.rbegin(); it != moved_.rend(); ++it)
                MoveFileExW(it->second.c_str(), it->first.c_str(), MOVEFILE_WRITE_THROUGH);
        }
        for (auto lock : locks_) CloseHandle(lock);
        SetLastError(error);
    }
    bool Prepare(const fs::path& target, bool plugins) {
        bool exists = false;
        if (!Exists(target, exists)) return false;
        if (!exists) return true;
        std::vector<std::vector<fs::path>> levels{Pair(target, plugins)};
        for (;;) {
            const fs::path next(levels.back().front().native() + L"1");
            if (next.native().size() >= 250) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
            if (plugins) {
                // Allocate before acquiring the handle so allocation failure
                // cannot leave an exclusive lock behind.
                locks_.push_back(INVALID_HANDLE_VALUE);
                auto& lock = locks_.back();
                lock = CreateFileW((next.native() + L".mms-save.lock").c_str(),
                    GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (lock == INVALID_HANDLE_VALUE) { locks_.pop_back(); return false; }
                // Do not rename files that an interrupted save still refers to.
                if (!Exists(next.native() + L".mms-save.json", exists)) return false;
                if (exists) { SetLastError(ERROR_BUSY); return false; }
            }
            auto paths = Pair(next, plugins);
            bool occupied = false;
            for (const auto& path : paths) {
                if (!Exists(path, exists)) return false;
                occupied |= exists;
            }
            levels.push_back(std::move(paths));
            if (!occupied) break;
        }
        // Allocate journal capacity before the first move/copy. Individual path
        // copies happen before their IO, so an exception also rolls back safely.
        moved_.reserve(levels.size() * levels.front().size());
        copied_.reserve(levels.front().size());
        for (std::size_t i = levels.size() - 1; i > 1; --i) {
            for (std::size_t j = 0; j < levels[i].size(); ++j) {
                if (!Exists(levels[i-1][j], exists)) return false;
                if (!exists) continue;
                moved_.emplace_back(levels[i-1][j], levels[i][j]);
                if (!MoveFileExW(levels[i-1][j].c_str(), levels[i][j].c_str(), MOVEFILE_WRITE_THROUGH)) {
                    moved_.pop_back(); return false;
                }
            }
        }
        for (std::size_t j = 0; j < levels[0].size(); ++j) {
            if (!Exists(levels[0][j], exists)) return false;
            if (!exists) continue;
            copied_.push_back(levels[1][j]);
            if (!CopyFileW(levels[0][j].c_str(), levels[1][j].c_str(), TRUE)) {
                if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                    copied_.pop_back();
                return false;
            }
        }
        return true;
    }
    void Commit() noexcept { committed_ = true; }
};

// Original profile has no plugin pair publisher. Preserve the PMM/EMM pair
// with the same preparation/rollback rules around its single atomic replace.
inline bool Publish(const fs::path& temporary, const fs::path& target) {
    Retention retention;
    if (!retention.Prepare(target, false)) return false;
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    retention.Commit();
    return true;
}
}
