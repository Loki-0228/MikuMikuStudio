static LRESULT CALLBACK RejectWidth(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_WINDOWPOSCHANGING) { auto* position = reinterpret_cast<WINDOWPOS*>(lp); if (!(position->flags & SWP_NOSIZE) && position->cx == 77) position->cx = 78; }
    return DefSubclassProc(hwnd, message, wp, lp);
}
static void Win32HostChecks() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(parent != nullptr);
    HWND first = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD, 10, 20, 30, 40, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND second = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD, 50, 20, 30, 40, parent, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(first && second);
    CHECK(native_ui::ApplyWindows({{first, {20, 30, 80, 70}}, {second, {90, 30, 150, 70}}}) == native_ui::ApplyResult::Applied);
    auto position = [&](HWND window) { RECT r{}; GetWindowRect(window, &r); MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2); return r; };
    CHECK(position(first).left == 20 && position(second).left == 90);
    CHECK(native_ui::ApplyWindows({{first, {0, 0, 1, 1}}, {nullptr, {0, 0, 1, 1}}}) == native_ui::ApplyResult::Restored);
    CHECK(position(first).left == 20);
    CHECK(SetWindowSubclass(second, RejectWidth, 1, 0));
    CHECK(native_ui::ApplyWindows({{first, {0, 0, 10, 10}}, {second, {0, 0, 77, 10}}}) == native_ui::ApplyResult::Restored);
    CHECK(position(first).left == 20 && position(second).left == 90 && position(second).right == 150);
    RemoveWindowSubclass(second, RejectWidth, 1);
    HDC dc = GetDC(parent); CHECK(dc != nullptr);
    native_ui::PanelBuffer buffer; HDC memory = buffer.Begin(dc, 20, 30, 96, RGB(11, 22, 33)); CHECK(memory != nullptr);
    CHECK(GetPixel(memory, 5, 5) == RGB(11, 22, 33)); HBITMAP bitmap = buffer.Bitmap();
    SetPixel(memory, 5, 5, RGB(99, 88, 77)); CHECK(buffer.Begin(dc, 20, 30, 96, RGB(11, 22, 33)) == memory && buffer.Bitmap() == bitmap && GetPixel(memory, 5, 5) == RGB(99, 88, 77));
    CHECK(buffer.Copy(dc, {-10, -10, 10, 10}));
    CHECK(buffer.Begin(dc, 20, 30, 144, RGB(11, 22, 33)) != nullptr && GetPixel(buffer.Begin(dc, 20, 30, 144, RGB(11, 22, 33)), 5, 5) == RGB(11, 22, 33));
    CHECK(buffer.Begin(dc, -1, 30, 144, RGB(0, 0, 0)) == nullptr);
    buffer.Reset(); CHECK(buffer.Bitmap() == nullptr && !buffer.Copy(dc, {0, 0, 10, 10}));
    ReleaseDC(parent, dc); DestroyWindow(parent);
}
