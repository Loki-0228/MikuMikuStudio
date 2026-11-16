#pragma once

#include <filesystem>
#include <system_error>
#ifdef _WIN32
#include <winerror.h>
#endif

namespace mikumikustudio::project_recovery {

inline std::filesystem::path JournalPath(const std::filesystem::path& target) {
    auto journal = target;
    journal += ".mms-save.json";
    return journal;
}

// Only definite absence allows callers to skip journal handling. A dangling
// link, directory or failed lookup is not proof that the name is unoccupied.
inline bool JournalAbsent(const std::filesystem::path& journal, std::error_code& error) {
    const auto status = std::filesystem::symlink_status(journal, error);
    if (status.type() != std::filesystem::file_type::not_found ||
        (error && error != std::errc::no_such_file_or_directory)) return false;
#ifdef _WIN32
    // Windows reports PATH_NOT_FOUND for both missing directories and paths
    // that cross a regular file. Check the nearest existing ancestor.
    const std::error_code pathNotFound(ERROR_PATH_NOT_FOUND, std::system_category());
    if (error == pathNotFound) {
        for (auto parent = journal.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            auto parentStatus = std::filesystem::symlink_status(parent, error);
            if (parentStatus.type() == std::filesystem::file_type::not_found &&
                error == std::errc::no_such_file_or_directory && parent.has_relative_path()) continue;
            if (!error && !std::filesystem::is_directory(parentStatus))
                parentStatus = std::filesystem::status(parent, error);
            if (error) return false;
            if (!std::filesystem::is_directory(parentStatus)) {
                error = std::make_error_code(std::errc::not_a_directory);
                return false;
            }
            break;
        }
    }
#endif
    error.clear();
    return true;
}

// An open or save reports a failed lookup before trying to create a lock.
inline bool JournalAbsent(const std::filesystem::path& journal) {
    std::error_code error;
    const auto absent = JournalAbsent(journal, error);
    if (error) throw std::filesystem::filesystem_error("inspect recovery record", journal, error);
    return absent;
}

}
