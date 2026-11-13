#include "../../src/plugins/ui_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace {
thread_local bool faultEnabled = false, faultInjected = false;
thread_local std::size_t allocationIndex = 0, failAt = (std::numeric_limits<std::size_t>::max)();
}
void* operator new(std::size_t size) {
    if (faultEnabled && allocationIndex++ == failAt) { faultInjected = true; throw std::bad_alloc{}; }
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
MmsString S(const char* text) { return {text,std::strlen(text)}; }
constexpr const char* groupId = "example.preview.group";
constexpr const char* firstId = "example.preview.first";
constexpr const char* secondId = "example.preview.second";
LayoutPreviewValue Value(MmsHandle element, uint32_t fields, bool visible = true,
                         float width = 120, float height = 40, int32_t order = -9) {
    return {element,fields,visible,width,height,order};
}
struct Fixture {
    unsigned notifications = 0;
    std::function<void()> onChanged;
    UiRegistry registry{[this] { ++notifications; if (onChanged) onChanged(); }};
    UiClient owner{&registry,"example.preview"}, other{&registry,"example.foreign"};
    MmsUiApiV1 api = registry.Api(owner);
    MmsUiLayoutApiV1_1 history = registry.LayoutHistoryApi(owner);
    MmsHandle group = 0, first = 0, second = 0, foreign = 0;
    Fixture() {
        RegisterCommand(owner,"example.preview.run");
        RegisterCommand(other,"example.foreign.run");
        group = RegisterElement(owner,groupId,"mms.toolbar.main.extensions",MMS_UI_GROUP,nullptr);
        first = RegisterElement(owner,firstId,groupId,MMS_UI_BUTTON,"example.preview.run");
        second = RegisterElement(owner,secondId,groupId,MMS_UI_BUTTON,"example.preview.run");
        foreign = RegisterElement(other,"example.foreign.button","mms.toolbar.main.extensions",MMS_UI_BUTTON,"example.foreign.run");
        auto layout = registry.LayoutApi(owner);
        for (const char* id : {firstId,secondId}) {
            auto hints = registry.elements.at(id).effectiveHints;
            hints.minimum[0] = 10; hints.minimum[1] = 10;
            hints.maximum[0] = 300; hints.maximum[1] = 200;
            CHECK(layout.setDefaultHints(layout.context,S(id),&hints) == MMS_OK);
        }
    }
    void RegisterCommand(UiClient& client, const char* id) {
        auto ui = registry.Api(client);
        MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
        command.info.id = S(id); command.info.ownerPlugin = {client.id.data(),client.id.size()};
        command.getState = [](void*,MmsObjectRef,MmsCommandStateV1* state) -> MmsResult {
            state->enabled = state->visible = 1; return MMS_OK;
        };
        command.invoke = [](void*,MmsObjectRef,MmsSpan,MmsHandle) -> MmsResult { return MMS_OK; };
        MmsHandle handle = 0;
        CHECK(ui.registerCommand(ui.context,&command,&handle) == MMS_OK && handle != 0);
    }
    MmsHandle RegisterElement(UiClient& client, const char* id, const char* parent, uint32_t kind, const char* command) {
        auto ui = registry.Api(client);
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>();
        element.id = S(id); element.parentId = S(parent); element.ownerPlugin = {client.id.data(),client.id.size()};
        element.kind = kind; element.capabilities = MMS_UI_HIDE | MMS_UI_MOVE | MMS_UI_RESIZE | MMS_UI_LAYOUT;
        if (kind == MMS_UI_GROUP) element.capabilities |= MMS_UI_CONTAINER;
        if (command) element.commandId = S(command);
        MmsHandle handle = 0;
        CHECK(ui.registerElement(ui.context,&element,&handle) == MMS_OK && handle != 0); return handle;
    }
    EffectiveElementLayout Effective(const char* id = firstId) { return registry.EffectiveLayout(registry.elements.at(id)); }
    LayoutPreviewInfo Info(MmsHandle handle) {
        LayoutPreviewInfo info{};
        CHECK(registry.QueryLayoutPreview(owner,handle,info) == MMS_OK); return info;
    }
    MmsUiLayoutHistoryInfoV1 History() {
        MmsUiLayoutHistoryInfoV1 info{}; info.h = Header<MmsUiLayoutHistoryInfoV1>();
        CHECK(history.queryHistory(history.context,&info) == MMS_OK); return info;
    }
    MmsHandle Begin(const std::vector<LayoutPreviewValue>& values) {
        MmsHandle handle = 0;
        CHECK(registry.BeginLayoutPreview(owner,values,handle) == MMS_OK && handle != 0); return handle;
    }
    MmsResult PersistVisible(const char* id, bool visible) {
        MmsHandle tx = 0; CHECK(api.beginLayout(api.context,registry.Revision(),&tx) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_VISIBLE;
        op.elementId = S(id); op.expectedGeneration = registry.elements.at(id).desc.generation; op.value.visible = visible;
        CHECK(api.applyLayout(api.context,tx,&op) == MMS_OK); return api.commitLayout(api.context,tx);
    }
    MmsResult PersistSize(const char* id, float width, float height) {
        MmsHandle tx = 0; CHECK(api.beginLayout(api.context,registry.Revision(),&tx) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_SIZE;
        op.elementId = S(id); op.expectedGeneration = registry.elements.at(id).desc.generation;
        op.value.size.width = width; op.value.size.height = height;
        CHECK(api.applyLayout(api.context,tx,&op) == MMS_OK); return api.commitLayout(api.context,tx);
    }
};
bool Equal(const EffectiveElementLayout& a,const EffectiveElementLayout& b) {
    return a.visible == b.visible && a.width == b.width && a.height == b.height &&
        std::memcmp(&a.hints,&b.hints,sizeof(a.hints)) == 0;
}
struct Persistent {
    std::string json;
    uint64_t revision;
    uint32_t undo, redo;
    explicit Persistent(Fixture& f) : json(f.registry.ExportPreferences()), revision(f.registry.PreferencesRevision()) {
        const auto h = f.History(); undo = h.undoDepth; redo = h.redoDepth;
    }
    void Same(Fixture& f) const {
        CHECK(json == f.registry.ExportPreferences() && revision == f.registry.PreferencesRevision());
        const auto h = f.History(); CHECK(undo == h.undoDepth && redo == h.redoDepth);
    }
};
struct State {
    Persistent persistent;
    EffectiveElementLayout first, second;
    uint64_t uiRevision, firstGeneration, secondGeneration;
    unsigned notifications;
    explicit State(Fixture& f) : persistent(f), first(f.Effective()), second(f.Effective(secondId)),
        uiRevision(f.registry.Revision()), firstGeneration(f.registry.elements.at(firstId).desc.generation),
        secondGeneration(f.registry.elements.at(secondId).desc.generation), notifications(f.notifications) {}
    void Same(Fixture& f) const {
        persistent.Same(f);
        CHECK(Equal(first,f.Effective()) && Equal(second,f.Effective(secondId)));
        CHECK(uiRevision == f.registry.Revision() && notifications == f.notifications);
        CHECK(firstGeneration == f.registry.elements.at(firstId).desc.generation && secondGeneration == f.registry.elements.at(secondId).desc.generation);
    }
};
void FieldsAndOwnership() {
    Fixture f; const State start(f);
    auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto visible = f.Begin(values);
    CHECK(f.Info(visible).revision == 1 && f.Info(visible).state == LayoutPreviewState::Active);
    start.Same(f); // Claiming fields does not apply the supplied value.
    MmsHandle untouched = 999;
    CHECK(f.registry.BeginLayoutPreview(f.owner,values,untouched) == MMS_E_CONFLICT && untouched == 999);
    CHECK(f.registry.BeginLayoutPreview(f.other,values,untouched) == MMS_E_ARGUMENT && untouched == 999);
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(f.foreign,PreviewVisible)},untouched) == MMS_E_ARGUMENT && untouched == 999);
    LayoutPreviewInfo sentinel{999,LayoutPreviewState::Committed};
    CHECK(f.registry.QueryLayoutPreview(f.other,visible,sentinel) == MMS_E_NOT_FOUND && sentinel.revision == 999 && sentinel.state == LayoutPreviewState::Committed);
    CHECK(f.registry.ApplyLayoutPreview(f.other,visible,1,values) == MMS_E_NOT_FOUND);
    CHECK(f.registry.EndLayoutPreview(f.other,visible,1,false) == MMS_E_NOT_FOUND);
    CHECK(f.registry.ReleaseLayoutPreview(f.other,visible) == MMS_E_NOT_FOUND);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,visible) == MMS_E_BUSY);
    // Begin only reserves fields; even ignored invalid size values cannot
    // become effective before an explicitly validated Apply.
    const auto sized = f.Begin({Value(f.first,PreviewSize,true,-5,(std::numeric_limits<float>::quiet_NaN)())});
    CHECK(Equal(f.Effective(),start.first));
    const auto ordered = f.Begin({Value(f.first,PreviewOrder)});
    const auto group = f.Begin({Value(f.group,PreviewVisible,false)});
    CHECK(f.registry.ApplyLayoutPreview(f.owner,visible,1,values) == MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,sized,1,{Value(f.first,PreviewSize,true,150,60)}) == MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,ordered,1,{Value(f.first,PreviewOrder,true,0,0,-44)}) == MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,group,1,{Value(f.group,PreviewVisible,false)}) == MMS_OK);
    CHECK(!f.Effective().visible && f.Effective().width == 150 && f.Effective().height == 60 && f.Effective().hints.order == -44);
    CHECK(!f.Effective(groupId).visible && f.registry.elements.at(groupId).visible);
    start.persistent.Same(f);
    CHECK(f.registry.elements.at(firstId).visible && f.registry.elements.at(firstId).width == 0 &&
          f.registry.elements.at(firstId).effectiveHints.order == start.first.hints.order);
    const auto revision = f.Info(visible).revision;
    CHECK(revision > 1);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,visible,1,values) == MMS_E_STALE && f.Info(visible).revision == revision);
    CHECK(f.registry.EndLayoutPreview(f.owner,visible,1,false) == MMS_E_STALE);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,visible,revision,values) == MMS_OK && f.Info(visible).revision > revision);
    for (auto handle : {visible,sized,ordered,group}) {
        CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false) == MMS_OK);
        CHECK(f.Info(handle).state == LayoutPreviewState::RolledBack);
        CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false) == MMS_E_NOT_READY);
        CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
        CHECK(f.registry.QueryLayoutPreview(f.owner,handle,sentinel) == MMS_E_NOT_FOUND);
    }
    CHECK(Equal(f.Effective(),start.first)); start.persistent.Same(f);
}
void ValidationAndAtomicity() {
    Fixture f;
    const auto good = std::vector<LayoutPreviewValue>{Value(f.first,PreviewSize,true,180,70),Value(f.second,PreviewSize,true,90,35)};
    const auto handle = f.Begin(good);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,good) == MMS_OK);
    const State before(f); const auto revision = f.Info(handle).revision;
    auto bad = good; bad[0].width = 230; bad[1].width = 301;
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,bad) == MMS_E_ARGUMENT); before.Same(f);
    for (float invalid : {0.f,-1.f,9.f,32769.f,(std::numeric_limits<float>::infinity)(),(std::numeric_limits<float>::quiet_NaN)()}) {
        bad = good; bad[1].width = invalid;
        CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,bad) == MMS_E_ARGUMENT); before.Same(f);
    }
    bad = good; std::swap(bad[0],bad[1]);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,bad) == MMS_E_ARGUMENT); before.Same(f);
    bad = good; bad[1].fields = PreviewVisible;
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,bad) == MMS_E_ARGUMENT); before.Same(f);
    bad = good; bad.pop_back();
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,bad) == MMS_E_ARGUMENT); before.Same(f);
    CHECK(f.Info(handle).revision == revision && f.Info(handle).state == LayoutPreviewState::Active);
    MmsHandle out = 987;
    CHECK(f.registry.BeginLayoutPreview(f.owner,{},out) == MMS_E_ARGUMENT && out == 987);
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(f.second,0)},out) == MMS_E_ARGUMENT && out == 987);
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(f.second,8)},out) == MMS_E_ARGUMENT && out == 987);
    const auto body = [&] { return f.registry.ApplyLayoutPreview(f.owner,handle,revision,good); };
    CHECK(f.registry.WithReadOnlyCallback(body) == MMS_E_BUSY); before.Same(f);
    MmsResult threadApply = MMS_OK, threadQuery = MMS_OK, threadEnd = MMS_OK;
    std::thread worker([&] {
        LayoutPreviewInfo info{};
        threadApply = f.registry.ApplyLayoutPreview(f.owner,handle,revision,good);
        threadQuery = f.registry.QueryLayoutPreview(f.owner,handle,info);
        threadEnd = f.registry.EndLayoutPreview(f.owner,handle,revision,false);
    }); worker.join();
    CHECK(threadApply == MMS_E_THREAD && threadQuery == MMS_E_THREAD && threadEnd == MMS_E_THREAD); before.Same(f);
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,revision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
}
void BaselineAndCommit() {
    Fixture f; const auto baseline = f.Effective();
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewSize,true,170,55)};
    const auto handle = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values) == MMS_OK);
    CHECK(f.PersistVisible(firstId,false) == MMS_OK);
    CHECK(f.Info(handle).state == LayoutPreviewState::Active && !f.Effective().visible && f.Effective().width == 170);
    const Persistent current(f);
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false) == MMS_OK);
    CHECK(!f.Effective().visible && f.Effective().width == baseline.width); current.Same(f);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
    CHECK(f.history.undo(f.history.context) == MMS_OK && f.Effective().visible);
    const auto changed = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible | PreviewSize | PreviewOrder,false,200,80,-101)};
    const auto committed = f.Begin(changed);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,committed,1,changed) == MMS_OK);
    const auto historyBefore = f.History();
    CHECK(f.registry.EndLayoutPreview(f.owner,committed,f.Info(committed).revision,true) == MMS_OK);
    CHECK(f.Info(committed).state == LayoutPreviewState::Committed);
    const auto historyAfter = f.History();
    CHECK(historyAfter.undoDepth == historyBefore.undoDepth + 1 && historyAfter.redoDepth == 0);
    CHECK(!f.registry.elements.at(firstId).visible && f.registry.elements.at(firstId).width == 200 && f.Effective().hints.order == -101);
    CHECK(f.history.undo(f.history.context) == MMS_OK && Equal(f.Effective(),baseline));
    CHECK(f.Info(committed).state == LayoutPreviewState::Committed); // Undo must not reactivate it.
    CHECK(f.history.redo(f.history.context) == MMS_OK && f.Effective().width == 200 && !f.Effective().visible);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,committed) == MMS_OK);
    const auto conflict = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,conflict,1,values) == MMS_OK);
    CHECK(f.PersistSize(firstId,250,90) == MMS_OK);
    CHECK(f.Info(conflict).state == LayoutPreviewState::Revoked && f.Effective().width == 250 && f.Effective().height == 90);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,conflict) == MMS_OK);
}
void StopAndRevoke() {
    Fixture f; const State before(f);
    const auto foreignValues = std::vector<LayoutPreviewValue>{Value(f.foreign,PreviewVisible,false)};
    MmsHandle foreign = 0;
    CHECK(f.registry.BeginLayoutPreview(f.other,foreignValues,foreign) == MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.other,foreign,1,foreignValues) == MMS_OK);
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto handle = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values) == MMS_OK);
    f.owner.stopping = true;
    MmsHandle output = 333;
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(f.second,PreviewVisible)},output) == MMS_E_NOT_READY && output == 333);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,f.Info(handle).revision,values) == MMS_E_NOT_READY);
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,true) == MMS_E_NOT_READY);
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false) == MMS_OK);
    CHECK(f.Info(handle).state == LayoutPreviewState::RolledBack && f.Effective().visible);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
    f.owner.stopping = false;
    const auto revoked = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,revoked,1,values) == MMS_OK);
    f.owner.stopping = true; f.registry.RevokeLayoutPreviews(f.owner);
    const auto info = f.Info(revoked); const auto notifications = f.notifications;
    CHECK(info.state == LayoutPreviewState::Revoked && f.Effective().visible);
    f.registry.RevokeLayoutPreviews(f.owner);
    CHECK(f.Info(revoked).revision == info.revision && f.notifications == notifications);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,revoked) == MMS_OK);
    LayoutPreviewInfo foreignInfo{};
    CHECK(f.registry.QueryLayoutPreview(f.other,foreign,foreignInfo) == MMS_OK && foreignInfo.state == LayoutPreviewState::Active);
    CHECK(!f.registry.EffectiveLayout(f.registry.elements.at("example.foreign.button")).visible);
    CHECK(f.registry.EndLayoutPreview(f.other,foreign,foreignInfo.revision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.other,foreign) == MMS_OK);
    before.persistent.Same(f);
}
void DisjointCommit() {
    for (const bool orderFirst : {true,false}) {
        Fixture f; const auto baseline = f.Effective(); const auto historyBefore = f.History();
        const auto sizes = std::vector<LayoutPreviewValue>{Value(f.first,PreviewSize,true,170,55)};
        const auto orders = std::vector<LayoutPreviewValue>{Value(f.first,PreviewOrder,true,0,0,-44)};
        const auto size = f.Begin(sizes), order = f.Begin(orders);
        CHECK(f.registry.ApplyLayoutPreview(f.owner,size,1,sizes) == MMS_OK);
        CHECK(f.registry.ApplyLayoutPreview(f.owner,order,1,orders) == MMS_OK);
        const auto first = orderFirst ? order : size, remaining = orderFirst ? size : order;
        const auto remainingBefore = f.Info(remaining);
        CHECK(f.registry.EndLayoutPreview(f.owner,first,f.Info(first).revision,true) == MMS_OK);
        CHECK(f.Info(first).state == LayoutPreviewState::Committed);
        CHECK(f.Info(remaining).state == LayoutPreviewState::Active && f.Info(remaining).revision == remainingBefore.revision);
        CHECK(f.Effective().width == 170 && f.Effective().height == 55 && f.Effective().hints.order == -44);
        const auto& persistent = f.registry.elements.at(firstId);
        if (orderFirst) {
            CHECK(persistent.width == baseline.width && persistent.height == baseline.height && persistent.effectiveHints.order == -44);
        } else {
            CHECK(persistent.width == 170 && persistent.height == 55 && persistent.effectiveHints.order == baseline.hints.order);
        }
        CHECK(f.History().undoDepth == historyBefore.undoDepth + 1);
        CHECK(f.registry.EndLayoutPreview(f.owner,remaining,f.Info(remaining).revision,true) == MMS_OK);
        CHECK(f.Info(remaining).state == LayoutPreviewState::Committed);
        CHECK(f.History().undoDepth == historyBefore.undoDepth + 2);
        CHECK(f.registry.elements.at(firstId).width == 170 && f.registry.elements.at(firstId).height == 55 &&
              f.registry.elements.at(firstId).effectiveHints.order == -44);
        CHECK(f.registry.ReleaseLayoutPreview(f.owner,size) == MMS_OK);
        CHECK(f.registry.ReleaseLayoutPreview(f.owner,order) == MMS_OK);
    }
}
void PartialSameValueTakeover() {
    Fixture f; const Persistent baseline(f);
    const auto original = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible | PreviewSize,false,170,55),
                                                        Value(f.second,PreviewVisible,false)};
    const auto handle = f.Begin(original);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,original) == MMS_OK);
    const auto oldRevision = f.Info(handle).revision;
    // Both writes match the stored baseline. They still take ownership of
    // visibility, including an entry whose remaining field mask becomes zero.
    CHECK(f.PersistVisible(firstId,true) == MMS_OK);
    CHECK(f.Info(handle).state == LayoutPreviewState::Active && f.Info(handle).revision > oldRevision);
    CHECK(f.Effective().visible && !f.Effective(secondId).visible && f.Effective().width == 170);
    CHECK(f.PersistVisible(secondId,true) == MMS_OK);
    baseline.Same(f);
    const auto remainingRevision = f.Info(handle).revision;
    CHECK(f.Info(handle).state == LayoutPreviewState::Active && f.Effective(secondId).visible);
    const auto sizes = std::vector<LayoutPreviewValue>{Value(f.first,PreviewSize,false,190,65)};
    const State beforeInvalid(f);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,oldRevision,sizes) == MMS_E_STALE); beforeInvalid.Same(f);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,remainingRevision,original) == MMS_E_ARGUMENT); beforeInvalid.Same(f);
    CHECK(f.Info(handle).revision == remainingRevision);
    // A new session can claim the relinquished field. Applying and committing
    // the old session's surviving size must not overwrite that new owner.
    const auto visibility = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto visible = f.Begin(visibility);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,visible,1,visibility) == MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,remainingRevision,sizes) == MMS_OK);
    CHECK(!f.Effective().visible && f.Effective().width == 190 && f.Effective().height == 65);
    const auto visibleRevision = f.Info(visible).revision;
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,true) == MMS_OK);
    CHECK(f.Info(handle).state == LayoutPreviewState::Committed);
    CHECK(f.Info(visible).state == LayoutPreviewState::Active && f.Info(visible).revision == visibleRevision);
    CHECK(f.registry.elements.at(firstId).visible && f.registry.elements.at(secondId).visible);
    CHECK(f.registry.elements.at(firstId).width == 190 && f.registry.elements.at(firstId).height == 65);
    CHECK(!f.Effective().visible && f.Effective(secondId).visible);
    CHECK(f.History().undoDepth == baseline.undo + 1);
    CHECK(f.registry.EndLayoutPreview(f.owner,visible,visibleRevision,false) == MMS_OK);
    CHECK(f.Effective().visible && f.Effective().width == 190);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,visible) == MMS_OK);
}
void ChangedReentry() {
    for (const bool undoInside : {false,true}) {
        Fixture f; const auto baseline = f.Effective(); const auto historyBefore = f.History();
        const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible | PreviewSize,false,180,60)};
        const auto handle = f.Begin(values);
        CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values) == MMS_OK);
        bool armed = true; unsigned callbacks = 0;
        f.onChanged = [&] {
            if (!armed) return;
            armed = false; ++callbacks;
            CHECK(f.Info(handle).state == LayoutPreviewState::Committed);
            CHECK(f.registry.elements.at(firstId).width == 180 && !f.registry.elements.at(firstId).visible);
            CHECK(f.History().undoDepth == historyBefore.undoDepth + 1);
            CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
            if (undoInside) {
                CHECK(f.history.undo(f.history.context) == MMS_OK);
                CHECK(Equal(f.Effective(),baseline));
                CHECK(f.History().undoDepth == historyBefore.undoDepth && f.History().redoDepth == 1);
            }
        };
        CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,true) == MMS_OK);
        f.onChanged = {};
        CHECK(!armed && callbacks == 1);
        LayoutPreviewInfo sentinel{777,LayoutPreviewState::Active};
        CHECK(f.registry.QueryLayoutPreview(f.owner,handle,sentinel) == MMS_E_NOT_FOUND && sentinel.revision == 777);
        if (undoInside) {
            CHECK(Equal(f.Effective(),baseline));
            CHECK(f.History().undoDepth == historyBefore.undoDepth && f.History().redoDepth == 1);
            CHECK(f.history.redo(f.history.context) == MMS_OK);
        }
        CHECK(!f.Effective().visible && f.Effective().width == 180 && f.Effective().height == 60);
        CHECK(f.History().undoDepth == historyBefore.undoDepth + 1 && f.History().redoDepth == 0);
    }
}
void TargetLifetimeAndFailedImport() {
    Fixture f;
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto replaced = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,replaced,1,values) == MMS_OK);
    const auto revision = f.Info(replaced).revision;
    const State beforeFailure(f);
    CHECK(f.registry.ImportPreferences("{invalid-json") != MMS_OK); beforeFailure.Same(f);
    CHECK(f.Info(replaced).state == LayoutPreviewState::Active && f.Info(replaced).revision == revision);
    auto descriptor = f.registry.elements.at(firstId).desc;
    descriptor.parentId = S("example.preview.missing");
    CHECK(f.api.updateElement(f.api.context,f.first,&descriptor) != MMS_OK); beforeFailure.Same(f);
    CHECK(f.Info(replaced).state == LayoutPreviewState::Active && f.Info(replaced).revision == revision);
    descriptor = f.registry.elements.at(firstId).desc;
    CHECK(f.api.updateElement(f.api.context,f.first,&descriptor) == MMS_OK);
    CHECK(f.registry.elements.at(firstId).handle == f.first && f.Effective().visible);
    CHECK(f.Info(replaced).state == LayoutPreviewState::Revoked);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,replaced,f.Info(replaced).revision,values) == MMS_E_NOT_READY);
    CHECK(f.registry.EndLayoutPreview(f.owner,replaced,f.Info(replaced).revision,true) == MMS_E_NOT_READY);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,replaced) == MMS_OK);

    const auto removed = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,removed,1,values) == MMS_OK);
    const auto group = f.Begin({Value(f.group,PreviewVisible,false)});
    CHECK(f.registry.ApplyLayoutPreview(f.owner,group,1,{Value(f.group,PreviewVisible,false)}) == MMS_OK);
    const State beforeBusy(f); const auto groupRevision = f.Info(group).revision;
    CHECK(f.api.unregisterItem(f.api.context,f.group) == MMS_E_BUSY); beforeBusy.Same(f);
    CHECK(f.Info(group).state == LayoutPreviewState::Active && f.Info(group).revision == groupRevision);
    const auto oldHandle = f.first;
    CHECK(f.api.unregisterItem(f.api.context,oldHandle) == MMS_OK);
    CHECK(f.Info(removed).state == LayoutPreviewState::Revoked);
    f.first = f.RegisterElement(f.owner,firstId,groupId,MMS_UI_BUTTON,"example.preview.run");
    CHECK(f.first != oldHandle && f.Effective().visible);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,removed,f.Info(removed).revision,values) == MMS_E_NOT_READY);
    CHECK(f.registry.EndLayoutPreview(f.owner,removed,f.Info(removed).revision,true) == MMS_E_NOT_READY);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,removed) == MMS_OK);
    CHECK(f.Info(group).state == LayoutPreviewState::Active && f.Info(group).revision == groupRevision);
    CHECK(f.registry.EndLayoutPreview(f.owner,group,groupRevision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,group) == MMS_OK);
}
void HostLayoutTokens() {
    Fixture f;
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto preview = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,preview,1,values) == MMS_OK);
    const State before(f); uint64_t untouched = 987;
    for (uint32_t mask : {0u,32u,33u,UINT32_MAX}) {
        CHECK(f.registry.BeginHostLayout(mask,untouched) == MMS_E_ARGUMENT && untouched == 987); before.Same(f);
    }
    CHECK(f.registry.WithReadOnlyCallback([&] { return f.registry.BeginHostLayout(HostLayoutAll,untouched); }) == MMS_E_BUSY);
    CHECK(untouched == 987); before.Same(f);
    MmsResult workerResult = MMS_OK;
    std::thread worker([&] { workerResult = f.registry.BeginHostLayout(HostLayoutToolbar,untouched); }); worker.join();
    CHECK(workerResult == MMS_E_THREAD && untouched == 987); before.Same(f);
    CHECK(!f.registry.GeometryReady());
    std::map<std::string,UiRegistry::Geometry> geometry;
    for (const auto& [id,e] : f.registry.elements) if (e.desc.capabilities & MMS_UI_LAYOUT)
        geometry.emplace(id,UiRegistry::Geometry{0,0,120,40,true,false});
    CHECK(f.registry.PublishGeometry(geometry,f.registry.Revision()) && f.registry.GeometryReady());
    uint64_t old = 0, replacement = 0, properties = 0;
    CHECK(f.registry.BeginHostLayout(HostLayoutToolbar | HostLayoutLeft,old) == MMS_OK && old != 0);
    CHECK(!f.registry.GeometryReady());
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutToolbar | HostLayoutLeft,old));
    CHECK(f.Info(preview).state == LayoutPreviewState::Revoked && f.Effective().visible);
    CHECK(f.registry.BeginHostLayout(HostLayoutToolbar,replacement) == MMS_OK && replacement > old);
    CHECK(!f.registry.IsHostLayoutCurrent(HostLayoutToolbar | HostLayoutLeft,old));
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutLeft,old));
    CHECK(f.registry.BeginHostLayout(HostLayoutProperties,properties) == MMS_OK && properties > replacement);
    bool workerCurrent = true, workerGeometry = true;
    CHECK(f.registry.PublishGeometry(geometry,f.registry.Revision()) && f.registry.GeometryReady());
    std::thread cleanup([&] {
        workerCurrent = f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement);
        workerGeometry = f.registry.GeometryReady();
        f.registry.EndHostLayout(replacement);
    }); cleanup.join();
    CHECK(!workerCurrent && !workerGeometry && f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement));
    f.registry.EndHostLayout(old);
    CHECK(!f.registry.IsHostLayoutCurrent(HostLayoutLeft,old));
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement));
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutProperties,properties));
    CHECK(!f.registry.IsHostLayoutCurrent(0,replacement) && !f.registry.IsHostLayoutCurrent(16,replacement) &&
          !f.registry.IsHostLayoutCurrent(HostLayoutToolbar,0));
    Fixture other; uint64_t foreign = 0;
    CHECK(other.registry.BeginHostLayout(HostLayoutToolbar,foreign) == MMS_OK && foreign > properties);
    f.registry.EndHostLayout(foreign); other.registry.EndHostLayout(replacement);
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement));
    CHECK(other.registry.IsHostLayoutCurrent(HostLayoutToolbar,foreign));
    f.registry.EndHostLayout(replacement); f.registry.EndHostLayout(properties); other.registry.EndHostLayout(foreign);
    CHECK(!f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement));
    CHECK(f.Info(preview).state == LayoutPreviewState::Revoked && f.Effective().visible);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,preview,f.Info(preview).revision,values) == MMS_E_NOT_READY);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,preview) == MMS_OK);
}
void HostLayoutPartialTakeover() {
    Fixture f;
    constexpr const char* propertyId = "example.preview.property-button";
    constexpr const char* leftId = "example.preview.left-button";
    constexpr const char* rightId = "example.preview.right-button";
    const auto property = f.RegisterElement(f.owner,propertyId,"mms.properties.main.extensions",MMS_UI_BUTTON,"example.preview.run");
    const auto left = f.RegisterElement(f.owner,leftId,"mms.sidebar.left.extensions",MMS_UI_BUTTON,"example.preview.run");
    const auto right = f.RegisterElement(f.owner,rightId,"mms.sidebar.right.extensions",MMS_UI_BUTTON,"example.preview.run");
    const Persistent baseline(f);
    const auto original = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible | PreviewSize,false,170,55),
        Value(property,PreviewSize,true,80,30),Value(left,PreviewVisible,false),Value(right,PreviewVisible,false)};
    const auto session = f.Begin(original);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,session,1,original) == MMS_OK);
    const auto previousRevision = f.Info(session).revision;
    uint64_t host = 0;
    CHECK(f.registry.BeginHostLayout(HostLayoutToolbar | HostLayoutLeft,host) == MMS_OK);
    CHECK(f.Info(session).state == LayoutPreviewState::Active && f.Info(session).revision > previousRevision);
    CHECK(f.Effective().visible && f.Effective().width == 0 && f.Effective(leftId).visible);
    CHECK(f.Effective(propertyId).width == 80 && !f.Effective(rightId).visible);
    const auto revision = f.Info(session).revision;
    MmsHandle untouched = 888;
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(f.first,PreviewVisible)},untouched) == MMS_E_BUSY && untouched == 888);
    CHECK(f.registry.BeginLayoutPreview(f.owner,{Value(left,PreviewVisible)},untouched) == MMS_E_BUSY && untouched == 888);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,session,previousRevision,original) == MMS_E_STALE);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,session,revision,original) == MMS_E_ARGUMENT);
    const auto remaining = std::vector<LayoutPreviewValue>{Value(property,PreviewSize,true,95,35),Value(right,PreviewVisible,false)};
    CHECK(f.registry.ApplyLayoutPreview(f.owner,session,revision,remaining) == MMS_OK);
    CHECK(f.Effective(propertyId).width == 95 && !f.Effective(rightId).visible);
    // Unrelated mounts continue accepting new previews and normal cleanup.
    const auto propertyVisible = f.Begin({Value(property,PreviewVisible,false)});
    CHECK(f.registry.ApplyLayoutPreview(f.owner,propertyVisible,1,{Value(property,PreviewVisible,false)}) == MMS_OK);
    CHECK(f.registry.EndLayoutPreview(f.owner,propertyVisible,f.Info(propertyVisible).revision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,propertyVisible) == MMS_OK);
    uint64_t rightHost = 0;
    CHECK(f.registry.BeginHostLayout(HostLayoutRight,rightHost) == MMS_OK);
    CHECK(f.Info(session).state == LayoutPreviewState::Active && f.Effective(rightId).visible);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,session,f.Info(session).revision,{Value(property,PreviewSize,true,100,40)}) == MMS_OK);
    f.registry.EndHostLayout(host); f.registry.EndHostLayout(rightHost);
    CHECK(f.Effective().visible && f.Effective().width == 0 && f.Effective(leftId).visible && f.Effective(rightId).visible);
    const auto fresh = f.Begin({Value(f.first,PreviewVisible,false)});
    CHECK(f.registry.EndLayoutPreview(f.owner,fresh,f.Info(fresh).revision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,fresh) == MMS_OK);
    uint64_t all = 0;
    CHECK(f.registry.BeginHostLayout(HostLayoutAll,all) == MMS_OK);
    CHECK(f.Info(session).state == LayoutPreviewState::Revoked && f.Effective(propertyId).width == 0);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,session) == MMS_OK);
    f.registry.EndHostLayout(all); baseline.Same(f);
}
void HostLayoutReentryAndAllocation() {
    Fixture f;
    const auto left = f.RegisterElement(f.owner,"example.preview.reentry-left","mms.sidebar.left.extensions",MMS_UI_BUTTON,"example.preview.run");
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewVisible,false)};
    const auto preview = f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,preview,1,values) == MMS_OK);
    const Persistent baseline(f);
    bool armed = true; uint64_t replacement = 0;
    f.onChanged = [&] {
        if (!armed) return;
        armed = false;
        MmsHandle denied = 333;
        CHECK(f.registry.BeginLayoutPreview(f.owner,values,denied) == MMS_E_BUSY && denied == 333);
        CHECK(f.Info(preview).state == LayoutPreviewState::Revoked && f.Effective().visible);
        CHECK(f.registry.BeginHostLayout(HostLayoutToolbar,replacement) == MMS_OK);
    };
    uint64_t untouched = 444;
    CHECK(f.registry.BeginHostLayout(HostLayoutAll,untouched) == MMS_E_STALE && untouched == 444);
    f.onChanged = {};
    CHECK(!armed && f.registry.IsHostLayoutCurrent(HostLayoutToolbar,replacement));
    CHECK(!f.registry.IsHostLayoutCurrent(HostLayoutAll,replacement));
    CHECK(f.Info(preview).state == LayoutPreviewState::Revoked && f.Effective().visible);
    const auto releasedSlot = f.Begin({Value(left,PreviewVisible,false)});
    CHECK(f.registry.EndLayoutPreview(f.owner,releasedSlot,f.Info(releasedSlot).revision,false) == MMS_OK);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,releasedSlot) == MMS_OK);
    f.registry.EndHostLayout(replacement);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,preview) == MMS_OK);
    // Only notification topology rebuilding may allocate. Its failure cannot
    // leave a partially acquired host mask or report rollback after takeover.
    uint64_t all = 0;
    allocationIndex = 0; failAt = 0; faultInjected = false; faultEnabled = true;
    MmsResult result = MMS_E_FAILED;
    try { result = f.registry.BeginHostLayout(HostLayoutAll,all); }
    catch (...) { faultEnabled = false; CHECK(false); }
    faultEnabled = false;
    CHECK(faultInjected && result == MMS_OK && all != 0 && f.registry.IsHostLayoutCurrent(HostLayoutAll,all));
    f.registry.EndHostLayout(all); baseline.Same(f);
}
enum class AllocAction { Begin, Apply, Commit };
LayoutPreviewValue Parent(MmsHandle element,MmsHandle parent,uint32_t fields=PreviewParent) {
    auto value=Value(element,fields);value.parent=parent;return value;
}
std::string ParentOf(Fixture& f,const char* id) {return String(f.registry.EffectiveParent(f.registry.elements.at(id)));}
constexpr const char* secondGroup="example.preview.destination";
MmsHandle Destination(Fixture& f,const char* parent="mms.toolbar.main.extensions") {
    return f.RegisterElement(f.owner,secondGroup,parent,MMS_UI_GROUP,nullptr);
}
void ParentBasicsAndPublicBoundary() {
    Fixture f;const auto destination=Destination(f);const Persistent saved(f);
    auto values=std::vector<LayoutPreviewValue>{Parent(f.first,destination)};
    const auto handle=f.Begin(values);
    CHECK(ParentOf(f,firstId)==groupId);saved.Same(f);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values)==MMS_OK);
    CHECK(ParentOf(f,firstId)==secondGroup && String(f.registry.elements.at(firstId).desc.parentId)==groupId);
    CHECK(f.registry.HasLayoutPreviewFields(f.first,PreviewParent));saved.Same(f);
    const auto& observed=f.registry.elements.at(firstId);
    allocationIndex=0;failAt=0;faultInjected=false;faultEnabled=true;
    const auto borrowed=f.registry.EffectiveParent(observed);
    const auto visible=f.registry.ControlVisibility(observed);
    faultEnabled=false;
    CHECK(!faultInjected && allocationIndex==0 && borrowed.bytes==std::strlen(secondGroup) &&
          std::memcmp(borrowed.data,secondGroup,size_t(borrowed.bytes))==0 && visible==ControlLayoutVisibility::Visible);
    for(unsigned minor:{0u,1u}) {
        const auto api=f.registry.LayoutSessionApi(f.owner,minor);
        MmsUiLayoutSessionCapsV1 caps{};caps.h=Header<MmsUiLayoutSessionCapsV1>();
        CHECK(api.queryCapabilities(api.context,&caps)==MMS_OK && caps.fields==7);
        MmsUiLayoutSessionTargetV1 target{Header<MmsUiLayoutSessionTargetV1>(),f.first,PreviewParent,0};
        MmsSpan span{&target,1,sizeof(target),1};MmsHandle sentinel=123456;
        CHECK(api.begin(api.context,span,&sentinel)==MMS_E_ARGUMENT && sentinel==123456);
        auto output=target;output.element=998;output.fields=99;
        CHECK(api.queryClaim(api.context,handle,f.Info(handle).revision,0,&output)==MMS_E_UNSUPPORTED &&
              output.element==998 && output.fields==99);
    }
    CHECK(f.PersistVisible(secondGroup,false)==MMS_OK);
    CHECK(f.registry.ControlVisibility(f.registry.elements.at(firstId))==ControlLayoutVisibility::TemporaryHidden);
    CHECK(f.PersistVisible(firstId,false)==MMS_OK);
    CHECK(f.registry.ControlVisibility(f.registry.elements.at(firstId))==ControlLayoutVisibility::PersistentHidden);
    CHECK(f.PersistVisible(firstId,true)==MMS_OK);
    CHECK(f.registry.ControlVisibility(f.registry.elements.at(firstId))==ControlLayoutVisibility::TemporaryHidden);
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false)==MMS_OK);
    CHECK(ParentOf(f,firstId)==groupId && f.registry.ControlVisibility(f.registry.elements.at(firstId))==ControlLayoutVisibility::Visible);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle)==MMS_OK);
    CHECK(f.PersistVisible(secondGroup,true)==MMS_OK);
    const auto before=f.History();
    values[0].fields|=PreviewOrder;values[0].order=-47;
    const auto commit=f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,commit,1,values)==MMS_OK);
    CHECK(f.registry.EndLayoutPreview(f.owner,commit,f.Info(commit).revision,true)==MMS_OK);
    CHECK(f.History().undoDepth==before.undoDepth+1 && ParentOf(f,firstId)==secondGroup && f.Effective().hints.order==-47);
    CHECK(f.registry.elements.at(firstId).handle==f.first && f.registry.elements.at(firstId).desc.commandId.bytes!=0);
    CHECK(f.history.undo(f.history.context)==MMS_OK && ParentOf(f,firstId)==groupId);
    CHECK(f.history.redo(f.history.context)==MMS_OK && ParentOf(f,firstId)==secondGroup && f.Effective().hints.order==-47);
}
void ParentGraphAndTakeover() {
    // Batch order intentionally presents the temporarily cyclic MOVE first.
    Fixture f;const auto destination=Destination(f,groupId);
    const auto root=f.registry.elements.at("mms.toolbar.main.extensions").handle;
    auto values=std::vector<LayoutPreviewValue>{Parent(f.group,destination),Parent(destination,root)};
    const auto handle=f.Begin(values);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values)==MMS_OK);
    CHECK(ParentOf(f,groupId)==secondGroup && ParentOf(f,secondGroup)=="mms.toolbar.main.extensions");
    CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,true)==MMS_OK);
    CHECK(ParentOf(f,groupId)==secondGroup && ParentOf(f,secondGroup)=="mms.toolbar.main.extensions");
    CHECK(f.history.undo(f.history.context)==MMS_OK && ParentOf(f,groupId)=="mms.toolbar.main.extensions" && ParentOf(f,secondGroup)==groupId);
    // Compose the same legal graph with two sessions. Retracting the supporting
    // B->root override must revoke A->B, not expose A<->B indefinitely.
    auto lift=std::vector<LayoutPreviewValue>{Parent(destination,root)};
    const auto a=f.Begin(lift);CHECK(f.registry.ApplyLayoutPreview(f.owner,a,1,lift)==MMS_OK);
    auto nest=std::vector<LayoutPreviewValue>{Parent(f.group,destination)};
    const auto b=f.Begin(nest);CHECK(f.registry.ApplyLayoutPreview(f.owner,b,1,nest)==MMS_OK);
    const auto aRevision=f.Info(a).revision,bRevision=f.Info(b).revision;
    auto cyclicApply=lift;cyclicApply[0].parent=f.group;
    CHECK(f.registry.ApplyLayoutPreview(f.owner,a,aRevision,cyclicApply)==MMS_E_ARGUMENT &&
          f.Info(a).revision==aRevision && f.Info(b).revision==bRevision &&
          ParentOf(f,groupId)==secondGroup && ParentOf(f,secondGroup)=="mms.toolbar.main.extensions");
    CHECK(f.registry.EndLayoutPreview(f.owner,a,f.Info(a).revision,false)==MMS_OK);
    CHECK(f.Info(b).state==LayoutPreviewState::Revoked && (f.Info(b).reasons&MMS_LAYOUT_SESSION_TARGET_CHANGED));
    CHECK(ParentOf(f,groupId)=="mms.toolbar.main.extensions" && ParentOf(f,secondGroup)==groupId);
    // Same-root user takeover withdraws Parent just like other field claims.
    auto moved=std::vector<LayoutPreviewValue>{Parent(f.first,destination)};
    const auto c=f.Begin(moved);CHECK(f.registry.ApplyLayoutPreview(f.owner,c,1,moved)==MMS_OK);
    uint64_t token=0;CHECK(f.registry.BeginHostLayout(HostLayoutToolbar,token)==MMS_OK && token);
    CHECK(f.Info(c).state==LayoutPreviewState::Revoked && ParentOf(f,firstId)==groupId);
    f.registry.EndHostLayout(token);
}
void ParentDisjointAndPersistentHide() {
    Fixture f;const auto destination=Destination(f,groupId);
    auto parents=std::vector<LayoutPreviewValue>{Parent(f.first,destination)};
    auto orders=std::vector<LayoutPreviewValue>{Value(f.first,PreviewOrder,true,120,40,-73)};
    const auto move=f.Begin(parents),order=f.Begin(orders);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,move,1,parents)==MMS_OK);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,order,1,orders)==MMS_OK);
    CHECK(f.PersistVisible(groupId,false)==MMS_OK);
    CHECK(f.registry.ControlVisibility(f.registry.elements.at(firstId))==ControlLayoutVisibility::PersistentHidden);
    CHECK(f.PersistVisible(groupId,true)==MMS_OK);
    const auto orderRevision=f.Info(order).revision;
    CHECK(f.registry.EndLayoutPreview(f.owner,move,f.Info(move).revision,true)==MMS_OK);
    CHECK(f.Info(order).state==LayoutPreviewState::Active && f.Info(order).revision==orderRevision &&
          f.Effective().hints.order==-73 && ParentOf(f,firstId)==secondGroup);
    CHECK(f.registry.EndLayoutPreview(f.owner,order,orderRevision,true)==MMS_OK);
    CHECK(f.Effective().hints.order==-73);
    // Same-parent persistent intent must supersede a temporary Parent claim,
    // even though the stored parent is not numerically/textually changing.
    const auto root=f.registry.elements.at("mms.toolbar.main.extensions").handle;
    parents[0].parent=root;const auto preview=f.Begin(parents);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,preview,1,parents)==MMS_OK);
    MmsHandle transaction=0;CHECK(f.api.beginLayout(f.api.context,f.registry.Revision(),&transaction)==MMS_OK);
    MmsLayoutOpV1 operation{};operation.h=Header<MmsLayoutOpV1>();operation.operation=MMS_LAYOUT_MOVE;
    operation.elementId=S(firstId);operation.parentId=S(secondGroup);
    operation.expectedGeneration=f.registry.elements.at(firstId).desc.generation;
    CHECK(f.api.applyLayout(f.api.context,transaction,&operation)==MMS_OK && f.api.commitLayout(f.api.context,transaction)==MMS_OK);
    CHECK(f.Info(preview).state==LayoutPreviewState::Revoked && ParentOf(f,firstId)==secondGroup);
}
void ParentInvalidAndLifetime() {
    Fixture f;const auto destination=Destination(f);
    const auto foreign=f.RegisterElement(f.other,"example.foreign.group","mms.toolbar.main.extensions",MMS_UI_GROUP,nullptr);
    const auto otherRoot=f.registry.elements.at("mms.properties.main.extensions").handle;
    for(auto target:{MmsHandle(0),f.first,f.second,foreign,otherRoot,MmsHandle(999999)}) {
        const State before(f);MmsHandle out=919;
        CHECK(f.registry.BeginLayoutPreview(f.owner,{Parent(f.first,target)},out)!=MMS_OK && out==919);
        before.Same(f);CHECK(ParentOf(f,firstId)==groupId);
    }
    auto valid=std::vector<LayoutPreviewValue>{Parent(f.first,destination)};
    const auto handle=f.Begin(valid);CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,valid)==MMS_OK);
    const auto revision=f.Info(handle).revision;const State applied(f);
    auto invalid=valid;invalid[0].parent=otherRoot;
    CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,invalid)!=MMS_OK);
    applied.Same(f);CHECK(ParentOf(f,firstId)==secondGroup && f.Info(handle).revision==revision);
    CHECK(f.api.unregisterItem(f.api.context,destination)==MMS_OK);
    CHECK(f.Info(handle).state==LayoutPreviewState::Revoked && ParentOf(f,firstId)==groupId);
    const auto replacement=Destination(f);CHECK(replacement!=destination && ParentOf(f,firstId)==groupId);
    // Re-registration under the same handle must also invalidate dependencies.
    valid[0].parent=replacement;const auto second=f.Begin(valid);
    CHECK(f.registry.ApplyLayoutPreview(f.owner,second,1,valid)==MMS_OK);
    auto description=f.registry.elements.at(secondGroup).desc;
    CHECK(f.api.updateElement(f.api.context,replacement,&description)==MMS_OK);
    CHECK(f.Info(second).state==LayoutPreviewState::Revoked && ParentOf(f,firstId)==groupId);
    auto cyclic=std::vector<LayoutPreviewValue>{Parent(f.group,replacement),Parent(replacement,f.group)};
    MmsHandle untouched=919;const State before(f);
    CHECK(f.registry.BeginLayoutPreview(f.owner,cyclic,untouched)==MMS_E_ARGUMENT && untouched==919);before.Same(f);
    // Stop restores the original parent without losing queryable terminal state.
    const auto stop=f.Begin(valid);CHECK(f.registry.ApplyLayoutPreview(f.owner,stop,1,valid)==MMS_OK);
    f.owner.stopping=true;f.registry.RevokeLayoutPreviews(f.owner);
    CHECK(f.Info(stop).state==LayoutPreviewState::Revoked && ParentOf(f,firstId)==groupId);
    CHECK(f.registry.ReleaseLayoutPreview(f.owner,stop)==MMS_OK);
}
struct ParentTopologyProbe {
    UiRegistry& registry;
    RefreshClient client;
    HWND window=nullptr;
    MmsUiRefreshApiV1 api{};
    MmsUiRefreshRequestV1 request{};
    explicit ParentTopologyProbe(Fixture& f):registry(f.registry),client{&registry.Refresh(),f.owner.id} {
        // A private message-only window makes the queue's presentation records
        // valid. Requests are cancelled before Drain: this observes admission,
        // not a replacement for the real NativeUi geometry tests.
        window=CreateWindowExW(0,L"STATIC",L"layout-parent-topology",0,0,0,1,1,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
        CHECK(window!=nullptr);
        registry.Refresh().SetWake([]{return true;});
        registry.Refresh().SetLayout([](MmsHandle){return MMS_OK;});
        std::vector<RefreshPresentation> presentations;
        for(auto handle:{f.group,f.first,f.second}) {
            RefreshPresentation presentation{};presentation.element=handle;presentation.owner=f.owner.id;
            presentation.instance=handle;presentation.window=window;presentation.width=100;presentation.height=30;
            presentation.visible=true;presentation.layoutRefresh=true;presentation.container=handle==f.group;
            presentations.push_back(presentation);
        }
        registry.Refresh().SyncTargets(presentations);api=registry.Refresh().Api(client);
        MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();
        CHECK(api.getTarget(api.context,f.group,&target)==MMS_OK);
        request.h=Header<MmsUiRefreshRequestV1>();request.target=target.target;
        request.kinds=MMS_UI_REFRESH_LAYOUT;request.scope=MMS_UI_REFRESH_SUBTREE;
        Expect(MMS_OK);
    }
    void Expect(MmsResult expected) {
        MmsHandle output=919;const auto result=api.request(api.context,&request,&output);
        CHECK(result==expected);
        if(result==MMS_OK) {
            CHECK(output && output!=919);
            CHECK(api.cancel(api.context,output)==MMS_OK);
            CHECK(api.release(api.context,output)==MMS_OK);
        } else CHECK(output==919);
    }
    ~ParentTopologyProbe() {
        registry.Refresh().Retire(client);
        if(window)DestroyWindow(window);
    }
};
std::size_t ParentAllocationCase(AllocAction action,std::size_t index,bool requireFault) {
    Fixture f;const auto destination=Destination(f);
    const auto values=std::vector<LayoutPreviewValue>{Parent(f.first,destination)};
    MmsHandle handle=0,output=919;
    if(action!=AllocAction::Begin)handle=f.Begin(values);
    if(action==AllocAction::Commit)CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values)==MMS_OK);
    ParentTopologyProbe topology(f);
    const auto parent=ParentOf(f,firstId);const State before(f);const auto revision=handle?f.Info(handle).revision:0;
    allocationIndex=0;failAt=index;faultInjected=false;faultEnabled=true;
    MmsResult result=MMS_E_FAILED;
    try {
        if(action==AllocAction::Begin)result=f.registry.BeginLayoutPreview(f.owner,values,output);
        else if(action==AllocAction::Apply)result=f.registry.ApplyLayoutPreview(f.owner,handle,revision,values);
        else result=f.registry.EndLayoutPreview(f.owner,handle,revision,true);
    } catch(...) {faultEnabled=false;CHECK(false);}
    faultEnabled=false;const auto count=allocationIndex;
    if(requireFault)CHECK(faultInjected);
    const bool publishedFailure=faultInjected && result==MMS_OK;
    if(result==MMS_E_MEMORY) {
        before.Same(f);CHECK(ParentOf(f,firstId)==parent);topology.Expect(MMS_OK);
        if(action==AllocAction::Begin) {
            CHECK(output==919);
            result=f.registry.BeginLayoutPreview(f.owner,values,output);
        }
        else {
            CHECK(f.Info(handle).state==LayoutPreviewState::Active && f.Info(handle).revision==revision);
            result=action==AllocAction::Apply?f.registry.ApplyLayoutPreview(f.owner,handle,revision,values):
                f.registry.EndLayoutPreview(f.owner,handle,revision,true);
        }
    }
    CHECK(result==MMS_OK);
    if(action==AllocAction::Begin) {
        handle=output;CHECK(handle && handle!=919);before.Same(f);
        CHECK(f.Info(handle).state==LayoutPreviewState::Active && f.Info(handle).revision==1 && ParentOf(f,firstId)==parent);
        CHECK(!publishedFailure); // Begin has no post-publication Changed call.
    } else {
        CHECK(ParentOf(f,firstId)==secondGroup && f.registry.Revision()==before.uiRevision+1 &&
              f.notifications==before.notifications+1 && f.Info(handle).revision==revision+1);
        if(action==AllocAction::Apply) {
            before.persistent.Same(f);
            CHECK(f.Info(handle).state==LayoutPreviewState::Active && String(f.registry.elements.at(firstId).desc.parentId)==groupId);
        } else {
            CHECK(f.Info(handle).state==LayoutPreviewState::Committed && String(f.registry.elements.at(firstId).desc.parentId)==secondGroup &&
                  f.History().undoDepth==before.persistent.undo+1 && !f.History().redoDepth &&
                  f.registry.PreferencesRevision()==before.persistent.revision+1);
        }
    }
    if(publishedFailure) {
        topology.Expect(MMS_E_NOT_READY);
        const State published(f);const auto publishedParent=ParentOf(f,firstId);const auto publishedRevision=f.Info(handle).revision;
        // Retry only derived topology publication. Reapplying/committing would
        // hide a duplicate edit or a second history entry.
        f.registry.Refresh().SyncTopology();
        topology.Expect(MMS_OK);published.Same(f);
        CHECK(ParentOf(f,firstId)==publishedParent && f.Info(handle).revision==publishedRevision);
        f.registry.Refresh().SyncTopology();topology.Expect(MMS_OK);published.Same(f);
    } else topology.Expect(MMS_OK);
    if(handle && f.Info(handle).state==LayoutPreviewState::Active)
        CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false)==MMS_OK);
    if(handle)CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle)==MMS_OK);
    return count;
}
void ParentAllocations() {
    for(auto action:{AllocAction::Begin,AllocAction::Apply,AllocAction::Commit}) {
        const auto count=ParentAllocationCase(action,(std::numeric_limits<std::size_t>::max)(),false);
        CHECK(count>0 && count<4096);
        for(std::size_t index=0;index<count;++index)ParentAllocationCase(action,index,true);
        std::printf("layout parent allocation action=%d points=%zu\n",int(action),count);
    }
}
std::size_t AllocationCase(AllocAction action,std::size_t index,bool requireFault) {
    Fixture f;
    const auto values = std::vector<LayoutPreviewValue>{Value(f.first,PreviewSize | PreviewVisible,false,155,45),Value(f.second,PreviewOrder,true,0,0,-88)};
    MmsHandle handle = action == AllocAction::Begin ? 0 : f.Begin(values);
    if (action == AllocAction::Commit) CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,1,values) == MMS_OK);
    const auto revision = handle ? f.Info(handle).revision : 0;
    const State before(f);
    MmsHandle output = 919;
    allocationIndex = 0; failAt = index; faultInjected = false; faultEnabled = true;
    MmsResult result = MMS_E_FAILED;
    try {
        if (action == AllocAction::Begin) result = f.registry.BeginLayoutPreview(f.owner,values,output);
        else if (action == AllocAction::Apply) result = f.registry.ApplyLayoutPreview(f.owner,handle,revision,values);
        else result = f.registry.EndLayoutPreview(f.owner,handle,revision,true);
    } catch (...) { faultEnabled = false; CHECK(false); }
    faultEnabled = false; const auto count = allocationIndex;
    if (requireFault) CHECK(faultInjected);
    if (result == MMS_E_MEMORY) {
        before.Same(f);
        if (action == AllocAction::Begin) {
            CHECK(output == 919);
            handle = f.Begin(values); // Failed claim must leave no reserved fields.
        } else {
            CHECK(f.Info(handle).state == LayoutPreviewState::Active && f.Info(handle).revision == revision);
            if (action == AllocAction::Apply) CHECK(f.registry.ApplyLayoutPreview(f.owner,handle,revision,values) == MMS_OK);
            else CHECK(f.registry.EndLayoutPreview(f.owner,handle,revision,true) == MMS_OK);
        }
    } else {
        CHECK(result == MMS_OK);
        if (action == AllocAction::Begin) { handle = output; before.Same(f); }
        else if (action == AllocAction::Apply) {
            before.persistent.Same(f); CHECK(f.Effective().width == 155 && !f.Effective().visible && f.Effective(secondId).hints.order == -88);
        } else {
            CHECK(f.Info(handle).state == LayoutPreviewState::Committed);
            CHECK(f.History().undoDepth == before.persistent.undo + 1);
            CHECK(f.registry.elements.at(firstId).width == 155 && !f.registry.elements.at(firstId).visible);
        }
    }
    if (handle && f.Info(handle).state == LayoutPreviewState::Active)
        CHECK(f.registry.EndLayoutPreview(f.owner,handle,f.Info(handle).revision,false) == MMS_OK);
    if (handle) CHECK(f.registry.ReleaseLayoutPreview(f.owner,handle) == MMS_OK);
    return count;
}
void AllocationFailures() {
    for (const auto action : {AllocAction::Begin,AllocAction::Apply,AllocAction::Commit}) {
        const auto count = AllocationCase(action,(std::numeric_limits<std::size_t>::max)(),false);
        CHECK(count > 0 && count < 4096);
        for (std::size_t i = 0; i < count; ++i) AllocationCase(action,i,true);
        std::printf("layout preview allocation action=%d points=%zu\n",int(action),count);
    }
}
}
static void RepeatedMoveStorage() {
    Fixture f;
    const auto original = f.registry.elements.at(firstId);
    for (unsigned i = 0; i < 128; ++i) {
        const char* parent = i % 2 ? groupId : "mms.toolbar.main.extensions";
        MmsHandle tx = 0;
        CHECK(f.api.beginLayout(f.api.context, f.registry.Revision(), &tx) == MMS_OK);
        MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.operation = MMS_LAYOUT_MOVE;
        op.elementId = S(firstId); op.parentId = S(parent);
        op.expectedGeneration = f.registry.elements.at(firstId).desc.generation;
        CHECK(f.api.applyLayout(f.api.context, tx, &op) == MMS_OK);
        CHECK(f.api.commitLayout(f.api.context, tx) == MMS_OK);
        const auto& current = f.registry.elements.at(firstId);
        CHECK(current.arena->strings.size() <= 7);
        CHECK(String(current.desc.parentId) == parent);
        CHECK(String(current.desc.commandId) == "example.preview.run");
        CHECK(String(original.desc.parentId) == groupId);
    }
}
int main() {
    try { RepeatedMoveStorage(); FieldsAndOwnership(); ValidationAndAtomicity(); BaselineAndCommit(); StopAndRevoke();
          DisjointCommit(); PartialSameValueTakeover(); ChangedReentry(); TargetLifetimeAndFailedImport();
          HostLayoutTokens(); HostLayoutPartialTakeover(); HostLayoutReentryAndAllocation(); AllocationFailures();
          ParentBasicsAndPublicBoundary();ParentGraphAndTakeover();ParentDisjointAndPersistentHide();
          ParentInvalidAndLifetime();ParentAllocations(); }
    catch (...) { faultEnabled = false; CHECK(false); }
    std::printf("layout_preview_tests: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
