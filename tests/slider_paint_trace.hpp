#pragma once
// Test-only observation. Every intercepted message reaches the installed real
// window procedure exactly once. Capturing pixels is diagnostic overhead; this
// module cannot measure normal frame latency or prove a visible flicker fixed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace slider_paint_trace {
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    explicit Dib(int w, int h, HDC compatible = nullptr) : width(w), height(h) {
        if (w <= 0 || h <= 0 || w > 2048 || h > 512) return;
        dc = CreateCompatibleDC(compatible); if (!dc) return;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (bitmap) old = SelectObject(dc, bitmap);
    }
    ~Dib() { if (old && old != HGDI_ERROR) SelectObject(dc, old); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
    Dib(const Dib&) = delete; Dib& operator=(const Dib&) = delete;
    explicit operator bool() const { return dc && pixels && old && old != HGDI_ERROR; }
};
struct Snapshot {
    HWND window = nullptr;
    int id = 0, width = 0, height = 0, tick = 0, clipKind = ERROR;
    bool copied = false, visible = false, borrowedDc = false;
    RECT thumb{}, channel{}, update{}, clip{};
    unsigned paints = 0, erases = 0;
    std::vector<std::uint32_t> pixels;
    bool Live() const noexcept { return copied && visible && !borrowedDc && clipKind != NULLREGION && clipKind != ERROR; }
    bool Varied() const noexcept {
        if (pixels.empty()) return false;
        const auto first = pixels.front() & 0xffffffu;
        for (auto pixel : pixels) if ((pixel & 0xffffffu) != first) return true;
        return false;
    }
    std::uint64_t Hash() const noexcept {
        std::uint64_t value = 1469598103934665603ull;
        for (auto pixel : pixels) { value ^= pixel & 0xffffffu; value *= 1099511628211ull; }
        return value;
    }
    bool ThumbUniform() const noexcept {
        const int left = (std::clamp)(int(thumb.left), 0, width), right = (std::clamp)(int(thumb.right), 0, width);
        const int top = (std::clamp)(int(thumb.top), 0, height), bottom = (std::clamp)(int(thumb.bottom), 0, height);
        if (!copied || left >= right || top >= bottom) return false;
        const auto first = pixels[std::size_t(top) * width + left] & 0xffffffu;
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x)
            if ((pixels[std::size_t(y) * width + x] & 0xffffffu) != first) return false;
        return true;
    }
};
struct PixelPair { Snapshot before, after; };
struct Event {
    std::uint64_t sequence = 0;
    LONGLONG begin = 0, end = 0;
    UINT message = 0;
    unsigned depth = 0;
    int dispatchId = 0;
    HWND sourceWindow = nullptr;
    LRESULT result = 0;
    bool parent = false, brushKnown = false;
    COLORREF brush = 0;
    std::string phase;
    std::vector<PixelPair> targets;
};
inline std::size_t Changed(const Snapshot& before, const Snapshot& after) noexcept {
    if (!before.copied || !after.copied || before.width != after.width || before.height != after.height) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < before.pixels.size(); ++i)
        count += (before.pixels[i] & 0xffffffu) != (after.pixels[i] & 0xffffffu);
    return count;
}
class Trace {
    struct Watch { Trace* trace = nullptr; HWND window = nullptr; bool parent = false; unsigned paints = 0, erases = 0; };
    std::array<Watch, 8> watches_{};
    HWND parent_ = nullptr;
    std::string phase_ = "setup";
    std::uint64_t next_ = 0;
    unsigned depth_ = 0;
    bool sampling_ = false, failed_ = false, truncated_ = false;
    std::size_t retained_ = 0;
    static constexpr std::size_t kBytes = 64 * 1024 * 1024, kEvents = 2048;
    std::vector<Event> events_;
    Watch* Find(HWND window) noexcept { for (auto& w : watches_) if (w.window == window) return &w; return nullptr; }
    static bool Interesting(bool parent, UINT msg) noexcept {
        if (msg == WM_PAINT || msg == WM_ERASEBKGND) return true;
        if (parent) return msg == WM_CTLCOLORSTATIC || msg == WM_HSCROLL;
        return msg == WM_PRINTCLIENT || msg == TBM_SETPOS || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
            msg == WM_MOUSEMOVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS;
    }
    Snapshot Capture(HWND window, HDC source = nullptr) {
        Snapshot out; out.window = window; out.id = GetDlgCtrlID(window); out.borrowedDc = source != nullptr;
        RECT client{}; if (!IsWindow(window) || !GetClientRect(window, &client)) return out;
        out.width = client.right; out.height = client.bottom; out.visible = IsWindowVisible(window) != FALSE;
        out.tick = int(SendMessageW(window, TBM_GETPOS, 0, 0));
        SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&out.thumb));
        SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&out.channel));
        GetUpdateRect(window, &out.update, FALSE);
        if (const auto* watch = Find(window)) { out.paints = watch->paints; out.erases = watch->erases; }
        if (out.width <= 0 || out.height <= 0 || out.width > 2048 || out.height > 512) return out;
        const auto bytes = std::size_t(out.width) * out.height * sizeof(std::uint32_t);
        if (retained_ > kBytes || bytes > kBytes - retained_) { truncated_ = true; return out; }
        const HDC dc = source ? source : GetDC(window); if (!dc) return out;
        struct Release { HWND window; HDC dc; bool owned; ~Release() { if (owned) ReleaseDC(window, dc); } } release{window, dc, !source};
        out.clipKind = GetClipBox(dc, &out.clip);
        Dib bitmap(out.width, out.height, dc);
        if (bitmap && BitBlt(bitmap.dc, 0, 0, out.width, out.height, dc, 0, 0, SRCCOPY)) {
            GdiFlush(); out.pixels.assign(bitmap.pixels, bitmap.pixels + std::size_t(out.width) * out.height);
            out.copied = true; retained_ += bytes;
        }
        return out;
    }
    void Before(Event& event, Watch& watch, UINT msg, WPARAM wp, LPARAM lp) {
        event.sequence = ++next_; event.depth = depth_; event.parent = watch.parent;
        event.dispatchId = watch.parent ? 0 : GetDlgCtrlID(watch.window);
        event.message = msg; event.phase = phase_; LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); event.begin = tick.QuadPart;
        HWND source = watch.window;
        if (watch.parent && (msg == WM_HSCROLL || msg == WM_CTLCOLORSTATIC)) source = reinterpret_cast<HWND>(lp);
        event.sourceWindow = source;
        RECT sourceBounds{}; GetWindowRect(source, &sourceBounds);
        for (auto& target : watches_) if (target.window && !target.parent) {
            RECT bounds{}, overlap{}; GetWindowRect(target.window, &bounds);
            const bool all = watch.parent && (msg == WM_PAINT || msg == WM_ERASEBKGND);
            if (!all && target.window != source && !IntersectRect(&overlap, &bounds, &sourceBounds)) continue;
            HDC dc = nullptr;
            if (target.window == source && ((!watch.parent && (msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT)) ||
                (watch.parent && msg == WM_CTLCOLORSTATIC))) dc = reinterpret_cast<HDC>(wp);
            PixelPair pair; pair.before = Capture(target.window, dc); event.targets.push_back(std::move(pair));
        }
    }
    void After(Event& event, WPARAM wp) {
        for (auto& pair : event.targets) {
            HDC dc = nullptr;
            const HWND source = event.sourceWindow;
            if (pair.before.window == source && ((!event.parent && (event.message == WM_ERASEBKGND || event.message == WM_PRINTCLIENT)) ||
                (event.parent && event.message == WM_CTLCOLORSTATIC))) dc = reinterpret_cast<HDC>(wp);
            pair.after = Capture(pair.before.window, dc);
        }
        if (event.parent && event.message == WM_CTLCOLORSTATIC && event.result) {
            LOGBRUSH brush{};
            if (GetObjectW(reinterpret_cast<HGDIOBJ>(event.result), sizeof(brush), &brush) == sizeof(brush) && brush.lbStyle == BS_SOLID) {
                event.brushKnown = true; event.brush = brush.lbColor;
            }
        }
        LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); event.end = tick.QuadPart;
    }
    static LRESULT CALLBACK Observe(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& watch = *reinterpret_cast<Watch*>(data); auto& trace = *watch.trace;
        if (trace.sampling_ || !Interesting(watch.parent, msg)) return DefSubclassProc(window, msg, wp, lp);
        if (!watch.parent && msg == WM_PAINT) ++watch.paints;
        if (!watch.parent && msg == WM_ERASEBKGND) ++watch.erases;
        if (trace.events_.size() >= kEvents) { trace.truncated_ = true; return DefSubclassProc(window, msg, wp, lp); }
        Event event; bool captured = false;
        try { trace.sampling_ = true; trace.Before(event, watch, msg, wp, lp); captured = true; }
        catch (...) { trace.failed_ = true; }
        trace.sampling_ = false; ++trace.depth_;
        const auto result = DefSubclassProc(window, msg, wp, lp);
        --trace.depth_;
        if (captured) try {
            trace.sampling_ = true; event.result = result; trace.After(event, wp); trace.events_.push_back(std::move(event));
        } catch (...) { trace.failed_ = true; }
        trace.sampling_ = false; return result;
    }
    static void Bitmap(const std::filesystem::path& path, const Snapshot& value) {
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{}; file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + DWORD(value.pixels.size() * sizeof(std::uint32_t));
        info.biSize = sizeof(info); info.biWidth = value.width; info.biHeight = -value.height; info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
        stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
        stream.write(reinterpret_cast<const char*>(value.pixels.data()), std::streamsize(value.pixels.size() * sizeof(std::uint32_t)));
        Require(bool(stream), "write slider paint evidence bitmap");
    }
public:
    explicit Trace(HWND parent, const std::array<HWND, 7>& controls) : parent_(parent) {
        events_.reserve(256);
        try {
            for (std::size_t i = 0; i < watches_.size(); ++i) {
                auto& w = watches_[i]; w.trace = this; w.window = i < controls.size() ? controls[i] : parent; w.parent = i == controls.size();
                Require(w.window && SetWindowSubclass(w.window, Observe, reinterpret_cast<UINT_PTR>(&w), reinterpret_cast<DWORD_PTR>(&w)),
                    "install slider paint observer over the actual native procedure");
            }
        } catch (...) { Detach(); throw; }
    }
    ~Trace() { Detach(); }
    Trace(const Trace&) = delete; Trace& operator=(const Trace&) = delete;
    void Detach() noexcept { for (auto& w : watches_) if (IsWindow(w.window)) RemoveWindowSubclass(w.window, Observe, reinterpret_cast<UINT_PTR>(&w)); }
    void Phase(const std::string& value) { phase_ = value; }
    Snapshot Read(HWND window, HDC dc = nullptr) { sampling_ = true;
        try { auto result = Capture(window, dc); sampling_ = false; return result; }
        catch (...) { sampling_ = false; throw; } }
    const std::vector<Event>& Events() const noexcept { return events_; }
    bool Complete() const noexcept { return !failed_ && !truncated_; }
    void Write(const std::filesystem::path& directory) const {
        std::filesystem::create_directories(directory); std::ofstream csv(directory / "paint-trace.csv");
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        csv << "# real native forwarding; capture overhead invalidates performance comparisons; qpc_frequency=" << frequency.QuadPart
            << "; failed=" << failed_ << "; truncated=" << truncated_ << "\n";
        csv << "seq,depth,phase,msg,dispatch_id,target_id,parent,begin,end,result,borrowed_dc,live,varied_before,varied_after,"
               "tick_before,tick_after,paint_before,paint_after,erase_before,erase_after,changed_pixels,thumb_uniform_before,thumb_uniform_after,"
               "hash_before,hash_after,update_l,update_t,update_r,update_b,clip_kind,clip_l,clip_t,clip_r,clip_b,brush_known,brush_rgb\n";
        std::array<unsigned, 7> images{};
        for (const auto& event : events_) for (const auto& pair : event.targets) {
            const auto& a = pair.before; const auto& b = pair.after; const auto changed = Changed(a, b);
            csv << event.sequence << ',' << event.depth << ',' << event.phase << ',' << event.message << ',' << event.dispatchId << ',' << a.id
                << ',' << event.parent << ',' << event.begin << ',' << event.end << ',' << event.result << ',' << a.borrowedDc << ',' << (a.Live() && b.Live())
                << ',' << a.Varied() << ',' << b.Varied() << ',' << a.tick << ',' << b.tick << ',' << a.paints << ',' << b.paints << ',' << a.erases << ',' << b.erases
                << ',' << changed << ',' << a.ThumbUniform() << ',' << b.ThumbUniform() << ',' << a.Hash() << ',' << b.Hash()
                << ',' << a.update.left << ',' << a.update.top << ',' << a.update.right << ',' << a.update.bottom << ',' << a.clipKind
                << ',' << a.clip.left << ',' << a.clip.top << ',' << a.clip.right << ',' << a.clip.bottom << ',' << event.brushKnown << ',' << event.brush << '\n';
            std::size_t control = 0; while (control < images.size() && GetDlgCtrlID(watches_[control].window) != a.id) ++control;
            const bool controlled = !event.parent && a.window == event.sourceWindow &&
                event.phase.find("-controlled-native-") != std::string::npos &&
                (event.message == WM_ERASEBKGND || event.message == WM_PRINTCLIENT);
            if (a.copied && b.copied && (controlled || (control < images.size() && images[control] < 4 &&
                (changed || (!event.parent && event.message == WM_PAINT))))) {
                const auto name = std::to_string(event.sequence) + "-" + std::to_string(a.id);
                Bitmap(directory / (name + "-before.bmp"), a); Bitmap(directory / (name + "-after.bmp"), b);
                if (!controlled && control < images.size()) ++images[control];
            }
        }
        Require(bool(csv), "write slider paint trace");
    }
};
} // namespace slider_paint_trace
