// Real PMM read/write and first editor paint on a transparent fixture window.
namespace pmm_expression_checks {
namespace ac = animation_curve;
namespace mc = morph_curve;
namespace ch = curve_host;
namespace ce = curve_editor;

#include "expression_curve_first_paint.inc"

int offscreenX, offscreenY;
HWND firstMainWindow;
HDC firstMainPaint;
RECT firstMainCurve, firstMainDirty;
int mainPaints;
std::vector<unsigned> firstMainPixels;

HDC WINAPI BeginWithMain(HWND window, LPPAINTSTRUCT paint) {
    const HDC dc = expression_paint::Begin(window, paint);
    if (window == firstMainWindow && dc && !IsRectEmpty(&paint->rcPaint)) {
        ++mainPaints;
        if (mainPaints == 1) {
            firstMainDirty = paint->rcPaint;
            firstMainPaint = dc;
        }
    }
    return dc;
}

BOOL WINAPI EndWithMain(HWND window, const PAINTSTRUCT* paint) {
    if (window == firstMainWindow) firstMainPaint = nullptr;
    return expression_paint::End(window, paint);
}

BOOL WINAPI BlitWithMain(HDC dst, int x, int y, int width, int height, HDC src, int sx, int sy, DWORD op) {
    using expression_paint::Require;
    const BOOL result = expression_paint::Blit(dst, x, y, width, height, src, sx, sy, op);
    if (dst != firstMainPaint || !firstMainPixels.empty()) return result;
    const RECT presented{x, y, x + width, y + height};
    if (!EqualRect(&presented, &firstMainDirty)) return result;
    RECT overlap{};
    Require(result && op == SRCCOPY && IntersectRect(&overlap, &presented, &firstMainCurve) &&
            EqualRect(&overlap, &firstMainCurve), "the first main-window paint presents the whole bottom curve");
    const int side = firstMainCurve.right - firstMainCurve.left;
    Require(side >= 64 && side == firstMainCurve.bottom - firstMainCurve.top, "use the real square bottom-panel geometry");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HDC copy = CreateCompatibleDC(src);
    const HBITMAP bitmap = CreateDIBSection(src, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(copy && bitmap && bits, "allocate a bottom curve pixel capture");
    const auto old = SelectObject(copy, bitmap);
    Require(old && old != HGDI_ERROR &&
            expression_paint::blit(copy, 0, 0, side, side, src, sx + firstMainCurve.left - x,
                sy + firstMainCurve.top - y, SRCCOPY) && GdiFlush(),
            "copy the bottom square from the actual buffer sent to the first main-window paint DC");
    const auto* pixels = static_cast<const unsigned*>(bits);
    firstMainPixels.assign(pixels, pixels + size_t(side) * side);
    SelectObject(copy, old);
    DeleteObject(bitmap);
    DeleteDC(copy);
    return result;
}

void CheckBottomFirstPaint(MMDApp* app, const ac::Track& track) {
    using expression_paint::Require;
    const auto window = app->MainWindow();
    Require(!IsWindowVisible(window), "arm capture before exposing any main-window paint");
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) |
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    Require(SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA) &&
            SetWindowPos(window, HWND_TOPMOST, 16, 16, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE),
            "allow an invisible, input-transparent main-window paint inside the monitor");
    BYTE alpha = 255;
    DWORD flags = 0;
    Require(GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && !alpha && (flags & LWA_ALPHA),
            "do not expose the fixture or take focus");
    RECT client{};
    GetClientRect(window, &client);
    firstMainCurve = BottomCurveRect(app, client.bottom);
    firstMainWindow = window;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    RedrawWindow(window, &firstMainCurve, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
    firstMainWindow = nullptr;
    ShowWindow(window, SW_HIDE);
    Require(GetForegroundWindow() != window && mainPaints > 0 && !firstMainPixels.empty(),
            "capture the first real main-window paint after load without opening the editor");

    const int side = firstMainCurve.right - firstMainCurve.left;
    const int last = side - 1;
    const auto row = [&](double weight) { return last - int(std::lround(last * weight)); };
    const auto blueNear = [&](int x, int expected) {
        for (int y = std::max(0, expected - 3); y <= std::min(last, expected + 3); ++y) {
            const auto pixel = firstMainPixels.at(size_t(y) * side + x) & 0xffffff;
            if (pixel == 0x0000ff) return y;
        }
        return -1;
    };
    for (int frame : {5, 10, 14}) {
        const int x = int(std::lround(frame * last / 20.));
        const double fraction = x / double(last);
        const int expected = row(ac::Evaluate(track, 20 * fraction).value);
        const int chord = row(track.keys.front().value * (1 - fraction) + track.keys.back().value * fraction);
        const int measured = blueNear(x, expected), straight = blueNear(x, chord);
        std::printf("EXPRESSION_BOTTOM_FIRST_PAINT frame=%d side=%d x=%d measured=%d expected=%d chord=%d chord_ink=%d\n",
                    frame, side, x, measured, expected, chord, straight);
        Require(std::abs(expected - chord) > 7 && measured >= 0 && straight < 0,
                "the main bottom preview displays the saved expression curve on its first paint");
    }
}
LRESULT CALLBACK ParkWindows(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->x = offscreenX;
            create->y = offscreenY;
            create->dwExStyle |= WS_EX_NOACTIVATE;
        }
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

HWND OpenEditor(MMDApp* app) {
    using namespace expression_paint;
    canvas = {}; mmd = {};
    owner = app->MainWindow();
    ch::OpenMorph(app, 0, false);
    HWND editor = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM result) -> BOOL {
        wchar_t name[64]{};
        if (GetWindow(window, GW_OWNER) != owner || !GetClassNameW(window, name, 64) ||
            std::wcscmp(name, L"MikuMikuStudioCurveEditorWindow") != 0) return TRUE;
        *reinterpret_cast<HWND*>(result) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&editor));
    Require(editor && !IsWindowVisible(editor),
        "open the real expression editor owned by this hidden application");
    const auto style = GetWindowLongPtrW(editor, GWL_EXSTYLE);
    SetWindowLongPtrW(editor, GWL_EXSTYLE, style | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    constexpr auto required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    Require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & required) == required &&
            SetLayeredWindowAttributes(editor, 0, 0, LWA_ALPHA),
        "make the fixture invisible and input-transparent before placing it within monitor bounds");
    BYTE alpha = 255;
    DWORD layerFlags = 0;
    Require(GetLayeredWindowAttributes(editor, nullptr, &alpha, &layerFlags) && alpha == 0 &&
            (layerFlags & LWA_ALPHA) && SetWindowPos(editor, HWND_TOPMOST, 16, 16, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE),
        "allow real paint invalidation on-screen with a fully transparent nonactivating window");
    ShowWindow(editor, SW_SHOWNOACTIVATE);
    RedrawWindow(editor, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    std::printf("EXPRESSION_PMM paint canvas=%d mmd=%d\n", canvas.paints, mmd.paints);
    Require(!IsWindowVisible(owner) && GetForegroundWindow() != editor &&
            !canvas.pixels.empty() && !mmd.pixels.empty(),
        "capture the real first WM_PAINT on both curve surfaces without activating the editor");
    return editor;
}

int Run(const wchar_t* stage) {
    using expression_paint::Require;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const bool prepare = std::wcscmp(stage, L"prepare") == 0;
    Require(prepare || std::wcscmp(stage, L"reopen") == 0, "use a named isolated curve fixture phase");
    wchar_t exe[32768]{};
    const auto n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const auto directory = fs::current_path();
    Require(n && n < std::size(exe) && fs::equivalent(fs::path(exe).parent_path(), directory),
        "the copied executable runs from its own fixture directory");
    const auto modelPath = directory / L"expression.pmx", project = directory / L"expression.pmm";
    const auto receipt = directory / L"authored-controls.txt";
    offscreenX = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    offscreenY = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 64;
    const HHOOK parking = SetWindowsHookExW(WH_CBT, ParkWindows, nullptr, GetCurrentThreadId());
    Require(parking != nullptr && MH_Initialize() == MH_OK, "guard activation and observe owned first paints");
    using namespace expression_paint;
    Hook(&BeginPaint, &BeginWithMain, begin); Hook(&EndPaint, &EndWithMain, end); Hook(&BitBlt, &BlitWithMain, blit);
    auto instance = std::make_unique<MMDApp>();
    auto* app = instance.get(); g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Require(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE) && !IsWindowVisible(app->MainWindow()),
        "start the real hidden production editor and renderer");
    app->EnglishUI() = 2;
    for (int i = 0; i < 35; ++i) app->ThemeColorAt(i) = RGB(200, 200, 200);
    if (prepare) {
        Require(!fs::exists(modelPath) && !fs::exists(project) && !fs::exists(receipt), "own fresh model and PMM paths");
        pmm_selection_checks::WritePmx(modelPath, {L"root"}, {L"smile"});
        LoadModelFile(app, modelPath.c_str(), false);
    } else {
        Require(LoadSceneFile(project.c_str()), "load the actual saved PMM in a new process");
    }
    auto* raw = app->ModelSlot(0);
    Require(raw && mdl::Mdl(raw)->morphCount == 1 && !app->ModelSlot(1), "load exactly the original one-morph PMX");
    auto& model = *mdl::Mdl(raw);
    ac::Track authored;
    std::string error;
    if (prepare) {
        app->SetSelectedModelSlot(0);
        app->CameraMode() = 0;
        model.selectedMorphs[0] = 0;
        for (auto i = std::size_t{0}; i < mdl::BoneKeyCapacity(model); ++i)
            model.boneKeys[i].allocated = 0;
        auto& head = model.morphKeys[0];
        head.value = .1f; head.allocated = 1; head.next = 1;
        auto& tail = model.morphKeys[1];
        tail.frame = 20; tail.value = .9f; tail.allocated = 1; tail.previous = 0;
        app->CurrentFrame() = 3;
        Require(mc::Read(raw, 0, authored, error) && authored.keys.size() == 2 &&
                ac::SetMmdControls(authored, 1, {20, 20, 107, 107}, error) &&
                mc::Write(raw, {{mc::StableId(0, model.morphs[0].name), authored}}, error),
            "prepare two selected native keys with a known straight interval");
        const auto editor = OpenEditor(app);
        Geometry(editor, authored, false, 0);
        const auto panel = GetDlgItem(editor, 1031);
        const auto square = Square(panel);
        const auto from = OnSquare(square, 20. / 127, 20. / 127);
        const auto to = OnSquare(square, 22. / 127, 115. / 127);
        SendMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
        Require(GetCapture() == panel, "the actual MMD handle owns the pointer gesture");
        SendMessageW(panel, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
        SendMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
        Require(mc::Read(raw, 0, authored, error) && authored.keys.size() == 2 &&
                ac::Evaluate(authored, 10).value > .60,
            "the production handle gesture authors a nonlinear curve");
        ce::CloseOwned(owner);
        app->CurrentFrame() = 10;
        Require(model.morphKeys[0].next != 0, "the authored continuation stays in the native timeline");
        model.morphKeys[0].allocated = 0;
        model.morphKeys[model.morphKeys[0].next].allocated = 1;
        SeekSelectedModelToCurrentFrame(app);
        Require(SaveSceneCopy(app, project.c_str()), "persist the edited expression through the real PMM writer");
        std::array<double, 4> controls{};
        Require(ac::GetMmdControls(authored, 1, controls), "capture the authored MMD controls before exit");
        std::ofstream reference(receipt);
        reference << std::setprecision(17);
        for (double control : controls) reference << control << '\n';
        for (int frame : {5, 10, 14}) reference << ac::Evaluate(authored, frame).value << '\n';
        reference.close();
        Require(bool(reference), "record the authored curve and expected samples outside the PMM");
    } else {
        Require(app->CurrentFrame() == 10 && mc::Read(raw, 0, authored, error) && authored.keys.size() == 2,
            "load both authored expression endpoints and the saved playhead");
        std::ifstream reference(receipt);
        std::array<double, 4> controls{}, expected{};
        for (auto& value : expected) reference >> value;
        Require(bool(reference) && ac::GetMmdControls(authored, 1, controls) && controls == expected &&
                ac::Evaluate(authored, 10).value > .60,
            "the real PMM restores the authored handles before any editor input");
        const auto loadedValue = model.morphs[0].value;
        std::printf("EXPRESSION_PMM load frame=10 value=%.9g expected=%.9g\n",
                    loadedValue, ac::Evaluate(authored, 10).value);
        Require(std::abs(loadedValue - ac::Evaluate(authored, 10).value) < 1e-6,
            "the reloaded model applies the authored value at its saved playhead");
        Require(app->CameraMode() == 0 && app->SelectedModel() == raw && model.selectedMorphs[0] == 0 &&
                model.morphKeys[0].next && model.morphKeys[model.morphKeys[0].next].allocated,
            "restore the saved model and marked expression key without post-load input");
        CheckBottomFirstPaint(app, authored);
        const auto linear = GetDlgItem(app->MainWindow(), panel::kCurveLinearButton);
        Require(linear && !IsWindowEnabled(linear) && !app->state.interpCurveUniformFound,
            "expression preview cannot invoke the legacy bone interpolation edits");
        Require(model.boneCount && model.boneKeys && !model.boneKeys[0].allocated,
            "the saved model has an unmarked bone key");
        model.boneKeys[0].allocated = 1;
        SelectionReeval(app);
        Require(IsWindowEnabled(linear) && app->state.interpCurveUniformFound,
            "mixed selection still offers the existing bone interpolation controls");
        model.boneKeys[0].allocated = 0;
        SelectionReeval(app);
        Require(!IsWindowEnabled(linear) && !app->SceneModified(),
            "returning to the expression preview leaves the project clean");
        const auto editor = OpenEditor(app);
        // Frame 10 is the saved playhead; its line covers the curve at that column.
        Geometry(editor, authored, true, 0, 11);
        for (int frame : {5, 10, 14}) {
            double expectedSample = 0;
            reference >> expectedSample;
            app->CurrentFrame() = frame;
            SeekSelectedModelToCurrentFrame(app);
            std::printf("EXPRESSION_PMM seek frame=%d value=%.9g expected=%.9g\n",
                        frame, model.morphs[0].value, expectedSample);
            Require(bool(reference) && std::abs(model.morphs[0].value - expectedSample) < 1e-6,
                "normal native frame seeks sample the same curve the first paint displays");
        }
        ce::CloseOwned(owner);
    }
    owner = nullptr;
    background_save::Shutdown(app);
    Require(DestroyWindow(app->MainWindow()) != FALSE, "destroy only the fixture editor host");
    ShutdownCleanup(app); g_Block = nullptr;
    instance.reset();
    Require(MH_Uninitialize() == MH_OK && UnhookWindowsHookEx(parking), "retire the fixture hooks");
    std::printf("PASS expression PMM phase=%ls\n", stage);
    return 0;
}
} // namespace pmm_expression_checks
