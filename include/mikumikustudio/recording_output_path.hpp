#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "mikumikustudio/ui_language.hpp"

namespace mikumikustudio::recording_output_path {

struct FileIdentity {
    DWORD volume;
    std::uint64_t index;
    std::uint64_t size;
    FILETIME modified;

    bool operator==(const FileIdentity& other) const noexcept {
        return volume == other.volume && index == other.index && size == other.size &&
            CompareFileTime(&modified, &other.modified) == 0;
    }
    bool operator!=(const FileIdentity& other) const noexcept { return !(*this == other); }
};

struct Selection {
    std::wstring path;
    std::optional<FileIdentity> existing;
};

inline DWORD ReadIdentity(HANDLE file, FileIdentity& identity, DWORD* attributes = nullptr) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return GetLastError();
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return ERROR_ACCESS_DENIED;
    identity = {info.dwVolumeSerialNumber,
        (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow, info.ftLastWriteTime};
    if (attributes) *attributes = info.dwFileAttributes;
    return ERROR_SUCCESS;
}

inline DWORD ReadSelection(const wchar_t* path, Selection& selected) {
    selected = {};
    if (!path || !*path) return ERROR_INVALID_NAME;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error || absolute.filename().empty()) return ERROR_INVALID_NAME;
    selected.path = absolute.native();
    const HANDLE file = CreateFileW(selected.path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD failure = GetLastError();
        return failure == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : failure;
    }
    FileIdentity identity{};
    const DWORD inspected = ReadIdentity(file, identity);
    CloseHandle(file);
    if (inspected == ERROR_SUCCESS) selected.existing = identity;
    return inspected;
}

inline bool SameSelection(const Selection& before, const Selection& current) noexcept {
    return before.path == current.path && before.existing == current.existing;
}

inline bool ReferencesSameFile(const wchar_t* path, const wchar_t* input) {
    if (!path || !*path || !input || !*input) return false;
    const auto identity = [](const wchar_t* name) -> std::optional<FileIdentity> {
        const HANDLE file = CreateFileW(name, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return std::nullopt;
        FileIdentity result{};
        const DWORD error = ReadIdentity(file, result);
        CloseHandle(file);
        return error == ERROR_SUCCESS ? std::optional<FileIdentity>(result) : std::nullopt;
    };
    const auto outputFile = identity(path), inputFile = identity(input);
    if (outputFile && inputFile)
        return outputFile->volume == inputFile->volume && outputFile->index == inputFile->index;

    // A decoder or external writer can deny opening a second handle. The
    // same normalized spelling remains protected while identity is unavailable.
    std::error_code outputError, inputError;
    const auto outputPath = std::filesystem::absolute(path, outputError).lexically_normal();
    const auto inputPath = std::filesystem::absolute(input, inputError).lexically_normal();
    return !outputError && !inputError &&
        CompareStringOrdinal(outputPath.c_str(), -1, inputPath.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Keep the original recording path's placeholder only for writers that need it.
inline DWORD ProbeWritable(const wchar_t* path, bool createPlaceholder = true) noexcept {
    if (path == nullptr || *path == L'\0') return ERROR_INVALID_NAME;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        createPlaceholder ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return !createPlaceholder && error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    if (!CloseHandle(file)) return GetLastError();
    return ERROR_SUCCESS;
}

inline const wchar_t* FailureCaption(unsigned char language) noexcept {
    return language == kUiChinese ? L"视频导出失败"
         : language == kUiEnglish ? L"Video export failed"
                                  : L"動画出力に失敗しました";
}

inline bool ProtectBackgroundVideo(HWND owner, const wchar_t* path, const wchar_t* background,
                                   unsigned char language) {
    if (!ReferencesSameFile(path, background)) return true;
    std::wstring message = language == kUiChinese
        ? L"所选输出会覆盖当前工程的背景视频。请选择其他输出文件。"
        : language == kUiEnglish
        ? L"This output would replace the current project's background video. Choose another output file."
        : L"現在のプロジェクトの背景動画は上書きできません。別の出力ファイルを選択してください。";
    message += L"\n\n";
    message += path;
    MessageBoxW(owner, message.c_str(), FailureCaption(language), MB_OK | MB_ICONWARNING);
    return false;
}

inline std::wstring FailureMessage(const wchar_t* path, DWORD error,
                                  unsigned char language) {
    std::wstring output = path != nullptr ? path : L"";
    if (!output.empty()) {
        const DWORD required = GetFullPathNameW(output.c_str(), 0, nullptr, nullptr);
        if (required != 0) {
            std::wstring full(required, L'\0');
            const DWORD length = GetFullPathNameW(output.c_str(), required, full.data(), nullptr);
            if (length != 0 && length < required) {
                full.resize(length);
                output = std::move(full);
            }
        }
    }
    const LANGID languageId = language == kUiChinese
        ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
        : language == kUiEnglish ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
                                : MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
    wchar_t reason[2048]{};
    constexpr DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (FormatMessageW(flags, nullptr, error, languageId, reason, 2048, nullptr) == 0)
        FormatMessageW(flags, nullptr, error, 0, reason, 2048, nullptr);
    const wchar_t* introduction = language == kUiChinese
        ? L"无法写入输出文件，视频导出未开始。\n\n输出路径：\n"
        : language == kUiEnglish
        ? L"Cannot write the output file. Video export was not started.\n\nOutput path:\n"
        : L"出力ファイルに書き込めないため、動画出力を開始できません。\n\n出力先：\n";
    std::wstring message = introduction;
    message += output;
    message += L"\n\nWin32 ";
    message += std::to_wstring(error);
    message += L": ";
    message += reason[0] != L'\0' ? reason
        : language == kUiChinese ? L"无法获取系统错误说明。"
        : language == kUiEnglish ? L"System error description is unavailable."
                                 : L"システムエラーの説明を取得できません。";
    return message;
}

inline void ReportChangedSelection(HWND owner, const wchar_t* path, unsigned char language) {
    std::wstring text = language == kUiChinese
        ? L"确认后输出文件已发生变化。请重新选择输出路径并确认。"
        : language == kUiEnglish
        ? L"The output file changed after confirmation. Choose the output path and confirm again."
        : L"確認後に出力ファイルが変更されました。出力先を選び直して確認してください。";
    text += L"\n\n";
    text += path;
    MessageBoxW(owner, text.c_str(), FailureCaption(language), MB_OK | MB_ICONWARNING);
}

} // namespace mikumikustudio::recording_output_path
