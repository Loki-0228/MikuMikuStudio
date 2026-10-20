// Include after ManifestText/Write. Uses the separately built lifecycle DLL:
// no direct queue client or forged refresh service stands in for Manager.
#include <mms/refresh.h>

namespace manager_refresh_checks {
static void Pump(unsigned milliseconds) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        for (unsigned i = 0; i < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
            if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        Sleep(1);
    } while (GetTickCount64() < until);
}
struct Probe {
    MmsResult (MMS_CALL *state)(MmsUiRefreshStateV1*) = nullptr;
    MmsResult (MMS_CALL *release)() = nullptr;
    MmsResult (MMS_CALL *setState)(uint32_t, uint32_t, uint32_t) = nullptr;
    explicit Probe(HMODULE module) {
        CHECK(module != nullptr);
        state = reinterpret_cast<decltype(state)>(GetProcAddress(module, "RefreshProbeState"));
        release = reinterpret_cast<decltype(release)>(GetProcAddress(module, "RefreshProbeRelease"));
        setState = reinterpret_cast<decltype(setState)>(GetProcAddress(module, "RefreshProbeSetState"));
        CHECK(state && release && setState);
    }
    MmsUiRefreshStateV1 Read() const {
        MmsUiRefreshStateV1 value{}; value.h = Header<MmsUiRefreshStateV1>(); CHECK(state(&value) == MMS_OK); return value;
    }
    void Complete(uint32_t kinds = MMS_UI_REFRESH_REPAINT) const {
        const auto until = GetTickCount64() + 2000;
        auto value = Read();
        while (value.status == MMS_UI_REFRESH_PENDING && GetTickCount64() < until) { Pump(5); value = Read(); }
        CHECK(value.status == MMS_UI_REFRESH_COMPLETED && value.result == MMS_OK && value.completedKinds == kinds);
    }
    void ReleaseWorker() const {
        MmsResult result = MMS_E_FAILED; std::thread worker([&] { result = release(); }); worker.join(); CHECK(result == MMS_OK);
    }
};
struct Window {
    HWND value = nullptr;
    Window() {
        value = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Manager refresh contract", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 640, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(value != nullptr); CHECK(SetMenu(value, CreateMenu())); ShowWindow(value, SW_SHOWNOACTIVATE);
    }
    ~Window() { if (value) { auto menu = GetMenu(value); SetMenu(value, nullptr); if (menu) DestroyMenu(menu); DestroyWindow(value); } }
};
static void Run(const fs::path& root, const fs::path& lifecycleDll, bool stopPending, bool reserved = false) {
    const auto directory = root / (reserved ? "refresh-manager-reserved" : stopPending ? "refresh-manager-pending" : "refresh-manager-completed");
    const auto package = directory / "PLUGINS" / "fixture";
    const std::string id = reserved ? "org.example.gui-refresh-reserved" : "org.example.gui-refresh";
    fs::create_directories(package); fs::copy_file(lifecycleDll, package / "refresh-lifecycle.dll");
    Write(package / "plugin.json", ManifestText(id, "refresh-lifecycle.dll"));
    std::vector<std::string> logs; bool allow = false; unsigned guardCalls = 0;
    const auto log = [&](const std::string& value) { logs.push_back(value); };
    const auto count = [&](const char* value) { return std::count_if(logs.begin(), logs.end(), [&](const auto& s) { return s.find(value) != std::string::npos; }); };
    Manager manager(directory / "user", log); manager.SetRefreshGuard([&] { ++guardCalls; return allow; });
    manager.Load(package.parent_path());
    const auto statuses = manager.Status();
    if (statuses.size() != 1 || statuses[0].stage != "started") {
        for (const auto& status : statuses)
            std::cerr << "REFRESH_MANAGER_START id=" << status.id << " stage=" << status.stage << " error=" << status.error << '\n';
        for (const auto& entry : logs) std::cerr << "REFRESH_MANAGER_LOG " << entry << '\n';
    }
    CHECK(statuses.size() == 1 && statuses[0].stage == "started");
    CHECK(count("REFRESH_SERVICE_PASS") == 1);
    const auto module = GetModuleHandleW(L"refresh-lifecycle.dll"); Probe probe(module);
    Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log);
    native->Refresh(); CHECK(native->Layout(10, 500, 140, true) > 0);
    HWND button = nullptr; unsigned pluginButtons = 0;
    for (auto child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[32]{}; GetClassNameW(child, name, 32);
        // NativeUi also creates its own "more" button (ID 0). This fixture
        // owns exactly one registered toolbar command in the dispatch range.
        const int controlId = GetDlgCtrlID(child);
        if (!std::wcscmp(name, L"Button") && controlId >= 0xa000 && controlId < 0xc000) {
            button = child; ++pluginButtons;
        }
    }
    CHECK(button != nullptr && pluginButtons == 1 && IsWindowVisible(button));
    const auto click = [&](unsigned keyboard = 0) {
        const auto before = count("REFRESH_WORKER_PENDING");
        if (keyboard == 1) {
            SetFocus(button); MSG message{}; message.hwnd = button; message.message = WM_KEYDOWN; message.wParam = VK_RETURN;
            CHECK(GetFocus() == button && native->DialogMessage(&message));
        } else if (keyboard == 2) {
            SetFocus(button); CHECK(GetFocus() == button);
            SendMessageW(button, WM_KEYDOWN, VK_SPACE, 1);
            SendMessageW(button, WM_KEYUP, VK_SPACE, LPARAM(1) | (LPARAM(3) << 30));
        } else SendMessageW(native->Toolbar(), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
        if (count("REFRESH_TARGET_IN_COMMAND") != before + 1 || count("REFRESH_WORKER_PENDING") != before + 1) {
            std::cerr << "REFRESH_MANAGER_CLICK keyboard=" << keyboard << " id=" << GetDlgCtrlID(button)
                << " visible=" << IsWindowVisible(button) << " enabled=" << IsWindowEnabled(button)
                << " focus=" << (GetFocus() == button) << " before=" << before << '\n';
            for (const auto& entry : logs) std::cerr << "REFRESH_MANAGER_LOG " << entry << '\n';
        }
        CHECK(count("REFRESH_TARGET_IN_COMMAND") == before + 1 && count("REFRESH_WORKER_PENDING") == before + 1);
        CHECK(count("REFRESH_WORKER_FAILED") == 0 && probe.Read().status == MMS_UI_REFRESH_PENDING);
    };
    click(true);
    // NativeUi::Command has already performed its ordinary Refresh/Layout.
    // Unchanged geometry must not invalidate the callback's captured rectangle.
    Pump(40); CHECK(guardCalls != 0 && probe.Read().status == MMS_UI_REFRESH_PENDING);
    allow = true; probe.Complete(); probe.ReleaseWorker();
    const auto revision = manager.Ui().Revision();
    CHECK(manager.Storage().Prepare({}) == MMS_OK && manager.Storage().Prepared());
    click(2); Pump(40); CHECK(probe.Read().status == MMS_UI_REFRESH_PENDING);
    manager.Storage().Cancel(); probe.Complete(); const auto geometry = probe.Read().geometryRevision; probe.ReleaseWorker();
    CHECK(manager.Ui().Revision() == revision); // Repaint never re-registers UI.
    // No command, Refresh or Layout between changing the provider's result and
    // the pure refresh message: only STATE can update these native properties.
    allow = false; CHECK(probe.setState(0, 1, 1) == MMS_OK);
    Pump(40); CHECK(probe.Read().status == MMS_UI_REFRESH_PENDING && IsWindowEnabled(button));
    allow = true; probe.Complete(MMS_UI_REFRESH_STATE);
    CHECK(!IsWindowEnabled(button) && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED);
    CHECK(manager.Ui().Revision() == revision && probe.Read().geometryRevision == geometry && !manager.CanInvokeEdit()); probe.ReleaseWorker();
    CHECK(probe.setState(1, 0, 1) == MMS_OK); probe.Complete(MMS_UI_REFRESH_STATE); probe.ReleaseWorker();
    CHECK(IsWindowEnabled(button) && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    if (reserved) {
        RECT before{}, after{}; CHECK(GetWindowRect(button, &before));
        const auto preferences = manager.Ui().PreferencesRevision();
        CHECK(probe.setState(1, 1, 0) == MMS_OK); probe.Complete(MMS_UI_REFRESH_STATE);
        CHECK(!IsWindowVisible(button)); probe.ReleaseWorker();
        CHECK(probe.setState(1, 0, 1) == MMS_OK); probe.Complete(MMS_UI_REFRESH_STATE);
        CHECK(IsWindowVisible(button) && GetWindowRect(button, &after) && EqualRect(&before, &after)); probe.ReleaseWorker();
        CHECK(manager.Ui().Revision() == revision && manager.Ui().PreferencesRevision() == preferences);
    }
    allow = !stopPending; click();
    if (!stopPending) probe.Complete();
    CHECK(!manager.Stop());
    CHECK(GetModuleHandleW(L"refresh-lifecycle.dll") == module && manager.Status()[0].stage == "stopping");
    CHECK(count("STOP_REFRESH_TICKET_RETAINED") == 1 && count("DESTROY") == 0);
    auto stopped = probe.Read();
    CHECK(stopped.status == (stopPending ? MMS_UI_REFRESH_CANCELLED : MMS_UI_REFRESH_COMPLETED));
    CHECK(stopped.result == (stopPending ? MMS_E_CANCELLED : MMS_OK));
    probe.ReleaseWorker();
    CHECK(manager.Stop() && manager.Status()[0].stage == "stopped" && count("DESTROY") == 1);
    CHECK(GetModuleHandleW(L"refresh-lifecycle.dll") == nullptr);
    CHECK(manager.Ui().commands.empty() && manager.Ui().elements.size() == 4);
    native.reset(); CHECK(manager.Stop());
}
} // namespace manager_refresh_checks

static void ManagerRefreshChecks(const fs::path& root, const fs::path& lifecycleDll) {
    manager_refresh_checks::Run(root, lifecycleDll, false);
    manager_refresh_checks::Run(root, lifecycleDll, true);
    manager_refresh_checks::Run(root, lifecycleDll, false, true);
}
