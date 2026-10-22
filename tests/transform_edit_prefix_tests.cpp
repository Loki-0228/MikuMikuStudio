// MAINTENANCE: regression-keep. Real hidden EDIT creation/subclass regression.
// Font/width variants simulate layout sizes, not real cross-monitor DPI.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../src/window/ui_controls.inc"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace mikumikustudio {
HWND CreateUiPanelControl(const ui::ControlSpec&, HWND, HINSTANCE, int);
LRESULT CALLBACK EditSubclassProc(HWND, UINT, WPARAM, LPARAM);
}
using namespace mikumikustudio;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
// Independent list: do not import TransformEditShowsPrefix as the oracle.
constexpr int targets[]{478,479,480,481,482,483,484,544,545,546,547,548,549,550,572,575,577};
constexpr int exclusions[]{417,448,485,506,511,516,521,554,561};
const char* longNumber = "-123456789012345678901234567890123456789012345678901234567890.12345678901234567890";
ui::ControlSpec Spec(int id) {
    for (const auto& spec : ui::kControls) if (spec.id == id) return spec;
    Check(false, "independent fixture ID exists in production control table"); return {};
}
struct View {
    std::string text;
    DWORD begin = 0, end = 0;
    LRESULT first = 0;
    int x = 0;
    int lastX = 0;
    bool modified = false;
};
View Read(HWND window) {
    View value; char text[512]{}; GetWindowTextA(window, text, sizeof text); value.text = text;
    SendMessageA(window, EM_GETSEL, reinterpret_cast<WPARAM>(&value.begin), reinterpret_cast<LPARAM>(&value.end));
    value.first = SendMessageA(window, EM_GETFIRSTVISIBLELINE, 0, 0);
    value.x = static_cast<short>(LOWORD(SendMessageA(window, EM_POSFROMCHAR, 0, 0)));
    if (!value.text.empty())
        value.lastX = static_cast<short>(LOWORD(SendMessageA(window, EM_POSFROMCHAR, value.text.size() - 1, 0)));
    value.modified = SendMessageA(window, EM_GETMODIFY, 0, 0) != 0;
    return value;
}
bool Same(const View& a, const View& b) {
    return a.text == b.text && a.begin == b.begin && a.end == b.end && a.first == b.first && a.x == b.x &&
        a.lastX == b.lastX && a.modified == b.modified;
}
struct Host {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    MMDApp* previous = g_Block;
    HWND owner = nullptr, alternate = nullptr;
    Host() {
        ::new(static_cast<void*>(&app->state)) MMDAppState{};
        owner = CreateWindowExA(0, "STATIC", "owned prefix test", WS_OVERLAPPED, 0, 0, 500, 160,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        alternate = CreateWindowExA(0, "STATIC", "owned prefix alternate", WS_OVERLAPPED, 0, 0, 500, 160,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(owner && alternate && !IsWindowVisible(owner) && !IsWindowVisible(alternate), "create only hidden owned test windows");
        app->MainWindow() = owner; app->WindowLayoutReady() = 1; app->FrameStepPlayback() = 1;
        app->CurrentFrame() = 37; app->SceneModified() = 1;
        app->CameraDistance() = -77.125f; app->BoneRotationEditDegreesX() = 12.34567f;
        g_Block = app.get();
    }
    ~Host() {
        SetFocus(owner); DestroyWindow(alternate); DestroyWindow(owner); g_Block = previous;
    }
    void Stable() const {
        Check(app->CurrentFrame() == 37 && app->SceneModified() == 1 && app->CameraDistance() == -77.125f &&
            app->BoneRotationEditDegreesX() == 12.34567f && app->FrameStepPlayback() == 1 && !app->SelectedModel(),
            "display-only messages preserve authored-state sentinels and never create a model or move the playhead");
    }
};
struct Fields {
    Host& host;
    HWND field = nullptr, native = nullptr;
    HFONT font = nullptr;
    WNDPROC original = nullptr;
    Fields(Host& h, int id, int percent, bool floating = false) : host(h) {
        const HWND parent = floating ? host.alternate : host.owner;
        if (floating) host.app->FloatingWindow() = parent;
        auto spec = Spec(id); spec.x = {0, 2}; spec.y = {0, 2};
        spec.w = MulDiv(38, percent, 100); spec.h = MulDiv(24, percent, 100);
        field = CreateUiPanelControl(spec, parent, GetModuleHandleW(nullptr), 250);
        Check(field != nullptr, "create real EDIT through production panel factory");
        const auto style = GetWindowLongPtrA(field, GWL_STYLE);
        bool target = false; for (int expected : targets) target |= id == expected;
        const auto expectedAlignment = target && !build_profile::original ? 0 : spec.style & (ES_CENTER | ES_RIGHT);
        Check((style & (ES_CENTER | ES_RIGHT)) == expectedAlignment, "production alignment follows independent target-ID and profile expectations");
        original = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(field, GWLP_WNDPROC));
        host.app->OriginalEditProc() = original;
        InstallControlSubclass(host.app.get(), field, id);
        Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(field, GWLP_WNDPROC)) == EditSubclassProc,
            "fixture installs the actual production numeric EDIT subclass");
        native = CreateWindowExA(spec.ex, "EDIT", "", DWORD(style), 2, 70, spec.w, spec.h, parent,
            reinterpret_cast<HMENU>(INT_PTR(29002)), GetModuleHandleW(nullptr), nullptr);
        Check(native != nullptr, "create unmodified native EDIT reference with the same effective style");
        font = CreateFontW(-MulDiv(14, percent, 100), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
        Check(font != nullptr, "create owned font for simulated layout size");
        for (HWND control : {field, native}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            SendMessageA(control, EM_SETLIMITTEXT, 256, 0);
            SetWindowLongPtrA(control, GWLP_USERDATA, 0x13579);
        }
    }
    ~Fields() {
        if (IsWindow(field)) SendMessageA(field, EM_SETMODIFY, FALSE, 0);
        SetFocus(host.owner);
        if (IsWindow(field)) DestroyWindow(field);
        if (IsWindow(native)) DestroyWindow(native);
        DeleteObject(font); host.app->FloatingWindow() = nullptr;
    }
    void Stable() const {
        host.Stable();
        Check(GetWindowLongPtrA(field, GWLP_USERDATA) == 0x13579 && GetWindowLongPtrA(native, GWLP_USERDATA) == 0x13579,
            "prefix presentation does not overwrite control user data");
    }
};
void Focus(HWND window) { SetFocus(window); Check(GetFocus() == window, "test focus is held by the exact owned HWND"); }
void Tail(HWND window) {
    SendMessageA(window, EM_SETSEL, 10000, 10000); SendMessageA(window, EM_SCROLLCARET, 0, 0);
    const auto value = Read(window);
    Check(value.begin == value.text.size() && value.end == value.begin,
        "fixture places the native caret at the actual text end");
    // Excluded ID 448 has no ES_AUTOHSCROLL in the real creation table.
    // Keep that native behavior; every target ID has automatic scrolling.
    if (GetWindowLongPtrA(window, GWL_STYLE) & ES_AUTOHSCROLL)
        Check(value.first > 0 || value.x < 0,
            "long scrolling numeric fixture actually hides its prefix outside the narrow EDIT");
}
void Prefix(HWND window, const std::string& expected, bool modified) {
    const auto value = Read(window); RECT rect{}; GetClientRect(window, &rect);
    Check(value.text == expected && value.modified == modified && value.begin == 0 && value.end == 0,
        "unfocused prefix presentation resets selection without altering text or native modify semantics");
    Check(value.first == 0 && value.x >= 0 && value.x < rect.right,
        "first numeric character is actually inside the visible EDIT rectangle");
}
void FocusedChecks(Fields& fields) {
    for (int operation = 0; operation < 3; ++operation) {
        const auto perform = [&](HWND window) {
            Focus(window); SetWindowTextA(window, longNumber); Tail(window);
            SendMessageA(window, EM_SETMODIFY, FALSE, 0);
            if (operation == 0) SetWindowTextA(window, "-987654321098765432109876543210987654321098765432109876543210.43210987654321");
            if (operation == 1) SendMessageA(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>("9876"));
            if (operation == 2) SendMessageA(window, WM_CHAR, '7', 1);
            Check(GetFocus() == window, "text update never steals owned editing focus");
            const auto result = Read(window);
            if (operation != 0 && (GetWindowLongPtrA(window, GWL_STYLE) & ES_AUTOHSCROLL)) {
                // prefix-full-03 observed first=0 with char zero at x=-620:
                // this native single-line EDIT was horizontally scrolled.
                const bool tail = (result.first > 0 || result.x < 0) && result.begin == result.text.size();
                if (!tail)
                    std::fprintf(stderr,
                        "DIAG focused operation=%d control=%s id=%d length=%zu selection=%lu,%lu first=%lld x=%d style=0x%llx focus=%p target=%p modified=%d\n",
                        operation, window == fields.field ? "field" : "native", GetDlgCtrlID(window), result.text.size(),
                        static_cast<unsigned long>(result.begin), static_cast<unsigned long>(result.end),
                        static_cast<long long>(result.first), result.x,
                        static_cast<unsigned long long>(GetWindowLongPtrA(window, GWL_STYLE)),
                        static_cast<void*>(GetFocus()), static_cast<void*>(window), int(result.modified));
                Check(tail, "focused insertion keeps its native end caret and horizontal scroll");
            }
            return result;
        };
        const auto actual = perform(fields.field); const auto expected = perform(fields.native);
        Check(Same(actual, expected), "focused set-text replace-selection and typing preserve native selection scrolling and text behavior");
        // prefix-full-03: EM_REPLACESEL produced lastX=60 in both real and
        // native EDITs (client width 36, glyph width 8). Native replacement
        // does not guarantee scrolling to the newly appended glyph. Preserve
        // that exact behavior above; actual WM_CHAR typing must show its tail.
        if (operation == 2 && (GetWindowLongPtrA(fields.field, GWL_STYLE) & ES_AUTOHSCROLL)) {
            RECT rect{}; GetClientRect(fields.field, &rect);
            HDC dc = GetDC(fields.field); Check(dc != nullptr, "measure actual numeric font for last-character visibility");
            const auto oldFont = SelectObject(dc, fields.font); SIZE glyph{};
            const BOOL measured = GetTextExtentPoint32A(dc, actual.text.data() + actual.text.size() - 1, 1, &glyph);
            SelectObject(dc, oldFont); ReleaseDC(fields.field, dc);
            Check(measured && glyph.cx > 0, "last numeric character has a measurable native glyph width");
            const bool visible = actual.lastX > -glyph.cx && actual.lastX < rect.right;
            if (!visible)
                std::fprintf(stderr, "DIAG last-character operation=%d id=%d field-x=%d native-x=%d client-right=%ld glyph-width=%ld length=%zu\n",
                    operation, GetDlgCtrlID(fields.field), actual.lastX, expected.lastX, rect.right, glyph.cx, actual.text.size());
            Check(visible, "last numeric glyph intersects the client view after focused insertion");
        }
        fields.Stable();
    }
    // A nonempty selection also remains native while an edit is active.
    Focus(fields.field); SetWindowTextA(fields.field, longNumber); SendMessageA(fields.field, EM_SETSEL, 25, 40);
    SendMessageA(fields.field, EM_SCROLLCARET, 0, 0); const auto selected = Read(fields.field);
    Check(selected.begin == 25 && selected.end == 40, "active user selection is not collapsed by presentation logic");
}
void UnfocusedChecks(Fields& fields, bool target) {
    Focus(fields.host.owner);
    for (int operation = 0; operation < 2; ++operation) {
        for (HWND window : {fields.field, fields.native}) {
            SetWindowTextA(window, longNumber); Tail(window); SendMessageA(window, EM_SETMODIFY, FALSE, 0);
        }
        const auto perform = [&](HWND window) {
            if (operation == 0) SetWindowTextA(window, longNumber);
            else SendMessageA(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>("999"));
        };
        perform(fields.field); perform(fields.native);
        if (target && !build_profile::original) Prefix(fields.field, std::string(longNumber) + (operation ? "999" : ""), operation != 0);
        else Check(Same(Read(fields.field), Read(fields.native)), "non-target and original-profile unfocused updates retain native display behavior");
        fields.Stable();
    }
    const auto blur = [&](HWND window) {
        Focus(window); SetWindowTextA(window, longNumber); Tail(window); SendMessageA(window, EM_SETMODIFY, FALSE, 0);
        Focus(fields.host.owner); Check(GetFocus() != window, "real blur moves focus off the numeric EDIT"); return Read(window);
    };
    const auto actual = blur(fields.field), expected = blur(fields.native);
    if (target && !build_profile::original) Prefix(fields.field, longNumber, false);
    else Check(Same(actual, expected), "excluded and original-profile real blur keep native selection and scroll behavior");
    fields.Stable();
}
struct Reentry {
    Fields& fields; int action; bool fired = false; unsigned subsequent = 0;
    std::unique_ptr<MMDApp> replacement;
    static thread_local Reentry* active;
    static LRESULT CALLBACK Forward(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto& self = *active;
        if (self.fired && window == self.fields.field && (message == EM_SETSEL || message == EM_SCROLLCARET)) ++self.subsequent;
        const auto result = CallWindowProcA(self.fields.original, window, message, wp, lp);
        const UINT trigger = self.action == 5 ? WM_KILLFOCUS : self.action == 6 ? EM_SETSEL : WM_SETTEXT;
        if (!self.fired && window == self.fields.field && message == trigger) {
            self.fired = true;
            if (self.action == 0) DestroyWindow(window);
            if (self.action == 1) SetParent(window, self.fields.host.alternate);
            if (self.action == 2) SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(self.fields.original));
            if (self.action == 3) Focus(window);
            if (self.action == 4) Focus(self.fields.host.alternate);
            if (self.action == 5 || self.action == 6) Focus(window);
            if (self.action == 7) g_Block = nullptr;
            if (self.action == 8) g_Block = self.replacement.get();
            if (IsWindow(window)) {
                // Seed a new context's caret using the native proc directly.
                CallWindowProcA(self.fields.original, window, EM_SETSEL, 31, 31);
                CallWindowProcA(self.fields.original, window, EM_SCROLLCARET, 0, 0);
            }
        }
        return result;
    }
    Reentry(Fields& f, int which) : fields(f), action(which) {
        if (action == 8) {
            replacement = std::make_unique<MMDApp>();
            ::new(static_cast<void*>(&replacement->state)) MMDAppState{};
            replacement->OriginalEditProc() = fields.original;
            replacement->FrameStepPlayback() = 1;
        }
        active = this; fields.host.app->OriginalEditProc() = Forward;
    }
    ~Reentry() {
        g_Block = fields.host.app.get();
        fields.host.app->OriginalEditProc() = fields.original; active = nullptr;
    }
};
thread_local Reentry* Reentry::active = nullptr;
void ReentryChecks() {
    for (int action = 0; action < 9; ++action) {
        Host host; Fields fields(host, 544, 175); Focus(host.owner);
        if (action == 5) {
            Focus(fields.field); SetWindowTextA(fields.field, longNumber); Tail(fields.field);
            SendMessageA(fields.field, EM_SETMODIFY, FALSE, 0);
        }
        {
            Reentry reentry(fields, action);
            if (action == 5) SetFocus(host.owner);
            else SetWindowTextA(fields.field, longNumber);
            if (action == 7 || action == 8) {
                Check(IsWindow(fields.field) && g_Block == (action == 7 ? nullptr : reentry.replacement.get()),
                    "native proc changes the live app identity while preserving the target HWND");
                // Restore before any further message reaches the real subclass.
                // The outer text update has already returned with the stale app.
                g_Block = host.app.get();
            }
            if (action == 6 && build_profile::original) {
                Check(!reentry.fired && reentry.subsequent == 0,
                    "original profile never issues the new prefix selection message");
            } else {
                const bool allowedPrefix = action == 4 && !build_profile::original;
                Check(reentry.fired && reentry.subsequent == (allowedPrefix ? 2u : 0u),
                    "invalidated or focused target receives no stale prefix messages; unrelated focus permits prefix");
            }
            if (action == 0) Check(!IsWindow(fields.field), "destroyed target remains destroyed after original proc returns");
            else if (action == 4 && !build_profile::original) {
                Check(GetFocus() == host.alternate, "unrelated owned focus remains intact");
                Prefix(fields.field, longNumber, false);
            }
            else if (action == 6 && build_profile::original) {
                Check(GetFocus() == host.owner && Read(fields.field).text == longNumber,
                    "original profile leaves focus and text intact without invoking new presentation callbacks");
            }
            else {
                const auto view = Read(fields.field);
                Check(view.text == longNumber && view.begin == 31 && view.end == 31,
                    "reentrant new-context caret and text survive outer old-context postprocessing");
                if (action == 3 || action == 5 || action == 6)
                    Check(GetFocus() == fields.field,
                        "original proc or prefix-selection callback really reacquires target focus");
            }
        }
        if (IsWindow(fields.field) && action == 2)
            SetWindowLongPtrA(fields.field, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc));
        host.Stable();
    }
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t desktop[128]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        !_wcsicmp(desktop, L"Default"), "prefix tests require inherited Default desktop and never switch the desktop");
    // Only USER32 EDIT and STATIC classes are used here; they need no
    // common-control initialization. ICC_STANDARD_CLASSES requires a v6
    // activation context which this standalone test does not request.
    // Each fixture checks actual creation through the production factory.
    for (int percent : {100, 175, 200}) for (bool floating : {false, true}) {
        for (int id : targets) {
            std::printf("RUN prefix id=%d layout=%d floating=%d\n", id, percent, int(floating));
            Host host; Fields fields(host, id, percent, floating); FocusedChecks(fields); UnfocusedChecks(fields, true);
        }
        for (int id : exclusions) {
            Host host; Fields fields(host, id, percent, floating); FocusedChecks(fields); UnfocusedChecks(fields, false);
        }
    }
    ReentryChecks();
    std::printf("PASS transform edit prefix checks=%u profile=%s (font/width simulation only)\n", checks,
        build_profile::original ? "original" : "enhanced");
    return 0;
}
