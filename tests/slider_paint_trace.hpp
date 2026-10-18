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
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
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
    DWORD dcType = 0;
    HWND dcWindow = nullptr;
    POINT viewportOrigin{}, windowOrigin{}, deviceOrigin{};
    POINT thumbDevice[2]{};
    int mapMode = 0, bitmapWidth = 0, bitmapHeight = 0;
    bool mappedThumb = false;
    std::uint64_t paintSequence = 0;
    unsigned paints = 0, erases = 0;
    std::vector<std::uint32_t> pixels;
    bool Live() const noexcept { return copied && visible && !borrowedDc && clipKind != NULLREGION && clipKind != ERROR; }
    bool ThumbReadable() const noexcept {
        if (!copied || mapMode != MM_TEXT || clipKind != SIMPLEREGION || thumb.left < 0 || thumb.top < 0 ||
            thumb.right > width || thumb.bottom > height || thumb.left >= thumb.right || thumb.top >= thumb.bottom ||
            clip.left > thumb.left || clip.top > thumb.top || clip.right < thumb.right || clip.bottom < thumb.bottom) return false;
        if (dcType != OBJ_MEMDC) return true;
        return mappedThumb && thumbDevice[0].x >= 0 && thumbDevice[0].y >= 0 &&
            thumbDevice[1].x > thumbDevice[0].x && thumbDevice[1].y > thumbDevice[0].y &&
            thumbDevice[1].x <= bitmapWidth && thumbDevice[1].y <= bitmapHeight;
    }
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
    int dispatchId = 0, sourceId = 0;
    HWND sourceWindow = nullptr;
    LRESULT result = 0, nativeResult = 0;
    bool parent = false, brushKnown = false, customDraw = false, diagnosticItems = false;
    UINT notifyCode = 0, drawStage = 0, itemState = 0;
    UINT_PTR item = 0;
    RECT drawRect{};
    HDC suppliedDc = nullptr;
    COLORREF brush = 0;
    std::string phase;
    std::vector<PixelPair> targets;
};
// A separate, allocation-free notification audit. No pixel/GDI/window query is
// made while collecting these records, including the final-return hook.
struct MetadataEvent {
    std::uint64_t sequence = 0;
    HWND source = nullptr;
    LPARAM argument = 0;
    int passId = 0, id = 0;
    UINT stage = 0, itemState = 0;
    UINT_PTR item = 0;
    LRESULT nativeResult = 0, returnedResult = 0, finalResult = 0;
    bool returned = false, finalObserved = false;
};
inline std::size_t Changed(const Snapshot& before, const Snapshot& after) noexcept {
    if (!before.copied || !after.copied || before.width != after.width || before.height != after.height) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < before.pixels.size(); ++i)
        count += (before.pixels[i] & 0xffffffu) != (after.pixels[i] & 0xffffffu);
    return count;
}
inline std::size_t ChangedIn(const Snapshot& before, const Snapshot& after, RECT rect) noexcept {
    if (!before.copied || !after.copied || before.width != after.width || before.height != after.height) return 0;
    const int left = (std::clamp)(int(rect.left), 0, before.width), right = (std::clamp)(int(rect.right), 0, before.width);
    const int top = (std::clamp)(int(rect.top), 0, before.height), bottom = (std::clamp)(int(rect.bottom), 0, before.height);
    std::size_t changed = 0;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const auto index = std::size_t(y) * before.width + x;
        changed += (before.pixels[index] & 0xffffffu) != (after.pixels[index] & 0xffffffu);
    }
    return changed;
}
inline const char* DrawStageName(UINT stage) noexcept {
    switch (stage) {
    case CDDS_PREPAINT: return "prepaint";
    case CDDS_POSTPAINT: return "postpaint";
    case CDDS_ITEMPREPAINT: return "item-prepaint";
    case CDDS_ITEMPOSTPAINT: return "item-postpaint";
    case CDDS_PREERASE: return "preerase";
    case CDDS_POSTERASE: return "posterase";
    default: return "other";
    }
}
class Trace {
    struct Watch { Trace* trace = nullptr; HWND window = nullptr; bool parent = false; unsigned paints = 0, erases = 0; std::uint64_t paintSequence = 0; };
    std::array<Watch, 8> watches_{};
    HWND parent_ = nullptr;
    std::string phase_ = "setup";
    std::uint64_t next_ = 0;
    unsigned depth_ = 0;
    bool sampling_ = false, failed_ = false, truncated_ = false, diagnosticItems_ = false, queuedUiMessages_ = false;
    std::size_t retained_ = 0;
    static constexpr std::size_t kBytes = 64 * 1024 * 1024, kEvents = 2048;
    std::vector<Event> events_;
    struct MetadataStorage { std::array<MetadataEvent, kEvents> events{}; std::size_t count = 0; };
    // Allocate this fixed buffer before any observer or return hook is installed.
    std::unique_ptr<MetadataStorage> metadata_ = std::make_unique<MetadataStorage>();
    bool metadataOnly_ = false;
    int metadataPassId_ = 0;
    HHOOK returnHook_ = nullptr;
    inline static thread_local Trace* returnAudit_ = nullptr;
    Watch* Find(HWND window) noexcept { for (auto& w : watches_) if (w.window == window) return &w; return nullptr; }
    static LRESULT ItemResult(UINT stage, LRESULT nativeResult) noexcept {
        if (nativeResult != CDRF_DODEFAULT) return nativeResult;
        if (stage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (stage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
        return nativeResult;
    }
    static LRESULT CALLBACK Returned(int code, WPARAM wp, LPARAM lp) noexcept {
        auto* trace = returnAudit_;
        if (code >= 0 && trace && trace->metadataOnly_) {
            const auto& returned = *reinterpret_cast<const CWPRETSTRUCT*>(lp);
            if (returned.hwnd == trace->parent_ && returned.message == WM_NOTIFY && returned.lParam) {
                const auto& header = *reinterpret_cast<const NMHDR*>(returned.lParam);
                const auto* source = trace->Find(header.hwndFrom);
                if (header.code == UINT(NM_CUSTOMDRAW) && source && !source->parent) {
                    const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(returned.lParam);
                    for (std::size_t n = trace->metadata_->count; n; --n) {
                        auto& event = trace->metadata_->events[n - 1];
                        if (!event.finalObserved && event.returned && event.argument == returned.lParam &&
                            event.source == header.hwndFrom && event.stage == draw.dwDrawStage) {
                            event.finalResult = returned.lResult; event.finalObserved = true; break;
                        }
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    LRESULT MetadataMessage(Watch& watch, UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (!watch.parent && message == WM_PAINT) ++watch.paints;
        if (!watch.parent && message == WM_ERASEBKGND) ++watch.erases;
        MetadataEvent* event = nullptr;
        UINT stage = 0;
        bool customDraw = false;
        if (watch.parent && message == WM_NOTIFY && lp) {
            const auto& header = *reinterpret_cast<const NMHDR*>(lp);
            const auto* source = Find(header.hwndFrom);
            if (header.code == UINT(NM_CUSTOMDRAW) && source && !source->parent) {
                const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lp);
                stage = draw.dwDrawStage; customDraw = true;
                if (metadata_->count == metadata_->events.size()) truncated_ = true;
                else {
                    event = &metadata_->events[metadata_->count++];
                    event->sequence = ++next_; event->source = header.hwndFrom; event->argument = lp;
                    event->passId = metadataPassId_; event->id = int(header.idFrom);
                    event->stage = stage; event->item = draw.dwItemSpec; event->itemState = draw.uItemState;
                }
            }
        }
        const auto nativeResult = DefSubclassProc(watch.window, message, wp, lp);
        const auto result = customDraw ? ItemResult(stage, nativeResult) : nativeResult;
        if (event) { event->nativeResult = nativeResult; event->returnedResult = result; event->returned = true; }
        return result;
    }
    static bool Interesting(bool parent, UINT msg) noexcept {
        if (msg == WM_PAINT || msg == WM_ERASEBKGND) return true;
        if (parent) return (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) || msg == WM_HSCROLL || msg == WM_NOTIFY;
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
        if (const auto* watch = Find(window)) { out.paints = watch->paints; out.erases = watch->erases; out.paintSequence = watch->paintSequence; }
        if (out.width <= 0 || out.height <= 0 || out.width > 2048 || out.height > 512) return out;
        const auto bytes = std::size_t(out.width) * out.height * sizeof(std::uint32_t);
        if (retained_ > kBytes || bytes > kBytes - retained_) { truncated_ = true; return out; }
        const HDC dc = source ? source : GetDC(window); if (!dc) return out;
        struct Release { HWND window; HDC dc; bool owned; ~Release() { if (owned) ReleaseDC(window, dc); } } release{window, dc, !source};
        out.dcType = GetObjectType(dc); out.dcWindow = WindowFromDC(dc);
        GetViewportOrgEx(dc, &out.viewportOrigin); GetWindowOrgEx(dc, &out.windowOrigin); GetDCOrgEx(dc, &out.deviceOrigin);
        out.mapMode = GetMapMode(dc); out.thumbDevice[0] = {out.thumb.left,out.thumb.top}; out.thumbDevice[1] = {out.thumb.right,out.thumb.bottom};
        out.mappedThumb = LPtoDP(dc,out.thumbDevice,2) != FALSE;
        if(out.dcType == OBJ_MEMDC){BITMAP selected{};
            if(GetObjectW(GetCurrentObject(dc,OBJ_BITMAP),sizeof(selected),&selected)==sizeof(selected)){
                out.bitmapWidth=selected.bmWidth;out.bitmapHeight=selected.bmHeight;
            }
        }
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
        event.diagnosticItems = diagnosticItems_;
        HWND source = watch.window;
        const bool ctlColor = watch.parent && msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC;
        if (watch.parent && (msg == WM_HSCROLL || ctlColor)) source = reinterpret_cast<HWND>(lp);
        if (watch.parent && msg == WM_NOTIFY) {
            const auto& header = *reinterpret_cast<const NMHDR*>(lp); source = header.hwndFrom;
            event.notifyCode = header.code; event.dispatchId = int(header.idFrom);
            if (header.code == UINT(NM_CUSTOMDRAW)) {
                const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lp); event.customDraw = true;
                event.drawStage = draw.dwDrawStage; event.item = draw.dwItemSpec; event.itemState = draw.uItemState;
                event.drawRect = draw.rc; event.suppliedDc = draw.hdc;
            }
        }
        if (!watch.parent && msg == WM_PAINT) watch.paintSequence = event.sequence;
        if ((!watch.parent && (msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT)) || ctlColor)
            event.suppliedDc = reinterpret_cast<HDC>(wp);
        event.sourceWindow = source;
        event.sourceId = GetDlgCtrlID(source);
        RECT sourceBounds{}; GetWindowRect(source, &sourceBounds);
        for (auto& target : watches_) if (target.window && !target.parent) {
            RECT bounds{}, overlap{}; GetWindowRect(target.window, &bounds);
            const bool all = watch.parent && (msg == WM_PAINT || msg == WM_ERASEBKGND);
            if (!all && target.window != source && !IntersectRect(&overlap, &bounds, &sourceBounds)) continue;
            // Keep the actual window pixels separate from any supplied theme
            // buffer. OBJ_MEMDC is not evidence that these pixels were shown.
            PixelPair pair; pair.before = Capture(target.window); event.targets.push_back(std::move(pair));
            if (target.window == source && event.suppliedDc) {
                PixelPair supplied; supplied.before = Capture(target.window, event.suppliedDc); event.targets.push_back(std::move(supplied));
            }
        }
    }
    void After(Event& event) {
        for (auto& pair : event.targets) {
            pair.after = Capture(pair.before.window, pair.before.borrowedDc ? event.suppliedDc : nullptr);
        }
        if (event.parent && event.message >= WM_CTLCOLORMSGBOX && event.message <= WM_CTLCOLORSTATIC && event.nativeResult) {
            LOGBRUSH brush{};
            if (GetObjectW(reinterpret_cast<HGDIOBJ>(event.nativeResult), sizeof(brush), &brush) == sizeof(brush) && brush.lbStyle == BS_SOLID) {
                event.brushKnown = true; event.brush = brush.lbColor;
            }
        }
        LARGE_INTEGER tick{}; QueryPerformanceCounter(&tick); event.end = tick.QuadPart;
    }
    static LRESULT CALLBACK Observe(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& watch = *reinterpret_cast<Watch*>(data); auto& trace = *watch.trace;
        if (trace.metadataOnly_) return trace.MetadataMessage(watch, msg, wp, lp);
        const bool queuedUi=trace.queuedUiMessages_&&watch.parent&&(msg==WM_APP+0x6a||msg==WM_TIMER);
        if (trace.sampling_ || (!Interesting(watch.parent, msg)&&!queuedUi)) return DefSubclassProc(window, msg, wp, lp);
        if (watch.parent && msg == WM_NOTIFY) {
            if (!lp) return DefSubclassProc(window, msg, wp, lp);
            const auto* source = trace.Find(reinterpret_cast<const NMHDR*>(lp)->hwndFrom);
            if (!source || source->parent) return DefSubclassProc(window, msg, wp, lp);
        }
        if (!watch.parent && msg == WM_PAINT) ++watch.paints;
        if (!watch.parent && msg == WM_ERASEBKGND) ++watch.erases;
        if (trace.events_.size() >= kEvents) { trace.truncated_ = true; return DefSubclassProc(window, msg, wp, lp); }
        Event event; bool captured = false;
        try { trace.sampling_ = true; trace.Before(event, watch, msg, wp, lp); captured = true; }
        catch (...) { trace.failed_ = true; }
        trace.sampling_ = false; ++trace.depth_;
        const auto nativeResult = DefSubclassProc(window, msg, wp, lp);
        auto result = nativeResult;
        // A separate, explicitly labelled diagnostic asks only for additional
        // notifications. The unmodified baseline never enters this branch.
        // The native handler still runs once and every item draws by default.
        // Ask for the documented item-draw value alone at PREPAINT. The actual
        // themed trackbars returned only POSTPAINT when this diagnostic used
        // NOTIFYITEMDRAW | NOTIFYPOSTPAINT (0x30); that combined request did not
        // expose any item stages. Request item postpaint at ITEMPREPAINT instead.
        if (captured && event.customDraw && event.diagnosticItems && nativeResult == CDRF_DODEFAULT) {
            result = ItemResult(event.drawStage, nativeResult);
        }
        --trace.depth_;
        if (captured) try {
            trace.sampling_ = true; event.nativeResult = nativeResult; event.result = result; trace.After(event); trace.events_.push_back(std::move(event));
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
        events_.reserve(kEvents);
        try {
            for (std::size_t i = 0; i < watches_.size(); ++i) {
                auto& w = watches_[i]; w.trace = this; w.window = i < controls.size() ? controls[i] : parent; w.parent = i == controls.size();
                Require(w.window && SetWindowSubclass(w.window, Observe, reinterpret_cast<UINT_PTR>(&w), reinterpret_cast<DWORD_PTR>(&w)),
                    "install slider paint observer over the actual native procedure");
            }
        } catch (...) { Detach(); throw; }
    }
    ~Trace() { EndMetadataItems(); Detach(); }
    Trace(const Trace&) = delete; Trace& operator=(const Trace&) = delete;
    void Detach() noexcept { for (auto& w : watches_) if (IsWindow(w.window)) RemoveWindowSubclass(w.window, Observe, reinterpret_cast<UINT_PTR>(&w)); }
    void Phase(const std::string& value) { phase_ = value; }
    // Optional observations for the separate real message-pump pass only.
    // Merely records the original main-window message before/after forwarding.
    void QueuedUiMessages(bool value) noexcept { queuedUiMessages_=value; }
    void ItemNotifications(bool enabled) noexcept { diagnosticItems_ = enabled; }
    void BeginMetadataItems(int id) {
        Require(!metadataOnly_ && !returnHook_ && !returnAudit_, "one fixed-buffer metadata return audit at a time");
        metadataPassId_ = id; returnAudit_ = this;
        returnHook_ = SetWindowsHookExW(WH_CALLWNDPROCRET, Returned, nullptr, GetCurrentThreadId());
        if (!returnHook_) { returnAudit_ = nullptr; throw std::runtime_error("install native customdraw final-return audit"); }
        metadataOnly_ = true;
    }
    void EndMetadataItems() noexcept {
        metadataOnly_ = false;
        if (returnHook_) { if (!UnhookWindowsHookEx(returnHook_)) failed_ = true; returnHook_ = nullptr; }
        if (returnAudit_ == this) returnAudit_ = nullptr;
    }
    const MetadataEvent* MetadataEvents() const noexcept { return metadata_->events.data(); }
    std::size_t MetadataCount() const noexcept { return metadata_->count; }
    Snapshot Read(HWND window, HDC dc = nullptr) { sampling_ = true;
        try { auto result = Capture(window, dc); sampling_ = false; return result; }
        catch (...) { sampling_ = false; throw; } }
    const std::vector<Event>& Events() const noexcept { return events_; }
    using PaintKey = std::tuple<std::uintptr_t,std::uint64_t,std::string>;
    static PaintKey Key(const Event& event,const Snapshot& value) { return {reinterpret_cast<std::uintptr_t>(value.window),value.paintSequence,event.phase}; }
    std::map<PaintKey,const Snapshot*> FinalPaints() const {
        std::map<PaintKey,const Snapshot*> finals;
        for(const auto& event:events_)if(!event.parent&&event.message==WM_PAINT)
            for(const auto& pair:event.targets)if(pair.after.window==event.sourceWindow&&!pair.after.borrowedDc)
                finals[Key(event,pair.after)]=&pair.after;
        return finals;
    }
    bool Complete() const noexcept { return !failed_ && !truncated_; }
    void Write(const std::filesystem::path& directory) const {
        std::filesystem::create_directories(directory); std::ofstream csv(directory / "paint-trace.csv");
        std::ofstream audit(directory / "paint-return-audit.csv");
        audit << "# metadata-only pass: fixed storage; no observer screenshots, GDI, allocations or TBM getters inside native messages\n"
              << "seq,pass_id,source_id,source_window,stage,stage_name,item,item_state,native_result,returned_result,final_observed,final_result,final_matches\n";
        for (std::size_t n = 0; n < metadata_->count; ++n) {
            const auto& event = metadata_->events[n];
            audit << event.sequence << ',' << event.passId << ',' << event.id << ',' << reinterpret_cast<std::uintptr_t>(event.source)
                  << ',' << event.stage << ',' << DrawStageName(event.stage) << ',' << event.item << ',' << event.itemState
                  << ',' << event.nativeResult << ',' << event.returnedResult << ',' << event.finalObserved << ',' << event.finalResult
                  << ',' << (event.finalObserved && event.finalResult == event.returnedResult) << '\n';
        }
        Require(bool(audit), "write fixed-buffer native return audit");
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        csv << "# real native forwarding; capture overhead invalidates performance comparisons; qpc_frequency=" << frequency.QuadPart
            << "; failed=" << failed_ << "; truncated=" << truncated_
            << "; supplied DC and window snapshots are separate; bmp_pair_limit=256; bmp_bytes_limit=33554432\n";
        csv << "seq,depth,phase,msg,dispatch_id,target_id,parent,begin,end,result,borrowed_dc,live,varied_before,varied_after,"
               "tick_before,tick_after,paint_before,paint_after,erase_before,erase_after,changed_pixels,thumb_uniform_before,thumb_uniform_after,"
               "hash_before,hash_after,update_l,update_t,update_r,update_b,clip_kind,clip_l,clip_t,clip_r,clip_b,brush_known,brush_rgb,"
               "diagnostic_items,custom_draw,notify_code,draw_stage,stage_name,item,item_state,draw_l,draw_t,draw_r,draw_b,"
               "native_result,returned_result,return_changed,supplied_dc,dc_type,dc_window,viewport_x,viewport_y,window_x,window_y,device_x,device_y,"
               "paint_seq,changed_old_thumb,changed_new_thumb,changed_channel,thumb_l,thumb_t,thumb_r,thumb_b,"
               "thumb_readable,map_mode,bitmap_w,bitmap_h,thumb_device_l,thumb_device_t,thumb_device_r,thumb_device_b,"
               "same_paint_final_available,thumb_diff_same_paint_final,source_window,source_id\n";
        std::array<unsigned, 7> images{}, baselineStageImages{}, stageImages{}, diagnosticFinalImages{};
        const auto finals=FinalPaints();
        std::size_t imagePairs = 0, imageBytes = 0;
        for (const auto& event : events_) for (const auto& pair : event.targets) {
            const auto& a = pair.before; const auto& b = pair.after; const auto changed = Changed(a, b);
            const auto final=finals.find(Key(event,a));
            const bool comparable=final!=finals.end()&&a.ThumbReadable()&&final->second->ThumbReadable()&&a.tick==final->second->tick;
            csv << event.sequence << ',' << event.depth << ',' << event.phase << ',' << event.message << ',' << event.dispatchId << ',' << a.id
                << ',' << event.parent << ',' << event.begin << ',' << event.end << ',' << event.result << ',' << a.borrowedDc << ',' << (a.Live() && b.Live())
                << ',' << a.Varied() << ',' << b.Varied() << ',' << a.tick << ',' << b.tick << ',' << a.paints << ',' << b.paints << ',' << a.erases << ',' << b.erases
                << ',' << changed << ',' << a.ThumbUniform() << ',' << b.ThumbUniform() << ',' << a.Hash() << ',' << b.Hash()
                << ',' << a.update.left << ',' << a.update.top << ',' << a.update.right << ',' << a.update.bottom << ',' << a.clipKind
                << ',' << a.clip.left << ',' << a.clip.top << ',' << a.clip.right << ',' << a.clip.bottom << ',' << event.brushKnown << ',' << event.brush
                << ',' << event.diagnosticItems << ',' << event.customDraw << ',' << int(event.notifyCode) << ',' << event.drawStage
                << ',' << DrawStageName(event.drawStage) << ',' << event.item << ',' << event.itemState
                << ',' << event.drawRect.left << ',' << event.drawRect.top << ',' << event.drawRect.right << ',' << event.drawRect.bottom
                << ',' << event.nativeResult << ',' << event.result << ',' << (event.nativeResult != event.result)
                << ',' << reinterpret_cast<std::uintptr_t>(event.suppliedDc) << ',' << a.dcType << ',' << reinterpret_cast<std::uintptr_t>(a.dcWindow)
                << ',' << a.viewportOrigin.x << ',' << a.viewportOrigin.y << ',' << a.windowOrigin.x << ',' << a.windowOrigin.y
                << ',' << a.deviceOrigin.x << ',' << a.deviceOrigin.y << ',' << a.paintSequence
                << ',' << ChangedIn(a,b,a.thumb) << ',' << ChangedIn(a,b,b.thumb) << ',' << ChangedIn(a,b,a.channel)
                << ',' << a.thumb.left << ',' << a.thumb.top << ',' << a.thumb.right << ',' << a.thumb.bottom
                << ',' << a.ThumbReadable() << ',' << a.mapMode << ',' << a.bitmapWidth << ',' << a.bitmapHeight
                << ',' << a.thumbDevice[0].x << ',' << a.thumbDevice[0].y << ',' << a.thumbDevice[1].x << ',' << a.thumbDevice[1].y
                << ',' << comparable << ',' << (comparable?ChangedIn(a,*final->second,a.thumb):0)
                << ',' << reinterpret_cast<std::uintptr_t>(event.sourceWindow) << ',' << event.sourceId << '\n';
            std::size_t control = 0; while (control < images.size() && GetDlgCtrlID(watches_[control].window) != a.id) ++control;
            const bool controlled = !event.parent && a.window == event.sourceWindow &&
                event.phase.find("-controlled-native-") != std::string::npos &&
                (event.message == WM_ERASEBKGND || event.message == WM_PRINTCLIENT);
            const bool stage = event.customDraw && a.window == event.sourceWindow && control < stageImages.size() &&
                (event.diagnosticItems ? stageImages[control] < 20 : baselineStageImages[control] < 4);
            const bool finalImage = event.diagnosticItems && !event.parent && event.message == WM_PAINT &&
                a.window == event.sourceWindow && control < diagnosticFinalImages.size() && diagnosticFinalImages[control] < 1;
            // Preserve any naturally delivered erase in the independent pump
            // pass even if early moving-thumb paint samples used the small
            // per-control image allowance. Global BMP byte/pair caps still apply.
            const bool queuedErase = queuedUiMessages_ && !event.parent && event.message == WM_ERASEBKGND &&
                a.window == event.sourceWindow;
            const auto bytes = (a.pixels.size() + b.pixels.size()) * sizeof(std::uint32_t) + 2 * (sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
            if (a.copied && b.copied && imagePairs < 256 && bytes <= 32 * 1024 * 1024 - imageBytes &&
                (controlled || stage || finalImage || queuedErase || (control < images.size() && images[control] < 4 &&
                (changed || (!event.parent && event.message == WM_PAINT))))) {
                const auto name = std::to_string(event.sequence) + "-" + std::to_string(a.id) + (a.borrowedDc ? "-supplied" : "-window");
                Bitmap(directory / (name + "-before.bmp"), a); Bitmap(directory / (name + "-after.bmp"), b);
                ++imagePairs; imageBytes += bytes;
                if (stage) ++(event.diagnosticItems ? stageImages[control] : baselineStageImages[control]);
                if (finalImage) ++diagnosticFinalImages[control];
                if (!controlled && control < images.size()) ++images[control];
            }
        }
        Require(bool(csv), "write slider paint trace");
    }
};
} // namespace slider_paint_trace
