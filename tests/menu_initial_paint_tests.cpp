// MAINTENANCE: regression-keep. Hidden real HWND/HMENU integration.
// Popup metadata/paint are simulated;
// this does not assert that text in a visible system menu has been repaired.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include "mikumikustudio/build_profile.hpp"
#include "../src/window/menu_initial_paint_detail.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
thread_local long failAfter = -1;
thread_local unsigned allocationFailures = 0;
}
void* operator new(std::size_t size) {
    if (failAfter == 0) { ++allocationFailures; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace mip = mikumikustudio::menu_initial_paint;
namespace detail = mip::detail;
namespace {
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", text); std::exit(1); }
}
enum class Action { None, Destroy, Exit, Uninit, Reopen, Reattach, OtherMenu, RemovedSubclass };
struct Fixture {
    struct Candidate { HWND window{}; detail::Popup description{}; unsigned paints = 0; };
    static thread_local Fixture* active;
    std::array<HWND, 16> windows{};
    std::array<HMENU, 8> menus{};
    std::array<Candidate, 8> candidates{};
    std::size_t windowCount = 0, menuCount = 0, candidateCount = 0;
    ULONGLONG clock = 1000;
    bool realClock = false, failInstall = false, failArm = false;
    unsigned arms = 0, disarms = 0, installs = 0, commands = 0, nativeTimers = 0;
    unsigned enumerations = 0, descriptions = 0, unhandledTimers = 0;
    SUBCLASSPROC installedProc = nullptr;
    bool rawRemovalSucceeded = false, rawReattachSucceeded = false;
    unsigned initBeforeReady = 0, initCalls = 0;
    bool nativeSawUncancelledState = false;
    bool originalIdleSeen = false, requireOriginalIdleBeforePaint = false, paintedBeforeOriginalIdle = false;
    HWND actionOwner{}; HMENU actionMenu{}, otherMenu{};
    Action initAction = Action::None, idleAction = Action::None, describeAction = Action::None, paintAction = Action::None;
    HWND initialForeground = GetForegroundWindow(), initialCapture = GetCapture();

    static ULONGLONG Now() noexcept { return active->realClock ? detail::NativePlatform().now() : active->clock; }
    static void Enumerate(WNDENUMPROC callback, LPARAM context) noexcept {
        ++active->enumerations;
        // The list is fixed storage: callbacks may close/reopen/destroy owners.
        const auto count = active->candidateCount;
        for (std::size_t i = 0; i < count; ++i)
            if (!callback(active->candidates[i].window, context)) break;
    }
    static bool Describe(HWND window, detail::Popup& popup) noexcept {
        ++active->descriptions;
        for (std::size_t i = 0; i < active->candidateCount; ++i) {
            if (active->candidates[i].window != window) continue;
            popup = active->candidates[i].description;
            active->Fire(active->describeAction);
            return true;
        }
        return false;
    }
    static void Paint(HWND window) noexcept {
        if (active->requireOriginalIdleBeforePaint && !active->originalIdleSeen) active->paintedBeforeOriginalIdle = true;
        for (std::size_t i = 0; i < active->candidateCount; ++i)
            if (active->candidates[i].window == window) ++active->candidates[i].paints;
        active->Fire(active->paintAction);
    }
    static UINT_PTR Arm(HWND owner, UINT_PTR id, UINT delay) noexcept {
        ++active->arms;
        return active->failArm ? 0 : detail::NativePlatform().arm(owner, id, delay);
    }
    static void Disarm(HWND owner, UINT_PTR id) noexcept {
        ++active->disarms; detail::NativePlatform().disarm(owner, id);
    }
    static bool Install(HWND owner, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR data) noexcept {
        ++active->installs;
        active->installedProc = proc;
        return !active->failInstall && detail::NativePlatform().install(owner, proc, id, data);
    }
    static LRESULT CALLBACK Original(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (active) {
            if (message == WM_UNINITMENUPOPUP && detail::Inspect(window, reinterpret_cast<HMENU>(wp)).found)
                active->nativeSawUncancelledState = true;
            if (message == WM_EXITMENULOOP && detail::Inspect(window).openings != 0)
                active->nativeSawUncancelledState = true;
            if ((message == WM_DESTROY || message == WM_NCDESTROY) && detail::Inspect(window).attached)
                active->nativeSawUncancelledState = true;
        }
        if (message == WM_INITMENUPOPUP && active) {
            ++active->initCalls;
            const auto state = detail::Inspect(window, reinterpret_cast<HMENU>(wp));
            if (state.found && !state.ready) ++active->initBeforeReady;
            if (window == active->actionOwner) active->Fire(active->initAction);
            return 0x1234;
        }
        if (message == WM_COMMAND && active) { ++active->commands; return 0x5678; }
        if (message == WM_ENTERIDLE && wp == MSGF_MENU && active) {
            active->originalIdleSeen = true;
            if (window == active->actionOwner) active->Fire(active->idleAction);
            return 0x2468;
        }
        if (message == WM_TIMER && wp == detail::kTimer && active) ++active->unhandledTimers;
        if (message == WM_TIMER && wp != detail::kTimer) return 0x4321;
        return DefWindowProcW(window, message, wp, lp);
    }
    void Fire(Action& stored) noexcept {
        const auto action = stored; stored = Action::None;
        switch (action) {
        case Action::None: break;
        case Action::Destroy: DestroyWindow(actionOwner); break;
        case Action::Exit: SendMessageW(actionOwner, WM_EXITMENULOOP, FALSE, 0); break;
        case Action::Uninit: SendMessageW(actionOwner, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(actionMenu), 0); break;
        case Action::Reopen:
            SendMessageW(actionOwner, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(actionMenu), 0);
            SendMessageW(actionOwner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(actionMenu), 0); break;
        case Action::Reattach:
            mip::Detach(actionOwner); mip::Attach(actionOwner);
            SendMessageW(actionOwner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(actionMenu), 0); break;
        case Action::OtherMenu:
            SendMessageW(actionOwner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(otherMenu), 0); break;
        case Action::RemovedSubclass:
            rawRemovalSucceeded = RemoveWindowSubclass(actionOwner, installedProc, detail::kSubclass) != FALSE;
            rawReattachSucceeded = mip::Attach(actionOwner);
            SendMessageW(actionOwner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(otherMenu), 0); break;
        }
    }
    Fixture() {
        Check(detail::AttachmentCount() == 0, "previous fixture leaves no attachments");
        active = this;
        const detail::Platform platform{Now, Enumerate, Describe, Paint, Arm, Disarm, Install};
        Check(detail::SetPlatformForTesting(&platform), "install complete thread-local popup seam");
    }
    ~Fixture() {
        for (std::size_t i = 0; i < windowCount; ++i) {
            const auto window = windows[i];
            mip::Detach(window);
            if (IsWindow(window)) {
                KillTimer(window, detail::kTimer);
                DestroyWindow(window);
            }
            Check(!IsWindow(window), "owned hidden HWND really destroyed");
        }
        for (std::size_t i = 0; i < menuCount; ++i) {
            Check(DestroyMenu(menus[i]) != FALSE, "owned HMENU destroyed exactly once");
            Check(!IsMenu(menus[i]), "no owned menu survives fixture cleanup");
        }
        Check(detail::AttachmentCount() == 0, "destroy/detach releases all helper state");
        Check(!nativeSawUncancelledState, "UNINIT/EXIT/DESTROY cancel helper state before original window procedure runs");
        Check(detail::SetPlatformForTesting(nullptr), "restore native seam with no attachments");
        active = nullptr;
    }
    HWND Window(HWND parent = nullptr) {
        Check(windowCount < windows.size(), "owned window capacity");
        const auto result = CreateWindowExW(0, L"MMSMenuPaintHiddenTests", L"hidden menu paint test",
            parent ? WS_CHILD : WS_OVERLAPPED, 0, 0, 80, 50, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(result && !IsWindowVisible(result), "create real hidden owned HWND without showing it");
        windows[windowCount++] = result; return result;
    }
    HMENU Menu() {
        Check(menuCount < menus.size(), "owned menu capacity");
        const auto result = CreatePopupMenu();
        Check(result && AppendMenuW(result, MF_STRING, 18001, L"owned command"), "create real populated popup HMENU");
        menus[menuCount++] = result; return result;
    }
    Candidate& Popup(HMENU menu) {
        Check(candidateCount < candidates.size(), "popup metadata capacity");
        auto& result = candidates[candidateCount++]; result.window = Window();
        result.description = {menu, GetCurrentThreadId(), true, true}; return result;
    }
    void Attach(HWND owner) {
        Check(mip::Attach(owner), "real subclass attachment succeeds");
        Check(detail::Inspect(owner).attached, "real attachment is inspectable");
        // Install wrapper delegates the actual SetWindowSubclass call. State
        // and native message pass-through independently verify its behavior.
    }
    void Begin(HWND owner, HMENU menu, ULONGLONG time) {
        clock = time;
        Check(SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0) == 0x1234,
            "INIT forwards the exact original result");
    }
    void Tick(HWND owner, ULONGLONG time) { clock = time; SendMessageW(owner, WM_TIMER, detail::kTimer, 0); }
    void Idle(HWND owner, ULONGLONG time) {
        clock = time;
        const HWND popup = candidateCount ? candidates[0].window : nullptr;
        Check(SendMessageW(owner, WM_ENTERIDLE, MSGF_MENU, reinterpret_cast<LPARAM>(popup)) == 0x2468,
            "idle preserves the original window procedure return value");
    }
    void NoInput() const {
        const auto foreground = GetForegroundWindow(), capture = GetCapture();
        for (std::size_t i = 0; i < windowCount; ++i)
            Check(foreground != windows[i] && capture != windows[i], "test never acquires foreground or capture");
        if (foreground != initialForeground || capture != initialCapture)
            std::puts("NOTE external foreground/capture changed; no restoration or user-window action performed");
    }
};
thread_local Fixture* Fixture::active = nullptr;

void TimingChecks() {
    for (int path = 0; path != 5; ++path) {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu); f.Attach(owner);
        f.Begin(owner, menu, 1000);
        Check(f.initBeforeReady == 1 && detail::Inspect(owner, menu).ready, "Begin precedes native INIT; Complete follows it");
        Check(detail::Inspect(owner).armed && f.arms == 1 && popup.paints == 0, "opening arms a real timer without eager painting");
        if (path == 4) {
            popup.description.visible = false; f.Tick(owner, 1599);
            Check(detail::Inspect(owner).armed && popup.paints == 0, "invisible opening waits before 600ms");
            f.Tick(owner, 1600);
            Check(!detail::Inspect(owner).armed && popup.paints == 0, "never-visible opening stops at 600ms");
            f.Tick(owner, 1700); Check(popup.paints == 0, "stale timer cannot paint expired opening");
        } else {
            const ULONGLONG visible = path == 3 ? 1280 : 1016;
            if (path == 3) { popup.description.visible = false; f.Tick(owner, 1279); popup.description.visible = true; }
            if (path == 1) f.Idle(owner, visible); else f.Tick(owner, visible);
            auto state = detail::Inspect(owner, menu);
            Check(popup.paints == 1 && state.first && state.visibleSince == visible, "first visible observation paints exactly once and starts visible clock");
            if (path < 2) {
                f.Idle(owner, visible + 1); f.Idle(owner, visible + 2); f.Idle(owner, visible + 3);
                Check(popup.paints == (path == 0 ? 2u : 1u), "idle paints once or coalesces with first observation");
            }
            f.Tick(owner, visible + 219);
            Check(popup.paints == (path == 0 ? 2u : 1u), "no final paint before 220ms of actual visibility");
            f.Tick(owner, visible + 220);
            Check(popup.paints == (path == 0 ? 3u : 2u) && !detail::Inspect(owner).armed,
                "one final paint at visible+220ms stops timer even without idle");
            f.Idle(owner, visible + 230); f.Tick(owner, visible + 300);
            Check(popup.paints == (path == 0 ? 3u : 2u), "finished opening never paints again");
        }
        f.NoInput();
    }
}
void FilteringChecks() {
    for (int invalid = 0; invalid < 4; ++invalid) {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); const auto other = f.Menu();
        auto& popup = f.Popup(menu); f.Attach(owner); f.Begin(owner, menu, 1000);
        if (invalid == 0) popup.description.visible = false;
        if (invalid == 1) popup.description.menuClass = false;
        if (invalid == 2) popup.description.thread = GetCurrentThreadId() + 1;
        if (invalid == 3) popup.description.menu = other;
        f.Tick(owner, 1016); Check(popup.paints == 0 && !detail::Inspect(owner, menu).first, "each invalid popup attribute independently excludes painting");
        popup.description = {menu, GetCurrentThreadId(), true, true};
        f.Tick(owner, 1032); Check(popup.paints == 1, "restoring only rejected attribute admits same popup");
    }
}
void IsolationChecks() {
    Fixture f; const auto a = f.Window(), b = f.Window(), child = f.Window(a);
    const auto ma = f.Menu(), mb = f.Menu(), mc = f.Menu(), second = f.Menu();
    auto& pa = f.Popup(ma); auto& pb = f.Popup(mb); auto& pc = f.Popup(mc); auto& ps = f.Popup(second);
    f.Attach(a); f.Attach(b); f.Attach(child); Check(detail::AttachmentCount() == 3, "parent child and second owner have separate attachments");
    const auto attachedEpoch = detail::Inspect(a).ownerEpoch;
    const auto installsBeforeRepeat = f.installs;
    Check(mip::Attach(a) && f.installs == installsBeforeRepeat && detail::AttachmentCount() == 3 && detail::Inspect(a).ownerEpoch == attachedEpoch,
        "repeated Attach retains one attachment and the same owner generation");
    Check(!detail::SetPlatformForTesting(nullptr), "cannot replace hooks under live attachments");
    f.Begin(a, ma, 1000); f.Begin(a, second, 1000); f.Begin(b, mb, 1000); f.Begin(child, mc, 1000);
    Check(f.initCalls == 4 && f.initBeforeReady == 4, "repeated attachment preserves exactly one native INIT route per message");
    Check(detail::Inspect(a).openings == 2, "two menus coexist in one owner");
    SendMessageW(a, WM_UNINITMENUPOPUP, reinterpret_cast<WPARAM>(ma), 0);
    Check(!detail::Inspect(a, ma).found && detail::Inspect(a, second).found, "closing one menu preserves sibling opening");
    SendMessageW(child, WM_EXITMENULOOP, FALSE, 0);
    Check(detail::Inspect(child).openings == 0 && detail::Inspect(a, second).found && detail::Inspect(b, mb).found,
        "child exit preserves parent and other owner");
    f.Tick(a, 1016); f.Tick(b, 1016); f.Tick(child, 1016);
    Check(pa.paints == 0 && ps.paints == 1 && pb.paints == 1 && pc.paints == 0, "only still-open owner/menu pairs paint");
    const auto old = detail::Inspect(a, second).openingEpoch;
    f.Begin(a, second, 1020); Check(detail::Inspect(a, second).openingEpoch != old, "same HMENU reopening obtains new epoch");
    Check(SendMessageW(a, WM_COMMAND, 18001, 0) == 0x5678 && f.commands == 1, "menu command and return value are untouched");
    Check(SendMessageW(a, WM_TIMER, 123, 0) == 0x4321, "foreign timer reaches original procedure");
    const auto paints = ps.paints; mip::Detach(a); mip::Detach(a); f.Tick(a, 1050);
    Check(ps.paints == paints && !detail::Inspect(a).attached, "double detach and queued timer are harmless");
    f.Tick(b, 1236); Check(pb.paints == 2 && !detail::Inspect(b).armed, "other owner completes after first owner detach");
    f.NoInput();
}
void ReentryChecks() {
    for (int source = 0; source < 3; ++source) {
        for (Action action : {Action::Destroy, Action::Exit, Action::Uninit, Action::Reopen, Action::Reattach, Action::OtherMenu}) {
            Fixture f; const auto owner = f.Window(), otherOwner = f.Window(); const auto menu = f.Menu(), another = f.Menu(), independent = f.Menu();
            auto& popup = f.Popup(menu); auto& extra = f.Popup(another); auto& unaffected = f.Popup(independent);
            f.Attach(owner); f.Attach(otherOwner); f.Begin(otherOwner, independent, 1000);
            f.actionOwner = owner; f.actionMenu = menu; f.otherMenu = another;
            if (source == 0) f.initAction = action;
            const auto armsBeforeInit = f.arms;
            f.Begin(owner, menu, 1000);
            if (source == 0) {
                if (action == Action::Destroy || action == Action::Exit || action == Action::Uninit)
                    Check(f.arms == armsBeforeInit,
                        "original INIT cancellation/destroy never arms its stale opening");
                else if (action == Action::Reopen || action == Action::Reattach)
                    Check(f.arms == armsBeforeInit + 1,
                        "nested INIT completion arms once; outer stale completion must not arm reopened generation again");
            }
            const auto initialEpoch = detail::Inspect(owner, menu).openingEpoch;
            const auto initialOwnerEpoch = detail::Inspect(owner).ownerEpoch;
            // A second candidate for the same HMENU is deliberately visited
            // after reentry. The old enumeration must not paint its new epoch.
            Fixture::Candidate* duplicate = nullptr;
            if (source != 0 && (action == Action::Reopen || action == Action::Reattach)) duplicate = &f.Popup(menu);
            if (source == 1) f.describeAction = action;
            if (source == 2) f.paintAction = action;
            if (source != 0) f.Tick(owner, 1016);
            if (action == Action::Destroy || action == Action::Exit || action == Action::Uninit) {
                Check(!detail::Inspect(owner, menu).found && !detail::Inspect(owner).armed,
                    "close/destroy during native INIT, describe or paint cannot resurrect old opening");
                Check(popup.paints == (source == 2 ? 1u : 0u), "invalidated callback cannot produce an extra old-generation paint");
            } else if (action == Action::Reopen || action == Action::Reattach) {
                const auto reopened = detail::Inspect(owner, menu);
                Check(reopened.found && reopened.ready && !reopened.first, "nested reopened menu is ready and has not inherited old paint flags");
                if (source != 0) {
                    Check(reopened.openingEpoch != initialEpoch || reopened.ownerEpoch != initialOwnerEpoch,
                        "describe/paint reentry creates a distinct owner/opening epoch pair");
                    Check(popup.paints == (source == 2 ? 1u : 0u) && duplicate->paints == 0,
                        "old enumeration never paints the reopened menu, even through a later matching popup");
                }
                const auto before = popup.paints; f.Tick(owner, 1032);
                Check(popup.paints == before + 1, "new generation receives its own initial paint");
                f.Tick(owner, 1252); Check(popup.paints == before + 2 && !detail::Inspect(owner).armed, "new generation completes its own visible duration");
            } else {
                Check(detail::Inspect(owner, menu).found && detail::Inspect(owner, another).found,
                    "opening another menu during callback preserves both independent states");
                f.Tick(owner, 1032); Check(extra.paints == 1, "new second menu gets one initial paint");
            }
            f.Tick(otherOwner, 1016); Check(unaffected.paints == 1, "reentry does not cancel another owner's first paint");
            f.NoInput();
        }
    }
}
void FailureChecks() {
    Fixture f; const auto owner = f.Window(), other = f.Window(); const auto menu = f.Menu(), independent = f.Menu();
    auto& popup = f.Popup(menu); auto& unaffected = f.Popup(independent);
    detail::Platform incomplete{};
    Check(!detail::SetPlatformForTesting(&incomplete), "incomplete platform table is rejected even without attachments");
    f.Attach(other); f.Begin(other, independent, 1000);
    f.failInstall = true;
    Check(!mip::Attach(owner) && f.installs == 2 && !detail::Inspect(owner).attached,
        "native subclass install failure is reached and leaves no attachment");
    Check(SendMessageW(owner, WM_COMMAND, 18001, 0) == 0x5678, "failed attachment retains native command behavior");
    f.failInstall = false; f.Attach(owner); f.failArm = true; const auto calls = f.arms;
    f.Begin(owner, menu, 1000);
    Check(f.arms == calls + 1 && !detail::Inspect(owner).armed, "SetTimer failure is reached and leaves no armed state");
    f.Tick(owner, 1016); Check(popup.paints == 0, "failed timer cannot later paint through stale delivery");
    f.failArm = false; f.Begin(owner, menu, 1020); f.Tick(owner, 1036);
    Check(popup.paints == 1 && detail::Inspect(owner).armed, "timer failure recovers on later opening");
    f.Tick(other, 1016); Check(unaffected.paints == 1, "API failures leave other owner's opening intact");
    f.NoInput();
}
LRESULT CALLBACK ForeignSubclass(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    return DefSubclassProc(window, message, wp, lp);
}
void GuardRegressionChecks() {
    {
        Fixture f; const auto owner = f.Window(); const auto oldMenu = f.Menu(), newMenu = f.Menu();
        auto& oldPopup = f.Popup(oldMenu); auto& newPopup = f.Popup(newMenu);
        f.Attach(owner); const auto epoch = detail::Inspect(owner).ownerEpoch;
        f.actionOwner = owner; f.actionMenu = oldMenu; f.otherMenu = newMenu; f.initAction = Action::RemovedSubclass;
        f.Begin(owner, oldMenu, 1000);
        DWORD_PTR installedToken = 0;
        const auto current = detail::Inspect(owner, newMenu);
        Check(f.rawRemovalSucceeded && f.rawReattachSucceeded && current.ownerEpoch != epoch,
            "raw subclass removal inside original INIT requires a new attachment generation");
        Check(GetWindowSubclass(owner, f.installedProc, detail::kSubclass, &installedToken) && installedToken == current.ownerEpoch,
            "fresh attachment generation is installed in the actual native subclass reference data");
        Check(!detail::Inspect(owner, oldMenu).found && current.found && current.ready && current.armed && f.initCalls == 2,
            "outer old INIT completion cannot revive old menu after raw-remove/reattach and nested new INIT");
        f.Tick(owner, 1016);
        Check(oldPopup.paints == 0 && newPopup.paints == 1, "only new menu paints after raw subclass replacement");
    }
    {
        Fixture f; const auto owner = f.Window();
        Check(SetWindowSubclass(owner, ForeignSubclass, detail::kSubclass, 0xabc), "install unrelated real subclass with same numeric ID");
        Check(SetTimer(owner, detail::kTimer, 10, nullptr) != 0, "unregistered owner has a real independently owned timer");
        const auto disarms = f.disarms;
        mip::Detach(owner); mip::Detach(owner);
        DWORD_PTR sentinel = 0;
        Check(f.disarms == disarms && GetWindowSubclass(owner, ForeignSubclass, detail::kSubclass, &sentinel) && sentinel == 0xabc,
            "Detach of unregistered same-thread window does not disarm or remove another component's subclass");
        const auto deadline = GetTickCount64() + 1000;
        while (f.unhandledTimers == 0 && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, owner, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (f.unhandledTimers) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        Check(f.unhandledTimers > 0, "independent system timer still actually fires after ignored Detach");
        KillTimer(owner, detail::kTimer);
        Check(RemoveWindowSubclass(owner, ForeignSubclass, detail::kSubclass), "release unrelated owned subclass explicitly");
    }
    {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu); f.Attach(owner);
        const auto invalid = CreatePopupMenu(); Check(invalid && DestroyMenu(invalid), "create then destroy actual menu handle for invalid-handle test");
        const auto arms = f.arms;
        f.Begin(owner, invalid, 1000); f.Begin(owner, nullptr, 1000);
        Check(detail::Inspect(owner).openings == 0 && !detail::Inspect(owner).armed && f.arms == arms && f.initCalls == 2,
            "destroyed and null HMENU reach native INIT but neither registers an opening nor arms a timer");
        f.Begin(owner, menu, 1020); f.Tick(owner, 1036);
        Check(popup.paints == 1, "valid menu still opens after rejected menu handles");
    }
    {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu); f.Attach(owner);
        popup.description.visible = false; f.Begin(owner, menu, 1000); f.Tick(owner, 1600);
        Check(!detail::Inspect(owner).armed && !detail::Inspect(owner, menu).finished,
            "invisible timeout stops polling without claiming completion");
        const auto enums = f.enumerations, descriptions = f.descriptions;
        popup.description.visible = true; f.Tick(owner, 1616); f.Tick(owner, 1632);
        Check(f.enumerations == enums && f.descriptions == descriptions && popup.paints == 0,
            "unarmed residual timers cannot enumerate or paint a later-visible popup");
        f.Idle(owner, 1640);
        Check(popup.paints == 1 && detail::Inspect(owner, menu).first && !detail::Inspect(owner).armed,
            "timeout retains native idle observation behavior without rearming polling");
    }
}
void IdleOriginalOrderChecks() {
    {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu);
        f.Attach(owner); f.Begin(owner, menu, 1000); f.requireOriginalIdleBeforePaint = true;
        f.Idle(owner, 1016);
        Check(f.originalIdleSeen && !f.paintedBeforeOriginalIdle && popup.paints == 1,
            "normal idle executes original content handling before initial popup paint");
    }
    for (Action action : {Action::Exit, Action::Uninit, Action::Destroy, Action::Reopen, Action::Reattach}) {
        Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu);
        f.Attach(owner); f.Begin(owner, menu, 1000);
        const auto before = detail::Inspect(owner, menu);
        f.actionOwner = owner; f.actionMenu = menu; f.idleAction = action;
        f.requireOriginalIdleBeforePaint = true; f.Idle(owner, 1016);
        Check(f.originalIdleSeen && !f.paintedBeforeOriginalIdle && popup.paints == 0,
            "original idle close/reopen cancels the captured old idle paint before any redraw");
        const auto after = detail::Inspect(owner, menu);
        if (action == Action::Reopen || action == Action::Reattach) {
            Check(after.found && after.ready && !after.first &&
                (after.ownerEpoch != before.ownerEpoch || after.openingEpoch != before.openingEpoch),
                "old idle does not adopt a generation opened synchronously by the original handler");
            f.Tick(owner, 1032); Check(popup.paints == 1, "new timer gives nested reopened menu its independent first paint");
        } else Check(!after.found && !after.armed, "idle close/destroy leaves no resurrected old opening");
    }
}
struct AllocationAttempt {
    int phase = 0;
    long budget = 0;
    bool succeeded = false;
    unsigned failures = 0;
    struct NarrowFailure {
        explicit NarrowFailure(long budget) { allocationFailures = 0; failAfter = budget; }
        ~NarrowFailure() { failAfter = -1; }
    };
    static DWORD WINAPI Run(void* context) {
        auto& attempt = *static_cast<AllocationAttempt*>(context);
        if (attempt.phase == 0) {
            // Do not call Inspect/AttachmentCount/SetPlatform here: those
            // could initialize the TLS registry before the first Attach.
            const HWND window = CreateWindowExW(0, L"MMSMenuPaintHiddenTests", L"cold attach allocation test", WS_OVERLAPPED,
                0, 0, 80, 50, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(window && !IsWindowVisible(window), "cold Attach owns a real hidden window");
            bool attached = false;
            { NarrowFailure failure(attempt.budget); attached = mip::Attach(window); }
            attempt.failures = allocationFailures;
            const auto state = detail::Inspect(window);
            attempt.succeeded = attempt.failures == 0 && attached && state.attached;
            if (!attempt.succeeded) {
                Check(attempt.failures > 0 && !attached && !state.attached && !state.armed && state.openings == 0,
                    "first TLS/owner allocation failure leaves no attachment or timer");
                Check(mip::Attach(window) && detail::Inspect(window).attached,
                    "cold allocation failure recovers on native-platform Attach");
            }
            mip::Detach(window);
            Check(detail::AttachmentCount() == 0 && DestroyWindow(window) && !IsWindow(window),
                "cold Attach worker releases native subclass and window before exit");
            return 0;
        }
        // A new owned thread for each attempt avoids reusing the production
        // TLS registry's allocation from a previous successful Attach.
        Fixture f; const auto owner = f.Window(), other = f.Window();
        const auto menu = f.Menu(), independent = f.Menu();
        auto& popup = f.Popup(menu); auto& unaffected = f.Popup(independent);
        if (attempt.phase != 0) { f.Attach(other); f.Begin(other, independent, 1000); }
        const auto beforeOther = detail::Inspect(other, independent);
        HMENU existingMenu = nullptr;
        if (attempt.phase >= 2) f.Attach(owner);
        if (attempt.phase == 3) {
            existingMenu = f.Menu(); f.Popup(existingMenu); f.Begin(owner, existingMenu, 1000);
        }
        const auto beforeExisting = detail::Inspect(owner, existingMenu);
        bool attached = false; LRESULT result = 0;
        {
            NarrowFailure failure(attempt.budget);
            if (attempt.phase >= 2)
                result = SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(menu), 0);
            else attached = mip::Attach(owner);
        }
        attempt.failures = allocationFailures;
        const auto state = detail::Inspect(owner, menu);
        if (attempt.phase >= 2) {
            Check(result == 0x1234 && f.initCalls == (attempt.phase == 3 ? 3u : 2u),
                "allocation failure inside Begin does not escape or skip the allocation-free original wndproc");
            attempt.succeeded = attempt.failures == 0 && state.found && state.ready && state.armed;
        } else attempt.succeeded = attempt.failures == 0 && attached && state.attached;
        if (!attempt.succeeded) {
            Check(attempt.failures > 0, "each rejected attempt reached actual operator-new fault injection");
            if (attempt.phase == 3) {
                const auto existing = detail::Inspect(owner, existingMenu);
                Check(!state.found && state.armed && existing.found && existing.ready && existing.armed &&
                    existing.ownerEpoch == beforeExisting.ownerEpoch && existing.openingEpoch == beforeExisting.openingEpoch &&
                    existing.first == beforeExisting.first && existing.opened == beforeExisting.opened,
                    "new menu allocation failure preserves same owner's existing opening and timer exactly");
            } else {
                Check(!state.armed && !state.found && (attempt.phase == 2 || !state.attached),
                    "failed narrow allocation leaves no opening/timer or partially attached owner");
            }
            const auto previousPaints = popup.paints; f.Tick(owner, 1016);
            Check(popup.paints == previousPaints, "stale timer cannot revive failed allocation");
        }
        if (attempt.phase != 0) {
            const auto afterOther = detail::Inspect(other, independent);
            Check(afterOther.attached && afterOther.armed && afterOther.ready &&
                afterOther.ownerEpoch == beforeOther.ownerEpoch && afterOther.openingEpoch == beforeOther.openingEpoch,
                "allocation rejection preserves another owner's registered opening");
            f.Tick(other, 1016); Check(unaffected.paints == 1, "other owner's real message route still paints after allocation injection");
        }
        if (attempt.phase < 2 && !attached) f.Attach(owner);
        if (attempt.phase < 2 || !attempt.succeeded) f.Begin(owner, menu, 1020);
        f.Tick(owner, 1036);
        Check(popup.paints == 1 && detail::Inspect(owner, menu).first,
            "removing allocation fault permits recovery through real Attach/INIT/timer");
        f.NoInput();
        return 0;
    }
};
void AllocationChecks() {
    for (int phase = 0; phase < 4; ++phase) {
        bool succeeded = false; unsigned failedAttempts = 0;
        for (long budget = 0; budget < 256; ++budget) {
            AllocationAttempt attempt; attempt.phase = phase; attempt.budget = budget;
            const HANDLE thread = CreateThread(nullptr, 0, AllocationAttempt::Run, &attempt, 0, nullptr);
            Check(thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0,
                "fresh allocation-sweep thread completes and cleans its own hidden windows");
            DWORD code = 1; Check(GetExitCodeThread(thread, &code) && code == 0, "allocation-sweep worker returns normally");
            CloseHandle(thread);
            if (attempt.succeeded) { succeeded = true; break; }
            ++failedAttempts;
        }
        Check(succeeded && failedAttempts > 0, "each cold/warm Attach and empty/nonempty Begin scan reaches real failures and then success");
        std::printf("ALLOC phase=%d failed-attempts=%u\n", phase, failedAttempts);
    }
}
struct ForeignWindow {
    HANDLE ready = nullptr, close = nullptr;
    HWND window = nullptr;
    BOOL destroyed = FALSE;
    bool installed = false, timerSet = false, subclassPreserved = false, timerPreserved = false;
    static DWORD WINAPI Run(void* context) {
        auto& self = *static_cast<ForeignWindow*>(context);
        self.window = CreateWindowExW(0, L"MMSMenuPaintHiddenTests", L"owned other-thread window", WS_OVERLAPPED,
            0, 0, 80, 50, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (self.window) {
            self.installed = SetWindowSubclass(self.window, ForeignSubclass, detail::kSubclass, 0xdef) != FALSE;
            self.timerSet = SetTimer(self.window, detail::kTimer, 16, nullptr) != 0;
        }
        SetEvent(self.ready);
        const auto waited = WaitForSingleObject(self.close, 5000);
        if (self.window) {
            DWORD_PTR sentinel = 0;
            self.subclassPreserved = GetWindowSubclass(self.window, ForeignSubclass, detail::kSubclass, &sentinel) && sentinel == 0xdef;
            self.timerPreserved = KillTimer(self.window, detail::kTimer) != FALSE;
            RemoveWindowSubclass(self.window, ForeignSubclass, detail::kSubclass);
            self.destroyed = DestroyWindow(self.window);
        }
        return waited == WAIT_OBJECT_0 ? 0 : 1;
    }
};
void ForeignThreadChecks() {
    Fixture f; ForeignWindow other;
    other.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    other.close = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(other.ready && other.close, "create owned thread synchronization events");
    DWORD threadId = 0;
    const HANDLE thread = CreateThread(nullptr, 0, ForeignWindow::Run, &other, 0, &threadId);
    Check(thread && WaitForSingleObject(other.ready, 2000) == WAIT_OBJECT_0,
        "owned worker thread creates its own hidden window");
    Check(other.window && !IsWindowVisible(other.window) && GetWindowThreadProcessId(other.window, nullptr) == threadId &&
        threadId != GetCurrentThreadId(), "cross-thread rejection uses a real other-thread HWND");
    Check(!mip::Attach(other.window) && !detail::Inspect(other.window).attached && f.installs == 0,
        "foreign-thread Attach rejects before native subclass installation");
    const auto disarms = f.disarms;
    mip::Detach(other.window);
    Check(f.disarms == disarms, "cross-thread Detach does not call the native disarm wrapper");
    Check(GetForegroundWindow() != other.window && GetCapture() != other.window,
        "hidden worker window never acquires input");
    SetEvent(other.close);
    Check(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0 && other.destroyed && !IsWindow(other.window),
        "worker destroys its own HWND before completing");
    Check(other.installed && other.timerSet && other.subclassPreserved && other.timerPreserved,
        "worker's independently owned subclass and timer survive cross-thread Detach");
    CloseHandle(thread); CloseHandle(other.ready); CloseHandle(other.close);
}
void RealTimerChecks() {
    Fixture f; f.realClock = true;
    const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu); f.Attach(owner);
    f.Begin(owner, menu, 1000); const auto deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline && (popup.paints < 2 || detail::Inspect(owner).armed)) {
        MSG message{};
        while (PeekMessageW(&message, owner, 0, 0, PM_REMOVE)) {
            if (message.message == WM_TIMER && message.wParam == detail::kTimer) ++f.nativeTimers;
            DispatchMessageW(&message);
        }
        if (popup.paints >= 2 && !detail::Inspect(owner).armed) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    Check(f.nativeTimers >= 2 && popup.paints == 2 && !detail::Inspect(owner).armed,
        "actual system WM_TIMER deliveries trigger first/final paint within two seconds without idle or sleep");
    const auto before = popup.paints;
    SendMessageW(owner, WM_EXITMENULOOP, FALSE, 0); mip::Detach(owner);
    SendMessageW(owner, WM_TIMER, detail::kTimer, 0);
    Check(popup.paints == before && !detail::Inspect(owner).attached, "residual timer after native timer cleanup cannot restart painting");
    f.NoInput();
}
void OriginalProfileChecks() {
    Fixture f; const auto owner = f.Window(); const auto menu = f.Menu(); auto& popup = f.Popup(menu);
    Check(!mip::Attach(owner) && !mip::Attach(owner), "original profile refuses new helper attachment");
    f.Begin(owner, menu, 1000); f.Tick(owner, 1016); f.Idle(owner, 1017); mip::Detach(owner);
    Check(!detail::Inspect(owner).attached && detail::AttachmentCount() == 0 && f.installs == 0 && f.arms == 0 && popup.paints == 0,
        "original profile preserves zero helper state and never installs or arms");
    Check(SendMessageW(owner, WM_COMMAND, 18001, 0) == 0x5678 && f.initCalls == 1 && f.initBeforeReady == 0,
        "original profile retains native procedure messages and return values");
    f.NoInput();
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "require inherited Default desktop without switching it");
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&controls) != FALSE, "initialize supported common-control subclass infrastructure");
    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = Fixture::Original; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MMSMenuPaintHiddenTests";
    Check(RegisterClassExW(&wc) != 0, "register owned hidden test window class");
    if (mikumikustudio::build_profile::original) OriginalProfileChecks();
    else {
        std::puts("RUN menu timing"); TimingChecks();
        std::puts("RUN menu filtering"); FilteringChecks();
        std::puts("RUN menu isolation"); IsolationChecks();
        std::puts("RUN menu reentry"); ReentryChecks();
        std::puts("RUN menu API failures"); FailureChecks();
        std::puts("RUN attachment and stale-timer guards"); GuardRegressionChecks();
        std::puts("RUN original idle ordering"); IdleOriginalOrderChecks();
        std::puts("RUN narrow allocation failures"); AllocationChecks();
        std::puts("RUN cross-thread attach rejection"); ForeignThreadChecks();
        std::puts("RUN real hidden owner timers"); RealTimerChecks();
    }
    Check(UnregisterClassW(wc.lpszClassName, wc.hInstance) != FALSE, "all real windows release their class");
    std::printf("PASS menu initial paint checks=%u profile=%s (simulated popup discovery/paint; no visible-menu assertion)\n",
        checks.load(), mikumikustudio::build_profile::original ? "original" : "enhanced");
    return 0;
}
