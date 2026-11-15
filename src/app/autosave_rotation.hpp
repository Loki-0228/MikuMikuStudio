#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace mikumikustudio::autosave::rotation {
namespace fs = std::filesystem;

inline auto ReadWriteTime(const fs::path& path, std::error_code& error) noexcept {
    return fs::last_write_time(path, error);
}

struct Ranking {
    std::vector<fs::path> paths;
    bool complete;
};

template<class ReadTime>
Ranking Rank(std::vector<fs::path> paths, ReadTime readTime) {
    struct Entry {
        fs::path path;
        std::optional<fs::file_time_type> time;
    };
    std::vector<Entry> entries;
    entries.reserve(paths.size());
    bool complete = true;
    for (auto& path : paths) {
        std::error_code error;
        const auto time = readTime(path, error);
        if (error) {
            complete = false;
            entries.push_back({std::move(path), std::nullopt});
        } else {
            entries.push_back({std::move(path), time});
        }
    }
    // No filesystem access during sorting. Unknown ages are listed last and
    // never used to decide which backup to remove.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.time.has_value() != b.time.has_value()) return a.time.has_value();
        if (a.time != b.time) return a.time < b.time;
        return a.path.native() < b.path.native();
    });
    for (std::size_t i = 0; i < entries.size(); ++i)
        paths[i] = std::move(entries[i].path);
    return {std::move(paths), complete};
}

enum class Action { Append, Rotate, Full, AgeUnavailable };
struct Plan {
    Action action;
    std::vector<fs::path> oldestFirst;
    bool CanWrite() const noexcept { return action == Action::Append || action == Action::Rotate; }
};

template<class ReadTime>
Plan Prepare(std::vector<fs::path> paths, unsigned limit, bool overwriteOldest, ReadTime readTime) {
    if (paths.size() < limit) return {Action::Append, {}};
    if (!overwriteOldest) return {Action::Full, {}};
    auto ranked = Rank(std::move(paths), readTime);
    // Even a known-age backup could be newer than an unreadable one. Defer
    // the entire rotation before publishing or deleting any backup.
    if (!ranked.complete) return {Action::AgeUnavailable, {}};
    return {Action::Rotate, std::move(ranked.paths)};
}
}
