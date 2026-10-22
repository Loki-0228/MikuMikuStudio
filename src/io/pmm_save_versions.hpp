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
    static std::wstring NextNumber(std::wstring number) {
        for (auto i = number.size(); i > 0; --i) {
            if (number[i-1] != L'9') { ++number[i-1]; return number; }
            number[i-1] = L'0';
        }
        return L"1" + number;
    }
    static std::set<std::wstring> NumberedSuffixes(const fs::path& target, bool plugins) {
        std::set<std::wstring> suffixes;
        const auto main = target.filename().native();
        const auto effect = pmm_paths::Effect(target).filename().native();
        const std::wstring side = L".mms-plugins.json";
        const auto collect = [&](const std::wstring& name, const std::wstring& prefix) {
            if (name.size() <= prefix.size() || _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size())) return;
            auto suffix = name.substr(prefix.size());
            if (suffix.front() == L'0' || suffix.find_first_not_of(L"0123456789") != std::wstring::npos) return;
            suffixes.insert(std::move(suffix));
        };
        const auto directory = target.parent_path().empty() ? fs::path(L".") : target.parent_path();
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto name = entry.path().filename().native();
            collect(name, main);
            collect(name, effect);
            if (plugins && name.size() > side.size() &&
                !_wcsicmp(name.c_str() + name.size() - side.size(), side.c_str()))
                collect(name.substr(0, name.size() - side.size()), main);
        }
        return suffixes;
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
    bool Prepare(const fs::path& target, bool plugins, bool numbered = false) {
        bool exists = false;
        if (!Exists(target, exists)) return false;
        if (!exists) return true;
        const auto active = Pair(target, plugins);
        const fs::path first(target.native() + L"1");
        if (!VersionPath(first)) return false;
        const auto previous = Pair(first, plugins);
        std::vector<fs::path> projects;
        std::set<std::wstring> suffixes;
        if (numbered) {
            suffixes = NumberedSuffixes(target, plugins);
            std::vector<std::wstring> ordered(suffixes.begin(), suffixes.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
                return a.size() != b.size() ? a.size() > b.size() : a > b;
            });
            std::set<std::wstring> lockedSuffixes{L"1"};
            for (const auto& suffix : ordered) {
                const auto next = NextNumber(suffix);
                const fs::path from(target.native() + suffix), to(target.native() + next);
                if (!VersionPath(from) || !VersionPath(to)) return false;
                lockedSuffixes.insert(suffix); lockedSuffixes.insert(next);
                const auto source = Pair(from, plugins), destination = Pair(to, plugins);
                for (std::size_t j = 0; j < source.size(); ++j)
                    moved_.push_back({source[j], destination[j]});
            }
            for (const auto& suffix : lockedSuffixes) projects.emplace_back(target.native() + suffix);
        } else {
            std::vector<std::vector<fs::path>> levels{active};
            for (;;) {
                const fs::path next(levels.back().front().native() + L"1");
                if (!VersionPath(next)) return false;
                auto paths = Pair(next, plugins);
                bool occupied = false;
                for (const auto& path : paths) {
                    if (!Exists(path, exists)) return false;
                    occupied |= exists;
                }
                projects.push_back(next);
                levels.push_back(std::move(paths));
                if (!occupied) break;
            }
            for (std::size_t i = levels.size() - 1; i > 1; --i)
                for (std::size_t j = 0; j < active.size(); ++j)
                    moved_.push_back({levels[i-1][j], levels[i][j]});
        }
        for (std::size_t j = 0; j < active.size(); ++j) copied_.push_back({active[j], previous[j]});
        if (plugins && !LockVersions(projects)) return false;
        // A version may have appeared/disappeared during lock acquisition.
        // Reject that plan rather than leave an unexamined generation behind.
        if (numbered && suffixes != NumberedSuffixes(target, plugins)) {
            SetLastError(ERROR_BUSY); return false;
        }
        return Execute();
    }
    void Commit() noexcept { committed_ = true; }
};

// Original profile has no plugin pair publisher. Preserve the PMM/EMM pair
// with the same preparation/rollback rules around its single atomic replace.
inline bool Publish(const fs::path& temporary, const fs::path& target, bool numbered = false) {
    Retention retention;
    if (!retention.Prepare(target, false, numbered)) return false;
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    retention.Commit();
    return true;
}
}
