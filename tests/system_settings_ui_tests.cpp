// Real modal settings UI, isolated files and hidden windows; no renderer or history runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/system_settings_ui.hpp"
#include "mikumikustudio/background_picture_source.hpp"
#include "../src/app/settings_json.hpp"
#include "../src/plugins/history_runtime.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t MiB = 1024ull * 1024;
unsigned checks = 0;
fs::path settingsPath;
HWND owner = nullptr;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", text); std::exit(1); }
}
std::string Bytes() {
    std::ifstream input(settingsPath, std::ios::binary);
    Check(input.good(), "read isolated settings bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
plugins::Json Section(const char* name) {
    std::string value;
    Check(system_settings::ReadSection(name, value) == system_settings::Result::Ok, "read persisted settings section");
    return settings_json::Object(value);
}
void Unknowns() {
    Check(Section("history").At("futureHistory").At("keep").UInt() == 17 &&
          Section("resources").At("futureResource").At("keep").UInt() == 23 &&
          Section("settingsUiSentinel").At("keep").UInt() == 31,
          "settings save preserves unknown members and unrelated sections");
}
void Preferences(bool record, unsigned steps, unsigned historyMiB, unsigned sourceMiB) {
    const auto history = Section("history");
    Check(settings_json::Bool(history, "recordAdjustments", !record) == record &&
          history.At("steps").UInt() == steps && history.At("memoryMiB").UInt() == historyMiB &&
          Section("resources").At("backgroundPictureSourcesMiB").UInt() == sourceMiB,
          "real Save persists history and picture-source preferences together");
    Unknowns();
}
LRESULT CALLBACK Hidden(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_WINDOWPOSCHANGING) {
        auto* position = reinterpret_cast<WINDOWPOS*>(lp);
        position->flags &= ~SWP_SHOWWINDOW;
        position->flags |= SWP_NOACTIVATE;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Hidden, id);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK Guard(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->dwExStyle |= WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
            create->x = create->y = -32000;
            Check(SetWindowSubclass(reinterpret_cast<HWND>(wp), Hidden, 1, 0) != FALSE,
                  "guard only this thread's modal windows against visibility");
        }
    }
    if (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
struct Scenario {
    bool record, save;
    unsigned steps, historyMiB, sourceMiB;
    const wchar_t* invalid = nullptr;
    bool blockedWrite = false;
};
struct Driver {
    Scenario next;
    HWND dialog = nullptr, error = nullptr;
    unsigned ticks = 0, found = 0, errors = 0;
    unsigned ordinal = 0;
    const char* stage = "waiting-dialog";
    bool diagnosedMissing = false;
    bool busy = false, acted = false, expectError = false;
    bool initialRecord;
    unsigned initialSteps, initialHistory, initialSource;
    std::string before;
    media::PictureSourceBudgetUsage usage;
    static Driver* active;
    explicit Driver(Scenario scenario) : next(scenario), before(Bytes()), usage(media::GlobalPictureSourceBudget().Usage()) {
        const auto current = system_settings_ui::HistoryPreference();
        initialRecord = current.recordAdjustments;
        initialSteps = current.steps; initialHistory = current.memoryMiB;
        initialSource = Section("resources").At("backgroundPictureSourcesMiB").UInt();
    }
    static BOOL CALLBACK Describe(HWND window, LPARAM value) {
        const auto& self = *reinterpret_cast<Driver*>(value);
        wchar_t cls[80]{}; GetClassNameW(window, cls, 80);
        std::printf("UI case=%u enumerate hwnd=%p owner=%p expectedOwner=%p class=%ls enabled=%d visible=%d id101=%p id106=%p idOK=%p\n",
            self.ordinal, window, GetWindow(window, GW_OWNER), owner, cls, IsWindowEnabled(window), IsWindowVisible(window),
            GetDlgItem(window, 101), GetDlgItem(window, 106), GetDlgItem(window, IDOK));
        return TRUE;
    }
    static BOOL CALLBACK Find(HWND window, LPARAM value) {
        auto& self = *reinterpret_cast<Driver*>(value);
        wchar_t cls[80]{}; GetClassNameW(window, cls, 80);
        if (GetWindow(window, GW_OWNER) == owner && GetDlgItem(window, 101) && GetDlgItem(window, 106)) {
            if (!self.dialog) {
                self.dialog = window; ++self.found;
                std::printf("UI case=%u found dialog tick=%u hwnd=%p owner=%p enabled=%d visible=%d id101=%p id106=%p\n",
                    self.ordinal, self.ticks, window, GetWindow(window, GW_OWNER), IsWindowEnabled(window), IsWindowVisible(window),
                    GetDlgItem(window, 101), GetDlgItem(window, 106));
            }
        } else if (self.dialog && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId() &&
                   GetWindow(window, GW_OWNER) == self.dialog && !std::wcscmp(cls, L"#32770")) {
            Check(self.expectError, "only an expected invalid-input message box may open");
            Check(!IsWindowVisible(window), "validation message stays hidden");
            if (self.error != window) {
                self.error = window; ++self.errors;
                std::printf("UI case=%u found error tick=%u hwnd=%p owner=%p busy=%d stage=%s posting WM_CLOSE\n",
                    self.ordinal, self.ticks, window, GetWindow(window, GW_OWNER), int(self.busy), self.stage);
                Check(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE,
                      "close only the expected owned MB_OK dialog without relying on its hidden button creation");
            }
        }
        return TRUE;
    }
    static void CALLBACK Poll(HWND, UINT, UINT_PTR, DWORD) {
        auto& self = *active;
        ++self.ticks;
        if (self.ticks == 500) {
            std::printf("UI case=%u TIMEOUT stage=%s busy=%d acted=%d expectedError=%d found=%u errors=%u dialog=%p valid=%d enabled=%d visible=%d\n",
                self.ordinal, self.stage, int(self.busy), int(self.acted), int(self.expectError), self.found, self.errors,
                self.dialog, IsWindow(self.dialog), IsWindowEnabled(self.dialog), IsWindowVisible(self.dialog));
            EnumThreadWindows(GetCurrentThreadId(), Describe, reinterpret_cast<LPARAM>(&self));
        }
        Check(self.ticks < 500, "real settings dialog completes within the local ten-second guard");
        EnumThreadWindows(GetCurrentThreadId(), Find, reinterpret_cast<LPARAM>(&self));
        if (!self.dialog && !self.diagnosedMissing) {
            self.diagnosedMissing = true;
            std::printf("UI case=%u first tick without ready dialog, stage=%s\n", self.ordinal, self.stage);
            EnumThreadWindows(GetCurrentThreadId(), Describe, reinterpret_cast<LPARAM>(&self));
        }
        if (self.busy || self.acted || !self.dialog || !IsWindowEnabled(self.dialog)) return;
        self.busy = self.acted = true;
        self.stage = "editing-controls";
        Check(!IsWindowVisible(self.dialog) && !IsWindowVisible(owner), "settings and its owner remain hidden");
        Check(GetDlgItem(self.dialog, 102) && GetDlgItem(self.dialog, 103) && GetDlgItem(self.dialog, 104),
              "existing history controls retain their public dialog identities");
        Check(IsDlgButtonChecked(self.dialog, self.initialRecord ? 101 : 102) == BST_CHECKED &&
              GetDlgItemInt(self.dialog, 103, nullptr, FALSE) == self.initialSteps &&
              GetDlgItemInt(self.dialog, 104, nullptr, FALSE) == self.initialHistory &&
              GetDlgItemInt(self.dialog, 106, nullptr, FALSE) == self.initialSource,
              "dialog initializes both preference groups from actual unified settings");
        CheckRadioButton(self.dialog, 101, 102, self.next.record ? 101 : 102);
        SetDlgItemInt(self.dialog, 103, self.next.steps, FALSE);
        SetDlgItemInt(self.dialog, 104, self.next.historyMiB, FALSE);
        SetDlgItemInt(self.dialog, 106, self.next.sourceMiB, FALSE);
        if (self.next.invalid || self.next.blockedWrite) {
            HANDLE locked = INVALID_HANDLE_VALUE;
            if (self.next.invalid) SetDlgItemTextW(self.dialog, 106, self.next.invalid);
            if (self.next.blockedWrite) {
                locked = CreateFileW(settingsPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                Check(locked != INVALID_HANDLE_VALUE, "exclusively hold only isolated settings during actual Save");
            }
            self.expectError = true;
            self.stage = self.next.blockedWrite ? "save-with-locked-file" : "save-invalid-value";
            std::printf("UI case=%u enter %s\n", self.ordinal, self.stage);
            SendMessageW(self.dialog, WM_COMMAND, IDOK, 0);
            std::printf("UI case=%u returned from rejected Save errors=%u dialogValid=%d enabled=%d\n",
                self.ordinal, self.errors, IsWindow(self.dialog), IsWindowEnabled(self.dialog));
            self.stage = "checking-rejected-save";
            self.expectError = false;
            Check(self.errors == 1 && IsWindow(self.dialog) && IsWindowEnabled(self.dialog),
                  "rejected Save closes its warning but keeps the editable settings dialog open");
            const auto current = media::GlobalPictureSourceBudget().Usage();
            if (self.next.blockedWrite) {
                LARGE_INTEGER size{};
                Check(GetFileSizeEx(locked, &size) && size.QuadPart == static_cast<LONGLONG>(self.before.size()),
                      "failed persistence leaves isolated file length unchanged while still locked");
                std::string observed(self.before.size(), '\0'); DWORD read = 0;
                Check(ReadFile(locked, observed.data(), static_cast<DWORD>(observed.size()), &read, nullptr) &&
                      read == observed.size() && observed == self.before,
                      "failed persistence leaves both settings groups byte-identical before unlocking");
                Check(CloseHandle(locked) != FALSE, "release isolated settings lock so the same dialog can retry Save");
            }
            Check(Bytes() == self.before && current.limit == self.usage.limit && current.used == self.usage.used,
                  "rejected Save changes neither settings group nor retained-source accounting");
            SetDlgItemInt(self.dialog, 106, self.next.sourceMiB, FALSE);
        }
        self.stage = self.next.save ? "final-save" : "cancel";
        std::printf("UI case=%u enter %s\n", self.ordinal, self.stage);
        SendMessageW(self.dialog, WM_COMMAND, self.next.save ? IDOK : IDCANCEL, 0);
        std::printf("UI case=%u returned from %s dialogValid=%d\n", self.ordinal, self.stage, IsWindow(self.dialog));
        self.stage = "waiting-modal-return";
        self.busy = false;
    }
    void Run(MMDApp* app) {
        static unsigned sequence = 0; ordinal = ++sequence;
        std::printf("UI case=%u begin record=%d save=%d steps=%u historyMiB=%u sourceMiB=%u invalid=%ls blockedWrite=%d owner=%p\n",
            ordinal, int(next.record), int(next.save), next.steps, next.historyMiB, next.sourceMiB,
            next.invalid ? next.invalid : L"none", int(next.blockedWrite), owner);
        Check(active == nullptr && GetCapture() == nullptr, "settings driver starts without a nested driver or capture");
        active = this;
        Check(SetTimer(owner, 1, 20, Poll) != 0, "start bounded native modal driver");
        Check(system_settings_ui::HandleCommand(app, system_settings_ui::kCommand), "invoke actual system settings command");
        std::printf("UI case=%u HandleCommand returned stage=%s ticks=%u\n", ordinal, stage, ticks);
        KillTimer(owner, 1); active = nullptr;
        Check(acted && found == 1 && !IsWindow(dialog), "actual modal settings dialog completed exactly once");
        const auto after = media::GlobalPictureSourceBudget().Usage();
        Check(after.used == usage.used, "changing preferences preserves all live picture-source charges");
        if (next.save) {
            Preferences(next.record, next.steps, next.historyMiB, next.sourceMiB);
            Check(after.limit == std::uint64_t(next.sourceMiB) * MiB, "Save immediately applies the configured global source limit");
        } else {
            Check(Bytes() == before && after.limit == usage.limit, "Cancel leaves file bytes and active budget unchanged");
        }
        Check(errors == (next.invalid || next.blockedWrite ? 1u : 0u), "only requested validation or persistence warning was displayed");
    }
};
Driver* Driver::active = nullptr;
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 3 && !std::wcscmp(argv[1], L"--isolated"), "explicit isolated EXE directory is required");
    wchar_t module[32768]{};
    const auto length = GetModuleFileNameW(nullptr, module, 32768);
    Check(length && length < 32768, "resolve actual test executable");
    const auto directory = fs::canonical(fs::path(module).parent_path());
    Check(fs::equivalent(directory, fs::canonical(argv[2])), "test runs inside the explicitly supplied isolated installation");
    settingsPath = system_settings::Path();
    Check(settingsPath == directory / L"data" / L"settings.json" && !fs::exists(settingsPath.parent_path()),
          "refuse any preexisting data before creating isolated settings");
    Check(system_settings::WriteSection("history", "{\"recordAdjustments\":false,\"steps\":32,\"memoryMiB\":64,\"futureHistory\":{\"keep\":17}}") == system_settings::Result::Ok &&
          system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":128,\"futureResource\":{\"keep\":23}}") == system_settings::Result::Ok &&
          system_settings::WriteSection("settingsUiSentinel", "{\"keep\":31}") == system_settings::Result::Ok,
          "seed disposable settings with both groups and unknown fields");
    const auto bitmap = settingsPath.parent_path() / L"settings-ui-source.bmp";
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = info.biHeight = 8; info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = 8 * 8 * 3;
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + info.biSizeImage;
    { std::ofstream output(bitmap, std::ios::binary); const std::array<char, 8 * 8 * 3> pixels{};
      output.write(reinterpret_cast<const char*>(&file), sizeof(file)); output.write(reinterpret_cast<const char*>(&info), sizeof(info));
      output.write(pixels.data(), pixels.size()); output.close(); Check(bool(output), "write only owned tiny BMP source"); }
    auto& budget = media::GlobalPictureSourceBudget(); const auto originalBudget = budget.Usage();
    budget.Configure(128 * MiB);
    media::BackgroundPictureSourcePtr source;
    Check(media::CaptureBackgroundPictureSource(bitmap.c_str(), MiB, budget, source) == media::PictureSourceResult::Ok && source,
          "hold a real source lease across settings UI edits");
    const auto* bytes = source->Data(); const auto retained = source->RetainedBytes();
    Check(budget.Usage().used == originalBudget.used + retained, "source lease is charged to the actual global budget");
    // This isolated EXE has no v6 activation manifest. EDIT/BUTTON are native
    // standard classes; use the same compatible common-control initialization
    // as the independent scene-history UI fixture, without requiring v6.
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&common) != FALSE, "initialize native controls");
    owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, L"STATIC", L"Settings test owner", WS_POPUP,
        -32000, -32000, 600, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner && !IsWindowVisible(owner), "create hidden owner without desktop input changes");
    auto app = std::make_unique<MMDApp>(); std::memset(&app->state, 0, sizeof(app->state)); app->Hwnd() = owner; app->EnglishUI() = 1;
    Check(plugins::NativeHistory(app.get()) == nullptr && app->Renderer() == nullptr,
          "exercise actual no-runtime settings path without a renderer or fabricated history");
    HHOOK hook = SetWindowsHookExW(WH_CBT, Guard, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "install current-thread hidden modal guard");
    for (const auto scenario : {Scenario{true, true, 37, 96, 48}, Scenario{false, false, 9, 32, 256},
                               Scenario{false, true, 7, 32, 16}, Scenario{true, true, 41, 128, 4096},
                               Scenario{false, true, 11, 48, 32, L"15"}, Scenario{true, true, 19, 80, 64, L"4097"},
                               Scenario{false, true, 23, 96, 96, nullptr, true}}) {
        Driver driver(scenario); driver.Run(app.get());
        Check(source->Data() == bytes && source->RetainedBytes() == retained,
              "saving or cancelling preferences never replaces the retained source");
    }
    Check(UnhookWindowsHookEx(hook) != FALSE, "remove owned modal guard");
    Check(DestroyWindow(owner) != FALSE, "destroy hidden owner"); app->Hwnd() = nullptr; owner = nullptr;
    source.reset(); Check(budget.Usage().used == originalBudget.used, "final source release returns its charge"); budget.Configure(originalBudget.limit);
    Unknowns();
    Check(DeleteFileW(bitmap.c_str()) && DeleteFileW(settingsPath.c_str()) &&
          DeleteFileW((settingsPath.wstring() + L".lock").c_str()) && RemoveDirectoryW(settingsPath.parent_path().c_str()),
          "remove only new fixture files and their now-empty data directory");
    std::printf("PASS system settings UI picture budget: %u checks; real hidden dialog, no history runtime\n", checks);
}
