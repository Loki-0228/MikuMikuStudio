// Capture the actual child paint after the divider's WM_MOUSEMOVE. The
// production update region and destination DC are left untouched.
namespace bottom_live_drag {
struct Paint {
    RECT client{}, dirty{}, thumb{}, channel{};
    POINT origin{};
    std::vector<DWORD> pixels;
};
HWND host = nullptr;
bool observing = false;
std::array<std::vector<Paint>, 3> paints;
decltype(&EndPaint) realEnd = nullptr;

LRESULT CALLBACK AllowWideFixture(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR width) {
    const LRESULT result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_GETMINMAXINFO) {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lp);
        limits.ptMaxTrackSize.x = std::max(limits.ptMaxTrackSize.x, static_cast<LONG>(width));
        return 0;
    }
    return result;
}

template<class Draw>
std::vector<DWORD> Pixels(HDC compatible, int width, int height, Draw draw) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const auto dc = CreateCompatibleDC(compatible);
    const auto bitmap = CreateDIBSection(compatible, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(dc && bitmap && bits, "allocate the live slider paint sample");
    const auto previous = SelectObject(dc, bitmap);
    Check(previous && previous != HGDI_ERROR, "select the live paint sample bitmap");
    draw(dc);
    Check(GdiFlush() != FALSE, "flush the live slider sample");
    const auto* begin = static_cast<const DWORD*>(bits);
    std::vector<DWORD> result(begin, begin + size_t(width) * height);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return result;
}

BOOL WINAPI ObserveEnd(HWND window, const PAINTSTRUCT* ps) {
    if (observing && ps && ps->hdc && GetParent(window) == host && !IsRectEmpty(&ps->rcPaint)) {
        const int index = GetDlgCtrlID(window) - panel::kLightDirSliderX;
        if (index >= 0 && index < static_cast<int>(paints.size())) {
            Paint sample;
            sample.dirty = ps->rcPaint;
            Check(GetClientRect(window, &sample.client) && !IsRectEmpty(&sample.client) &&
                  ClientToScreen(window, &sample.origin) && ScreenToClient(host, &sample.origin),
                "observe the actual updated lighting slider client and its position");
            SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&sample.thumb));
            SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&sample.channel));
            sample.pixels = Pixels(ps->hdc, sample.client.right, sample.client.bottom, [&](HDC dc) {
                Check(BitBlt(dc, 0, 0, sample.client.right, sample.client.bottom, ps->hdc, 0, 0, SRCCOPY),
                    "read the real painted lighting slider DC");
            });
            paints[index].push_back(std::move(sample));
        }
    }
    return realEnd(window, ps);
}

void CheckPaint(const Paint& sample, int id, int width) {
    Check(EqualRect(&sample.client, &sample.dirty), "moving a slider invalidates its complete client");
    const auto background = Pixels(nullptr, sample.client.right, sample.client.bottom, [&](HDC dc) {
        SetViewportOrgEx(dc, -sample.origin.x, -sample.origin.y, nullptr);
        HandlePaletteChanged(dc);
    });
    const auto changed = [&](RECT region, bool excludeThumb) {
        IntersectRect(&region, &region, &sample.client);
        unsigned count = 0;
        for (int y = region.top; y < region.bottom; ++y) {
            for (int x = region.left; x < region.right; ++x) {
                if (excludeThumb && PtInRect(&sample.thumb, {x, y})) continue;
                const size_t offset = size_t(y) * sample.client.right + x;
                count += ((sample.pixels[offset] ^ background[offset]) & 0xffffffu) != 0;
            }
        }
        return count;
    };
    const auto thumb = changed(sample.thumb, false), track = changed(sample.channel, true);
    const size_t margin = 2u * sample.client.right + 2;
    const bool sameMargin = ((sample.pixels[margin] ^ background[margin]) & 0xffffffu) == 0;
    std::printf("LIVE_DIVIDER width=%d id=%d thumb=%u track=%u margin=%u\n", width, id, thumb, track, unsigned(sameMargin));
    Check(thumb >= 8 && track >= 8 && sameMargin,
        "the submitted track and thumb pixels use the current template gradient during capture");
}

void Run(MMDApp* app) {
    host = app->MainWindow();
    app->CameraMode() = 1;
    app->UiOptionFlag(1) = 0;
    app->UiOptionFlag(2) = 1;
    app->UiOptionFlag(3) = 0;
    app->UiOptionFlag(6) = 0;
    HandleWindowSize(app);
    int percent = 100;
    constexpr int grip = 4;
    const auto unexpanded = BottomPanelSpans(app);
    const auto initial = BottomPanelSpan(unexpanded, grip);
    const int extra = kLightTwoColumnWidth + 20 - (initial.right - initial.left);
    RECT client{}, outer{};
    Check(GetClientRect(host, &client) && GetWindowRect(host, &outer), "measure the fixture window and its frame");
    const int requested = ui_scale::Percent();
    const int needed = MulDiv(unexpanded.back().right + extra + 8, requested, 100);
    auto* renderer = app->Renderer();
    if (renderer->presentParameters.BackBufferWidth < static_cast<UINT>(needed)) {
        renderer->screenWidth = needed;
        renderer->presentParameters.BackBufferWidth = needed;
        Check(PostDeviceReset(app), "allocate a wider real D3D9 backbuffer for this fixture");
    }
    IDirect3DSurface9* backbuffer = nullptr;
    D3DSURFACE_DESC backing{};
    Check(SUCCEEDED(renderer->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
          backbuffer && SUCCEEDED(backbuffer->GetDesc(&backing)), "measure the actual D3D9 backbuffer");
    backbuffer->Release();
    Check(backing.Width >= static_cast<UINT>(needed), "the renderer can back the requested client width");
    bool extendedTrackLimit = false;
    constexpr UINT_PTR wideFixtureSubclass = 1;
    if (client.right < needed) {
        const int border = outer.right - outer.left - client.right;
        const int width = needed + border;
        extendedTrackLimit = width > GetSystemMetrics(SM_CXMAXTRACK);
        if (extendedTrackLimit)
            Check(SetWindowSubclass(host, AllowWideFixture, wideFixtureSubclass, width),
                "extend only this fixture's native tracking limit");
        Check(SetWindowPos(host, nullptr, 0, 0, width, outer.bottom - outer.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
            "allocate enough width to cross the column boundary at the requested scale");
    }
    app->bottomPanelSizing.extra[grip] = extra;
    HandleWindowSize(app);
    const auto spans = BottomPanelSpans(app, &percent);
    const auto start = BottomPanelSpan(spans, grip);
    const int startWidth = start.right - start.left + start.extra;
    GetClientRect(host, &client);
    std::printf("LIVE_DIVIDER setup requested=%d effective=%d start_width=%d client=%ld required_client=%d extended_track_limit=%d backbuffer=%u\n",
        requested, percent, startWidth, client.right, needed, extendedTrackLimit, backing.Width);
    Check(percent == requested && startWidth == kLightTwoColumnWidth + 20,
        "reserve actual layout space for the two-column transition without scale compression");

    // Make this fixture receive real update regions without exposing a visible
    // window or taking focus on the user's desktop.
    const auto extended = GetWindowLongPtrW(host, GWL_EXSTYLE);
    constexpr auto required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    SetWindowLongPtrW(host, GWL_EXSTYLE, extended | required);
    BYTE alpha = 255;
    DWORD flags = 0;
    Check((GetWindowLongPtrW(host, GWL_EXSTYLE) & required) == required &&
          SetLayeredWindowAttributes(host, 0, 0, LWA_ALPHA) &&
          GetLayeredWindowAttributes(host, nullptr, &alpha, &flags) && alpha == 0 && (flags & LWA_ALPHA),
        "verify the fixture is fully transparent and nonactivating before allowing native paints");
    Check(SetWindowPos(host, HWND_TOPMOST, 16, 16, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE),
        "keep the transparent host in the monitor's paintable area");
    originalShow(host, SW_SHOWNOACTIVATE);
    Check(IsWindowVisible(host) && GetForegroundWindow() != host, "the transparent fixture is never the foreground window");
    RedrawWindow(host, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    Hook(&EndPaint, &ObserveEnd, &realEnd);

    GetClientRect(host, &client);
    const int x = BottomPanelX(spans, start.right, percent);
    const int y = MainPanelLayout(app, client.bottom).bottomTop + 10;
    Check(HitBottomPanelEdge(app, x, y) == grip, "press the actual lighting panel divider");
    SendMessageW(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    unsigned crossed = 0, steps = 0, frameCount = 0;
    bool lastColumns = true;
    for (int offset : {16, 10, 4, 0, -4, -12, -4, 0, 4, 10, 16, 20}) {
        std::array<RECT, 3> old{};
        for (unsigned i = 0; i < old.size(); ++i) old[i] = ControlBounds(host, panel::kLightDirSliderX + i);
        for (auto& frames : paints) frames.clear();
        observing = true;
        const int next = x + MulDiv(kLightTwoColumnWidth + offset - startWidth, percent, 100);
        SendMessageW(host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(next, y));
        Check(GetCapture() == host && app->bottomPanelSizing.dragging == grip,
            "every intermediate paint occurs while the same divider owns capture");
        const auto span = BottomPanelSpan(BottomPanelSpans(app), grip);
        const int width = span.right - span.left + span.extra;
        const bool columns = ControlBounds(host, panel::kLightDirSliderX).top ==
                             ControlBounds(host, panel::kLightColorSliderR).top;
        crossed += columns != lastColumns;
        lastColumns = columns;
        for (unsigned i = 0; i < old.size(); ++i) {
            const int id = panel::kLightDirSliderX + i;
            const auto actual = ControlBounds(host, id);
            const HWND slider = GetDlgItem(host, id);
            UpdateWindow(slider); // Consume pending damage, without invalidating anything in the test.
            if (!EqualRect(&old[i], &actual)) Check(!paints[i].empty(), "the divider schedules an actual paint for each moved slider");
            for (const auto& frame : paints[i]) {
                Check(frame.origin.x == actual.left && frame.origin.y == actual.top &&
                      frame.client.right == actual.right - actual.left &&
                      frame.client.bottom == actual.bottom - actual.top,
                    "every submitted slider paint uses the geometry committed for that mouse move");
                CheckPaint(frame, id, width);
                ++frameCount;
            }
        }
        observing = false;
        ++steps;
    }
    SendMessageW(host, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Check(crossed == 2 && frameCount >= steps * 3 && GetCapture() != host && app->bottomPanelSizing.dragging < 0,
        "a complete held gesture crosses both layout directions and retires normally");
    originalShow(host, SW_HIDE);
    MH_DisableHook(reinterpret_cast<void*>(&EndPaint));
    MH_RemoveHook(reinterpret_cast<void*>(&EndPaint));
    if (extendedTrackLimit)
        Check(RemoveWindowSubclass(host, AllowWideFixture, wideFixtureSubclass), "restore the test window's normal tracking limit");
    host = nullptr;
    std::printf("PASS live lighting divider paint scale=%d steps=%u paints=%u crossings=%u\n",
        ui_scale::Percent(), steps, frameCount, crossed);
}
} // namespace bottom_live_drag
