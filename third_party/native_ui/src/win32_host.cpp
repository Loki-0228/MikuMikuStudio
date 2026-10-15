#include <native_ui/win32_host.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace native_ui {
namespace {
bool Current(HWND window, RECT& rect) {
    if (!GetWindowRect(window, &rect)) return false;
    MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&rect), 2);
    return true;
}
bool Equal(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
void Damage(HWND parent, RECT rect) { if (parent && IsWindow(parent)) RedrawWindow(parent, &rect, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); }
}
ApplyResult ApplyWindows(const std::vector<WindowMove>& moves) {
    std::vector<WindowMove> prior; prior.reserve(moves.size());
    std::map<HWND, std::vector<size_t>> groups;
    std::vector<HWND> parents; parents.reserve(moves.size());
    std::set<HWND> seen;
    for (const auto& move : moves) {
        if (!IsWindow(move.window) || GetWindowThreadProcessId(move.window, nullptr) != GetCurrentThreadId() ||
            !(GetWindowLongPtrW(move.window, GWL_STYLE) & WS_CHILD) || !seen.insert(move.window).second ||
            move.bounds.right < move.bounds.left || move.bounds.bottom < move.bounds.top) return ApplyResult::Restored;
        RECT old{}; if (!Current(move.window, old)) return ApplyResult::Restored;
        auto parent = GetParent(move.window); if (!parent) return ApplyResult::Restored;
        groups[parent].push_back(prior.size()); parents.push_back(parent); prior.push_back({move.window, old});
    }
    bool ok = true;
    for (const auto& [parent, children] : groups) {
        HDWP batch = BeginDeferWindowPos(int(children.size())); if (!batch) { ok = false; break; }
        for (size_t i : children) {
            const auto& move = moves[i]; const auto& r = move.bounds;
            batch = DeferWindowPos(batch, move.window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
            if (!batch) { ok = false; break; }
        }
        if (!batch || !EndDeferWindowPos(batch)) { ok = false; break; }
    }
    for (size_t i = 0; i < moves.size(); ++i) { const auto& move = moves[i]; RECT actual{}; if (GetParent(move.window) != parents[i] || !Current(move.window, actual) || !Equal(actual, move.bounds)) ok = false; }
    if (!ok) {
        bool restored = true;
        for (size_t i = 0; i < prior.size(); ++i) {
            const auto& move = prior[i];
            if (!IsWindow(move.window) || GetParent(move.window) != parents[i]) { restored = false; continue; }
            const auto& r = move.bounds;
            if (!SetWindowPos(move.window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW)) restored = false;
            RECT actual{}; if (!Current(move.window, actual) || !Equal(actual, r)) restored = false;
        }
        for (size_t i = 0; i < moves.size(); ++i) { Damage(parents[i], prior[i].bounds); Damage(parents[i], moves[i].bounds); }
        return restored ? ApplyResult::Restored : ApplyResult::Failed;
    }
    for (size_t i = 0; i < moves.size(); ++i) if (!Equal(prior[i].bounds, moves[i].bounds)) {
        RECT damage{}; UnionRect(&damage, &prior[i].bounds, &moves[i].bounds); Damage(GetParent(moves[i].window), damage);
    }
    return ApplyResult::Applied;
}
PanelBuffer::~PanelBuffer() { Reset(); }
void PanelBuffer::Reset() noexcept {
    if (memory_ && previous_) SelectObject(memory_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    memory_ = nullptr; bitmap_ = nullptr; previous_ = nullptr; width_ = height_ = 0; key_ = 0;
}
HDC PanelBuffer::Begin(HDC target, int width, int height, uint64_t key, COLORREF background) {
    if (width <= 0 || height <= 0 || uint64_t(width) * height > 64ull * 1024 * 1024) return nullptr;
    if (memory_ && width == width_ && height == height_ && key == key_) return memory_;
    HDC memory = CreateCompatibleDC(target); if (!memory) return nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(memory); return nullptr; }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) { DeleteObject(bitmap); DeleteDC(memory); return nullptr; }
    const uint32_t value = (GetRValue(background) << 16) | (GetGValue(background) << 8) | GetBValue(background);
    std::fill_n(static_cast<uint32_t*>(pixels), size_t(width) * height, value);
    Reset(); memory_ = memory; bitmap_ = bitmap; previous_ = previous; width_ = width; height_ = height; key_ = key; return memory_;
}
bool PanelBuffer::Copy(HDC target, const RECT& damage) const {
    if (!memory_) return false;
    RECT bounds{0, 0, width_, height_}, r{}; if (!IntersectRect(&r, &bounds, &damage)) return true;
    return BitBlt(target, r.left, r.top, r.right - r.left, r.bottom - r.top, memory_, r.left, r.top, SRCCOPY) != FALSE;
}
}
