#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/plugin_storage.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <io.h>
#include <share.h>

namespace mikudancestudio::autosave {
namespace fs = std::filesystem;
namespace {
Settings settings;
bool loaded = false, full = false;
std::atomic<bool> saving{false};
std::atomic<MMDApp*> activeApp{nullptr};
DWORD mainThread = 0;
ULONGLONG nextSave = 0, nextScan = 0;

fs::path UserDirectory() {
    wchar_t directory[32768]{};
    const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, 32768);
    return n && n < 32768 ? fs::path(directory) / L"MikuDanceStudio" : fs::path();
}
fs::path SettingsPath() {
    // Isolated integration tests use a disposable profile, never the user's.
    wchar_t overridePath[32768]{};
    const auto n = GetEnvironmentVariableW(L"MIKUDANCESTUDIO_AUTOSAVE_CONFIG", overridePath, 32768);
    if (n && n < 32768) return overridePath;
    const auto directory = UserDirectory();
    return directory.empty() ? fs::path() : directory / L"autosave.ini";
}
Settings Defaults() {
    Settings value;
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (length && length < 32768)
        value.directory = (fs::path(executable).parent_path() / L"backups").wstring();
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
bool Ready(MMDApp* app, bool crash) {
    return app && GetCurrentThreadId() == mainThread && IsWindow(app->MainWindow()) &&
        !app->SceneMutationInProgress() && !SceneSaveInProgress() && !bake::UiLocked() &&
        (crash || (IsWindowEnabled(app->MainWindow()) && !app->RecordingWindow() &&
            !app->PlaybackActive() && !app->FrameStepPlayback() && !GetCapture()));
}
fs::path NewName(MMDApp* app, bool crash) {
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
    const fs::path directory(settings.directory);
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
    __try { BackupNow(activeApp.load(), true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
}

Settings LoadSettings(const fs::path& path) {
    Settings value = Defaults();
    if (path.empty()) return value;
    wchar_t directory[32768]{};
    GetPrivateProfileStringW(L"autosave", L"directory", value.directory.c_str(), directory, 32768, path.c_str());
    value.directory = directory;
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
    const std::wstring body = L"\xfeff[autosave]\r\nenabled=" + std::to_wstring(value.enabled) +
        L"\r\nintervalMinutes=" + std::to_wstring(value.intervalMinutes) + L"\r\nmaxBackups=" +
        std::to_wstring(value.maxBackups) + L"\r\noverwriteOldest=" + std::to_wstring(value.overwriteOldest) +
        L"\r\ndirectory=" + value.directory + L"\r\n";
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
        // The folder serves both periodic and always-on crash backups. Validate
        // folder changes even with periodic saving off, but still allow turning
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
    try { SetFull(app, Backups(settings.directory).size() >= settings.maxBackups); }
    catch (...) { SetFull(app, false); }
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
Result BackupNow(MMDApp* app, bool crash) {
    if (!Current().enabled && !crash) return Result::Disabled;
    if (!Ready(app, crash) || saving.exchange(true)) return Result::Busy;
    struct Release { ~Release() { saving.store(false); } } release;
    try {
        fs::create_directories(settings.directory);
        // Serialize instances sharing one folder; never wait inside a crash handler.
        const auto lockPath = fs::path(settings.directory) / L".mms-autosave.lock";
        HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock == INVALID_HANDLE_VALUE) return Result::Busy;
        struct Unlock { HANDLE h; ~Unlock() { CloseHandle(h); } } unlock{lock};
        auto old = Backups(settings.directory);
        if (old.size() >= settings.maxBackups && !settings.overwriteOldest) {
            if (!crash) SetFull(app, true);
            return Result::Full;
        }
        const auto destination = NewName(app, crash);
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
            if (victim == old.end() || !Owned(*victim) || !plugins::DeletePluginBackup(victim->c_str())) {
                plugins::DeletePluginBackup(destination.c_str());
                if (!crash) SetFull(app, true);
                return Result::Full;
            }
            old.erase(victim);
        }
        if (!crash) SetFull(app, old.size() + 1 >= settings.maxBackups);
        runtime_log::WritePath(crash ? "crash_backup" : "autosave", destination.c_str());
        return Result::Saved;
    } catch (const std::exception& error) {
        runtime_log::Write("AUTOSAVE failed: %s", error.what());
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
void Tick(MMDApp* app, ULONGLONG now) {
    Current();
    if (now >= nextScan) {
        nextScan = now + 5000;
        try { SetFull(app, Backups(settings.directory).size() >= settings.maxBackups); }
        catch (...) { SetFull(app, false); }
    }
    if (!settings.enabled || now < nextSave || !Ready(app, false)) return;
    nextSave = now + ULONGLONG(settings.intervalMinutes) * 60000;
    const auto result = BackupNow(app);
    if (result == Result::Failed) runtime_log::Write("AUTOSAVE could not save backup");
}
void TryCrashBackup(unsigned long code) noexcept {
    // Stack/heap corruption, another thread, or a half-mutated scene cannot be
    // safely serialized. Completed periodic backups remain available in all cases.
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374UL || code == 0xC0000409UL ||
        GetCurrentThreadId() != mainThread || !activeApp.load() || saving.load()) return;
    CrashAttempt();
}
}
