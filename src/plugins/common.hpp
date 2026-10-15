#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mms/plugin.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mikudancestudio::plugins {
namespace fs = std::filesystem;
using Log = std::function<void(const std::string&)>;
template<class T> constexpr MmsHeader Header() { return {sizeof(T), 1, 0}; }
template<class T> bool Valid(const T* p) { return p && p->h.size >= sizeof(T) && p->h.major == 1 && p->h.minor == 0; }
inline MmsString Text(const std::string& s) { return {s.data(), s.size()}; }
inline std::string String(MmsString s) {
    if (s.bytes > 1024 * 1024 || (s.bytes && !s.data)) throw std::invalid_argument("string extent");
    std::string r(s.data ? s.data : "", static_cast<size_t>(s.bytes));
    if (r.find('\0') != std::string::npos) throw std::invalid_argument("embedded NUL");
    if (!r.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, r.data(), int(r.size()), nullptr, 0))
        throw std::invalid_argument("invalid UTF-8");
    return r;
}
inline std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n) throw std::invalid_argument("invalid UTF-8");
    std::wstring r(n, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n); return r;
}
inline std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::invalid_argument("invalid UTF-16");
    std::string r(n, 0); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n, nullptr, nullptr); return r;
}
inline bool ValidId(std::string_view s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
inline std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size())); return s; }
inline bool SamePath(const fs::path& a, const fs::path& b) { return Lower(a.lexically_normal().native()) == Lower(b.lexically_normal().native()); }
template<class F> MmsResult Boundary(F&& f) noexcept {
    try { return f(); } catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::invalid_argument&) { return MMS_E_ARGUMENT; }
    catch (...) { return MMS_E_FAILED; }
}
template<class T> MmsResult Table(uint32_t major, uint32_t minor, uint32_t bytes, void* out, const T& value) {
    if (!out || bytes < sizeof(T)) return MMS_E_ARGUMENT;
    if (major != 1 || minor > 0) return MMS_E_VERSION;
    std::memcpy(out, &value, sizeof(T)); return MMS_OK;
}
}
