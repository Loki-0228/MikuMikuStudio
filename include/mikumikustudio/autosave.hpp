#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <filesystem>

namespace mikumikustudio {
class MMDApp;
// A copy never changes the active project name, dirty state, title or save hooks.
bool SaveSceneCopy(MMDApp*, const wchar_t* destination);
bool SceneSaveInProgress() noexcept;
// UI snapshot capture and completion only; background disk IO permits edits.
bool SceneSaveBlocksEdits() noexcept;
namespace autosave {
inline constexpr unsigned kSettingsCommand = 32813;
inline constexpr char kOwnershipKey[] = "org.mikumikustudio.autosave.filename";
struct Settings {
    bool enabled = false; // Periodic saving only; crash recovery is always armed.
    unsigned intervalMinutes = 1;
    unsigned maxBackups = 100;
    bool overwriteOldest = false;
    bool keepPrevious = false; // Preserve same-directory PMM1 and paired EMM versions.
    std::wstring directory; // Periodic copies only. Crash copies use data/backups.
};
enum class Result { Saved, Disabled, Busy, Full, Failed, Empty };
// Production settings use the autosave section in data/settings.json.
// These explicit INI helpers support tests and one-time legacy migration.
// The default backup directory is resolved beside the current EXE.
Settings LoadSettings(const std::filesystem::path&);
// Display and store installation-contained folders relative to the EXE;
// the live Settings directory remains absolute.
std::wstring DisplayDirectory(const std::wstring&);
bool SaveSettings(const std::filesystem::path&, const Settings&);
bool Validate(const Settings&, std::wstring& error);
const Settings& Current();
bool Configure(MMDApp*, const Settings&, std::wstring& error);
void Initialize(MMDApp*);
void Shutdown() noexcept;
// Invoked only by the outer main loop, never from a nested dialog message loop.
// Optional backup work must not propagate C++ failures into the message loop
// or the fatal-error callback. A failed attempt can be retried later.
void Tick(MMDApp*, ULONGLONG now) noexcept;
Result BackupNow(MMDApp*, bool crash = false) noexcept;
// Periodic UI path: captures now and writes through the shared save worker.
bool QueueBackup(MMDApp*) noexcept;
void TryCrashBackup(unsigned long exceptionCode) noexcept;
std::vector<std::filesystem::path> Backups(const std::filesystem::path&);
bool FolderFull() noexcept;
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*, unsigned);
void PaintMenuWarning(MMDApp*, HDC dc = nullptr);
const wchar_t* Text(MMDApp*, const wchar_t* zh, const wchar_t* en, const wchar_t* ja);
}
}
