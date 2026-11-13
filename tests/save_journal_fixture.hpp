#pragma once

#include "../src/plugins/json.hpp"
#include <MinHook.h>

namespace save_journal_fixture {
inline mikumikustudio::plugins::Json Parse(std::string_view record) {
    using mikumikustudio::plugins::Json;
    const auto trailer = record.find("}MMS-SAVE-");
    if (trailer == record.npos) return Json::Parse(record);
    auto json = Json::Parse(record.substr(0, trailer + 1));
    const auto suffix = record.substr(trailer + 1);
    auto& decision = json.object.at("decision");
    if (suffix == "MMS-SAVE-FORWARD\n" && decision.String() == "rollback") {
        decision.text = "forward";
    } else if ((suffix == "MMS-SAVE-FORWARD\nMMS-SAVE-ROLLBACK\n" && decision.String() == "rollback") ||
               (suffix == "MMS-SAVE-ROLLBACK\n" && decision.String() == "forward")) {
        decision.text = "rollback";
    } else {
        throw std::runtime_error("unexpected decision trailer in fixture");
    }
    return json;
}

inline std::wstring journalPath;
inline HANDLE ready, parked;
inline DWORD timeout;
inline decltype(&FlushFileBuffers) originalFlush;

inline BOOL WINAPI Flush(HANDLE file) {
    const BOOL ok = originalFlush(file);
    const DWORD error = GetLastError();
    if (ok) {
        wchar_t path[32768];
        const DWORD length = GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
        if (length > 4 && length < 32768 && std::wstring_view(path + 4, length - 4) == journalPath) {
            SignalObjectAndWait(ready, parked, timeout, FALSE);
            ExitProcess(75);
        }
    }
    SetLastError(error);
    return ok;
}

inline void ArmDecisionBarrier(const std::filesystem::path& journal, HANDLE notification, HANDLE stop, DWORD wait) {
    journalPath = journal.native();
    ready = notification;
    parked = stop;
    timeout = wait;
    auto* api = reinterpret_cast<void*>(&FlushFileBuffers);
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(api, reinterpret_cast<void*>(&Flush), reinterpret_cast<void**>(&originalFlush)) != MH_OK ||
        MH_EnableHook(api) != MH_OK)
        throw std::runtime_error("hook recovery decision flush");
}

inline bool MatchesHandleRename(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, const void* data,
                                DWORD size, const std::filesystem::path& from, const std::filesystem::path& to) {
    if (type != FileRenameInfo || !data || size < sizeof(FILE_RENAME_INFO)) return false;
    const auto& rename = *static_cast<const FILE_RENAME_INFO*>(data);
    if (rename.RootDirectory || rename.FileNameLength % sizeof(wchar_t) ||
        rename.FileNameLength > size - offsetof(FILE_RENAME_INFO, FileName) ||
        std::wstring_view(rename.FileName, rename.FileNameLength / sizeof(wchar_t)) != to.native())
        return false;
    wchar_t path[32768];
    const DWORD length = GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
    if (!length || length >= 32768) return false;
    std::wstring_view source(path, length);
    constexpr std::wstring_view prefix = LR"(\\?\)";
    if (source.substr(0, prefix.size()) == prefix && from.native().substr(0, prefix.size()) != prefix)
        source.remove_prefix(prefix.size());
    return source == from.native();
}

namespace rename_barrier {
inline std::filesystem::path source, destination;
inline HANDLE reached, stop;
inline DWORD wait;
inline decltype(&MoveFileExW) originalMove;
inline decltype(&SetFileInformationByHandle) originalSet;
inline decltype(&CloseHandle) originalClose;
inline HANDLE renamedFile = INVALID_HANDLE_VALUE;

inline void After(bool match, BOOL ok, DWORD error) {
    if (match && ok) {
        SignalObjectAndWait(reached, stop, wait, FALSE);
        ExitProcess(75);
    }
    SetLastError(error);
}

inline BOOL WINAPI Move(LPCWSTR from, LPCWSTR to, DWORD flags) {
    const BOOL ok = originalMove(from, to, flags);
    const DWORD error = GetLastError();
    After(from && to && source.native() == from && destination.native() == to, ok, error);
    return ok;
}

inline BOOL WINAPI Set(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID data, DWORD size) {
    const DWORD incoming = GetLastError();
    const bool match = MatchesHandleRename(file, type, data, size, source, destination);
    SetLastError(incoming);
    const BOOL ok = originalSet(file, type, data, size);
    if (match && ok) renamedFile = file;
    return ok;
}

inline BOOL WINAPI Close(HANDLE file) {
    const BOOL ok = originalClose(file);
    const DWORD error = GetLastError();
    if (file == renamedFile) {
        renamedFile = INVALID_HANDLE_VALUE;
        // The parent can inspect the moved member once the real source handle closes.
        After(true, ok, error);
    }
    return ok;
}
}

inline void ArmRenameBarrier(const std::filesystem::path& from, const std::filesystem::path& to,
                             HANDLE notification, HANDLE parked, DWORD timeout) {
    using namespace rename_barrier;
    source = from;
    destination = to;
    reached = notification;
    stop = parked;
    wait = timeout;
    auto* move = reinterpret_cast<void*>(&MoveFileExW);
    auto* set = reinterpret_cast<void*>(&SetFileInformationByHandle);
    auto* close = reinterpret_cast<void*>(&CloseHandle);
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(move, reinterpret_cast<void*>(&Move), reinterpret_cast<void**>(&originalMove)) != MH_OK ||
        MH_CreateHook(set, reinterpret_cast<void*>(&Set), reinterpret_cast<void**>(&originalSet)) != MH_OK ||
        MH_CreateHook(close, reinterpret_cast<void*>(&Close), reinterpret_cast<void**>(&originalClose)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        throw std::runtime_error("hook completed archive rename");
}
}
