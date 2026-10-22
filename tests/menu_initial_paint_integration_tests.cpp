// MAINTENANCE: regression-keep. Production entry integration with owned, hidden Default-desktop windows.
// Only CreateUIControls is replaced: no complete MMS startup, MME DLL or GPU.
// Popup discovery and paint are simulated; no visible-menu claim is made.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <MinHook.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/effect_display.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/window/menu_initial_paint_detail.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <new>

using namespace mikumikustudio;
namespace detail = menu_initial_paint::detail;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
constexpr wchar_t kClass[] = L"MMSMenuInitialPaintIntegration";
struct Fixture {
    static Fixture* active;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    MMDApp* previous = g_Block;
    std::array<HWND, 4> roots{};
    std::array<HWND, 32> descendants{};
    std::size_t rootCount = 0, descendantCount = 0;
    HWND popup{};
    HMENU menu{};
    ULONGLONG clock = 1000;
    unsigned installs = 0, arms = 0, paints = 0, creates = 0;
    unsigned inits = 0, commands = 0, notifications = 0;
    bool attachedAtCreate = false;

    static ULONGLONG Now() noexcept { return active->clock; }
    static void Enumerate(WNDENUMPROC callback, LPARAM data) noexcept {
        if (active->popup) callback(active->popup, data);
    }
    static bool Describe(HWND window, detail::Popup& out) noexcept {
        if (window != active->popup || !IsWindow(window)) return false;
        out = {active->menu, GetCurrentThreadId(), true, true};
        return true;
    }
    static void Paint(HWND window) noexcept {
        if (window == active->popup) ++active->paints;
    }
    static UINT_PTR Arm(HWND window, UINT_PTR id, UINT period) noexcept {
        ++active->arms;
        return detail::NativePlatform().arm(window, id, period);
    }
    static void Disarm(HWND window, UINT_PTR id) noexcept {
        detail::NativePlatform().disarm(window, id);
    }
    static bool Install(HWND window, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data) noexcept {
        ++active->installs;
        return detail::NativePlatform().install(window, proc, id, data);
    }
    static bool Controls(MMDApp* value, HWND window) {
        Check(active && value == active->app.get() && window == value->MainWindow(),
            "production WM_CREATE reaches the exact controls initialization boundary");
        ++active->creates;
        active->attachedAtCreate = detail::Inspect(window).attached;
        return true;
    }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_CREATE &&
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams == active) {
            active->app->MainWindow() = window;
            // Real WM_CREATE produced by CreateWindowEx; never forward destroy,
            // size or paint into full MMS settings/renderer startup/shutdown.
            return MainWndProc(window, message, wp, lp);
        }
        if (message == WM_INITMENUPOPUP) { ++active->inits; return 0x1234; }
        if (message == WM_COMMAND) { ++active->commands; return 0x5678; }
        if (message == WM_NOTIFY) { ++active->notifications; return CDRF_NEWFONT; }
        return DefWindowProcW(window, message, wp, lp);
    }
    static BOOL CALLBACK Collect(HWND child, LPARAM data) {
        auto& self = *reinterpret_cast<Fixture*>(data);
        Check(self.descendantCount < self.descendants.size(), "fixed descendant collection capacity");
        self.descendants[self.descendantCount++] = child;
        return TRUE;
    }
    Fixture() {
        Check(!active && detail::AttachmentCount() == 0, "isolated fixture starts without attachments");
        new (&app->state) MMDAppState{};
        g_Block = app.get(); active = this;
        const detail::Platform platform{Now, Enumerate, Describe, Paint, Arm, Disarm, Install};
        Check(detail::SetPlatformForTesting(&platform), "install popup seam with native subclass and timers");
        Check(MH_Initialize() == MH_OK, "initialize private process hook");
        Check(MH_CreateHook(&CreateUIControls, &Controls, nullptr) == MH_OK &&
            MH_EnableHook(&CreateUIControls) == MH_OK, "replace only heavy controls initialization");
    }
    ~Fixture() {
        NoInput();
        for (std::size_t i = 0; i < rootCount; ++i) DestroyRoot(roots[i]);
        Check(detail::AttachmentCount() == 0, "real window destruction clears all helper attachments");
        if (menu) Check(DestroyMenu(menu) != FALSE, "destroy owned popup menu");
        Check(MH_DisableHook(&CreateUIControls) == MH_OK && MH_RemoveHook(&CreateUIControls) == MH_OK,
            "restore production controls entry");
        Check(MH_Uninitialize() == MH_OK, "release private process hook");
        Check(detail::SetPlatformForTesting(nullptr), "restore platform after all windows are destroyed");
        g_Block = previous; active = nullptr;
    }
    HWND Root(bool main = false) {
        Check(rootCount < roots.size(), "root window capacity");
        auto window = CreateWindowExW(0, kClass, L"hidden integration", WS_OVERLAPPED,
            0, 0, 96, 64, nullptr, nullptr, GetModuleHandleW(nullptr), main ? this : nullptr);
        Check(window && !IsWindowVisible(window), "real hidden top-level window creation");
        roots[rootCount++] = window; return window;
    }
    HWND Child(HWND parent, const wchar_t* cls, DWORD extra = 0) {
        auto window = CreateWindowExW(0, cls, L"", WS_CHILD | extra,
            0, 0, 48, 24, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window && !IsWindowVisible(window), "real hidden child window creation");
        return window;
    }
    void CollectChildren(HWND editor) {
        descendantCount = 0;
        EnumChildWindows(editor, Collect, reinterpret_cast<LPARAM>(this));
    }
    void DestroyRoot(HWND window) {
        if (!window || !IsWindow(window)) return;
        Check(DestroyWindow(window) != FALSE, "destroy owned root without explicit helper detach");
        Check(!IsWindow(window) && !detail::Inspect(window).attached, "NCDESTROY removes root attachment");
    }
    void NoInput() const {
        const HWND foreground = GetForegroundWindow(), focus = GetFocus(), capture = GetCapture();
        for (std::size_t i = 0; i < rootCount; ++i) if (IsWindow(roots[i])) {
            Check(!IsWindowVisible(roots[i]), "fixture never shows a root window");
            for (HWND current : {foreground, focus, capture})
                Check(current != roots[i] && !IsChild(roots[i], current), "owned windows acquire no foreground focus or capture");
        }
    }
    void Route(HWND owner) {
        if (!menu) {
            menu = CreatePopupMenu();
            Check(menu && AppendMenuW(menu, MF_STRING, 18001, L"integration command"), "own real HMENU");
            popup = Root();
        }
        const auto beforeInits = inits, beforeArms = arms, beforePaints = paints;
        clock = 1000;
        Check(SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0) == 0x1234 &&
            inits == beforeInits + 1, "menu INIT passes through once with exact result");
        if (build_profile::original) {
            Check(!detail::Inspect(owner).attached && arms == beforeArms, "original profile creates no helper timer or state");
        } else {
            Check(detail::Inspect(owner).openings == 1 && detail::Inspect(owner, menu).ready &&
                arms == beforeArms + 1, "production attachment starts one opening and one native timer");
        }
        clock = 1016;
        SendMessageW(owner, WM_TIMER, detail::kTimer, 0);
        SendMessageW(owner, WM_TIMER, detail::kTimer, 0);
        Check(paints == beforePaints + (build_profile::original ? 0u : 1u),
            "production-installed subclass performs one first paint, repeated tick does not duplicate it");
        const auto beforeCommands = commands;
        Check(SendMessageW(owner, WM_COMMAND, 18001, 0) == 0x5678 && commands == beforeCommands + 1,
            "command passes through once with exact result");
        SendMessageW(owner, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
        Check(!detail::Inspect(owner).armed && !detail::Inspect(owner, menu).found,
            "uninit removes opening and disarms timer");
        NoInput();
    }
    void Display(HWND editor, HWND list, HWND nonList) {
        NMLVCUSTOMDRAW draw{};
        draw.nmcd.hdr.hwndFrom = list;
        draw.nmcd.hdr.code = NM_CUSTOMDRAW;
        constexpr std::array<DWORD, 3> stages{CDDS_PREPAINT, CDDS_ITEMPREPAINT, CDDS_SUBITEM | CDDS_ITEMPREPAINT};
        constexpr std::array<LRESULT, 3> additions{CDRF_NOTIFYITEMDRAW, CDRF_NOTIFYSUBITEMDRAW, CDRF_NOTIFYPOSTPAINT};
        for (std::size_t i = 0; i < stages.size(); ++i) {
            draw.nmcd.dwDrawStage = stages[i];
            const auto before = notifications;
            Check(SendMessageW(editor, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&draw)) == (CDRF_NEWFONT | additions[i]) &&
                notifications == before + 1, "existing real display subclass composes custom draw flags and forwards once");
        }
        draw.nmcd.hdr.hwndFrom = nonList;
        const auto before = notifications;
        Check(SendMessageW(editor, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&draw)) == CDRF_NEWFONT &&
            notifications == before + 1, "non-list notification retains native return value");
    }
};
Fixture* Fixture::active = nullptr;

void MainEntryChecks() {
    Fixture f;
    const auto main = f.Root(true);
    Check(f.creates == 1 && f.attachedAtCreate == !build_profile::original &&
        detail::Inspect(main).attached == !build_profile::original,
        "real production WM_CREATE attaches before controls initialization only in enhanced profile");
    Check(f.installs == (build_profile::original ? 0u : 1u) &&
        detail::AttachmentCount() == (build_profile::original ? 0u : 1u), "main entry installs exactly one native subclass");
    // Never call Attach(main) here: doing so would conceal missing integration.
    f.Route(main);
    f.DestroyRoot(main);
    Check(detail::AttachmentCount() == 0, "main NCDESTROY clears integration state");
}
void EffectEntryChecks() {
    Fixture f;
    const auto editor = f.Root();
    const auto container = f.Child(editor, L"STATIC");
    const auto grandchild = f.Child(container, L"EDIT");
    const auto list = f.Child(editor, WC_LISTVIEWW, LVS_REPORT);
    f.CollectChildren(editor);
    Check(f.descendantCount >= 3, "fixture contains child grandchild and real listview descendants");
    Check(detail::AttachmentCount() == 0, "plain editor creation does not implicitly attach helper");
    InstallEffectListDisplay(editor, f.app.get());
    const std::size_t expected = build_profile::original ? 0 : f.descendantCount + 1;
    Check(detail::Inspect(editor).attached == !build_profile::original &&
        detail::AttachmentCount() == expected && f.installs == expected,
        "production display installation attaches editor and each existing descendant once");
    std::array<std::uint64_t, 32> epochs{};
    for (std::size_t i = 0; i < f.descendantCount; ++i) {
        const auto state = detail::Inspect(f.descendants[i]);
        Check(state.attached == !build_profile::original, "every actual descendant receives expected profile attachment");
        epochs[i] = state.ownerEpoch;
    }
    const auto editorEpoch = detail::Inspect(editor).ownerEpoch;
    f.Display(editor, list, container);
    InstallEffectListDisplay(editor, f.app.get());
    Check(f.installs == expected && detail::AttachmentCount() == expected &&
        detail::Inspect(editor).ownerEpoch == editorEpoch, "repeated production installation preserves owner and install count");
    for (std::size_t i = 0; i < f.descendantCount; ++i)
        Check(detail::Inspect(f.descendants[i]).ownerEpoch == epochs[i], "repeated installation preserves descendant epochs");
    f.Display(editor, list, container);
    f.Route(editor);
    const auto late = f.Child(container, L"STATIC");
    Check(!detail::Inspect(late).attached, "late child starts native until production installation is requested again");
    InstallEffectListDisplay(editor, f.app.get());
    Check(detail::Inspect(late).attached == !build_profile::original &&
        f.installs == expected + (build_profile::original ? 0u : 1u), "repeat display installation discovers only the new descendant");
    Check(DestroyWindow(container) != FALSE, "destroy an actual child subtree");
    for (auto window : {container, grandchild, late})
        Check(!IsWindow(window) && !detail::Inspect(window).attached, "subtree NCDESTROY cleans descendants without manual detach");
    Check(detail::Inspect(editor).attached == !build_profile::original &&
        detail::Inspect(list).attached == !build_profile::original &&
        detail::AttachmentCount() == (build_profile::original ? 0 : expected - 2),
        "subtree destruction preserves sibling and editor state only");
    f.Display(editor, list, editor);
    f.DestroyRoot(editor);
    Check(detail::AttachmentCount() == 0 && !detail::Inspect(list).attached,
        "editor destruction clears all remaining descendant state");
    Check(f.creates == 0, "MME display path never enters whole app controls initialization");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "require inherited Default desktop without switching it");
    INITCOMMONCONTROLSEX common{sizeof common, ICC_LISTVIEW_CLASSES};
    Check(InitCommonControlsEx(&common) != FALSE, "initialize real listview class");
    WNDCLASSW cls{}; cls.lpfnWndProc = Fixture::Proc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = kClass;
    Check(RegisterClassW(&cls) != 0, "register hidden integration class");
    MainEntryChecks(); EffectEntryChecks();
    Check(UnregisterClassW(kClass, cls.hInstance) != FALSE, "all owned integration windows destroyed");
    std::printf("PASS menu initial paint integration checks=%u profile=%s; popup visuals not tested\n", checks,
        build_profile::original ? "original" : "enhanced");
    return 0;
}
