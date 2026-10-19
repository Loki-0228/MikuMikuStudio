// Standalone experiment. This file does not change MMS's control procedure.
// Run in a disposable desktop process: trackbar_buffer_tests.exe <output-dir>.
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr wchar_t kClass[] = L"MMS.NativeTrackbarBuffer.Experiment";
constexpr wchar_t kStateProperty[] = L"MMS.NativeTrackbarBuffer.State";
constexpr COLORREF kMarkerA = RGB(231, 17, 149), kMarkerB = RGB(13, 223, 97);
constexpr unsigned kMaxPaints = 512;
struct Fixture;
struct Surface;
enum class Failure { None, ReferenceDC, MemoryDC, DIB, SelectDIB, UpdateRegion, Marker, Transfer };
const char* Name(Failure value) {
    switch (value) {
    case Failure::ReferenceDC: return "reference-dc";
    case Failure::MemoryDC: return "memory-dc";
    case Failure::DIB: return "dib";
    case Failure::SelectDIB: return "select-dib";
    case Failure::UpdateRegion: return "update-region";
    case Failure::Marker: return "marker-initialization";
    case Failure::Transfer: return "final-transfer";
    default: return "none";
    }
}
void Need(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint32_t RGBOnly(std::uint32_t pixel) { return pixel & 0x00ffffffu; }
std::uint32_t DibRGB(COLORREF color) {
    return (std::uint32_t(GetRValue(color)) << 16) | (std::uint32_t(GetGValue(color)) << 8) | GetBValue(color);
}
struct Fingerprint { std::uint64_t hash = 1469598103934665603ull; bool readable = true; };
Fingerprint ReadLive(HDC dc, int width, int height) noexcept {
    Fingerprint result;result.readable=false;
    if(!dc||GetObjectType(dc)!=OBJ_DC||width<=0||height<=0||width>2048||height>256)return result;
    // Diagnostic capture only. Render resources/fallback decisions remain
    // separate; a failed capture cannot change native painting or its result.
    struct Capture {
        HDC memory=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
        ~Capture(){if(previous&&previous!=HGDI_ERROR)SelectObject(memory,previous);
            if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);}
    } capture;
    capture.memory=CreateCompatibleDC(dc);if(!capture.memory)return result;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::uint32_t* pixels=nullptr;
    capture.bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
    if(!capture.bitmap||!pixels)return result;
    capture.previous=SelectObject(capture.memory,capture.bitmap);
    if(!capture.previous||capture.previous==HGDI_ERROR)return result;
    if(!BitBlt(capture.memory,0,0,width,height,dc,0,0,SRCCOPY)||!GdiFlush())return result;
    result.readable=true;
    for(std::size_t i=0;i<std::size_t(width)*height;++i)
        result.hash=(result.hash^RGBOnly(pixels[i]))*1099511628211ull;
    return result;
}
struct Observation {
    std::uint64_t serial = 0;
    unsigned nativePaints = 0, begins = 0, ends = 0, redirects = 0;
    unsigned prepaints = 0, ctlcolors = 0;
    bool fallback = false, copied = false, transferFailed = false, retry = false;
    bool nestedPaint = false, liveChangedAtPrepaint = false, liveChangedAtCtlcolor = false, phaseReadFailed = false;
    Fingerprint before{}, after{};
    LRESULT nativeResult = 0;
};
struct State {
    Fixture* fixture = nullptr;
    HWND hwnd = nullptr;
    WNDPROC native = nullptr;
    int logicalId = 0;
    COLORREF background = 0;
    bool buffered = false, bypassNext = false, preparing = false;
    Failure inject = Failure::None;
    Surface* active = nullptr;
    Observation* observation = nullptr;
    std::array<Observation, kMaxPaints> paints{};
    unsigned count = 0, nativePaints = 0, begins = 0, ends = 0, fallbacks = 0, transferFailures = 0;
    unsigned rawPaints = 0, nativeKeyDowns = 0, nativeKeyUps = 0;
    bool truncated = false;
};
State* Find(HWND window) noexcept { return static_cast<State*>(GetPropW(window, kStateProperty)); }
struct Surface {
    HWND window = nullptr;
    HDC reference = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    HRGN update = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() {
        if (previous && previous != HGDI_ERROR && memory) SelectObject(memory, previous);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (reference) ReleaseDC(window, reference);
        if (update) DeleteObject(update);
    }
    bool Prepare(HWND hwnd, Failure fail, bool withRegion) noexcept {
        window = hwnd;
        RECT client{};
        if (!GetClientRect(hwnd, &client) || client.left || client.top || client.right <= 0 || client.bottom <= 0 ||
            client.right > 2048 || client.bottom > 256) return false;
        width = client.right; height = client.bottom;
        if (fail == Failure::ReferenceDC || !(reference = GetDC(hwnd))) return false;
        if (fail == Failure::MemoryDC || !(memory = CreateCompatibleDC(reference))) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        if (fail == Failure::DIB || !(bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS,
            reinterpret_cast<void**>(&pixels), nullptr, 0))) return false;
        if (fail == Failure::SelectDIB) return false;
        previous = SelectObject(memory, bitmap);
        if (!previous || previous == HGDI_ERROR) return false;
        if (withRegion) {
            if (fail == Failure::UpdateRegion || !(update = CreateRectRgn(0, 0, 0, 0))) return false;
            // Internal/no-update WM_PAINT is passed straight through, preserving native work.
            const int region = GetUpdateRgn(hwnd, update, FALSE);
            if (region == ERROR || region == NULLREGION) return false;
        }
        if (fail == Failure::Marker) return false;
        std::fill_n(pixels, std::size_t(width) * height, DibRGB(kMarkerA));
        return true;
    }
};
LRESULT NativePaint(State& state, HWND window, WPARAM wp, LPARAM lp) noexcept {
    ++state.nativePaints;
    if (state.observation) ++state.observation->nativePaints;
    return CallWindowProcA(state.native, window, WM_PAINT, wp, lp);
}
void RetryDirect(State& state, const Surface& surface) noexcept {
    if (!state.hwnd || !IsWindow(state.hwnd)) return;
    state.bypassNext = true;
    if (state.observation) state.observation->retry = true;
    // EndPaint already consumed the original update region. Preserve it for a
    // later, unbuffered WM_PAINT, never rerun native paint in this message.
    if (!surface.update || !InvalidateRgn(state.hwnd, surface.update, FALSE))
        InvalidateRect(state.hwnd, nullptr, FALSE);
}
LRESULT CALLBACK BufferedProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    State* state = Find(window);
    if (!state) return DefWindowProcA(window, message, wp, lp);
    if (message == WM_NCDESTROY) {
        const auto native = state->native;
        RemovePropW(window, kStateProperty); state->hwnd = nullptr;
        return CallWindowProcA(native, window, message, wp, lp);
    }
    if (message == WM_KEYDOWN) ++state->nativeKeyDowns;
    if (message == WM_KEYUP) ++state->nativeKeyUps;
    if (message == WM_ERASEBKGND && state->active) {
        if (state->observation) ++state->observation->redirects;
        // Only BeginPaint/native paint nested inside this prepared transaction
        // uses the buffer. Independent erases remain native screen operations.
        return CallWindowProcA(state->native, window, message,
            reinterpret_cast<WPARAM>(state->active->memory), lp);
    }
    if (message != WM_PAINT || wp || !state->buffered)
        return CallWindowProcA(state->native, window, message, wp, lp);
    Observation* previousObservation = state->observation;
    Observation local{};
    local.serial = std::uint64_t(state->count) + 1;
    Observation* observation = &local;
    if (state->count < state->paints.size()) observation = &state->paints[state->count++];
    else state->truncated = true;
    observation->serial = local.serial;
    state->observation = observation;
    struct ObservationGuard {
        State& state; Observation* previous;
        ~ObservationGuard() { state.observation = previous; }
    } observationGuard{*state, previousObservation};
    if (state->active || state->preparing) {
        observation->nestedPaint = true;
        observation->fallback = true; ++state->fallbacks;
        return observation->nativeResult = NativePaint(*state, window, wp, lp);
    }
    const Failure injected = state->inject; state->inject = Failure::None;
    Surface surface;
    state->preparing = true;
    const bool prepared = !state->bypassNext && surface.Prepare(window, injected, true);
    state->preparing = false;
    if (!prepared) {
        state->bypassNext = false;
        observation->fallback = true; ++state->fallbacks;
        return observation->nativeResult = NativePaint(*state, window, wp, lp);
    }
    observation->before = ReadLive(surface.reference, surface.width, surface.height);
    PAINTSTRUCT paint{};
    state->active = &surface;
    const HDC destination = BeginPaint(window, &paint);
    ++state->begins; ++observation->begins;
    struct PaintGuard {
        State& state; HWND window; PAINTSTRUCT& paint; Observation& observation; bool ended = false;
        void End() noexcept {
            if (ended) return;
            EndPaint(window, &paint); ++state.ends; ++observation.ends;
            state.active = nullptr; ended = true;
        }
        ~PaintGuard() { End(); }
    } paintGuard{*state, window, paint, *observation};
    if (!destination) {
        paintGuard.End(); RetryDirect(*state, surface); return 0;
    }
    const LRESULT result = NativePaint(*state, window, reinterpret_cast<WPARAM>(surface.memory), lp);
    observation->nativeResult = result;
    GdiFlush();
    observation->after = ReadLive(surface.reference, surface.width, surface.height);
    RECT current{};
    const bool sameWindow = state->hwnd == window && IsWindow(window) && GetClientRect(window, &current) &&
        current.left == 0 && current.top == 0 && current.right == surface.width && current.bottom == surface.height;
    if (sameWindow && injected != Failure::Transfer)
        observation->copied = BitBlt(destination, 0, 0, surface.width, surface.height, surface.memory, 0, 0, SRCCOPY) != FALSE;
    if (!observation->copied) {
        observation->transferFailed = true; ++state->transferFailures;
    }
    paintGuard.End();
    if (!observation->copied && sameWindow) RetryDirect(*state, surface);
    return result;
}
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    bool operator==(const Image& other) const { return width == other.width && height == other.height && pixels == other.pixels; }
};
Image From(const Surface& surface) {
    GdiFlush();
    Image result{surface.width, surface.height, {}};
    result.pixels.assign(surface.pixels, surface.pixels + std::size_t(surface.width) * surface.height);
    for (auto& pixel : result.pixels) pixel = RGBOnly(pixel);
    return result;
}
Image WindowImage(HWND window) {
    Surface surface; Need(surface.Prepare(window, Failure::None, false), "allocate real-window capture");
    Need(GetObjectType(surface.reference) == OBJ_DC, "window capture must use actual display DC");
    Need(BitBlt(surface.memory, 0, 0, surface.width, surface.height, surface.reference, 0, 0, SRCCOPY) != FALSE, "capture actual native window pixels");
    return From(surface);
}
Image NativeImage(State& state, COLORREF marker, bool eraseFirst) {
    Surface surface; Need(surface.Prepare(state.hwnd, Failure::None, false), "allocate native WM_PAINT(HDC) capture");
    std::fill_n(surface.pixels, std::size_t(surface.width) * surface.height, DibRGB(marker));
    if (eraseFirst) CallWindowProcA(state.native, state.hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(surface.memory), 0);
    ++state.rawPaints;
    CallWindowProcA(state.native, state.hwnd, WM_PAINT, reinterpret_cast<WPARAM>(surface.memory), 0);
    return From(surface);
}
std::size_t Different(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return std::size_t(-1);
    std::size_t count = 0; for (std::size_t i = 0; i < a.pixels.size(); ++i) count += a.pixels[i] != b.pixels[i];
    return count;
}
bool Varied(const Image& image) {
    return !image.pixels.empty() && std::any_of(image.pixels.begin(), image.pixels.end(), [&](auto value) { return value != image.pixels[0]; });
}
void SaveBitmap(const std::filesystem::path& file, const Image& image) {
    BITMAPFILEHEADER fileHeader{}; BITMAPINFOHEADER header{};
    fileHeader.bfType = 0x4d42; fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header);
    fileHeader.bfSize = fileHeader.bfOffBits + DWORD(image.pixels.size() * sizeof(std::uint32_t));
    header.biSize = sizeof(header); header.biWidth = image.width; header.biHeight = -image.height;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    std::ofstream stream(file, std::ios::binary); Need(bool(stream), "create bounded bitmap evidence");
    stream.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(image.pixels.data()), std::streamsize(image.pixels.size() * sizeof(std::uint32_t)));
    Need(bool(stream), "write bitmap evidence");
}
struct Fixture {
    HWND parent = nullptr; HBRUSH fovBrush = nullptr, redBrush = nullptr;
    std::array<State, 4> states{};
    unsigned scrolls = 0, failures = 0, checks = 0, files = 0;
    bool classRegistered = false;
    std::uint64_t bytes = 0;
    const ULONGLONG started=GetTickCount64();
    std::filesystem::path output;
    std::ofstream report, forms;
    explicit Fixture(std::filesystem::path path) : output(std::move(path)), report(output / "trackbar-buffer-report.txt"), forms(output / "white-form.csv") {
        Need(bool(report) && bool(forms), "create prototype reports");
        forms << "phase,id,source,tick,focus,ui_state,white_thumb_border,white_thumb_interior,background_thumb_interior,thumb_interior_pixels,white_client_edges,white_bbox_l,white_bbox_t,white_bbox_r,white_bbox_b\n";
        report << "Standalone native WM_PAINT(HDC) buffer experiment. Not a production helper or a flicker fix claim. "
            "Actual window GetDC pixels and separately rendered HDC pixels are labelled; neither certifies DWM visibility. "
            "Synthetic keyboard/down/up messages are not hardware input. No WM_PRINT/WM_PRINTCLIENT substitution.\n"
            "Point-in-time input admission checks every VK high bit and rejects foreign thread capture; no BlockInput, "
            "foreground request or cursor movement. GetAsyncKeyState polling can consume low bits; this is not an atomic input sandbox.\n"
            "Budget:60 seconds total at phase/pump admission,512 messages and1.5 seconds per pump; window reads use one bounded displayDC-to-DIB copy plus CPU hash, not per-pixel GetPixel calls.\n"
            "The exact same native v6 procedure handles input/theme/focus/disabled/painting. The prototype only intercepts ordinary WM_PAINT; "
            "a supplied WM_PAINT HDC passes through. Transaction-local WM_ERASEBKGND is redirected; independent native erase is retained. "
            "Transfer failure schedules one later direct native paint, never reruns native paint within the consumed message.\n";
        fovBrush = CreateSolidBrush(RGB(37, 37, 71)); redBrush = CreateSolidBrush(RGB(44, 88, 44));
        if (!fovBrush || !redBrush) {
            if (fovBrush) DeleteObject(fovBrush); if (redBrush) DeleteObject(redBrush);
            fovBrush = redBrush = nullptr; Need(false, "create MMS-style background brushes");
        }
    }
    ~Fixture() {
        const auto capture=GetCapture();
        if(capture&&std::any_of(states.begin(),states.end(),[&](const auto& state){return state.hwnd==capture;})){
            SendMessageA(capture,WM_CANCELMODE,0,0);if(GetCapture()==capture)ReleaseCapture();
        }
        if (parent) DestroyWindow(parent);
        if (fovBrush) DeleteObject(fovBrush); if (redBrush) DeleteObject(redBrush);
        if (classRegistered) UnregisterClassW(kClass, GetModuleHandleW(nullptr));
    }
    void Check(bool condition, const std::string& label) {
        ++checks; failures += !condition; report << (condition ? "PASS " : "FAIL ") << label << '\n';
    }
    void Store(const std::string& name, const Image& image) {
        Need(++files <= 192, "BMP count quota"); bytes += image.pixels.size() * 4;
        Need(bytes <= 16 * 1024 * 1024, "BMP byte quota"); SaveBitmap(output / (name + ".bmp"), image);
    }
    void Form(const std::string& phase, State& state, const char* source, const Image& image) {
        RECT thumb{}; SendMessageA(state.hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        unsigned border = 0, interior = 0, background = 0, area = 0, clientEdges = 0;
        RECT whiteBounds{image.width, image.height, 0, 0};
        for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
            const auto pixel = image.pixels[std::size_t(y) * image.width + x];
            const bool white = ((pixel >> 16) & 255) >= 220 && ((pixel >> 8) & 255) >= 220 && (pixel & 255) >= 220;
            if (white) { whiteBounds.left = std::min(whiteBounds.left, LONG(x)); whiteBounds.top = std::min(whiteBounds.top, LONG(y));
                whiteBounds.right = std::max(whiteBounds.right, LONG(x + 1)); whiteBounds.bottom = std::max(whiteBounds.bottom, LONG(y + 1)); }
            if (white && (x < 2 || y < 2 || x >= image.width - 2 || y >= image.height - 2)) ++clientEdges;
            if (x >= thumb.left && x < thumb.right && y >= thumb.top && y < thumb.bottom) {
                const bool inside = x > thumb.left && x < thumb.right - 1 && y > thumb.top && y < thumb.bottom - 1;
                if (inside) { ++area; interior += white; background += pixel == DibRGB(state.background); }
                else border += white;
            }
        }
        if (!whiteBounds.right) whiteBounds = {};
        forms << phase << ',' << state.logicalId << ',' << source << ',' << SendMessageA(state.hwnd, TBM_GETPOS, 0, 0) << ','
            << (GetFocus() == state.hwnd) << ',' << SendMessageA(state.hwnd, WM_QUERYUISTATE, 0, 0) << ','
            << border << ',' << interior << ',' << background << ',' << area << ',' << clientEdges << ','
            << whiteBounds.left << ',' << whiteBounds.top << ',' << whiteBounds.right << ',' << whiteBounds.bottom << '\n';
    }
};
void GuardInput(Fixture& fixture) {
    Need(GetTickCount64()-fixture.started<=60000,"60-second whole-prototype budget exhausted; incomplete diagnostic");
    for(int key=1;key<=255;++key)Need((GetAsyncKeyState(key)&0x8000)==0,"real held key/button makes prototype incomplete; do not take capture");
    const auto capture=GetCapture();
    Need(!capture||std::any_of(fixture.states.begin(),fixture.states.end(),[&](const auto& state){return state.hwnd==capture;}),
        "foreign capture makes prototype incomplete; never release or steal it");
}
void ObserveParent(State* state, bool prepaint) noexcept {
    if (!state || !state->active || !state->observation) return;
    auto& observation = *state->observation;
    if (prepaint) ++observation.prepaints; else ++observation.ctlcolors;
    const auto live = ReadLive(state->active->reference, state->active->width, state->active->height);
    observation.phaseReadFailed|=!live.readable;
    const bool different = live.readable && observation.before.readable && live.hash != observation.before.hash;
    if (prepaint) observation.liveChangedAtPrepaint |= different; else observation.liveChangedAtCtlcolor |= different;
}
LRESULT CALLBACK ParentProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        fixture = static_cast<Fixture*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fixture));
    }
    if (fixture && message == WM_CTLCOLORSTATIC) {
        const auto state = Find(reinterpret_cast<HWND>(lp)); ObserveParent(state, false);
        if (state) return reinterpret_cast<LRESULT>(state->logicalId == 447 ? fixture->fovBrush : fixture->redBrush);
    }
    if (fixture && message == WM_NOTIFY && lp) {
        const auto* notification = reinterpret_cast<const NMHDR*>(lp);
        if (notification->code == NM_CUSTOMDRAW) {
            const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
            if (draw->dwDrawStage == CDDS_PREPAINT) ObserveParent(Find(notification->hwndFrom), true);
            // MMS does not owner-draw FOV/RGB trackbars or request item stages.
            return CDRF_DODEFAULT;
        }
    }
    if (fixture && message == WM_HSCROLL) { ++fixture->scrolls; return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
void Pump(Fixture& fixture) {
    GuardInput(fixture);
    const auto started = GetTickCount64(); unsigned count = 0;
    MSG message{};
    while (PeekMessageW(&message, fixture.parent, 0, 0, PM_NOREMOVE)) {
        GuardInput(fixture);
        Need(message.message != WM_QUIT && message.message != WM_CLOSE, "external close/quit leaves prototype incomplete");
        Need(!(message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST) && message.message != WM_CHAR &&
            message.message != WM_LBUTTONDOWN && message.message != WM_LBUTTONUP && message.message != WM_RBUTTONDOWN &&
            message.message != WM_MBUTTONDOWN && message.message != WM_MOUSEWHEEL, "queued user input is not consumed by prototype");
        Need(++count <= 512 && GetTickCount64() - started <= 1500, "bounded own-window prototype pump");
        Need(PeekMessageW(&message, fixture.parent, 0, 0, PM_REMOVE) != FALSE, "remove observed own-window diagnostic message");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    for (auto& state : fixture.states) if (state.hwnd) UpdateWindow(state.hwnd);
    GuardInput(fixture);
    GdiFlush();
}
void AuditDpi(Fixture& fixture, HWND window) {
    using Context = HANDLE(WINAPI*)(HWND); using Awareness = int(WINAPI*)(HANDLE); using Dpi = UINT(WINAPI*)(HWND);
    const auto user = GetModuleHandleW(L"user32.dll");
    const auto context = reinterpret_cast<Context>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
    const auto awareness = reinterpret_cast<Awareness>(GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext"));
    const auto dpi = reinterpret_cast<Dpi>(GetProcAddress(user, "GetDpiForWindow"));
    const auto actual = context ? context(window) : nullptr;
    RECT client{};GetClientRect(window,&client);
    const HDC display=GetDC(window);POINT device{client.right,client.bottom};
    const bool mapped=display&&LPtoDP(display,&device,1);
    fixture.report << "DPI hwnd=" << reinterpret_cast<std::uintptr_t>(window)
        << " awareness=" << (actual && awareness ? awareness(actual) : -1)
        << " effective_dpi=" << (dpi ? dpi(window) : 0)
        << " client="<<client.right<<'x'<<client.bottom<<" device_extent_known="<<mapped
        <<" device_extent="<<device.x<<'x'<<device.y<<" map_mode="<<(display?GetMapMode(display):0)
        << " (missing APIs unavailable on Win7; no assumption that process-DPI requests succeeded)\n";
    if(display)ReleaseDC(window,display);
}
void Create(Fixture& fixture) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; Need(InitCommonControlsEx(&controls) != FALSE, "initialize native trackbars");
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.lpfnWndProc = ParentProc;
    cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); cls.lpszClassName = kClass;
    Need(RegisterClassExW(&cls) != 0, "register prototype parent");
    fixture.classRegistered = true;
    fixture.parent = CreateWindowExW(0, kClass, L"MMS native trackbar buffer experiment", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        80, 80, 580, 220, nullptr, nullptr, cls.hInstance, &fixture);
    Need(fixture.parent != nullptr, "create actual prototype window");
    for (unsigned i = 0; i < fixture.states.size(); ++i) {
        auto& state = fixture.states[i]; state.fixture = &fixture; state.logicalId = i < 2 ? 447 : 455;
        state.background = i < 2 ? RGB(37, 37, 71) : RGB(44, 88, 44); state.buffered = i % 2 != 0;
        state.hwnd = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | (i < 2 ? 0 : TBS_NOTICKS),
            20 + (i % 2) * 270, 30 + (i / 2) * 65, 220, 24, fixture.parent,
            reinterpret_cast<HMENU>(INT_PTR(100 + i)), cls.hInstance, nullptr);
        Need(state.hwnd != nullptr, "create actual native ANSI trackbar matching MMS");
        state.native = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(state.hwnd, GWLP_WNDPROC));
        Need(state.native && SetPropW(state.hwnd, kStateProperty, &state), "attach own prototype state");
        // Both sides use the same ANSI forwarding layer, matching MMS's actual
        // subclass encoding. Only state.buffered selects the WM_PAINT experiment.
        SetLastError(0); const auto previous = SetWindowLongPtrA(state.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(BufferedProc));
        Need(previous != 0 || GetLastError() == 0, "install common prototype-only ANSI forwarding wrapper");
        fixture.Check(!IsWindowUnicode(state.hwnd),"baseline and candidate both actually use the same ANSI forwarding encoding");
        SendMessageA(state.hwnd, TBM_SETRANGEMIN, FALSE, i < 2 ? 1 : 0);
        SendMessageA(state.hwnd, TBM_SETRANGEMAX, FALSE, i < 2 ? 125 : 255);
        SendMessageA(state.hwnd, TBM_SETPOS, FALSE, i < 2 ? 30 : 154);
    }
    using VersionFunction = HRESULT(CALLBACK*)(void*);
    struct Version { DWORD size, major, minor, build, platform; };
    for (auto& state : fixture.states) {
        const auto module = reinterpret_cast<HMODULE>(GetClassLongPtrW(state.hwnd, GCLP_HMODULE));
        const auto version = reinterpret_cast<VersionFunction>(GetProcAddress(module, "DllGetVersion"));
        Version value{sizeof(value)}; wchar_t path[32768]{};
        const auto count = GetModuleFileNameW(module, path, DWORD(std::size(path)));
        const bool known = version && SUCCEEDED(version(&value));
        fixture.report << "NATIVE_CLASS id=" << state.logicalId << " buffered=" << state.buffered << " unicode=" << IsWindowUnicode(state.hwnd)
            << " version_known=" << known << " version=" << value.major << '.' << value.minor << '.' << value.build << '\n';
        if (count) { const auto utf8Size = WideCharToMultiByte(CP_UTF8, 0, path, int(count), nullptr, 0, nullptr, nullptr);
            std::string utf8(std::size_t(std::max(0, utf8Size)), '\0');
            if (utf8Size) WideCharToMultiByte(CP_UTF8, 0, path, int(count), utf8.data(), utf8Size, nullptr, nullptr);
            fixture.report << "MODULE " << utf8 << '\n'; }
        Need(known && value.major >= 6, "prototype requires actual loaded native common-controls v6");
        AuditDpi(fixture, state.hwnd);
    }
    POINT cursor{}; Need(GetCursorPos(&cursor) != FALSE, "query initial OS cursor");
    Need(!GetCapture(), "prototype does not take capture from another operation");
    ShowWindow(fixture.parent, SW_SHOWNOACTIVATE);
    for (auto& state : fixture.states) { RECT rect{}; Need(GetWindowRect(state.hwnd, &rect), "query actual child rectangle");
        Need(!PtInRect(&rect, cursor), "cursor over a test slider would introduce uncontrolled hover; incomplete"); }
    RedrawWindow(fixture.parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    Pump(fixture);
}
template<class Apply>
void Scenario(Fixture& fixture, const std::string& name, bool focused, Apply apply) {
    for (unsigned kind = 0; kind < 2; ++kind) {
        std::array<Image, 2> windows;
        for (unsigned member = 0; member < 2; ++member) {
            auto& state = fixture.states[kind * 2 + member]; apply(state); Pump(fixture);
            Need(IsWindowVisible(state.hwnd), "read a real visible native trackbar");
            fixture.Check((GetFocus()==state.hwnd)==focused, name+" actual per-control focus matches the same requested baseline/buffer state");
            // WindowImage owns/releases its actual GetDC; no borrowed/DIB DC is
            // ever described as a visible-window capture.
            windows[member] = WindowImage(state.hwnd);
            const auto nativeA = NativeImage(state, kMarkerA, false);
            const auto nativeB = NativeImage(state, kMarkerB, false);
            const auto erased = NativeImage(state, kMarkerA, true);
            const auto prefix = name + "-" + std::to_string(state.logicalId) + (member ? "-buffer" : "-native");
            fixture.Store(prefix + "-window", windows[member]); fixture.Store(prefix + "-hdc-A", nativeA); fixture.Store(prefix + "-hdc-B", nativeB);
            fixture.Check(Varied(windows[member]), prefix + " actual native-window content is nonuniform");
            fixture.Check(nativeA == nativeB, prefix + " native WM_PAINT(HDC) fully replaces both distinct marker backgrounds");
            fixture.Check(std::none_of(nativeA.pixels.begin(), nativeA.pixels.end(), [](auto value) { return value == DibRGB(kMarkerA); }), prefix + " no marker A remains");
            fixture.Check(std::none_of(nativeB.pixels.begin(), nativeB.pixels.end(), [](auto value) { return value == DibRGB(kMarkerB); }), prefix + " no marker B remains");
            fixture.Check(windows[member] == nativeA, prefix + " actual window equals native WM_PAINT(HDC); differing_pixels=" + std::to_string(Different(windows[member], nativeA)));
            fixture.Check(windows[member] == erased, prefix + " explicit native erase+paint HDC equals actual window");
            fixture.Check(windows[member] == WindowImage(state.hwnd), prefix + " independent HDC rendering leaves actual window pixels unchanged");
            fixture.Form(name, state, member ? "buffer-window" : "native-window", windows[member]);
            fixture.Form(name, state, "native-hdc-A", nativeA);
            if (GetCapture() == state.hwnd) {
                RECT thumb{}; SendMessageA(state.hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
                SendMessageA(state.hwnd, WM_LBUTTONUP, 0, MAKELPARAM((thumb.left + thumb.right) / 2, (thumb.top + thumb.bottom) / 2));
                Need(GetCapture() != state.hwnd, "synthetic native release relinquishes only test capture"); Pump(fixture);
            }
        }
        fixture.Check(windows[0] == windows[1], name + "-" + std::to_string(fixture.states[kind * 2].logicalId) + " baseline/buffer exact final pixels; differing_pixels=" + std::to_string(Different(windows[0], windows[1])));
    }
}
void Failures(Fixture& fixture) {
    for (auto& state : fixture.states) if (state.buffered) {
        SetFocus(fixture.parent); Pump(fixture);
        const auto expected = WindowImage(state.hwnd);
        for (auto failure : {Failure::ReferenceDC, Failure::MemoryDC, Failure::DIB, Failure::SelectDIB, Failure::UpdateRegion, Failure::Marker}) {
            const auto native = state.nativePaints, begins = state.begins, ends = state.ends, fallbacks = state.fallbacks;
            state.inject = failure; InvalidateRect(state.hwnd, nullptr, FALSE); UpdateWindow(state.hwnd);
            fixture.Check(state.nativePaints == native + 1 && state.begins == begins && state.ends == ends && state.fallbacks == fallbacks + 1,
                "injected " + std::string(Name(failure)) + " fails before BeginPaint and forwards native once id=" + std::to_string(state.logicalId));
            fixture.Check(WindowImage(state.hwnd) == expected, "allocation fallback preserves actual final native pixels");
        }
        const auto native = state.nativePaints, begins = state.begins, ends = state.ends, failures = state.transferFailures;
        state.inject = Failure::Transfer; InvalidateRect(state.hwnd, nullptr, FALSE); UpdateWindow(state.hwnd);
        fixture.Check(state.nativePaints == native + 1 && state.begins == begins + 1 && state.ends == ends + 1 &&
            state.transferFailures == failures + 1 && state.bypassNext && GetUpdateRect(state.hwnd, nullptr, FALSE),
            "failed transfer pairs Begin/End and retains update without repeating native in consumed message");
        UpdateWindow(state.hwnd);
        fixture.Check(state.nativePaints == native + 2 && state.begins == begins + 1 && state.ends == ends + 1 && !state.bypassNext &&
            !GetUpdateRect(state.hwnd, nullptr, FALSE), "one later direct native paint completes failed transfer; no same-message replay");
        fixture.Check(WindowImage(state.hwnd) == expected, "deferred transfer failure recovery keeps native pixels");
    }
}
void Finish(Fixture& fixture) {
    std::ofstream csv(fixture.output / "paint-transactions.csv");
    Need(bool(csv), "create transaction ledger");
    csv << "id,serial,native_paints,begin,end,redirected_erase,prepaints,ctlcolors,fallback,copied,transfer_failed,retry,nested,live_before_readable,live_after_readable,live_before_hash,live_after_hash,live_changed_at_prepaint,live_changed_at_ctlcolor,phase_read_failed,native_result\n";
    for (const auto& state : fixture.states) if (state.buffered) {
        fixture.Check(!state.truncated && state.begins == state.ends, "bounded paint recording and paired Begin/End id=" + std::to_string(state.logicalId));
        fixture.Check(state.count&&std::any_of(state.paints.begin(),state.paints.begin()+state.count,[](const auto& item){return item.copied;}),
            "actual buffered native window paints were completed; an empty/fallback-only trace cannot qualify");
        for (unsigned i = 0; i < state.count; ++i) {
            const auto& item = state.paints[i];
            fixture.Check(!item.nestedPaint, "no unbuffered reentrant paint escapes the transaction");
            if (item.begins) fixture.Check(item.nativePaints <= 1 && item.begins == item.ends && !item.nestedPaint &&
                !item.liveChangedAtPrepaint && !item.liveChangedAtCtlcolor && !item.phaseReadFailed &&
                item.before.readable && item.after.readable && item.before.hash == item.after.hash,
                "buffer transaction native once; real window unchanged until final copy id=" + std::to_string(state.logicalId) + " serial=" + std::to_string(item.serial));
            csv << state.logicalId << ',' << item.serial << ',' << item.nativePaints << ',' << item.begins << ',' << item.ends << ',' << item.redirects << ','
                << item.prepaints << ',' << item.ctlcolors << ',' << item.fallback << ',' << item.copied << ',' << item.transferFailed << ',' << item.retry << ','
                << item.nestedPaint << ',' << item.before.readable << ',' << item.after.readable << ',' << item.before.hash << ',' << item.after.hash << ','
                << item.liveChangedAtPrepaint << ',' << item.liveChangedAtCtlcolor << ',' << item.phaseReadFailed << ',' << item.nativeResult << '\n';
        }
    }
    fixture.report << "SUMMARY checks=" << fixture.checks << " failures=" << fixture.failures << " bmp_files=" << fixture.files << " bmp_bytes=" << fixture.bytes
        << " scroll_notifications=" << fixture.scrolls << '\n'
        << "White/thumb-body/client-edge statistics are in white-form.csv. A native final white outline is preserved and reported, not hidden. "
        "Exact final equality does not prove a visible flash was observed or removed; independent external erase and compositor timing remain outside the atomic transaction.\n";
    Need(bool(csv) && bool(fixture.report) && bool(fixture.forms), "finish prototype evidence");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: trackbar_buffer_tests.exe <output-directory>\n"); return 2; }
    try {
        const auto output = std::filesystem::absolute(argv[1]); std::filesystem::create_directories(output);
        Fixture fixture(output); Create(fixture);
        Scenario(fixture, "initial", false, [](State& state) { SetFocus(state.fixture->parent); });
        Scenario(fixture, "focused-cues-hidden", true, [&](State& state) {
            EnableWindow(state.hwnd, TRUE); SetFocus(state.hwnd); SendMessageA(state.hwnd, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
            fixture.Check((SendMessageA(state.hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)!=0,"native focus-cue state was actually hidden");
        });
        Scenario(fixture, "focused-cues-visible", true, [&](State& state) {
            SetFocus(state.hwnd); SendMessageA(state.hwnd, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            fixture.Check((SendMessageA(state.hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)==0,"native focus-cue state was actually visible");
        });
        // Keep the preceding native partial-update transition and every exact
        // comparison. This additional case tests the full-update hypothesis;
        // it cannot replace or excuse the original two-pixel discrepancy.
        Scenario(fixture, "focused-cues-visible-full-invalidation", true, [&](State& state) {
            SetFocus(state.hwnd);SendMessageA(state.hwnd,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
            fixture.Check((SendMessageA(state.hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)==0,"full-invalidation comparison retains visible native focus cues");
            InvalidateRect(state.hwnd,nullptr,FALSE);UpdateWindow(state.hwnd);
        });
        Scenario(fixture, "disabled", false, [&](State& state) { SetFocus(state.fixture->parent); EnableWindow(state.hwnd, FALSE);
            fixture.Check(!IsWindowEnabled(state.hwnd),"native control actually disabled"); });
        Scenario(fixture, "position", false, [&](State& state) {
            EnableWindow(state.hwnd, TRUE); SetFocus(state.fixture->parent); SendMessageA(state.hwnd, TBM_SETPOS, TRUE, state.logicalId == 447 ? 82 : 37);
            fixture.Check(IsWindowEnabled(state.hwnd)&&SendMessageA(state.hwnd,TBM_GETPOS,0,0)==(state.logicalId==447?82:37),"native position and enabled state actually applied");
        });
        Scenario(fixture, "resized", false, [&](State& state) {
            SetFocus(state.fixture->parent); SetWindowPos(state.hwnd, nullptr, 0, 0, 180, 28, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{};fixture.Check(GetClientRect(state.hwnd,&client)&&client.right==180&&client.bottom==28,"native resize actually applied");
        });
        Scenario(fixture, "keyboard", true, [&](State& state) {
            SetFocus(state.hwnd); const auto before = SendMessageA(state.hwnd, TBM_GETPOS, 0, 0);
            const auto downs = state.nativeKeyDowns, ups = state.nativeKeyUps, scrolls = fixture.scrolls;
            SendMessageA(state.hwnd, WM_KEYDOWN, VK_RIGHT, 1); SendMessageA(state.hwnd, WM_KEYUP, VK_RIGHT, LPARAM(0xc0000001u));
            fixture.Check(SendMessageA(state.hwnd, TBM_GETPOS, 0, 0) == before + 1 && fixture.scrolls > scrolls &&
                (!state.buffered || (state.nativeKeyDowns == downs + 1 && state.nativeKeyUps == ups + 1)),
                "native keyboard position/notifications pass through wrapper id=" + std::to_string(state.logicalId));
        });
        Scenario(fixture, "synthetic-held-move", true, [&](State& state) {
            GuardInput(fixture);Need(!GetCapture(),"synthetic down requires no existing capture, including this prototype's other controls");
            SetFocus(state.hwnd); RECT thumb{}; SendMessageA(state.hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            const int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
            const auto before = SendMessageA(state.hwnd, TBM_GETPOS, 0, 0);
            SendMessageA(state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            GuardInput(fixture);
            SendMessageA(state.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 8, y));
            fixture.Check(GetCapture() == state.hwnd && SendMessageA(state.hwnd, TBM_GETPOS, 0, 0) != before,
                "native synthetic captured move changes the thumb before final-image comparison");
        });
        Scenario(fixture, "supplied-HDC-passthrough", false, [&](State& state) {
            SetFocus(state.fixture->parent);
            if (state.buffered) {
                Surface surface; Need(surface.Prepare(state.hwnd, Failure::None, false), "prepare supplied-HDC passthrough");
                const auto count = state.count, native = state.nativePaints;
                SendMessageA(state.hwnd, WM_PAINT, reinterpret_cast<WPARAM>(surface.memory), 0);
                fixture.Check(state.count == count && state.nativePaints == native, "caller-supplied WM_PAINT HDC bypasses buffering unchanged");
                fixture.Check(From(surface) == NativeImage(state, kMarkerA, false), "caller-supplied HDC receives the exact same native image");
            }
        });
        Failures(fixture); Finish(fixture);
        std::printf("trackbar buffer prototype: %u checks, %u failures; evidence: %s\n", fixture.checks, fixture.failures, output.string().c_str());
        return fixture.failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "INCOMPLETE/FAILED native trackbar prototype: %s\n", error.what()); return 1;
    }
}
