#include "native_property_input.hpp"
#include <mms/refresh.h>

namespace native_property_input_checks {
void Pump(unsigned milliseconds = 8) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        for (unsigned n = 0; n < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        Sleep(1);
    } while (GetTickCount64() < until);
}
struct Window {
    HWND value = nullptr, sink = nullptr;
    unsigned closes = 0;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_CLOSE && self) { ++self->closes; return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
    Window() {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Proc; wc.lpszClassName = L"MMS.Tests.NativePropertyInput";
        CHECK(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
        value = CreateWindowExW(0, wc.lpszClassName, L"Property INPUT native contract", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            20, 20, 800, 600, nullptr, nullptr, wc.hInstance, this);
        CHECK(value && SetMenu(value, CreateMenu()));
        sink = CreateWindowExW(0, L"BUTTON", L"Focus sink", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 5, 420, 90, 26,
            value, nullptr, wc.hInstance, nullptr); CHECK(sink);
        ShowWindow(value, SW_SHOWNOACTIVATE); SetActiveWindow(value);
    }
    ~Window() {
        if (IsWindow(value)) { auto menu = GetMenu(value); SetMenu(value, nullptr); if (menu) DestroyMenu(menu); DestroyWindow(value); }
    }
};
std::wstring Caption(HWND window) {
    const int count = GetWindowTextLengthW(window); CHECK(count >= 0 && count < 4096);
    std::wstring value(size_t(count) + 1, L'\0'); const auto read = GetWindowTextW(window, value.data(), int(value.size()));
    CHECK(read == count); value.resize(size_t(read)); return value;
}
HWND FindField(HWND root, unsigned id = 0x7201) {
    struct Search { unsigned id; HWND found = nullptr; unsigned count = 0; } search{id};
    EnumChildWindows(root, [](HWND child, LPARAM context) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(context); wchar_t name[32]{}; GetClassNameW(child, name, 32);
        if (GetDlgCtrlID(child) == int(search.id) && !std::wcscmp(name, L"Edit")) { search.found = child; ++search.count; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    CHECK(search.count == 1 && search.found); return search.found;
}
void Text(HWND field, const wchar_t* text) {
    SetFocus(field); CHECK(GetFocus() == field);
    SendMessageW(field, EM_SETSEL, 0, -1); CHECK(SetWindowTextW(field, text)); CHECK(Caption(field) == text);
}
void Enter(HWND field) {
    SendMessageW(field, WM_KEYDOWN, VK_RETURN, 1); SendMessageW(field, WM_CHAR, L'\r', 1);
    SendMessageW(field, WM_KEYUP, VK_RETURN, LPARAM(1) | (LPARAM(3) << 30));
}
void Escape(HWND field) {
    SendMessageW(field, WM_KEYDOWN, VK_ESCAPE, 1); SendMessageW(field, WM_CHAR, VK_ESCAPE, 1);
    SendMessageW(field, WM_KEYUP, VK_ESCAPE, LPARAM(1) | (LPARAM(3) << 30));
}
struct Fixture {
    const std::string id = "org.example.native-input";
    Window window;
    ManagerEditCase edit;
    ManagerEditProbe probe;
    std::unique_ptr<NativeUi> native;
    MmsHandle element = 0;
    HWND field = nullptr, wrapper = nullptr;
    std::vector<std::string> logs;
    Fixture(const fs::path& root, const fs::path& dll, bool host = true, bool readonly = false):
        edit(root, host), probe(edit.Package(dll, id)) {
        edit.Load();
        if (readonly) {
            const auto record = edit.manager->Ui().properties.at(id + ".editable"); auto api = edit.manager->Ui().Api(*record->owner);
            auto desc = record->desc; desc.info.flags = MMS_PROPERTY_READ_ONLY; desc.stage = nullptr;
            CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK);
        }
        element = ui_input_broker_checks::RegisterInput(edit, id);
        const auto record = edit.manager->Ui().properties.at(id + ".editable"); auto layout = edit.manager->Ui().LayoutApi(*record->owner);
        MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>();
        hints.minimum[0] = 90; hints.preferred[0] = 240; hints.preferred[1] = 28; hints.grow[0] = 1;
        CHECK(layout.setDefaultHints(layout.context, mikumikustudio::plugins::Text(id + ".input"), &hints) == MMS_OK);
        native = std::make_unique<NativeUi>(edit.manager->Ui(), window.value, [this](const std::string& value) { logs.push_back(value); });
        Refresh(); field = FindField(native->Toolbar()); wrapper = GetParent(field);
        CHECK(wrapper && GetParent(wrapper) == native->Toolbar() && IsWindowVisible(field) && Caption(field) == L"0");
    }
    ~Fixture() { native.reset(); }
    void Refresh(int width = 500) { native->Refresh(); CHECK(native->Layout(10, width, 180, true) > 0); }
    void Blur() { SetFocus(window.sink); CHECK(GetFocus() == window.sink); Pump(); }
    void Close() { native.reset(); edit.Close(); }
    MmsUiApiV1 OwnerApi() {
        const auto record = edit.manager->Ui().properties.at(id + ".editable"); return edit.manager->Ui().Api(*record->owner);
    }
    void Clean(unsigned steps, int value, uint64_t calls) {
        CHECK(probe.Value() == value && probe.PropertyCalls() == calls && !probe.PropertyCandidates());
        CHECK(!edit.host->attached->HasPending() && edit.host->history.UndoCount() == steps && edit.observations->clean);
    }
};
void Editing(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-editing", dll); const auto revision = f.edit.Revision();
    CHECK(!(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY));
    Text(f.field, L"7"); CHECK(f.probe.Value() == 0 && !f.edit.host->history.UndoCount()); Enter(f.field);
    f.Clean(1, 7, 1); CHECK(Caption(f.field) == L"7" && f.edit.Revision() == revision + 1 && f.edit.observations->published == 1);
    SendMessageW(f.field, WM_CHAR, L'\r', 1); f.Blur(); f.Clean(1, 7, 1); // No duplicate translated character/focus submission.
    CHECK(f.edit.Invoke(f.id, "undo") == MMS_OK); f.Refresh(); CHECK(Caption(f.field) == L"0");
    CHECK(f.edit.Invoke(f.id, "redo") == MMS_OK); f.Refresh(); CHECK(Caption(f.field) == L"7");
    for (const auto* text : {L"-", L"not-an-int", L"101"}) {
        Text(f.field, text); Enter(f.field); f.Clean(1, 7, 1); Escape(f.field); CHECK(Caption(f.field) == L"7");
    }
    Text(f.field, L"9"); Enter(f.field); f.Clean(2, 9, 2);
    Text(f.field, L"09"); Enter(f.field); f.Clean(2, 9, 2); CHECK(Caption(f.field) == L"9");
    const auto stableRevision = f.edit.Revision(); Text(f.field, L"11");
    f.Refresh(310); CHECK(Caption(f.field) == L"11" && f.edit.Revision() == stableRevision); f.Clean(2, 9, 2);
    CHECK(f.edit.Invoke(f.id, "once", stableRevision) == MMS_OK && f.probe.Value() == 10);
    f.Refresh(); CHECK(Caption(f.field) == L"11"); Enter(f.field); f.Clean(3, 10, 2); CHECK(Caption(f.field) == L"10");
    Text(f.field, L"12"); CHECK(f.edit.Invoke(f.id, "undo") == MMS_OK && f.probe.Value() == 9);
    f.Refresh(); CHECK(Caption(f.field) == L"12"); Enter(f.field); f.Clean(2, 9, 2); CHECK(Caption(f.field) == L"9");
    f.Blur(); f.Close();
}
void CanonicalChangeWithoutRevision(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-canonical", dll); const auto revision = f.edit.Revision(); Text(f.field, L"12");
    StoredData data; data.schema = 1; data.bytes.resize(sizeof(int)); const int changed = 5; std::memcpy(data.bytes.data(), &changed, sizeof changed);
    StoredRecords records; records.emplace(f.id, std::move(data)); CHECK(f.edit.manager->Storage().PrepareRestore(std::move(records)) == MMS_OK);
    CHECK(f.edit.manager->Storage().Commit() && f.probe.Value() == changed && f.edit.Revision() == revision);
    f.Refresh(); CHECK(Caption(f.field) == L"12"); Enter(f.field); f.Clean(0, 5, 0); CHECK(Caption(f.field) == L"5"); f.Close();
}
void ImeMessages(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-ime-messages", dll); SetFocus(f.field); CHECK(GetFocus() == f.field);
    SendMessageW(f.field, WM_IME_STARTCOMPOSITION, 0, 0);
    MSG tab{}; tab.hwnd = f.field; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
    CHECK(SendMessageW(f.field, WM_GETDLGCODE, VK_TAB, reinterpret_cast<LPARAM>(&tab)) & DLGC_WANTTAB);
    if (!f.native->DialogMessage(&tab)) { TranslateMessage(&tab); DispatchMessageW(&tab); }
    CHECK(GetFocus() == f.field);
    Text(f.field, L"7");
    SendMessageW(f.field, WM_IME_KEYDOWN, VK_RETURN, 1); SendMessageW(f.field, WM_KEYDOWN, VK_RETURN, 1);
    SendMessageW(f.field, WM_IME_ENDCOMPOSITION, 0, 0); SendMessageW(f.field, WM_CHAR, L'\r', 1);
    SendMessageW(f.field, WM_KEYUP, VK_RETURN, LPARAM(1) | (LPARAM(3) << 30));
    f.Clean(0, 0, 0); CHECK(Caption(f.field) == L"7"); Enter(f.field); f.Clean(1, 7, 1);
    f.Blur(); f.Clean(1, 7, 1); f.Close();
}
void FreshFocus(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-fresh-focus", dll); f.Blur(); const auto revision = f.edit.Revision();
    StoredData data; data.schema = 1; data.bytes.resize(sizeof(int)); const int before = 5;
    std::memcpy(data.bytes.data(), &before, sizeof before); StoredRecords records; records.emplace(f.id, std::move(data));
    CHECK(f.edit.manager->Storage().PrepareRestore(std::move(records)) == MMS_OK && f.edit.manager->Storage().Commit());
    CHECK(f.probe.Value() == before && f.edit.Revision() == revision && Caption(f.field) == L"0");
    Text(f.field, L"12"); Enter(f.field); f.Clean(1, 12, 1);
    CHECK(f.edit.Invoke(f.id, "undo") == MMS_OK && f.probe.Value() == before); f.Refresh(); CHECK(Caption(f.field) == L"5"); f.Close();
}
struct ProviderLifetime {
    MmsPropertyDescV1 original{}; bool stopped = false; unsigned lateCalls = 0;
    inline static thread_local ProviderLifetime* current = nullptr;
    ProviderLifetime* previous = nullptr;
    ProviderLifetime(): previous(current) { current = this; }
    ~ProviderLifetime() { current = previous; }
    static ProviderLifetime& Active(void* context) {
        CHECK(current && current->original.context == context); return *current;
    }
    static MmsResult MMS_CALL Read(void* context, MmsObjectRef target, MmsValue* out, MmsHandle* lease) {
        auto& audit = Active(context);
        if (audit.stopped) { ++audit.lateCalls; return MMS_E_FAILED; }
        return audit.original.read(audit.original.context, target, out, lease);
    }
    static void MMS_CALL Release(void* context, MmsHandle lease) {
        auto& audit = Active(context);
        if (audit.stopped) { ++audit.lateCalls; return; }
        audit.original.releaseValue(audit.original.context, lease);
    }
    static MmsResult MMS_CALL Stage(void* context, MmsHandle tx, MmsObjectRef target, const MmsValue* value) {
        auto& audit = Active(context);
        if (audit.stopped) { ++audit.lateCalls; return MMS_E_FAILED; }
        return audit.original.stage(audit.original.context, tx, target, value);
    }
};
void Retirement(const fs::path& root, const fs::path& dll) {
    for (unsigned kind = 0; kind < 5; ++kind) {
        Fixture f(root / ("native-input-retire-" + std::to_string(kind)), dll); ProviderLifetime audit;
        if (kind == 4) {
            const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable"); audit.original = record->desc; auto desc = record->desc;
            desc.read = ProviderLifetime::Read; desc.releaseValue = ProviderLifetime::Release; desc.stage = ProviderLifetime::Stage;
            auto api = f.OwnerApi(); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK); f.Refresh();
        }
        Text(f.field, L"7"); const auto field = f.field;
        if (kind == 0) { f.native->Layout(10, 500, 180, false); f.Blur(); f.Clean(0, 0, 0); }
        if (kind == 1) { EnableWindow(f.wrapper, FALSE); f.Blur(); f.Clean(0, 0, 0); EnableWindow(f.wrapper, TRUE); }
        if (kind == 2) {
            auto api = f.OwnerApi(); CHECK(api.unregisterItem(api.context, f.element) == MMS_OK);
            f.native->Refresh(); f.Blur(); CHECK(!IsWindow(field)); f.Clean(0, 0, 0);
        }
        if (kind == 3) {
            const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable"); auto desc = record->desc; auto api = f.OwnerApi();
            desc.info.label = S("same handle replacement"); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK);
            f.Refresh(); CHECK(Caption(field) == L"0"); Enter(field); f.Blur(); f.Clean(0, 0, 0);
        }
        if (kind == 4) {
            audit.stopped = true;
            CHECK(f.edit.manager->Stop()); f.Blur(); f.native->Refresh(); CHECK(!IsWindow(field));
            CHECK(!audit.lateCalls && f.probe.PropertyCalls() == 0 && !f.probe.PropertyCandidates() && !f.edit.host->history.UndoCount() && !f.edit.observations->published);
        }
        f.Close();
    }
    for (bool readonly : {false, true}) {
        Fixture f(root / (readonly ? "native-input-readonly" : "native-input-nohost"), dll, readonly, readonly);
        CHECK(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY); SetFocus(f.field); CHECK(GetFocus() == f.field);
        SendMessageW(f.field, EM_SETSEL, 0, -1); SendMessageW(f.field, WM_CHAR, L'7', 1); Enter(f.field); f.Blur();
        CHECK(Caption(f.field) == L"0" && !f.probe.PropertyCalls() && !f.probe.PropertyCandidates() && !f.edit.host->history.UndoCount()); f.Close();
    }
}
void Bounds(HWND wrapper) {
    RECT client{}; CHECK(GetClientRect(wrapper, &client)); unsigned fields = 0, labels = 0;
    for (HWND child = GetWindow(wrapper, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{}; CHECK(GetWindowRect(child, &rect)); MapWindowPoints(nullptr, wrapper, reinterpret_cast<POINT*>(&rect), 2);
        CHECK(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom);
        wchar_t name[32]{}; GetClassNameW(child, name, 32);
        if (!std::wcscmp(name, L"Edit")) { ++fields; CHECK(rect.right > rect.left && rect.bottom > rect.top); }
        if (!std::wcscmp(name, L"Static")) ++labels;
    }
    CHECK(fields == 1 && labels == 1);
}
void LayoutAndRefresh(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-layout", dll); const auto handle = f.element, revision = f.edit.Revision();
    RECT first{}, second{}; CHECK(GetClientRect(f.wrapper, &first)); Bounds(f.wrapper);
    f.Refresh(310); CHECK(GetClientRect(f.wrapper, &second)); Bounds(f.wrapper); CHECK(first.right > second.right);
    CHECK(f.element == handle && f.edit.manager->Ui().elements.at(f.id + ".input").handle == handle && FindField(f.native->Toolbar()) == f.field);
    RefreshClient owner{&f.edit.manager->Ui().Refresh(), f.id}; auto api = f.edit.manager->Ui().Refresh().Api(owner);
    Text(f.field, L"7");
    for (const auto kinds : {MMS_UI_REFRESH_REPAINT, MMS_UI_REFRESH_LAYOUT}) for (const auto scope : {MMS_UI_REFRESH_SELF, MMS_UI_REFRESH_SUBTREE}) {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>(); CHECK(api.getTarget(api.context, handle, &target) == MMS_OK);
        CHECK(target.ready && target.target.element == handle && (target.supportedKinds & kinds) && (target.supportedScopes & scope));
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>(); request.target = target.target; request.kinds = kinds; request.scope = scope;
        MmsHandle ticket = 0; CHECK(api.request(api.context, &request, &ticket) == MMS_OK && ticket);
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>(); const auto until = GetTickCount64() + 2000;
        do { Pump(2); CHECK(api.query(api.context, ticket, &state) == MMS_OK); } while (state.status == MMS_UI_REFRESH_PENDING && GetTickCount64() < until);
        CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.result == MMS_OK && state.completedKinds == kinds); CHECK(api.release(api.context, ticket) == MMS_OK);
        f.Clean(0, 0, 0); CHECK(f.edit.Revision() == revision && !f.edit.observations->published && Caption(f.field) == L"7"); Bounds(f.wrapper);
    }
    Escape(f.field); CHECK(Caption(f.field) == L"0");
    f.Close();
}
struct CloseProbe {
    MmsPropertyDescV1 original{}; Window* window = nullptr; unsigned before = 0, after = 0;
    inline static thread_local CloseProbe* current = nullptr;
    CloseProbe* previous = nullptr;
    CloseProbe(): previous(current) { current = this; }
    ~CloseProbe() { current = previous; }
    static CloseProbe& Active(void* context) {
        CHECK(current && current->original.context == context); return *current;
    }
    static MmsResult MMS_CALL Read(void* context, MmsObjectRef target, MmsValue* value, MmsHandle* lease) {
        auto& probe = Active(context); return probe.original.read(probe.original.context, target, value, lease);
    }
    static void MMS_CALL Release(void* context, MmsHandle lease) {
        auto& probe = Active(context); probe.original.releaseValue(probe.original.context, lease);
    }
    static MmsResult MMS_CALL Stage(void* context, MmsHandle tx, MmsObjectRef target, const MmsValue* value) {
        auto& probe = Active(context); probe.before = probe.window->closes;
        SendMessageW(probe.window->value, WM_CLOSE, 11, 22); SendMessageW(probe.window->value, WM_CLOSE, 33, 44);
        probe.after = probe.window->closes; return probe.original.stage(probe.original.context, tx, target, value);
    }
};
void DeferredClose(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-input-deferred-close", dll); CloseProbe close;
    const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable"); close.original = record->desc; close.window = &f.window;
    auto desc = record->desc; desc.read = CloseProbe::Read; desc.releaseValue = CloseProbe::Release; desc.stage = CloseProbe::Stage;
    auto api = f.OwnerApi(); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK); f.Refresh();
    Text(f.field, L"7"); Enter(f.field); CHECK(close.before == 0 && close.after == 0 && f.window.closes == 0);
    f.Clean(1, 7, 1); Pump(); CHECK(f.window.closes == 1 && IsWindow(f.window.value)); f.Close();
}
struct ReentrantReadProbe {
    MmsPropertyDescV1 original{};
    HWND field = nullptr, wrapper = nullptr;
    unsigned reads = 0, trigger = 0, action = 0, fired = 0;
    inline static thread_local ReentrantReadProbe* active = nullptr;
    ReentrantReadProbe() { CHECK(!active); active = this; }
    ~ReentrantReadProbe() { active = nullptr; }
    void Fire() {
        ++fired; CHECK(fired == 1);
        if (action == 0) Escape(field);
        if (action == 1) ShowWindow(wrapper, SW_HIDE);
        if (action == 2) EnableWindow(wrapper, FALSE);
        if (action == 3) CHECK(SetWindowTextW(field, L"8"));
        if (action == 4) SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
    }
    static MmsResult MMS_CALL Read(void* context, MmsObjectRef target, MmsValue* value, MmsHandle* lease) {
        CHECK(active && active->original.context == context);
        auto& probe = *active;
        const auto result = probe.original.read(context, target, value, lease);
        if (++probe.reads == probe.trigger) probe.Fire();
        return result;
    }
};
void ReentrantInput(const fs::path& root, const fs::path& dll) {
    // 5: after staging a candidate; 6: after reserving its history entry.
    // 0: from the host's Reserve itself, before Store's final guard check.
    for (unsigned phase : {5u, 6u, 0u}) for (unsigned action = 0; action != 5; ++action) {
        Fixture f(root / ("native-input-reentrant-" + std::to_string(phase) + "-" + std::to_string(action)), dll);
        ReentrantReadProbe probe; probe.field = f.field; probe.wrapper = f.wrapper; probe.action = action;
        const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable"); probe.original = record->desc;
        auto desc = record->desc; desc.read = ReentrantReadProbe::Read;
        auto api = f.OwnerApi(); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK); f.Refresh();
        Text(f.field, L"7"); probe.reads = 0; probe.trigger = phase;
        if (!phase) f.edit.host->onReserve = [&] { probe.Fire(); };
        Enter(f.field); CHECK(probe.fired == 1);
        f.edit.host->onReserve = {}; probe.trigger = 0;
        f.Clean(0, 0, 1); CHECK(f.probe.Cancels() == 1 && !f.edit.observations->published);
        if (action == 3 || action == 4) {
            CHECK(Caption(f.field) == (action == 3 ? L"8" : L"7"));
            if (action == 4) SendMessageW(f.field, WM_IME_ENDCOMPOSITION, 0, 0);
            Enter(f.field); f.Clean(1, action == 3 ? 8 : 7, 2);
        } else {
            CHECK(Caption(f.field) == L"0");
            ShowWindow(f.wrapper, SW_SHOWNA); EnableWindow(f.wrapper, TRUE);
            Text(f.field, L"9"); Enter(f.field); f.Clean(1, 9, 2);
        }
        f.Close();
    }
}
void ReboundFocus() {
    ui_input_broker_checks::Fixture f; Window window; f.Bridges();
    NativePropertyInput input(f.registry, f.elementId, window.value, 0xb002, {}, {}); input.Refresh(96);
    SetWindowPos(input.Window(), nullptr, 5, 5, 500, 28, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const auto old = FindField(input.Window()); SetFocus(window.sink);
    const std::string newId = "example.input.vector";
    auto desc = f.desc; desc.info.id = mikumikustudio::plugins::Text(newId); desc.info.valueType = MMS_VALUE_VECTOR; desc.info.components = 2;
    desc.info.minimum = {}; desc.info.maximum = {}; MmsHandle property = 0;
    CHECK(f.ui.registerProperty(f.ui.context, &desc, &property) == MMS_OK);
    auto element = f.registry.elements.at(f.elementId).desc; element.propertyId = mikumikustudio::plugins::Text(newId);
    CHECK(f.ui.updateElement(f.ui.context, f.element, &element) == MMS_OK);
    f.value = {}; f.value.type = MMS_VALUE_VECTOR; f.value.components = 2; f.value.value.vector[0] = 1; f.value.value.vector[1] = 2;
    // Deliberately focus the still displayed scalar before NativeUi refreshes.
    SetFocus(old); Pump(); auto x = FindField(input.Window()), y = FindField(input.Window(), 0x7202);
    CHECK(GetFocus() == x && f.registry.elements.at(f.elementId).handle == f.element);
    Text(x, L"5"); Text(y, L"6"); Enter(y);
    CHECK(f.edits == 1 && f.value.value.vector[0] == 5 && f.value.value.vector[1] == 6);
    SetFocus(window.sink); input.Refresh(175); Pump(); CHECK(GetFocus() == window.sink);
}
void LargeFields() {
    // TEXT already has the same 1 MiB codec bound; BYTES may hold 16 MiB,
    // while its default hexadecimal field can represent only 512 KiB.
    for (uint32_t type : {MMS_VALUE_BYTES}) {
        ui_input_broker_checks::Fixture f; Window window; f.Bridges();
        const std::string id = "example.input.large", data(1024 * 1024 + 1, 'a');
        auto desc = f.desc; desc.info.id = mikumikustudio::plugins::Text(id); desc.info.valueType = type;
        desc.info.minimum = {}; desc.info.maximum = {}; MmsHandle property = 0;
        CHECK(f.ui.registerProperty(f.ui.context, &desc, &property) == MMS_OK);
        auto element = f.registry.elements.at(f.elementId).desc; element.propertyId = mikumikustudio::plugins::Text(id);
        CHECK(f.ui.updateElement(f.ui.context, f.element, &element) == MMS_OK);
        f.value = {}; f.value.type = type;
        if (type == MMS_VALUE_TEXT) f.value.value.text = mikumikustudio::plugins::Text(data);
        else f.value.value.bytes = {data.data(), uint64_t(data.size())};
        NativePropertyInput input(f.registry, f.elementId, window.value, 0xb003, {}, {}); input.Refresh(96);
        SetWindowPos(input.Window(), nullptr, 5, 5, 500, 28, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const auto field = FindField(input.Window()); const auto summary = Caption(field);
        CHECK(!summary.empty() && summary.size() < 512 && (GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY));
        SetFocus(field); SendMessageW(field, WM_CHAR, L'7', 1); Enter(field); Pump();
        CHECK(Caption(field) == summary && (GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY) && !f.edits);
    }
}
void VectorFocus() {
    ui_input_broker_checks::Fixture f; Window window;
    const auto oldElement = f.element, oldProperty = f.property;
    CHECK(f.ui.unregisterItem(f.ui.context, f.element) == MMS_OK);
    CHECK(f.ui.unregisterItem(f.ui.context, f.property) == MMS_OK);
    f.value = {}; f.value.type = MMS_VALUE_VECTOR; f.value.components = 2; f.value.value.vector[0] = 1; f.value.value.vector[1] = 2;
    auto desc = f.desc; desc.info.valueType = MMS_VALUE_VECTOR; desc.info.components = 2; desc.info.minimum = {}; desc.info.maximum = {};
    CHECK(f.ui.registerProperty(f.ui.context, &desc, &f.property) == MMS_OK && f.property != oldProperty);
    auto element = ui_input_broker_checks::Input(f.elementId, f.owner.id, f.propertyId);
    CHECK(f.ui.registerElement(f.ui.context, &element, &f.element) == MMS_OK && f.element != oldElement); f.Bridges();
    NativePropertyInput input(f.registry, f.elementId, window.value, 0xb001, {}, {}); input.Refresh(96);
    SetWindowPos(input.Window(), nullptr, 5, 5, 500, 28, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HWND x = FindField(input.Window(), 0x7201), y = FindField(input.Window(), 0x7202);
    Text(x, L"5"); SetFocus(y); CHECK(GetFocus() == y); Pump(); CHECK(f.edits == 0);
    Text(y, L"6"); SetFocus(window.sink); CHECK(GetFocus() == window.sink); Pump();
    CHECK(f.edits == 1 && f.value.value.vector[0] == 5 && f.value.value.vector[1] == 6);
}
} // namespace native_property_input_checks

static void NativePropertyInputChecks(const fs::path& root, const fs::path& dll) {
    native_property_input_checks::Editing(root, dll);
    native_property_input_checks::CanonicalChangeWithoutRevision(root, dll);
    native_property_input_checks::ImeMessages(root, dll);
    native_property_input_checks::FreshFocus(root, dll);
    native_property_input_checks::Retirement(root, dll);
    native_property_input_checks::LayoutAndRefresh(root, dll);
    native_property_input_checks::DeferredClose(root, dll);
    native_property_input_checks::ReentrantInput(root, dll);
    native_property_input_checks::ReboundFocus();
    native_property_input_checks::LargeFields();
    native_property_input_checks::VectorFocus();
    std::cout << "native property INPUT messages/history/stale/IME sequence/layout/retirement PASS\n";
}
