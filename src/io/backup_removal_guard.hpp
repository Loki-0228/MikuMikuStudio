#pragma once

#include <filesystem>
#include <system_error>

namespace mikumikustudio::project_recovery {

// The caller holds the same project lock as recoverable project saves. Only
// definite absence permits deletion. An unusable journal also reserves the
// backup, and a dangling symlink must not look like a missing journal.
inline bool BackupJournalAbsent(const std::filesystem::path& target) {
    auto journal = target;
    journal += ".mms-save.json";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(journal, error);
    return status.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory);
}

}
