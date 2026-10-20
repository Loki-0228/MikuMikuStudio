// Include after input_state_content_checks.inc. All views, registrations,
// codecs, STATE/DATA adapters and tickets below are production objects.
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/viewport_panel_layout.hpp"
#include "mikumikustudio/viewport_panel_layout.hpp"
#include <mms/cpp/ui.hpp>
#include <mms/cpp/refresh.hpp>

namespace property_mount_checks {
using namespace mikumikustudio;
namespace sdk = mms::sdk;
using native_property_input_checks::Caption;
using native_property_input_checks::Enter;
using native_property_input_checks::Escape;
using native_property_input_checks::FindField;
using native_property_input_checks::Pump;
constexpr const char* anchor = "mms.properties.main.extensions";
struct Fixture : input_state_content_checks::Fixture {
    const std::string groupId = "example.input.property-group";
    const std::string topId = "example.input.top-peer";
    std::unique_ptr<sdk::RegistrationBundle> sdkControls;
    sdk::RegistrationRef top;
    std::vector<MmsHandle> commands, buttons;
    MmsHandle separator = 0;
    HWND topField = nullptr;
    unsigned commandStates = 0;
    bool commandVisible = true;
    explicit Fixture(bool extras = true, bool editable = true): input_state_content_checks::Fixture(editable) {
        MmsUiElementV1 container{}; container.h = Header<MmsUiElementV1>(); container.id = Text(groupId);
        container.ownerPlugin = Text(data.owner.id); container.parentId = S(anchor); container.kind = MMS_UI_GROUP;
        container.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT | MMS_UI_MOVE | MMS_UI_HIDE;
        CHECK(data.ui.registerElement(data.ui.context, &container, &group) == MMS_OK);
        auto hints = data.registry.LayoutApi(data.owner); MmsUiLayoutHintsV1 groupHints{}; groupHints.h = Header<MmsUiLayoutHintsV1>();
        groupHints.flow = MMS_FLOW_WRAP; groupHints.grow[0] = groupHints.shrink[0] = 1;
        CHECK(hints.setDefaultHints(hints.context, Text(groupId), &groupHints) == MMS_OK);
        auto element = data.registry.elements.at(data.elementId).desc; element.parentId = Text(groupId);
        CHECK(data.ui.updateElement(data.ui.context, data.element, &element) == MMS_OK);
        MmsUiLayoutHintsV1 inputHints{}; inputHints.h = Header<MmsUiLayoutHintsV1>();
        inputHints.minimum[0] = 90; inputHints.preferred[0] = 240; inputHints.preferred[1] = 28;
        inputHints.grow[0] = inputHints.shrink[0] = 1;
        CHECK(hints.setDefaultHints(hints.context, Text(data.elementId), &inputHints) == MMS_OK);
        sdk::UiServices services; CHECK(sdk::UiServices::Query(Host(), services) == MMS_OK);
        sdkControls = std::make_unique<sdk::RegistrationBundle>(services);
        sdk::InputSpec spec{topId, data.owner.id, "Top input", "mms.toolbar.main.extensions", data.propertyId, &inputHints};
        CHECK(sdkControls->AddInput(spec, &top) == MMS_OK);
        if (extras) {
            for (unsigned i = 0; i != 2; ++i) {
                const std::string id = "example.input.property-button-" + std::to_string(i), commandId = id + ".command";
                MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = Text(commandId);
                command.info.ownerPlugin = Text(data.owner.id); command.info.label = S("Property command"); command.context = this;
                command.getState = [](void* p, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
                    auto& f = *static_cast<Fixture*>(p); ++f.commandStates;
                    *out = {Header<MmsCommandStateV1>(), 1, 0, f.commandVisible ? 1u : 0u, 0, {}}; return MMS_OK;
                };
                command.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
                MmsHandle handle = 0; CHECK(data.ui.registerCommand(data.ui.context, &command, &handle) == MMS_OK); commands.push_back(handle);
                element = {}; element.h = Header<MmsUiElementV1>(); element.id = Text(id); element.ownerPlugin = Text(data.owner.id);
                element.parentId = Text(groupId); element.label = S("Property command"); element.commandId = Text(commandId);
                element.kind = MMS_UI_BUTTON; element.capabilities = MMS_UI_LAYOUT | MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE;
                CHECK(data.ui.registerElement(data.ui.context, &element, &handle) == MMS_OK); buttons.push_back(handle);
                MmsUiLayoutHintsV1 fixed{}; fixed.h = Header<MmsUiLayoutHintsV1>();
                fixed.minimum[0] = fixed.preferred[0] = fixed.maximum[0] = 100;
                fixed.minimum[1] = fixed.preferred[1] = fixed.maximum[1] = 24;
                CHECK(hints.setDefaultHints(hints.context, Text(id), &fixed) == MMS_OK);
            }
            element = {}; element.h = Header<MmsUiElementV1>(); element.id = S("example.input.property-separator");
            element.ownerPlugin = Text(data.owner.id); element.parentId = Text(groupId); element.kind = MMS_UI_SEPARATOR;
            element.capabilities = MMS_UI_LAYOUT | MMS_UI_MOVE | MMS_UI_HIDE;
            CHECK(data.ui.registerElement(data.ui.context, &element, &separator) == MMS_OK);
        }
        Arrange(); Bind();
    }
    MmsHostV1 Host() {
        MmsHostV1 host{}; host.h = Header<MmsHostV1>(); host.context = this;
        host.queryService = [](void* p, MmsString name, uint32_t major, uint32_t minor, uint32_t bytes, void* out) -> MmsResult {
            auto& f = *static_cast<Fixture*>(p); const auto id = String(name);
            if (id == "mms.ui") return Table(major, minor, bytes, out, f.data.ui);
            if (id == "mms.ui.layout") return Table(major, minor, bytes, out, f.data.registry.LayoutApi(f.data.owner));
            if (id == "mms.ui.surface") return Table(major, minor, bytes, out, f.data.registry.SurfaceApi(f.data.owner));
            if (id == "mms.ui.refresh") return Table(major, minor, bytes, out, f.refresh);
            return MMS_E_NOT_FOUND;
        }; return host;
    }
    void Arrange(int width = 700, bool refreshViews = true, int capacity = 180) {
        RECT client{}; CHECK(GetClientRect(window.value, &client));
        native->SetPropertyArea(std::max(0, int(client.bottom) - 160), capacity, true);
        if (refreshViews) native->Refresh();
        CHECK(native->Layout(10, width, 180, true) > 0);
    }
    void Bind() {
        field = FindField(native->PropertyArea()); wrapper = GetParent(field); labelWindow = input_state_content_checks::Label(wrapper);
        topField = FindField(native->Toolbar());
        CHECK(GetParent(wrapper) == native->PropertyArea() && GetParent(GetParent(topField)) == native->Toolbar());
    }
    MmsUiRefreshTargetV1 AnyTarget(MmsHandle handle) {
        MmsUiRefreshTargetV1 target{}; target.h = Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context, handle, &target) == MMS_OK); return target;
    }
    static RECT In(HWND parent, HWND child) {
        RECT rect{}; CHECK(GetWindowRect(child, &rect)); SetLastError(ERROR_SUCCESS);
        CHECK(MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2) || GetLastError() == ERROR_SUCCESS); return rect;
    }
    void Bounds() {
        const auto area = In(window.value, native->PropertyArea()), upper = In(window.value, native->Toolbar());
        RECT client{}; CHECK(GetClientRect(window.value, &client));
        CHECK(area.left == 10 && area.bottom == client.bottom - 160 && area.top >= upper.bottom && area.right <= client.right);
        CHECK(area.bottom - area.top == native->PropertyAreaHeight());
        for (auto child = GetWindow(native->PropertyArea(), GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) if (IsWindowVisible(child)) {
            const auto box = In(native->PropertyArea(), child); RECT extent{}; CHECK(GetClientRect(native->PropertyArea(), &extent));
            CHECK(box.left >= 0 && box.top >= 0 && box.right <= extent.right && box.bottom <= extent.bottom);
        }
        using GetDpi = UINT(WINAPI*)(HWND);
        const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        HDC dc = getDpi ? nullptr : GetDC(window.value);
        const float scale = 96.0f / (getDpi ? getDpi(window.value) : dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96);
        if (dc) ReleaseDC(window.value, dc);
        auto layout = data.registry.LayoutApi(data.owner); MmsUiLayoutSnapshotV1 snapshot{}; snapshot.h = Header<MmsUiLayoutSnapshotV1>();
        CHECK(layout.acquireSnapshot(layout.context, &snapshot) == MMS_OK); unsigned matched = 0;
        const RECT input = In(window.value, wrapper);
        for (uint64_t i = 0; i < snapshot.items.count; ++i) {
            const auto& item = *reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data) + size_t(i * snapshot.items.stride));
            if (String(item.elementId) != data.elementId && String(item.elementId) != anchor) continue;
            const RECT expected = String(item.elementId) == data.elementId ? input : area; ++matched;
            CHECK(std::abs(item.rect[0] - expected.left * scale) < 1 && std::abs(item.rect[1] - expected.top * scale) < 1);
            CHECK(std::abs(item.rect[2] - (expected.right - expected.left) * scale) < 1 && std::abs(item.rect[3] - (expected.bottom - expected.top) * scale) < 1);
        }
        layout.releaseSnapshot(layout.context, snapshot.handle); CHECK(matched == 2);
    }
};
void AnchorsAndIdentity() {
    Fixture f; auto surfaces = f.data.registry.SurfaceApi(f.data.owner); MmsSpan anchors{}; MmsHandle lease = 0;
    CHECK(surfaces.acquireAnchors(surfaces.context, &anchors, &lease) == MMS_OK);
    unsigned found = 0;
    for (uint64_t i = 0; i < anchors.count; ++i) {
        const auto& item = *reinterpret_cast<const MmsUiAnchorV1*>(static_cast<const unsigned char*>(anchors.data) + size_t(i * anchors.stride));
        if (String(item.id) == anchor) { ++found; CHECK(item.allowedKinds == ((1ull << MMS_UI_BUTTON) | (1ull << MMS_UI_GROUP) | (1ull << MMS_UI_SEPARATOR) | (1ull << MMS_UI_INPUT))); }
    }
    surfaces.releaseAnchors(surfaces.context, lease); CHECK(found == 1);
    CHECK(f.top.handle == f.data.registry.elements.at(f.topId).handle && f.data.registry.properties.size() == 1);
    CHECK(f.data.registry.elements.at(f.data.elementId).handle == f.data.element && String(f.data.registry.elements.at(f.data.elementId).desc.parentId) == f.groupId);
    CHECK(f.AnyTarget(f.data.element).target.element == f.data.element && f.AnyTarget(f.top.handle).target.element == f.top.handle);
    CHECK(f.AnyTarget(f.group).supportedScopes == MMS_UI_REFRESH_SUBTREE);
    CHECK((f.AnyTarget(f.data.element).supportedKinds & 15u) == 15u);
    CHECK(f.AnyTarget(f.separator).supportedKinds == (MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_LAYOUT));
    MmsUiLayoutHintsV1 menu{}; menu.h = Header<MmsUiLayoutHintsV1>(); menu.overflow = MMS_UI_OVERFLOW_MENU;
    auto layout = f.data.registry.LayoutApi(f.data.owner);
    CHECK(layout.setDefaultHints(layout.context, S("example.input.property-button-0"), &menu) == MMS_E_UNSUPPORTED);
    f.Bounds(); f.Clean();
    const int wide = f.native->PropertyAreaHeight(); const HWND field = f.field, upper = f.topField;
    f.Arrange(280, false); CHECK(f.native->PropertyAreaHeight() > wide); f.Bounds();
    f.Arrange(700, false); CHECK(f.native->PropertyAreaHeight() == wide && FindField(f.native->PropertyArea()) == field && FindField(f.native->Toolbar()) == upper); f.Bounds();
    CHECK(Caption(f.field) == L"2" && Caption(f.topField) == L"2"); f.Clean();
}
void SharedRefreshAndDraft() {
    Fixture f; f.Register(); const auto field = f.field;
    native_property_input_checks::Text(field, L"-"); SendMessageW(field, EM_SETSEL, 0, 1); SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto reads = f.data.reads;
    f.Drain(); CHECK(f.data.reads == reads && Caption(field) == L"-" && Caption(f.labelWindow) == Wide(f.label) && GetFocus() == field);
    DWORD begin = 0, end = 0; SendMessageW(field, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end)); CHECK(begin == 0 && end == 1);
    SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0); Escape(field);
    f.data.value = ui_input_broker_checks::Integer(9);
    const auto ticket = f.Submit(15, true, true, 801); CHECK(f.State(ticket).status == MMS_UI_REFRESH_PENDING);
    CHECK(f.data.reads == reads && Caption(field) == L"2"); f.queue.Drain(); f.Complete(ticket, 15);
    CHECK(f.data.reads == reads + 1 && Caption(field) == L"9" && Caption(f.topField) == L"2" && f.field == field); f.Clean(0, 9);
    sdk::RefreshServices service; CHECK(sdk::RefreshServices::Query(f.Host(), service) == MMS_OK); sdk::RefreshTicket owned;
    CHECK(service.RefreshPropertyElementCurrent(f.top.handle, owned) == MMS_OK); const auto handle = owned.get(); f.queue.Drain();
    MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>();
    CHECK(f.refresh.query(f.refresh.context, handle, &state) == MMS_OK && state.status == MMS_UI_REFRESH_COMPLETED && state.completedKinds == 3);
    CHECK(Caption(f.topField) == L"9" && owned.Release() == MMS_OK); f.Clean(0, 9);
    native_property_input_checks::Text(field, L"7"); f.data.value = ui_input_broker_checks::Integer(8); f.Drain(MMS_UI_REFRESH_DATA);
    CHECK(Caption(field) == L"7"); Enter(field); CHECK(Caption(field) == L"8"); f.Clean(0, 8);
}
void CollapsePermissionsAndUnmount() {
    Fixture f(false); f.Register(); const auto target = f.Target().target; const auto field = f.field;
    native_property_input_checks::Text(field, L"7"); f.fields = 63; f.enabled = 1; f.visible = 0;
    f.Drain(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT);
    CHECK(!IsWindowVisible(f.wrapper) && f.native->PropertyAreaHeight() == 0 && Caption(field) == L"2");
    CHECK(GetFocus() != field && !f.Target().ready && f.Target().target.instanceGeneration == target.instanceGeneration); f.Clean();
    f.visible = 1; f.Drain(MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_LAYOUT);
    CHECK(IsWindowVisible(field) && f.native->PropertyAreaHeight() > 0 && f.field == field); f.Bounds();
    native_property_input_checks::Text(field, L"7"); f.enabled = 0; f.Drain();
    CHECK(!IsWindowEnabled(field) && Caption(field) == L"2"); f.Clean();
    f.enabled = 1; f.Drain(); CHECK(IsWindowEnabled(field));
    native_property_input_checks::Text(field, L"7");
    CHECK(f.sdkControls->Close() == MMS_OK); const auto pending = f.Submit();
    f.data.registry.Retire(f.data.owner); f.native->Refresh(); Pump();
    CHECK(!IsWindow(field) && f.native->PropertyAreaHeight() == 0 && GetFocus() != field);
    CHECK(f.State(pending).status != MMS_UI_REFRESH_COMPLETED && !f.State(pending).completedKinds); f.Clean();
    Fixture readonly(false, false);
    CHECK(GetWindowLongPtrW(readonly.field, GWL_STYLE) & ES_READONLY);
    CHECK(SetWindowTextW(readonly.field, L"7")); Enter(readonly.field); SetFocus(readonly.window.sink); Pump(); readonly.Clean();
}
void CrossMountIdentity() {
    Fixture f(false); const auto record = f.data.registry.properties.at(f.data.propertyId);
    const auto original = f.Target().target; const HWND old = f.field, top = f.topField;
    const int controlId = GetDlgCtrlID(f.wrapper);
    native_property_input_checks::Text(old, L"7"); const auto pending = f.Submit();
    const auto move = [&](const char* parent) {
        MmsHandle transaction = 0; CHECK(f.data.ui.beginLayout(f.data.ui.context, f.data.registry.Revision(), &transaction) == MMS_OK);
        MmsLayoutOpV1 operation{}; operation.h = Header<MmsLayoutOpV1>(); operation.operation = MMS_LAYOUT_MOVE;
        operation.elementId = Text(f.groupId); operation.parentId = S(parent);
        operation.expectedGeneration = f.data.registry.elements.at(f.groupId).desc.generation;
        CHECK(f.data.ui.applyLayout(f.data.ui.context, transaction, &operation) == MMS_OK);
        CHECK(f.data.ui.commitLayout(f.data.ui.context, transaction) == MMS_OK); f.Arrange();
    };
    move("mms.toolbar.main.extensions");
    CHECK(!IsWindow(old) && IsWindow(top) && f.native->PropertyAreaHeight() == 0 && GetFocus() != old);
    const HWND movedWrapper = GetDlgItem(f.native->Toolbar(), controlId), movedField = GetDlgItem(movedWrapper, 0x7201);
    CHECK(movedWrapper && movedField && Caption(movedField) == L"2");
    CHECK(f.Target().target.element == original.element && f.Target().target.instanceGeneration != original.instanceGeneration);
    CHECK(f.data.registry.properties.at(f.data.propertyId) == record && f.State(pending).status == MMS_UI_REFRESH_FAILED && !f.State(pending).completedKinds); f.Clean();
    native_property_input_checks::Text(movedField, L"7"); move(anchor); f.Bind();
    CHECK(!IsWindow(movedField) && Caption(f.field) == L"2" && f.data.registry.elements.at(f.data.elementId).handle == original.element); f.Clean();
    native_property_input_checks::Text(f.field, L"7"); Enter(f.field); f.Clean(1, 7);
}
void EmptyMountUnchanged() {
    native_property_input_checks::Window window; UiRegistry registry;
    const auto before = Fixture::In(window.value, window.sink); NativeUi native(registry, window.value, Log{});
    RECT client{}; CHECK(GetClientRect(window.value, &client)); native.SetPropertyArea(std::max(0, int(client.bottom) - 160), 180, true);
    native.Refresh(); CHECK(native.Layout(10, 700, 180, true) == 0);
    const auto after = Fixture::In(window.value, window.sink);
    CHECK(native.PropertyAreaHeight() == 0 && !IsWindowVisible(native.PropertyArea()) && EqualRect(&before, &after));
}
void ViewportReservation() {
    const RECT client{0, 0, 1280, 900}; FrameViewportGeometry before, zero, reserved;
    CHECK(CalculateFrameViewport(client, 280, 48, FrameViewportMode::Docked, 1920, 1080, before));
    CHECK(CalculateFrameViewport(client, 280, 48, FrameViewportMode::Docked, 1920, 1080, zero, 0));
    CHECK(EqualRect(&before.available, &zero.available) && EqualRect(&before.scene, &zero.scene) && EqualRect(&before.presentationBounds, &zero.presentationBounds));
    CHECK(CalculateFrameViewport(client, 280, 48, FrameViewportMode::Docked, 1920, 1080, reserved, 72));
    CHECK(reserved.available.bottom == before.available.bottom - 72 && reserved.presentationBounds.bottom == client.bottom - 160 - 72);
    CHECK(FrameRectWithin(reserved.scene, reserved.available) && reserved.available.left == before.available.left && reserved.available.top == before.available.top);
    const auto panels = MakeViewportPanelLayout(reserved.available, reserved.scene, true, true);
    CHECK(panels.bottomBand.bottom <= client.bottom - 160 - 72 && FrameRectWithin(panels.overlay, reserved.presentationBounds));
    for (const auto mode : {FrameViewportMode::Fullscreen, FrameViewportMode::Floating}) {
        CHECK(CalculateFrameViewport(client, 280, 48, mode, 1920, 1080, before));
        CHECK(CalculateFrameViewport(client, 280, 48, mode, 1920, 1080, reserved, 72));
        CHECK(EqualRect(&before.available, &reserved.available) && EqualRect(&before.scene, &reserved.scene));
    }
    CHECK(!CalculateFrameViewport({0,0,900,260}, 280, 48, FrameViewportMode::Docked, 1920,1080,reserved,72));
    CHECK(!reserved.scene.right && !reserved.available.bottom);
}
} // namespace property_mount_checks
static void PropertyMountChecks() {
    property_mount_checks::AnchorsAndIdentity(); property_mount_checks::SharedRefreshAndDraft();
    property_mount_checks::CollapsePermissionsAndUnmount(); property_mount_checks::CrossMountIdentity();
    property_mount_checks::EmptyMountUnchanged(); property_mount_checks::ViewportReservation();
    std::cout << "Property mount original objects/native layout/refresh/draft/unload/viewport PASS\n";
}
