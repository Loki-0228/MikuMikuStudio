// Hidden production painting: validate translated header bounds against the
// real panel borders and the native volume control, then save one review image.
#pragma once
#include <fstream>
#include "mikumikustudio/ui_language.hpp"

namespace bottom_panel_checks {
struct Header { std::wstring text; RECT bounds; SIZE extent; UINT format; };
inline HDC capture = nullptr;
inline HWND host = nullptr;
inline std::vector<Header> headers;
inline decltype(&BeginPaint) originalBegin = nullptr;
inline decltype(&EndPaint) originalEnd = nullptr;
inline decltype(&DrawTextW) originalText = nullptr;
inline HDC WINAPI Begin(HWND window, LPPAINTSTRUCT paint) {
    if (capture && window == host) {
        *paint = {}; paint->hdc = capture; GetClientRect(window, &paint->rcPaint); return capture;
    }
    return originalBegin(window, paint);
}
inline BOOL WINAPI End(HWND window, const PAINTSTRUCT* paint) {
    return capture && window == host ? TRUE : originalEnd(window, paint);
}
inline int WINAPI Text(HDC dc, LPCWSTR text, int length, LPRECT bounds, UINT format) {
    if (dc == capture && (format & DT_CENTER) && !(format & DT_CALCRECT)) {
        const int count = length < 0 ? int(wcslen(text)) : length;
        Header header{std::wstring(text, count), *bounds, {}, format};
        GetTextExtentPoint32W(dc, text, count, &header.extent);
        headers.push_back(header);
    }
    return originalText(dc, text, length, bounds, format);
}
inline void SavePanelImage(const std::filesystem::path& output, HDC dc, int width, int height) {
    // Composite child controls only for the fully expanded camera panel.
    EnumChildWindows(host, [](HWND child, LPARAM parameter) -> BOOL {
        if (GetParent(child) != host || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) return TRUE;
        RECT rect{}, client{}; GetWindowRect(child, &rect); GetClientRect(host, &client);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&rect), 2);
        if (rect.top < client.bottom - 160) return TRUE;
        HDC dc = reinterpret_cast<HDC>(parameter);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
        SetViewportOrgEx(dc, rect.left, rect.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
        return TRUE;
    }, reinterpret_cast<LPARAM>(dc));
    constexpr int stripHeight = 160;
    BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -stripHeight, 1, 32, BI_RGB};
    void* pixels = nullptr;
    HDC strip = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateDIBSection(strip, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Require(bitmap && pixels, "create bottom panel review image");
    const auto previous = SelectObject(strip, bitmap);
    BitBlt(strip, 0, 0, width, stripHeight, dc, 0, height - stripHeight, SRCCOPY);
    GdiFlush();
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + width * stripHeight * 4;
    std::ofstream stream(output, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels), width * stripHeight * 4);
    Require(bool(stream), "save bottom panel review image");
    SelectObject(strip, previous); DeleteObject(bitmap); DeleteDC(strip);
}
inline void Run(MMDApp* app) {
    host = app->MainWindow();
    const auto fixture = std::filesystem::absolute(L"fixture.pmx");
    LoadModelFile(app, fixture.c_str(), false);
    Require(app->SelectedModel() != nullptr, "load fixture for both camera and model panel modes");
    Hook(&BeginPaint, &Begin, &originalBegin);
    Hook(&EndPaint, &End, &originalEnd);
    Hook(&DrawTextW, &Text, &originalText);
    app->EnglishUI() = kUiChinese;
    LocalizeUI(app);
    for (int width : {1280, 1920}) {
        SetWindowPos(host, nullptr, 0, 0, width, 820, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
        RECT client{}; GetClientRect(host, &client);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32, BI_RGB};
        void* pixels = nullptr;
        capture = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(capture, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        Require(bitmap && pixels, "create hidden production paint surface");
        const auto previous = SelectObject(capture, bitmap);
        for (bool camera : {true, false}) for (int flags = 0; flags < (camera ? 16 : 4); ++flags) {
            app->CameraMode() = camera;
            app->UiOptionFlag(1) = flags & 1;
            app->UiOptionFlag(2) = (flags >> 1) & 1;
            app->UiOptionFlag(3) = (flags >> 2) & 1;
            app->UiOptionFlag(6) = (flags >> 3) & 1;
            app->UiOptionFlag(4) = flags & 1;
            app->UiOptionFlag(5) = (flags >> 1) & 1;
            HandleWindowSize(app);
            app->WindowLayoutReady() = 0; // GDI-only verification, no nested scene presentation.
            headers.clear();
            HandleWindowPaint(app);
            unsigned expected = 5; // interpolation, model, view, play, volume
            for (int bit = 0; bit < (camera ? 4 : 2); ++bit) expected += (flags >> bit) & 1;
            Require(headers.size() == expected, "all expanded panels have exactly one translated header");
            bool fits = true, borders = true, centered = true, volumeFits = false;
            for (const auto& header : headers) {
                const auto& r = header.bounds;
                fits &= header.extent.cx <= r.right - r.left && header.extent.cy <= r.bottom - r.top;
                // Compare with actual GDI border pixels, not a copied layout formula.
                borders &= GetPixel(capture, r.left - 1, r.top + 20) == RGB(0, 0, 0) &&
                           GetPixel(capture, r.right + 1, r.top + 20) == RGB(0, 0, 0) &&
                           GetPixel(capture, (r.left + r.right) / 2, r.top - 1) == RGB(0, 0, 0);
                centered &= (header.format & (DT_CENTER | DT_VCENTER)) == (DT_CENTER | DT_VCENTER);
                if (header.text == L"音量") {
                    RECT control{}; GetWindowRect(GetDlgItem(host, panel::kFrameVolumeSlider), &control);
                    MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&control), 2);
                    volumeFits = control.left >= r.left && control.right <= r.right &&
                                 control.top >= r.bottom && control.bottom < client.bottom - 3;
                }
                if (!fits || !borders) std::printf("HEADER bounds=%ld,%ld,%ld,%ld extent=%ld,%ld\n",
                    r.left, r.top, r.right, r.bottom, header.extent.cx, header.extent.cy);
            }
            Check(fits && borders && centered, "translated headers fit and center inside painted panel borders");
            Check(volumeFits, "volume slider stays inside its panel below the title");
            if (camera && flags == 15)
                SavePanelImage(std::filesystem::path(L"bottom-panels-" + std::to_wstring(width) + L".bmp"),
                    capture, client.right, client.bottom);
        }
        SelectObject(capture, previous); DeleteObject(bitmap); DeleteDC(capture); capture = nullptr;
    }
    Check(!IsWindowVisible(host), "layout verification stays hidden");
    MH_DisableHook(&BeginPaint); MH_DisableHook(&EndPaint); MH_DisableHook(&DrawTextW);
}
}
