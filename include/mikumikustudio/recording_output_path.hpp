#pragma once

#include <Windows.h>
#include <string>
#include <utility>

#include "mikumikustudio/ui_language.hpp"

namespace mikumikustudio::recording_output_path {

// Preserve existing output until the encoder itself starts writing. A new
// output keeps the empty placeholder used by the original recording path.
inline DWORD ProbeWritable(const wchar_t* path) noexcept {
    if (path == nullptr || *path == L'\0') return ERROR_INVALID_NAME;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    if (!CloseHandle(file)) return GetLastError();
    return ERROR_SUCCESS;
}

inline const wchar_t* FailureCaption(unsigned char language) noexcept {
    return language == kUiChinese ? L"视频导出失败"
         : language == kUiEnglish ? L"Video export failed"
                                  : L"動画出力に失敗しました";
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

} // namespace mikumikustudio::recording_output_path
