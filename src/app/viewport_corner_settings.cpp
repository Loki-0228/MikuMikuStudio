// ===========================================================================
// 操作柄停靠角的程序目录设置。接口与约束见同名头文件。
//
// 文件读写自己做（CreateFileW/ReadFile + 临时文件 + MoveFileExW 原子替换）：
// 内容固定两行，解析规则严格，测试能直接写坏值验证回退。
//
// 解析规则（严格）：
//   * 只认 [viewport] 节内的 corner= 行；节名写在方括号里、单独成行。
//   * 值必须整行就是 tl/tr/bl/br 之一（大小写不敏感），前后只允许空白。
//     corner=tl1、corner=tl x 这类带尾巴的值一律判非法。
//   * 读不到合法值就回退默认右下，绝不猜。
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/local_data_path.hpp"
#include <Windows.h>

#include <mutex>
#include <string>

#include "mikudancestudio/viewport_corner_settings.hpp"

namespace mikudancestudio {
namespace viewport_corner_settings {
namespace {

std::mutex g_mutex;
Corner g_corner = viewport_tool_layout::kDefaultCorner;
bool g_loaded = false;

const wchar_t* Token(Corner corner) {
    switch (corner) {
    case Corner::TopLeft: return L"tl";
    case Corner::TopRight: return L"tr";
    case Corner::BottomLeft: return L"bl";
    case Corner::BottomRight: return L"br";
    }
    return L"br";
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 大小写不敏感的定长比较（两侧都是 ASCII）。
bool EqNoCase(const char* text, size_t length, const char* expect) {
    size_t i = 0;
    for (; i < length && expect[i] != '\0'; ++i)
        if (LowerAscii(text[i]) != expect[i])
            return false;
    return i == length && expect[i] == '\0';
}

// 值必须整行恰好是 tl/tr/bl/br：长度 2，后面没有尾巴。
bool TokenToCorner(const char* text, size_t length, Corner* out) {
    if (length != 2)
        return false;
    struct Entry { char a, b; Corner corner; };
    static constexpr Entry kEntries[] = {
        {'t', 'l', Corner::TopLeft}, {'t', 'r', Corner::TopRight},
        {'b', 'l', Corner::BottomLeft}, {'b', 'r', Corner::BottomRight}};
    for (const Entry& entry : kEntries) {
        if (LowerAscii(text[0]) == entry.a && LowerAscii(text[1]) == entry.b) {
            *out = entry.corner;
            return true;
        }
    }
    return false;
}

// 整体失败时 *out 保持默认：非法值不是"部分读到"，直接回退。
bool Parse(const char* data, size_t size, Corner* out) {
    size_t pos = 0;
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF)
        pos = 3;                                  // 跳过 UTF-8 BOM
    bool inViewportSection = false;
    while (pos < size) {
        size_t end = pos;
        while (end < size && data[end] != '\n' && data[end] != '\r')
            ++end;
        size_t begin = pos;
        size_t last = end;
        while (begin < last && IsSpace(data[begin]))
            ++begin;
        while (last > begin && IsSpace(data[last - 1]))
            --last;
        if (last > begin) {
            if (data[begin] == '[' && data[last - 1] == ']') {
                // 节名行：只认 [viewport]（大小写不敏感）。
                inViewportSection = EqNoCase(data + begin + 1,
                                             last - begin - 2, "viewport");
            } else if (inViewportSection) {
                size_t eq = begin;
                while (eq < last && data[eq] != '=')
                    ++eq;
                if (eq < last && EqNoCase(data + begin, eq - begin, "corner")) {
                    if (!TokenToCorner(data + eq + 1, last - (eq + 1), out))
                        return false;             // 值带尾巴等：整体回退
                    return true;
                }
            }
        }
        if (end >= size)
            break;
        pos = end + 1;
        if (data[end] == '\r' && pos < size && data[pos] == '\n')
            ++pos;
    }
    return false;
}

// 只在绝对路径上补目录：相对路径（含 %VAR% 展开失败时的字面量）不建目录。
bool LooksAbsolute(const std::wstring& dir) {
    if (dir.size() >= 3 && dir[1] == L':' &&
        (dir[2] == L'\\' || dir[2] == L'/'))
        return true;
    return dir.size() >= 2 && dir[0] == L'\\' && dir[1] == L'\\';
}

}  // namespace

std::wstring SettingsPath() {
    return local_data::File(L"viewport.ini").wstring();
}

bool LoadFrom(const std::wstring& path, Corner* out) {
    if (out == nullptr)
        return false;
    *out = viewport_tool_layout::kDefaultCorner;
    if (path.empty())
        return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;                      // 缺文件：默认值已就位
    char data[1024]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, data, sizeof(data) - 1, &read, nullptr);
    CloseHandle(file);
    if (ok == FALSE)
        return false;
    return Parse(data, static_cast<size_t>(read), out);
}

bool SaveTo(const std::wstring& path, Corner corner) {
    if (path.empty())
        return false;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos &&
        LooksAbsolute(path.substr(0, slash)))
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    const std::wstring temp = path + L".tmp";
    std::string body = "[viewport]\r\ncorner=";
    for (const wchar_t* p = Token(corner); *p != L'\0'; ++p)
        body.push_back(static_cast<char>(*p));
    body += "\r\n";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, body.data(),
                              static_cast<DWORD>(body.size()), &written,
                              nullptr);
    CloseHandle(file);
    if (ok == FALSE || written != body.size()) {
        DeleteFileW(temp.c_str());
        return false;
    }
    // 临时文件 + 原子替换，避免写一半的 ini 被下次启动读到。
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

Corner Current() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_loaded) {
        g_loaded = true;
        LoadFrom(SettingsPath(), &g_corner);
    }
    return g_corner;
}

bool Set(Corner corner) {
    bool saved = false;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_corner = corner;      // 本次运行立刻生效，与是否写盘无关
        g_loaded = true;
        saved = SaveTo(SettingsPath(), corner);
    }
    return saved;
}

void ResetCacheForTest(Corner corner) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_corner = corner;
    g_loaded = true;
}

}  // namespace viewport_corner_settings
}  // namespace mikudancestudio
