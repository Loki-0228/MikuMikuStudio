namespace original_camera_sendinput_checks {
using namespace mikumikustudio;
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void Pump(unsigned milliseconds = 80) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(1);
    } while (GetTickCount64() < until);
}
bool Run(MMDApp* app) {
    const auto window = app->MainWindow(), previous = GetForegroundWindow();
    POINT saved{}; GetCursorPos(&saved);
    bool ok = false, injected = false;
    try {
        for (int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, VK_LBUTTON})
            Require(!(GetAsyncKeyState(key) & 0x8000), "desktop modifier/button is held; input test cannot start");
        SetWindowPos(window, HWND_TOP, 40, 40, 1000, 700, SWP_SHOWWINDOW);
        ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(window); Pump();
        if (GetForegroundWindow() != window && previous) {
            const auto owner = GetWindowThreadProcessId(previous, nullptr);
            if (owner != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), owner, TRUE)) {
                BringWindowToTop(window); SetForegroundWindow(window); SetFocus(window);
                AttachThreadInput(GetCurrentThreadId(), owner, FALSE); Pump();
            }
        }
        std::printf("INPUT foreground=%p target=%p previous=%p\n", GetForegroundWindow(), window, previous);
        Require(GetForegroundWindow() == window, "cannot foreground the isolated test; no input injected");
        const auto inject = [&](INPUT* input, UINT count) {
            Require(GetForegroundWindow() == window, "test lost foreground; input injection stopped");
            const UINT sent = SendInput(count, input, sizeof(INPUT));
            injected = injected || sent != 0;
            Require(sent == count, "SendInput was refused or incomplete"); Pump();
        };
        app->CameraMode() = 1; app->CameraPitch() = app->CameraYaw() = app->CameraRoll() = 0;
        app->CameraPositionX() = app->CameraPositionY() = app->CameraPositionZ() = 0;
        const auto key = [&](WORD vk) {
            INPUT input[2]{}; input[0].type = input[1].type = INPUT_KEYBOARD;
            input[0].ki.wVk = input[1].ki.wVk = vk; input[1].ki.dwFlags = KEYEVENTF_KEYUP;
            inject(input, 2);
        };
        key('W'); Require(std::abs(app->CameraPositionZ() - 8) < .001f, "original SendInput W down/up");
        key('S'); Require(std::abs(app->CameraPositionZ()) < .001f, "original SendInput S down/up");
        key('D'); Require(std::abs(app->CameraPositionX() - 4) < .001f, "original SendInput D down/up");
        key('A'); Require(std::abs(app->CameraPositionX()) < .001f, "original SendInput A down/up");
        RECT bounds = app->ViewportRect();
        POINT begin{(bounds.left + bounds.right)/2, (bounds.top + bounds.bottom)/2}; ClientToScreen(window, &begin);
        Require(SetCursorPos(begin.x, begin.y), "position test cursor"); Pump();
        INPUT mouse{}; mouse.type = INPUT_MOUSE; mouse.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; inject(&mouse, 1);
        Require(GetCapture() == window, "real mouse button did not reach original camera");
        mouse.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        mouse.mi.dx = LONG((begin.x + 50 - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535LL / (GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
        mouse.mi.dy = LONG((begin.y + 25 - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535LL / (GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
        inject(&mouse, 1);
        POINT end{}; GetCursorPos(&end);
        Require(std::abs(app->CameraYaw() + (end.x-begin.x)/500.f) < .003f &&
            std::abs(app->CameraPitch() + (end.y-begin.y)/500.f) < .003f, "original SendInput mouse rotation");
        mouse.mi.dwFlags = MOUSEEVENTF_LEFTUP; inject(&mouse, 1);
        Require(!GetCapture(), "mouse release must end capture");
        app->CameraMode() = 0; const auto z = app->CameraPositionZ(); key('W');
        Require(app->CameraPositionZ() == z, "model mode must not move camera");
        app->CameraMode() = 1;
        mouse.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; inject(&mouse, 1);
        Require(GetCapture() == window, "capture before real focus transfer");
        const auto other = CreateWindowW(L"STATIC", L"Isolated focus target", WS_OVERLAPPEDWINDOW,
            60, 60, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(other != nullptr, "focus target creation"); ShowWindow(other, SW_SHOW); SetForegroundWindow(other); SetFocus(other); Pump();
        const bool released = GetCapture() != window;
        DestroyWindow(other); SetForegroundWindow(window); SetFocus(window);
        mouse.mi.dwFlags = MOUSEEVENTF_LEFTUP; inject(&mouse, 1);
        Require(released, "actual focus transfer did not release camera gesture");
        ok = true;
    } catch (const std::exception& error) { std::printf("SendInput check failed: %s\n", error.what()); }
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    ShowWindow(window, SW_HIDE); SetCursorPos(saved.x, saved.y);
    if (previous && IsWindow(previous)) SetForegroundWindow(previous);
    if (!ok && !injected) { std::puts("UNVERIFIED SendInput: environment precondition failed; no input sent"); return false; }
    Check(ok, "original camera operating-system input queue checks");
    std::puts("PASS original DLL SendInput WASD/mouse/mode/actual focus transfer");
    return true;
}
}
