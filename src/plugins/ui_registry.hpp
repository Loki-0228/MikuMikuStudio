#pragma once
#include "common.hpp"
#include "ui_refresh.hpp"
#include <mms/ui.h>
#include <mms/surface.h>
#include <mms/layout.h>
#include <map>
#include <deque>

namespace mikudancestudio::plugins {
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
struct PropertyRecord { MmsHandle handle = 0; UiClient* owner = nullptr; std::shared_ptr<Arena> arena; MmsPropertyDescV1 desc{}; };
struct ElementRecord {
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
class UiRegistry {
    struct Lease;
    struct Layout;
    struct View;
    struct Preferences;
    DWORD thread_ = GetCurrentThreadId();
    uint64_t revision_ = 1, next_ = 1, generation_ = 1, nextPresentation_ = 1;
    unsigned callbackDepth_ = 0;
    UiClient* invoking_ = nullptr;
    std::function<void(UiClient&, MmsResult)> commandFinished_;
    std::map<MmsHandle, std::unique_ptr<Lease>> leases_;
    std::map<MmsHandle, std::unique_ptr<Layout>> layouts_;
    std::map<std::string, std::unique_ptr<View>> views_;
    UiRefreshQueue refresh_;
    std::unique_ptr<Preferences> preferences_;
    uint64_t preferencesRevision_ = 1;
    std::function<void()> changed_;
    uint64_t geometryRevision_ = 0, geometryUiRevision_ = 0;
    std::map<std::string, MmsUiLayoutItemV1> geometry_;
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
    std::map<std::string, std::shared_ptr<CommandRecord>> commands;
    std::map<std::string, std::shared_ptr<PropertyRecord>> properties;
    std::map<std::string, ElementRecord> elements;
    std::map<std::string, std::shared_ptr<SurfaceRecord>> surfaces;
    explicit UiRegistry(std::function<void()> changed = {},
        std::function<void(UiClient&, MmsResult)> commandFinished = {});
    ~UiRegistry();
    MmsUiApiV1 Api(UiClient&);
    MmsUiSurfaceApiV1 SurfaceApi(UiClient&);
    MmsUiLayoutApiV1 LayoutApi(UiClient&);
    UiRefreshQueue& Refresh() noexcept { return refresh_; }
    MmsResult Invoke(UiClient&, MmsString, MmsObjectRef, MmsSpan);
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
    MmsResult ShowSurface(const std::string& element, HWND parent, const MmsUiRectV1&, uint32_t dpi, bool visible);
    MmsResult FocusSurface(const std::string& element);
    void DestroyViews();
    // UI-adapter identity. Never derived from description/layout generations.
    uint64_t NewPresentationInstance();
    struct SurfacePresentation { HWND window = nullptr; uint64_t instance = 0; bool visible = false; };
    // UI-thread read only: does not create a view or invoke plugin callbacks.
    bool ExistingSurface(const std::string& element, SurfacePresentation&) const noexcept;
    struct Geometry { float x, y, width, height; bool visible, overflowed; };
    bool PublishGeometry(const std::map<std::string, Geometry>&, uint64_t revision);
};
}
