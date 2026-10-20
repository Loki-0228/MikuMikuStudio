#include <thread>

namespace ui_input_broker_checks {
MmsValue Integer(int64_t value) { MmsValue v{}; v.type = MMS_VALUE_INT; v.value.signedValue = value; return v; }
MmsUiElementV1 Input(const std::string& id, const std::string& owner, const std::string& property) {
    MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = Text(id); e.ownerPlugin = Text(owner);
    e.parentId = S("mms.toolbar.main.extensions"); e.label = S("Editable value"); e.propertyId = Text(property);
    e.kind = MMS_UI_INPUT; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE | MMS_UI_LAYOUT; return e;
}
struct Fixture {
    unsigned finished = 0, reads = 0, releases = 0, edits = 0;
    UiRegistry registry{{}, [this](UiClient&, MmsResult) { ++finished; }};
    UiClient owner{&registry, "example.input"}, foreign{&registry, "example.foreign"};
    MmsUiApiV1 ui = registry.Api(owner), other = registry.Api(foreign);
    std::string propertyId = "example.input.property", elementId = "example.input.field";
    MmsHandle property = 0, element = 0;
    uint64_t revision = 19;
    MmsValue value = Integer(2);
    std::function<void()> duringRead;
    MmsPropertyDescV1 desc{};
    explicit Fixture(bool readonly = false) {
        desc.info.h = Header<MmsPropertyDescV1>(); desc.info.id = Text(propertyId); desc.info.ownerPlugin = Text(owner.id);
        desc.info.valueType = MMS_VALUE_INT; desc.info.label = S("Input");
        desc.info.flags = readonly ? MMS_PROPERTY_READ_ONLY : MMS_PROPERTY_UNDOABLE;
        desc.info.minimum = Integer(0); desc.info.maximum = Integer(100); desc.context = this;
        desc.read = [](void* context, MmsObjectRef, MmsValue* out, MmsHandle* lease) -> MmsResult {
            auto& f = *static_cast<Fixture*>(context); CHECK(f.registry.InCallback()); ++f.reads;
            *out = f.value; *lease = 55; if (f.duringRead) f.duringRead(); return MMS_OK;
        };
        desc.releaseValue = [](void* context, MmsHandle lease) {
            auto& f = *static_cast<Fixture*>(context); CHECK(lease == 55 && f.registry.InCallback()); ++f.releases;
        };
        desc.stage = [](void*, MmsHandle, MmsObjectRef, const MmsValue*) { return MMS_E_NOT_READY; };
        CHECK(ui.registerProperty(ui.context, &desc, &property) == MMS_OK);
        auto e = Input(elementId, owner.id, propertyId); CHECK(ui.registerElement(ui.context, &e, &element) == MMS_OK);
    }
    void Bridges(MmsResult editResult = MMS_OK) {
        registry.SetPropertyInputBridges([this](UiClient& c, uint64_t& out) {
            CHECK(&c == &owner); out = revision; return MMS_OK;
        }, [this, editResult](UiClient& c, const PropertyInputSnapshot& snapshot, const MmsValue& proposed) {
            CHECK(&c == &owner && registry.InvokingClient() == &owner && registry.InCallback());
            CHECK(snapshot.sceneRevision == revision && registry.IsPropertyInputCurrent(snapshot));
            ++edits; if (!editResult) value = proposed; return editResult;
        });
    }
};
void RegistrationAndReadOnly() {
    Fixture nohost; PropertyInputSnapshot snapshot;
    CHECK(nohost.registry.CapturePropertyInput(nohost.elementId, snapshot) == MMS_OK);
    CHECK(!snapshot.editable && !snapshot.sceneRevision && snapshot.value.View().value.signedValue == 2);
    CHECK(nohost.reads == 1 && nohost.releases == 1 && nohost.registry.IsPropertyInputCurrent(snapshot));
    CHECK(nohost.registry.CommitPropertyInput(snapshot, Integer(4)) == MMS_E_NOT_READY && !nohost.edits && !nohost.finished);
    Fixture readonly(true); readonly.Bridges();
    CHECK(readonly.registry.CapturePropertyInput(readonly.elementId, snapshot) == MMS_OK && !snapshot.editable);
    CHECK(readonly.registry.CommitPropertyInput(snapshot, Integer(4)) == MMS_E_NOT_READY);
    auto e = Input(nohost.elementId, nohost.owner.id, nohost.propertyId); MmsHandle untouched = 0x9988;
    const auto reject = [&](MmsUiElementV1 input, MmsResult expected) {
        input.id = S("example.input.rejected");
        CHECK(nohost.ui.registerElement(nohost.ui.context, &input, &untouched) == expected && untouched == 0x9988);
    };
    auto bad = e; bad.propertyId = {}; reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.commandId = S("example.input.command"); reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.surfaceId = S("example.input.surface"); reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.propertyId = S("example.input.missing"); reject(bad, MMS_E_NOT_FOUND);
    bad = e; bad.capabilities |= MMS_UI_SHORTCUT; reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.capabilities |= MMS_UI_CONTAINER; reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.parentId = S("mms.menu.plugins"); reject(bad, MMS_E_ARGUMENT);
    bad = e; bad.ownerPlugin = Text(nohost.foreign.id); bad.id = S("example.foreign.input");
    CHECK(nohost.other.registerElement(nohost.other.context, &bad, &untouched) == MMS_E_ARGUMENT && untouched == 0x9988);
    CHECK(nohost.ui.unregisterItem(nohost.ui.context, nohost.property) == MMS_E_BUSY);
    MmsUiLayoutHintsV1 hints{}; hints.h = Header<MmsUiLayoutHintsV1>(); hints.overflow = MMS_UI_OVERFLOW_MENU;
    auto layout = nohost.registry.LayoutApi(nohost.owner);
    CHECK(layout.setDefaultHints(layout.context, Text(nohost.elementId), &hints) == MMS_E_UNSUPPORTED);
    MmsLayoutOpV1 shortcut{}; shortcut.h = Header<MmsLayoutOpV1>(); shortcut.operation = MMS_LAYOUT_SHORTCUT;
    shortcut.elementId = Text(nohost.elementId); shortcut.expectedGeneration = nohost.registry.elements.at(nohost.elementId).desc.generation;
    shortcut.value.shortcut = {'Q', 0, 0, 0}; MmsHandle tx = 0;
    CHECK(nohost.ui.beginLayout(nohost.ui.context, nohost.registry.Revision(), &tx) == MMS_OK);
    CHECK(nohost.ui.applyLayout(nohost.ui.context, tx, &shortcut) == MMS_OK);
    CHECK(nohost.ui.commitLayout(nohost.ui.context, tx) == MMS_E_UNSUPPORTED);
}
void CanonicalAndIdentity() {
    Fixture f; f.Bridges(); PropertyInputSnapshot snapshot;
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK && snapshot.editable && snapshot.sceneRevision == f.revision);
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(2)) == MMS_OK && f.edits == 0 && !f.finished);
    for (auto value : {Integer(-1), Integer(101)}) CHECK(f.registry.CommitPropertyInput(snapshot, value) == MMS_E_ARGUMENT);
    auto malformed = Integer(3); malformed.components = 1;
    CHECK(f.registry.CommitPropertyInput(snapshot, malformed) == MMS_E_ARGUMENT);
    CHECK(f.registry.WithReadOnlyCallback([&] { return f.registry.CommitPropertyInput(snapshot, Integer(4)); }) == MMS_E_BUSY);
    bool threadCurrent = true; MmsResult captureThread = MMS_OK, commitThread = MMS_OK;
    std::thread thread([&] { PropertyInputSnapshot out; threadCurrent = f.registry.IsPropertyInputCurrent(snapshot);
        captureThread = f.registry.CapturePropertyInput(f.elementId, out); commitThread = f.registry.CommitPropertyInput(snapshot, Integer(4)); });
    thread.join(); CHECK(!threadCurrent && captureThread == MMS_E_THREAD && commitThread == MMS_E_THREAD);
    f.value = Integer(3); CHECK(f.registry.CommitPropertyInput(snapshot, Integer(4)) == MMS_E_STALE && !f.edits);
    f.value = Integer(2); ++f.revision; CHECK(f.registry.CommitPropertyInput(snapshot, Integer(4)) == MMS_E_STALE && !f.edits);
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK);
    auto input = f.registry.elements.at(f.elementId).desc; input.label = S("relabeled without changing binding");
    CHECK(f.ui.updateElement(f.ui.context, f.element, &input) == MMS_OK && f.registry.IsPropertyInputCurrent(snapshot));
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(4)) == MMS_OK && f.edits == 1 && f.value.value.signedValue == 4 && !f.finished);
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK);
    auto replacement = f.desc; replacement.info.label = S("same-handle new property");
    CHECK(f.ui.updateProperty(f.ui.context, f.property, &replacement) == MMS_OK);
    CHECK(f.registry.properties.at(f.propertyId)->handle == snapshot.property->handle && !f.registry.IsPropertyInputCurrent(snapshot));
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(6)) == MMS_E_STALE && f.edits == 1);
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK);
    f.owner.stopping = true; CHECK(!f.registry.IsPropertyInputCurrent(snapshot));
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(6)) == MMS_E_STALE); f.owner.stopping = false;
    input = Input(f.elementId, f.owner.id, f.propertyId);
    CHECK(f.ui.unregisterItem(f.ui.context, f.element) == MMS_OK && !f.registry.IsPropertyInputCurrent(snapshot));
    MmsHandle replacementElement = 0; CHECK(f.ui.registerElement(f.ui.context, &input, &replacementElement) == MMS_OK);
    CHECK(replacementElement != f.element && !f.registry.IsPropertyInputCurrent(snapshot));
    // Failed captures keep the caller's last complete snapshot.
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK); const auto oldRevision = snapshot.sceneRevision;
    f.duringRead = [&] { ++f.revision; };
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_E_STALE && snapshot.sceneRevision == oldRevision);
    CHECK(f.reads == f.releases && !f.registry.HasLeases(f.owner));
}
void RevokedIntent() {
    Fixture f; f.Bridges(); PropertyInputSnapshot snapshot;
    CHECK(f.registry.CapturePropertyInput(f.elementId, snapshot) == MMS_OK);
    auto intent = std::make_shared<std::atomic<bool>>(false); snapshot.intent = intent;
    const auto reads = f.reads;
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(7)) == MMS_E_STALE && f.reads == reads && !f.edits);
    intent->store(true, std::memory_order_release);
    f.duringRead = [&] { intent->store(false, std::memory_order_release); };
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(7)) == MMS_E_STALE && f.reads == reads + 1 && !f.edits);
    CHECK(f.reads == f.releases && !f.registry.HasLeases(f.owner) && !f.registry.InCallback());
    f.duringRead = {}; snapshot.intent = std::make_shared<std::atomic<bool>>(true);
    CHECK(f.registry.CommitPropertyInput(snapshot, Integer(7)) == MMS_OK && f.edits == 1);
}
struct IntentStageGuard final : EditStageGuard {
    std::shared_ptr<const std::atomic<bool>> intent;
    explicit IntentStageGuard(std::shared_ptr<const std::atomic<bool>> value): intent(std::move(value)) {}
    bool IsCurrent() const noexcept override { return intent->load(std::memory_order_acquire); }
};
void StoreIntentPhases() {
    // Verify the existing guard checkpoints through every preparation callback
    // and immediately before the first void commit, without weakening them.
    for (unsigned phase = 0; phase != 5; ++phase) {
        PropertyStageCase c; auto intent = std::make_shared<std::atomic<bool>>(true);
        auto guard = std::make_shared<IntentStageGuard>(intent);
        EditParticipantFixture participant{&c.stateA, 0, 8}; auto table = participant.Api("example.stage.a");
        const auto transaction = c.Begin(), other = c.Begin();
        CHECK(c.store.StageProperty(c.a, transaction, guard, [&](PropertyStageToken& token) {
            return c.store.JoinPropertyStage(token, c.a, &table);
        }) == MMS_OK);
        const auto revoke = [&] { intent->store(false, std::memory_order_release); };
        if (phase == 0) participant.onPrepare = revoke;
        if (phase == 1) participant.onLeasedRead = revoke;
        if (phase == 2) participant.onRelease = revoke;
        if (phase == 3) c.host.onReserve = revoke;
        if (phase != 4) CHECK(c.api.prepare(c.api.context, transaction) == MMS_E_STALE);
        else {
            CHECK(c.api.prepare(c.api.context, transaction) == MMS_OK); revoke();
            CHECK(c.api.commit(c.api.context, transaction) == MMS_E_STALE);
        }
        CHECK(participant.cancels == 1 && !participant.commits && !c.stateA.value && !c.host.entries && c.host.revision == 1);
        CHECK(c.store.HasPending() && !c.host.locked && !c.store.InCallback());
        CHECK(c.api.prepare(c.api.context, transaction) == MMS_E_STALE);
        c.api.cancel(c.api.context, other); CHECK(!c.store.HasPending());
    }
}
struct CancelProbe {
    inline static CancelProbe* active = nullptr;
    MmsPropertyDescV1 original{};
    std::shared_ptr<std::atomic<bool>> intent;
    unsigned reads = 0, cancelAtRead = 0;
    bool cancelAfterStage = false;
    CancelProbe() { CHECK(!active); active = this; }
    ~CancelProbe() { active = nullptr; }
    static MmsResult MMS_CALL Read(void* context, MmsObjectRef target, MmsValue* value, MmsHandle* lease) {
        if (!active || active->original.context != context) return MMS_E_FAILED;
        const auto result = active->original.read(context, target, value, lease);
        if (++active->reads == active->cancelAtRead && active->intent)
            active->intent->store(false, std::memory_order_release);
        return result;
    }
    static MmsResult MMS_CALL Stage(void* context, MmsHandle transaction, MmsObjectRef target, const MmsValue* value) {
        if (!active || active->original.context != context) return MMS_E_FAILED;
        const auto result = active->original.stage(context, transaction, target, value);
        if (!result && active->cancelAfterStage && active->intent) active->intent->store(false, std::memory_order_release);
        return result;
    }
};
MmsHandle RegisterInput(ManagerEditCase& f, const std::string& id) {
    const auto property = f.manager->Ui().properties.at(id + ".editable");
    auto api = f.manager->Ui().Api(*property->owner); const std::string inputId = id + ".input", propertyId = id + ".editable";
    auto input = Input(inputId, id, propertyId); MmsHandle handle = 0;
    CHECK(api.registerElement(api.context, &input, &handle) == MMS_OK); return handle;
}
void ManagerChecks(const fs::path& root, const fs::path& dll) {
    const std::string id = "org.example.input-broker";
    {
        ManagerEditCase f(root / "input-nohost", false); ManagerEditProbe probe(f.Package(dll, id)); f.Load(); RegisterInput(f, id);
        PropertyInputSnapshot snapshot; CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        CHECK(!snapshot.editable && snapshot.value.View().value.signedValue == 0 && !snapshot.sceneRevision);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(9)) == MMS_E_NOT_READY && probe.PropertyCalls() == 0);
        f.Close();
    }
    {
        ManagerEditCase f(root / "input-history"); ManagerEditProbe probe(f.Package(dll, id)); f.Load(); const auto handle = RegisterInput(f, id);
        PropertyInputSnapshot snapshot; const auto revision = f.Revision(); const auto gates = *f.allowed;
        CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        CHECK(snapshot.element == handle && snapshot.editable && snapshot.sceneRevision == revision && *f.allowed == gates && !f.host->entered);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(0)) == MMS_OK && *f.allowed == gates && !probe.PropertyCalls());
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(9)) == MMS_OK && probe.Value() == 9 && probe.PropertyCalls() == 1);
        CHECK(f.Revision() == revision + 1 && f.host->history.UndoCount() == 1 && f.observations->published == 1 && f.observations->clean);
        CHECK(!probe.PropertyCandidates() && !f.host->attached->HasPending());
        CHECK(f.Invoke(id, "undo") == MMS_OK && probe.Value() == 0); CHECK(f.Invoke(id, "redo") == MMS_OK && probe.Value() == 9);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(11)) == MMS_E_STALE && probe.PropertyCalls() == 1);
        CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        auto record = snapshot.property; auto api = f.manager->Ui().Api(*record->owner); auto desc = record->desc;
        desc.info.label = S("new input metadata"); CHECK(api.updateProperty(api.context, record->handle, &desc) == MMS_OK);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(11)) == MMS_E_STALE && probe.PropertyCalls() == 1);
        f.Close();
    }
    {
        ManagerEditCase f(root / "input-exact-failure"); ManagerEditProbe probe(f.Package(dll, id)); f.Load(); RegisterInput(f, id);
        const auto revision = f.Revision(); CHECK(f.Invoke(id, "property-pending", revision) == MMS_OK); const auto other = probe.Pending();
        PropertyInputSnapshot snapshot; CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        probe.PropertyMode(2); CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(9)) == MMS_E_ARGUMENT);
        CHECK(probe.Pending() == other && probe.PropertyCandidates() == 1 && probe.Cancels() == 1 && !probe.Value());
        CHECK(f.host->attached->HasPending() && f.Revision() == revision && !f.host->history.UndoCount() && !f.observations->published);
        probe.PropertyMode(0); CHECK(f.Invoke(id, "finish") == MMS_OK && probe.Value() == 7 && !probe.PropertyCandidates());
        CHECK(f.host->history.UndoCount() == 1 && f.observations->published == 1); f.Close();
    }
    {
        ManagerEditCase f(root / "input-canonical-change"); ManagerEditProbe probe(f.Package(dll, id)); f.Load(); RegisterInput(f, id);
        PropertyInputSnapshot snapshot; CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK); const auto revision = f.Revision();
        StoredData data; data.schema = 1; data.bytes.resize(sizeof(int)); const int changed = 5; std::memcpy(data.bytes.data(), &changed, sizeof(changed));
        StoredRecords restore; restore.emplace(id, std::move(data)); CHECK(f.manager->Storage().PrepareRestore(std::move(restore)) == MMS_OK);
        CHECK(f.manager->Storage().Commit() && probe.Value() == changed && f.Revision() == revision);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(9)) == MMS_E_STALE && !probe.PropertyCalls() && !f.host->history.UndoCount());
        f.Close();
    }
    for (unsigned cancelAt : {1u, 4u, 0u}) {
        ManagerEditCase f(root / ("input-intent-cancel-" + std::to_string(cancelAt)));
        ManagerEditProbe probe(f.Package(dll, id)); f.Load(); RegisterInput(f, id);
        CancelProbe callback; const auto record = f.manager->Ui().properties.at(id + ".editable"); callback.original = record->desc;
        auto wrapped = callback.original; wrapped.read = CancelProbe::Read; wrapped.stage = CancelProbe::Stage;
        auto ui = f.manager->Ui().Api(*record->owner); CHECK(ui.updateProperty(ui.context, record->handle, &wrapped) == MMS_OK);
        const auto revision = f.Revision(); CHECK(f.Invoke(id, "property-pending", revision) == MMS_OK);
        const auto other = probe.Pending(); CHECK(other && probe.PropertyCandidates() == 1);
        PropertyInputSnapshot snapshot; CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        auto intent = std::make_shared<std::atomic<bool>>(true); snapshot.intent = intent; callback.intent = intent;
        callback.reads = 0; callback.cancelAtRead = cancelAt; callback.cancelAfterStage = cancelAt == 0;
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(9)) == MMS_E_STALE);
        CHECK(!intent->load(std::memory_order_acquire) && probe.Pending() == other && probe.PropertyCandidates() == 1);
        CHECK(probe.Cancels() == (cancelAt == 1 ? 0 : 1) && probe.Value() == 0 && f.Revision() == revision);
        CHECK(!f.host->history.UndoCount() && !f.observations->published && f.host->attached->HasPending());
        callback.cancelAtRead = 0; callback.cancelAfterStage = false; callback.intent.reset();
        CHECK(f.Invoke(id, "finish") == MMS_OK && probe.Value() == 7 && !probe.PropertyCandidates());
        CHECK(f.host->history.UndoCount() == 1 && f.observations->published == 1 && !f.host->attached->HasPending());
        CHECK(f.manager->Ui().CapturePropertyInput(id + ".input", snapshot) == MMS_OK);
        snapshot.intent = std::make_shared<std::atomic<bool>>(true);
        CHECK(f.manager->Ui().CommitPropertyInput(snapshot, Integer(11)) == MMS_OK && probe.Value() == 11);
        CHECK(f.host->history.UndoCount() == 2 && f.observations->published == 2 && !f.host->attached->HasPending()); f.Close();
    }
}
}
static void UiInputBrokerChecks(const fs::path& root, const fs::path& dll) {
    ui_input_broker_checks::RegistrationAndReadOnly();
    ui_input_broker_checks::CanonicalAndIdentity();
    ui_input_broker_checks::RevokedIntent();
    ui_input_broker_checks::StoreIntentPhases();
    ui_input_broker_checks::ManagerChecks(root, dll);
}
