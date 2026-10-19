#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <memory>
#include <cstring>
#include "mikumikustudio/mmd_app.hpp"

#include "mikumikustudio/viewport_corner_settings.hpp"
#include "../src/window/viewport_corner_menu.hpp"

namespace {
unsigned popupDraws = 0;
unsigned changedProcedures = 0;
HWND popupWindow = nullptr;
unsigned blankPaints = 0;
unsigned capturedPaints = 0;
int clickCell = 0;
unsigned routedClicks = 0;
void CheckPopupPaint() {
    RECT bounds{};
    GetWindowRect(popupWindow, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) { ++blankPaints; return; }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all{0, 0, width, height};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    // Ask this popup to paint into our render target; never read the desktop.
    SendMessageW(popupWindow, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT);
    GdiFlush();
    MENUBARINFO menu{sizeof(menu)};
    GetMenuBarInfo(popupWindow, OBJID_CLIENT, 0, &menu);
    for (UINT item = 0; item < 2; ++item) {
        RECT row{};
        GetMenuItemRect(nullptr, menu.hMenu, item, &row);
        OffsetRect(&row, -bounds.left, -bounds.top);
        unsigned ink = 0;
        for (int y = row.top + 2; y < row.bottom - 2 && y < height; ++y)
            for (int x = row.left + 20; x < row.right - 12 && x < width; ++x) {
                if (x < 0 || y < 0) continue;
                const unsigned rgb = static_cast<unsigned*>(pixels)[y * width + x];
                if (((rgb >> 16) & 255) + ((rgb >> 8) & 255) + (rgb & 255) < 600) ++ink;
            }
        if (!ink) ++blankPaints;
        std::printf("popup_paint item=%u text_pixels=%u\n", item, ink);
    }
    wchar_t path[1024]{};
    if (capturedPaints++ == 0 && GetEnvironmentVariableW(L"MMS_MENU_PAINT_OUTPUT", path, 1024)) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + width * height * 4;
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"wb") == 0) {
            fwrite(&header, sizeof(header), 1, file);
            fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file);
            fwrite(pixels, 4, size_t(width) * height, file);
            fclose(file);
        }
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
BOOL CALLBACK FindPopup(HWND window, LPARAM) {
    MENUBARINFO info{sizeof(info)};
    if (GetMenuBarInfo(window, OBJID_CLIENT, 0, &info) && info.hMenu)
        popupWindow = window;
    return TRUE;
}
LRESULT CALLBACK PopupOwner(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    using namespace mikumikustudio;
    if (message == WM_MEASUREITEM && viewport_corner_menu::HandleMeasureItem(
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp))) return TRUE;
    if (message == WM_INITMENUPOPUP)
        viewport_corner_menu::OnInitMenuPopup(reinterpret_cast<HMENU>(wp));
    if (message == WM_DRAWITEM) {
        EnumThreadWindows(GetCurrentThreadId(), FindPopup, 0);
        const LONG_PTR before = GetWindowLongPtrW(popupWindow, GWLP_WNDPROC);
        const bool handled = viewport_corner_menu::HandleDrawItem(
            reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        const LONG_PTR after = GetWindowLongPtrW(popupWindow, GWLP_WNDPROC);
        if (handled) {
            ++popupDraws;
            if (before != after) ++changedProcedures;
        }
        if (handled) return TRUE;
    }
    if (message == WM_EXITMENULOOP) viewport_corner_menu::OnExitMenuLoop();
    if (message == WM_TIMER) {
        CheckPopupPaint();
        MENUBARINFO menu{sizeof(menu)};
        RECT item{};
        GetMenuBarInfo(popupWindow, OBJID_CLIENT, 0, &menu);
        GetMenuItemRect(nullptr, menu.hMenu, GetMenuItemCount(menu.hMenu) - 1, &item);
        const int caption = (item.bottom - item.top) * 22 / 110;
        const int rowHeight = (item.bottom - item.top - caption) / 2;
        MSG input{};
        input.hwnd = popupWindow;
        input.pt = {item.left + (item.right - item.left) * (1 + 2 * (clickCell % 2)) / 4,
                    item.top + caption + rowHeight * (clickCell / 2) + rowHeight / 2};
        input.message = WM_MOUSEMOVE;
        CallMsgFilterW(&input, MSGF_MENU);
        input.message = WM_LBUTTONUP;
        if (CallMsgFilterW(&input, MSGF_MENU)) ++routedClicks;
        EndMenu();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

// A real native popup on a private, inactive desktop. No mouse movement,
// foreground window, screen capture, or change to the user's desktop.
bool InitialPopupChecks() {
    using namespace mikumikustudio;
    const HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    wchar_t name[80];
    swprintf_s(name, L"mms-menu-test-%lu", GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::fprintf(stderr, "FAIL: private menu desktop unavailable: %lu\n", GetLastError());
        if (desktop) CloseDesktop(desktop);
        return false;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = PopupOwner;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MmsInitialPopupTest";
    RegisterClassW(&wc);
    HWND owner = CreateWindowW(wc.lpszClassName, L"", WS_POPUP,
                              0, 0, 480, 480, nullptr, nullptr, wc.hInstance, nullptr);
    HMENU bar = CreateMenu(), popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, 201, L"About");
    AppendMenuW(popup, MF_STRING, 301, L"Remember folders");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), L"Help");
    SetMenu(owner, bar);
    static auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    app->Hwnd() = owner;
    viewport_corner_menu::EnsureItem(app.get());
    wchar_t previousLocalData[32768]{};
    const DWORD previousLocalDataSize = GetEnvironmentVariableW(
        L"LOCALAPPDATA", previousLocalData, 32768);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring testData = std::wstring(temp) + name;
    CreateDirectoryW(testData.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", testData.c_str());
    bool ok = owner != nullptr;
    for (int opening = 0; opening < 4 && ok; ++opening) {
        clickCell = opening;
        const unsigned previousDraws = popupDraws;
        SetTimer(owner, 1, 150, nullptr);
        TrackPopupMenuEx(popup, TPM_RETURNCMD, 10, 10, owner, nullptr);
        KillTimer(owner, 1);
        ok = popupWindow && popupDraws > previousDraws;
        ok = ok && static_cast<int>(viewport_corner_settings::Current()) == opening;
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", previousLocalDataSize ? previousLocalData : nullptr);
    std::printf("routed_corner_clicks=%u\n", routedClicks);
    std::printf("initial_popup draws=%u changed_window_procedures=%u %s\n",
                popupDraws, changedProcedures, ok && !changedProcedures ? "PASS" : "FAIL");
    SetMenu(owner, nullptr);
    DestroyWindow(owner);
    app->Hwnd() = nullptr;
    DestroyMenu(bar);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    SetThreadDesktop(previous);
    CloseDesktop(desktop);
    return ok && changedProcedures == 0 && blankPaints == 0 && routedClicks == 4;
}
}

// Exercise the production owner-draw handler on a shared memory DC. The next
// menu item must inherit the caller's state, not the corner picker's highlight.
int main() {
    using namespace mikumikustudio;
    // Run before creating any GDI/window resources on the normal desktop.
    if (!InitialPopupChecks()) return 1;
    constexpr int width = 240;
    constexpr int height = 190;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                     &pixels, nullptr, 0);
    HMENU menu = CreatePopupMenu();
    if (!dc || !bitmap || !pixels || !menu) {
        std::fprintf(stderr, "FAIL: could not create memory drawing fixture\n");
        if (menu) DestroyMenu(menu);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return 1;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HGDIOBJ oldFont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    if (!AppendMenuW(menu, MF_OWNERDRAW,
                     viewport_corner_menu::kViewportCornerMenuCommand, nullptr)) {
        std::fprintf(stderr, "FAIL: could not create owner-draw item\n");
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        DestroyMenu(menu);
        return 1;
    }

    unsigned failures = 0;
    unsigned checks = 0;
    constexpr UINT itemStates[] = {0, ODS_SELECTED, ODS_FOCUS,
                                   ODS_SELECTED | ODS_FOCUS};
    for (int corner = 0; corner < 4; ++corner) {
        viewport_corner_settings::ResetCacheForTest(
            static_cast<viewport_tool_layout::Corner>(corner));
        for (unsigned stateIndex = 0; stateIndex < 4; ++stateIndex) {
            SelectClipRgn(dc, nullptr);
            RECT all{0, 0, width, height};
            FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            IntersectClipRect(dc, 10, 5, 230, 175);
            RECT originalClip{};
            GetClipBox(dc, &originalClip);
            constexpr COLORREF originalColor = RGB(17, 34, 51);
            const int originalMode = stateIndex % 2 ? TRANSPARENT : OPAQUE;
            SetTextColor(dc, originalColor);
            SetBkColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, originalMode);
            SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
            HGDIOBJ originalFont = GetCurrentObject(dc, OBJ_FONT);

            DRAWITEMSTRUCT draw{};
            draw.CtlType = ODT_MENU;
            draw.itemID = viewport_corner_menu::kViewportCornerMenuCommand;
            draw.itemAction = ODA_DRAWENTIRE;
            draw.itemState = itemStates[stateIndex];
            draw.hwndItem = reinterpret_cast<HWND>(menu);
            draw.hDC = dc;
            draw.rcItem = {20, 10, 170, 120};
            const bool handled = viewport_corner_menu::HandleDrawItem(&draw);
            RECT actualClip{};
            GetClipBox(dc, &actualClip);
            const bool restored = GetTextColor(dc) == originalColor &&
                GetBkMode(dc) == originalMode &&
                GetBkColor(dc) == RGB(255, 255, 255) &&
                GetTextAlign(dc) == (TA_LEFT | TA_TOP | TA_NOUPDATECP) &&
                GetCurrentObject(dc, OBJ_FONT) == originalFont &&
                EqualRect(&originalClip, &actualClip);

            RECT nextItem{20, 130, 230, 170};
            DrawTextW(dc, L"NEXT MENU ITEM", -1, &nextItem,
                      DT_LEFT | DT_TOP | DT_SINGLELINE);
            GdiFlush();
            unsigned ink = 0;
            const auto* image = static_cast<const unsigned*>(pixels);
            for (int y = 130; y < 170; ++y)
                for (int x = 20; x < 230; ++x)
                    if ((image[y * width + x] & 0xffffffu) != 0xffffffu) ++ink;
            const bool passed = handled && restored && ink > 0;
            ++checks;
            if (!passed) ++failures;
            std::printf("corner=%d state=%u dc_restored=%d next_text_pixels=%u %s\n",
                        corner, itemStates[stateIndex], restored, ink,
                        passed ? "PASS" : "FAIL");
        }
    }

    DestroyMenu(menu);
    SelectClipRgn(dc, nullptr);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    std::printf("menu draw state: %u/%u passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
