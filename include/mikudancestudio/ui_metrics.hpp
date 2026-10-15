#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace mikudancestudio::ui_metrics {
enum Counter : size_t { Layout, Paint, PaintPixels, FrameDriver, Scene, EffectBegin, Reset, Present, Count };
inline const wchar_t* Output() noexcept {
    static const auto path = [] { std::array<wchar_t, 32768> p{}; DWORD n = GetEnvironmentVariableW(L"MMS_UI_METRICS", p.data(), DWORD(p.size())); if (!n || n >= p.size()) p[0] = 0; return p; }();
    return path.data();
}
inline std::array<std::atomic<uint64_t>, Count> counts{};
inline void Add(Counter counter, uint64_t amount = 1) noexcept { if (*Output()) counts[counter].fetch_add(amount, std::memory_order_relaxed); }
inline void Write() noexcept {
    if (!*Output()) return;
    FILE* file = nullptr; if (_wfopen_s(&file, Output(), L"ab") || !file) return;
    constexpr const char* names[] = {"layout", "paint", "paintPixels", "frameDriver", "scene", "effectBegin", "reset", "present"};
    std::fputc('{', file);
    for (size_t i = 0; i < Count; ++i) std::fprintf(file, "%s\"%s\":%llu", i ? "," : "", names[i], static_cast<unsigned long long>(counts[i].load(std::memory_order_relaxed)));
    std::fputs("}\n", file); std::fclose(file);
}
}
