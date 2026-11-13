// Exercise the exact shared production adapter on fixture-owned native windows.
// A supervisor must hold the non-input desktop until this process exits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <MinHook.h>
#include "../src/window/native_dialog_backdrop.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
namespace backdrop = mikumikustudio::native_dialog_backdrop;
namespace {
unsigned checks = 0, failures = 0;
bool counterexample = false, counterexampleUsed = false, retainCounterexample = false;
std::wstring desktopName;
HDESK desktop = nullptr; // Intentionally retained through process termination.
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", message); }
}
void Need(bool value, const char* message) {
    Check(value, message);
    if (!value) { std::fflush(stdout); std::exit(3); }
}
void PrivateDesktop() {
    wchar_t name[256]{}; DWORD size = 0; BOOL input = TRUE;
    const auto current = GetThreadDesktop(GetCurrentThreadId());
    Need(GetUserObjectInformationW(current, UOI_NAME, name, sizeof name, &size) && desktopName == name,
        "thread is on the exact supervisor-held fixture desktop");
    Need(GetUserObjectInformationW(current, UOI_IO, &input, sizeof input, &size) && !input,
        "fixture desktop is never the input desktop");
}
void NoReparse(fs::path path) {
    for (;;) {
        const auto attr = GetFileAttributesW(path.c_str());
        Need(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT), "fixture parent has no reparse points");
        const auto parent = path.parent_path(); if (parent == path) break; path = parent;
    }
}
void Prepare(int argc, wchar_t** argv) {
    Need((argc == 4 || argc == 5) && std::wstring(argv[1]) == L"--desktop-bootstrap",
        "usage: test --desktop-bootstrap UNIQUE-DESKTOP NEW-OUTPUT [--counterexample-early-release|--counterexample-retain]");
    if (argc == 5) {
        retainCounterexample = std::wstring(argv[4]) == L"--counterexample-retain";
        Need(retainCounterexample || std::wstring(argv[4]) == L"--counterexample-early-release", "only explicit lifetime counterexamples are accepted");
        counterexample = true;
    }
    desktopName = argv[2];
    Need(!desktopName.empty() && desktopName.size() < 128 && _wcsicmp(desktopName.c_str(), L"Default") != 0, "fresh non-default desktop name");
    for (wchar_t ch : desktopName) Need((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
        (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_', "ASCII desktop identifier");
    const fs::path output = fs::path(argv[3]).lexically_normal();
    Need(output.is_absolute() && output != output.root_path() && !fs::exists(output), "new absolute fixture output only");
    NoReparse(output.parent_path());
    Need(fs::create_directory(output), "exclusively create fixture output");
    fs::current_path(output);
    unsigned windows = 0;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND, LPARAM data)->BOOL { ++*reinterpret_cast<unsigned*>(data); return TRUE; }, reinterpret_cast<LPARAM>(&windows));
    Need(windows == 0, "create desktop before any thread windows");
    bool exists = false;
    Need(EnumDesktopsW(GetProcessWindowStation(), [](LPWSTR name, LPARAM data)->BOOL {
        if (_wcsicmp(name, desktopName.c_str()) == 0) *reinterpret_cast<bool*>(data) = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&exists)) && !exists, "desktop name is not already in use");
    SetLastError(ERROR_SUCCESS);
    desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const auto error = GetLastError();
    Need(desktop && error != ERROR_ALREADY_EXISTS && SetThreadDesktop(desktop), "create and attach non-input desktop without token or ACL changes");
    PrivateDesktop();
    const std::string expected(desktopName.begin(), desktopName.end());
    { std::ofstream out("desktop-ready.tmp", std::ios::binary); out << expected; out.close(); Need(bool(out), "write ready receipt"); }
    Need(MoveFileExW(L"desktop-ready.tmp", L"desktop-ready.txt", MOVEFILE_WRITE_THROUGH), "publish exact desktop name");
    const auto end = GetTickCount64() + 30000;
    while (!fs::exists("desktop-held.txt") && GetTickCount64() < end) Sleep(10);
    Need(fs::is_regular_file("desktop-held.txt"), "supervisor must hold desktop within deadline");
    std::ifstream receipt("desktop-held.txt", std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(receipt)), {});
    Need(actual == expected, "supervisor receipt matches exact desktop");
    PrivateDesktop();
}

struct Resources {
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HRGN update = nullptr, replay = nullptr;
    bool Valid() const {
        BITMAP info{}; RECT bounds{};
        return GetCurrentObject(memory, OBJ_BITMAP) == bitmap && GetObjectW(bitmap, sizeof info, &info) == sizeof info &&
            GetRgnBox(update, &bounds) != ERROR && GetRgnBox(replay, &bounds) != ERROR;
    }
    bool Released() const {
        BITMAP info{}; RECT bounds{};
        // GetObjectType alone returned nonzero for successfully deleted handles
        // in this fixture, even after GdiFlush. Verify usable object state instead;
        // independent deletion hooks also require one successful release per owner.
        return !GetCurrentObject(memory, OBJ_BITMAP) && GetObjectW(bitmap, sizeof info, &info) == 0 &&
            GetRgnBox(update, &bounds) == ERROR && GetRgnBox(replay, &bounds) == ERROR;
    }
};
enum class Mode { Normal, Forward, Erase, Fallback };
const char* Name(Mode mode) { return mode == Mode::Normal ? "normal" : mode == Mode::Forward ? "forward" : mode == Mode::Erase ? "erase" : "fallback"; }
constexpr UINT destroyMessage = WM_APP + 73;
constexpr LONG_PTR businessMarker = 0x123456;
struct Fixture {
    HWND window = nullptr;
    Mode mode = Mode::Normal;
    Resources resources;
    DWORD thread = 0;
    bool armed = false, destroyedInCallback = false, resourcesSurvivedCallback = false;
    unsigned paintDepth = 0, ncDestroy = 0, bitbltDenied = 0, fillDenied = 0;
    unsigned deletes[4]{}, successfulDeletes[4]{}; // DC, bitmap, update, replay
    HDC paintDc = nullptr;
};
Fixture* live = nullptr;
decltype(&BeginPaint) realBegin = nullptr;
decltype(&EndPaint) realEnd = nullptr;
decltype(&BitBlt) realBlt = nullptr;
decltype(&FillRect) realFill = nullptr;
decltype(&DeleteObject) realDeleteObject = nullptr;
decltype(&DeleteDC) realDeleteDC = nullptr;
BOOL WINAPI ObserveDeleteObject(HGDIOBJ object) {
    int index = -1;
    if (object && live && live->thread == GetCurrentThreadId()) {
        if (object == live->resources.bitmap) index = 1;
        else if (object == live->resources.update) index = 2;
        else if (object == live->resources.replay) index = 3;
    }
    const BOOL result = realDeleteObject(object);
    if (index >= 0) { ++live->deletes[index]; if (result) ++live->successfulDeletes[index]; }
    return result;
}
BOOL WINAPI ObserveDeleteDC(HDC dc) {
    const bool match = dc && live && live->thread == GetCurrentThreadId() && dc == live->resources.memory;
    const BOOL result = realDeleteDC(dc);
    if (match) { ++live->deletes[0]; if (result) ++live->successfulDeletes[0]; }
    return result;
}
bool PaintTarget(HDC dc) {
    return live && live->armed && live->mode == Mode::Fallback && live->thread == GetCurrentThreadId() &&
        live->paintDc && dc == live->paintDc && GetObjectType(dc) == OBJ_DC && WindowFromDC(dc) == live->window;
}
HDC WINAPI ObserveBegin(HWND window, LPPAINTSTRUCT paint) {
    const auto dc = realBegin(window, paint);
    if (live && live->armed && live->mode == Mode::Fallback && live->thread == GetCurrentThreadId() && window == live->window)
        live->paintDc = dc;
    return dc;
}
BOOL WINAPI ObserveEnd(HWND window, const PAINTSTRUCT* paint) {
    if (live && live->thread == GetCurrentThreadId() && window == live->window && paint && paint->hdc == live->paintDc)
        live->paintDc = nullptr;
    return realEnd(window, paint);
}
BOOL WINAPI DenyBlt(HDC dc, int x, int y, int w, int h, HDC source, int sx, int sy, DWORD rop) {
    if (PaintTarget(dc) && source == live->resources.memory) { ++live->bitbltDenied; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return realBlt(dc, x, y, w, h, source, sx, sy, rop);
}
int WINAPI DenyFill(HDC dc, const RECT* rect, HBRUSH brush) {
    if (PaintTarget(dc)) { ++live->fillDenied; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return realFill(dc, rect, brush);
}
void DestroyInside(Fixture& f) {
    f.armed = false;
    Check(f.resources.Valid(), "all owned GDI objects are live at the destruction boundary");
    Need(DestroyWindow(f.window), "destroy only the current fixture window synchronously");
    f.destroyedInCallback = true;
    Check(!IsWindow(f.window) && f.ncDestroy == 1, "nested WM_NCDESTROY completes before callback returns");
    f.resourcesSurvivedCallback = f.resources.Valid();
    Check(f.resourcesSurvivedCallback, "outer adapter entry retains its GDI objects through nested destruction");
    bool noRelease = true; for (unsigned calls : f.deletes) noRelease = noRelease && calls == 0;
    Check(noRelease, "none of the four owned GDI objects is deleted during nested destruction");
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* f = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        f = static_cast<Fixture*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(f));
    }
    if (f && f->armed && ((f->mode == Mode::Forward && message == destroyMessage) ||
        (f->mode == Mode::Fallback && message == WM_PAINT))) {
        if (f->mode == Mode::Fallback) Check(f->bitbltDenied == 1 && f->fillDenied == 1 && !f->paintDc,
            "real first-pass copy and fill failed before the native paint callback");
        DestroyInside(*f); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
LRESULT CALLBACK ObserveWindow(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference) {
    auto& f = *reinterpret_cast<Fixture*>(reference);
    if (message == WM_NCDESTROY) {
        ++f.ncDestroy;
        if (counterexample && !retainCounterexample && !counterexampleUsed && f.mode == Mode::Forward) {
            DWORD_PTR value = 0;
            Need(GetWindowSubclass(window, backdrop::detail::BackdropProc, backdrop::detail::kBackdropSubclass, &value), "counterexample targets exact attached production state");
            // Deliberately violate the lease using only this fixture's buffer.
            // The normal test never changes adapter state or suppresses teardown.
            reinterpret_cast<backdrop::detail::DialogBackdrop*>(value)->buffer.Reset();
            counterexampleUsed = true;
        }
        RemoveWindowSubclass(window, ObserveWindow, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    if (message == WM_ERASEBKGND && f.armed && f.mode == Mode::Erase && f.paintDepth) {
        Check(f.paintDepth == 1, "destruction occurs during the real BeginPaint erase reentry");
        DestroyInside(f); return 1;
    }
    if (message == WM_PAINT) ++f.paintDepth;
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_PAINT) --f.paintDepth;
    return result;
}
void RunCase(Mode mode, unsigned iteration) {
    PrivateDesktop(); Fixture f; f.mode = mode; f.thread = GetCurrentThreadId(); live = &f;
    f.window = CreateWindowExW(0, L"MMSBackdropLifetimeFixture", L"isolated backdrop fixture", WS_OVERLAPPEDWINDOW,
        30, 30, 240, 180, nullptr, nullptr, GetModuleHandleW(nullptr), &f);
    Need(f.window != nullptr, "create owned native fixture");
    SetWindowLongPtrW(f.window, DWLP_USER, businessMarker);
    backdrop::Attach(f.window);
    DWORD_PTR reference = 0, second = 0;
    Need(GetWindowSubclass(f.window, backdrop::detail::BackdropProc, backdrop::detail::kBackdropSubclass, &reference), "actual shared production adapter is attached");
    backdrop::Attach(f.window);
    Need(GetWindowSubclass(f.window, backdrop::detail::BackdropProc, backdrop::detail::kBackdropSubclass, &second) && second == reference, "duplicate Attach preserves one live state");
    Need(SetWindowSubclass(f.window, ObserveWindow, 17, reinterpret_cast<DWORD_PTR>(&f)), "observe exact fixture callbacks");
    ShowWindow(f.window, SW_SHOWNOACTIVATE);
    Need(RedrawWindow(f.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW), "warm buffer through actual native WM_PAINT");
    auto* state = reinterpret_cast<backdrop::detail::DialogBackdrop*>(reference);
    Need(state->buffer.Bitmap() != nullptr && state->entries == 0 && !state->retired, "fresh adapter rendered and returned with no outstanding entry");
    RECT client{}; Need(GetClientRect(f.window, &client), "fixture client bounds");
    HDC dc = GetDC(f.window); Need(dc != nullptr, "borrow fixture DC for resource observation");
    f.resources.bitmap = state->buffer.Bitmap(); f.resources.update = state->update; f.resources.replay = state->replay;
    f.resources.memory = state->buffer.Begin(dc, client.right, client.bottom, backdrop::detail::BackdropKey(f.window, *state), GetSysColor(COLOR_3DFACE));
    Check(state->buffer.Bitmap() == f.resources.bitmap && f.resources.Valid(), "observe already-painted cached buffer without replacing it");
    Check(GetPixel(dc, 12, 12) == GetSysColor(COLOR_3DFACE), "reopened fixture paints the actual parent background");
    ReleaseDC(f.window, dc);
    Check(GetWindowLongPtrW(f.window, DWLP_USER) == businessMarker, "shared adapter preserves business context");
    bool detached = false;
    if (retainCounterexample && !counterexampleUsed && mode == Mode::Normal) {
        // Deliberately retain this one fixture's state across destruction. Release
        // it only after checking the missing teardown so the test leaves no leak.
        Need(RemoveWindowSubclass(f.window, backdrop::detail::BackdropProc, backdrop::detail::kBackdropSubclass), "counterexample detaches only the fixture's owned adapter");
        detached = counterexampleUsed = true;
    }
    f.armed = true;
    if (mode == Mode::Normal) { f.armed = false; Need(DestroyWindow(f.window), "normal fixture close"); }
    else if (mode == Mode::Forward) SendMessageW(f.window, destroyMessage, 0, 0);
    else {
        Need(InvalidateRect(f.window, nullptr, TRUE), "invalidate before synchronous paint destruction");
        SendMessageW(f.window, WM_PAINT, 0, 0);
    }
    Check(!IsWindow(f.window) && f.ncDestroy == 1, "fixture destroyed exactly once");
    Check(f.paintDepth == 0, "all outer paint observers returned");
    Need(GdiFlush(), "finish the UI thread's queued GDI work before checking released handles");
    Check(f.resources.Released(), "buffer DC, bitmap and both regions released after outer callback returns");
    bool releasedOnce = true;
    for (unsigned i = 0; i < 4; ++i) releasedOnce = releasedOnce && f.deletes[i] == 1 && f.successfulDeletes[i] == 1;
    Check(releasedOnce, "each exact owned GDI object has one successful production deletion");
    RECT regionBounds{}; BITMAP bitmapInfo{};
    std::printf("RESOURCE types=%lu/%lu/%lu/%lu bitmap_bytes=%d region_status=%d/%d dc_bitmap=%p deletes=%u/%u/%u/%u success=%u/%u/%u/%u\n",
        GetObjectType(f.resources.memory), GetObjectType(f.resources.bitmap), GetObjectType(f.resources.update), GetObjectType(f.resources.replay),
        GetObjectW(f.resources.bitmap, sizeof bitmapInfo, &bitmapInfo), GetRgnBox(f.resources.update, &regionBounds), GetRgnBox(f.resources.replay, &regionBounds),
        GetCurrentObject(f.resources.memory, OBJ_BITMAP), f.deletes[0], f.deletes[1], f.deletes[2], f.deletes[3],
        f.successfulDeletes[0], f.successfulDeletes[1], f.successfulDeletes[2], f.successfulDeletes[3]);
    if (mode != Mode::Normal) Check(f.destroyedInCallback, "requested synchronous callback destruction was reached");
    if (mode == Mode::Fallback) Check(f.bitbltDenied == 1 && f.fillDenied == 1, "fallback fault is confined to one exact live paint pass");
    else Check(f.bitbltDenied == 0 && f.fillDenied == 0, "ordinary paths receive no drawing fault injection");
    std::printf("CASE iteration=%u mode=%s callback=%u retained=%u released=%u nc=%u copy_fail=%u fill_fail=%u\n",
        iteration, Name(mode), f.destroyedInCallback, f.resourcesSurvivedCallback, f.resources.Released(), f.ncDestroy, f.bitbltDenied, f.fillDenied);
    if (IsWindow(f.window)) DestroyWindow(f.window);
    if (detached) delete state;
    live = nullptr;
}
void Hooks(bool install) {
    if (install) {
        Need(MH_Initialize() == MH_OK, "initialize only fixture process hooks");
        Need(MH_CreateHook(reinterpret_cast<void*>(&BeginPaint), reinterpret_cast<void*>(&ObserveBegin), reinterpret_cast<void**>(&realBegin)) == MH_OK, "observe production paint entry");
        Need(MH_CreateHook(reinterpret_cast<void*>(&EndPaint), reinterpret_cast<void*>(&ObserveEnd), reinterpret_cast<void**>(&realEnd)) == MH_OK, "observe production paint exit");
        Need(MH_CreateHook(reinterpret_cast<void*>(&BitBlt), reinterpret_cast<void*>(&DenyBlt), reinterpret_cast<void**>(&realBlt)) == MH_OK, "prepare exact buffer-copy fault");
        Need(MH_CreateHook(reinterpret_cast<void*>(&FillRect), reinterpret_cast<void*>(&DenyFill), reinterpret_cast<void**>(&realFill)) == MH_OK, "prepare exact direct-fill fault");
        Need(MH_CreateHook(reinterpret_cast<void*>(&DeleteObject), reinterpret_cast<void*>(&ObserveDeleteObject), reinterpret_cast<void**>(&realDeleteObject)) == MH_OK, "observe exact owned object deletion");
        Need(MH_CreateHook(reinterpret_cast<void*>(&DeleteDC), reinterpret_cast<void*>(&ObserveDeleteDC), reinterpret_cast<void**>(&realDeleteDC)) == MH_OK, "observe exact owned DC deletion");
    }
    for (void* target : {reinterpret_cast<void*>(&BeginPaint), reinterpret_cast<void*>(&EndPaint), reinterpret_cast<void*>(&BitBlt), reinterpret_cast<void*>(&FillRect),
        reinterpret_cast<void*>(&DeleteObject), reinterpret_cast<void*>(&DeleteDC)}) {
        if (install) Need(MH_EnableHook(target) == MH_OK, "enable owned hook");
        else { Need(MH_DisableHook(target) == MH_OK, "disable owned hook"); Need(MH_RemoveHook(target) == MH_OK, "remove owned hook"); }
    }
    if (!install) Need(MH_Uninitialize() == MH_OK, "release hook resources after all windows and callbacks returned");
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        Prepare(argc, argv);
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_STANDARD_CLASSES}; Need(InitCommonControlsEx(&init), "initialize native controls");
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MMSBackdropLifetimeFixture"; wc.cbWndExtra = DLGWINDOWEXTRA;
        Need(RegisterClassExW(&wc) != 0, "register private fixture class");
        Hooks(true);
        for (Mode mode : {Mode::Normal, Mode::Forward, Mode::Erase, Mode::Fallback}) RunCase(mode, 0);
        GdiFlush(); SetLastError(ERROR_SUCCESS); const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Need(before != 0 || GetLastError() == ERROR_SUCCESS, "GDI census available after one complete warm-up cycle");
        for (unsigned iteration = 1; iteration <= 16; ++iteration)
            for (Mode mode : {Mode::Normal, Mode::Forward, Mode::Erase, Mode::Fallback}) RunCase(mode, iteration);
        GdiFlush(); SetLastError(ERROR_SUCCESS); const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Check(after != 0 || GetLastError() == ERROR_SUCCESS, "final GDI census succeeds, including a valid zero count");
        Check(after == before, "complete repeated open/paint/destroy cycles retain no additional GDI objects");
        Check(counterexampleUsed == counterexample, "lifetime counterexample runs only when explicitly requested");
        Hooks(false); Need(UnregisterClassW(wc.lpszClassName, wc.hInstance), "all fixture windows gone before unregistering class");
        PrivateDesktop();
        std::printf("SUMMARY checks=%u failures=%u counterexample=%u gdi_before=%lu gdi_after=%lu cycles=17 modes=4\n", checks, failures, counterexample, before, after);
        return failures ? 1 : 0;
    } catch (...) { std::puts("FATAL unhandled fixture exception"); return 3; }
}
