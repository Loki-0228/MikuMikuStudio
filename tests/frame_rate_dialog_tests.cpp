// MAINTENANCE: regression-keep; the "Correct by frame rate..." floating window
// (include/mikumikustudio/frame_rate_correction.hpp,
//  src/window/frame_rate_dialog.cpp).
//
// This test owns the backend, because the window module is compiled on its own
// here: the stub below defines Prepare/Apply/Available/RequestSaveAs (and the
// opaque Plan) and records what the window asked for.  Build it WITHOUT the real
// backend, or the linker sees two definitions.  Defining
// MIKUMIKUSTUDIO_FRAME_RATE_REAL_BACKEND skips the stub and the behavioural
// checks, leaving the pure UI checks (window, languages, layout, menu) that do
// not depend on what Prepare decides.
//
// Covered with hidden windows only, no GPU, no desktop capture, no window shown
// on screen:
//   * the Edit menu item: added exactly once at the end of submenu index 1,
//     re-texted for the three languages and enabled/disabled from Available,
//   * the window: created hidden, never activated, one instance per app,
//     destroyed by Close,
//   * the trimmed layout: one short scope line at the top, one short hint line
//     at the bottom saying what Apply does and that same-frame keys merge (the
//     conversion formula is deliberately not printed), every label measured with
//     the font the control actually uses and fitting with padding, every control
//     inside the client area, and real ink through WM_PRINT (the images are
//     evidence),
//   * the initial state: Apply and Copy disabled until there is something to act
//     on, and showing the window prepares and applies nothing,
//   * Preview: parses decimals (29.97), refuses junk without calling the
//     backend, shows the summary only and never calls RequestSaveAs,
//   * a parameter change drops the prepared plan, the report and the Copy button,
//   * a failed Prepare or Apply shows the backend's message in the status line,
//     opens no modal window and never calls RequestSaveAs,
//   * a successful Apply: the detailed per-key report (rounded keys and merged
//     keys, including the merge range, a key that is reported as both, and a
//     channel longer than 2000 characters kept whole), a closing note that
//     leaves the save state to the Save As result instead of assuming it, Apply
//     disarmed so a plan cannot be applied twice, and RequestSaveAs called
//     exactly once, after the report is already on screen,
//   * the complete report: the UI trims nothing, the report area's text limit is
//     lifted, the last change and the trailing note are present, and Copy report
//     copies exactly that complete text.  Only the backend's own budget may omit
//     entries, and its count is printed instead of silently dropping details,
//   * Copy report: the report text reaches the clipboard (the original clipboard
//     text is restored afterwards).
//
// WM_PRINT limits, as everywhere in this port: edit and combo controls can print
// incompletely while hidden, so the layout checks use control rectangles and
// text metrics and the image only has to prove that the window painted.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/mmd_app.hpp"

namespace frc = mikumikustudio::frame_rate_correction;
using mikumikustudio::MMDApp;

// ---------------------------------------------------------------------------
// Stub backend.
// ---------------------------------------------------------------------------
#if !defined(MIKUMIKUSTUDIO_FRAME_RATE_REAL_BACKEND)
namespace mikumikustudio {
namespace frame_rate_correction {

class Plan {
public:
    Rates rates;
    bool applied = false;
};

namespace {
int g_prepareCalls = 0;
int g_applyCalls = 0;
int g_availableCalls = 0;
int g_saveAsCalls = 0;
bool g_available = true;
bool g_prepareFails = false;
bool g_applyFails = false;
std::wstring g_prepareError = L"stub refused the preview";
std::wstring g_applyError = L"stub refused the apply";
Report g_report;
Rates g_lastRates;
// Set by the test to observe the window state at the moment Save As is asked
// for, which is how the report-before-save ordering is checked.
void (*g_onSaveAs)() = nullptr;
}  // namespace

bool Prepare(MMDApp&, const Rates& rates, PlanPtr& plan, Report& report,
             std::wstring& error) {
    ++g_prepareCalls;
    g_lastRates = rates;
    if (g_prepareFails) {
        error = g_prepareError;
        return false;
    }
    auto created = std::make_shared<Plan>();
    created->rates = rates;
    plan = created;
    report = g_report;
    error.clear();
    return true;
}

bool Apply(MMDApp&, const PlanPtr& plan, std::wstring& error) {
    ++g_applyCalls;
    if (plan == nullptr) {
        error = L"stub received no plan";
        return false;
    }
    if (g_applyFails) {
        error = g_applyError;
        return false;
    }
    plan->applied = true;
    error.clear();
    return true;
}

void RequestSaveAs(MMDApp&) {
    ++g_saveAsCalls;
    if (g_onSaveAs != nullptr) g_onSaveAs();
}

bool Available(MMDApp*) {
    ++g_availableCalls;
    return g_available;
}

}  // namespace frame_rate_correction
}  // namespace mikumikustudio
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::wstring g_log;

void Note(const std::wstring& line) {
    g_log += line;
    g_log += L"\r\n";
}

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
        g_log += L"FAIL ";
        for (const char* p = what; *p != '\0'; ++p)
            g_log += static_cast<wchar_t>(*p);
        g_log += L"\r\n";
    }
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
std::wstring Text(HWND control) {
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return std::wstring();
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int got = GetWindowTextW(control, text.data(), length + 1);
    text.resize(got > 0 ? static_cast<std::size_t>(got) : 0u);
    return text;
}

std::vector<HWND> Children(HWND parent) {
    std::vector<HWND> out;
    for (HWND child = GetWindow(parent, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT))
        out.push_back(child);
    return out;
}

std::vector<HWND> ChildrenOfClass(HWND parent, const wchar_t* expected) {
    std::vector<HWND> out;
    for (HWND child : Children(parent)) {
        wchar_t name[64]{};
        GetClassNameW(child, name, 64);
        // The predefined window classes are "Edit", "Static" and "Button"; the
        // comparison is case-insensitive so the caller may spell them either way.
        if (_wcsicmp(name, expected) == 0) out.push_back(child);
    }
    return out;
}

HWND FindByText(HWND parent, const wchar_t* expected) {
    for (HWND child : Children(parent))
        if (Text(child) == expected) return child;
    return nullptr;
}

HWND FindContaining(HWND parent, const wchar_t* needle) {
    const std::wstring probe = needle;
    for (HWND child : Children(parent))
        if (Text(child).find(probe) != std::wstring::npos) return child;
    return nullptr;
}

bool AnyTextContains(HWND window, const std::wstring& needle) {
    if (needle.empty()) return false;
    if (Text(window).find(needle) != std::wstring::npos) return true;
    for (HWND child : Children(window))
        if (Text(child).find(needle) != std::wstring::npos) return true;
    return false;
}

std::wstring AllText(HWND window) {
    std::wstring all = Text(window);
    for (HWND child : Children(window)) {
        all += L"\n";
        all += Text(child);
    }
    return all;
}

int CountOccurrences(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) return 0;
    int count = 0;
    for (std::size_t at = text.find(needle); at != std::wstring::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

RECT InClient(HWND parent, HWND child) {
    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

RECT ClientRect(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    return rect;
}

int MeasureWidth(HWND control, const std::wstring& text) {
    HDC dc = GetDC(control);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(control, dc);
    return size.cx;
}

int MeasureHeight(HWND control) {
    HDC dc = GetDC(control);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(control, dc);
    return metrics.tmHeight;
}

int WrappedHeight(HWND control, const std::wstring& text, int width) {
    HDC dc = GetDC(control);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    RECT rect{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &rect,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(control, dc);
    return rect.bottom - rect.top;
}

// A modeless top-level window created by the module is the only window owned by
// the test's owner window that carries controls; the system's own "Default IME"
// window is owned by the host as well but has no children, so it is skipped.
struct OwnerSearch {
    HWND owner;
    HWND found;
};

BOOL CALLBACK OwnerProc(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<OwnerSearch*>(parameter);
    if (GetWindow(window, GW_OWNER) != search->owner) return TRUE;
    if (GetWindow(window, GW_CHILD) == nullptr) return TRUE;
    search->found = window;
    return FALSE;
}

HWND FindOwnedWindow(HWND owner) {
    OwnerSearch search{owner, nullptr};
    EnumWindows(OwnerProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Evidence for the log: every top-level window owned by `owner`.
struct OwnedDump {
    HWND owner;
};

BOOL CALLBACK OwnedDumpProc(HWND window, LPARAM parameter) {
    auto* dump = reinterpret_cast<OwnedDump*>(parameter);
    if (GetWindow(window, GW_OWNER) != dump->owner) return TRUE;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    wchar_t title[128]{};
    GetWindowTextW(window, title, 128);
    std::wstring line = L"owned window class=\"";
    line += name;
    line += L"\" title=\"";
    line += title;
    line += L"\" children=";
    int children = 0;
    for (HWND child = GetWindow(window, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT))
        ++children;
    line += std::to_wstring(children);
    Note(line);
    return TRUE;
}

void LogOwnedWindows(HWND owner, const wchar_t* when) {
    Note(std::wstring(L"owned windows ") + when + L":");
    OwnedDump dump{owner};
    EnumWindows(OwnedDumpProc, reinterpret_cast<LPARAM>(&dump));
}

int CountMenuItems(HMENU menu, unsigned id) {
    int count = 0;
    const int items = GetMenuItemCount(menu);
    for (int index = 0; index < items; ++index)
        if (GetMenuItemID(menu, index) == static_cast<UINT>(id)) ++count;
    return count;
}

void ClickButton(HWND dialog, HWND button) {
    SendMessageW(button, BM_CLICK, 0, 0);
}

void SetTextAndNotify(HWND dialog, HWND edit, const wchar_t* text) {
    SetWindowTextW(edit, text);
    // WM_SETTEXT on an edit does not have to raise EN_CHANGE, so the test sends
    // the notification the window documents.
    SendMessageW(dialog, WM_COMMAND,
                 MAKEWPARAM(static_cast<int>(GetWindowLongPtrW(edit, GWLP_ID)),
                            EN_CHANGE),
                 reinterpret_cast<LPARAM>(edit));
}

// ---------------------------------------------------------------------------
// WM_PRINT evidence.
// ---------------------------------------------------------------------------
void EnsureDirectory(const std::wstring& path) {
    std::wstring prefix;
    for (const wchar_t character : path) {
        prefix += character;
        if (character == L'\\' || character == L'/')
            CreateDirectoryW(prefix.c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

bool SaveBitmap(HDC dc, HBITMAP bitmap, int width, int height,
                const std::wstring& path) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = -height;  // top-down
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height));
    if (GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(),
                  reinterpret_cast<BITMAPINFO*>(&header),
                  DIB_RGB_COLORS) == 0)
        return false;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path.c_str(), L"wb") != 0 || stream == nullptr)
        return false;
    std::fwrite(&file, sizeof(file), 1, stream);
    std::fwrite(&header, sizeof(header), 1, stream);
    std::fwrite(pixels.data(), pixels.size(), 1, stream);
    std::fclose(stream);
    return true;
}

// Prints the window into a bitmap, counts how much of it is not one flat
// background colour, and saves the image as evidence.
// Prints the window into a bitmap and saves it as evidence.  Hidden edit
// controls do not always draw through the parent's WM_PRINT, so the controls
// named in `printClientChildren` are asked to draw themselves with
// WM_PRINTCLIENT into the same DC, at their own position: this is the real
// control painting, never a substitute drawing.
double PrintWindowInk(HWND window, const std::wstring& path, double& darkShare,
                      const std::vector<HWND>& printClientChildren) {
    const RECT client = ClientRect(window);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    darkShare = 0.0;
    if (width <= 0 || height <= 0) return 0.0;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                 PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    for (HWND child : printClientChildren) {
        if (child == nullptr) continue;
        const RECT rect = InClient(window, child);
        POINT old{};
        SetViewportOrgEx(dc, rect.left, rect.top, &old);
        // WM_PRINT is routed by the control into its own WM_PRINTCLIENT for the
        // client area and adds its frame, so this is the control painting
        // itself; nothing here draws a stand-in.  A hidden edit control may
        // still decline to paint, which is why the style and the margins are
        // also checked directly below.
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_ERASEBKGND);
        SetViewportOrgEx(dc, old.x, old.y, nullptr);
    }
    Check(SaveBitmap(dc, bitmap, width, height, path),
          "the printed client area is written as evidence");
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = -height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height));
    double ink = 0.0;
    if (GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(),
                  reinterpret_cast<BITMAPINFO*>(&header),
                  DIB_RGB_COLORS) != 0) {
        std::vector<unsigned int> histogram;
        std::vector<unsigned int> colours;
        unsigned long long dark = 0;
        const std::size_t count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t index = 0; index < count; ++index) {
            const unsigned char* pixel = pixels.data() + index * 4;
            const unsigned int colour =
                static_cast<unsigned int>(pixel[0]) |
                (static_cast<unsigned int>(pixel[1]) << 8) |
                (static_cast<unsigned int>(pixel[2]) << 16);
            const int luminance =
                (pixel[0] * 30 + pixel[1] * 59 + pixel[2] * 11) / 100;
            if (luminance < 128) ++dark;
            bool known = false;
            for (std::size_t slot = 0; slot < colours.size(); ++slot) {
                if (colours[slot] == colour) {
                    ++histogram[slot];
                    known = true;
                    break;
                }
            }
            if (!known) {
                colours.push_back(colour);
                histogram.push_back(1);
            }
        }
        unsigned int top = 0;
        for (const unsigned int value : histogram) top = std::max(top, value);
        ink = 1.0 - static_cast<double>(top) / static_cast<double>(count);
        darkShare = static_cast<double>(dark) / static_cast<double>(count);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ink;
}

// ---------------------------------------------------------------------------
// One language pass: texts, layout and the printed image.
// ---------------------------------------------------------------------------
struct Language {
    unsigned code;
    const wchar_t* originalLabel;
    const wchar_t* currentLabel;
    const wchar_t* previewLabel;
    const wchar_t* applyLabel;
    const wchar_t* copyLabel;
    const wchar_t* closeLabel;
    const wchar_t* menuNeedle;
    const wchar_t* topNeedle;         // scope line: whole project + frame 0
    const wchar_t* hintApplyNeedle;   // hint line: Save As opens after applying
    const wchar_t* hintMergeNeedle;   // hint line: same-frame keys merge
    const wchar_t* title;
};

const Language kLanguages[3] = {
    {0, L"元のFPS", L"現在のFPS", L"プレビュー", L"適用", L"レポートをコピー",
     L"閉じる", L"フレームレート", L"プロジェクト全体",
     L"自動で「名前を付けて保存」", L"同じフレームのキーは統合",
     L"フレームレートで補正"},
    {1, L"Original FPS", L"Current FPS", L"Preview", L"Apply", L"Copy report",
     L"Close", L"Correct by frame rate", L"Converts the whole project",
     L"Save As opens automatically", L"keys on the same frame merge",
     L"Correct by frame rate"},
    {2, L"原帧率", L"现帧率", L"预览", L"应用", L"复制报告", L"关闭",
     L"根据帧率修正", L"换算整个工程", L"应用后自动另存为",
     L"同帧关键帧会合并", L"根据帧率修正"},
};

HWND EditBesideLabel(HWND dialog, const wchar_t* labelText) {
    HWND label = FindByText(dialog, labelText);
    if (label == nullptr) return nullptr;
    const RECT labelRect = InClient(dialog, label);
    HWND best = nullptr;
    int bestDelta = 100000;
    for (HWND edit : ChildrenOfClass(dialog, L"EDIT")) {
        const RECT rect = InClient(dialog, edit);
        if (rect.left < labelRect.right) continue;
        const int labelCentre = (labelRect.top + labelRect.bottom) / 2;
        const int centre = (rect.top + rect.bottom) / 2;
        const int delta = std::abs(centre - labelCentre);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = edit;
        }
    }
    return bestDelta <= 8 ? best : nullptr;
}

HWND MultilineEdit(HWND dialog) {
    for (HWND edit : ChildrenOfClass(dialog, L"EDIT"))
        if ((GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE) != 0) return edit;
    return nullptr;
}

HWND StatusEdit(HWND dialog, HWND original, HWND current) {
    for (HWND edit : ChildrenOfClass(dialog, L"EDIT")) {
        if (edit == original || edit == current) continue;
        if ((GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE) != 0) continue;
        return edit;
    }
    return nullptr;
}

// The top and hint lines are wrapped statics: they must not be clipped.
void CheckStaticFits(HWND control, const char* what) {
    if (control == nullptr) {
        Check(false, what);
        return;
    }
    const RECT rect = InClient(GetParent(control), control);
    const int needed =
        WrappedHeight(control, Text(control), rect.right - rect.left);
    Check(needed <= rect.bottom - rect.top + 2, what);
}

void CheckLayout(HWND dialog, const Language& language,
                 const std::wstring& outputDirectory) {
    const RECT client = ClientRect(dialog);
    const wchar_t* texts[4] = {language.originalLabel, language.currentLabel,
                               language.previewLabel, language.copyLabel};
    for (const wchar_t* text : texts) {
        HWND control = FindByText(dialog, text);
        Check(control != nullptr, "the language's control text is present");
        if (control == nullptr) continue;
        const RECT rect = InClient(dialog, control);
        const int width = rect.right - rect.left;
        const int measured = MeasureWidth(control, text);
        Check(measured + 4 <= width,
              "a label fits inside its control with padding");
        if (measured + 4 > width) {
            std::wstring line = L"clipped: \"";
            line += text;
            line += L"\" measured ";
            line += std::to_wstring(measured);
            line += L" in ";
            line += std::to_wstring(width);
            Note(line);
        }
    }
    // The two short lines are visible, carry the required wording and fit.
    HWND top = FindContaining(dialog, language.topNeedle);
    Check(top != nullptr, "the scope line states the whole project and frame 0");
    CheckStaticFits(top, "the scope line is not clipped in this language");
    HWND hint = FindContaining(dialog, language.hintMergeNeedle);
    Check(hint != nullptr, "the hint line states that same-frame keys merge");
    CheckStaticFits(hint, "the hint line is not clipped in this language");
    Check(hint != nullptr &&
              Text(hint).find(language.hintApplyNeedle) != std::wstring::npos,
          "the hint line says Save As opens after applying");
    // The hints stay short on purpose: the conversion formula is not printed
    // anywhere in the window.
    Check(hint != nullptr &&
              Text(hint).find(L"new frame = original frame") == std::wstring::npos &&
              Text(hint).find(L"新しいフレーム = 元のフレーム") == std::wstring::npos &&
              Text(hint).find(L"新帧 = 原帧") == std::wstring::npos,
          "the hint line does not spell out the conversion formula");
    // Every control keeps the padding margin inside the client area.
    for (HWND child : Children(dialog)) {
        const RECT rect = InClient(dialog, child);
        Check(rect.left >= 4 && rect.top >= 4 && rect.right <= client.right - 4 &&
                  rect.bottom <= client.bottom - 4,
              "a control sits inside the client area with the padding margin");
    }
    // The two rate fields are the only editable controls, and they must look
    // like boxes: a client edge, 4 px of text margin on both sides, and enough
    // room that the widest number the dialog formats never touches the frame.
    HWND originalField = EditBesideLabel(dialog, language.originalLabel);
    HWND currentField = EditBesideLabel(dialog, language.currentLabel);
    Check(originalField != nullptr && currentField != nullptr,
          "both rate fields are found for the style checks");
    if (originalField != nullptr && currentField != nullptr) {
        for (HWND field : {originalField, currentField}) {
            Check((GetWindowLongPtrW(field, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0,
                  "a rate field carries the native client edge");
            const LRESULT margins = SendMessageW(field, EM_GETMARGINS, 0, 0);
            Check(LOWORD(margins) == 4 && HIWORD(margins) == 4,
                  "a rate field keeps 4 px of text margin on both sides");
            if (LOWORD(margins) != 4 || HIWORD(margins) != 4) {
                std::wstring line = L"field margins: left ";
                line += std::to_wstring(LOWORD(margins));
                line += L", right ";
                line += std::to_wstring(HIWORD(margins));
                Note(line);
            }
            const RECT rect = InClient(dialog, field);
            const int boxWidth = rect.right - rect.left;
            const int boxHeight = rect.bottom - rect.top;
            // 2 px of edge + 4 px of margin on each side, plus 4 px of slack.
            Check(MeasureWidth(field, L"1000") + 2 * (2 + 4) + 4 <= boxWidth,
                  "the rate field is wide enough for its text inside the frame");
            Check(MeasureHeight(field) + 2 * 2 + 2 <= boxHeight,
                  "the rate field is tall enough for its text inside the frame");
        }
    }
    // Only those two: the read-only areas are not editable boxes.
    HWND reportArea = MultilineEdit(dialog);
    Check(reportArea != nullptr &&
              (GetWindowLongPtrW(reportArea, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0,
          "the report area is not given an editable box edge");
    // No language may claim the conversion is lossless.
    const std::wstring all = AllText(dialog);
    Check(all.find(L"lossless") == std::wstring::npos &&
              all.find(L"Lossless") == std::wstring::npos &&
              all.find(L"無損失") == std::wstring::npos &&
              all.find(L"无损") == std::wstring::npos,
          "nothing claims the conversion is lossless");
    // The report is empty before anything is prepared, and it must not claim
    // anything was saved either.
    double dark = 0.0;
    const std::wstring image = outputDirectory + L"\\frame-rate-ui-" +
                               std::to_wstring(language.code) + L".bmp";
    const double ink = PrintWindowInk(dialog, image, dark,
                                      {originalField, currentField});
    Check(ink > 0.0005, "the window prints real content, not one flat colour");
    std::wstring line = L"language ";
    line += std::to_wstring(language.code);
    line += L": ink ";
    line += std::to_wstring(ink);
    line += L", dark ";
    line += std::to_wstring(dark);
    line += L", image ";
    line += image;
    Note(line);
}

#if !defined(MIKUMIKUSTUDIO_FRAME_RATE_REAL_BACKEND)
// ---------------------------------------------------------------------------
// Behaviour that only the stub can drive.
// ---------------------------------------------------------------------------
HWND g_hookReport = nullptr;
std::wstring g_reportAtSaveAs;

void HookOnSaveAs() { g_reportAtSaveAs = Text(g_hookReport); }

// Reads the current clipboard text, empty when it holds no text.
std::wstring ClipboardText(bool& readable) {
    readable = false;
    if (!OpenClipboard(nullptr)) return std::wstring();
    std::wstring text;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT); handle != nullptr) {
        if (const auto* data =
                static_cast<const wchar_t*>(GlobalLock(handle));
            data != nullptr) {
            text = data;
            GlobalUnlock(handle);
        }
        readable = true;
    } else {
        readable = true;  // readable, just not text
    }
    CloseClipboard();
    return text;
}

bool PutClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            if (void* target = GlobalLock(memory); target != nullptr) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                if (!ok) GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
    return ok;
}

void CheckBackendBehaviour(HWND dialog, HWND owner, MMDApp& app) {
    // What the stub reports.  The detail list is as long as the counts claim,
    // with one ASCII and one non-ASCII channel name.
    frc::g_report.inputKeys = 100;
    frc::g_report.outputKeys = 90;
    frc::g_report.mergeGroups = 3;
    frc::g_report.mergedKeys = 12;
    frc::g_report.roundedKeys = 4;
    frc::g_report.models = 1;
    frc::g_report.accessories = 2;
    frc::g_report.mmvTracks = 3;
    frc::g_report.meshCaches = 4;
    frc::g_report.maxSampledValueError = 0.00123;
    frc::g_report.worstChannel = L"camera/x";
    frc::g_report.detailsTruncated = false;
    frc::g_report.omittedDetailCount = 0;
    frc::g_report.changes.clear();
    // One channel far longer than any fixed formatting buffer: a long model name
    // plus a tab name plus an FX path.  The report has to carry it whole.
    std::wstring longChannel = L"model0/";
    while (longChannel.size() < 2100)
        longChannel += L"超長い名前のモデル/ボーン/センター・右腕\t";
    longChannel += L"FX/effect/very/long/path.fxsub";
    Check(longChannel.size() > 2000,
          "the long channel fixture exceeds 2000 characters");
    for (int index = 0; index < 4; ++index) {
        frc::FrameChange change;
        change.channel = index == 0   ? L"camera/x"
                         : index == 3 ? longChannel
                                      : L"model0/髪";
        change.originalFrame = 15 + index;
        change.newFrame = 14 + index;
        change.exactFrame = 14.25 + index * 0.5;
        change.kind = frc::FrameChange::ChangeKind::Rounded;
        frc::g_report.changes.push_back(change);
    }
    for (int index = 0; index < 12; ++index) {
        frc::FrameChange change;
        if (index == 0) {
            // The very same input key as the first Rounded entry: it needed
            // rounding and it lost its slot, and the two kinds are independent,
            // so a complete report holds one entry of each for this key.
            change.channel = L"camera/x";
            change.originalFrame = 15;
            change.newFrame = 14;
            change.exactFrame = 14.25;
            change.mergeFirstFrame = 14;
            change.mergeLastFrame = 15;
        } else {
            change.channel = L"model0/センター";
            change.originalFrame = 20 + index;
            change.newFrame = 14 + index;
            change.exactFrame = 20.0 + index;
            change.mergeFirstFrame = 14 + index;
            change.mergeLastFrame = 15 + index;
        }
        change.kind = frc::FrameChange::ChangeKind::Merged;
        frc::g_report.changes.push_back(change);
    }

    HWND preview = FindByText(dialog, L"Preview");
    HWND apply = FindByText(dialog, L"Apply");
    HWND copy = FindByText(dialog, L"Copy report");
    HWND close = FindByText(dialog, L"Close");
    HWND original = EditBesideLabel(dialog, L"Original FPS");
    HWND current = EditBesideLabel(dialog, L"Current FPS");
    HWND report = MultilineEdit(dialog);
    HWND status = StatusEdit(dialog, original, current);
    Check(preview != nullptr && apply != nullptr && copy != nullptr &&
              close != nullptr,
          "the four buttons are found by their captions");
    Check(original != nullptr && current != nullptr,
          "both frame-rate fields are found beside their labels");
    Check(report != nullptr && status != nullptr,
          "the result area and the status line are found");
    if (preview == nullptr || apply == nullptr || copy == nullptr ||
        close == nullptr || original == nullptr || current == nullptr ||
        report == nullptr || status == nullptr)
        return;

    // The initial state: nothing has been prepared, so nothing can run.
    Check(!IsWindowEnabled(apply), "Apply starts disabled");
    Check(!IsWindowEnabled(copy), "Copy starts disabled without a report");
    Check(IsWindowEnabled(preview), "Preview starts enabled while available");
    Check(Text(original) == L"30" && Text(current) == L"30",
          "both fields start at the Rates defaults");
    Check(frc::g_prepareCalls == 0 && frc::g_applyCalls == 0 &&
              frc::g_saveAsCalls == 0,
          "showing the window prepares, applies and saves nothing");
    Check(frc::g_availableCalls > 0,
          "the window asks the backend whether it may act");

    // Junk never reaches the backend.
    SetTextAndNotify(dialog, original, L"abc");
    ClickButton(dialog, preview);
    Check(frc::g_prepareCalls == 0,
          "a non-numeric rate is refused before the backend is called");
    Check(!Text(status).empty(), "a refused rate explains itself");
    SetTextAndNotify(dialog, original, L"30");

    // Preview: decimals survive, the summary appears, Apply is armed and the
    // save flow is not touched.
    SetTextAndNotify(dialog, original, L"29.97");
    Check(!IsWindowEnabled(apply), "changing a value leaves Apply disabled");
    ClickButton(dialog, preview);
    Check(frc::g_prepareCalls == 1, "Preview prepares exactly once");
    Check(std::fabs(frc::g_lastRates.originalFPS - 29.97) < 1e-9 &&
              std::fabs(frc::g_lastRates.currentFPS - 30.0) < 1e-9,
          "the fractional original rate reaches the backend unchanged");
    Check(frc::g_applyCalls == 0, "Preview never applies");
    Check(frc::g_saveAsCalls == 0, "Preview never asks for Save As");
    Check(IsWindowEnabled(apply), "a successful Prepare arms Apply");
    Check(IsWindowEnabled(copy), "a summary can be copied");

    const std::wstring summary = Text(report);
    Check(summary.find(L"29.97") != std::wstring::npos,
          "the report states the source and current frame rate");
    Check(summary.find(L"Input keys") != std::wstring::npos &&
              summary.find(L"100") != std::wstring::npos &&
              summary.find(L"Output keys") != std::wstring::npos &&
              summary.find(L"90") != std::wstring::npos,
          "the summary shows the key counts before and after");
    Check(summary.find(L"Merge groups") != std::wstring::npos &&
              summary.find(L"Merged keys") != std::wstring::npos &&
              summary.find(L"12") != std::wstring::npos,
          "the summary shows the merge groups and the merged keys");
    Check(summary.find(L"Rounded keys") != std::wstring::npos &&
              summary.find(L"4") != std::wstring::npos,
          "the summary shows the rounded keys");
    Check(summary.find(L"Max sampled value error") != std::wstring::npos &&
              summary.find(L"0.00123") != std::wstring::npos,
          "the summary shows the sampled value error");
    Check(summary.find(L"camera/x") != std::wstring::npos,
          "the summary names the channel with the largest error");
    Check(summary.find(L"Not applied yet") != std::wstring::npos,
          "Preview says the conversion has not been applied yet");
    Check(summary.find(L"Change details:") == std::wstring::npos,
          "Preview shows the summary only, not the per-key list");
    Check(summary.find(L"Save As result") == std::wstring::npos,
          "Preview says nothing about a save that has not happened yet");

    // A parameter change drops the plan, the report and the Copy button.
    SetTextAndNotify(dialog, current, L"29.97");
    Check(!IsWindowEnabled(apply), "a parameter change disarms Apply");
    Check(!IsWindowEnabled(copy), "a parameter change disarms Copy");
    Check(Text(report).empty(), "a parameter change clears the result area");
    SetTextAndNotify(dialog, current, L"30");

    // A failed Prepare leaves nothing to apply, shows the message and pops no
    // window (a modal box would be owned by the dialog and carry controls).
    frc::g_prepareFails = true;
    ClickButton(dialog, preview);
    Check(frc::g_applyCalls == 0, "a failed Prepare never applies");
    Check(frc::g_saveAsCalls == 0, "a failed Prepare never asks for Save As");
    Check(!IsWindowEnabled(apply), "a failed Prepare leaves Apply disabled");
    Check(!IsWindowEnabled(copy), "a failed Prepare leaves Copy disabled");
    Check(AnyTextContains(dialog, frc::g_prepareError),
          "a failed Prepare shows the backend's message");
    Check(FindOwnedWindow(dialog) == nullptr,
          "a failed Prepare opens no modal window");
    frc::g_prepareFails = false;

    // A failed Apply keeps the plan so the user can retry, reports why, and
    // still never asks for Save As.
    ClickButton(dialog, preview);
    Check(IsWindowEnabled(apply), "a new Prepare arms Apply again");
    frc::g_applyFails = true;
    ClickButton(dialog, apply);
    Check(frc::g_applyCalls == 1, "Apply calls the backend once");
    Check(frc::g_saveAsCalls == 0, "a failed Apply never asks for Save As");
    Check(AnyTextContains(dialog, frc::g_applyError),
          "a failed Apply shows the backend's message");
    Check(IsWindowEnabled(apply), "a failed Apply keeps the plan for a retry");
    Check(FindOwnedWindow(dialog) == nullptr,
          "a failed Apply opens no modal window");
    // The detail text is built before Apply but installed only after it
    // succeeds, so a failed Apply still shows the preview summary.
    Check(Text(report).find(L"Change details:") == std::wstring::npos &&
              Text(report).find(L"Not applied yet") != std::wstring::npos,
          "a failed Apply leaves the preview report in place");

    // A successful Apply: details, the not-saved note, Apply disarmed, and one
    // Save As request made after the report is already on screen.
    g_hookReport = report;
    g_reportAtSaveAs.clear();
    frc::g_onSaveAs = HookOnSaveAs;
    frc::g_applyFails = false;
    ClickButton(dialog, apply);
    Check(frc::g_applyCalls == 2, "the retry reaches the backend");
    Check(frc::g_saveAsCalls == 1,
          "a successful Apply asks for Save As exactly once");
    Check(!IsWindowEnabled(apply), "a successful Apply disarms Apply");

    const std::wstring detail = Text(report);
    Check(detail.find(L"Change details:") != std::wstring::npos,
          "a successful Apply shows the per-key detail");
    Check(detail.find(L"camera/x") != std::wstring::npos &&
              detail.find(L"14.25") != std::wstring::npos,
          "the detail lists a rounded frame with its exact value");
    Check(detail.find(L"model0/センター") != std::wstring::npos &&
              detail.find(L"Merged") != std::wstring::npos,
          "the detail lists a merged key with its channel");
    Check(detail.find(L"Rounded") != std::wstring::npos,
          "the detail names the rounding kind");
    Check(detail.find(L"group 14") != std::wstring::npos,
          "the merged row carries the source group range");
    Check(detail.find(L"camera/x · Rounded") != std::wstring::npos &&
              detail.find(L"camera/x · Merged") != std::wstring::npos,
          "one key is reported as both rounded and merged");
    // Nothing is trimmed on the way to the screen: the last change and the
    // trailing note are there, and the control can hold a long report.
    Check(detail.find(L"model0/センター · Merged · key frame 31") !=
              std::wstring::npos,
          "the report lists every change, with no UI truncation");
    Check(SendMessageW(report, EM_GETLIMITTEXT, 0, 0) >= 1000000,
          "the report area accepts a long report");
    Check(detail.find(longChannel) != std::wstring::npos,
          "a channel longer than 2000 characters is kept whole in the report");
    Check(detail.find(longChannel + L" · Rounded · frame 18 → 17 (exact 15.75)") !=
              std::wstring::npos,
          "the row after a long channel still carries its frames");
    Check(detail.find(L"Save As result") != std::wstring::npos,
          "the report leaves the save state to the Save As result");
    Check(detail.find(L"not saved") == std::wstring::npos &&
              detail.find(L"尚未保存") == std::wstring::npos,
          "the report does not assume the result is still unsaved");
    // Nothing may read as "the project was saved": the Save As flow has only
    // just been asked for, and the user is free to cancel it.
    Check(detail.find(L"was saved") == std::wstring::npos &&
              detail.find(L"has been saved") == std::wstring::npos &&
              detail.find(L"已保存") == std::wstring::npos &&
              detail.find(L"保存しました") == std::wstring::npos,
          "the report never claims the project was saved");
    Check(Text(status).find(L"saved") == std::wstring::npos &&
              Text(status).find(L"Saved") == std::wstring::npos,
          "the status line never claims the project was saved");
    Check(g_reportAtSaveAs.find(L"Change details:") != std::wstring::npos,
          "the detail report is already on screen when Save As is requested");
    Check(Text(status).find(L"cancelling Save As does not undo") !=
              std::wstring::npos,
          "the status line says a cancelled Save As keeps the correction");
    Check(Text(status).find(L"Use Save As to write") == std::wstring::npos &&
              Text(status).find(L"请用另存为写盘") == std::wstring::npos,
          "the status line does not send the user to save again");
    // The exact wording, for the native and MMV subtasks that fill the report.
    Note(L"report after a successful Apply:");
    Note(detail);
    frc::g_onSaveAs = nullptr;

    // Applying again is impossible while no plan is armed, so Save As cannot be
    // requested twice for one conversion.
    ClickButton(dialog, apply);
    Check(frc::g_applyCalls == 2 && frc::g_saveAsCalls == 1,
          "a disarmed Apply cannot trigger a second Save As");

    // The two truncation notices are printed, never silent.
    frc::g_report.detailsTruncated = true;
    frc::g_report.omittedDetailCount = 7;
    ClickButton(dialog, preview);
    ClickButton(dialog, apply);
    const std::wstring truncated = Text(report);
    Check(truncated.find(L"omitted 7 detail entries") != std::wstring::npos,
          "the backend's omitted detail count is printed");
    Check(frc::g_saveAsCalls == 2, "the second conversion asks for Save As too");
    frc::g_report.detailsTruncated = false;
    frc::g_report.omittedDetailCount = 0;

    // Copy report reaches the clipboard; the original text is put back.
    bool clipboardReadable = false;
    const std::wstring savedClipboard = ClipboardText(clipboardReadable);
    const std::wstring reportText = Text(report);
    ClickButton(dialog, copy);
    bool verifyReadable = false;
    const std::wstring copiedClipboard = ClipboardText(verifyReadable);
    if (clipboardReadable && verifyReadable) {
        Check(copiedClipboard == reportText,
              "Copy report puts the whole report on the clipboard");
        Check(copiedClipboard.find(L"Save As result") != std::wstring::npos,
              "the copied report includes the end of the list");
        Check(copiedClipboard.find(longChannel) != std::wstring::npos,
              "the copied report keeps the long channel whole");
    } else {
        Note(L"clipboard unavailable; the Copy report content check was skipped");
    }
    if (clipboardReadable) Check(PutClipboardText(savedClipboard),
                                 "the original clipboard text is restored");

    // A genuinely large report: the text is built before Apply, so the whole
    // list reaches the area and the Save As request still happens.
    {
        const int kLargeRows = 20000;
        frc::g_report.changes.clear();
        frc::g_report.roundedKeys = 0;
        frc::g_report.mergedKeys = static_cast<std::uint64_t>(kLargeRows);
        frc::g_report.mergeGroups = 1;
        frc::g_report.changes.resize(static_cast<std::size_t>(kLargeRows));
        for (int index = 0; index < kLargeRows; ++index) {
            frc::FrameChange& change = frc::g_report.changes[index];
            change.channel = L"model0/センター";
            change.originalFrame = 100 + index;
            change.newFrame = 50 + index;
            change.exactFrame = 100.0 + index;
            change.kind = frc::FrameChange::ChangeKind::Merged;
            change.mergeFirstFrame = 50 + index;
            change.mergeLastFrame = 51 + index;
        }
        ClickButton(dialog, preview);
        ClickButton(dialog, apply);
        const std::wstring large = Text(report);
        Check(frc::g_saveAsCalls == 3,
              "a large report still reaches the Save As request");
        Check(large.size() > 500000,
              "the large report is rendered in full");
        Check(CountOccurrences(large, L" · Merged · ") == kLargeRows,
              "no row of the large report is dropped");
        Check(large.find(L"Save As result") != std::wstring::npos,
              "the large report keeps its closing note");
        std::wstring line = L"large report: ";
        line += std::to_wstring(large.size());
        line += L" characters, ";
        line += std::to_wstring(kLargeRows);
        line += L" rows";
        Note(line);
    }

    // The availability gate reaches both the menu and the window.
    frc::g_available = false;
    frc::RefreshMenu(&app);
    HMENU edit = GetSubMenu(GetMenu(owner), 1);
    Check((GetMenuState(edit, frc::kMenuCommand, MF_BYCOMMAND) & MF_GRAYED) != 0,
          "the menu command is greyed while the backend is unavailable");
    Check(!IsWindowEnabled(preview), "Preview is disabled while unavailable");
    frc::g_available = true;
    frc::RefreshMenu(&app);
    Check((GetMenuState(edit, frc::kMenuCommand, MF_BYCOMMAND) & MF_GRAYED) == 0,
          "the menu command is enabled again when available");
    Check(IsWindowEnabled(preview), "Preview is enabled again");

    // Cancel only closes: it must not apply or save anything.
    const int applyCalls = frc::g_applyCalls;
    const int saveAsCalls = frc::g_saveAsCalls;
    SendMessageW(dialog, WM_CLOSE, 0, 0);
    Check(FindOwnedWindow(owner) == nullptr, "Close destroys the window");
    Check(frc::g_applyCalls == applyCalls && frc::g_saveAsCalls == saveAsCalls,
          "Close neither applies nor asks for Save As");
}
#endif  // stub backend

}  // namespace

int main() {
    const std::wstring outputDirectory = L".local\\artifacts\\frame-rate-ui";
    EnsureDirectory(outputDirectory);

    // A hidden owner window with File and Edit menus, exactly the shape
    // RefreshMenu expects (GetSubMenu(mainMenu, 1) is Edit).
    HWND owner = CreateWindowExW(0, L"STATIC", L"frame rate owner",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 900, 600, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "a hidden owner window exists");
    if (owner == nullptr) return 1;
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit");
    SetMenu(owner, bar);

    auto app = std::make_unique<MMDApp>();
    std::memset(static_cast<void*>(&app->state), 0, sizeof(app->state));
    app->state.hwnd = owner;
    app->state.englishUI = 1;

    // The command is added to the end of Edit exactly once, whatever the
    // language and however often the host refreshes.
    for (int pass = 0; pass < 3; ++pass) frc::RefreshMenu(app.get());
    Check(CountMenuItems(edit, frc::kMenuCommand) == 1,
          "the command is appended exactly once");
    const int editCount = GetMenuItemCount(edit);
    Check(GetMenuItemID(edit, editCount - 1) == frc::kMenuCommand,
          "the command is the last item of the Edit menu");
    for (const Language& language : kLanguages) {
        app->state.englishUI = static_cast<unsigned char>(language.code);
        frc::RefreshMenu(app.get());
        wchar_t buffer[128]{};
        GetMenuStringW(edit, frc::kMenuCommand, buffer, 128, MF_BYCOMMAND);
        Check(std::wstring(buffer).find(language.menuNeedle) != std::wstring::npos,
              "the menu text follows the UI language");
    }
    Check(CountMenuItems(edit, frc::kMenuCommand) == 1,
          "refreshing in every language still leaves one item");

    // HandleCommand routes only its own id, and a foreign id must not open a
    // window.  The command's own id is deliberately not dispatched here: it
    // ends in ShowDialog(app) with visible=true, and a background test must not
    // put a window on the desktop.  kMenuCommand is pinned instead, and the real
    // host run covers the menu click itself.
    Check(frc::kMenuCommand == 32920u, "the command id is the frozen one");
    Check(!frc::HandleCommand(app.get(), frc::kMenuCommand + 1),
          "a foreign command id is not handled");
    Check(!frc::HandleCommand(app.get(), 0),
          "command id 0 is not handled either");
    Check(FindOwnedWindow(owner) == nullptr,
          "a foreign command id opens no window");
    LogOwnedWindows(owner, L"before the dialog exists");
    app->state.englishUI = 1;

    // The window itself: visible=false only creates it, so it must stay hidden
    // and must not be activated.
    frc::ShowDialog(app.get(), false);
    HWND dialog = FindOwnedWindow(owner);
    Check(dialog != nullptr, "the dialog window exists");
    if (dialog == nullptr) return 1;
    Check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_VISIBLE) == 0,
          "visible=false leaves the window hidden");
    Check(GetForegroundWindow() != dialog, "creating it does not activate it");
    const HWND first = dialog;
    frc::ShowDialog(app.get(), false);
    Check(FindOwnedWindow(owner) == first,
          "a second call reuses the same window instead of adding one");

    for (const Language& language : kLanguages) {
        app->state.englishUI = static_cast<unsigned char>(language.code);
        frc::RefreshMenu(app.get());
        Check(Text(dialog) == language.title,
              "the window title follows the UI language");
        Check(FindByText(dialog, language.originalLabel) != nullptr &&
                  FindByText(dialog, language.currentLabel) != nullptr,
              "the two frame-rate labels follow the UI language");
        Check(FindByText(dialog, language.previewLabel) != nullptr &&
                  FindByText(dialog, language.applyLabel) != nullptr &&
                  FindByText(dialog, language.copyLabel) != nullptr &&
                  FindByText(dialog, language.closeLabel) != nullptr,
              "the four buttons follow the UI language");
        CheckLayout(dialog, language, outputDirectory);
    }
    app->state.englishUI = 1;
    frc::RefreshMenu(app.get());
    HWND apply = FindByText(dialog, L"Apply");
    Check(apply != nullptr && !IsWindowEnabled(apply),
          "Apply is disabled before any successful Prepare");

#if !defined(MIKUMIKUSTUDIO_FRAME_RATE_REAL_BACKEND)
    CheckBackendBehaviour(dialog, owner, *app);
    // The destroyed window is forgotten: the next request builds a new one.
    frc::ShowDialog(app.get(), false);
    HWND fresh = FindOwnedWindow(owner);
    Check(fresh != nullptr,
          "the module builds a new window after the old one closed");
    if (fresh != nullptr) DestroyWindow(fresh);
#endif

    DestroyWindow(owner);

    const std::wstring reportPath =
        outputDirectory + L"\\frame-rate-ui-report.txt";
    FILE* stream = nullptr;
    if (_wfopen_s(&stream, reportPath.c_str(), L"wb") == 0 && stream != nullptr) {
        std::fwrite(g_log.data(), sizeof(wchar_t), g_log.size(), stream);
        std::fclose(stream);
    }
    std::printf("frame_rate_dialog_tests: %d checks, %d failures\n", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
