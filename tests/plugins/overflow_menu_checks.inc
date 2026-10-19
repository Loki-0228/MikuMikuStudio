// Include after state_content_checks.inc. Exercises the production NativeUi
// adapter and a real TrackPopupMenu loop, not a substitute menu builder.
#include "native_ui_overflow.hpp"
#include <exception>
#include <cstdlib>

namespace overflow_allocation_fault {
inline thread_local std::size_t bytes = 0;
inline thread_local unsigned hits = 0;
inline thread_local DWORD resources = 0;
}
// Test executable only. The static production adapter resolves its ordinary
// operator new here, allowing a real allocation failure during UTF conversion.
void* operator new(std::size_t size) {
    if (overflow_allocation_fault::bytes && size == overflow_allocation_fault::bytes) {
        overflow_allocation_fault::bytes = 0; ++overflow_allocation_fault::hits;
        overflow_allocation_fault::resources = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace overflow_menu_checks {
struct Fixture {
    manager_refresh_checks::Window window;
    UiRegistry registry;
    UiClient owner{&registry, "example.overflow"};
    MmsUiApiV1 api = registry.Api(owner);
    MmsUiLayoutApiV1 layout = registry.LayoutApi(owner);
    std::unique_ptr<NativeUi> native;
    std::array<MmsHandle, 3> buttons{}, commands{};
    std::array<HWND, 3> controls{};
    std::array<unsigned, 3> calls{};
    std::array<uint32_t, 3> enabled{{1, 1, 1}}, checked{}, visible{{1, 1, 1}};
    struct Context { Fixture* self; size_t index; } contexts[3]{{this, 0}, {this, 1}, {this, 2}};
    bool grouped = false;
    explicit Fixture(bool group = false): grouped(group) {
        MmsHandle handle = 0;
        if (grouped) {
            MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S("example.overflow.group");
            element.ownerPlugin = Text(owner.id); element.parentId = S("mms.toolbar.main.extensions");
            element.kind = MMS_UI_GROUP; element.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_HIDE;
            element.label = S("Grouped commands");
            CHECK(api.registerElement(api.context, &element, &handle) == MMS_OK);
            MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.overflow = 3;
            hints.flags = MMS_HINT_KEEP_TOGETHER; hints.flow = MMS_FLOW_ROW;
            CHECK(layout.setDefaultHints(layout.context, element.id, &hints) == MMS_OK);
        }
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto id = owner.id + ".button" + std::to_string(i), commandId = id + ".command";
            MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
            command.info.id = Text(commandId); command.info.ownerPlugin = Text(owner.id); command.context = &contexts[i];
            command.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* state) -> MmsResult {
                auto& context = *static_cast<Context*>(p); auto& x = *context.self; const auto i = context.index;
                *state = {Header<MmsCommandStateV1>(), x.enabled[i], x.checked[i], x.visible[i], 0, {}}; return MMS_OK;
            };
            command.invoke = [](void* p, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult {
                auto& context = *static_cast<Context*>(p); ++context.self->calls[context.index]; return MMS_OK;
            };
            CHECK(api.registerCommand(api.context, &command, &commands[i]) == MMS_OK);
            MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = Text(id); element.ownerPlugin = Text(owner.id);
            element.commandId = Text(commandId); element.parentId = grouped ? S("example.overflow.group") : S("mms.toolbar.main.extensions");
            element.kind = MMS_UI_BUTTON; element.capabilities = MMS_UI_LAYOUT | MMS_UI_HIDE | MMS_UI_MOVE;
            const auto caption = "Button " + std::to_string(i); element.label = Text(caption);
            CHECK(api.registerElement(api.context, &element, &buttons[i]) == MMS_OK);
            MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.minimum[0] = hints.preferred[0] = 90;
            hints.preferred[1] = 24; hints.order = int32_t(i); if (!grouped) hints.overflow = 3;
            CHECK(layout.setDefaultHints(layout.context, element.id, &hints) == MMS_OK);
        }
        native = std::make_unique<NativeUi>(registry, window.value, Log{}); native->Refresh(); native->Layout(0, 500, 32, true);
        for (auto child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            for (size_t i = 0; i < controls.size(); ++i) if (Caption(child) == L"Button " + std::to_wstring(i)) controls[i] = child;
        for (const auto control : controls) CHECK(control);
    }
    ~Fixture() { native.reset(); registry.Retire(owner); }
    static std::wstring Caption(HWND window) { wchar_t buffer[1024]{}; GetWindowTextW(window, buffer, 1024); return buffer; }
    HWND More() const {
        for (auto child = GetWindow(native->Toolbar(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            if (Caption(child) == L"更多") return child;
        return nullptr;
    }
    bool Shown(HWND control) const { return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0; }
    MmsUiLayoutItemV1 Geometry(size_t index) {
        MmsUiLayoutSnapshotV1 value{}; value.h = Header<MmsUiLayoutSnapshotV1>();
        CHECK(layout.acquireSnapshot(layout.context, &value) == MMS_OK);
        struct Guard { MmsUiLayoutApiV1 api; MmsHandle handle; ~Guard() { api.releaseSnapshot(api.context, handle); } } guard{layout, value.handle};
        const auto wanted = owner.id + ".button" + std::to_string(index);
        for (uint64_t i = 0; i < value.items.count; ++i) {
            auto* item = reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const char*>(value.items.data) + i * value.items.stride);
            if (String(item->elementId) == wanted) { auto out = *item; out.elementId = {}; return out; }
        }
        throw std::runtime_error("overflow geometry missing");
    }
};
// The hook only observes the owning thread. Cancellation is posted back into
// the real menu loop; a timer prevents a broken test from hanging indefinitely.
struct MenuLoop {
    Fixture& fixture;
    std::function<void(HMENU)> inspect;
    HMENU seen = nullptr;
    HHOOK hook = nullptr;
    HHOOK filter = nullptr;
    HHOOK queued = nullptr;
    bool timedOut = false;
    bool selectFirst = false;
    bool waitForClose = false;
    unsigned parentMenuNotifications = 0, menuCommands = 0;
    unsigned closeMessages = 0;
    bool menuCharObserved = false;
    bool inspectPosted = false, inspectCompleted = false, characterPosted = false;
    unsigned inspectDeliveries = 0, characterRemovals = 0, characterFilters = 0, characterDispatches = 0, characterRetries = 0;
    DWORD inspectPostError = ERROR_SUCCESS, characterPostError = ERROR_SUCCESS;
    struct Trace { char source; UINT message; HWND window; WPARAM wp; LPARAM lp; DWORD elapsed; };
    std::array<Trace, 128> trace{};
    size_t traceCount = 0, traceDropped = 0;
    DWORD started = GetTickCount();
    std::vector<WPARAM> selections;
    std::vector<UINT> events;
    std::vector<std::pair<UINT, WPARAM>> input;
    std::exception_ptr error;
    inline static thread_local MenuLoop* current = nullptr;
    static constexpr UINT message = WM_APP + 0x3a1;
    static constexpr UINT_PTR timer = 0x4f564652;
    static bool Relevant(UINT msg) noexcept {
        return msg == message || msg == WM_TIMER || msg == WM_CHAR || msg == WM_SYSCHAR ||
            msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
            msg == WM_ENTERMENULOOP || msg == WM_INITMENU || msg == WM_INITMENUPOPUP ||
            msg == WM_ENTERIDLE || msg == WM_MENUSELECT || msg == WM_MENUCHAR || msg == WM_EXITMENULOOP ||
            msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
            msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_CLOSE ||
            msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP;
    }
    void Record(char source, HWND window, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        if (traceCount < trace.size()) trace[traceCount++] = {source, msg, window, wp, lp, GetTickCount() - started};
        else ++traceDropped;
    }
    void Dump() const {
        std::cerr << "overflow loop seen=" << bool(seen) << " timeout=" << timedOut << " parent=" << parentMenuNotifications
            << " commands=" << menuCommands << " select=" << selectFirst << " close=" << waitForClose
            << " inspect_post=" << inspectPosted << ':' << inspectPostError << " inspect_delivered=" << inspectDeliveries
            << " inspect_completed=" << inspectCompleted << " inspect_error=" << bool(error)
            << " char_post=" << characterPosted << ':' << characterPostError << " char_retry=" << characterRetries
            << " char_removed=" << characterRemovals
            << " char_filter=" << characterFilters << " char_dispatch=" << characterDispatches
            << " menuchar=" << menuCharObserved << " foreground=" << GetForegroundWindow()
            << " focus=" << GetFocus() << " capture=" << GetCapture() << " trace_dropped=" << traceDropped << '\n';
        // S: sent notification, O: owner subclass, R/P: removed/peeked queue
        // message, F: MSGF_MENU. Bounded storage does not allocate in hooks.
        for (size_t i = 0; i < traceCount; ++i) {
            const auto& entry = trace[i];
            std::cerr << "overflow trace ms=" << entry.elapsed << " source=" << entry.source << " hwnd=" << entry.window
                << " msg=" << entry.message << " wp=" << entry.wp << " lp=" << entry.lp << '\n';
        }
    }
    static LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && current) {
            auto& self = *current; const auto& sent = *reinterpret_cast<const CWPSTRUCT*>(lp);
            if ((sent.hwnd == self.fixture.native->Toolbar() || sent.hwnd == self.fixture.window.value) && Relevant(sent.message))
                self.Record('S', sent.hwnd, sent.message, sent.wParam, sent.lParam);
            if (sent.hwnd == self.fixture.native->Toolbar()) {
                if (sent.message == WM_MENUSELECT) self.selections.push_back(sent.wParam);
                if (sent.message == WM_CANCELMODE || sent.message == WM_EXITMENULOOP || sent.message == WM_INITMENUPOPUP)
                    self.events.push_back(sent.message);
            }
            if (sent.hwnd == self.fixture.native->Toolbar() && sent.message == WM_INITMENUPOPUP && !self.seen) {
                self.seen = reinterpret_cast<HMENU>(sent.wParam);
                SetLastError(ERROR_SUCCESS);
                self.inspectPosted = PostMessageW(sent.hwnd, message, 0, 0) != FALSE;
                self.inspectPostError = self.inspectPosted ? ERROR_SUCCESS : GetLastError();
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK Owner(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<MenuLoop*>(data);
        if (Relevant(msg)) self.Record('O', window, msg, wp, lp);
        if (msg == WM_CHAR && wp == L'~') ++self.characterDispatches;
        if (window == self.fixture.window.value &&
            (msg == WM_ENTERMENULOOP || msg == WM_INITMENU || msg == WM_INITMENUPOPUP || msg == WM_EXITMENULOOP))
            ++self.parentMenuNotifications;
        if (msg == WM_COMMAND && !lp) ++self.menuCommands;
        if (window == self.fixture.window.value && msg == WM_CLOSE) ++self.closeMessages;
        if (msg == WM_MENUCHAR && self.selectFirst && reinterpret_cast<HMENU>(lp) == self.seen) {
            // Documented native-menu input protocol, including for owner-drawn
            // menus. Let USER32 execute the first real item and return its ID.
            self.menuCharObserved = true; return MAKELRESULT(0, MNC_EXECUTE);
        }
        if (msg == message || (msg == WM_TIMER && wp == timer)) {
            if (msg == message) {
                ++self.inspectDeliveries;
                try { self.inspect(self.seen); self.inspectCompleted = true; }
                catch (...) { self.error = std::current_exception(); }
            }
            else {
                // A menu that is still settling can drop the posted character
                // before USER32 turns it into WM_MENUCHAR. Retry the documented
                // path a bounded number of times before declaring a timeout.
                if (self.selectFirst && !self.error && !self.menuCharObserved && self.characterPosted &&
                    self.characterRetries < 2) {
                    ++self.characterRetries;
                    SetLastError(ERROR_SUCCESS);
                    self.characterPosted = PostMessageW(self.fixture.native->Toolbar(), WM_CHAR, L'~', 1) != FALSE;
                    self.characterPostError = self.characterPosted ? ERROR_SUCCESS : GetLastError();
                    SetTimer(self.fixture.window.value, timer, 1000, nullptr);
                    return 0;
                }
                self.timedOut = true;
            }
            if (msg == message && self.selectFirst && !self.error) {
                // Drive the actual native selection path on this thread. The
                // production owner receives no fake WM_COMMAND from the test.
                SetLastError(ERROR_SUCCESS);
                self.characterPosted = PostMessageW(self.fixture.native->Toolbar(), WM_CHAR, L'~', 1) != FALSE;
                self.characterPostError = self.characterPosted ? ERROR_SUCCESS : GetLastError();
            } else if (!self.waitForClose || self.error || msg == WM_TIMER) EndMenu();
            return 0;
        }
        return DefSubclassProc(window, msg, wp, lp);
    }
    static LRESULT CALLBACK Filter(int code, WPARAM wp, LPARAM lp) {
        if (code == MSGF_MENU && current) {
            auto& self = *current;const auto& message=*reinterpret_cast<const MSG*>(lp);
            self.input.emplace_back(message.message,message.wParam);
            if (Relevant(message.message)) self.Record('F', message.hwnd, message.message, message.wParam, message.lParam);
            if (message.message == WM_CHAR && message.wParam == L'~') ++self.characterFilters;
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    static LRESULT CALLBACK Queued(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && current) {
            auto& self = *current; const auto& message = *reinterpret_cast<const MSG*>(lp);
            if (Relevant(message.message)) self.Record(wp == PM_REMOVE ? 'R' : 'P', message.hwnd, message.message, message.wParam, message.lParam);
            if (wp == PM_REMOVE && message.message == WM_CHAR && message.wParam == L'~' &&
                message.hwnd == self.fixture.native->Toolbar()) ++self.characterRemovals;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    MenuLoop(Fixture& x, std::function<void(HMENU)> callback, bool choose = false): fixture(x), inspect(std::move(callback)), selectFirst(choose) {
        CHECK(!current); current = this;
        CHECK(SetWindowSubclass(fixture.window.value, Owner, 0x4f5646, reinterpret_cast<DWORD_PTR>(this)));
        CHECK(SetWindowSubclass(fixture.native->Toolbar(), Owner, 0x4f5646, reinterpret_cast<DWORD_PTR>(this)));
        hook = SetWindowsHookExW(WH_CALLWNDPROC, Observe, nullptr, GetCurrentThreadId()); CHECK(hook);
        filter = SetWindowsHookExW(WH_MSGFILTER, Filter, nullptr, GetCurrentThreadId()); CHECK(filter);
        queued = SetWindowsHookExW(WH_GETMESSAGE, Queued, nullptr, GetCurrentThreadId()); CHECK(queued);
        CHECK(SetTimer(fixture.window.value, timer, 3000, nullptr));
    }
    ~MenuLoop() {
        KillTimer(fixture.window.value, timer); if (hook) UnhookWindowsHookEx(hook);
        if(filter)UnhookWindowsHookEx(filter);
        if (queued) UnhookWindowsHookEx(queued);
        RemoveWindowSubclass(fixture.window.value, Owner, 0x4f5646); current = nullptr;
        RemoveWindowSubclass(fixture.native->Toolbar(), Owner, 0x4f5646);
    }
    void Run() {
        const auto more = fixture.More(); CHECK(more && fixture.Shown(more));
        SendMessageW(more, BM_CLICK, 0, 0);
        if (error) { Dump(); std::rethrow_exception(error); }
        if (!seen || timedOut || parentMenuNotifications || menuCommands || (selectFirst && !menuCharObserved)) Dump();
        CHECK(seen && !timedOut && !parentMenuNotifications && !menuCommands && (!selectFirst || menuCharObserved));
    }
};
static std::wstring MenuText(HMENU menu, unsigned id, bool position = false) {
    wchar_t text[1024]{}; MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_STRING; item.dwTypeData = text; item.cch = 1024;
    CHECK(GetMenuItemInfoW(menu, id, position, &item)); return text;
}
static void NativeResizeIdentity() {
    Fixture x; const auto revision = x.registry.Revision(), preferences = x.registry.PreferencesRevision();
    RefreshClient client{&x.registry.Refresh(), x.owner.id}; auto refresh = x.registry.Refresh().Api(client);
    MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
    CHECK(refresh.getTarget(refresh.context, x.buttons[2], &target) == MMS_OK && target.ready);
    const auto original = target.target;
    CHECK(!x.Shown(x.More()));
    CHECK(x.native->Layout(0, 190, 32, true) == 32);
    CHECK(x.Shown(x.More()) && x.Shown(x.controls[0]) && !x.Shown(x.controls[1]) && !x.Shown(x.controls[2]));
    const auto first = x.Geometry(0), last = x.Geometry(2);
    CHECK(first.visible && !first.overflowed && !last.visible && last.overflowed && last.rect[2] > 0 && last.rect[3] > 0);
    CHECK(refresh.getTarget(refresh.context, x.buttons[2], &target) == MMS_OK && target.ready);
    CHECK(target.target.element == original.element && target.target.instanceGeneration == original.instanceGeneration);
    SetFocus(x.controls[0]); x.native->Layout(0, 60, 32, true); CHECK(GetFocus() == x.More());
    x.native->Layout(0, 0, 32, true); CHECK(!x.Shown(x.More()));
    RECT rect{}; CHECK(GetClientRect(x.More(), &rect) && rect.right == 0);
    x.native->Layout(0, 500, 32, true); CHECK(!x.Shown(x.More()));
    for (size_t i = 0; i < x.controls.size(); ++i) CHECK(x.Shown(x.controls[i]) && GetDlgCtrlID(x.controls[i]) >= 0xa000);
    CHECK(x.registry.Revision() == revision && x.registry.PreferencesRevision() == preferences);
    x.registry.Refresh().Retire(client);
}
static void NativeMenuStateAndLifetime() {
    Fixture x; x.enabled[1] = 0; x.checked[2] = 1; x.native->Refresh(); x.native->Layout(0, 190, 32, true);
    const auto id1 = unsigned(GetDlgCtrlID(x.controls[1])), id2 = unsigned(GetDlgCtrlID(x.controls[2]));
    MenuLoop loop(x, [&](HMENU menu) {
        CHECK(GetMenuItemCount(menu) == 2 && GetMenuItemID(menu, 0) == id1 && GetMenuItemID(menu, 1) == id2);
        CHECK((GetMenuState(menu, id1, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) && (GetMenuState(menu, id2, MF_BYCOMMAND) & MF_CHECKED));
        CHECK(MenuText(menu, id1) == L"Button 1");
        // The ordinary dispatch gate must not invoke disabled commands, even
        // if a stale native menu or caller still has its previous numeric ID.
        CHECK(x.native->Command(id1)); CHECK(x.calls[1] == 0);
        x.enabled[1] = 1; x.checked[1] = 1; x.native->Refresh();
        CHECK(!(GetMenuState(menu, id1, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) && (GetMenuState(menu, id1, MF_BYCOMMAND) & MF_CHECKED));
        CHECK(x.native->Command(id1) && x.calls[1] == 1);
        x.registry.Retire(x.owner); x.native->Refresh();
        CHECK(GetMenuState(menu, id1, MF_BYCOMMAND) == UINT(-1));
        CHECK(!x.native->Command(id1) && x.calls[1] == 1);
    });
    loop.Run(); CHECK(!x.Shown(x.More()));
}
static void NativeMenuSingleSelection() {
    Fixture x; x.native->Layout(0, 190, 32, true);
    const auto id = unsigned(GetDlgCtrlID(x.controls[1]));
    MenuLoop loop(x, [&](HMENU menu) {
        CHECK(GetMenuItemCount(menu) == 2 && GetMenuItemID(menu, 0) == id);
    }, true);
    loop.Run();
    if (x.calls[0] != 0 || x.calls[1] != 1 || x.calls[2] != 0)
        { std::cerr << "overflow selection calls=" << x.calls[0] << ',' << x.calls[1] << ',' << x.calls[2] << " menu=";
          for (auto value : loop.selections) std::cerr << std::hex << value << ',';
          std::cerr << " events=";for (auto value : loop.events)std::cerr<<value<<',';
          std::cerr<<" input=";for(const auto& value:loop.input)std::cerr<<value.first<<':'<<value.second<<',';std::cerr<<std::dec<<'\n'; }
    CHECK(x.calls[0] == 0 && x.calls[1] == 1 && x.calls[2] == 0);
    // Flush any posted notification after TPM_RETURNCMD returns. It must not
    // create a duplicate invocation or escape to the native main window.
    manager_refresh_checks::Pump(5);
    CHECK(x.calls[0] == 0 && x.calls[1] == 1 && x.calls[2] == 0 && !loop.menuCommands && !loop.parentMenuNotifications);
}
static void NativeCloseDuringPopup() {
    Fixture x; x.native->Layout(0, 190, 32, true);
    MenuLoop loop(x, [&](HMENU menu) {
        CHECK(IsMenu(menu) && x.native->PopupActive());
        CHECK(PostMessageW(x.window.value, WM_CLOSE, 0x41, 0x42));
    });
    loop.waitForClose = true; loop.Run();
    CHECK(loop.closeMessages == 1 && IsWindow(x.window.value) && !x.native->PopupActive());
    CHECK(x.calls[0] == 0 && x.calls[1] == 0 && x.calls[2] == 0);
    manager_refresh_checks::Pump(10);
    CHECK(loop.closeMessages == 2 && !IsWindow(x.window.value) && !x.native->PopupActive());
}
static void NativeSubmenuAllocationFailure() {
    Fixture x(true);
    // This exact allocation belongs to Wide(group label), after the child
    // HMENU and its three command items have been built, before AppendMenu.
    const std::string label(1016, 'G');
    auto element = x.registry.elements.at("example.overflow.group").desc; element.label = Text(label);
    CHECK(x.api.updateElement(x.api.context, x.registry.elements.at("example.overflow.group").handle, &element) == MMS_OK);
    x.native->Refresh(); x.native->Layout(0, 190, 48, true);
    const auto allocationBytes = (std::wstring(label.size(), L'\0').capacity() + 1) * sizeof(wchar_t);
    // Native focus/IME/menu initialization lazily creates other USER objects.
    // Warm the same HWND and drain that work before measuring HMENU ownership.
    { MenuLoop warm(x, [&](HMENU menu) { CHECK(GetSubMenu(menu, 0)); }); warm.Run(); }
    manager_refresh_checks::Pump(5);
    const auto before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); CHECK(before > 0);
    overflow_allocation_fault::hits = 0; overflow_allocation_fault::resources = 0;
    overflow_allocation_fault::bytes = allocationBytes;
    SendMessageW(x.More(), BM_CLICK, 0, 0);
    overflow_allocation_fault::bytes = 0;
    CHECK(overflow_allocation_fault::hits == 1 && overflow_allocation_fault::resources >= before + 2);
    const auto after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (after != before || x.native->PopupActive()) std::cerr << "overflow allocation resources before=" << before
        << " during=" << overflow_allocation_fault::resources << " after=" << after << " popup=" << x.native->PopupActive() << '\n';
    CHECK(after == before && !x.native->PopupActive());
    std::cout << "OVERFLOW_ALLOCATION resources=" << before << ',' << overflow_allocation_fault::resources << ',' << after
        << " fault_bytes=" << allocationBytes << " recovered=1\n";
    CHECK(x.calls[0] == 0 && x.calls[1] == 0 && x.calls[2] == 0);
    // The next real popup still works after the exception boundary recovers.
    MenuLoop loop(x, [&](HMENU menu) { CHECK(GetSubMenu(menu, 0) && GetMenuItemCount(GetSubMenu(menu, 0)) == 3); });
    loop.Run();
}
static void NativeGroupAndStateContent() {
    Fixture x(true); x.native->Layout(0, 190, 48, true);
    for (auto control : x.controls) CHECK(!x.Shown(control));
    MenuLoop loop(x, [&](HMENU menu) {
        CHECK(GetMenuItemCount(menu) == 1); const auto group = GetSubMenu(menu, 0); CHECK(group && GetMenuItemCount(group) == 3);
        CHECK(MenuText(menu, 0, true) == L"Grouped commands");
        const auto id = unsigned(GetDlgCtrlID(x.controls[2]));
        RefreshClient client{&x.registry.Refresh(), x.owner.id}; auto refresh = x.registry.Refresh().Api(client);
        auto content = x.registry.StateContentApi(x.owner); MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context, x.buttons[2], &target) == MMS_OK && target.ready);
        struct Context { MmsUiRefreshStateContentApiV1* api; } context{&content};
        MmsUiStateContentDescV1 descriptor{}; descriptor.h = Header<MmsUiStateContentDescV1>(); descriptor.target = target.target;
        descriptor.fields = MMS_UI_STATE_LABEL; descriptor.context = &context;
        descriptor.prepare = [](void* p, const MmsUiTargetRefV1*, MmsHandle builder) -> MmsResult {
            auto& context = *static_cast<Context*>(p); MmsUiStateContentV1 value{}; value.h = Header<MmsUiStateContentV1>();
            value.fields = MMS_UI_STATE_LABEL; value.label = S("Updated overflow caption");
            return context.api->writePrepared(context.api->context, builder, &value);
        };
        MmsHandle provider = 0; CHECK(content.registerProvider(content.context, &descriptor, &provider) == MMS_OK);
        x.enabled[2] = 0; x.checked[2] = 1;
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>(); request.target = target.target;
        request.kinds = MMS_UI_REFRESH_STATE; request.scope = MMS_UI_REFRESH_SELF;
        MmsHandle ticket = 0; CHECK(refresh.request(refresh.context, &request, &ticket) == MMS_OK); x.registry.Refresh().Drain();
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>(); CHECK(refresh.query(refresh.context, ticket, &state) == MMS_OK);
        CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.result == MMS_OK);
        CHECK(MenuText(group, id) == L"Updated overflow caption" && (GetMenuState(group, id, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) &&
            (GetMenuState(group, id, MF_BYCOMMAND) & MF_CHECKED));
        CHECK(!x.Shown(x.controls[2]));
        CHECK(content.unregisterProvider(content.context, provider) == MMS_OK);
        CHECK(refresh.release(refresh.context, ticket) == MMS_OK); x.registry.Refresh().Retire(client);
    });
    loop.Run(); x.native->Layout(0, 500, 48, true);
    for (auto control : x.controls) CHECK(x.Shown(control));
    CHECK(Fixture::Caption(x.controls[2]) == L"Updated overflow caption");
}
}
static void OverflowMenuChecks() {
    overflow_menu_checks::NativeResizeIdentity();
    overflow_menu_checks::NativeMenuStateAndLifetime();
    overflow_menu_checks::NativeMenuSingleSelection();
    overflow_menu_checks::NativeCloseDuringPopup();
    overflow_menu_checks::NativeSubmenuAllocationFailure();
    overflow_menu_checks::NativeGroupAndStateContent();
}
