#pragma once

#include "mikumikustudio/recording_output_path.hpp"

#include <cstring>
#include <new>
#include <vector>

namespace mikumikustudio::staged_output {
namespace detail {
struct OutputHandle {
    HANDLE value;
    ~OutputHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

inline std::vector<unsigned char> RenameInfo(const std::wstring& destination) {
    const auto nameBytes = destination.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename->FileName, destination.c_str(), nameBytes + sizeof(wchar_t));
    return buffer;
}
}

// All paths are absolute and on the same volume. The caller supplies a spare
// path in the output directory and owns cleanup of any unpublished staging file.
// The previous file is returned by path if restoration or retirement fails.
inline HRESULT Publish(const std::wstring& temporary, const std::wstring& destination,
                       const std::optional<recording_output_path::FileIdentity>& approved,
                       std::wstring spare, std::wstring& retained) noexcept {
    retained.clear();
    const auto publishNew = [&] {
        return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)
            ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    };
    if (!approved) return publishNew();

    try {
        // Allocate both rename buffers before any mutation. Pin and check the
        // approved file by handle while excluding other writes and renames.
        auto park = detail::RenameInfo(spare);
        auto restore = detail::RenameInfo(destination);
        const detail::OutputHandle previous{CreateFileW(destination.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (previous.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        recording_output_path::FileIdentity current{};
        DWORD attributes = 0;
        const DWORD inspected = recording_output_path::ReadIdentity(previous.value, current, &attributes);
        if (inspected != ERROR_SUCCESS) return HRESULT_FROM_WIN32(inspected);
        if (current != *approved) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        if (attributes & FILE_ATTRIBUTE_READONLY) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);

        if (!SetFileInformationByHandle(previous.value, FileRenameInfo, park.data(), static_cast<DWORD>(park.size())))
            return HRESULT_FROM_WIN32(GetLastError());
        const HRESULT result = publishNew();
        if (FAILED(result)) {
            if (!SetFileInformationByHandle(previous.value, FileRenameInfo, restore.data(), static_cast<DWORD>(restore.size())))
                retained.swap(spare);
            return result;
        }
        FILE_DISPOSITION_INFO dispose{TRUE};
        if (!SetFileInformationByHandle(previous.value, FileDispositionInfo, &dispose, sizeof(dispose)))
            retained.swap(spare);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
}
