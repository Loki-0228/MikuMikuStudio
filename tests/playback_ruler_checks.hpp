#pragma once

static void CheckPlaybackRuler(const char* outputDirectory) {
    constexpr int width = 1100, height = 130;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HBITMAP blank = CreateBitmap(width, height, 1, 32, nullptr);
    HDC dc = CreateCompatibleDC(nullptr), blankDc = CreateCompatibleDC(nullptr);
    HWND window = CreateWindowW(L"STATIC", L"Playback ruler regression", WS_POPUP,
        0, 0, width, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(bitmap && blank && dc && blankDc && window, "create hidden native ruler fixture");
    const auto oldBitmap = SelectObject(dc, bitmap);
    const auto oldBlank = SelectObject(blankDc, blank);
    auto app = std::make_unique<MMDApp>(); app->state = {};
    auto renderer = std::make_unique<D3DRenderer>();
    renderer->screenWidth = width; renderer->screenHeight = height;
    app->Renderer() = renderer.get(); app->PanelDC() = dc;
    app->state.bmpPanelSpare = blank; app->Hwnd() = window;
    app->SidebarWidth() = width; app->EnglishUI() = 1;
    app->state.optflag[0] = 1; app->state.jointLineMap[0] = -1;
    app->CurrentFrame() = 3;
    app->ThemeColorAt(25) = 1250132; app->ThemeColorAt(26) = 3946345;
    app->ThemeColorAt(30) = 9895830; app->ThemeColorAt(31) = 16750230;
    app->ThemeColorAt(32) = 4934475; app->ThemeColorAt(33) = 0;
    app->ThemeColorAt(34) = 6579455;
    const auto fill = [&](RECT area, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color); FillRect(blankDc, &area, brush); DeleteObject(brush);
    };
    fill({0, 0, width, height}, RGB(45, 45, 45));
    fill({0, 0, width, 16}, 5401478);
    for (int row = 0; row != 8; ++row) {
        const int top = 16 + row * 14;
        fill({0, top, width, top + 13}, row % 2 ? RGB(24, 24, 24) : RGB(52, 52, 52));
    }
    SelectObject(blankDc, oldBlank); DeleteDC(blankDc);
    for (int fps : {30, 60, 24, 59, 1}) {
        app->PlaybackTiming().playbackFps = fps;
        PanelPaint(app.get()); GdiFlush();
        Check(GetPixel(dc, 100 + fps * 13, 90) == app->ThemeColor(UiThemeColor::Accent),
              "native ruler accents the exact whole-second column");
        Check(fps == 1 || GetPixel(dc, 100 + (fps - 1) * 13, 90) != app->ThemeColor(UiThemeColor::Accent),
              "native ruler does not accent the frame before a whole second");
        if (outputDirectory) {
            char path[1024]; sprintf_s(path, "%s/ruler-%d.bmp", outputDirectory, fps);
            FILE* file = nullptr; fopen_s(&file, path, "wb");
            Check(file != nullptr, "open ruler verification image");
            BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
            header.bfOffBits = sizeof header + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + width * height * 4;
            Check(fwrite(&header, sizeof header, 1, file) == 1 &&
                  fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file) == 1 &&
                  fwrite(pixels, width * height * 4, 1, file) == 1,
                  "write native ruler verification image");
            fclose(file);
        }
    }
    Check(!IsWindowVisible(window), "ruler regression leaves its window hidden");
    app->Renderer() = nullptr; app->PanelDC() = nullptr; app->state.bmpPanelSpare = nullptr;
    SelectObject(dc, oldBitmap); DeleteDC(dc); DeleteObject(bitmap); DeleteObject(blank);
    DestroyWindow(window);
}
