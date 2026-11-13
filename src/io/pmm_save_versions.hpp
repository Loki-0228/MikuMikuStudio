#pragma once
#include "mikumikustudio/pmm_paths.hpp"
#include <Windows.h>
#include <vector>
#include <utility>
#include <algorithm>
#include <set>
#include <string>

namespace mikumikustudio::pmm_versions {
namespace fs = std::filesystem;
inline std::vector<fs::path> Pair(const fs::path& project, bool plugins = false) {
    std::vector<fs::path> paths{project, pmm_paths::Effect(project)};
    if (plugins) paths.emplace_back(project.native() + pmm_paths::PluginSidecarSuffix);
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
// before the final checks: another MMS may have opened one as its active file.
// Preparation never removes the current files. An ordinary publication failure
// restores the version chain; a published/recoverable pair keeps its archive.
class Retention {
    struct Operation {
        fs::path from, to;
        bool present = false;
        bool destinationPresent = false;
        bool completed = false;
    };
    std::vector<Operation> moved_;
    std::vector<Operation> copied_;
    std::vector<HANDLE> locks_;
    bool committed_ = false;

    static bool VersionPath(const fs::path& path) {
        if (path.native().size() < 250) return true;
        SetLastError(ERROR_FILENAME_EXCED_RANGE); return false;
    }
    bool LockVersions(const std::vector<fs::path>& projects) {
        // Prepare every path and handle slot before acquiring the first lock.
        std::vector<std::pair<fs::path, fs::path>> paths;
        paths.reserve(projects.size());
        for (const auto& project : projects)
            paths.emplace_back(project.native() + L".mms-save.lock", project.native() + L".mms-save.json");
        locks_.reserve(projects.size());
        for (const auto& path : paths) {
            const HANDLE lock = CreateFileW(path.first.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (lock == INVALID_HANDLE_VALUE) return false;
            locks_.push_back(lock); // Capacity is already reserved; cannot strand the handle.
        }
        for (const auto& path : paths) {
            bool exists = false;
            if (!Exists(path.second, exists)) return false;
            if (exists) { SetLastError(ERROR_BUSY); return false; }
        }
        return true;
    }
    bool Execute() {
        // No path copies or container growth after the first version mutation.
        // Check even missing members' destinations for directories/reparse points.
        for (auto* operations : {&moved_, &copied_}) for (auto& operation : *operations) {
            if (!Exists(operation.from, operation.present) || !Exists(operation.to, operation.destinationPresent)) return false;
        }
        for (const auto* operations : {&moved_, &copied_}) for (const auto& operation : *operations) {
            if (operation.destinationPresent && std::none_of(moved_.begin(), moved_.end(), [&](const auto& move) {
                return move.present && move.from == operation.to;
            })) {
                SetLastError(ERROR_ALREADY_EXISTS); return false;
            }
        }
        for (auto& operation : moved_) {
            if (!operation.present) continue;
            if (!MoveFileExW(operation.from.c_str(), operation.to.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
            operation.completed = true;
        }
        for (auto& operation : copied_) {
            if (!operation.present) continue;
            // CopyFile can create a partial file before failing. Track it unless
            // failure says an independently owned destination already existed.
            operation.completed = true;
            if (!CopyFileW(operation.from.c_str(), operation.to.c_str(), TRUE)) {
                if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                    operation.completed = false;
                return false;
            }
        }
        return true;
    }
public:
    Retention() = default;
    Retention(const Retention&) = delete;
    Retention& operator=(const Retention&) = delete;
    ~Retention() {
        const DWORD error = GetLastError();
        if (!committed_) {
            for (auto it = copied_.rbegin(); it != copied_.rend(); ++it) {
                if (!it->completed) continue;
                const DWORD attributes = GetFileAttributesW(it->to.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                    SetFileAttributesW(it->to.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                DeleteFileW(it->to.c_str());
            }
            for (auto it = moved_.rbegin(); it != moved_.rend(); ++it)
                if (it->completed) MoveFileExW(it->to.c_str(), it->from.c_str(), MOVEFILE_WRITE_THROUGH);
        }
        for (auto lock : locks_) CloseHandle(lock);
        SetLastError(error);
    }
    // Includes the active level and the vacant terminal level. Recovery must
    // lock the recorded depth: a partially moved level may already look empty.
    bool Plan(const fs::path& target, bool plugins, std::vector<std::vector<fs::path>>& levels,
              std::size_t recordedLevels = 0) {
        levels = {Pair(target, plugins)};
        std::vector<fs::path> projects;
        do {
            const fs::path next(levels.back().front().native() + L"1");
            if (!VersionPath(next)) return false;
            auto paths = Pair(next, plugins);
            bool occupied = false;
            if (!recordedLevels) {
                for (const auto& path : paths) {
                    bool exists = false;
                    if (!Exists(path, exists)) return false;
                    occupied |= exists;
                }
            }
            projects.push_back(next);
            levels.push_back(std::move(paths));
            if (!recordedLevels && !occupied) break;
        } while (!recordedLevels || levels.size() < recordedLevels);
        return LockVersions(projects);
    }
    bool Prepare(const fs::path& target, bool plugins) {
        bool exists = false;
        if (!Exists(target, exists)) return false;
        if (!exists) return true;
        std::vector<std::vector<fs::path>> levels;
        if (!Plan(target, plugins, levels)) return false;
        for (std::size_t i = levels.size() - 1; i > 1; --i)
            for (std::size_t j = 0; j < levels[0].size(); ++j)
                moved_.push_back({levels[i-1][j], levels[i][j]});
        for (std::size_t j = 0; j < levels[0].size(); ++j) copied_.push_back({levels[0][j], levels[1][j]});
        return Execute();
    }
    void Commit() noexcept { committed_ = true; }
};

// Original publication shares the durable file transaction implementation,
// without constructing plugin data. Failure preserves a Win32 error code.
bool Publish(const fs::path& temporary, const fs::path& target) noexcept;
}
