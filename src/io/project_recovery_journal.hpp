#pragma once

#include <filesystem>
#include <system_error>

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
