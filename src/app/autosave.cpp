#include "mikumikustudio/background_save.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/effect_api.hpp"
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <io.h>
#include <share.h>

namespace mikumikustudio::autosave {
namespace fs = std::filesystem;
namespace {
Settings settings;
bool loaded = false, full = false;
// Process-local: neither changing settings nor reopening a project resets this.
bool fullWarningShown = false;
std::wstring fullFolders;
std::atomic<bool> saving{false};
std::atomic<MMDApp*> activeApp{nullptr};
DWORD mainThread = 0;
ULONGLONG nextSave = 0, nextScan = 0;

fs::path SettingsPath() {
    // Isolated integration tests use a disposable profile, never the user's.
    wchar_t overridePath[32768]{};
    const auto n = GetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", overridePath, 32768);
    if (n && n < 32768) return overridePath;
    // Each installation owns its settings. Do not inherit the old shared
    // LOCALAPPDATA profile, whose absolute backup path may belong to another EXE.
    return local_data::File(L"autosave.ini");
}
fs::path NormalizeDirectoryPath(fs::path path) {
    path = path.lexically_normal();
    // filesystem::path treats a trailing separator as an empty filename.
    // Remove those empty components without walking above a volume/share root.
    while (!path.empty() && path != path.root_path() && path.filename().empty()) {
        const fs::path parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return path;
}
Settings Defaults() {
    Settings value;
    const auto directory = local_data::File(L"backups");
    if (!directory.empty()) value.directory = directory.wstring();
    return value;
}
bool Regular(const fs::path& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
bool Owned(const fs::path& path) {
    if (!Regular(path) || path.extension() != L".pmm" ||
        path.stem().wstring().find(L"_autosave_") == std::wstring::npos) return false;
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0)) return false;
    struct Close { int fd; ~Close() { _close(fd); } } close{fd};
    pmm_extension::Document document;
    std::uint64_t standardSize = 0;
    if (pmm_extension::Read(fd, document, standardSize) != pmm_extension::ReadResult::Loaded) return false;
    const auto* marker = pmm_extension::Find(document, kOwnershipKey);
    const auto filename = path.filename().u8string();
    return marker && std::string(marker->value.begin(), marker->value.end()) == filename;
}
void SetFull(MMDApp* app, bool value) {
    if (full == value) return;
    full = value;
    if (app && IsWindow(app->MainWindow())) {
        DrawMenuBar(app->MainWindow());
        PaintMenuWarning(app);
    }
}
void RefreshCapacity(MMDApp* app) noexcept {
    try {
        // Publish the complete scan only. Allocation failure must not erase a
        // known-full folder or leave a partially constructed warning behind.
        std::wstring folders;
        const fs::path crashDirectory = Defaults().directory;
        const fs::path periodicDirectory = settings.directory;
        const auto inspect = [&](const fs::path& directory) {
            if (directory.empty()) return;
            try {
                if (Backups(directory).size() >= settings.maxBackups) {
                    if (!folders.empty()) folders += L"\n";
                    folders += directory.native();
                }
            } catch (const fs::filesystem_error&) {
                // Missing or unreadable folders do not suppress the other scan.
            }
        };
        inspect(periodicDirectory);
        std::error_code ec;
        if (_wcsicmp(periodicDirectory.c_str(), crashDirectory.c_str()) != 0 &&
            !fs::equivalent(periodicDirectory, crashDirectory, ec)) inspect(crashDirectory);
        fullFolders.swap(folders);
        SetFull(app, !fullFolders.empty());
    } catch (...) {
        runtime_log::Write("AUTOSAVE capacity scan failed; retaining previous status");
    }
}
bool Ready(MMDApp* app, bool crash) {
    return app && GetCurrentThreadId() == mainThread && IsWindow(app->MainWindow()) &&
        !app->SceneMutationInProgress() && !SceneSaveInProgress() && !bake::UiLocked() &&
        (crash || (IsWindowEnabled(app->MainWindow()) && !app->RecordingWindow() &&
            !app->PlaybackActive() && !app->FrameStepPlayback() && !GetCapture()));
}
void WarnIfFull(MMDApp* app) noexcept {
    if (!full || fullWarningShown || saving.load() || !Ready(app, false)) return;
    try {
        const std::wstring message = std::wstring(Text(app,
            L"以下备份文件夹已达到数量上限（", L"These backup folders have reached the limit (",
            L"次のバックアップフォルダーが保存数の上限（")) + std::to_wstring(settings.maxBackups) +
            Text(app, L" 份）：\n\n", L" files):\n\n", L" 件）に達しました：\n\n") + fullFolders + L"\n\n" +
            (settings.overwriteOldest ? Text(app,
                L"已开启覆盖旧备份，后续备份将尝试替换最旧的备份。",
                L"Overwrite oldest is enabled. Future backups will try to replace the oldest backup.",
                L"古いバックアップの上書きが有効です。次回は最も古いバックアップの置き換えを試みます。") : Text(app,
                L"已关闭覆盖旧备份，这些文件夹暂时无法保存新备份。请清理不再需要的备份，或在“自动保存设置”中增加上限、开启覆盖旧备份。",
                L"Overwrite oldest is disabled, so new backups cannot be saved in these folders. Remove unneeded backups, or raise the limit or enable overwriting in Autosave settings.",
                L"古いバックアップの上書きが無効なため、これらのフォルダーには新しく保存できません。不要なバックアップを削除するか、「自動保存の設定」で上限を増やすか上書きを有効にしてください。")) +
            Text(app, L"\n\n本次启动不再提示。", L"\n\nThis warning will not appear again until you restart MMS.",
                L"\n\nMMS を再起動するまで、この警告は再表示されません。");
        // Set before entering the modal loop, including any reentrant idle work.
        fullWarningShown = true;
        if (!MessageBoxW(app->MainWindow(), message.c_str(), Text(app,
            L"备份文件夹已满", L"Backup folder is full", L"バックアップフォルダーがいっぱいです"), MB_OK | MB_ICONWARNING))
            fullWarningShown = false; // A box that could not be created was not shown.
    } catch (...) {
        fullWarningShown = false;
        runtime_log::Write("AUTOSAVE capacity warning deferred");
    }
}
fs::path NewName(MMDApp* app, bool crash, const fs::path& directory) {
    std::wstring stem = fs::path(app->EnvFileName()).stem().wstring();
    // A recovered backup continues the original name instead of nesting timestamps.
    const auto suffix = stem.rfind(L"_autosave_");
    if (suffix != std::wstring::npos && Owned(app->EnvFileName())) stem.resize(suffix);
    if (stem.empty()) stem = Text(app, L"未命名", L"Untitled", L"無題");
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t stamp[96]{};
    swprintf_s(stamp, L"_autosave_%04u%02u%02u_%02u%02u%02u_%03u%s",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, crash ? L"_crash" : L"");
    for (unsigned collision = 0; collision < 10000; ++collision) {
        const auto path = directory / (stem + stamp + (collision ? L"_" + std::to_wstring(collision) : L"") + L".pmm");
        if (path.wstring().size() >= 256) return {};
        std::error_code error;
        if (!fs::exists(path, error) && !error) return path;
    }
    return {};
}
// Windows SEH is deliberately isolated from C++ stack-unwinding objects.
void CrashAttempt() noexcept {
    __try {
        const auto result = BackupNow(activeApp.load(), true);
        const char* status = result == Result::Saved ? "saved" : result == Result::Full ? "folder full" :
            result == Result::Busy ? "scene busy or folder locked" : "save failed";
        runtime_log::Write("CRASH_BACKUP %s", status);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime_log::Write("CRASH_BACKUP failed with exception=0x%08lX", GetExceptionCode());
    }
}
}

Settings LoadSettings(const fs::path& path) {
    Settings value = Defaults();
    if (path.empty()) return value;
    wchar_t directory[32768]{};
    wchar_t directoryMode[32]{};
    GetPrivateProfileStringW(L"autosave", L"directoryMode", L"custom", directoryMode, 32, path.c_str());
    if (wcscmp(directoryMode, L"executable") != 0) {
        GetPrivateProfileStringW(L"autosave", L"directory", value.directory.c_str(), directory, 32768, path.c_str());
        value.directory = directory;
    }
    value.enabled = GetPrivateProfileIntW(L"autosave", L"enabled", 0, path.c_str()) == 1;
    value.intervalMinutes = GetPrivateProfileIntW(L"autosave", L"intervalMinutes", 1, path.c_str());
    value.maxBackups = GetPrivateProfileIntW(L"autosave", L"maxBackups", 100, path.c_str());
    value.overwriteOldest = GetPrivateProfileIntW(L"autosave", L"overwriteOldest", 0, path.c_str()) == 1;
    std::wstring error;
    return Validate(value, error) ? value : Defaults();
}
bool Validate(const Settings& value, std::wstring& error) {
    error.clear();
    if (value.intervalMinutes < 1 || value.intervalMinutes > 1440) error = L"保存间隔须为 1 至 1440 分钟。 / Interval: 1–1440 minutes.";
    else if (value.maxBackups < 1 || value.maxBackups > 100000) error = L"备份上限须为 1 至 100000 份。 / Backup limit: 1–100000.";
    else if (value.directory.empty() || !fs::path(value.directory).is_absolute() ||
        value.directory.size() > 200 || value.directory.find_first_of(L"\r\n") != std::wstring::npos)
        error = L"请选择有效的备份文件夹，路径长度不超过 200 个字符。 / Choose a backup folder with a path of at most 200 characters.";
    return error.empty();
}
bool SaveSettings(const fs::path& path, const Settings& value) {
    std::wstring error;
    if (path.empty() || !Validate(value, error)) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    const auto temporary = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const auto defaultDirectory = Defaults().directory;
    const auto directoryPath = NormalizeDirectoryPath(value.directory);
    const auto defaultDirectoryPath = NormalizeDirectoryPath(defaultDirectory);
    const bool besideExecutable = !defaultDirectory.empty() && _wcsicmp(
        directoryPath.c_str(), defaultDirectoryPath.c_str()) == 0;
    const std::wstring body = L"\xfeff[autosave]\r\nenabled=" + std::to_wstring(value.enabled) +
        L"\r\nintervalMinutes=" + std::to_wstring(value.intervalMinutes) + L"\r\nmaxBackups=" +
        std::to_wstring(value.maxBackups) + L"\r\noverwriteOldest=" + std::to_wstring(value.overwriteOldest) +
        L"\r\ndirectoryMode=" + (besideExecutable ? L"executable" : L"custom") +
        L"\r\ndirectory=" + (besideExecutable ? std::wstring(L"data/backups") : value.directory) + L"\r\n";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(body.size() * sizeof(wchar_t));
    bool ok = WriteFile(file, body.data(), size, &written, nullptr) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    ok = ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temporary.c_str());
    // Invalidate the Win32 profile API cache after the atomic replacement.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return ok;
}
const Settings& Current() {
    if (!loaded) { settings = LoadSettings(SettingsPath()); loaded = true; }
    return settings;
}
bool Configure(MMDApp* app, const Settings& value, std::wstring& error) {
    if (!Validate(value, error)) return false;
    if (value.enabled || value.directory != Current().directory) {
        std::error_code ec;
        fs::create_directories(value.directory, ec);
        if (ec) { error = L"无法创建备份文件夹，请检查路径和权限。 / Cannot create backup folder."; return false; }
        // Validate folder changes even with periodic saving off, but allow turning
        // periodic saving off after the existing folder becomes unavailable.
        wchar_t probe[MAX_PATH]{};
        if (!GetTempFileNameW(value.directory.c_str(), L"mms", 0, probe)) {
            error = L"备份文件夹不可写，请选择其他文件夹。 / Backup folder is not writable."; return false;
        }
        DeleteFileW(probe);
    }
    if (!SaveSettings(SettingsPath(), value)) {
        error = L"无法保存自动保存设置。 / Cannot save autosave settings."; return false;
    }
    settings = value; loaded = true;
    nextSave = GetTickCount64() + ULONGLONG(settings.intervalMinutes) * 60000;
    nextScan = 0;
    RefreshCapacity(app);
    RefreshMenu(app);
    return true;
}
std::vector<fs::path> Backups(const fs::path& directory) {
    std::vector<fs::path> result;
    std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    if (ec) throw fs::filesystem_error("Read autosave folder", directory, ec);
    for (; it != end; it.increment(ec)) {
        if (ec) throw fs::filesystem_error("Read autosave folder", directory, ec);
        if (Owned(it->path())) result.push_back(it->path());
    }
    if (ec) throw fs::filesystem_error("Read autosave folder", directory, ec);
    // All timestamps have fixed width; use last-write time across different stems.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        std::error_code ea, eb;
        const auto ta = fs::last_write_time(a, ea), tb = fs::last_write_time(b, eb);
        return !ea && !eb && ta != tb ? ta < tb : a.native() < b.native();
    });
    return result;
}
bool FolderFull() noexcept { return full; }
bool QueueBackup(MMDApp* app) noexcept {
    try {
        if (!Current().enabled || !Ready(app, false)) return false;
        struct BackupWork {
            Settings settings;
            fs::path destination, active;
            std::vector<fs::path> old;
            HANDLE lock = INVALID_HANDLE_VALUE;
            bool full = false, overwritten = false;
            ~BackupWork() { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); }
        };
        auto work = std::make_shared<BackupWork>();
        work->settings = settings;
        work->destination = NewName(app, false, settings.directory);
        if (work->destination.empty()) return false;
        if (*app->EnvFileName()) work->active = fs::absolute(app->EnvFileName());
        background_save::Options options;
        options.backup = true;
        options.beforeWrite = [work] {
            fs::create_directories(work->settings.directory);
            const auto lockPath = fs::path(work->settings.directory) / L".mms-autosave.lock";
            work->lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (work->lock == INVALID_HANDLE_VALUE) return false;
            work->old = Backups(work->settings.directory);
            work->full = work->old.size() >= work->settings.maxBackups && !work->settings.overwriteOldest;
            if (!work->full && work->old.size() >= work->settings.maxBackups)
                background_save::ReportBackupOverwrite();
            return !work->full;
        };
        options.afterWrite = [work] {
            // Publish the new snapshot before removing anything. Protect the
            // active recovery project, and delete only owned PMM/EMM pairs.
            while (work->old.size() >= work->settings.maxBackups) {
                auto victim = std::find_if(work->old.begin(), work->old.end(), [&](const auto& path) {
                    std::error_code error;
                    return _wcsicmp(path.c_str(), work->active.c_str()) != 0 &&
                        (work->active.empty() || !fs::equivalent(path, work->active, error));
                });
                if (victim == work->old.end() || !Owned(*victim) || !DeleteFileW(victim->c_str())) {
                    DeleteFileW(work->destination.c_str());
                    work->full = true;
                    return false;
                }
                auto sidecar = *victim; sidecar.replace_extension(L".emm");
                DeleteFileW(sidecar.c_str());
                work->old.erase(victim);
                work->overwritten = true;
            }
            return true;
        };
        options.completed = [app, work](bool success) {
            if (work->full) background_save::SetStatus(app, background_save::Status::Full);
            else if (success && work->overwritten)
                background_save::SetStatus(app, background_save::Status::Overwritten);
            nextScan = 0;
            if (success) runtime_log::WritePath("autosave", work->destination.c_str());
        };
        return background_save::Request(app, work->destination.c_str(), std::move(options));
    } catch (...) {
        background_save::SetStatus(app, background_save::Status::BackupFailed);
        runtime_log::Write("AUTOSAVE could not capture backup snapshot");
        return false;
    }
}
Result BackupNow(MMDApp* app, bool crash) noexcept {
    try {
        if (!Current().enabled && !crash) return Result::Disabled;
        if (!Ready(app, crash) || saving.exchange(true)) return Result::Busy;
        struct Release { ~Release() { saving.store(false); } } release;
        const fs::path directory = crash ? Defaults().directory : settings.directory;
        if (directory.empty()) return Result::Failed;
        fs::create_directories(directory);
        // Serialize instances sharing one folder; never wait inside a crash handler.
        const auto lockPath = directory / L".mms-autosave.lock";
        HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return Result::Busy;
        struct Unlock { HANDLE h; ~Unlock() { CloseHandle(h); } } unlock{lock};
        auto old = Backups(directory);
        if (old.size() >= settings.maxBackups && !settings.overwriteOldest) {
            if (!crash) RefreshCapacity(app);
            return Result::Full;
        }
        const auto destination = NewName(app, crash, directory);
        if (destination.empty() || !SaveSceneCopy(app, destination.c_str())) return Result::Failed;
        // Commit the new backup first. A failed save must never cost an old backup.
        // Also protect the currently opened project even if it is a recovery file.
        std::error_code ec;
        const fs::path active = *app->EnvFileName() ? fs::absolute(app->EnvFileName(), ec) : fs::path();
        while (old.size() >= settings.maxBackups) {
            auto victim = std::find_if(old.begin(), old.end(), [&](const auto& p) {
                std::error_code sameError;
                return _wcsicmp(p.c_str(), active.c_str()) != 0 &&
                    (active.empty() || !fs::equivalent(p, active, sameError));
            });
            if (victim == old.end() || !Owned(*victim) || !DeleteFileW(victim->c_str())) {
                DeleteFileW(destination.c_str());
                auto sidecar = destination; sidecar.replace_extension(L".emm");
                DeleteFileW(sidecar.c_str());
                if (!crash) RefreshCapacity(app);
                return Result::Full;
            }
            auto sidecar = *victim; sidecar.replace_extension(L".emm");
            DeleteFileW(sidecar.c_str());
            old.erase(victim);
        }
        NotifyEffectFileSave(destination.c_str());
        if (!crash) RefreshCapacity(app);
        runtime_log::WritePath(crash ? "crash_backup" : "autosave", destination.c_str());
        return Result::Saved;
    } catch (const std::exception& error) {
        runtime_log::Write("AUTOSAVE failed: %s", error.what());
        return Result::Failed;
    } catch (...) {
        runtime_log::Write("AUTOSAVE failed with an unknown exception");
        return Result::Failed;
    }
}
void Initialize(MMDApp* app) {
    Current(); mainThread = GetCurrentThreadId(); activeApp.store(app);
    nextSave = GetTickCount64() + ULONGLONG(settings.intervalMinutes) * 60000;
    nextScan = 0;
    runtime_log::SetCrashBackup(TryCrashBackup);
    RefreshMenu(app);
}
void Shutdown() noexcept {
    runtime_log::SetCrashBackup(nullptr);
    activeApp.store(nullptr);
}
void Tick(MMDApp* app, ULONGLONG now) noexcept {
    try {
        Current();
        if (now >= nextScan && !SceneSaveInProgress()) {
            nextScan = now + 5000;
            RefreshCapacity(app);
        }
        WarnIfFull(app);
        if (!settings.enabled || now < nextSave || !Ready(app, false)) return;
        nextSave = now + ULONGLONG(settings.intervalMinutes) * 60000;
        if (!QueueBackup(app)) runtime_log::Write("AUTOSAVE could not start backup");
        WarnIfFull(app);
    } catch (...) {
        runtime_log::Write("AUTOSAVE idle work deferred after an exception");
    }
}
void TryCrashBackup(unsigned long code) noexcept {
    // Stack/heap corruption, another thread, or a half-mutated scene cannot be
    // safely serialized. Completed periodic backups remain available in all cases.
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374UL || code == 0xC0000409UL ||
        GetCurrentThreadId() != mainThread || !activeApp.load() || saving.load()) {
        runtime_log::Write("CRASH_BACKUP skipped: unsafe exception, foreign thread, or unavailable scene");
        return;
    }
    CrashAttempt();
}
}
