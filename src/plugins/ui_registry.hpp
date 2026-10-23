#pragma once
#include "common.hpp"
#include "ui_refresh.hpp"
#include "ui_property_value.hpp"
#include "ui_catalog.hpp"
#include <mms/ui.h>
#include <mms/surface.h>
#include <mms/layout.h>
#include <mms/refresh_state.h>
#include <map>
#include <deque>
#include <string_view>

namespace mikumikustudio::plugins {
inline bool NativeControlAnchor(std::string_view id) noexcept {
    return id == "mms.toolbar.main.extensions" || id == "mms.properties.main.extensions" ||
        id == "mms.sidebar.left.extensions" || id == "mms.sidebar.right.extensions";
}
struct Json;
class UiRegistry;
struct UiClient { UiRegistry* registry; std::string id; bool stopping = false, unsafeToUnload = false; };
struct Arena {
    std::deque<std::string> strings;
    std::vector<std::unique_ptr<unsigned char[]>> blocks;
    MmsString Copy(MmsString);
    MmsValue Copy(const MmsValue&);
    MmsSpan Choices(MmsSpan);
    MmsSpan Arguments(MmsSpan);
};
struct CommandRecord { MmsHandle handle = 0; UiClient* owner = nullptr; std::shared_ptr<Arena> arena; MmsCommandDescV1 desc{}; };
struct PropertyRecord { MmsHandle handle = 0; uint64_t registrationGeneration = 0; UiClient* owner = nullptr; std::shared_ptr<Arena> arena; MmsPropertyDescV1 desc{}; };
struct PropertyStageFrame;
struct PropertyInputSnapshot {
    std::string elementId, propertyId;
    MmsHandle element = 0;
    std::shared_ptr<PropertyRecord> property;
    UiPropertyValue value;
    uint64_t sceneRevision = 0;
    bool editable = false;
    // A native draft revokes only its own in-flight request. Legacy internal
    // callers can omit the token; it never crosses the public C ABI.
    std::shared_ptr<const std::atomic<bool>> intent;
};
struct ElementRecord {
    // Pure host observations share registration handles but never participate
    // in plugin layout, presentation, preferences or refresh topology.
    bool nativeObserved = false;
    MmsHandle handle = 0;
    std::shared_ptr<Arena> arena;
    MmsUiElementV1 desc{};
    std::string defaultParent;
    bool visible = true;
    float width = 0, height = 0;
    uint32_t key = 0, modifiers = 0, scope = 0;
    uint64_t order = 0;
    bool userOrder = false;
    bool layoutGroup = false;
    MmsUiLayoutHintsV1 defaultHints{}, effectiveHints{};
    bool userHints = false;
};
struct SurfaceRecord {
    MmsHandle handle = 0;
    UiClient* owner = nullptr;
    std::shared_ptr<Arena> arena;
    MmsUiSurfaceDescV1 desc{};
};
struct StateContentBudget { std::atomic<uint64_t> bytes{0}; };
struct ButtonStateContent {
    uint32_t fields = 0;
    // INPUT shares the same provider, builder, ownership and STATE queue.
    bool input = false, enabled = true, readOnly = false, visible = true;
    // Command-owned state, never writable by the text provider.
    bool checked = false;
    std::wstring label, description, disabledReason, help;
    std::shared_ptr<StateContentBudget> owner, global;
    uint64_t charge = 0;
    ~ButtonStateContent() { if (charge) { owner->bytes.fetch_sub(charge); global->bytes.fetch_sub(charge); } }
};
class UiRegistry {
    friend class UiCatalog;
    struct Lease;
    struct Layout;
    struct View;
    struct Preferences;
    DWORD thread_ = GetCurrentThreadId();
    uint64_t revision_ = 1, next_ = 1, generation_ = 1, nextPresentation_ = 1;
    std::function<void(uint64_t)> layoutChanged_;
    std::function<void(const std::string&, uint64_t)> propertyChanged_;
    uint64_t nextPropertyGeneration_ = 1;
    unsigned callbackDepth_ = 0;
    UiClient* invoking_ = nullptr;
    const PropertyStageFrame* propertyStage_ = nullptr;
    std::function<MmsResult(UiClient&, MmsHandle, const std::shared_ptr<PropertyRecord>&,
        MmsObjectRef, const MmsValue&)> propertyStageBridge_;
    std::function<MmsResult(UiClient&, uint64_t&)> propertyInputRevision_;
    std::function<MmsResult(UiClient&, const PropertyInputSnapshot&, const MmsValue&)> propertyInputEdit_;
    std::function<void(UiClient&, MmsResult)> commandFinished_;
    std::map<MmsHandle, std::unique_ptr<Lease>> leases_;
    std::map<MmsHandle, std::unique_ptr<Layout>> layouts_;
    std::map<std::string, std::unique_ptr<View>> views_;
    UiRefreshQueue refresh_;
    std::unique_ptr<Preferences> preferences_;
    uint64_t preferencesRevision_ = 1;
    std::function<void()> changed_;
    std::unique_ptr<UiCatalog> catalog_;
    uint64_t geometryRevision_ = 0, geometryUiRevision_ = 0;
    std::map<std::string, MmsUiLayoutItemV1> geometry_;
    bool geometryReady_ = false;
    struct StateProvider;
    struct StateBuilder;
    std::map<MmsHandle, std::shared_ptr<StateProvider>> stateProviders_;
    std::map<MmsHandle, MmsHandle> stateProviderElements_;
    std::map<MmsHandle, StateBuilder*> stateBuilders_;
    std::map<std::string, std::shared_ptr<StateContentBudget>> stateContentOwners_;
    std::shared_ptr<StateContentBudget> stateContentGlobal_ = std::make_shared<StateContentBudget>();
    MmsResult StateContentCaps(UiClient&, MmsUiStateContentCapabilitiesV1*);
    MmsResult StateContentProvider(UiClient&, const MmsUiStateContentDescV1*, MmsHandle*, MmsHandle replace = 0);
    MmsResult RemoveStateContentProvider(UiClient&, MmsHandle);
    MmsResult WriteStateContent(UiClient&, MmsHandle, const MmsUiStateContentV1*);
    void ForgetStateContent(MmsHandle) noexcept;
    MmsResult Check(UiClient&, bool mutate = false) const;
    bool Own(UiClient&, MmsString id, MmsString owner) const;
    bool Exists(const std::string&) const;
    void Changed();
    MmsResult Command(UiClient&, const MmsCommandDescV1*, MmsHandle*, MmsHandle replace = 0);
    MmsResult Element(UiClient&, const MmsUiElementV1*, MmsHandle*, MmsHandle replace = 0);
    MmsResult Property(UiClient&, const MmsPropertyDescV1*, MmsHandle*, MmsHandle replace = 0);
    MmsResult Remove(UiClient&, MmsHandle);
    MmsResult Surface(UiClient&, const MmsUiSurfaceDescV1*, MmsHandle*);
    MmsResult RemoveSurface(UiClient&, MmsHandle);
    MmsResult DefaultHints(UiClient&, MmsString, const MmsUiLayoutHintsV1*);
    MmsResult StageHints(UiClient&, MmsHandle, MmsString, uint64_t, const MmsUiLayoutHintsV1*);
    MmsResult LayoutSnapshot(UiClient&, MmsUiLayoutSnapshotV1*);
    void DestroyView(const std::string&);
    MmsResult Snapshot(UiClient&, uint32_t, MmsUiSnapshotV1*);
    MmsResult State(UiClient&, MmsString, MmsObjectRef, MmsCommandStateV1*, MmsHandle*);
    MmsResult Read(UiClient&, MmsString, MmsObjectRef, MmsValue*, MmsHandle*);
    MmsResult Stage(UiClient&, MmsHandle, MmsString, MmsObjectRef, const MmsValue*);
    MmsResult Begin(UiClient&, uint64_t, MmsHandle*);
    MmsResult Apply(UiClient&, MmsHandle, const MmsLayoutOpV1*);
    MmsResult Commit(UiClient&, MmsHandle);
    void Release(UiClient&, MmsHandle, unsigned kind);
    bool Allowed(const MmsUiElementV1&, const std::map<std::string, ElementRecord>&) const;
    MmsResult MergePreferences(std::map<std::string, ElementRecord>&,
        const std::map<std::string, Json>&, uint64_t& next, bool strict) const;
    std::unique_ptr<Preferences> CapturePreferences(const std::map<std::string, ElementRecord>&,
        const std::map<std::string, Json>&) const;
public:
    using PropertyStageBridge = std::function<MmsResult(UiClient&, MmsHandle,
        const std::shared_ptr<PropertyRecord>&, MmsObjectRef, const MmsValue&)>;
    using PropertyInputRevisionBridge = std::function<MmsResult(UiClient&, uint64_t&)>;
    using PropertyInputEditBridge = std::function<MmsResult(UiClient&, const PropertyInputSnapshot&, const MmsValue&)>;
    std::map<std::string, std::shared_ptr<CommandRecord>> commands;
    std::map<std::string, std::shared_ptr<PropertyRecord>> properties;
    std::map<std::string, ElementRecord> elements;
    std::map<std::string, std::shared_ptr<SurfaceRecord>> surfaces;
    explicit UiRegistry(std::function<void()> changed = {},
        std::function<void(UiClient&, MmsResult)> commandFinished = {});
    ~UiRegistry();
    MmsUiApiV1 Api(UiClient&);
    UiCatalog& Catalog() noexcept { return *catalog_; }
    MmsUiSurfaceApiV1 SurfaceApi(UiClient&);
    MmsUiLayoutApiV1 LayoutApi(UiClient&);
    MmsUiRefreshStateContentApiV1 StateContentApi(UiClient&);
    UiRefreshQueue& Refresh() noexcept { return refresh_; }
    // Request adapter work without editing registry/preferences revisions.
    void RequestPresentation() noexcept;
    MmsResult Invoke(UiClient&, MmsString, MmsObjectRef, MmsSpan);
    // DATA preparation is a read-only UI callback too. Keep the same mutation,
    // dispatch and unload gates as command/property state queries.
    MmsResult WithReadOnlyCallback(const std::function<MmsResult()>&);
    MmsResult WithReadOnlyCallback(MmsResult (*)(void*), void*);
    void SetPropertyStageBridge(PropertyStageBridge bridge) { propertyStageBridge_ = std::move(bridge); }
    void SetLayoutChanged(std::function<void(uint64_t)> changed) { layoutChanged_ = std::move(changed); }
    void SetPropertyChanged(std::function<void(const std::string&, uint64_t)> changed) {
        propertyChanged_ = std::move(changed);
    }
    MmsResult WithPropertyStage(const std::shared_ptr<PropertyRecord>&, MmsHandle,
        const std::function<MmsResult(const PropertyStageFrame&)>&);
    bool IsPropertyStageCurrent(const PropertyStageFrame&) const noexcept;
    bool IsPropertyCurrent(const std::string& cachedId, const PropertyRecord*, const UiClient*) const noexcept;
    void SetPropertyInputBridges(PropertyInputRevisionBridge revision, PropertyInputEditBridge edit) {
        propertyInputRevision_ = std::move(revision); propertyInputEdit_ = std::move(edit);
    }
    // Trusted native adapter entry points. They are not part of MmsUiApiV1.
    MmsResult CapturePropertyInput(const std::string&, PropertyInputSnapshot&);
    bool IsPropertyInputCurrent(const PropertyInputSnapshot&) const noexcept;
    bool IsPropertyInputTargetCurrent(MmsHandle, const PropertyRecord*) const noexcept;
    MmsResult CommitPropertyInput(const PropertyInputSnapshot&, const MmsValue&);
    // Keep the read-only callback guard through native state application.
    MmsResult WithElementState(MmsHandle, const std::function<MmsResult(const MmsCommandStateV1&)>&);
    MmsResult WithElementStateContent(const MmsUiTargetRefV1&, uint64_t providerRevision,
        const std::function<MmsResult(const MmsCommandStateV1&, const std::shared_ptr<const ButtonStateContent>&)>&);
    void Retire(UiClient&);
    bool HasLeases(const UiClient&) const;
    uint64_t Revision() const { return revision_; }
    // Internal host persistence. Import publishes only after full validation;
    // missing plugins remain in the document and are applied on registration.
    std::string ExportPreferences() const;
    MmsResult ImportPreferences(std::string_view);
    uint64_t PreferencesRevision() const { return preferencesRevision_; }
    bool InCallback() const { return callbackDepth_ != 0; }
    // Read-only callbacks nested inside invoke never inherit its permission.
    const UiClient* InvokingClient() const noexcept { return callbackDepth_ == 1 ? invoking_ : nullptr; }
    // The native STATE adapter defers suspension only until its exact-view
    // compensation finishes; it must suspend every failed publication itself.
    MmsResult ShowSurface(const std::string& element, HWND parent, const MmsUiRectV1&, uint32_t dpi, bool visible,
        bool deferFailureSuspension = false);
    MmsResult FocusSurface(const std::string& element);
    void DestroyViews();
    // UI-adapter identity. Never derived from description/layout generations.
    uint64_t NewPresentationInstance();
    struct SurfacePresentation {
        HWND window = nullptr; uint64_t instance = 0; bool visible = false;
        MmsUiRectV1 bounds{}; uint32_t dpi = 96;
    };
    // UI-thread read only: does not create a view or invoke plugin callbacks.
    bool ExistingSurface(const std::string& element, SurfacePresentation&) const noexcept;
    struct Geometry { float x, y, width, height; bool visible, overflowed; };
    bool PublishGeometry(const std::map<std::string, Geometry>&, uint64_t revision);
    void SuspendGeometry() noexcept { geometryReady_ = false; }
    MmsResult CheckVisibilityPublication(MmsHandle) const noexcept;
    MmsResult PublishVisibility(MmsHandle, bool visible) noexcept;
};
}
