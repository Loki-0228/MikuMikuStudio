// Include after slider_paint_trace_host_checks.inc in the disposable full host.
// Diagnostic only: do not substitute this supplied-HDC paint for production paint.
#include "../slider_paint_trace.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if !MMD_ORIGINAL_PROFILE
namespace light_slider_native_surface_checks {
namespace trace = slider_paint_trace;
namespace prior = slider_paint_trace_host_checks;
using mikumikustudio::MMDApp;
constexpr std::array<int, 6> kIds{455, 456, 457, 458, 459, 460};
constexpr std::array<std::uint32_t, 2> kMarkers{0x00e71195u, 0x000ddf61u};

struct Keys {
    MMDApp& app;
    const int frame;
    const mikumikustudio::mdl::CameraKey* camera;
    const mikumikustudio::mdl::LightKey* light;
    const std::uint64_t cameraIdentity, lightIdentity;
    const std::vector<mikumikustudio::mdl::CameraKey> cameraBytes;
    const std::vector<mikumikustudio::mdl::LightKey> lightBytes;
    explicit Keys(MMDApp& value) : app(value), frame(value.CurrentFrame()),
        camera(value.CameraKeys()), light(value.LightKeys()),
        cameraIdentity(mikumikustudio::mdl::KeyPoolIdentity(camera)),
        lightIdentity(mikumikustudio::mdl::KeyPoolIdentity(light)),
        cameraBytes(camera, camera + mikumikustudio::mdl::KeyPoolCapacity(camera)),
        lightBytes(light, light + mikumikustudio::mdl::KeyPoolCapacity(light)) {}
    void Check() const {
        using namespace mikumikustudio::mdl;
        trace::Require(app.CurrentFrame() == frame && app.CameraKeys() == camera && app.LightKeys() == light &&
            KeyPoolIdentity(camera) == cameraIdentity && KeyPoolIdentity(light) == lightIdentity &&
            KeyPoolCapacity(camera) == cameraBytes.size() && KeyPoolCapacity(light) == lightBytes.size() &&
            !std::memcmp(camera, cameraBytes.data(), cameraBytes.size() * sizeof(*camera)) &&
            !std::memcmp(light, lightBytes.data(), lightBytes.size() * sizeof(*light)),
            "native surface diagnosis preserves playhead and original VMD key pointers, identities, capacities and bytes");
    }
};

struct Session {
    MMDApp& app;
    const ULONGLONG started = GetTickCount64();
    const HWND initialFocus = GetFocus();
    HWND ownCapture = nullptr;
    std::array<UINT, 6> originalUi{};
    explicit Session(MMDApp& value) : app(value) {
        for (unsigned i = 0; i < kIds.size(); ++i)
            originalUi[i] = UINT(SendMessageW(GetDlgItem(app.MainWindow(), kIds[i]), WM_QUERYUISTATE, 0, 0));
    }
    ~Session() {
        // Never release a capture that another window has acquired.
        if (ownCapture && IsWindow(ownCapture) && GetCapture() == ownCapture) {
            SendMessageW(ownCapture, WM_CANCELMODE, 0, 0);
            if (GetCapture() == ownCapture) ReleaseCapture();
        }
        if (!GetCapture()) {
            if (initialFocus && IsWindow(initialFocus)) SetFocus(initialFocus);
            for (unsigned i = 0; i < kIds.size(); ++i) {
                const auto window = GetDlgItem(app.MainWindow(), kIds[i]);
                if (!window || !IsWindow(window)) continue;
                const auto actual = UINT(SendMessageW(window, WM_QUERYUISTATE, 0, 0));
                const auto set = originalUi[i] & ~actual, clear = actual & ~originalUi[i];
                if (set) SendMessageW(window, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, set), 0);
                if (clear) SendMessageW(window, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, clear), 0);
            }
        }
    }
    void Guard() const {
        trace::Require(GetTickCount64() - started < 20000, "INCOMPLETE native surface diagnosis exceeded its 20-second budget");
        for (int key = 1; key <= 255; ++key)
            if ((GetAsyncKeyState(key) & 0x8000) != 0)
                throw std::runtime_error("INCOMPLETE real held key/button prevents isolated native surface diagnosis; VK=" +
                    std::to_string(key));
        trace::Require(!GetCapture() || GetCapture() == ownCapture,
            "INCOMPLETE native surface diagnosis never takes foreign capture");
    }
    void Flush(HWND window) const {
        Guard();
        // Full final-window reference, not an observation of intermediate paint.
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        trace::Require(GdiFlush() != FALSE, "flush actual native slider painting");
    }
};

static trace::Snapshot WindowPixels(HWND window) {
    trace::Snapshot result;
    result.window = window; result.id = GetDlgCtrlID(window);
    RECT client{};
    trace::Require(GetClientRect(window, &client) && client.left == 0 && client.top == 0 &&
        client.right > 0 && client.bottom > 0 && client.right <= 2048 && client.bottom <= 128,
        "bounded actual light-slider client geometry");
    result.width = client.right; result.height = client.bottom;
    result.visible = IsWindowVisible(window) != FALSE;
    const HDC display = GetDC(window);
    trace::Require(display != nullptr, "obtain actual light-slider display DC");
    struct Release { HWND window; HDC dc; ~Release() { ReleaseDC(window, dc); } } release{window, display};
    trace::Require(GetObjectType(display) == OBJ_DC, "actual window evidence must use a display DC");
    result.clipKind = GetClipBox(display, &result.clip);
    trace::Require(result.clipKind == SIMPLEREGION && result.clip.left <= 0 && result.clip.top <= 0 &&
        result.clip.right >= result.width && result.clip.bottom >= result.height,
        "INCOMPLETE entire actual light-slider client must have a readable display clip");
    trace::Dib target(result.width, result.height, display);
    trace::Require(bool(target) && BitBlt(target.dc, 0, 0, result.width, result.height, display, 0, 0, SRCCOPY) && GdiFlush(),
        "copy actual light-slider pixels to a bounded DIB");
    result.pixels.assign(target.pixels, target.pixels + std::size_t(result.width) * result.height);
    result.copied = true;
    result.tick = int(SendMessageW(window, TBM_GETPOS, 0, 0));
    SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&result.thumb));
    trace::Require(result.Live() && result.Varied(), "INCOMPLETE actual window evidence must be visible and nonuniform");
    return result;
}

static void Bitmap(const std::filesystem::path& path, const trace::Snapshot& image) {
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER header{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + DWORD(image.pixels.size() * 4);
    header.biSize = sizeof(header); header.biWidth = image.width; header.biHeight = -image.height;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(image.pixels.data()), std::streamsize(image.pixels.size() * 4));
    trace::Require(bool(stream), "write bounded actual light-slider surface evidence");
}

static std::size_t Difference(const trace::Snapshot& a, const trace::Snapshot& b,
    std::ofstream& differences, int id, const char* phase, const char* comparison) {
    trace::Require(a.copied && b.copied && a.width == b.width && a.height == b.height && a.pixels.size() == b.pixels.size(),
        "compare equal geometry for every native surface pixel");
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const auto before = a.pixels[i] & 0xffffffu, after = b.pixels[i] & 0xffffffu;
        if (before == after) continue;
        if (count < 32) differences << id << ',' << phase << ',' << comparison << ',' << i % a.width << ',' << i / a.width
            << ',' << before << ',' << after << '\n';
        ++count;
    }
    return count;
}

static trace::Snapshot NativeSurface(MMDApp& app, HWND window, const trace::Snapshot& live,
    std::uint32_t marker, LRESULT& returned) {
    trace::Dib target(live.width, live.height);
    trace::Require(bool(target), "prepare bounded supplied-HDC native surface");
    std::fill_n(target.pixels, live.pixels.size(), marker);
    const prior::PumpInvariant invariant(app);
    const auto focus = GetFocus(), capture = GetCapture();
    const auto ui = SendMessageW(window, WM_QUERYUISTATE, 0, 0);
    const auto tick = SendMessageW(window, TBM_GETPOS, 0, 0);
    // The documented supplied HDC is consumed by the captured native class
    // procedure exactly once. No WM_PRINTCLIENT, customdraw flags or subclass
    // return values are substituted by this diagnosis.
    returned = CallWindowProcA(app.OriginalTrackbarProc(), window, WM_PAINT,
        reinterpret_cast<WPARAM>(target.dc), 0);
    trace::Require(GdiFlush() != FALSE, "flush original native supplied-HDC rendering");
    invariant.Check();
    trace::Require(GetFocus() == focus && GetCapture() == capture &&
        SendMessageW(window, WM_QUERYUISTATE, 0, 0) == ui && SendMessageW(window, TBM_GETPOS, 0, 0) == tick,
        "native supplied-HDC rendering preserves focus, capture, UI cues and tick");
    trace::Snapshot result = live;
    result.borrowedDc = true;
    result.pixels.assign(target.pixels, target.pixels + live.pixels.size());
    return result;
}

struct Totals { unsigned groups = 0, comparisons = 0, failed = 0, moved = 0; };
static void Probe(Session& session, const Keys& keys, HWND window, const char* phase,
    const std::filesystem::path& output, std::ofstream& report, std::ofstream& differences, Totals& totals) {
    session.Guard(); session.Flush(window); keys.Check();
    const auto before = WindowPixels(window);
    const prior::PumpInvariant invariant(session.app);
    LRESULT resultA = 0, resultB = 0;
    const auto a = NativeSurface(session.app, window, before, kMarkers[0], resultA);
    const auto afterA = WindowPixels(window);
    const auto b = NativeSurface(session.app, window, before, kMarkers[1], resultB);
    const auto afterB = WindowPixels(window);
    invariant.Check(); keys.Check(); session.Guard();
    const auto id = GetDlgCtrlID(window);
    const std::array<std::size_t, 5> changed{
        Difference(before, a, differences, id, phase, "window-vs-native-marker-A"),
        Difference(before, b, differences, id, phase, "window-vs-native-marker-B"),
        Difference(a, b, differences, id, phase, "marker-A-vs-marker-B"),
        Difference(before, afterA, differences, id, phase, "actual-window-after-native-A"),
        Difference(before, afterB, differences, id, phase, "actual-window-after-native-B")};
    std::array<unsigned, 2> markers{};
    for (std::size_t i = 0; i < before.pixels.size(); ++i) {
        markers[0] += (a.pixels[i] & 0xffffffu) == kMarkers[0];
        markers[1] += (b.pixels[i] & 0xffffffu) == kMarkers[1];
    }
    report << "SURFACE id=" << id << " phase=" << phase << " width=" << before.width << " height=" << before.height
        << " tick=" << before.tick << " ui=" << SendMessageW(window, WM_QUERYUISTATE, 0, 0)
        << " focused=" << (GetFocus() == window) << " captured=" << (GetCapture() == window)
        << " window_A_diff=" << changed[0] << " window_B_diff=" << changed[1] << " marker_dependency_diff=" << changed[2]
        << " actual_after_A_diff=" << changed[3] << " actual_after_B_diff=" << changed[4]
        << " marker_A_pixels=" << markers[0] << " marker_B_pixels=" << markers[1]
        << " native_result_A=" << resultA << " native_result_B=" << resultB << '\n';
    for (auto count : changed) { ++totals.comparisons; totals.failed += count != 0; }
    ++totals.groups;
    const auto prefix = std::to_string(id) + "-" + phase;
    Bitmap(output / (prefix + "-window-before.bmp"), before);
    Bitmap(output / (prefix + "-native-A.bmp"), a);
    Bitmap(output / (prefix + "-native-B.bmp"), b);
    Bitmap(output / (prefix + "-window-after.bmp"), afterB);
    report.flush(); differences.flush(); session.Guard();
}

static void Run(MMDApp* app, const std::filesystem::path& outputDirectory) {
    trace::Require(app && app == mikumikustudio::g_Block && app->MainWindow() && app->OriginalTrackbarProc() &&
        app->CameraKeys() && app->LightKeys() && !app->PlaybackActive() && !app->FrameStepPlayback() && !GetCapture(),
        "native light surface diagnosis requires an idle disposable full MMS host with no capture");
    const auto output = outputDirectory / "native-surface";
    std::filesystem::create_directories(output);
    std::ofstream report(output / "native-surface-report.txt"), differences(output / "pixel-differences.csv");
    trace::Require(bool(report) && bool(differences), "open native light surface diagnostic reports");
    report << "Actual six MMS lighting trackbars, their unchanged styles/client geometry and current original ANSI native procedure. "
        "Each reference uses final actual GetDC pixels after an explicit whole-client native repaint; comparison uses one original WM_PAINT(HDC) per distinct marker. "
        "Synthetic down/move/up are explicitly labelled; no physical input, cursor movement, frame pump, DWM or flicker-elimination claim. "
        "All RGB pixels are compared without focus/tick masking. Marker independence tests full output initialization; marker counts are also reported. "
        "At most 24 groups/48 supplied-HDC calls/96 BMPs; hard cooperative budget 20 seconds. GetAsyncKeyState high bits are checked; its low-bit observation is not claimed untouched.\n";
    differences << "id,phase,comparison,x,y,before_rgb,after_rgb\n";
    Totals totals;
    const Keys keys(*app);
    Session session(*app);
    try {
        for (int id : kIds) {
            session.Guard();
            const auto window = GetDlgItem(app->MainWindow(), id);
            RECT client{};
            trace::Require(window && IsWindowVisible(window) && IsWindowEnabled(window) && GetClientRect(window, &client) &&
                client.right == (id < 458 ? 136 : 132) && client.bottom == (id < 458 ? 18 : 20),
                "INCOMPLETE exact production RGB 136x18 and direction 132x20 HWNDs are required");
            report << "CONTROL id=" << id << " style=" << GetWindowLongPtrW(window, GWL_STYLE) << '\n';
            prior::ClassIdentity(window, report);
            SetFocus(app->MainWindow());
            SendMessageW(window, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
            Probe(session, keys, window, "idle-hidden", output, report, differences, totals);
            SetFocus(window);
            SendMessageW(window, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
            Probe(session, keys, window, "focused-hidden", output, report, differences, totals);

            SetFocus(app->MainWindow()); session.Flush(window); session.Guard();
            RECT thumb{}; SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            const POINT down{(thumb.left + thumb.right) / 2, (thumb.top + thumb.bottom) / 2};
            trace::Require(down.x >= 0 && down.x < client.right && down.y >= 0 && down.y < client.bottom,
                "native production light thumb center lies inside its real HWND");
            const int originalTick = int(SendMessageW(window, TBM_GETPOS, 0, 0));
            session.ownCapture = window;
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(down.x, down.y));
            trace::Require(GetCapture() == window && GetFocus() == window,
                "synthetic native light button-down owns actual focus and capture");
            session.Guard();
            const POINT move{(std::clamp)(down.x + (down.x < client.right / 2 ? 8 : -8), LONG(0), client.right - 1), down.y};
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(move.x, move.y));
            const int tick = int(SendMessageW(window, TBM_GETPOS, 0, 0));
            trace::Require(GetCapture() == window && (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) &&
                tick != originalTick && prior::Authored(*app, id, tick),
                "captured actual native MOVE changes tick with exact original lighting arithmetic and hidden focus cue");
            ++totals.moved;
            Probe(session, keys, window, "captured-moved", output, report, differences, totals);
            session.Guard();
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(move.x, move.y));
            trace::Require(!GetCapture(), "synthetic native light button-up releases its capture");
            session.ownCapture = nullptr;
            SetFocus(window);
            SendMessageW(window, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            trace::Require(GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS),
                "focused-visible control remains an unmasked strict native comparison");
            Probe(session, keys, window, "focused-visible", output, report, differences, totals);
        }
        keys.Check(); session.Guard();
    } catch (const std::exception& error) {
        report << "INCOMPLETE groups=" << totals.groups << " comparisons=" << totals.comparisons << " known_failed_comparisons="
            << totals.failed << " elapsed_ms=" << GetTickCount64() - session.started << " reason=" << error.what() << '\n';
        report.flush(); throw;
    }
    report << (totals.failed ? "FAILED" : "PASS") << " groups=" << totals.groups << " comparisons=" << totals.comparisons
        << " failed_comparisons=" << totals.failed << " real_native_moved_controls=" << totals.moved
        << " elapsed_ms=" << GetTickCount64() - session.started << '\n';
    report.flush();
    trace::Require(totals.groups == 24 && totals.moved == 6 && totals.failed == 0,
        "all six actual lighting native surfaces must initialize completely, match actual-window RGB exactly and leave that window unchanged");
}
} // namespace light_slider_native_surface_checks
#endif
