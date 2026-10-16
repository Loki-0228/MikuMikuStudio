#include "ui_preferences_file.hpp"
#include "ui_registry.hpp"
#include <atomic>

namespace mikudancestudio::plugins {
namespace {
constexpr size_t kMaxBytes = 1024 * 1024;
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(HANDLE h) : value(h) {}
    ~File() { Close(); }
    void Close() noexcept { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
    File(const File&) = delete;
};
struct Temporary {
    fs::path path;
    bool created = false;
    ~Temporary() { if (created) DeleteFileW(path.c_str()); }
};
bool Read(const fs::path& path, std::string& bytes) {
    File input(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return false;
        throw std::runtime_error("layout file open failed");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.value, &size) || size.QuadPart < 0 || size.QuadPart > kMaxBytes)
        throw std::invalid_argument("layout file extent");
    bytes.resize(size_t(size.QuadPart)); DWORD actual = 0;
    if (!::ReadFile(input.value, bytes.data(), DWORD(bytes.size()), &actual, nullptr) || actual != bytes.size())
        throw std::runtime_error("layout file read failed");
    return true;
}
}
MmsResult UiPreferencesFile::Load(UiRegistry& registry) {
    if (loaded_) return MMS_E_CONFLICT;
    loaded_ = true;
    return Boundary([&]() -> MmsResult {
        existed_ = Read(path_, baseline_);
        if (existed_) if (auto r = registry.ImportPreferences(baseline_)) return r;
        // With no file, allow start-time user preferences to be saved once.
        revision_ = existed_ ? registry.PreferencesRevision() : 0;
        writable_ = true; return MMS_OK;
    });
}
MmsResult UiPreferencesFile::Save(UiRegistry& registry) {
    if (!loaded_ || !writable_) return MMS_E_NOT_READY;
    if (registry.PreferencesRevision() == revision_) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        auto bytes = registry.ExportPreferences();
        if (bytes.size() > kMaxBytes) return MMS_E_MEMORY;
        fs::create_directories(path_.parent_path());
        auto lockPath = path_; lockPath += L".lock";
        File lock(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION ? MMS_E_BUSY : MMS_E_FAILED;
        std::string current;
        const bool exists = Read(path_, current);
        if (exists != existed_ || (exists && current != baseline_)) return MMS_E_CONFLICT;
        if (!existed_ || bytes != baseline_) {
            static std::atomic<uint64_t> sequence{0};
            Temporary temporary;
            temporary.path = path_.parent_path() / (path_.filename().wstring() + L"." +
                std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence) + L".tmp");
            File output(CreateFileW(temporary.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (output.value == INVALID_HANDLE_VALUE) return MMS_E_FAILED;
            temporary.created = true; DWORD actual = 0;
            if (!WriteFile(output.value, bytes.data(), DWORD(bytes.size()), &actual, nullptr) || actual != bytes.size() ||
                !FlushFileBuffers(output.value)) return MMS_E_FAILED;
            output.Close();
            if (!MoveFileExW(temporary.path.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return MMS_E_FAILED;
            temporary.created = false;
        }
        baseline_ = std::move(bytes); existed_ = true; revision_ = registry.PreferencesRevision();
        return MMS_OK;
    });
}
}
