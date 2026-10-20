// Include after ui_input_broker_checks.inc and native_property_input_checks.inc.
// The UI, provider, builder and queue are production objects. Only the INT
// property and its journal participant are a small deterministic data fixture.
#include <mms/cpp/state_content.hpp>
namespace input_state_content_checks {
using native_property_input_checks::Caption;
using native_property_input_checks::Enter;
using native_property_input_checks::Escape;
using native_property_input_checks::FindField;
using native_property_input_checks::Pump;
void SetRaw(HWND field, const wchar_t* text) { native_property_input_checks::Text(field, text); }
// Plugin-runtime tests do not construct the application/core. These are local
// edit-bridge observations, not proof of an actual scene evaluation/playhead.
struct EditObservation { bool dirty = false; int frame = 23; };
struct IntChange final : HistoryChange {
    MmsValue& value; EditObservation& observation; int64_t before, after;
    IntChange(MmsValue& v, EditObservation& o, int64_t next): value(v), observation(o), before(v.value.signedValue), after(next) {}
    struct Candidate final : PreparedHistoryChange {
        IntChange& owner; bool forward;
        Candidate(IntChange& o, bool f): owner(o), forward(f) {}
        bool IsCurrent() const noexcept override { return owner.value.value.signedValue == (forward ? owner.before : owner.after); }
        void Publish() noexcept override { owner.value.value.signedValue = forward ? owner.after : owner.before; owner.observation.dirty = true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        auto candidate = std::make_unique<Candidate>(*this, forward);
        if (!candidate->IsCurrent()) return MMS_E_STALE;
        out = std::move(candidate); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return sizeof(*this); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
HWND Label(HWND wrapper) {
    HWND found = nullptr;
    for (auto child = GetWindow(wrapper, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t type[32]{}; GetClassNameW(child, type, 32);
        if (!_wcsicmp(type, L"STATIC")) { CHECK(!found); found = child; }
    }
    CHECK(found); return found;
}
HWND Wrapper(HWND toolbar, const wchar_t* caption) {
    for (auto child = GetWindow(toolbar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!GetDlgItem(child, 0x7201)) continue;
        if (Caption(Label(child)) == caption) return child;
    }
    CHECK(false); return nullptr;
}
struct Fixture {
    ui_input_broker_checks::Fixture data;
    native_property_input_checks::Window window;
    EditObservation observation;
    StorageStore storage; SharedHistory history{storage};
    UiRefreshQueue& queue = data.registry.Refresh(); RefreshClient client{&queue, data.owner.id};
    MmsUiRefreshApiV1 refresh = queue.Api(client);
    MmsUiRefreshStateContentApiV1 content = data.registry.StateContentApi(data.owner);
    std::unique_ptr<NativeUi> native;
    HWND wrapper = nullptr, field = nullptr, labelWindow = nullptr;
    MmsHandle provider = 0, peer = 0, peerProvider = 0, group = 0;
    std::vector<MmsHandle> tickets;
    unsigned prepares = 0;
    uint32_t fields = 7, enabled = 0, readOnly = 0, visible = 0;
    bool legacy = false;
    std::string label = "Dynamic input", description = "Input description", reason = "Input disabled reason";
    std::function<MmsResult(MmsHandle)> prepare;
    explicit Fixture(bool host = true, bool readonly = false, bool subtree = false, bool emptyLabel = false): data(readonly) {
        if (host) data.registry.SetPropertyInputBridges([this](UiClient& owner, uint64_t& revision) {
            CHECK(&owner == &data.owner); return history.Revision(revision);
        }, [this](UiClient& owner, const PropertyInputSnapshot&, const MmsValue& value) {
            CHECK(&owner == &data.owner); ++data.edits;
            auto change = std::make_shared<IntChange>(data.value, observation, value.value.signedValue);
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            const auto result = history.PrepareNative("INPUT fixture property", change, transaction);
            return result ? result : history.CommitNative(std::move(transaction));
        });
        if (subtree) {
            MmsUiElementV1 container{}; container.h = Header<MmsUiElementV1>(); container.id = S("example.input.group");
            container.ownerPlugin = Text(data.owner.id); container.parentId = S("mms.toolbar.main.extensions");
            container.kind = MMS_UI_GROUP; container.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT;
            CHECK(data.ui.registerElement(data.ui.context, &container, &group) == MMS_OK);
            const std::string peerId = "example.input.peer";
            auto other = ui_input_broker_checks::Input(peerId, data.owner.id, data.propertyId);
            other.parentId = container.id; other.label = S("Peer input");
            CHECK(data.ui.registerElement(data.ui.context, &other, &peer) == MMS_OK);
        }
        // Registry mutations may replace the backing metadata arenas. Borrow
        // this descriptor only after the group's registrations are complete.
        auto element = data.registry.elements.at(data.elementId).desc;
        if (emptyLabel) element.label = {};
        if (subtree) element.parentId = S("example.input.group");
        CHECK(data.ui.updateElement(data.ui.context, data.element, &element) == MMS_OK);
        auto layout = data.registry.LayoutApi(data.owner); MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>();
        hints.flags = MMS_HINT_PRESERVE_HIDDEN_SLOT; hints.minimum[0] = 90; hints.preferred[0] = 240; hints.preferred[1] = 28; hints.grow[0] = 1;
        CHECK(layout.setDefaultHints(layout.context, Text(data.elementId), &hints) == MMS_OK);
        native = std::make_unique<NativeUi>(data.registry, window.value, Log{}); native->Refresh();
        CHECK(native->Layout(10, 700, 180, true) > 0);
        wrapper = Wrapper(native->Toolbar(), emptyLabel ? L"Input" : L"Editable value"); field = FindField(wrapper); labelWindow = Label(wrapper);
        CHECK(Caption(field) == L"2");
        prepare = [this](MmsHandle builder) {
            if (legacy) {
                MmsUiStateContentV1 value{}; value.h = Header<MmsUiStateContentV1>(); value.fields = fields;
                if (fields & MMS_UI_STATE_LABEL) value.label = Text(label);
                if (fields & MMS_UI_STATE_DESCRIPTION) value.description = Text(description);
                if (fields & MMS_UI_STATE_DISABLED_REASON) value.disabledReason = Text(reason);
                return content.writePrepared(content.context, builder, &value);
            }
            auto value = Value(); return content.writePrepared(content.context, builder, &value.content);
        };
    }
    ~Fixture() {
        if (provider) content.unregisterProvider(content.context, provider);
        if (peerProvider) content.unregisterProvider(content.context, peerProvider);
        for (auto ticket : tickets) { refresh.cancel(refresh.context, ticket); refresh.release(refresh.context, ticket); }
        native.reset(); CHECK(history.Reset() == MMS_OK); history.Collect(); queue.Retire(client); data.registry.Retire(data.owner);
    }
    MmsUiInputStateContentV1 Value() {
        MmsUiInputStateContentV1 value{}; value.content.h = Header<MmsUiInputStateContentV1>(); value.content.fields = fields;
        if (fields & MMS_UI_STATE_LABEL) value.content.label = Text(label);
        if (fields & MMS_UI_STATE_DESCRIPTION) value.content.description = Text(description);
        if (fields & MMS_UI_STATE_DISABLED_REASON) value.content.disabledReason = Text(reason);
        value.enabled = enabled; value.readOnly = readOnly; value.visible = visible; return value;
    }
    MmsUiRefreshTargetV1 Target(MmsHandle handle = 0) {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context, handle ? handle : data.element, &target) == MMS_OK);
        CHECK(target.supportedKinds & MMS_UI_REFRESH_STATE); return target;
    }
    MmsUiStateContentDescV1 Desc(MmsHandle handle = 0) {
        MmsUiStateContentDescV1 desc{}; desc.h = Header<MmsUiStateContentDescV1>(); desc.target = Target(handle).target;
        desc.fields = 63; desc.context = this;
        desc.prepare = [](void* context, const MmsUiTargetRefV1* target, MmsHandle builder) -> MmsResult {
            auto& f = *static_cast<Fixture*>(context); ++f.prepares;
            CHECK(f.data.registry.InCallback() && f.queue.ReadOnlyCause(f.client));
            CHECK(target && (target->element == f.data.element || target->element == f.peer));
            return f.prepare(builder);
        }; return desc;
    }
    void Register(bool both = false) {
        auto desc = Desc(); CHECK(content.registerProvider(content.context, &desc, &provider) == MMS_OK && provider);
        if (both) { desc = Desc(peer); CHECK(content.registerProvider(content.context, &desc, &peerProvider) == MMS_OK && peerProvider); }
    }
    MmsHandle Submit(uint32_t kinds = MMS_UI_REFRESH_STATE, bool worker = false, bool subtree = false, uint64_t key = 0) {
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = Target(subtree ? group : 0).target; request.kinds = kinds;
        request.scope = subtree ? MMS_UI_REFRESH_SUBTREE : MMS_UI_REFRESH_SELF; request.coalesceKey = key;
        MmsHandle ticket = 0; MmsResult result = MMS_E_FAILED;
        if (worker) { std::thread thread([&] { result = refresh.request(refresh.context, &request, &ticket); }); thread.join(); }
        else result = refresh.request(refresh.context, &request, &ticket);
        CHECK(result == MMS_OK && ticket); tickets.push_back(ticket); return ticket;
    }
    MmsUiRefreshStateV1 State(MmsHandle ticket) {
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>(); CHECK(refresh.query(refresh.context, ticket, &state) == MMS_OK); return state;
    }
    void Complete(MmsHandle ticket, uint32_t kinds = MMS_UI_REFRESH_STATE) {
        const auto state = State(ticket); CHECK(state.status == MMS_UI_REFRESH_COMPLETED && !state.result && state.completedKinds == kinds && !state.sourceRevision);
    }
    void Drain(uint32_t kinds = MMS_UI_REFRESH_STATE) { const auto ticket = Submit(kinds); queue.Drain(); Complete(ticket, kinds); }
    HWND Tooltip() {
        struct Search { HWND owner, found = nullptr; } search{window.value};
        EnumThreadWindows(GetCurrentThreadId(), [](HWND handle, LPARAM context) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(context); wchar_t type[64]{}; GetClassNameW(handle, type, 64);
            if (GetWindow(handle, GW_OWNER) == s.owner && !_wcsicmp(type, TOOLTIPS_CLASSW)) { s.found = handle; return FALSE; } return TRUE;
        }, reinterpret_cast<LPARAM>(&search)); CHECK(search.found); return search.found;
    }
    std::wstring Help(bool rectangle = false) {
        wchar_t text[8194]{}; TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V1_SIZE; tool.hwnd = native->Toolbar();
        tool.uId = rectangle ? UINT_PTR(data.element) : reinterpret_cast<UINT_PTR>(wrapper); tool.lpszText = text;
        CHECK(SendMessageW(Tooltip(), TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)));
        tool.lpszText = text; SendMessageW(Tooltip(), TTM_GETTEXTW, 8194, reinterpret_cast<LPARAM>(&tool)); return text;
    }
    void Clean(unsigned steps = 0, int value = 2) {
        CHECK(history.UndoCount() == steps && data.value.value.signedValue == value && data.edits == steps && !data.finished);
        CHECK(observation.frame == 23 && observation.dirty == bool(steps));
    }
};
void MetadataAndDraft() {
    Fixture f; f.Register(); const auto field = f.field, wrapper = f.wrapper; const auto revision = f.data.registry.Revision();
    SetRaw(field, L"-"); SendMessageW(field, EM_SETSEL, 0, 1); SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto reads = f.data.reads, releases = f.data.releases; uint64_t history = 0; CHECK(f.history.Revision(history) == MMS_OK);
    std::vector<MmsHandle> tickets; for (unsigned i = 0; i < 8; ++i) tickets.push_back(f.Submit(MMS_UI_REFRESH_STATE, true, false, 55));
    CHECK(!f.prepares); f.queue.Drain(); for (auto ticket : tickets) f.Complete(ticket);
    CHECK(f.prepares == 1 && f.data.reads == reads && f.data.releases == releases && Caption(field) == L"-" && GetFocus() == field);
    DWORD begin = 0, end = 0; SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
    CHECK(begin == 0 && end == 1 && Caption(f.labelWindow) == Wide(f.label) && f.Help() == Wide(f.description));
    MSG tab{}; tab.hwnd = field; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
    CHECK(SendMessageW(field, WM_GETDLGCODE, VK_TAB, reinterpret_cast<LPARAM>(&tab)) & DLGC_WANTTAB);
    SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0); Escape(field); CHECK(Caption(field) == L"2");
    const auto count = f.prepares, layoutReads = f.data.reads; f.Drain(MMS_UI_REFRESH_LAYOUT);
    CHECK(f.prepares == count && f.data.reads == layoutReads && Caption(f.labelWindow) == Wide(f.label) && f.field == field && GetParent(field) == wrapper);
    RECT outer{}, input{}, label{}; CHECK(GetWindowRect(wrapper, &outer) && GetWindowRect(field, &input) && GetWindowRect(f.labelWindow, &label));
    CHECK(input.left >= outer.left && input.right <= outer.right && label.left >= outer.left && label.right <= outer.right);
    uint64_t current = 0; CHECK(f.history.Revision(current) == MMS_OK && current == history && f.data.registry.Revision() == revision); f.Clean();
}
void AvailabilityAndPermissions() {
    for (unsigned restriction = 0; restriction != 3; ++restriction) {
        Fixture f; f.Register(); SetRaw(f.field, L"7"); const auto reads = f.data.reads;
        f.fields |= restriction == 0 ? MMS_UI_STATE_ENABLED : restriction == 1 ? MMS_UI_STATE_READ_ONLY : MMS_UI_STATE_VISIBLE;
        f.readOnly = restriction == 1 ? 1 : 0; f.Drain();
        CHECK(f.data.reads == reads && Caption(f.field) == L"2");
        if (restriction == 0) CHECK(!IsWindowEnabled(f.wrapper) && !IsWindowEnabled(f.field));
        if (restriction == 1) CHECK(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY);
        if (restriction == 2) CHECK(!IsWindowVisible(f.wrapper));
        f.Drain(MMS_UI_REFRESH_DATA);
        f.native->Refresh(); CHECK(f.native->Layout(10, 700, 180, true) > 0);
        if (restriction == 0) CHECK(!IsWindowEnabled(f.field));
        if (restriction == 1) CHECK(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY);
        if (restriction == 2) CHECK(!IsWindowVisible(f.wrapper));
        SetFocus(f.window.sink); Pump(); f.Clean();
        if (restriction == 0) {
            CHECK(f.Help(true) == Wide(f.description + "\n" + f.reason));
            RECT rect{}; CHECK(GetClientRect(f.wrapper, &rect)); POINT center{rect.right / 2, rect.bottom / 2};
            SetLastError(0); CHECK(MapWindowPoints(f.wrapper, f.native->Toolbar(), &center, 1) || !GetLastError());
            TTHITTESTINFOW hit{}; hit.hwnd = f.native->Toolbar(); hit.pt = center; hit.ti.cbSize = TTTOOLINFOW_V1_SIZE;
            CHECK(SendMessageW(f.Tooltip(), TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit)) && hit.ti.uId == UINT_PTR(f.data.element) && !(hit.ti.uFlags & TTF_IDISHWND));
        }
        f.fields = 63; f.enabled = f.visible = 1; f.readOnly = 0; f.Drain(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT);
        CHECK(IsWindowVisible(f.wrapper) && IsWindowEnabled(f.field) && !(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY));
        SetRaw(f.field, L"7"); Enter(f.field); f.Clean(1, 7);
    }
    for (unsigned restricted = 0; restricted != 2; ++restricted) {
        Fixture f(restricted != 0, restricted != 0); f.Register(); const auto reads = f.data.reads;
        f.fields = 63; f.enabled = f.visible = 1; f.Drain();
        CHECK(f.data.reads == reads && (GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY));
        CHECK(SetWindowTextW(f.field, L"8")); Enter(f.field); SetFocus(f.window.sink); Pump(); f.Clean();
    }
    Fixture hidden; hidden.Register(); const auto priorTarget = hidden.Target().target;
    auto hiddenDesc = hidden.Desc();
    MmsHandle layout = 0; CHECK(hidden.data.ui.beginLayout(hidden.data.ui.context, hidden.data.registry.Revision(), &layout) == MMS_OK);
    MmsLayoutOpV1 hide{}; hide.h = Header<MmsLayoutOpV1>(); hide.operation = MMS_LAYOUT_VISIBLE;
    hide.elementId = Text(hidden.data.elementId); hide.expectedGeneration = hidden.data.registry.elements.at(hidden.data.elementId).desc.generation;
    CHECK(hidden.data.ui.applyLayout(hidden.data.ui.context, layout, &hide) == MMS_OK && hidden.data.ui.commitLayout(hidden.data.ui.context, layout) == MMS_OK);
    hidden.native->Refresh(); CHECK(hidden.native->Layout(10, 700, 180, true) > 0 && !IsWindow(hidden.wrapper));
    // A user layout hide unmounts the presentation, unlike provider STATE hide.
    // The plugin cannot use an old target/provider to resurrect that view.
    MmsUiRefreshTargetV1 unavailable{}; unavailable.h = Header<MmsUiRefreshTargetV1>();
    CHECK(hidden.refresh.getTarget(hidden.refresh.context, hidden.data.element, &unavailable) == MMS_E_NOT_READY);
    CHECK(hidden.content.updateProvider(hidden.content.context, hidden.provider, &hiddenDesc) == MMS_E_STALE);
    MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>(); request.target = priorTarget;
    request.kinds = MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT; request.scope = MMS_UI_REFRESH_SELF;
    MmsHandle rejected = 779;
    CHECK(hidden.refresh.request(hidden.refresh.context, &request, &rejected) == MMS_E_STALE && rejected == 779);
    CHECK(!IsWindow(hidden.wrapper)); hidden.Clean();
}
void IdentityAndSubtree() {
    Fixture f(true, false, true); f.Register(true);
    const auto peerWrapper = Wrapper(f.native->Toolbar(), L"Peer input"); const auto reads = f.data.reads;
    std::vector<MmsHandle> tickets; for (unsigned i = 0; i < 6; ++i) tickets.push_back(f.Submit(MMS_UI_REFRESH_STATE, true, true, 77));
    f.queue.Drain(); for (auto ticket : tickets) f.Complete(ticket);
    CHECK(f.prepares == 2 && f.data.reads == reads && Caption(Label(peerWrapper)) == Wide(f.label)); f.Clean();
    auto pending = f.Submit(); auto desc = f.Desc(); CHECK(f.content.updateProvider(f.content.context, f.provider, &desc) == MMS_OK);
    f.queue.Drain(); CHECK(f.State(pending).result == MMS_E_STALE && !f.State(pending).completedKinds);
    pending = f.Submit(); const auto record = f.data.registry.properties.at(f.data.propertyId); auto property = record->desc; property.info.label = S("new exact property record");
    CHECK(f.data.ui.updateProperty(f.data.ui.context, f.data.property, &property) == MMS_OK);
    f.queue.Drain(); CHECK(f.State(pending).status == MMS_UI_REFRESH_FAILED && f.State(pending).result == MMS_E_STALE && !f.State(pending).completedKinds);
    CHECK(Caption(f.labelWindow) == Wide(f.label)); f.Clean();
}
void InvalidCandidates() {
    Fixture f; f.Register(); f.legacy = true; f.Drain(); CHECK(Caption(f.labelWindow) == Wide(f.label));
    f.legacy = false; const auto caption = Caption(f.labelWindow), help = f.Help(); const auto reads = f.data.reads;
    for (unsigned invalid = 0; invalid != 8; ++invalid) {
        f.prepare = [&](MmsHandle builder) -> MmsResult {
            auto value = f.Value();
            if (invalid == 0) value.content.reserved = 1;
            if (invalid == 1) value.reserved = 1;
            if (invalid == 2) { value.content.fields |= MMS_UI_STATE_ENABLED; value.enabled = 2; }
            if (invalid == 3) { value.content.fields |= MMS_UI_STATE_VISIBLE; value.content.h.size = 64; }
            if (invalid == 4) value.readOnly = 1;
            if (invalid == 5) value.content.fields |= 64;
            const std::string malformed("\xc0\x80", 2), oversized(4097, 'a');
            if (invalid == 6) value.content.label = Text(malformed);
            if (invalid == 7) value.content.label = Text(oversized);
            CHECK(f.content.writePrepared(f.content.context, builder, &value.content) != MMS_OK);
            return MMS_OK; // An ignored failed write still poisons its builder.
        };
        const auto ticket = f.Submit(); f.queue.Drain(); const auto state = f.State(ticket);
        CHECK(state.status == MMS_UI_REFRESH_FAILED && !state.completedKinds && Caption(f.labelWindow) == caption && f.Help() == help && f.data.reads == reads);
        CHECK(IsWindowEnabled(f.field) && IsWindowVisible(f.wrapper) && !(GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY)); f.Clean();
    }
}
struct CaptionFault {
    HWND label = nullptr; bool truncate = false; unsigned hits = 0;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR context) {
        auto& f = *reinterpret_cast<CaptionFault*>(context);
        if (message == WM_SETTEXT && lp && std::wstring_view(reinterpret_cast<const wchar_t*>(lp)) == L"Rejected input caption") {
            ++f.hits; if (!f.truncate) return FALSE;
            DefSubclassProc(hwnd, message, wp, reinterpret_cast<LPARAM>(L"Short")); return TRUE;
        }
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    ~CaptionFault() { if (IsWindow(label)) RemoveWindowSubclass(label, Proc, UINT_PTR(this)); }
};
void CaptionFailuresAndFallback() {
    for (bool truncate : {false, true}) {
        Fixture f; f.Register(); f.Drain(); const auto old = Caption(f.labelWindow), help = f.Help();
        CaptionFault fault; fault.label = f.labelWindow; fault.truncate = truncate;
        CHECK(SetWindowSubclass(fault.label, CaptionFault::Proc, UINT_PTR(&fault), DWORD_PTR(&fault)));
        f.label = "Rejected input caption"; const auto reads = f.data.reads; auto ticket = f.Submit(); f.queue.Drain();
        CHECK(f.State(ticket).status == MMS_UI_REFRESH_FAILED && f.State(ticket).result == MMS_E_FAILED && !f.State(ticket).completedKinds && fault.hits == 1);
        CHECK(Caption(f.labelWindow) == old && f.Help() == help && f.data.reads == reads); f.Clean();
        CHECK(RemoveWindowSubclass(fault.label, CaptionFault::Proc, UINT_PTR(&fault)));
        const auto prepares = f.prepares; f.Drain(MMS_UI_REFRESH_LAYOUT);
        CHECK(Caption(f.labelWindow) == old && f.prepares == prepares && f.data.reads == reads);
    }
    for (bool truncate : {false, true}) for (bool initiallyVisible : {true, false}) {
        Fixture f; f.Register(); f.fields = 63; f.enabled = 0; f.visible = initiallyVisible; f.Drain();
        const auto old = Caption(f.labelWindow), help = f.Help(true); const auto reads = f.data.reads;
        const auto rectangle = [&] {
            TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V1_SIZE; tool.hwnd = f.native->Toolbar();
            tool.uId = UINT_PTR(f.data.element); wchar_t text[8194]{}; tool.lpszText = text;
            CHECK(SendMessageW(f.Tooltip(), TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)));
            CHECK(!(tool.uFlags & TTF_IDISHWND)); return tool.rect;
        };
        const auto hit = [&] {
            RECT client{}; CHECK(GetClientRect(f.wrapper, &client)); POINT center{client.right / 2, client.bottom / 2};
            SetLastError(0); CHECK(MapWindowPoints(f.wrapper, f.native->Toolbar(), &center, 1) || !GetLastError());
            TTHITTESTINFOW value{}; value.hwnd = f.native->Toolbar(); value.pt = center; value.ti.cbSize = TTTOOLINFOW_V1_SIZE;
            const bool found = SendMessageW(f.Tooltip(), TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&value)) != FALSE;
            if (found) CHECK(value.ti.uId == UINT_PTR(f.data.element) && !(value.ti.uFlags & TTF_IDISHWND));
            return found;
        };
        const RECT before = rectangle();
        CHECK(!IsWindowEnabled(f.wrapper) && bool(IsWindowVisible(f.wrapper)) == initiallyVisible);
        CHECK(bool(IsRectEmpty(&before)) != initiallyVisible && hit() == initiallyVisible);
        CHECK(help == Wide(f.description + "\n" + f.reason));
        CaptionFault fault; fault.label = f.labelWindow; fault.truncate = truncate;
        CHECK(SetWindowSubclass(fault.label, CaptionFault::Proc, UINT_PTR(&fault), DWORD_PTR(&fault)));
        f.label = "Rejected input caption"; f.visible = !initiallyVisible;
        const auto ticket = f.Submit(); f.queue.Drain(); const auto state = f.State(ticket);
        CHECK(state.status == MMS_UI_REFRESH_FAILED && state.result == MMS_E_FAILED && !state.completedKinds && fault.hits == 1);
        const RECT after = rectangle();
        CHECK(EqualRect(&before, &after) && hit() == initiallyVisible);
        CHECK(Caption(f.labelWindow) == old && f.Help(true) == help && f.data.reads == reads);
        CHECK(!IsWindowEnabled(f.wrapper) && bool(IsWindowVisible(f.wrapper)) == initiallyVisible); f.Clean();
        CHECK(RemoveWindowSubclass(fault.label, CaptionFault::Proc, UINT_PTR(&fault)));
    }
    Fixture f(true, false, false, true); f.Register(); f.Drain(); f.fields = 0; f.legacy = true; f.Drain();
    CHECK(Caption(f.labelWindow) == L"Input" && f.Help().empty());
    f.fields = 7; f.Drain(); CHECK(Caption(f.labelWindow) == Wide(f.label));
    CHECK(f.content.unregisterProvider(f.content.context, f.provider) == MMS_OK); f.provider = 0; const auto reads = f.data.reads;
    f.Drain(); CHECK(Caption(f.labelWindow) == L"Input" && f.Help().empty() && f.data.reads == reads);
    SetRaw(f.field, L"7"); const auto pending = f.Submit(); f.queue.Retire(f.client); f.data.registry.Retire(f.data.owner);
    f.native->Refresh(); Pump(); CHECK(!IsWindow(f.wrapper) && f.State(pending).status == MMS_UI_REFRESH_CANCELLED); f.Clean();
}
} // namespace input_state_content_checks
static void InputStateContentChecks() {
    std::cout << "RUN INPUT STATE metadata/draft\n";
    input_state_content_checks::MetadataAndDraft();
    std::cout << "RUN INPUT STATE availability\n";
    input_state_content_checks::AvailabilityAndPermissions();
    std::cout << "RUN INPUT STATE identity/subtree\n";
    input_state_content_checks::IdentityAndSubtree();
    std::cout << "RUN INPUT STATE invalid candidates\n";
    input_state_content_checks::InvalidCandidates();
    std::cout << "RUN INPUT STATE caption failure/fallback\n";
    input_state_content_checks::CaptionFailuresAndFallback();
    std::cout << "INPUT STATE native metadata/draft/permissions/identity/failure/tooltip PASS\n";
}
