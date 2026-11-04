#pragma once
#include <filesystem>
#include <string>

namespace mikumikustudio::plugins {
struct LegacyMmeStartupStatus {
    std::string id, error;
    std::filesystem::path directory, settingsFile;
    bool loaded = false;
};
// UI thread, before the first D3D creation. A false result means a legacy hook
// initializer failed after it might have touched the host: do not create D3D.
bool BootstrapLegacyMme() noexcept;
const LegacyMmeStartupStatus& LegacyMmeStatus() noexcept;
}
