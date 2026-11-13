// Included by bone_curve_tests.cpp for the explicit --main-preview-reopen probe.
namespace main_preview {
constexpr int side = 128;
using Image = std::array<DWORD, side * side>;

struct Surface {
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap;
    HGDIOBJ previous;

    Surface() {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Check(dc && bitmap && bits, "allocate main-panel pixel surface");
        previous = SelectObject(dc, bitmap);
        Check(previous && previous != HGDI_ERROR, "select main-panel bitmap");
    }
    ~Surface() { SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); }
    Image pixels() const {
        GdiFlush();
        Image result;
        const auto* source = static_cast<const DWORD*>(bits);
        for (size_t i = 0; i < result.size(); ++i) result[i] = source[i] & 0xffffff;
        return result;
    }
};

RECT Tile(HWND host) {
    RECT client{};
    Check(GetClientRect(host, &client), "measure main client");
    return {8, client.bottom - 135, 8 + side, client.bottom - 135 + side};
}

struct Geometry { unsigned ink = 0, bowed = 0, chord = 0; };
Geometry Measure(const Image& image, bool camera) {
    Geometry result;
    for (int x = 32; x < 96; ++x) {
        bool ink = false, bowed = false, chord = false;
        for (int y = 0; y < side; ++y) {
            if (image[y * side + x] != (camera ? 0x0000ffu : 0u)) continue;
            const int offset = y - (127 - x);
            ink = true;
            bowed |= (camera ? -offset : offset) > 10;
            chord |= std::abs(offset) <= 2;
        }
        result.ink += ink;
        result.bowed += bowed;
        result.chord += chord;
    }
    return result;
}

struct Observer {
    Surface curve, capture;
    Image baseline{};
    bool camera = false;
    const char* phase = nullptr;
    unsigned frames = 0;
    bool inOperation = false;
    bool matched = true;

    static LRESULT CALLBACK Paint(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        if (msg != WM_PAINT) return DefSubclassProc(hwnd, msg, wp, lp);
        auto& self = *reinterpret_cast<Observer*>(data);
        RECT dirty{}, intersection{}, tile = Tile(hwnd);
        const bool observe = self.phase && GetUpdateRect(hwnd, &dirty, FALSE) &&
                             IntersectRect(&intersection, &dirty, &tile);
        const LRESULT result = MainWndProc(hwnd, msg, wp, lp);
        if (observe) {
            HDC client = GetDC(hwnd);
            Check(client != nullptr, "acquire painted main-window client");
            const bool copied = BitBlt(self.capture.dc, 0, 0, side, side, client, tile.left, tile.top, SRCCOPY) != FALSE;
            ReleaseDC(hwnd, client);
            Check(copied, "read actual main-window interpolation rectangle");
            const auto source = self.curve.pixels(), destination = self.capture.pixels();
            const bool sourceOk = source == self.baseline, clientOk = destination == self.baseline;
            const auto s = Measure(source, self.camera), d = Measure(destination, self.camera);
            std::printf("MAIN_PREVIEW %s %s %s paint=%u curve_matches=%d client_matches=%d "
                        "curve_ink/bowed/chord=%u/%u/%u client_ink/bowed/chord=%u/%u/%u\n",
                        self.camera ? "camera" : "bone", self.phase, self.inOperation ? "during" : "after",
                        ++self.frames, int(sourceOk), int(clientOk),
                        s.ink, s.bowed, s.chord, d.ink, d.bowed, d.chord);
            self.matched &= sourceOk && clientOk;
        }
        return result;
    }

    template<class Operation>
    void During(HWND host, const char* name, Operation perform) {
        // Erase the destination only. An omitted or clipped production BitBlt
        // can no longer pass by leaving the previous good frame on the screen.
        RECT tile = Tile(host);
        const auto expose = [&] {
            HDC client = GetDC(host);
            Check(client && PatBlt(client, tile.left, tile.top, side, side, WHITENESS), "clear destination witness");
            ReleaseDC(host, client);
            Check(InvalidateRect(host, &tile, FALSE), "expose the interpolation rectangle");
        };
        frames = 0;
        phase = name;
        inOperation = true;
        expose();
        perform(); // Capture synchronous WM_PAINT here as well as the queued first paint.
        inOperation = false;
        const unsigned during = frames;
        if (during) expose();
        UpdateWindow(host);
        Check(frames > during, "observed main-panel WM_PAINT after the operation returned");
        phase = nullptr;
    }
};

HWND OwnedEditor(HWND host) {
    HWND window = GetWindow(host, GW_HWNDFIRST);
    for (; window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (GetWindow(window, GW_OWNER) == host && curve_editor::OwnsWindow(host, window)) return window;
    }
    return nullptr;
}

void Cycle(MMDApp& app, HWND combo, Observer& probe, bool camera) {
    const HWND host = app.MainWindow();
    probe.camera = camera;
    app.CameraMode() = camera;
    EnsureInterpChannelCombo(&app, true);
    Check(SendMessageW(combo, CB_SETCURSEL, 0, 0) == 0, "select X on main panel");
    SelectionReeval(&app);
    probe.baseline = probe.curve.pixels();
    // Bone X accelerates upward near the end. Camera X bows the opposite
    // way. Count actual ink away from the chord, ignoring the camera's
    // black guides and red markers. No duplicate Bezier renderer.
    const auto bowedColumns = Measure(probe.baseline, camera).bowed;
    std::printf("MAIN_PREVIEW %s baseline off_chord_columns=%u\n", camera ? "camera" : "bone", bowedColumns);
    Check(bowedColumns >= 16, "main-panel baseline contains the expected curved interior");
    HWND editor = nullptr;
    auto open = [&] {
        curve_host::OpenInterpolation(&app, false);
        editor = OwnedEditor(host);
        Check(editor && SendMessageW(editor, WM_APP + 303, 0, 0) == 2, "opened real editor with both X keys");
        RECT owner{};
        GetWindowRect(host, &owner);
        Check(SetWindowPos(editor, nullptr, owner.right + 8, owner.top, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) && IsWindowVisible(editor),
              "show independent editor clear of the main panel");
    };
    auto close = [&] {
        SendMessageW(editor, WM_CLOSE, 0, 0);
        Check(!OwnedEditor(host), "editor close retires the owned window");
    };
    probe.During(host, "before-open", [] {});
    probe.During(host, "open", open);
    const auto firstSession = curve_editor::CaptureWindowSession(editor);
    probe.During(host, "closed", close);
    probe.During(host, "reopened", open);
    Check(!curve_editor::IsWindowSessionCurrent(editor, firstSession), "reopened editor is a new session");
    // Explicitly regenerate only AFTER sampling the first reopened frame.
    probe.During(host, "later-refresh", [&] { SelectionReeval(&app); });
    close();
    Check(probe.matched, "first and later main-window pixels match the curved pre-open baseline");
}

int Run(const char* expectedDesktop) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char desktop[128]{};
    DWORD needed = 0;
    const auto threadDesktop = GetThreadDesktop(GetCurrentThreadId());
    BOOL input = TRUE;
    Check(GetUserObjectInformationA(threadDesktop, UOI_NAME, desktop, sizeof desktop, &needed) &&
          std::strncmp(desktop, "MMSCurvePreview-", 16) == 0 && !std::strcmp(desktop, expectedDesktop) &&
          GetUserObjectInformationW(threadDesktop, UOI_IO, &input, sizeof input, &needed) && !input,
          "pixel probe owns a non-input desktop");
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof app->state);
    Model model;
    app->ModelSlot(0) = model.data();
    app->SidebarWidth() = 250;
    app->EnglishUI() = 2;
    app->CurrentFrame() = 10;
    bone_curve::Tracks tracks;
    for (size_t c = 0; c < tracks.size(); ++c) {
        ac::Key a{}, b{};
        b.frame = 20;
        a.value = b.value = c >= 6 ? 1 : 0;
        a.tangent = b.tangent = ac::Tangent::Free;
        a.interpolation = b.interpolation = ac::Interpolation::Bezier;
        if (c == 0) { b.value = 10; b.inSlope = 1.4; }
        tracks[c].keys = {a, b};
    }
    std::string error;
    Check(bone_curve::Write(model.data(), 0, tracks, error), "create authored bone X preview");
    app->CameraKeys() = mdl::KeyPoolCreate<mdl::CameraKey>(2);
    auto* keys = app->CameraKeys();
    keys[0].next = 1;
    keys[1].frame = 20;
    keys[1].eye[0] = 10;
    keys[1].selected = 1;
    for (int k = 0; k < 2; ++k) {
        keys[k].parentModel = -1;
        keys[k].distance = -45;
        keys[k].fov = 45;
        for (int c = 0; c < 6; ++c) {
            keys[k].interpolation[0][c] = 15; keys[k].interpolation[1][c] = 115;
            keys[k].interpolation[2][c] = 70; keys[k].interpolation[3][c] = 119;
        }
    }
    Observer probe;
    const HWND host = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"main panel pixel test",
        WS_POPUP | WS_CLIPCHILDREN, 8, 8, 420, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(host != nullptr, "create private main-panel host");
    g_Block = app.get();
    app->MainWindow() = host;
    app->CurveDC() = probe.curve.dc;
    const HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 100, 150,
        host, reinterpret_cast<HMENU>(433), GetModuleHandleW(nullptr), nullptr);
    Check(combo && SetWindowSubclass(host, Observer::Paint, 1, reinterpret_cast<DWORD_PTR>(&probe)),
          "route actual host WM_PAINT through MainWndProc");
    SetWindowPos(host, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Check(IsWindowVisible(host), "main window is drawable on private desktop");
    Cycle(*app, combo, probe, false);
    Cycle(*app, combo, probe, true);
    RemoveWindowSubclass(host, Observer::Paint, 1);
    DestroyWindow(host);
    app->MainWindow() = nullptr;
    app->CurveDC() = nullptr;
    mdl::KeyPoolDestroy(app->CameraKeys());
    app->ModelSlot(0) = nullptr;
    g_Block = nullptr;
    std::puts("PASS main-panel bone and camera first-paint reopen");
    return 0;
}

int Launch() {
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"MMSCurvePreview-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(original && desktop, "create isolated preview desktop");
    if (GetThreadDesktop(GetCurrentThreadId()) != original)
        Check(SetThreadDesktop(original), "restore launcher desktop");
    wchar_t exe[32768]{};
    const auto length = GetModuleFileNameW(nullptr, exe, 32768);
    Check(length && length < 32768, "locate main preview test executable");
    auto command = L"\"" + std::wstring(exe) + L"\" --main-preview-child " + name;
    // Forward a single diagnostic stream and NUL stdin without inheriting
    // unrelated runner handles. The child must exit before CloseDesktop.
    HANDLE handles[2]{};
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    handles[0] = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    Check(handles[0] != INVALID_HANDLE_VALUE && DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
        GetCurrentProcess(), &handles[1], 0, TRUE, DUPLICATE_SAME_ACCESS), "prepare isolated preview standard handles");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> storage(size);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(name.c_str());
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = handles[1];
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    Check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) &&
          UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles, sizeof handles, nullptr, nullptr), "restrict isolated preview handle inheritance");
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(handles[0]); CloseHandle(handles[1]);
    Check(started, "start preview child on its private desktop");
    CloseHandle(child.hThread);
    const auto wait = WaitForSingleObject(child.hProcess, 120000);
    if (wait != WAIT_OBJECT_0) {
        Check(TerminateProcess(child.hProcess, 124) && WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0,
              "terminate own timed-out preview child");
    }
    DWORD code = 1;
    Check(GetExitCodeProcess(child.hProcess, &code), "read preview child result");
    CloseHandle(child.hProcess);
    Check(CloseDesktop(desktop), "close preview desktop after child exit");
    Check(wait == WAIT_OBJECT_0 && code == 0, "private main-preview child passed");
    return 0;
}
} // namespace main_preview
