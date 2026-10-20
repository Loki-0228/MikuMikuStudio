namespace native_property_data_checks {
using native_property_input_checks::Caption;
using native_property_input_checks::Enter;
using native_property_input_checks::Escape;
using native_property_input_checks::FindField;
using native_property_input_checks::Fixture;
using native_property_input_checks::Text;
using native_property_input_checks::Window;
void Canonical(Fixture& f, int value) {
    StoredData data; data.schema = 1; data.bytes.resize(sizeof value); std::memcpy(data.bytes.data(), &value, sizeof value);
    StoredRecords records; records.emplace(f.id, std::move(data));
    CHECK(f.edit.manager->Storage().PrepareRestore(std::move(records)) == MMS_OK);
    CHECK(f.edit.manager->Storage().Commit() && f.probe.Value() == value);
}
struct Queue {
    UiRefreshQueue& queue;
    RefreshClient client;
    MmsUiRefreshApiV1 api;
    MmsHandle element = 0;
    Queue(UiRegistry& registry, const std::string& owner, MmsHandle value): queue(registry.Refresh()), client{&queue, owner}, api(queue.Api(client)), element(value) {}
    MmsUiRefreshTargetV1 Target() {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(api.getTarget(api.context, element, &target) == MMS_OK && target.ready);
        CHECK(target.supportedKinds & MMS_UI_REFRESH_DATA); return target;
    }
    MmsHandle Request(bool worker = false, uint32_t scope = MMS_UI_REFRESH_SELF, uint32_t kinds = MMS_UI_REFRESH_DATA) {
        const auto target = Target(); MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>();
        request.target = target.target; request.scope = scope; request.kinds = kinds; MmsHandle ticket = 0; MmsResult result = MMS_E_FAILED;
        if (worker) { std::thread caller([&] { result = api.request(api.context, &request, &ticket); }); caller.join(); }
        else result = api.request(api.context, &request, &ticket);
        CHECK(result == MMS_OK && ticket); return ticket;
    }
    MmsUiRefreshStateV1 Drain(MmsHandle ticket) {
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>();
        CHECK(api.query(api.context, ticket, &state) == MMS_OK && state.status == MMS_UI_REFRESH_PENDING);
        queue.Drain(); CHECK(api.query(api.context, ticket, &state) == MMS_OK && state.status != MMS_UI_REFRESH_PENDING);
        CHECK(!state.sourceRevision); CHECK(api.release(api.context, ticket) == MMS_OK); return state;
    }
};
void CurrentAndDraft(const fs::path& root, const fs::path& dll) {
    Fixture f(root / "native-property-data-current", dll); Queue q(f.edit.manager->Ui(), f.id, f.element);
    const auto initial = q.Target(); const auto revision = f.edit.Revision(); const auto field = f.field, wrapper = f.wrapper;
    const auto ticket = q.Request(true); Canonical(f, 5); CHECK(Caption(field) == L"0");
    auto state = q.Drain(ticket); CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == MMS_UI_REFRESH_DATA);
    CHECK(Caption(field) == L"5" && GetParent(field) == wrapper && FindField(wrapper) == field);
    f.Clean(0, 5, 0); CHECK(f.edit.Revision() == revision && !f.edit.observations->published);
    const auto after = q.Target(); CHECK(after.target.instanceGeneration == initial.target.instanceGeneration && after.geometryRevision == initial.geometryRevision);
    Text(field, L"7"); SendMessageW(field, EM_SETSEL, 0, 1); Canonical(f, 6);
    state = q.Drain(q.Request(false, MMS_UI_REFRESH_SUBTREE, MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT));
    CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == (MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_LAYOUT | MMS_UI_REFRESH_REPAINT));
    DWORD begin = 0, end = 0; SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
    CHECK(Caption(field) == L"7" && GetFocus() == field && begin == 0 && end == 1);
    Enter(field); f.Clean(0, 6, 0); CHECK(Caption(field) == L"6");
    Text(field, L"-"); Canonical(f, 8); CHECK(q.Drain(q.Request()).status == MMS_UI_REFRESH_COMPLETED);
    CHECK(Caption(field) == L"-"); Escape(field); CHECK(Caption(field) == L"8"); f.Clean(0, 8, 0);
    Text(field, L"9"); Enter(field); f.Clean(1, 9, 1); f.Close();
}
void FailuresAndIdentity(const fs::path& root, const fs::path& dll) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture f(root / ("native-property-data-failure-" + std::to_string(mode)), dll);
        native_property_input_checks::TransientReadProbe probe; const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable");
        probe.original = record->desc; auto desc = record->desc; desc.read = native_property_input_checks::TransientReadProbe::Read;
        auto api = f.OwnerApi(); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK); f.Refresh();
        Queue q(f.edit.manager->Ui(), f.id, f.element); Text(f.field, L"7"); Canonical(f, 5); probe.Arm(mode);
        const auto state = q.Drain(q.Request(true)); CHECK(state.status == MMS_UI_REFRESH_FAILED && !state.completedKinds && probe.failures == 1);
        CHECK(Caption(f.field) == L"7"); f.Clean(0, 5, 0);
        CHECK(q.Drain(q.Request()).status == MMS_UI_REFRESH_COMPLETED && Caption(f.field) == L"7");
        Escape(f.field); CHECK(Caption(f.field) == L"5");
        const auto stale = q.Request(); desc.info.label = S("new exact property record");
        CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK);
        auto old = MmsUiRefreshStateV1{}; old.h = Header<MmsUiRefreshStateV1>(); q.queue.Drain();
        CHECK(q.api.query(q.api.context, stale, &old) == MMS_OK && old.status == MMS_UI_REFRESH_FAILED && old.result == MMS_E_STALE && !old.completedKinds);
        CHECK(q.api.release(q.api.context, stale) == MMS_OK); f.Refresh();
        CHECK(q.Drain(q.Request()).status == MMS_UI_REFRESH_COMPLETED); f.Clean(0, 5, 0); f.Close();
    }
    Fixture f(root / "native-property-data-nohost", dll, false); Queue q(f.edit.manager->Ui(), f.id, f.element);
    Canonical(f, 4); CHECK(q.Drain(q.Request(true)).status == MMS_UI_REFRESH_COMPLETED);
    CHECK(Caption(f.field) == L"4" && (GetWindowLongPtrW(f.field, GWL_STYLE) & ES_READONLY) && !f.probe.PropertyCalls()); f.Close();
}
struct CaptionFault {
    HWND x = nullptr, y = nullptr;
    bool failRestore = false, applied = false, failed = false;
    static LRESULT CALLBACK Proc(HWND field, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR context) {
        auto& self = *reinterpret_cast<CaptionFault*>(context);
        if (message == WM_SETTEXT && lp) {
            const std::wstring_view text(reinterpret_cast<const wchar_t*>(lp));
            if (field == self.x && text == L"3") self.applied = true;
            if (field == self.y && text == L"4" && !self.failed) { self.failed = true; return FALSE; }
            if (field == self.x && text == L"1" && self.failRestore && self.applied) return FALSE;
        }
        return DefSubclassProc(field, message, wp, lp);
    }
    void Hook() { CHECK(SetWindowSubclass(x, Proc, UINT_PTR(this), DWORD_PTR(this))); CHECK(SetWindowSubclass(y, Proc, UINT_PTR(this), DWORD_PTR(this))); }
    ~CaptionFault() { if (IsWindow(x)) RemoveWindowSubclass(x, Proc, UINT_PTR(this)); if (IsWindow(y)) RemoveWindowSubclass(y, Proc, UINT_PTR(this)); }
};
void InitialFailure(const fs::path& root, const fs::path& dll) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture f(root / ("native-property-data-initial-" + std::to_string(mode)), dll); f.native.reset();
        native_property_input_checks::TransientReadProbe probe; const auto record = f.edit.manager->Ui().properties.at(f.id + ".editable");
        probe.original = record->desc; auto desc = record->desc; desc.read = native_property_input_checks::TransientReadProbe::Read;
        auto api = f.OwnerApi(); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK); Canonical(f, 5); probe.Arm(mode);
        f.native = std::make_unique<NativeUi>(f.edit.manager->Ui(), f.window.value, Log{}); f.Refresh();
        f.field = FindField(f.native->Toolbar()); f.wrapper = GetParent(f.field); const auto field = f.field;
        RECT before{}, after{}; CHECK(GetWindowRect(field, &before));
        CHECK(probe.failures == 1 && Caption(field).empty() && (GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY));
        Queue q(f.edit.manager->Ui(), f.id, f.element); const auto state = q.Drain(q.Request(true));
        CHECK(state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == MMS_UI_REFRESH_DATA && Caption(field) == L"5");
        CHECK(FindField(f.wrapper) == field && GetWindowRect(field, &after) && EqualRect(&before, &after) && !(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY));
        f.Clean(0, 5, 0); Text(field, L"7"); Enter(field); f.Clean(1, 7, 1); f.Close();
    }
}
void VectorRollback() {
    for (bool failRestore : {false, true}) {
        ui_input_broker_checks::Fixture f; Window window;
        CHECK(f.ui.unregisterItem(f.ui.context, f.element) == MMS_OK && f.ui.unregisterItem(f.ui.context, f.property) == MMS_OK);
        f.value = {}; f.value.type = MMS_VALUE_VECTOR; f.value.components = 2; f.value.value.vector[0] = 1; f.value.value.vector[1] = 2;
        auto property = f.desc; property.info.valueType = MMS_VALUE_VECTOR; property.info.components = 2; property.info.minimum = {}; property.info.maximum = {};
        CHECK(f.ui.registerProperty(f.ui.context, &property, &f.property) == MMS_OK);
        auto element = ui_input_broker_checks::Input(f.elementId, f.owner.id, f.propertyId);
        CHECK(f.ui.registerElement(f.ui.context, &element, &f.element) == MMS_OK); f.Bridges();
        NativeUi native(f.registry, window.value, {}); native.Refresh(); CHECK(native.Layout(10, 500, 180, true) > 0);
        CaptionFault fault; fault.x = FindField(native.Toolbar()); fault.y = FindField(native.Toolbar(), 0x7202); fault.failRestore = failRestore; fault.Hook();
        f.value.value.vector[0] = 3; f.value.value.vector[1] = 4; Queue q(f.registry, f.owner.id, f.element);
        const auto state = q.Drain(q.Request());
        if (state.status != MMS_UI_REFRESH_FAILED || state.result != MMS_E_FAILED || !fault.applied || !fault.failed)
            std::cerr << "property DATA rollback diagnostic status=" << state.status << " result=" << state.result << " completed=" << state.completedKinds
                << " applied=" << fault.applied << " failed=" << fault.failed << " x=" << Utf8(Caption(fault.x)) << " y=" << Utf8(Caption(fault.y)) << "\n";
        CHECK(state.status == MMS_UI_REFRESH_FAILED && state.result == MMS_E_FAILED && !state.completedKinds && fault.applied && fault.failed && !f.edits);
        CHECK(Caption(fault.y) == L"2");
        if (failRestore) {
            MmsUiRefreshTargetV1 paused{}; paused.h = Header<MmsUiRefreshTargetV1>();
            CHECK(q.api.getTarget(q.api.context, f.element, &paused) == MMS_OK && !paused.ready);
        } else {
            CHECK(Caption(fault.x) == L"1" && q.Target().ready);
            CHECK(q.Drain(q.Request()).status == MMS_UI_REFRESH_COMPLETED && Caption(fault.x) == L"3" && Caption(fault.y) == L"4");
        }
    }
}
} // namespace native_property_data_checks
static void NativePropertyDataChecks(const fs::path& root, const fs::path& dll) {
    native_property_data_checks::CurrentAndDraft(root, dll);
    native_property_data_checks::FailuresAndIdentity(root, dll);
    native_property_data_checks::VectorRollback();
    native_property_data_checks::InitialFailure(root, dll);
    std::cout << "native property DATA current/draft/rollback/identity/nohost PASS\n";
}
