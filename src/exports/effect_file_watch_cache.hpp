#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "mikumikustudio/effect_file_watch.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace mikumikustudio::effect_file_watch {
using ReadTime = std::uint64_t(__fastcall*)(const char*);
inline std::wstring Fold(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), DWORD(value.size()));
    return value;
}
class Directory {
    HANDLE handle_ = INVALID_HANDLE_VALUE, event_ = nullptr;
    OVERLAPPED pending_{};
    alignas(DWORD) std::array<unsigned char, 32768> buffer_{};
    bool healthy_ = false, outstanding_ = false;
    ULONGLONG polled_ = 0;
    bool Arm() {
        ResetEvent(event_);
        pending_ = {}; pending_.hEvent = event_;
        const BOOL result = ReadDirectoryChangesW(handle_, buffer_.data(), DWORD(buffer_.size()), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_CREATION, nullptr, &pending_, nullptr);
        outstanding_ = result || GetLastError() == ERROR_IO_PENDING;
        return outstanding_;
    }
public:
    std::unordered_map<std::wstring, std::uint64_t> versions;
    explicit Directory(const std::wstring& path) {
        handle_ = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return;
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) return;
        pending_.hEvent = event_;
        healthy_ = Arm();
    }
    ~Directory() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (outstanding_) {
                CancelIoEx(handle_, &pending_);
                DWORD ignored = 0; GetOverlappedResult(handle_, &pending_, &ignored, TRUE);
            }
            CloseHandle(handle_);
        }
        if (event_) CloseHandle(event_);
    }
    bool Healthy() const { return healthy_; }
    void Poll(ULONGLONG now, Stats& stats) {
        if (!healthy_ || now - polled_ < 10) return;
        polled_ = now;
        const auto wait = WaitForSingleObject(event_, 0);
        if (wait == WAIT_TIMEOUT) return;
        if (wait != WAIT_OBJECT_0) { healthy_ = false; return; }
        DWORD bytes = 0;
        const BOOL completed = GetOverlappedResult(handle_, &pending_, &bytes, FALSE);
        outstanding_ = false;
        healthy_ = false; // An allocation failure while decoding must fall back.
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        if (!completed && error != ERROR_NOTIFY_ENUM_DIR) { healthy_ = false; return; }
        if (!bytes) {
            // Overflow is not "no changes": invalidate every registered file.
            for (auto& item : versions) ++item.second;
            ++stats.notifications;
        } else {
            size_t offset = 0;
            while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
                const auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
                if (item->FileNameLength % sizeof(wchar_t) ||
                    item->FileNameLength > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
                    for (auto& version : versions) ++version.second;
                    break;
                }
                const auto name = Fold(std::wstring(item->FileName, item->FileNameLength / sizeof(wchar_t)));
                if (auto found = versions.find(name); found != versions.end()) ++found->second;
                ++stats.notifications;
                if (!item->NextEntryOffset) break;
                if (item->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName) ||
                    item->NextEntryOffset > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
                    for (auto& version : versions) ++version.second;
                    break;
                }
                offset += item->NextEntryOffset;
            }
        }
        healthy_ = Arm();
    }
};

class Cache {
    struct File {
        std::shared_ptr<Directory> directory;
        std::wstring name;
        std::uint64_t version = 0, time = 0;
        ULONGLONG checked = 0, used = 0;
    };
    std::unordered_map<std::string, File> files_;
    std::unordered_map<std::wstring, std::weak_ptr<Directory>> directories_;
    Stats stats_;
    static std::wstring Resolve(const char* bytes) {
        // Same read-only ACP fallbacks as effect_paths.cpp. Resolve once when
        // registering, not once per timestamp poll. Relative paths are left to
        // MME because their meaning can change with the current directory.
        if (!bytes || !*bytes || !((bytes[1] == ':' && (bytes[2] == '\\' || bytes[2] == '/')) ||
            (bytes[0] == '\\' && bytes[1] == '\\'))) return {};
        for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u}) {
            const int length = MultiByteToWideChar(cp, 0, bytes, -1, nullptr, 0);
            if (length < 2) continue;
            std::wstring path(size_t(length), L'\0');
            if (!MultiByteToWideChar(cp, 0, bytes, -1, path.data(), length)) continue;
            path.pop_back();
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return path;
        }
        return {};
    }
    void Prune(ULONGLONG now) {
        if (files_.size() < 2048) return;
        for (auto it = files_.begin(); it != files_.end();) {
            if (now - it->second.used > 60000) {
                it->second.directory->versions.erase(it->second.name);
                it = files_.erase(it);
            } else ++it;
        }
        for (auto it = directories_.begin(); it != directories_.end();) {
            if (it->second.expired()) it = directories_.erase(it); else ++it;
        }
    }
public:
    std::uint64_t Get(const char* path, ReadTime read) {
        const ULONGLONG now = GetTickCount64();
        auto found = files_.find(path);
        if (found == files_.end()) {
            Prune(now);
            const auto resolved = files_.size() < 2048 ? Resolve(path) : std::wstring{};
            if (resolved.empty()) { ++stats_.reads; return read(path); }
            const auto folder = std::filesystem::path(resolved).parent_path().wstring();
            const auto folderKey = Fold(folder);
            if (directories_.size() >= 256) {
                for (auto it = directories_.begin(); it != directories_.end();) {
                    if (it->second.expired()) it = directories_.erase(it); else ++it;
                }
            }
            auto& weak = directories_[folderKey];
            auto directory = weak.lock();
            if (!directory) {
                if (directories_.size() > 256) { directories_.erase(folderKey); ++stats_.reads; return read(path); }
                directory = std::make_shared<Directory>(folder); weak = directory;
            }
            if (!directory->Healthy()) { ++stats_.reads; return read(path); }
            File file; file.directory = directory;
            file.name = Fold(std::filesystem::path(resolved).filename().wstring());
            file.version = directory->versions[file.name];
            // Arm before reading the timestamp, so a write racing registration
            // remains queued and invalidates this first value on the next poll.
            ++stats_.reads; file.time = read(path); file.checked = file.used = now;
            found = files_.emplace(path, std::move(file)).first;
            return found->second.time;
        }
        auto& file = found->second;
        file.used = now; file.directory->Poll(now, stats_);
        const auto version = file.directory->versions[file.name];
        // Failed/missing files keep the original retry behavior. A staggered
        // safety read also handles providers that silently lose notifications.
        const auto safety = 30000 + std::hash<std::string>{}(found->first) % 1000;
        if (!file.directory->Healthy() || !file.time || version != file.version || now - file.checked >= safety) {
            ++stats_.reads; file.time = read(path); file.version = version; file.checked = now;
        } else ++stats_.hits;
        return file.time;
    }
    Stats GetStats() const {
        auto result = stats_; result.files = unsigned(files_.size());
        for (const auto& pair : directories_) if (!pair.second.expired()) ++result.directories;
        return result;
    }
};
}
