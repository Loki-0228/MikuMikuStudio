#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cwchar>
#include "mikumikustudio/text_encoding.hpp"

namespace mikumikustudio {
// Generic legacy text. File paths use existence-based code page selection in
// ResolveAnsiUserFile; never dereference the original x86 locale-table offsets.
void ConvertAnsiToWide(void* localeTableBase, const char* text,
                       wchar_t* destination, int destinationWords) {
    (void)localeTableBase;
    if (!destination || destinationWords <= 0) return;
    destination[0] = L'\0';
    if (!text || !*text) return;
    std::wstring decoded;
    if (text_encoding::HasUtf8Bom(text)) {
        text_encoding::Decode(text + 3, CP_UTF8, decoded);
    } else {
        for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u})
            if (text_encoding::Decode(text, cp, decoded)) break;
    }
    wcsncpy_s(destination, destinationWords, decoded.c_str(), _TRUNCATE);
}
}
