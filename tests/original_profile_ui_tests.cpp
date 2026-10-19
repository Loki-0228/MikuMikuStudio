// Real production controls/menus in a hidden, empty host. Run only from an
// isolated test runtime containing Data/. No model, picker or explicit
// FrameDriver call is used; startup still creates the production D3D device.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <MinHook.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

static_assert(MMD_ORIGINAL_PROFILE, "This UI test requires the mmd932 profile");
using namespace mikumikustudio;
namespace {
int checks = 0, failures = 0;
unsigned blockedActivations = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) ++failures;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
}
void Require(bool value, const char* label) {
    Check(value, label);
    if (!value) std::exit(2);
}
bool OwnWindow(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}
bool OwnTop(HWND window) {
    return OwnWindow(window) && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD);
}
decltype(&ShowWindow) originalShow = nullptr;
decltype(&SetWindowPos) originalPosition = nullptr;
decltype(&SetWindowPlacement) originalPlacement = nullptr;
BOOL WINAPI Hide(HWND window, int command) {
    return originalShow(window, OwnTop(window) ? SW_HIDE : command);
}
BOOL WINAPI Position(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (OwnTop(window)) flags = (flags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW | SWP_NOACTIVATE;
    return originalPosition(window, after, x, y, width, height, flags);
}
BOOL WINAPI Placement(HWND window, const WINDOWPLACEMENT* value) {
    if (!value || !OwnTop(window)) return originalPlacement(window, value);
    auto hidden = *value;
    hidden.showCmd = SW_HIDE;
    hidden.flags &= ~WPF_RESTORETOMAXIMIZED;
    return originalPlacement(window, &hidden);
}
LRESULT CALLBACK Guard(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->dwExStyle |= WS_EX_NOACTIVATE;
        }
    } else if (code == HCBT_ACTIVATE && OwnWindow(reinterpret_cast<HWND>(wp))) {
        ++blockedActivations;
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected MessageBoxA: %s\n", text ? text : ""); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected MessageBoxW: %ls\n", text ? text : L""); std::exit(2);
}
BOOL WINAPI UnexpectedPickerA(LPOPENFILENAMEA) {
    std::puts("FAIL unexpected file picker"); std::exit(2);
}
BOOL WINAPI UnexpectedPickerW(LPOPENFILENAMEW) {
    std::puts("FAIL unexpected file picker"); std::exit(2);
}
template<class F> void Hook(F target, F replacement, F* original = nullptr) {
    Require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement),
                         reinterpret_cast<void**>(original)) == MH_OK, "create hidden-host guard");
}
void AuditHidden(const char* phase) {
    bool hidden = true;
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        if (OwnTop(window) && IsWindowVisible(window)) *reinterpret_cast<bool*>(value) = false;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hidden));
    std::printf("HIDDEN phase=%s\n", phase);
    Check(hidden && !OwnWindow(GetForegroundWindow()), "no owned top-level window is visible or foreground");
}
std::wstring Caption(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}
std::wstring ComboText(HWND combo, int index) {
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    Require(length >= 0 && length < 512, "interpolation item has bounded text");
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const auto copied = SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    Require(copied == length, "read actual interpolation item");
    text.resize(static_cast<std::size_t>(length));
    return text;
}
struct MenuItem { UINT id; std::wstring text; };
void CollectMenu(HMENU menu, std::vector<MenuItem>& items, unsigned depth = 0) {
    Require(menu && depth < 16, "valid bounded production menu tree");
    for (int position = 0; position < GetMenuItemCount(menu); ++position) {
        wchar_t text[256]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        item.dwTypeData = text; item.cch = static_cast<UINT>(std::size(text));
        Require(GetMenuItemInfoW(menu, position, TRUE, &item) != FALSE, "read production menu item");
        items.push_back({item.wID, text});
        if (item.hSubMenu) CollectMenu(item.hSubMenu, items, depth + 1);
    }
}
void CheckMenus(MMDApp* app) {
    std::vector<MenuItem> menu;
    CollectMenu(GetMenu(app->MainWindow()), menu);
    bool extensionsAbsent = true, mmvAbsent = true, videoExport = false, videoBackground = false;
    for (const auto& item : menu) {
        // IDs cover the actual launchers plus their popup container, not just
        // whether the empty scene happened to grey an otherwise live action.
        if ((item.id >= 32800 && item.id <= 32804) || (item.id >= 32900 && item.id <= 32907) ||
            item.id == 32920 || (item.id >= 303 && item.id <= 308) || item.id == 32004)
            extensionsAbsent = false;
        auto lower = item.text;
        for (auto& letter : lower) letter = static_cast<wchar_t>(std::towlower(letter));
        if (item.id == 32600 || lower.find(L"mmv") != std::wstring::npos) mmvAbsent = false;
        // The language pass uses general "video" captions for these original
        // commands. AVI-only format handling is a separate IO contract; this
        // UI test checks that both actual commands remain, without opening a
        // system file dialog or inferring the format from translated words.
        if (item.id == 223) videoExport = !item.text.empty();
        if (item.id == 213) videoBackground = !item.text.empty();
    }
    Check(extensionsAbsent, "curve, FPS conversion, baking, Alembic and transparent-sort menu entries are absent");
    Check(mmvAbsent && !GetModuleHandleW(L"MMV4MMS.dll"), "MMV menu and runtime are absent");
    Check(videoExport && videoBackground, "original media export and background menu commands remain with captions");
}
void CheckControls(MMDApp* app) {
    const HWND main = app->MainWindow();
    bool absent = true;
    for (int id = 570; id <= 577; ++id) absent &= GetDlgItem(main, id) == nullptr;
    for (int id : {32900, 32901, 32902, 32904, 32905, 32906, 32907}) absent &= GetDlgItem(main, id) == nullptr;
    Check(absent, "scale readouts and curve-editor launcher controls were not created");
    Require(GetDlgItem(main, panel::kBoxSelectRadio) != nullptr, "original BOX control exists");
    const wchar_t* box[] = {L"BOX選択", L"Box-sel", L"框选"};
    Check(Caption(GetDlgItem(main, panel::kBoxSelectRadio)) == box[app->EnglishUI()], "491 is labelled BOX in the active language");
    const HWND combo = GetDlgItem(main, panel::kInterpCurveCombo);
    Require(combo != nullptr, "original interpolation combo exists");
    app->state.optflag[0] = 0;
    EnsureInterpChannelCombo(app, true);
    Check(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 5, "bone combo contains four original channels plus all");
    const wchar_t* all[] = {L"すべて", L"all", L"全部"};
    Check(ComboText(combo, 4) == all[app->EnglishUI()], "bone all remains at index four");
    SendMessageW(combo, CB_SETCURSEL, 4, 0);
    EnsureInterpChannelCombo(app);
    Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 4, "normal combo refresh preserves original all selection");
    app->state.optflag[0] = 1;
    EnsureInterpChannelCombo(app, true);
    Check(SendMessageW(combo, CB_GETCOUNT, 0, 0) == 7 && ComboText(combo, 6) == all[app->EnglishUI()],
          "camera keeps six original Bezier channels plus all");
    app->state.optflag[0] = 0;
    const HWND boxControl = GetDlgItem(main, panel::kBoxSelectRadio);
    SendMessageW(boxControl, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kBoxSelectRadio, BN_CLICKED), reinterpret_cast<LPARAM>(boxControl));
    Check(app->EditMode() == ViewportEditMode::BoneBox, "real 491 command selects BOX instead of scale");
    SendMessageW(boxControl, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kBoxSelectRadio, BN_CLICKED), reinterpret_cast<LPARAM>(boxControl));
    Check(app->EditMode() == ViewportEditMode::None, "real BOX command can be released");
    // The empty fixture may inspect individual model-panel controls, but must
    // return to the camera panel before a general refresh. LocalizeUI's real
    // model branch calls PostLoadInit(SelectedModel()), which requires a model.
    app->state.optflag[0] = 1;
    EnsureInterpChannelCombo(app, true);
}
void CheckOriginalShadow(MMDApp* app) {
    // Shadow controls belong to the camera panel; the fixture has no model.
    app->state.optflag[0] = 1;
    LocalizeUI(app);
    const HWND main = app->MainWindow();
    const HWND fine = GetDlgItem(main, panel::kSelfShadowCheckbox);
    const HWND slider = GetDlgItem(main, panel::kSelfShadowRangeSlider);
    Require(fine && slider && GetDlgItem(main, panel::kSelfShadowRangeEdit), "original fine-shadow and distance controls exist");
    Check(!GetDlgItem(main, panel::kAdaptiveShadowCheckbox), "adaptive shadow control was not created");
    Check(!GetDlgItem(main, panel::kShadowFilterCheckbox), "enhanced filter control was not created");
    Check(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0) == 0 && SendMessageW(slider, TBM_GETRANGEMAX, 0, 0) == 9999,
          "original shadow distance slider range is 0 through 9999");
    for (bool enabled : {false, true, false}) {
        SendMessageW(fine, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(panel::kSelfShadowCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(fine));
        const unsigned expected = enabled ? 4096 : 2048;
        Check(app->Renderer()->renderTargetWidth == expected && app->Renderer()->renderTargetHeight == expected,
              "real fine-shadow command retains original 2048/4096 dimensions");
    }
    for (int position : {0, 5000, 9999}) {
        SendMessageW(slider, TBM_SETPOS, TRUE, position);
        SendMessageW(main, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        const double expected = (10000.0 - position) / 100000.0;
        Check(std::abs(app->ShadowDistance() - expected) < 1e-7, "real slider edits original shadow coverage");
    }
    SetWindowTextW(GetDlgItem(main, panel::kSelfShadowRangeEdit), L"4321.5");
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(565, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, 565)));
    Check(std::abs(app->ShadowDistance() - 0.056785) < 1e-7, "original shadow numeric command preserves fractional precision");
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDPIAware();
    Require(!GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll"), "test runtime contains no native MME proxy");
    Require(MH_Initialize() == MH_OK, "initialize hidden-window guards");
    Hook(&ShowWindow, &Hide, &originalShow);
    Hook(&SetWindowPos, &Position, &originalPosition);
    Hook(&SetWindowPlacement, &Placement, &originalPlacement);
    Hook(&MessageBoxA, &UnexpectedA); Hook(&MessageBoxW, &UnexpectedW);
    Hook(&GetOpenFileNameA, &UnexpectedPickerA); Hook(&GetOpenFileNameW, &UnexpectedPickerW);
    Hook(&GetSaveFileNameA, &UnexpectedPickerA); Hook(&GetSaveFileNameW, &UnexpectedPickerW);
    Require(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable hidden-window guards");
    const HHOOK guard = SetWindowsHookExW(WH_CBT, &Guard, nullptr, GetCurrentThreadId());
    Require(guard != nullptr, "prevent top-level creation visibility and activation");
    auto* app = new MMDApp;
    g_Block = app; app->state = MMDAppState{}; app->InitDefaults();
    Require(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize real hidden production host");
    HandleWindowSize(app);
    AuditHidden("startup");
    for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
        Require(app->state.optflag[0] != 0, "empty fixture enters language refresh in camera mode");
        std::printf("LANGUAGE begin=%u\n", unsigned(language));
        app->EnglishUI() = language;
        LocalizeUI(app);
        std::printf("LANGUAGE %u\n", unsigned(language));
        CheckControls(app); CheckMenus(app);
        AuditHidden("localized controls");
    }
    CheckOriginalShadow(app);
    AuditHidden("commands complete");
    if (OwnWindow(GetFocus())) SetFocus(nullptr); // Commit while app resources still exist.
    ShutdownCleanup(app);
    // Production WM_DESTROY persists mmconfig.ini. This test deliberately
    // avoids that write, while still destroying its owned controls/window.
    const HWND main = app->MainWindow();
    if (IsWindow(main)) {
        SetWindowLongPtrW(main, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DefWindowProcW));
        Require(DestroyWindow(main) != FALSE, "destroy owned hidden host without saving user preferences");
    }
    g_Block = nullptr; delete app;
    AuditHidden("cleanup");
    UnhookWindowsHookEx(guard);
    MH_Uninitialize();
    std::printf("RESULT checks=%d failures=%d activation_attempts_blocked=%u explicit_frame_driver_calls=0 file_dialogs=0\n", checks, failures, blockedActivations);
    return failures ? 1 : 0;
}
