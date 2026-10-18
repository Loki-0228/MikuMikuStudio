// Include into a full, disposable scene host. Invoke before binding for ordinary
// coverage and after real NativePoseTestBinding for bound coverage, with separate
// fresh output folders. This authors live slider values, but never saves a scene.
#include "../slider_paint_trace.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../../src/plugins/native_pose_scope.hpp"
#endif
#include <cstdio>
#include <cstring>

namespace slider_paint_trace_host_checks {
namespace trace = slider_paint_trace;
static constexpr std::array<int, 7> kIds{447, 455, 456, 457, 458, 459, 460};
struct Summary {
    unsigned controls = 0, dragged = 0, liveControls = 0, eraseExposedThumb = 0;
    unsigned parentPixelChanges = 0, peerPixelChanges = 0, paintEvents = 0, eraseEvents = 0;
};
static bool SamePixels(const trace::Snapshot& a, const trace::Snapshot& b) {
    return a.copied && b.copied && a.width == b.width && a.height == b.height && trace::Changed(a, b) == 0;
}
static void Flush(HWND parent, const std::array<HWND, 7>& controls) {
    UpdateWindow(parent);
    for (auto control : controls) UpdateWindow(control);
    GdiFlush();
}
static bool Authored(mikudancestudio::MMDApp& app, int id, int position) {
    if (id == 447) return app.CameraFov() == float(position);
    const unsigned lane = unsigned(id - 455);
    return lane < 3 ? app.LightColor()[lane] == float(double(position) * .00390625) :
        app.LightDirection()[lane - 3] == float(double(position) / 100.0);
}
static void ControlledErase(trace::Trace& observer, HWND control, std::ofstream& report, Summary& summary) {
    RECT rect{}; GetClientRect(control, &rect); trace::Dib target(rect.right, rect.bottom);
    trace::Require(bool(target), "create controlled native erase DIB");
    // Calibrate the pixel observer against a deterministic change. This is a
    // sampler self-check, not a substituted native erase or a production fix.
    PatBlt(target.dc, 0, 0, rect.right, rect.bottom, WHITENESS);
    const auto white = observer.Read(control, target.dc);
    PatBlt(target.dc, 0, 0, rect.right, rect.bottom, BLACKNESS);
    const auto black = observer.Read(control, target.dc);
    trace::Require(trace::Changed(white, black) == std::size_t(rect.right) * rect.bottom, "pixel observer detects every calibrated RGB change");
    const auto id = GetDlgCtrlID(control);
    observer.Phase(std::to_string(id) + "-controlled-native-print");
    SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target.dc), PRF_CLIENT);
    const auto drawn = observer.Read(control, target.dc);
    observer.Phase(std::to_string(id) + "-controlled-native-erase");
    const auto erasedResult = SendMessageW(control, WM_ERASEBKGND, reinterpret_cast<WPARAM>(target.dc), 0);
    const auto erased = observer.Read(control, target.dc);
    observer.Phase(std::to_string(id) + "-controlled-native-restore");
    SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target.dc), PRF_CLIENT);
    const auto restored = observer.Read(control, target.dc);
    trace::Require(drawn.copied && drawn.Varied() && restored.copied && restored.Varied(), "native reference and restore contain real track/thumb pixels");
    const bool exposed = !drawn.ThumbUniform() && erased.ThumbUniform() && trace::Changed(drawn, erased) != 0;
    summary.eraseExposedThumb += exposed;
    report << "CONTROLLED id=" << id << " erase_result=" << erasedResult << " changed=" << trace::Changed(drawn, erased)
        << " thumb_uniform_before=" << drawn.ThumbUniform() << " thumb_uniform_after=" << erased.ThumbUniform()
        << " native_restore_matches=" << SamePixels(drawn, restored) << " exposed_thumb=" << exposed
        << " (owned DIB evidence; not proof that this intermediate reaches the screen)\n";
}
static Summary Run(mikudancestudio::MMDApp* app, const std::filesystem::path& output,
    bool expectedBound, bool requireLivePixels = true) {
    using namespace mikudancestudio;
    trace::Require(app && app == g_Block && app->MainWindow() && app->Renderer() && app->Renderer()->device,
        "slider paint tracing requires the actual isolated scene host and device");
    trace::Require(GetWindowThreadProcessId(app->MainWindow(), nullptr) == GetCurrentThreadId() &&
        IsWindowEnabled(app->MainWindow()) && !GetCapture() && !app->PlaybackActive() && !app->FrameStepPlayback(),
        "slider paint tracing requires an idle enabled UI-thread fixture without another capture");
#if !MMD_ORIGINAL_PROFILE
    trace::Require(bool(plugins::BoundNativePoseHistory(app)) == expectedBound, "trace mode matches actual native history binding");
#else
    trace::Require(!expectedBound, "original profile can trace only the actual ordinary route");
#endif
    std::array<HWND, 7> controls{};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        controls[i] = GetDlgItem(app->MainWindow(), kIds[i]); RECT rect{};
        trace::Require(controls[i] && IsWindowVisible(controls[i]) && IsWindowEnabled(controls[i]) && GetClientRect(controls[i], &rect) &&
            rect.right >= 40 && rect.bottom >= 12, "expand camera/light bands and mount all seven real sliders before tracing");
    }
    const auto frame = app->CurrentFrame();
    trace::Require(app->CameraKeys() && app->LightKeys(), "tracing fixture has original camera and light key pools");
    const std::vector<mdl::CameraKey> cameraKeys(app->CameraKeys(), app->CameraKeys() + mdl::KeyPoolCapacity(app->CameraKeys()));
    const std::vector<mdl::LightKey> lightKeys(app->LightKeys(), app->LightKeys() + mdl::KeyPoolCapacity(app->LightKeys()));
    std::filesystem::create_directories(output); std::ofstream report(output / "paint-report.txt");
    trace::Require(bool(report), "create slider paint report");
    report << "Native slider erase/paint diagnostic; mode=" << (expectedBound ? "bound" : "ordinary")
        << "; no WM_PAINT substitution; no scene save; no claim of flicker removal.\n";
    report << "update_* is the native pending update bounding rectangle before default processing; "
        << "clip_* belongs to the borrowed erase/print DC when borrowed_dc=1, otherwise a diagnostic GetDC snapshot. "
        << "The observer never calls BeginPaint before the native procedure.\n";
    using ThemeQuery = BOOL(WINAPI*)();
    const auto themeModule = GetModuleHandleW(L"uxtheme.dll");
    const auto themed = themeModule ? reinterpret_cast<ThemeQuery>(GetProcAddress(themeModule, "IsAppThemed")) : nullptr;
    report << "app_themed=" << (themed ? int(themed() != FALSE) : -1)
        << " parent_style=" << std::hex << GetWindowLongPtrW(app->MainWindow(), GWL_STYLE) << std::dec << '\n';
    Summary summary; trace::Trace observer(app->MainWindow(), controls);
    struct CaptureGuard { std::array<HWND, 7>& windows; ~CaptureGuard() {
        for (auto window : windows) if (GetCapture() == window) { SendMessageW(window, WM_CANCELMODE, 0, 0); if (GetCapture() == window) ReleaseCapture(); }
    } } capture{controls};
    try {
        // Native WM_PAINT warms theme state. WM_PRINTCLIENT is not used here.
        observer.Phase("warm-real-paint");
        for (auto control : controls) RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        Flush(app->MainWindow(), controls);
        for (auto control : controls) {
            const auto initial = observer.Read(control); ++summary.controls;
            const bool qualified = initial.Live() && initial.Varied(); summary.liveControls += qualified;
            report << "LIVE id=" << initial.id << " qualified=" << qualified << " clip=" << initial.clipKind << " varied=" << initial.Varied()
                << " tick=" << initial.tick << " size=" << initial.width << 'x' << initial.height
                << " style=" << std::hex << GetWindowLongPtrW(control, GWL_STYLE) << std::dec << '\n';
            if (requireLivePixels) trace::Require(qualified, "live slider pixels unavailable: use a visible on-screen fixture on a private desktop");
            observer.Phase(std::to_string(initial.id) + "-real-drag");
            RECT thumb{}, channel{}; SendMessageW(control, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(control, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            const int y = (thumb.top + thumb.bottom) / 2, x = (thumb.left + thumb.right) / 2;
            SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            trace::Require(GetCapture() == control, "actual native trackbar begins drag capture");
            bool moved = false; int lastX = x;
            for (int fraction : {1, 3, 2, 1}) {
                lastX = int(channel.left + (channel.right - channel.left) * fraction / 4);
                SendMessageW(control, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(lastX, y));
                Flush(app->MainWindow(), controls);
                const int tick = int(SendMessageW(control, TBM_GETPOS, 0, 0)); moved |= tick != initial.tick;
                trace::Require(Authored(*app, initial.id, tick), "native drag notification authors the exact FOV/RGB/direction value");
            }
            SendMessageW(control, WM_LBUTTONUP, 0, MAKELPARAM(lastX, y)); Flush(app->MainWindow(), controls);
            trace::Require(GetCapture() != control && moved, "native drag moves and releases without synthetic TBM_SETPOS"); ++summary.dragged;
            observer.Phase(std::to_string(initial.id) + "-same-tick-parent-paint");
            InvalidateRect(app->MainWindow(), nullptr, FALSE); UpdateWindow(app->MainWindow());
            ControlledErase(observer, control, report, summary);
        }
        trace::Require(app->CurrentFrame() == frame && cameraKeys.size() == mdl::KeyPoolCapacity(app->CameraKeys()) &&
            lightKeys.size() == mdl::KeyPoolCapacity(app->LightKeys()) &&
            !std::memcmp(cameraKeys.data(), app->CameraKeys(), cameraKeys.size() * sizeof(mdl::CameraKey)) &&
            !std::memcmp(lightKeys.data(), app->LightKeys(), lightKeys.size() * sizeof(mdl::LightKey)),
            "drawing trace and live author drags preserve playhead and original VMD key fields");
        trace::Require(observer.Complete(), "paint trace completed without capture exceptions or quota truncation");
    } catch (...) { observer.Write(output); report << "FAILED: evidence retained; no flicker conclusion.\n"; report.flush(); throw; }
    for (const auto& event : observer.Events()) {
        summary.paintEvents += !event.parent && event.message == WM_PAINT;
        summary.eraseEvents += !event.parent && event.message == WM_ERASEBKGND;
        for (const auto& pair : event.targets) {
            const auto& a = pair.before; const auto& b = pair.after;
            if (!a.Live() || !b.Live() || !trace::Changed(a, b) || a.tick != b.tick || a.paints != b.paints || a.erases != b.erases) continue;
            if (event.parent && event.message == WM_PAINT) ++summary.parentPixelChanges;
            if (!event.parent && (event.message == WM_PAINT || event.message == WM_ERASEBKGND) && a.window != event.sourceWindow) ++summary.peerPixelChanges;
        }
    }
    observer.Write(output);
    report << "SUMMARY controls=" << summary.controls << " dragged=" << summary.dragged << " live_qualified=" << summary.liveControls
        << " native_paints=" << summary.paintEvents << " native_erases=" << summary.eraseEvents
        << " parent_changes_without_child_paint=" << summary.parentPixelChanges << " peer_changes_without_peer_paint=" << summary.peerPixelChanges
        << " controlled_erase_exposed_thumb=" << summary.eraseExposedThumb << '\n'
        << "Pixel changes identify reproducible drawing stages, not the duration or visibility of a desktop flash. "
        << "Zero counts do not prove absence of flicker; inspect live/DIB classification and before/after artifacts.\n";
    report.flush(); trace::Require(bool(report), "finish slider paint report");
    std::printf("SLIDER_PAINT mode=%s controls=%u dragged=%u live=%u parent_pixel_changes=%u peer_pixel_changes=%u erase_exposed=%u (no visual-fix claim)\n",
        expectedBound ? "bound" : "ordinary", summary.controls, summary.dragged, summary.liveControls,
        summary.parentPixelChanges, summary.peerPixelChanges, summary.eraseExposedThumb);
    return summary;
}
} // namespace slider_paint_trace_host_checks
