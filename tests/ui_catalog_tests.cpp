// MAINTENANCE: independent catalog backend contract. Link the production
// UiRegistry/UiCatalog/UiRefresh objects; do not include their .cpp files here.
// These tests never enumerate another process, invoke a menu, synthesize global
// input, run a scene, or inspect pixels. The only HWNDs are hidden, owned STATIC
// windows used to publish real cached refresh identities.
#include "../src/plugins/ui_registry.hpp"
#include "../src/plugins/ui_catalog.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace {
thread_local long allocationCountdown = -1;
thread_local unsigned allocationFaults = 0;
}
void* operator new(std::size_t size) {
    if (allocationCountdown >= 0 && allocationCountdown-- == 0) {
        allocationCountdown = -1; ++allocationFaults; throw std::bad_alloc();
    }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0, cases = 0;
constexpr uint32_t kAllScopes = MMS_UI_CATALOG_REGISTRY | MMS_UI_CATALOG_MAIN_MENU | MMS_UI_CATALOG_MAIN_CONTROLS;
constexpr uint32_t kAllKnown = MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_ENABLED | MMS_UI_CATALOG_VISIBLE |
    MMS_UI_CATALOG_CHECKED | MMS_UI_CATALOG_BOUNDS | MMS_UI_CATALOG_PRESENTATION;
constexpr const char* kAnchors[] = {"mms.menu.plugins", "mms.toolbar.main.extensions", "mms.workspace.tabs.extensions",
    "mms.properties.main.extensions", "mms.sidebar.left.extensions", "mms.sidebar.right.extensions"};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL UI catalog after %u checks: %s\n", checks, message); std::fflush(stderr); std::exit(1); }
}
MmsString S(const char* value) { return {value, std::strlen(value)}; }
std::string Str(MmsString value) {
    Check(!value.bytes || value.data, "nonempty leased strings have storage");
    return {value.bytes ? value.data : "", std::size_t(value.bytes)};
}
template<class T> T OutputMarker() {
    T value; std::memset(&value, 0xa5, sizeof(value)); value.h = Header<T>(); return value;
}
MmsUiCatalogSnapshotV1 SnapshotMarker() {
    auto value = OutputMarker<MmsUiCatalogSnapshotV1>(); value.reserved = 0; return value;
}
MmsUiCatalogEntryV1 EntryMarker() {
    auto value = OutputMarker<MmsUiCatalogEntryV1>(); value.reserved = 0; return value;
}
template<class T> bool SameBytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }
bool SameRef(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) {
    return SameBytes(a.h, b.h) && a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
template<class T> const T& Record(const MmsSpan& span, uint64_t index) {
    Check(span.recordVersion == 1 && span.stride == sizeof(T) && index < span.count && span.data,
        "leased spans preserve the public record version, stride and extent");
    const auto* bytes = static_cast<const unsigned char*>(span.data) + index * span.stride;
    Check(reinterpret_cast<std::uintptr_t>(bytes) % alignof(T) == 0, "leased record storage has natural ABI alignment");
    const auto& record = *reinterpret_cast<const T*>(bytes);
    Check(record.h.size == sizeof(T) && record.h.major == 1 && record.h.minor == 0, "each leased record carries its own 1.0 header");
    return record;
}
const MmsUiCatalogEntryV1* Find(const MmsUiCatalogSnapshotV1& snapshot, const std::string& id) {
    const MmsUiCatalogEntryV1* result = nullptr;
    for (uint64_t i = 0; i < snapshot.elements.count; ++i) {
        const auto& entry = Record<MmsUiCatalogEntryV1>(snapshot.elements, i);
        if (Str(entry.id) == id) { Check(!result, "one element identity occurs only once per catalog"); result = &entry; }
    }
    return result;
}
const MmsUiCatalogRegistrationV1* Registration(const MmsUiCatalogSnapshotV1& snapshot, MmsHandle handle) {
    for (uint64_t i = 0; i < snapshot.registrations.count; ++i) {
        const auto& value = Record<MmsUiCatalogRegistrationV1>(snapshot.registrations, i);
        if (value.handle == handle) return &value;
    }
    return nullptr;
}
// Immutable-memory oracle independent of the publisher's implementation. It
// reads every public scalar and every byte of all six strings after retirement.
uint64_t Digest(const MmsUiCatalogSnapshotV1& snapshot) {
    uint64_t hash = 1469598103934665603ull;
    const auto bytes = [&](const void* data, std::size_t count) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < count; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    const auto text = [&](MmsString value) { bytes(&value.bytes, sizeof(value.bytes)); if (value.bytes) bytes(value.data, std::size_t(value.bytes)); };
    const auto scalar = [&](auto value) { bytes(&value, sizeof(value)); };
    scalar(snapshot.revision); scalar(snapshot.registryRevision); scalar(snapshot.projectEpoch);
    scalar(snapshot.requestedScopes); scalar(snapshot.availableScopes); scalar(snapshot.coverageFlags);
    scalar(snapshot.elements.count); scalar(snapshot.registrations.count);
    for (uint64_t i = 0; i < snapshot.elements.count; ++i) {
        const auto& e = *reinterpret_cast<const MmsUiCatalogEntryV1*>(static_cast<const unsigned char*>(snapshot.elements.data) + i * snapshot.elements.stride);
        scalar(e.element); text(e.id); text(e.ownerPlugin); text(e.parentId); text(e.label); text(e.commandId); text(e.propertyId);
        scalar(e.descriptionGeneration); scalar(e.target.element); scalar(e.target.instanceGeneration); scalar(e.target.projectEpoch);
        bytes(&e.bounds, sizeof(e.bounds)); scalar(e.kind); scalar(e.origin); scalar(e.scope); scalar(e.knownFields);
        scalar(e.state); scalar(e.role); scalar(e.ordinal); scalar(e.reserved);
    }
    for (uint64_t i = 0; i < snapshot.registrations.count; ++i) {
        const auto& r = *reinterpret_cast<const MmsUiCatalogRegistrationV1*>(static_cast<const unsigned char*>(snapshot.registrations.data) + i * snapshot.registrations.stride);
        scalar(r.handle); text(r.id); text(r.ownerPlugin); text(r.label); scalar(r.kind); scalar(r.flags);
    }
    return hash;
}

struct Fixture {
    unsigned changes = 0, stateReads = 0, propertyReads = 0, providerReads = 0, invokes = 0, layouts = 0, dataReads = 0;
    UiRegistry registry{[this] { ++changes; }};
    UiClient owner{&registry, "example.catalog"}, reader{&registry, "example.catalog.reader"},
        foreign{&registry, "example.catalog.foreign"}, sameNameReader{&registry, "example.catalog.reader"};
    UiRefreshQueue& refresh = registry.Refresh();
    RefreshClient refreshOwner{&refresh, owner.id};
    MmsUiApiV1 ui = registry.Api(owner);
    MmsUiCatalogApiV1 api = registry.Catalog().Api(reader);
    MmsUiRefreshApiV1 refreshApi = refresh.Api(refreshOwner);
    MmsHandle command = 0, property = 0, group = 0, button = 0, input = 0, menu = 0, stateProvider = 0;
    HWND window = nullptr, child = nullptr, inputWindow = nullptr;
    std::vector<RefreshPresentation> presentations;
    std::vector<MmsHandle> tickets;
    explicit Fixture(bool registrations = true) {
        if (!registrations) return;
        auto c = Command("example.catalog.command");
        Check(ui.registerCommand(ui.context, &c, &command) == MMS_OK && command, "register original command");
        MmsPropertyDescV1 p{}; p.info.h = Header<MmsPropertyDescV1>();
        p.info.id = S("example.catalog.property"); p.info.ownerPlugin = Text(owner.id); p.info.label = S("Original property");
        p.info.flags = MMS_PROPERTY_READ_ONLY; p.info.valueType = MMS_VALUE_NUMBER; p.context = this;
        p.read = [](void* context, MmsObjectRef, MmsValue* value, MmsHandle* lease) -> MmsResult {
            ++static_cast<Fixture*>(context)->propertyReads; *value = {}; value->type = MMS_VALUE_NUMBER; value->value.number = 123; *lease = 0; return MMS_OK;
        };
        p.releaseValue = [](void*, MmsHandle) {};
        Check(ui.registerProperty(ui.context, &p, &property) == MMS_OK && property, "register original scalar property without reading it");
        auto e = Element("example.catalog.group", "mms.toolbar.main.extensions", MMS_UI_GROUP);
        e.capabilities |= MMS_UI_CONTAINER; e.label = S("Original group");
        Check(ui.registerElement(ui.context, &e, &group) == MMS_OK && group, "register original group");
        e = Element("example.catalog.button", "example.catalog.group", MMS_UI_BUTTON); e.commandId = c.info.id; e.label = S("Original button");
        Check(ui.registerElement(ui.context, &e, &button) == MMS_OK && button, "register original command button");
        e = Element("example.catalog.input", "example.catalog.group", MMS_UI_INPUT); e.propertyId = p.info.id; e.label = S("Original input");
        Check(ui.registerElement(ui.context, &e, &input) == MMS_OK && input, "register original property input");
        e = Element("example.catalog.menu", "mms.menu.plugins", MMS_UI_MENU_ITEM); e.capabilities = MMS_UI_HIDE;
        e.commandId = c.info.id; e.label = S("Original plugin menu");
        Check(ui.registerElement(ui.context, &e, &menu) == MMS_OK && menu, "register original plugin menu item");
    }
    ~Fixture() {
        for (auto ticket : tickets) { refreshApi.cancel(refreshApi.context, ticket); refreshApi.release(refreshApi.context, ticket); }
        refresh.Retire(refreshOwner);
        if (window) DestroyWindow(window);
    }
    MmsCommandDescV1 Command(const char* id) {
        MmsCommandDescV1 c{}; c.info.h = Header<MmsCommandDescV1>(); c.info.id = S(id); c.info.ownerPlugin = Text(owner.id);
        c.info.label = S("Original command"); c.info.flags = MMS_COMMAND_INVOKE_ONLY; c.context = this;
        c.getState = [](void* context, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
            ++static_cast<Fixture*>(context)->stateReads; *out = {Header<MmsCommandStateV1>(), 0, 1, 0, 0, {}}; return MMS_OK;
        };
        c.invoke = [](void* context, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { ++static_cast<Fixture*>(context)->invokes; return MMS_OK; };
        return c;
    }
    MmsUiElementV1 Element(const char* id, const char* parent, uint32_t kind) {
        MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = S(id); e.ownerPlugin = Text(owner.id);
        e.parentId = S(parent); e.kind = kind; e.capabilities = MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_LAYOUT; return e;
    }
    MmsUiCatalogSnapshotV1 Acquire(uint32_t scopes = kAllScopes) {
        MmsUiCatalogSnapshotV1 out{}; out.h = Header<MmsUiCatalogSnapshotV1>();
        Check(api.acquireSnapshot(api.context, scopes, &out) == MMS_OK && out.handle, "acquire complete original catalog lease");
        Check(out.h.size == sizeof(out) && out.h.major == 1 && out.h.minor == 0 && !out.reserved && out.requestedScopes == scopes,
            "snapshot returns normalized 1.0 header and the exact requested scopes");
        return out;
    }
    void Release(const MmsUiCatalogSnapshotV1& snapshot) { Check(api.releaseSnapshot(api.context, snapshot.handle) == MMS_OK, "release exact catalog snapshot owner"); }
    MmsUiSnapshotV1 Legacy() {
        MmsUiSnapshotV1 out{}; out.h = Header<MmsUiSnapshotV1>(); Check(ui.acquireSnapshot(ui.context, 0, &out) == MMS_OK, "acquire original V1 UI snapshot"); return out;
    }
    void NoCallbacks() const {
        Check(!stateReads && !propertyReads && !providerReads && !invokes && !layouts && !dataReads,
            "catalog publication/query never evaluates state/property/providers, refresh layout/data, or executes a command");
    }
    void Materialize() {
        window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Owned catalog contract", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 400, 220, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create hidden process-owned refresh parent");
        child = CreateWindowExW(0, L"STATIC", L"Window text is not catalog metadata", WS_CHILD, 20, 30, 100, 25,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        inputWindow = CreateWindowExW(0, L"STATIC", L"No implicit property read", WS_CHILD, 140, 30, 100, 25,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(child && inputWindow, "create hidden process-owned targets for every actual subtree member");
        RefreshPresentation g{group, owner.id, 400, window, 96, 0, 0, 300, 100, true}; g.container = true;
        RefreshPresentation b{button, owner.id, 401, child, 96, 20, 30, 100, 25, true}; b.stateRefresh = true; b.kind = MMS_UI_BUTTON;
        RefreshPresentation p{input, owner.id, 402, inputWindow, 96, 140, 30, 100, 25, true}; p.kind = MMS_UI_INPUT;
        presentations = {g, b, p}; refresh.SyncTargets(presentations);
        refresh.SetLayout([this](MmsHandle) -> MmsResult { ++layouts; return MMS_OK; });
        refresh.SetPropertyData([this](MmsHandle, const std::function<MmsResult()>&, uint64_t&) -> MmsResult { ++dataReads; return MMS_E_FAILED; });
        refresh.SetStateContent([](MmsHandle, const std::shared_ptr<const ButtonStateContent>&, bool) -> MmsResult { return MMS_OK; });
        auto state = registry.StateContentApi(owner); MmsUiStateContentDescV1 provider{}; provider.h = Header<MmsUiStateContentDescV1>();
        provider.target = Target(button).target; provider.context = this; provider.fields = MMS_UI_STATE_LABEL;
        provider.prepare = [](void* context, const MmsUiTargetRefV1*, MmsHandle) -> MmsResult { ++static_cast<Fixture*>(context)->providerReads; return MMS_E_FAILED; };
        Check(state.registerProvider(state.context, &provider, &stateProvider) == MMS_OK, "install a real callback provider which catalog reads must never call");
    }
    MmsUiRefreshTargetV1 Target(MmsHandle element) {
        MmsUiRefreshTargetV1 value{}; value.h = Header<MmsUiRefreshTargetV1>();
        Check(refreshApi.getTarget(refreshApi.context, element, &value) == MMS_OK, "obtain original refresh target identity"); return value;
    }
    MmsHandle Subtree() {
        MmsUiRefreshRequestV1 request{}; request.h = Header<MmsUiRefreshRequestV1>(); request.target = Target(group).target;
        request.kinds = MMS_UI_REFRESH_REPAINT; request.scope = MMS_UI_REFRESH_SUBTREE;
        MmsHandle handle = 0; Check(refreshApi.request(refreshApi.context, &request, &handle) == MMS_OK && handle, "accept unrelated real SUBTREE work before catalog publication");
        tickets.push_back(handle); return handle;
    }
    MmsUiRefreshStateV1 Ticket(MmsHandle handle) {
        MmsUiRefreshStateV1 state{}; state.h = Header<MmsUiRefreshStateV1>();
        Check(refreshApi.query(refreshApi.context, handle, &state) == MMS_OK, "query previously accepted original refresh ticket"); return state;
    }
};

HostUiCatalogEntry Native(const char* id, uint64_t instance, uint32_t scope = MMS_UI_CATALOG_MAIN_CONTROLS) {
    HostUiCatalogEntry e; e.id = id; e.label = "Published native metadata"; e.instance = instance;
    e.kind = MMS_UI_BUTTON; e.role = MMS_UI_CATALOG_ROLE_BUTTON; e.scope = scope;
    e.knownFields = kAllKnown; e.state = MMS_UI_CATALOG_STATE_VISIBLE | MMS_UI_CATALOG_STATE_CHECKED;
    e.bounds = {11.5f, 22.25f, 103.75f, 24.5f}; e.ordinal = 17; return e;
}
HostUiCatalogPublication Publication() {
    HostUiCatalogPublication p; p.availableScopes = kAllScopes;
    p.coverageFlags = MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_MATERIALIZED_MENU |
        MMS_UI_CATALOG_DECLARED_CONTROLS | MMS_UI_CATALOG_INCOMPLETE_NATIVE;
    p.entries.push_back(Native("mms.catalog.control", 701));
    auto menu = Native("mms.catalog.menu", 702, MMS_UI_CATALOG_MAIN_MENU); menu.kind = MMS_UI_MENU; menu.role = MMS_UI_CATALOG_ROLE_MENU;
    menu.bounds = {}; menu.knownFields &= ~MMS_UI_CATALOG_BOUNDS; menu.ordinal = 3; p.entries.push_back(menu);
    auto row = menu; row.id = "mms.catalog.menu.item"; row.parentId = menu.id; row.kind = MMS_UI_MENU_ITEM;
    row.role = MMS_UI_CATALOG_ROLE_MENU_ITEM; row.ordinal = 4; p.entries.push_back(row); return p;
}

void CapabilitiesAnchorsAndOriginalHandles() {
    Fixture empty(false); auto caps = OutputMarker<MmsUiCatalogCapabilitiesV1>(); caps.reserved = 0;
    Check(empty.api.getCapabilities(empty.api.context, &caps) == MMS_OK && caps.h.size == sizeof(caps) &&
        caps.supportedScopes == kAllScopes && caps.maxEntries == 16384 && caps.maxLeasesPerPlugin == 16 && caps.maxLeasesGlobal == 128 &&
        caps.maxSnapshotBytes == 8ull * 1024 * 1024 && caps.maxRetainedBytesPerPlugin == 16ull * 1024 * 1024 &&
        caps.maxRetainedBytesGlobal == 64ull * 1024 * 1024 && !caps.reserved, "public catalog capabilities expose the frozen finite count/byte budgets");
    auto initial = empty.Acquire(MMS_UI_CATALOG_REGISTRY); auto old = empty.Legacy();
    Check(initial.elements.count == 6 && !initial.registrations.count && old.elements.count == 6 && !old.commands.count && !old.properties.count,
        "fresh catalog and unchanged legacy snapshot contain exactly the original six anchors");
    Check(initial.registryRevision == empty.registry.Revision() && initial.projectEpoch == empty.refresh.ProjectEpoch() &&
        (initial.coverageFlags & MMS_UI_CATALOG_REGISTRY_COMPLETE), "registry and project identities come from their original stores");
    for (const char* id : kAnchors) {
        const auto* e = Find(initial, id); Check(e && e->origin == MMS_UI_CATALOG_HOST_ANCHOR && e->scope == MMS_UI_CATALOG_REGISTRY &&
            e->element == empty.registry.elements.at(id).handle && e->descriptionGeneration == empty.registry.elements.at(id).desc.generation &&
            e->target.element == e->element && e->target.projectEpoch == initial.projectEpoch && !e->target.instanceGeneration &&
            e->knownFields == MMS_UI_CATALOG_LABEL && !e->state && !e->reserved,
            "each host anchor reuses the original registration handle and is honestly unmaterialized");
    }
    empty.ui.releaseSnapshot(empty.ui.context, old.handle); empty.Release(initial); empty.NoCallbacks();

    Fixture f; auto snapshot = f.Acquire(MMS_UI_CATALOG_REGISTRY); auto legacy = f.Legacy();
    Check(snapshot.elements.count == 10 && snapshot.registrations.count == 2 && legacy.elements.count == 10,
        "catalog includes all registered elements and both registration descriptions, without replacing V1 schemas");
    const auto* button = Find(snapshot, "example.catalog.button"); const auto* input = Find(snapshot, "example.catalog.input");
    Check(button && input && button->element == f.button && input->element == f.input && button->origin == MMS_UI_CATALOG_REGISTERED &&
        input->origin == MMS_UI_CATALOG_REGISTERED && Str(button->commandId) == "example.catalog.command" &&
        Str(input->propertyId) == "example.catalog.property" && button->descriptionGeneration == f.registry.elements.at("example.catalog.button").desc.generation,
        "catalog elements retain the exact original command/property associations and description generations");
    const auto* command = Registration(snapshot, f.command); const auto* property = Registration(snapshot, f.property);
    Check(command && property && command->kind == MMS_UI_CATALOG_COMMAND && property->kind == MMS_UI_CATALOG_PROPERTY &&
        command->flags == MMS_COMMAND_INVOKE_ONLY && property->flags == MMS_PROPERTY_READ_ONLY &&
        Str(command->id) == "example.catalog.command" && Str(property->id) == "example.catalog.property" &&
        Str(command->label) == "Original command" && Str(property->label) == "Original property",
        "registration catalog uses original handles, labels and flags rather than exporting callback descriptors");
    Check(button->knownFields == MMS_UI_CATALOG_LABEL && input->knownFields == MMS_UI_CATALOG_LABEL &&
        !button->state && !input->state && !button->target.instanceGeneration && button->role == MMS_UI_CATALOG_ROLE_BUTTON && input->role == MMS_UI_CATALOG_ROLE_INPUT,
        "unqueried provider/command state and unmaterialized geometry stay unknown");
    auto emptyLabel = f.registry.elements.at("example.catalog.input").desc; emptyLabel.label = {};
    Check(f.ui.updateElement(f.ui.context, f.input, &emptyLabel) == MMS_OK, "update the original description to an intentionally empty label");
    auto emptySnapshot = f.Acquire(MMS_UI_CATALOG_REGISTRY); const auto* emptyInput = Find(emptySnapshot, "example.catalog.input");
    Check(emptyInput && emptyInput->knownFields == MMS_UI_CATALOG_LABEL && !emptyInput->label.bytes && Str(input->label) == "Original input",
        "known empty registered labels differ from unknown native labels while an old leased label stays immutable"); f.Release(emptySnapshot);
    f.ui.releaseSnapshot(f.ui.context, legacy.handle); f.Release(snapshot); f.NoCallbacks();
}

void NativeMetadataScopesAndAliasing() {
    Fixture f;
    std::map<std::string, UiRegistry::Geometry> originalGeometry;
    for (const auto& [id, element] : f.registry.elements) if (element.desc.capabilities & MMS_UI_LAYOUT)
        originalGeometry.emplace(id, UiRegistry::Geometry{0, float(originalGeometry.size()) * 24, 180, 20, true, false});
    Check(!originalGeometry.empty() && f.registry.PublishGeometry(originalGeometry, f.registry.Revision()),
        "publish every original layout-capable registration's geometry before testing catalog isolation");
    auto layoutApi = f.registry.LayoutApi(f.owner); MmsUiLayoutSnapshotV1 baselineLayout{}; baselineLayout.h = Header<MmsUiLayoutSnapshotV1>();
    Check(layoutApi.acquireSnapshot(layoutApi.context, &baselineLayout) == MMS_OK && baselineLayout.items.count == originalGeometry.size(),
        "original layout snapshot is ready with its complete real published geometry before catalog publication");
    const auto baselineGeometryRevision = baselineLayout.geometryRevision;
    layoutApi.releaseSnapshot(layoutApi.context, baselineLayout.handle);
    const auto beforeRevision = f.registry.Revision(), beforePreferences = f.registry.PreferencesRevision();
    const auto preferences = f.registry.ExportPreferences(); const auto changes = f.changes;
    const auto original = f.registry.elements.at("example.catalog.button").desc;
    auto publication = Publication();
    HostUiCatalogEntry alias = Native("example.catalog.button", 0); alias.reuseElement = f.button; alias.ordinal = 23;
    alias.knownFields &= ~(MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS); alias.bounds = {};
    alias.label = "Published alias caption"; publication.entries.push_back(alias);
    auto unknown = Native("mms.catalog.unknown", 0); unknown.label.clear(); unknown.knownFields = 0; unknown.state = 0; unknown.bounds = {};
    unknown.role = MMS_UI_CATALOG_ROLE_UNKNOWN; unknown.ordinal = 99; publication.entries.push_back(unknown);
    Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish a complete native metadata candidate with an original-element alias");
    auto snapshot = f.Acquire(); auto onlyRegistry = f.Acquire(MMS_UI_CATALOG_REGISTRY);
    auto onlyMenu = f.Acquire(MMS_UI_CATALOG_MAIN_MENU); auto onlyControls = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS);
    const auto* control = Find(snapshot, "mms.catalog.control"); const auto* menu = Find(snapshot, "mms.catalog.menu.item");
    const auto* aliasEntry = Find(snapshot, "example.catalog.button"); const auto* unknownEntry = Find(snapshot, "mms.catalog.unknown");
    Check(control && control->origin == MMS_UI_CATALOG_NATIVE_OBSERVATION && control->kind == MMS_UI_BUTTON &&
        control->role == MMS_UI_CATALOG_ROLE_BUTTON && control->scope == MMS_UI_CATALOG_MAIN_CONTROLS && control->knownFields == kAllKnown &&
        control->state == (MMS_UI_CATALOG_STATE_VISIBLE | MMS_UI_CATALOG_STATE_CHECKED) && control->ordinal == 17 &&
        SameBytes(control->bounds, publication.entries[0].bounds) && control->target.element == control->element &&
        control->target.instanceGeneration == 701 && control->target.projectEpoch == snapshot.projectEpoch,
        "native observation preserves explicit false enabled state, checked/visible bits, DIP bounds, role, ordinal and instance");
    Check(menu && menu->role == MMS_UI_CATALOG_ROLE_MENU_ITEM && Str(menu->parentId) == "mms.catalog.menu" && menu->ordinal == 4 &&
        !(menu->knownFields & MMS_UI_CATALOG_BOUNDS), "native menu hierarchy and ordering do not invent HWND client bounds");
    Check(unknownEntry && !unknownEntry->knownFields && !unknownEntry->state && !unknownEntry->target.instanceGeneration &&
        !unknownEntry->label.bytes, "unknown label/state/presentation fields remain distinguishable from known empty/false values");
    Check(aliasEntry && aliasEntry->element == f.button && aliasEntry->origin == MMS_UI_CATALOG_REGISTERED &&
        !aliasEntry->target.instanceGeneration && aliasEntry->target.projectEpoch == snapshot.projectEpoch && aliasEntry->ordinal == 23 && aliasEntry->scope ==
        (MMS_UI_CATALOG_REGISTRY | MMS_UI_CATALOG_MAIN_CONTROLS) && Str(aliasEntry->label) == "Published alias caption" &&
        aliasEntry->knownFields == alias.knownFields && aliasEntry->state == alias.state,
        "host alias contributes published metadata to exactly the original registered element");
    Check(snapshot.elements.count == 14 && onlyRegistry.elements.count == 10 && onlyMenu.elements.count == 2 && onlyControls.elements.count == 3 &&
        !onlyMenu.registrations.count && !onlyControls.registrations.count,
        "scope filtering includes each matching element once and registry descriptions only when requested");
    Check(Find(onlyRegistry, "example.catalog.button") && !Find(onlyRegistry, "mms.catalog.control") &&
        Find(onlyControls, "example.catalog.button") && !Find(onlyControls, "example.catalog.input") && !Find(onlyMenu, "mms.catalog.control"),
        "an alias belongs to registry and its declared observed scope without exposing unrelated entries");
    Check(snapshot.availableScopes == kAllScopes && snapshot.coverageFlags == publication.coverageFlags,
        "native available scopes and explicit incomplete coverage are published without claiming unseen UI");
    const auto& after = f.registry.elements.at("example.catalog.button").desc;
    Check(SameBytes(original, after) && Str(after.label) == "Original button" && f.registry.Revision() == beforeRevision &&
        f.registry.PreferencesRevision() == beforePreferences && f.registry.ExportPreferences() == preferences && f.changes == changes,
        "aliasing neither replaces registered descriptions nor edits preferences, revision or Changed notifications");
    auto legacy = f.Legacy(); Check(legacy.elements.count == 10, "legacy V1 snapshot excludes every nativeObserved record");
    for (uint64_t i = 0; i < legacy.elements.count; ++i) Check(Str(Record<MmsUiElementV1>(legacy.elements, i).id).rfind("mms.catalog.", 0) != 0,
        "native directory-only observations never become legacy plugin UI entries");
    MmsUiLayoutSnapshotV1 layout{}; layout.h = Header<MmsUiLayoutSnapshotV1>();
    Check(layoutApi.acquireSnapshot(layoutApi.context, &layout) == MMS_OK && layout.items.count == originalGeometry.size() &&
        layout.uiRevision == beforeRevision && layout.geometryRevision == baselineGeometryRevision,
        "original layout geometry remains ready, complete and unchanged after metadata publication");
    for (uint64_t i = 0; i < layout.items.count; ++i) Check(Str(Record<MmsUiLayoutItemV1>(layout.items, i).elementId).rfind("mms.catalog.", 0) != 0,
        "native observations never enter the plugin layout snapshot");
    layoutApi.releaseSnapshot(layoutApi.context, layout.handle); f.ui.releaseSnapshot(f.ui.context, legacy.handle);
    f.Release(snapshot); f.Release(onlyRegistry); f.Release(onlyMenu); f.Release(onlyControls); f.NoCallbacks();

    auto emptyLabelPublication = publication;
    auto& emptyAlias = emptyLabelPublication.entries[3];
    Check(emptyAlias.reuseElement == f.button, "label boundary operates on the original registered button alias");
    emptyAlias.knownFields |= MMS_UI_CATALOG_LABEL; emptyAlias.label.clear();
    Check(f.registry.Catalog().Publish(emptyLabelPublication) == MMS_OK, "publish an explicitly known empty alias label");
    auto emptyLabelSnapshot = f.Acquire(); const auto* emptyAliasEntry = Find(emptyLabelSnapshot, "example.catalog.button");
    Check(emptyAliasEntry && (emptyAliasEntry->knownFields & MMS_UI_CATALOG_LABEL) && !emptyAliasEntry->label.bytes &&
        emptyAliasEntry->element == f.button && Str(f.registry.elements.at("example.catalog.button").desc.label) == "Original button",
        "the LABEL bit makes an empty alias caption override the catalog while preserving the original registered label");
    const auto emptyDigest = Digest(emptyLabelSnapshot);
    auto unknownLabelPublication = emptyLabelPublication;
    unknownLabelPublication.entries[3].knownFields &= ~MMS_UI_CATALOG_LABEL;
    Check(f.registry.Catalog().Publish(unknownLabelPublication) == MMS_OK, "publish the same empty alias bytes with LABEL explicitly unknown");
    auto fallbackSnapshot = f.Acquire(); const auto* fallbackEntry = Find(fallbackSnapshot, "example.catalog.button");
    Check(fallbackEntry && fallbackEntry->element == f.button && Str(fallbackEntry->label) == "Original button" &&
        Digest(emptyLabelSnapshot) == emptyDigest && !emptyAliasEntry->label.bytes &&
        SameBytes(f.registry.elements.at("example.catalog.button").desc, original) && f.registry.Revision() == beforeRevision &&
        f.registry.PreferencesRevision() == beforePreferences && f.changes == changes,
        "unknown alias LABEL falls back to the original description; publication distinguishes unknown from known empty without changing old leases or registry state");
    f.Release(fallbackSnapshot); f.Release(emptyLabelSnapshot); f.NoCallbacks();
}

void CachedTargetsAndUnrelatedSubtree() {
    Fixture f; f.Materialize(); const auto target = f.Target(f.button); const auto ticket = f.Subtree(); const auto pending = f.Ticket(ticket);
    const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision(); const auto changes = f.changes;
    auto before = f.Acquire(MMS_UI_CATALOG_REGISTRY); const auto* entry = Find(before, "example.catalog.button");
    Check(entry && SameRef(entry->target, target.target) && (entry->knownFields & (MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS)) ==
        (MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS) && entry->bounds.x == 20 && entry->bounds.y == 30 &&
        entry->bounds.width == 100 && entry->bounds.height == 25 && Str(entry->label) == "Original button",
        "registered target uses the original cached refresh reference/coordinates and does not read native window text");
    auto publication = Publication();
    auto alias = Native("example.catalog.button", 0); alias.reuseElement = f.button; alias.knownFields = MMS_UI_CATALOG_LABEL;
    alias.label = "Observed materialized alias"; alias.state = 0; alias.bounds = {}; publication.entries.push_back(alias);
    Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish observations while unrelated subtree ticket is accepted");
    Check(SameBytes(f.Ticket(ticket), pending) && f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.changes == changes,
        "catalog-only publication neither invalidates accepted subtree work nor publishes a different refresh topology");
    auto resolved = EntryMarker(); MmsHandle lease = 0;
    Check(f.api.resolveElement(f.api.context, &target.target, &resolved, &lease) == MMS_OK && lease &&
        resolved.element == f.button && SameRef(resolved.target, target.target) && Str(resolved.label) == "Observed materialized alias" &&
        (resolved.knownFields & (MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS)) == (MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS) &&
        resolved.bounds.x == 20 && resolved.bounds.y == 30 && resolved.bounds.width == 100 && resolved.bounds.height == 25,
        "alias resolve keeps the exact original cached target/geometry instead of inventing a second presentation identity");
    Check(f.api.releaseSnapshot(f.api.context, lease) == MMS_OK, "resolve lease shares the ordinary snapshot release path");
    f.NoCallbacks(); f.refresh.Drain(); const auto done = f.Ticket(ticket);
    Check(done.status == MMS_UI_REFRESH_COMPLETED && done.result == MMS_OK && done.completedKinds == MMS_UI_REFRESH_REPAINT,
        "unrelated previously accepted subtree still completes through its original queue after catalog publication");
    f.NoCallbacks(); f.Release(before);
    // The two catalog revisions describe the registry and native observation
    // publications, not a complete change token for the separately published
    // refresh cache. Do not call Catalog.Publish or edit a description here.
    auto oldCache = f.Acquire(MMS_UI_CATALOG_REGISTRY); const auto cacheDigest = Digest(oldCache);
    const auto* oldCachedEntry = Find(oldCache, "example.catalog.button"); Check(oldCachedEntry, "registered target is present before a refresh-cache-only publication");
    const auto oldCachedRef = oldCachedEntry->target;
    f.presentations[1].instance += 1000;
    f.presentations[1].x = 27.5f; f.presentations[1].y = 38.25f; f.presentations[1].width = 123.5f; f.presentations[1].height = 29.75f;
    f.refresh.SyncTargets(f.presentations);
    const auto refreshedTarget = f.Target(f.button);
    auto newCache = f.Acquire(MMS_UI_CATALOG_REGISTRY); const auto* newCachedEntry = Find(newCache, "example.catalog.button");
    Check(newCachedEntry && newCache.revision == oldCache.revision && newCache.registryRevision == oldCache.registryRevision &&
        newCache.projectEpoch == oldCache.projectEpoch && SameRef(newCachedEntry->target, refreshedTarget.target) &&
        !SameRef(newCachedEntry->target, oldCachedRef) && newCachedEntry->bounds.x == 27.5f && newCachedEntry->bounds.y == 38.25f &&
        newCachedEntry->bounds.width == 123.5f && newCachedEntry->bounds.height == 29.75f &&
        oldCachedEntry->bounds.x == 20 && oldCachedEntry->bounds.y == 30 && Digest(oldCache) == cacheDigest &&
        f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.changes == changes,
        "pure catalog reads reflect new cached presentation/geometry with both publication revisions unchanged and old lease bytes intact");
    auto staleCacheEntry = EntryMarker(); const auto staleCacheBefore = staleCacheEntry; MmsHandle cacheLease = 876543;
    Check(f.api.resolveElement(f.api.context, &oldCachedRef, &staleCacheEntry, &cacheLease) == MMS_E_STALE &&
        SameBytes(staleCacheEntry, staleCacheBefore) && cacheLease == 876543,
        "cache-only instance replacement invalidates the exact old ref even when neither catalog revision advances");
    Check(f.api.resolveElement(f.api.context, &refreshedTarget.target, &staleCacheEntry, &cacheLease) == MMS_OK &&
        SameRef(staleCacheEntry.target, refreshedTarget.target) && staleCacheEntry.bounds.x == 27.5f && staleCacheEntry.bounds.width == 123.5f,
        "resolve reads the new original cache identity and geometry without requiring a native metadata publication");
    Check(f.api.releaseSnapshot(f.api.context, cacheLease) == MMS_OK, "release cache-only resolve lease");
    auto repeatedCache = f.Acquire(MMS_UI_CATALOG_REGISTRY);
    Check(repeatedCache.revision == newCache.revision && repeatedCache.registryRevision == newCache.registryRevision && Digest(repeatedCache) == Digest(newCache),
        "repeated acquire and resolve do not manufacture metadata revisions as a side effect of observing changed cache state");
    f.Release(repeatedCache); f.Release(newCache); f.Release(oldCache); f.NoCallbacks();
    f.refresh.SuspendTargets(); auto suspended = f.Acquire(MMS_UI_CATALOG_REGISTRY); const auto* s = Find(suspended, "example.catalog.button");
    Check(s && !s->target.instanceGeneration && !(s->knownFields & (MMS_UI_CATALOG_PRESENTATION | MMS_UI_CATALOG_BOUNDS)),
        "suspended cached registry target reports no live presentation instead of retaining obsolete geometry");
    auto untouched = EntryMarker(); const auto copy = untouched; MmsHandle out = 0x12345678;
    Check(f.api.resolveElement(f.api.context, &refreshedTarget.target, &untouched, &out) == MMS_E_STALE && SameBytes(untouched, copy) && out == 0x12345678,
        "an old presentation reference cannot resolve after suspension and leaves both outputs untouched");
    f.Release(suspended); f.NoCallbacks();
}

void LeaseThreadStoppingAndRetirement() {
    Fixture f; auto publication = Publication(); Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish retained native snapshot baseline");
    auto held = f.Acquire(); const auto digest = Digest(held); const auto* current = Find(held, "mms.catalog.control"); Check(current, "find current native target");
    const auto reference = current->target;
    Check(f.registry.Catalog().HasLeases(f.reader) && f.registry.HasLeases(f.reader) && !f.registry.Catalog().HasLeases(f.sameNameReader),
        "catalog leases participate in unload protection with exact UiClient pointer ownership");
    auto foreign = f.registry.Catalog().Api(f.foreign); auto alias = f.registry.Catalog().Api(f.sameNameReader);
    Check(foreign.releaseSnapshot(foreign.context, held.handle) != MMS_OK && alias.releaseSnapshot(alias.context, held.handle) != MMS_OK && Digest(held) == digest,
        "foreign clients and a different client with the same textual ID cannot release another owner's immutable memory");
    MmsResult workerCaps = MMS_OK, workerAcquire = MMS_OK, workerResolve = MMS_OK, workerRelease = MMS_OK;
    auto caps = OutputMarker<MmsUiCatalogCapabilitiesV1>(); caps.reserved = 0; const auto capsBefore = caps;
    auto acquired = SnapshotMarker(); const auto acquiredBefore = acquired; auto resolved = EntryMarker(); const auto resolvedBefore = resolved;
    MmsHandle resolvedLease = 999; uint64_t workerDigest = 0;
    std::thread worker([&] {
        workerDigest = Digest(held);
        workerCaps = f.api.getCapabilities(f.api.context, &caps);
        workerAcquire = f.api.acquireSnapshot(f.api.context, kAllScopes, &acquired);
        workerResolve = f.api.resolveElement(f.api.context, &reference, &resolved, &resolvedLease);
        workerRelease = f.api.releaseSnapshot(f.api.context, held.handle);
    }); worker.join();
    Check(workerDigest == digest && workerCaps == MMS_E_THREAD && workerAcquire == MMS_E_THREAD && workerResolve == MMS_E_THREAD && workerRelease == MMS_E_THREAD &&
        SameBytes(caps, capsBefore) && SameBytes(acquired, acquiredBefore) && SameBytes(resolved, resolvedBefore) && resolvedLease == 999,
        "workers can read leased memory but cannot call any catalog method or consume a lease");
    publication.entries[0].label = "New publication"; publication.entries[0].instance = 711;
    Check(f.registry.Catalog().Publish(publication) == MMS_OK && Digest(held) == digest, "new publication never overwrites a previous snapshot arena");
    f.registry.Catalog().RetireInstance(711);
    auto stale = EntryMarker(); const auto staleBefore = stale; MmsHandle lease = 987;
    Check(f.api.resolveElement(f.api.context, &reference, &stale, &lease) == MMS_E_STALE && SameBytes(stale, staleBefore) && lease == 987,
        "retired/replaced native presentation references resolve STALE without acquiring a hidden lease");
    publication.entries[0].instance = 721; Check(f.registry.Catalog().Publish(publication) == MMS_OK, "rebuild native presentation after retirement");
    auto rebuilt = f.Acquire(); const auto* rebuiltEntry = Find(rebuilt, "mms.catalog.control");
    Check(rebuiltEntry && rebuiltEntry->target.instanceGeneration == 721 && !SameRef(rebuiltEntry->target, reference),
        "rebuilt observed target has the new exact presentation identity"); f.Release(rebuilt);
    f.registry.Catalog().Suspend(); Check(Digest(held) == digest, "suspending the catalog does not destroy leased metadata");
    Check(f.api.resolveElement(f.api.context, &reference, &stale, &lease) == MMS_E_STALE && SameBytes(stale, staleBefore) && lease == 987,
        "old reference remains stale while native publication is suspended");
    Check(f.registry.Catalog().Publish(publication) == MMS_OK, "republish after suspension");
    auto epochSnapshot = f.Acquire(); const auto* epochEntry = Find(epochSnapshot, "mms.catalog.control");
    Check(epochEntry, "republished native entry is present before advancing the actual project epoch"); const auto epochRef = epochEntry->target; f.Release(epochSnapshot);
    const auto epoch = f.refresh.ProjectEpoch(); f.refresh.AdvanceProjectEpoch();
    Check(f.refresh.ProjectEpoch() != epoch && f.api.resolveElement(f.api.context, &epochRef, &stale, &lease) == MMS_E_STALE &&
        SameBytes(stale, staleBefore) && lease == 987 && Digest(held) == digest, "original project epoch advance invalidates resolves while old memory remains readable");
    f.registry.Retire(f.owner); Check(Digest(held) == digest, "unregistering all provider elements does not release another plugin's catalog snapshot");
    f.reader.stopping = true;
    Check(f.api.acquireSnapshot(f.api.context, kAllScopes, &acquired) == MMS_E_NOT_READY && SameBytes(acquired, acquiredBefore) &&
        f.api.resolveElement(f.api.context, &epochRef, &stale, &lease) == MMS_E_NOT_READY && SameBytes(stale, staleBefore) && lease == 987,
        "stopping client cannot acquire a snapshot or resolve another new lease");
    workerDigest = 0; std::thread after([&] { workerDigest = Digest(held); }); after.join(); Check(workerDigest == digest, "held memory remains worker-readable after publish/retire/suspend/epoch/unregister/stop");
    f.Release(held); Check(!f.registry.Catalog().HasLeases(f.reader) && !f.registry.HasLeases(f.reader), "UI-thread release remains available during stop and clears unload protection");
    Check(f.api.releaseSnapshot(f.api.context, held.handle) != MMS_OK, "double release cannot consume another lease"); f.NoCallbacks();
}

void RegisteredAncestorLifecycle() {
    for (bool layoutGroup : {false, true}) {
        Fixture f(false);
        const char* parentId = layoutGroup ? "example.catalog.layout.parent" : "example.catalog.menu.parent";
        const char* branchId = layoutGroup ? "mms.catalog.layout.branch" : "mms.catalog.menu.branch";
        const char* leafId = layoutGroup ? "mms.catalog.layout.branch.leaf" : "mms.catalog.menu.branch.leaf";
        const char* realChildId = layoutGroup ? "example.catalog.layout.child" : "example.catalog.menu.child";
        const auto createParent = [&]() -> MmsHandle {
            if (!layoutGroup) {
                auto parent = f.Element(parentId, "mms.menu.plugins", MMS_UI_MENU);
                parent.capabilities = MMS_UI_CONTAINER | MMS_UI_HIDE; parent.label = S("Registered menu ancestor");
                MmsHandle handle = 0;
                Check(f.ui.registerElement(f.ui.context, &parent, &handle) == MMS_OK && handle,
                    "create original registered plugin-menu ancestor through the public UI registration path");
                return handle;
            }
            MmsHandle transaction = 0;
            Check(f.ui.beginLayout(f.ui.context, f.registry.Revision(), &transaction) == MMS_OK,
                "begin actual layout-group creation transaction");
            MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_CREATE_GROUP;
            op.elementId = S(parentId); op.parentId = S("mms.toolbar.main.extensions"); op.value.groupLabel = S("Created layout ancestor");
            Check(f.ui.applyLayout(f.ui.context, transaction, &op) == MMS_OK && f.ui.commitLayout(f.ui.context, transaction) == MMS_OK,
                "create a real preference-backed layout group instead of substituting a registered group");
            const auto& record = f.registry.elements.at(parentId);
            Check(record.layoutGroup && !record.nativeObserved && record.handle, "created ancestor has an original layout-group identity");
            return record.handle;
        };
        const auto removeParent = [&](MmsHandle handle) -> MmsResult {
            if (!layoutGroup) return f.ui.unregisterItem(f.ui.context, handle);
            MmsHandle transaction = 0;
            Check(f.ui.beginLayout(f.ui.context, f.registry.Revision(), &transaction) == MMS_OK,
                "begin real layout REMOVE_GROUP transaction");
            MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_REMOVE_GROUP;
            op.elementId = S(parentId); op.expectedGeneration = f.registry.elements.at(parentId).desc.generation;
            Check(f.ui.applyLayout(f.ui.context, transaction, &op) == MMS_OK, "stage REMOVE_GROUP with the current original description generation");
            return f.ui.commitLayout(f.ui.context, transaction);
        };

        const auto parentHandle = createParent();
        auto command = f.Command("example.catalog.lifecycle.command"); MmsHandle commandHandle = 0;
        Check(f.ui.registerCommand(f.ui.context, &command, &commandHandle) == MMS_OK && commandHandle,
            "register actual command for the real-child BUSY control");
        auto realChild = f.Element(realChildId, parentId, layoutGroup ? MMS_UI_BUTTON : MMS_UI_MENU_ITEM);
        if (!layoutGroup) realChild.capabilities = MMS_UI_HIDE;
        realChild.commandId = command.info.id; realChild.label = S("Actual registered child"); MmsHandle realChildHandle = 0;
        Check(f.ui.registerElement(f.ui.context, &realChild, &realChildHandle) == MMS_OK && realChildHandle,
            "register real child under the original ancestor before adding directory-only observations");

        HostUiCatalogPublication publication; const auto scope = layoutGroup ? MMS_UI_CATALOG_MAIN_CONTROLS : MMS_UI_CATALOG_MAIN_MENU;
        publication.availableScopes = MMS_UI_CATALOG_REGISTRY | scope;
        publication.coverageFlags = MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_INCOMPLETE_NATIVE |
            (layoutGroup ? MMS_UI_CATALOG_DECLARED_CONTROLS : MMS_UI_CATALOG_MATERIALIZED_MENU);
        auto branch = Native(branchId, layoutGroup ? 881 : 871, scope); branch.parentId = parentId;
        branch.kind = layoutGroup ? MMS_UI_GROUP : MMS_UI_MENU;
        branch.role = layoutGroup ? MMS_UI_CATALOG_ROLE_UNKNOWN : MMS_UI_CATALOG_ROLE_MENU;
        auto leaf = Native(leafId, layoutGroup ? 882 : 872, scope); leaf.parentId = branchId;
        if (!layoutGroup) {
            branch.knownFields &= ~MMS_UI_CATALOG_BOUNDS; branch.bounds = {};
            leaf.kind = MMS_UI_MENU_ITEM; leaf.role = MMS_UI_CATALOG_ROLE_MENU_ITEM;
            leaf.knownFields &= ~MMS_UI_CATALOG_BOUNDS; leaf.bounds = {};
        }
        publication.entries = {branch, leaf};
        Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish a two-level native observation branch rooted in the exact original registered ancestor");
        auto held = f.Acquire(); const auto digest = Digest(held);
        const auto* observedBranch = Find(held, branchId); const auto* observedLeaf = Find(held, leafId);
        Check(observedBranch && observedLeaf && Str(observedBranch->parentId) == parentId && Str(observedLeaf->parentId) == branchId,
            "directory initially contains both native levels and their declared ancestor chain");
        const std::array<MmsUiTargetRefV1, 2> oldRefs{observedBranch->target, observedLeaf->target};
        const auto beforeBlocked = f.registry.Revision(), beforePreferences = f.registry.PreferencesRevision();
        const auto preferences = f.registry.ExportPreferences();
        Check(removeParent(parentHandle) == MMS_E_BUSY && f.registry.elements.at(parentId).handle == parentHandle &&
            f.registry.Revision() == beforeBlocked && f.registry.PreferencesRevision() == beforePreferences &&
            f.registry.ExportPreferences() == preferences && Digest(held) == digest,
            "a genuine registered child still prevents parent removal atomically; native observations do not weaken that ownership guard");
        Check(f.ui.unregisterItem(f.ui.context, realChildHandle) == MMS_OK, "remove only the genuine registered child before testing observation-only ancestry");
        auto stillAttached = f.Acquire();
        Check(Find(stillAttached, branchId) && Find(stillAttached, leafId),
            "removing an unrelated real sibling does not invalidate a native branch whose original parent handle still exists"); f.Release(stillAttached);
        Check(removeParent(parentHandle) == MMS_OK && !f.registry.elements.count(parentId),
            "ordinary Remove and layout REMOVE_GROUP both succeed when remaining children are directory-only observations");
        const auto checkOrphaned = [&]() {
            auto fresh = f.Acquire();
            Check(!Find(fresh, branchId) && !Find(fresh, leafId) && Digest(held) == digest,
                "new snapshots omit the entire native branch after losing its original ancestor identity while old leased records remain readable");
            f.Release(fresh);
            for (const auto& ref : oldRefs) {
                auto out = EntryMarker(); const auto untouched = out; MmsHandle lease = 456789;
                Check(f.api.resolveElement(f.api.context, &ref, &out, &lease) == MMS_E_STALE && SameBytes(out, untouched) && lease == 456789,
                    "every old target in a multi-level orphaned native chain is STALE with unchanged outputs");
            }
        };
        checkOrphaned();
        const auto rebuiltHandle = createParent();
        Check(rebuiltHandle != parentHandle && f.registry.elements.at(parentId).handle == rebuiltHandle,
            "same textual parent ID is recreated with a distinct original identity");
        checkOrphaned();
        // Only a fresh trusted host observation can bind to the new ancestor.
        // An ordinary registry mutation or matching ID is never that signal.
        publication.entries[0].instance += 100; publication.entries[1].instance += 100;
        Check(f.registry.Catalog().Publish(publication) == MMS_OK, "explicit fresh host publication may observe the recreated ancestor's new instance chain");
        auto republished = f.Acquire(); const auto* newBranch = Find(republished, branchId); const auto* newLeaf = Find(republished, leafId);
        Check(newBranch && newLeaf && newBranch->target.instanceGeneration == publication.entries[0].instance &&
            newLeaf->target.instanceGeneration == publication.entries[1].instance && Digest(held) == digest,
            "fresh publication restores the whole new branch without changing the retired snapshot's original identity bytes");
        for (const auto& ref : oldRefs) {
            auto out = EntryMarker(); const auto untouched = out; MmsHandle lease = 123789;
            Check(f.api.resolveElement(f.api.context, &ref, &out, &lease) == MMS_E_STALE && SameBytes(out, untouched) && lease == 123789,
                "old presentation references cannot resolve even after the same IDs are explicitly observed again");
        }
        f.Release(republished); f.Release(held); f.NoCallbacks();
    }
}

void EffectiveAliasParentCycle() {
    Fixture f;
    // A's real registered description remains A -> toolbar. The valid control
    // candidate publishes B -> A and aliases A -> toolbar. Changing only the
    // alias parent to B creates an effective metadata cycle invisible in the
    // original registration graph: A -> B -> A.
    auto publication = Publication();
    auto branch = Native("mms.catalog.cycle.branch", 1981); branch.parentId = "example.catalog.group";
    branch.kind = MMS_UI_GROUP; branch.role = MMS_UI_CATALOG_ROLE_UNKNOWN;
    auto alias = Native("example.catalog.group", 0); alias.reuseElement = f.group;
    alias.parentId = "mms.toolbar.main.extensions"; alias.kind = MMS_UI_GROUP; alias.role = MMS_UI_CATALOG_ROLE_UNKNOWN;
    alias.knownFields = MMS_UI_CATALOG_LABEL; alias.label = "Observed registered ancestor"; alias.bounds = {}; alias.state = 0;
    publication.entries.push_back(branch); publication.entries.push_back(alias);
    Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish an acyclic alias/native graph whose registered ancestor retains its original description");
    auto held = f.Acquire(); const auto digest = Digest(held);
    const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision(); const auto changes = f.changes;
    const auto original = f.registry.elements.at("example.catalog.group").desc; const auto count = f.registry.elements.size();
    Check(Find(held, "mms.catalog.cycle.branch") && Find(held, "example.catalog.group") &&
        Str(original.parentId) == "mms.toolbar.main.extensions", "valid control establishes both actual metadata edges before testing the cycle");
    publication.entries.back().parentId = "mms.catalog.cycle.branch";
    Check(f.registry.Catalog().Publish(publication) == MMS_E_ARGUMENT, "effective alias A to native B to A cycle is rejected even though registered A still points at a normal anchor");
    auto current = f.Acquire();
    Check(current.revision == held.revision && current.registryRevision == held.registryRevision && Digest(current) == digest && Digest(held) == digest &&
        f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.changes == changes &&
        f.registry.elements.size() == count && SameBytes(f.registry.elements.at("example.catalog.group").desc, original),
        "cycle rejection preserves the complete old directory, leased strings, registered descriptions, preferences and both source revisions");
    f.Release(current); f.Release(held); f.NoCallbacks();
}

void ProtocolValidationAndNativeReadOnlyBoundary() {
    Fixture f; auto publication = Publication(); Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish native read-only identities");
    auto snapshot = f.Acquire(); const auto digest = Digest(snapshot); const auto* observed = Find(snapshot, "mms.catalog.control"); Check(observed, "native entry exists for permission checks");
    const auto ref = observed->target;
    for (uint32_t scopes : {0u, 8u, kAllScopes | 0x80000000u}) {
        auto output = SnapshotMarker(); const auto before = output;
        Check(f.api.acquireSnapshot(f.api.context, scopes, &output) == MMS_E_ARGUMENT && SameBytes(output, before), "scope zero and unknown bits are rejected before allocating or changing output");
    }
    for (unsigned bad = 0; bad < 4; ++bad) {
        auto output = SnapshotMarker(); if (bad == 0) --output.h.size; else if (bad == 1) output.h.major = 2; else if (bad == 2) output.h.minor = 1; else output.reserved = 1;
        const auto before = output; Check(f.api.acquireSnapshot(f.api.context, kAllScopes, &output) != MMS_OK && SameBytes(output, before), "bad snapshot headers and reserved fields are rejected atomically");
        auto caps = OutputMarker<MmsUiCatalogCapabilitiesV1>(); caps.reserved = 0;
        if (bad == 0) --caps.h.size; else if (bad == 1) caps.h.major = 2; else if (bad == 2) caps.h.minor = 1; else caps.reserved = 1;
        const auto oldCaps = caps; Check(f.api.getCapabilities(f.api.context, &caps) != MMS_OK && SameBytes(caps, oldCaps), "bad capability headers and reserved fields leave every output byte unchanged");
    }
    Check(f.api.getCapabilities(f.api.context, nullptr) == MMS_E_ARGUMENT && f.api.acquireSnapshot(f.api.context, kAllScopes, nullptr) == MMS_E_ARGUMENT,
        "null output pointers are refused");
    for (unsigned change = 0; change < 7; ++change) {
        auto expected = ref;
        if (change == 0) --expected.h.size; else if (change == 1) expected.h.major = 2; else if (change == 2) expected.h.minor = 1;
        else if (change == 3) ++expected.element; else if (change == 4) ++expected.instanceGeneration; else if (change == 5) ++expected.projectEpoch; else expected.element = 0;
        auto out = EntryMarker(); const auto before = out; MmsHandle lease = 765;
        Check(f.api.resolveElement(f.api.context, &expected, &out, &lease) != MMS_OK && SameBytes(out, before) && lease == 765,
            "resolve checks the complete header and exact original element/instance/project identity without partial output");
    }
    auto out = EntryMarker(); const auto outBefore = out; MmsHandle lease = 765;
    Check(f.api.resolveElement(f.api.context, nullptr, &out, &lease) == MMS_E_ARGUMENT && SameBytes(out, outBefore) && lease == 765 &&
        f.api.resolveElement(f.api.context, &ref, nullptr, &lease) == MMS_E_ARGUMENT && lease == 765 &&
        f.api.resolveElement(f.api.context, &ref, &out, nullptr) == MMS_E_ARGUMENT && SameBytes(out, outBefore), "resolve validates all pointer arguments before acquiring anything");
    // Even a UiClient carrying the textual host ID gains no mutation authority
    // over observed controls. This exercises record-level protection, not just
    // the ordinary plugin namespace check.
    UiClient host{&f.registry, "mms"}, namespaceOwner{&f.registry, "mms.catalog"}; auto hostApi = f.registry.Api(host); auto namespaceApi = f.registry.Api(namespaceOwner);
    const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision(); const auto savedPreferences = f.registry.ExportPreferences();
    const auto native = f.registry.elements.at("mms.catalog.control").desc;
    auto replacement = native; replacement.label = S("Must not change");
    Check(hostApi.updateElement(hostApi.context, observed->element, &replacement) != MMS_OK && hostApi.unregisterItem(hostApi.context, observed->element) != MMS_OK,
        "read-only observed element cannot be replaced or unregistered through the ordinary UI API");
    auto child = f.Element("example.catalog.forbidden", "mms.catalog.menu", MMS_UI_GROUP); child.capabilities |= MMS_UI_CONTAINER;
    MmsHandle untouched = 98765; Check(f.ui.registerElement(f.ui.context, &child, &untouched) != MMS_OK && untouched == 98765,
        "an observed native menu is not a plugin mount even when its visible role is a container");
    child.id = S("mms.catalog.injected"); child.ownerPlugin = Text(namespaceOwner.id); child.parentId = S("mms.toolbar.main.extensions");
    Check(namespaceApi.registerElement(namespaceApi.context, &child, &untouched) != MMS_OK && untouched == 98765, "plugins cannot claim the trusted native observation namespace");
    auto layout = f.registry.LayoutApi(host); MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.flow = MMS_FLOW_ROW;
    Check(layout.setDefaultHints(layout.context, native.id, &hints) != MMS_OK, "layout hints cannot mutate an observed native control");
    for (uint32_t operation : {MMS_LAYOUT_VISIBLE, MMS_LAYOUT_MOVE, MMS_LAYOUT_RESTORE, MMS_LAYOUT_SIZE}) {
        MmsHandle transaction = 0; Check(hostApi.beginLayout(hostApi.context, f.registry.Revision(), &transaction) == MMS_OK, "begin a real UI layout transaction to verify read-only rejection");
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = operation; op.elementId = native.id;
        op.expectedGeneration = native.generation; op.parentId = S("mms.toolbar.main.extensions");
        if (operation == MMS_LAYOUT_VISIBLE) op.value.visible = 0;
        if (operation == MMS_LAYOUT_SIZE) { op.value.size.width = 90; op.value.size.height = 20; }
        const auto staged = hostApi.applyLayout(hostApi.context, transaction, &op);
        if (staged == MMS_OK) Check(hostApi.commitLayout(hostApi.context, transaction) != MMS_OK, "observed native targets cannot commit hide/move/restore/resize operations");
        else hostApi.cancelLayout(hostApi.context, transaction);
    }
    Check(f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.registry.ExportPreferences() == savedPreferences &&
        SameBytes(f.registry.elements.at("mms.catalog.control").desc, native) && Digest(snapshot) == digest,
        "all rejected native mutations preserve original registry, preferences and prior snapshot memory");
    f.Release(snapshot); f.NoCallbacks();
}

void InvalidPublicationsAreAtomic() {
    Fixture f; auto publication = Publication(); Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish original candidate for failure atomicity");
    auto old = f.Acquire(); const auto oldDigest = Digest(old); const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    const auto elements = f.registry.elements.size(); const auto changes = f.changes;
    for (unsigned bad = 0; bad < 15; ++bad) {
        auto next = publication; auto& entry = next.entries[0];
        switch (bad) {
        case 0: entry.id = "outside.native.namespace"; break;
        case 1: entry.id = "mms.catalog.invalid id"; break;
        case 2: entry.label = std::string("bad\0label", 9); break;
        case 3: entry.label = std::string("\xc0\xaf", 2); break;
        case 4: entry.label.assign(65537, 'x'); break;
        case 5: entry.knownFields |= 0x80000000u; break;
        case 6: entry.state |= 0x80000000u; break;
        case 7: entry.scope = 0; break;
        case 8: entry.scope = 0x80000000u; break;
        case 9: next.availableScopes |= 0x80000000u; break;
        case 10: next.coverageFlags |= 0x80000000u; break;
        case 11: next.entries.push_back(entry); break;
        case 12: entry.reuseElement = f.command; break;
        case 13: { auto alias = Native("example.catalog.button", 901); alias.reuseElement = f.button;
            alias.knownFields = MMS_UI_CATALOG_LABEL; alias.state = 0; alias.bounds = {}; next.entries.push_back(alias); break; }
        case 14: { auto alias = Native("example.catalog.button", 0); alias.reuseElement = f.button;
            alias.knownFields = MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_PRESENTATION; alias.state = 0; alias.bounds = {}; next.entries.push_back(alias); break; }
        }
        Check(f.registry.Catalog().Publish(next) != MMS_OK, "malformed trusted publication is rejected as a whole");
        auto now = f.Acquire(); Check(now.revision == old.revision && Digest(now) == oldDigest && Digest(old) == oldDigest &&
            f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.registry.elements.size() == elements && f.changes == changes,
            "invalid publication never changes metadata revision, observed records, preferences, callback count or earlier immutable memory"); f.Release(now);
    }
    auto valid = publication; valid.entries[0].label.assign(65536, 'z');
    Check(f.registry.Catalog().Publish(valid) == MMS_OK, "a native metadata label of exactly 65536 valid non-NUL UTF-8 bytes is legal");
    std::fill(valid.entries[0].label.begin(), valid.entries[0].label.end(), 'q'); auto copied = f.Acquire();
    const auto* entry = Find(copied, "mms.catalog.control"); Check(entry && entry->label.bytes == 65536 && entry->label.data[0] == 'z' && entry->label.data[65535] == 'z',
        "publication owns its text independently of later changes in the adapter's input vector");
    f.Release(copied); f.Release(old); f.NoCallbacks();
}

void RecoverableAllocationFaults() {
    Fixture f; auto publication = Publication(); Check(f.registry.Catalog().Publish(publication) == MMS_OK, "publish baseline before actual global-new failure injection");
    auto old = f.Acquire(); const auto baseline = Digest(old), revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    unsigned acquisitionFailures = 0; bool acquiredCompletely = false;
    for (long fail = 0; fail < 1024; ++fail) {
        auto out = SnapshotMarker(); const auto untouched = out; allocationFaults = 0; allocationCountdown = fail;
        const auto result = f.api.acquireSnapshot(f.api.context, kAllScopes, &out); allocationCountdown = -1;
        if (result == MMS_OK) { Check(!allocationFaults && Digest(out) == baseline, "uninjected acquisition reaches the complete old metadata image"); f.Release(out); acquiredCompletely = true; break; }
        Check(result == MMS_E_MEMORY && allocationFaults == 1 && SameBytes(out, untouched) && Digest(old) == baseline,
            "every real acquisition allocation failure returns MEMORY with untouched output and live old lease");
        Check(f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences, "failed snapshot preparation cannot edit registry or user layout state"); ++acquisitionFailures;
    }
    Check(acquiredCompletely && acquisitionFailures > 2, "allocation sweep covers actual snapshot arena/record/lease preparation and eventually succeeds");
    auto next = publication; next.entries[0].id = "mms.catalog.replacement"; next.entries[0].label = "Candidate text"; next.entries[0].instance = 811;
    unsigned publicationFailures = 0; bool publishedCompletely = false;
    for (long fail = 0; fail < 2048; ++fail) {
        allocationFaults = 0; allocationCountdown = fail; const auto result = f.registry.Catalog().Publish(next); allocationCountdown = -1;
        if (result == MMS_OK) { Check(!allocationFaults, "complete publication succeeds only after the selected allocation fault lies beyond actual preparation"); publishedCompletely = true; break; }
        Check(result == MMS_E_MEMORY && allocationFaults == 1 && Digest(old) == baseline, "every real publication allocation refusal preserves old immutable memory");
        auto current = f.Acquire(); Check(current.revision == old.revision && Digest(current) == baseline &&
            f.registry.elements.count("mms.catalog.control") == 1 && !f.registry.elements.count("mms.catalog.replacement") &&
            f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences,
            "failed whole-candidate preparation never installs partial observations or advances either source revision"); f.Release(current); ++publicationFailures;
    }
    Check(publishedCompletely && publicationFailures > 2, "publication allocation sweep covers real candidate preparation before an atomic successful swap");
    auto updated = f.Acquire(); Check(updated.revision != old.revision && Find(updated, "mms.catalog.replacement") && !Find(updated, "mms.catalog.control") &&
        Digest(old) == baseline, "successful publication installs the entire replacement while the prior snapshot remains valid"); f.Release(updated); f.Release(old);
    std::array<MmsUiCatalogSnapshotV1, 16> all{};
    for (auto& lease : all) lease = f.Acquire();
    auto refused = SnapshotMarker(); const auto unchanged = refused;
    Check(f.api.acquireSnapshot(f.api.context, kAllScopes, &refused) != MMS_OK && SameBytes(refused, unchanged),
        "failed acquisition/publication sweeps did not leak any of the sixteen allowed lease slots");
    for (const auto& lease : all) f.Release(lease); Check(!f.registry.Catalog().HasLeases(f.reader), "all failed candidate reservations and successful leases are reclaimable"); f.NoCallbacks();
}

void CountAndLeaseQuotas() {
    Fixture f(false); std::array<UiClient, 9> owners{}; std::array<MmsUiCatalogApiV1, 9> apis{};
    std::array<std::array<MmsUiCatalogSnapshotV1, 16>, 8> held{};
    for (unsigned i = 0; i < owners.size(); ++i) { owners[i].registry = &f.registry; owners[i].id = "example.quota." + std::to_string(i); apis[i] = f.registry.Catalog().Api(owners[i]); }
    for (unsigned owner = 0; owner < held.size(); ++owner) {
        for (auto& snapshot : held[owner]) { snapshot.h = Header<MmsUiCatalogSnapshotV1>(); Check(apis[owner].acquireSnapshot(apis[owner].context, MMS_UI_CATALOG_REGISTRY, &snapshot) == MMS_OK,
            "every owner can retain sixteen small snapshots up to the global count limit"); }
        auto extra = SnapshotMarker(); const auto before = extra;
        Check(apis[owner].acquireSnapshot(apis[owner].context, MMS_UI_CATALOG_REGISTRY, &extra) != MMS_OK && SameBytes(extra, before), "seventeenth same-owner lease refuses without consuming another global slot");
    }
    auto extra = SnapshotMarker(); const auto untouched = extra;
    Check(apis[8].acquireSnapshot(apis[8].context, MMS_UI_CATALOG_REGISTRY, &extra) != MMS_OK && SameBytes(extra, untouched), "global 129th lease is rejected even for a fresh owner");
    Check(apis[0].releaseSnapshot(apis[0].context, held[0][0].handle) == MMS_OK, "release exactly one global lease reservation"); held[0][0].handle = 0;
    Check(apis[8].acquireSnapshot(apis[8].context, MMS_UI_CATALOG_REGISTRY, &extra) == MMS_OK && extra.handle, "released count capacity is immediately reusable by another owner");
    Check(apis[8].releaseSnapshot(apis[8].context, extra.handle) == MMS_OK, "release recovered global capacity");
    for (unsigned owner = 0; owner < held.size(); ++owner) for (auto& lease : held[owner]) if (lease.handle)
        Check(apis[owner].releaseSnapshot(apis[owner].context, lease.handle) == MMS_OK, "release each real global lease exactly once");

    // Exercise the shared entry count through real registrations, not direct
    // map injection: six anchors + command registrations consume the same cap.
    std::vector<MmsHandle> commands; commands.reserve(16379);
    for (unsigned i = 0; i < 16378; ++i) {
        const auto id = "example.catalog.command." + std::to_string(i); auto command = f.Command(id.c_str()); MmsHandle handle = 0;
        Check(f.ui.registerCommand(f.ui.context, &command, &handle) == MMS_OK && handle, "register real command toward the joint catalog entry budget"); commands.push_back(handle);
    }
    auto maximum = f.Acquire(MMS_UI_CATALOG_REGISTRY); Check(maximum.elements.count + maximum.registrations.count == 16384,
        "maximum snapshot counts elements and command/property registration descriptions together"); const auto digest = Digest(maximum);
    const auto id = std::string("example.catalog.one_too_many"); auto command = f.Command(id.c_str()); MmsHandle handle = 0;
    Check(f.ui.registerCommand(f.ui.context, &command, &handle) == MMS_OK, "old registration API is independent of catalog capacity");
    auto rejected = SnapshotMarker(); const auto before = rejected;
    Check(f.api.acquireSnapshot(f.api.context, MMS_UI_CATALOG_REGISTRY, &rejected) != MMS_OK && SameBytes(rejected, before) && Digest(maximum) == digest,
        "catalog cannot silently truncate the 16385th combined entry and keeps the original snapshot valid");
    Check(f.ui.unregisterItem(f.ui.context, handle) == MMS_OK, "remove the extra real registration");
    auto recovered = f.Acquire(MMS_UI_CATALOG_REGISTRY); Check(recovered.elements.count + recovered.registrations.count == 16384,
        "entry budget refusal leaves no permanent reservation after the real cause is removed"); f.Release(recovered); f.Release(maximum); f.NoCallbacks();
}

void ActualSnapshotByteBudgets() {
    Fixture f(false); HostUiCatalogPublication large; large.availableScopes = MMS_UI_CATALOG_MAIN_CONTROLS;
    large.entries.reserve(120);
    for (unsigned i = 0; i < 120; ++i) {
        auto entry = Native("mms.catalog.large", 1000 + i); entry.id += "." + std::to_string(i); entry.label.assign(65536, char('a' + i % 26));
        entry.label.replace(0, std::to_string(i).size(), std::to_string(i));
        large.entries.push_back(std::move(entry));
    }
    Check(f.registry.Catalog().Publish(large) == MMS_OK, "120 maximum-length labels fit a complete snapshot including records and storage capacity");
    auto old = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS); const auto digest = Digest(old); Check(old.elements.count == 120, "large catalog never truncates native metadata");
    // Unlike the 1500-node work-limit case, this input has only 7140 ancestor
    // links in total. Its already admitted text leaves insufficient room for
    // the retained parent-proof vectors, reaching storage admission before the
    // approximately 103k-step global parent-walk bound. Input creation is O(N).
    constexpr uint64_t chainRows = 120, chainLinks = chainRows * (chainRows - 1) / 2;
    static_assert(chainLinks == 7140 && chainLinks * 2 < 100000, "both complete parent walks stay below the independent work quota");
    const auto* oldFirst = Find(old, large.entries.front().id); Check(oldFirst, "large baseline has a resolvable original native target");
    const auto oldTarget = oldFirst->target;
    const auto originalRegistryRevision = f.registry.Revision(), originalPreferencesRevision = f.registry.PreferencesRevision();
    const auto originalPreferences = f.registry.ExportPreferences(); const auto originalChanges = f.changes; const auto originalElements = f.registry.elements.size();
    auto storageHeavy = large;
    for (std::size_t i = 1; i < storageHeavy.entries.size(); ++i) storageHeavy.entries[i].parentId = storageHeavy.entries[i - 1].id;
    Check(f.registry.Catalog().Publish(storageHeavy) == MMS_E_MEMORY,
        "120-node valid chain with already admitted maximum labels exceeds text-plus-ancestor storage before the independent parent-work quota");
    auto afterStorageRefusal = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS);
    Check(afterStorageRefusal.revision == old.revision && afterStorageRefusal.registryRevision == old.registryRevision &&
        Digest(afterStorageRefusal) == digest && Digest(old) == digest && f.registry.Revision() == originalRegistryRevision &&
        f.registry.PreferencesRevision() == originalPreferencesRevision && f.registry.ExportPreferences() == originalPreferences &&
        f.changes == originalChanges && f.registry.elements.size() == originalElements,
        "ancestor storage refusal preserves both source revisions, every old entry and label, registry identity, preferences and the held snapshot");
    f.Release(afterStorageRefusal);
    auto resolved = EntryMarker(); MmsHandle resolvedLease = 0;
    Check(f.api.resolveElement(f.api.context, &oldTarget, &resolved, &resolvedLease) == MMS_OK && resolvedLease &&
        SameRef(resolved.target, oldTarget) && Str(resolved.id) == large.entries.front().id && Str(resolved.label) == large.entries.front().label,
        "storage-only admission refusal leaves the original exact target resolvable with its complete maximum-length label");
    Check(f.api.releaseSnapshot(f.api.context, resolvedLease) == MMS_OK, "release old target lease after parent-storage refusal");
    Check(f.registry.Catalog().Publish(large) == MMS_OK, "original flat large publication succeeds after rejecting only its chained storage-heavy copy");
    auto recoveredStorage = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS);
    // A successful explicit republication may advance its publication token;
    // the complete entry image and registry/epoch tokens remain identical.
    auto recoveredImage = recoveredStorage; recoveredImage.revision = old.revision;
    Check(Digest(recoveredImage) == digest && Digest(old) == digest && f.registry.Revision() == originalRegistryRevision &&
        f.registry.PreferencesRevision() == originalPreferencesRevision && f.registry.ExportPreferences() == originalPreferences &&
        f.changes == originalChanges && f.registry.elements.size() == originalElements,
        "flat publication recovery retains the exact former targets, parentless entries and label bytes without leaked parent proofs or registry changes");
    f.Release(recoveredStorage);
    // The byte-quota cases below compare complete snapshots, including the
    // native publication token established by the successful recovery above.
    f.Release(old); old = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS); const auto quotaDigest = Digest(old);
    auto second = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS); auto denied = SnapshotMarker(); const auto unchanged = denied;
    Check(f.api.acquireSnapshot(f.api.context, MMS_UI_CATALOG_MAIN_CONTROLS, &denied) != MMS_OK && SameBytes(denied, unchanged) && Digest(old) == quotaDigest,
        "two near-eight-MiB leases fit sixteen MiB, while a third is rejected before its sixteenth handle slot");
    f.Release(second); auto replacement = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS); Check(Digest(replacement) == quotaDigest, "releasing a large snapshot returns its actual retained byte reservation"); f.Release(replacement);
    auto over = large;
    for (unsigned i = 120; i < 129; ++i) { auto entry = over.entries.front(); entry.id = "mms.catalog.large." + std::to_string(i); entry.instance = 1000 + i;
        entry.label.replace(0, std::to_string(i).size(), std::to_string(i)); over.entries.push_back(std::move(entry)); }
    Check(f.registry.Catalog().Publish(over) != MMS_OK && Digest(old) == quotaDigest, "over-eight-MiB native publication cannot replace the previous whole directory");
    auto after = f.Acquire(MMS_UI_CATALOG_MAIN_CONTROLS); Check(after.revision == old.revision && Digest(after) == quotaDigest,
        "oversized publication retains all previous entries and revision rather than a partial prefix"); f.Release(after); f.Release(old);
    std::array<UiClient, 9> clients{}; std::array<MmsUiCatalogApiV1, 9> apis{}; std::array<MmsUiCatalogSnapshotV1, 9> leases{};
    for (unsigned i = 0; i < clients.size(); ++i) { clients[i].registry = &f.registry; clients[i].id = "example.bytequota." + std::to_string(i); apis[i] = f.registry.Catalog().Api(clients[i]); }
    for (unsigned i = 0; i < 8; ++i) { leases[i].h = Header<MmsUiCatalogSnapshotV1>(); Check(apis[i].acquireSnapshot(apis[i].context, MMS_UI_CATALOG_MAIN_CONTROLS, &leases[i]) == MMS_OK,
        "eight different owners can retain one complete large snapshot under the global sixty-four-MiB budget"); }
    denied = SnapshotMarker(); const auto globalUnchanged = denied;
    Check(apis[8].acquireSnapshot(apis[8].context, MMS_UI_CATALOG_MAIN_CONTROLS, &denied) != MMS_OK && SameBytes(denied, globalUnchanged),
        "global byte capacity rejects a ninth large lease with only eight global handles active");
    Check(apis[0].releaseSnapshot(apis[0].context, leases[0].handle) == MMS_OK, "release global retained bytes from their exact owner"); leases[0].handle = 0;
    leases[8].h = Header<MmsUiCatalogSnapshotV1>(); Check(apis[8].acquireSnapshot(apis[8].context, MMS_UI_CATALOG_MAIN_CONTROLS, &leases[8]) == MMS_OK,
        "global byte reservations recover after release without republishing or restarting");
    for (unsigned i = 0; i < leases.size(); ++i) if (leases[i].handle)
        Check(apis[i].releaseSnapshot(apis[i].context, leases[i].handle) == MMS_OK, "release large snapshot backing stores and owner/global budget charges");
    f.NoCallbacks();
}

void DeepAncestorBudgetIsAtomic() {
    Fixture f; const auto shallow = Publication();
    Check(f.registry.Catalog().Publish(shallow) == MMS_OK, "publish shallow baseline before bounded ancestor-storage admission");
    auto held = f.Acquire(); const auto digest = Digest(held); const auto* oldControl = Find(held, "mms.catalog.control");
    Check(oldControl, "shallow baseline contains its original native target"); const auto originalTarget = oldControl->target;
    const auto revision = f.registry.Revision(), preferences = f.registry.PreferencesRevision();
    const auto preferencesText = f.registry.ExportPreferences(); const auto changes = f.changes; const auto elementCount = f.registry.elements.size();
    HostUiCatalogPublication deep; deep.availableScopes = MMS_UI_CATALOG_REGISTRY | MMS_UI_CATALOG_MAIN_CONTROLS;
    deep.coverageFlags = MMS_UI_CATALOG_REGISTRY_COMPLETE | MMS_UI_CATALOG_DECLARED_CONTROLS | MMS_UI_CATALOG_INCOMPLETE_NATIVE;
    constexpr unsigned count = 1500;
    static_assert(uint64_t(count) * (count - 1) / 2 * sizeof(MmsHandle) > 8ull * 1024 * 1024,
        "even handle-only per-entry ancestor retention exceeds the complete publication budget");
    deep.entries.reserve(count); std::string parent;
    for (unsigned i = 0; i < count; ++i) {
        // Each ID has constant-sized depth-independent text. Only the direct
        // parent is supplied, so this test constructs O(N) input rather than
        // manufacturing an O(N squared) ancestor fixture of its own.
        const auto id = "mms.catalog.deep." + std::to_string(i);
        auto entry = Native(id.c_str(), 10000 + i); entry.parentId = parent;
        entry.kind = MMS_UI_GROUP; entry.role = MMS_UI_CATALOG_ROLE_UNKNOWN;
        entry.knownFields = MMS_UI_CATALOG_LABEL | MMS_UI_CATALOG_VISIBLE | MMS_UI_CATALOG_BOUNDS | MMS_UI_CATALOG_PRESENTATION;
        entry.state = MMS_UI_CATALOG_STATE_VISIBLE; entry.bounds = {0, 0, 1, 1}; entry.ordinal = i;
        deep.entries.push_back(std::move(entry)); parent = id;
    }
    Check(f.registry.Catalog().Publish(deep) == MMS_E_MEMORY,
        "valid deep parent graph refuses MEMORY when text plus retained ancestor proofs exceed eight MiB");
    auto current = f.Acquire();
    Check(current.revision == held.revision && current.registryRevision == held.registryRevision && Digest(current) == digest && Digest(held) == digest &&
        f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.registry.ExportPreferences() == preferencesText &&
        f.changes == changes && f.registry.elements.size() == elementCount && !f.registry.elements.count("mms.catalog.deep.0"),
        "ancestor budget refusal installs no deep prefix and preserves old metadata, original registry, user preferences and held lease bytes");
    f.Release(current);
    auto resolved = EntryMarker(); MmsHandle lease = 0;
    Check(f.api.resolveElement(f.api.context, &originalTarget, &resolved, &lease) == MMS_OK && lease &&
        SameRef(resolved.target, originalTarget) && Str(resolved.id) == "mms.catalog.control" && Str(resolved.label) == "Published native metadata",
        "failed deep-graph preparation does not retire or replace the previous exact native target");
    Check(f.api.releaseSnapshot(f.api.context, lease) == MMS_OK, "release original target lease after ancestor admission refusal");
    Check(f.registry.Catalog().Publish(shallow) == MMS_OK, "shallow host publication succeeds after oversized ancestor candidate is discarded");
    auto recovered = f.Acquire(); const auto* recoveredControl = Find(recovered, "mms.catalog.control");
    Check(recoveredControl && SameRef(recoveredControl->target, originalTarget) && !Find(recovered, "mms.catalog.deep.0") && Digest(held) == digest &&
        f.registry.Revision() == revision && f.registry.PreferencesRevision() == preferences && f.changes == changes,
        "budget recovery leaves no poisoned publication reservation, deep branch or stale replacement identity");
    f.Release(recovered); f.Release(held); f.NoCallbacks();
}
}

int main() {
    const auto run = [](const char* name, void (*body)()) {
        ++cases; std::printf("RUN UI catalog: %s\n", name); std::fflush(stdout); body();
    };
    try {
        run("capabilities, anchors and original handles", CapabilitiesAnchorsAndOriginalHandles);
        run("native metadata, scopes and aliases", NativeMetadataScopesAndAliasing);
        run("cached targets and unrelated accepted subtree", CachedTargetsAndUnrelatedSubtree);
        run("leases, thread, stop and retirement", LeaseThreadStoppingAndRetirement);
        run("registered ancestor removal and same-ID reconstruction", RegisteredAncestorLifecycle);
        run("effective alias/native parent cycle", EffectiveAliasParentCycle);
        run("protocol and native read-only boundary", ProtocolValidationAndNativeReadOnlyBoundary);
        run("invalid publication atomicity", InvalidPublicationsAreAtomic);
        run("recoverable real allocation failures", RecoverableAllocationFaults);
        run("joint entry and owner/global lease limits", CountAndLeaseQuotas);
        run("actual snapshot and retained byte budgets", ActualSnapshotByteBudgets);
        run("bounded deep-ancestor publication budget", DeepAncestorBudgetIsAtomic);
        std::printf("PASS UI catalog: %u cases, %u checks\n", cases, checks); return 0;
    } catch (const std::exception& error) {
        allocationCountdown = -1; std::fprintf(stderr, "FAIL UI catalog exception after %u checks: %s\n", checks, error.what()); return 1;
    } catch (...) {
        allocationCountdown = -1; std::fprintf(stderr, "FAIL UI catalog unknown exception after %u checks\n", checks); return 1;
    }
}
