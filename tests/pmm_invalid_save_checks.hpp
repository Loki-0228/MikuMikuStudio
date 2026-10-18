// Included inside the PMM fixture namespace. Real modal save errors are hidden
// and closed after the modal loop starts. No arbitrary dialog is accepted.
struct ExpectedInvalidSaveDialog;
ExpectedInvalidSaveDialog* expectedInvalidSaveDialog = nullptr;
struct ExpectedInvalidSaveDialog {
    HWND owner = nullptr, candidate = nullptr, window = nullptr;
    UINT_PTR timer = 0;
    ULONGLONG deadline = 0;
    DWORD thread = GetCurrentThreadId();
    unsigned language = 0, dialogs = 0, scans = 0, hiddenShows = 0;
    explicit ExpectedInvalidSaveDialog(HWND parent, unsigned lang);
    ~ExpectedInvalidSaveDialog();
    bool Handled() const { return dialogs == 1 && window && !IsWindow(window); }
};
bool IsExpectedInvalidSaveWindow(HWND window) {
    const auto* expected = expectedInvalidSaveDialog;
    if (!expected || GetWindow(window, GW_OWNER) != expected->owner) return false;
    wchar_t cls[32]{}, title[32]{};
    GetClassNameW(window, cls, std::size(cls));
    GetWindowTextW(window, title, std::size(title));
    return std::wcscmp(cls, L"#32770") == 0 && std::wcscmp(title, L"PMM") == 0;
}
bool InvalidSaveDialogActivation(HWND window) {
    if (!expectedInvalidSaveDialog) return false;
    Check(IsExpectedInvalidSaveWindow(window),
        "only the expected owned PMM error dialog has its activation suppressed");
    // Keep closing out of the activation callback. A dispatched timer proves
    // the native modal loop is running before text validation and WM_CLOSE.
    return true;
}
struct InvalidSaveText { const wchar_t* expected; bool matched = false; unsigned buttons = 0, enabledButtons = 0; };
BOOL CALLBACK FindInvalidSaveText(HWND child, LPARAM parameter) {
    auto& probe = *reinterpret_cast<InvalidSaveText*>(parameter);
    wchar_t text[1024]{};
    wchar_t cls[64]{};
    GetWindowTextW(child, text, std::size(text));
    GetClassNameW(child, cls, std::size(cls));
    if (std::wcscmp(text, probe.expected) == 0) probe.matched = true;
    if (_wcsicmp(cls, L"Button") == 0) {
        ++probe.buttons;
        probe.enabledButtons += IsWindowEnabled(child) != FALSE;
    }
    return TRUE;
}
BOOL CALLBACK DescribeInvalidSaveChild(HWND child, LPARAM) {
    wchar_t text[1024]{}, cls[64]{};
    char utf8[4096]{};
    GetWindowTextW(child, text, std::size(text));
    GetClassNameW(child, cls, std::size(cls));
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, nullptr, nullptr);
    std::printf("TRACE invalid-save-child hwnd=%p class=%ls id=%d native_id=%lld thread=%lu enabled=%d visible=%d text=%s\n",
        static_cast<void*>(child), cls, GetDlgCtrlID(child), static_cast<long long>(GetWindowLongPtrW(child, GWLP_ID)),
        GetWindowThreadProcessId(child, nullptr), IsWindowEnabled(child), IsWindowVisible(child), utf8);
    return TRUE;
}
bool InvalidSaveDialogActive(HWND window) {
    auto* expected = expectedInvalidSaveDialog;
    if (!expected) return false;
    Check(IsExpectedInvalidSaveWindow(window),
        "normal invalid save opens only the expected owned PMM error dialog");
    Check(!expected->candidate || expected->candidate == window, "invalid save creates only one owned error dialog");
    expected->candidate = window;
    if (expected->window) {
        Check(expected->window == window && expected->dialogs == 1, "invalid save does not create a second error dialog");
        return true;
    }
    const wchar_t* messages[] = {
        L"キーフレームの参照またはフレーム範囲が無効なため、PMM を保存できません。PMM v3 に切り替えても修復できません。既存のファイルは変更していません。",
        L"PMM cannot be saved because a keyframe reference or frame range is invalid. Switching to PMM v3 cannot repair it. The existing file was not changed.",
        L"关键帧引用或帧范围无效，无法保存 PMM。切换 PMM v3 也无法修复此问题。已有文件未改动。"};
    InvalidSaveText text{messages[expected->language]};
    EnumChildWindows(window, FindInvalidSaveText, reinterpret_cast<LPARAM>(&text));
    // USER32 may pump timer messages while it is still creating child controls.
    // Keep the same deadline, but wait for the full language-specific body.
    if (!text.matched) return false;
    EnumChildWindows(window, DescribeInvalidSaveChild, 0);
    std::printf("TRACE invalid-save-ready language=%u scans=%u native_buttons=%u enabled_buttons=%u IDOK_constant=%d IDOK=%p hidden_shows=%u\n",
        expected->language, expected->scans, text.buttons, text.enabledButtons, IDOK,
        static_cast<void*>(GetDlgItem(window, IDOK)), expected->hiddenShows);
    // Control IDs are diagnostic. The required result is the exact owned
    // error message, followed by actual destruction through native WM_CLOSE.
    expected->window = window;
    ++expected->dialogs;
    ShowWindow(window, SW_HIDE);
    Check(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE,
        "dismiss the actual invalid-save dialog after its modal loop starts");
    return true;
}
BOOL CALLBACK FindInvalidSaveDialog(HWND window, LPARAM) {
    auto* expected = expectedInvalidSaveDialog;
    wchar_t cls[32]{};
    wchar_t title[128]{};
    GetClassNameW(window, cls, std::size(cls));
    GetWindowTextW(window, title, std::size(title));
    const bool dialog = std::wcscmp(cls, L"#32770") == 0;
    if (expected->scans == 1 || (dialog && !expected->candidate))
        std::printf("TRACE invalid-save-window hwnd=%p class=%ls title=%ls owner=%p expected_owner=%p IDOK=%p thread=%lu expected_thread=%lu visible=%d\n",
            static_cast<void*>(window), cls, title, static_cast<void*>(GetWindow(window, GW_OWNER)),
            static_cast<void*>(expected->owner), static_cast<void*>(GetDlgItem(window, IDOK)),
            GetWindowThreadProcessId(window, nullptr), expected->thread, IsWindowVisible(window));
    if (dialog) {
        InvalidSaveDialogActive(window);
    }
    return TRUE;
}
void CALLBACK DismissInvalidSaveDialog(HWND, UINT, UINT_PTR, DWORD) {
    auto* expected = expectedInvalidSaveDialog;
    if (!expected) return;
    Check(GetCurrentThreadId() == expected->thread, "modal dismissal timer stays on its original UI thread");
    ++expected->scans;
    if (GetTickCount64() >= expected->deadline) {
        std::printf("TRACE invalid-save-timeout scans=%u dialogs=%u candidate=%p hwnd=%p hidden_shows=%u thread=%lu\n",
            expected->scans, expected->dialogs, static_cast<void*>(expected->candidate), static_cast<void*>(expected->window),
            expected->hiddenShows, expected->thread);
        if (expected->candidate && !expected->window) EnumChildWindows(expected->candidate, DescribeInvalidSaveChild, 0);
    }
    Check(GetTickCount64() < expected->deadline, "invalid save dialog closes within its five-second deadline");
    EnumThreadWindows(expected->thread, FindInvalidSaveDialog, 0);
}
ExpectedInvalidSaveDialog::ExpectedInvalidSaveDialog(HWND parent, unsigned lang) : owner(parent), language(lang) {
    Check(!expectedInvalidSaveDialog && language < 3, "install one language-specific save-error expectation");
    expectedInvalidSaveDialog = this;
    deadline = GetTickCount64() + 5000;
    timer = SetTimer(nullptr, 0, 20, DismissInvalidSaveDialog);
    Check(timer != 0, "install bounded modal save-error dismissal timer");
}
ExpectedInvalidSaveDialog::~ExpectedInvalidSaveDialog() {
    KillTimer(nullptr, timer);
    expectedInvalidSaveDialog = nullptr;
}

#if !MMD_ORIGINAL_PROFILE
using InvalidSaveCapture = MmsResult(__fastcall*)(plugins::StorageStore*, plugins::StoredRecords&);
InvalidSaveCapture originalInvalidSaveCapture = nullptr;
decltype(&ShowWindow) originalInvalidSaveShowWindow = nullptr;
unsigned invalidSaveCaptureCalls = 0;
MmsResult __fastcall ObserveInvalidSaveCapture(plugins::StorageStore* store, plugins::StoredRecords& records) {
    ++invalidSaveCaptureCalls;
    return originalInvalidSaveCapture(store, records);
}
BOOL WINAPI HideInvalidSaveDialog(HWND window, int command) {
    const bool expected = IsExpectedInvalidSaveWindow(window);
    if (expected && command != SW_HIDE) ++expectedInvalidSaveDialog->hiddenShows;
    return originalInvalidSaveShowWindow(window, expected ? SW_HIDE : command);
}
struct InvalidSaveCaptureProbe {
    void* capture = nullptr;
    InvalidSaveCaptureProbe() {
        const auto status = MH_Initialize();
        Check(status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED, "initialize invalid-save capture observation");
        auto method = &plugins::StorageStore::Capture;
        static_assert(sizeof(method) == sizeof(capture), "single-inheritance serializer capture member address");
        std::memcpy(&capture, &method, sizeof(method));
        Check(MH_CreateHook(capture, &ObserveInvalidSaveCapture, reinterpret_cast<void**>(&originalInvalidSaveCapture)) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), &HideInvalidSaveDialog,
                reinterpret_cast<void**>(&originalInvalidSaveShowWindow)) == MH_OK,
            "observe real serializer capture and hide only expected PMM error dialogs");
        Check(MH_EnableHook(capture) == MH_OK && MH_EnableHook(reinterpret_cast<void*>(&ShowWindow)) == MH_OK,
            "enable invalid-save observations");
        invalidSaveCaptureCalls = 0;
    }
    ~InvalidSaveCaptureProbe() {
        MH_DisableHook(capture); MH_RemoveHook(capture);
        MH_DisableHook(reinterpret_cast<void*>(&ShowWindow)); MH_RemoveHook(reinterpret_cast<void*>(&ShowWindow));
    }
    unsigned Calls() const { return invalidSaveCaptureCalls; }
    void Reset() { invalidSaveCaptureCalls = 0; }
};
#endif
