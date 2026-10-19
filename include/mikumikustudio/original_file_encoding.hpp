#pragma once

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <Windows.h>
#include <cstring>
#include <string>

namespace mikumikustudio::original_file_encoding {
// Original MMD fields are CP932 byte strings. Keep full-width fixed fields
// distinct from strings that reserve a terminating byte; never truncate or
// fall back to the machine ACP/UTF-8.
inline bool Encode(const wchar_t* text, std::size_t maxBytes, std::string& bytes) {
    if (!text_encoding::EncodeExact(text, 932, bytes) || bytes.size() > maxBytes) {
        bytes.clear();
        return false;
    }
    return true;
}

inline bool Name(const char* raw, std::size_t capacity, const wchar_t* unicode,
                 std::size_t maxBytes, std::string& bytes) {
    bytes.clear();
    if (unicode) return Encode(unicode, maxBytes, bytes);
    const std::string bounded(raw, strnlen_s(raw, capacity));
    std::wstring wide;
    if (text_encoding::HasUtf8Bom(bounded.c_str()) ||
        !text_encoding::Decode(bounded.c_str(), 932, wide)) return false;
    // A valid legacy byte alias must retain its original identity bytes.
    if (bounded.size() > maxBytes) return false;
    bytes = bounded;
    return true;
}

inline bool Fixed(char* destination, std::size_t capacity, const wchar_t* text) {
    std::string bytes;
    if (!capacity || !Encode(text, capacity - 1, bytes)) return false;
    std::memset(destination, 0, capacity);
    std::memcpy(destination, bytes.data(), bytes.size());
    return true;
}

inline void Refuse(void* owner, unsigned language, const wchar_t* format) {
    runtime_log::Write("ORIGINAL_PROFILE save refused: field is not exact CP932 or exceeds its byte capacity");
    const wchar_t* messages[] = {
        L"名前または参照パスを元の MMD の CP932 フィールドに保存できません。文字と長さを確認してください。既存のファイルは変更していません。",
        L"A name or referenced path cannot fit an original MMD CP932 field exactly. Check its characters and length. The existing file was not changed.",
        L"名称或引用路径无法完整写入原版 MMD 的 CP932 字段。请检查字符和长度，已有文件未改动。"};
    if (owner) MessageBoxW(static_cast<HWND>(owner), messages[language <= 2 ? language : 1],
                           format, MB_OK | MB_ICONERROR);
    SetLastError(ERROR_NO_UNICODE_TRANSLATION);
}
}
