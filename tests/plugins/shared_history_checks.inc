// These exercise the production journal, not the old EditHostFixture. The
// small native participant models ownership/presence; real PMX/X detachment
// (Bullet, render caches, UI selection) still needs its own host integration.
struct HistoryValue : HistoryChange {
    int& live; int before, after;
    int* destroyed = nullptr;
    bool fail = false, stale = false, throwPrepare = false;
    uint64_t bytes = sizeof(HistoryValue);
    explicit HistoryValue(int& value, int a, int b) : live(value), before(a), after(b) {}
    ~HistoryValue() { if (destroyed) ++*destroyed; }
    struct Candidate : PreparedHistoryChange {
        HistoryValue& change; int expected, target;
        Candidate(HistoryValue& c, bool forward) : change(c), expected(forward ? c.before : c.after), target(forward ? c.after : c.before) {}
        bool IsCurrent() const noexcept override { return !change.stale && change.live == expected; }
        void Publish() noexcept override { change.live = target; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (throwPrepare) throw std::bad_alloc();
        if (fail) return MMS_E_NOT_READY;
        auto next = std::make_unique<Candidate>(*this, forward);
        if (!next->IsCurrent()) return MMS_E_STALE;
        out = std::move(next); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return bytes; }
};
struct HistoryObject {
    int value = 0;
    int* destroyed;
    ~HistoryObject() { ++*destroyed; }
};
struct HistoryPresence : HistoryChange {
    SceneIdentity& identities;
    std::shared_ptr<HistoryObject>& slot;
    std::shared_ptr<HistoryObject> object;
    bool insert;
    HistoryPresence(SceneIdentity& ids, std::shared_ptr<HistoryObject>& s,
                    std::shared_ptr<HistoryObject> o, bool add)
        : identities(ids), slot(s), object(std::move(o)), insert(add) {}
    struct Candidate : PreparedHistoryChange {
        HistoryPresence& change; bool present;
        SceneIdentity::Presence identity;
        Candidate(HistoryPresence& c, bool forward)
            : change(c), present(c.insert == forward), identity(c.identities.PreparePresence(c.object.get(), present)) {}
        bool IsCurrent() const noexcept override {
            return identity.IsCurrent() && (present ? !change.slot : change.slot == change.object);
        }
        void Publish() noexcept override {
            identity.Publish();
            if (present) change.slot = change.object; else change.slot.reset();
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        auto candidate = std::make_unique<Candidate>(*this, forward);
        if (!candidate->IsCurrent()) return MMS_E_STALE;
        out = std::move(candidate); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return sizeof(HistoryObject); }
};
static void SharedHistoryChecks() {
    StorageStore storage; SharedHistory history(storage); EditStore edit(history, storage);
    StorageClient sc; sc.id = "example.shared"; storage.Api(sc);
    SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
    CHECK(storage.Register(sc, S("example.shared"), &serializer, sizeof(serializer), &provider) == MMS_OK);
    EditClient client; client.id = sc.id; auto api = edit.Api(client);
    const auto revision = [&] { uint64_t r = 0; CHECK(history.Revision(r) == MMS_OK); return r; };
    auto native = [&](const char* label, std::shared_ptr<HistoryChange> change) {
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        CHECK(history.PrepareNative(label, std::move(change), tx) == MMS_OK);
        CHECK(history.CommitNative(std::move(tx)) == MMS_OK);
    };
    auto plugin = [&](int before, int after, std::shared_ptr<HistoryChange> change = {}) {
        MmsHandle tx = 0; EditParticipantFixture participant{&state, before, after}; auto p = participant.Api("example.shared");
        CHECK(api.begin(api.context, S("plugin operation"), revision(), &tx) == MMS_OK);
        CHECK(api.join(api.context, tx, &p) == MMS_OK);
        if (change) CHECK(edit.JoinNative(client, tx, std::move(change)) == MMS_OK);
        CHECK(api.prepare(api.context, tx) == MMS_OK);
        CHECK(api.commit(api.context, tx) == MMS_OK);
        CHECK(participant.commits == 1 && participant.cancels == 0 && participant.releases == 1);
    };
    int a = 0, b = 0;
    native("model A", std::make_shared<HistoryValue>(a, 0, 1));
    plugin(0, 10);
    native("model B", std::make_shared<HistoryValue>(b, 0, 2));
    CHECK(history.UndoCount() == 3 && *history.NextLabel(false) == "model B");
    CHECK(api.undo(api.context) == MMS_OK && a == 1 && b == 0 && state.value == 10);
    CHECK(api.undo(api.context) == MMS_OK && a == 1 && state.value == 0);
    CHECK(api.undo(api.context) == MMS_OK && a == 0);
    CHECK(api.undo(api.context) == MMS_E_NOT_FOUND);
    CHECK(api.redo(api.context) == MMS_OK && a == 1);
    CHECK(api.redo(api.context) == MMS_OK && state.value == 10);
    CHECK(api.redo(api.context) == MMS_OK && b == 2);
    // One transaction combines native and plugin work in a single node.
    plugin(10, 30, std::make_shared<HistoryValue>(a, 1, 9));
    CHECK(history.UndoCount() == 4 && a == 9 && state.value == 30);
    const auto beforeFailure = revision(); state.fail = true;
    CHECK(api.undo(api.context) == MMS_E_NOT_READY && a == 9 && state.value == 30);
    CHECK(revision() == beforeFailure && history.UndoCount() == 4);
    state.fail = false;
    CHECK(api.undo(api.context) == MMS_OK && a == 1 && state.value == 10);
    CHECK(api.redo(api.context) == MMS_OK && a == 9 && state.value == 30);
    // Three separately committed plugin steps remain three user undo steps.
    plugin(30, 31); plugin(31, 32); plugin(32, 33);
    for (int value : {32, 31, 30}) CHECK(api.undo(api.context) == MMS_OK && state.value == value);
    // A rejected/cancelled prepare must preserve the redo branch.
    auto bad = std::make_shared<HistoryValue>(a, 9, 11); bad->fail = true;
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    CHECK(history.PrepareNative("failed", bad, tx) == MMS_E_NOT_READY && !tx && history.RedoCount() == 3);
    bad->fail = false; CHECK(history.PrepareNative("cancelled", bad, tx) == MMS_OK);
    tx.reset(); CHECK(a == 9 && history.RedoCount() == 3);
    native("branch", bad); CHECK(a == 11 && history.RedoCount() == 0);
    CHECK(api.redo(api.context) == MMS_E_NOT_FOUND);
    // Native preparation/validation also fails without publishing serializers.
    bad->throwPrepare = true;
    const auto nativeFailure = revision();
    CHECK(api.undo(api.context) == MMS_E_MEMORY && a == 11 && revision() == nativeFailure);
    bad->throwPrepare = false; bad->stale = true;
    CHECK(api.undo(api.context) == MMS_E_STALE && a == 11 && revision() == nativeFailure);
    bad->stale = false;
    // Prepared transactions cannot cross a project reset.
    CHECK(history.PrepareNative("old project", std::make_shared<HistoryValue>(a, 11, 12), tx) == MMS_OK);
    CHECK(history.Reset() == MMS_OK);
    CHECK(history.CommitNative(std::move(tx)) == MMS_E_STALE && a == 11 && !history.UndoCount());
    history.Collect();
    // Missing or replaced serializers do not cause history to skip a node.
    plugin(30, 40); CHECK(storage.Unregister(sc, provider) == MMS_OK);
    CHECK(api.undo(api.context) == MMS_E_NOT_READY && state.value == 40 && history.UndoCount() == 1);
    CHECK(storage.Register(sc, S("example.shared"), &serializer, sizeof(serializer), &provider) == MMS_OK);
    state.value = 40;
    CHECK(api.undo(api.context) == MMS_OK && state.value == 30);
    // Prepared native publication is invalidated by an interleaved commit.
    CHECK(history.PrepareNative("stale", std::make_shared<HistoryValue>(a, 11, 12), tx) == MMS_OK);
    plugin(30, 41);
    CHECK(history.CommitNative(std::move(tx)) == MMS_E_STALE && a == 11);
    MmsResult threadResult = 0; std::thread worker([&] { threadResult = history.History(false); }); worker.join();
    CHECK(threadResult == MMS_E_THREAD);
    // The native participant's validation precedes plugin commit.
    auto stale = std::make_shared<HistoryValue>(a, 11, 12);
    MmsHandle handle = 0; EditParticipantFixture p{&state, 41, 42}; auto ptable = p.Api("example.shared");
    CHECK(api.begin(api.context, S("stale mixed"), revision(), &handle) == MMS_OK);
    CHECK(api.join(api.context, handle, &ptable) == MMS_OK && edit.JoinNative(client, handle, stale) == MMS_OK);
    CHECK(api.prepare(api.context, handle) == MMS_OK); stale->stale = true;
    CHECK(api.commit(api.context, handle) == MMS_E_STALE && !p.commits && p.cancels == 1 && state.value == 41 && a == 11);
    CHECK(history.Reset() == MMS_OK); history.Collect();

    SceneIdentity identities; int destroyed = 0;
    int firstToken=0, secondToken=0;
    const auto firstRef=identities.Get(&firstToken).object, secondRef=identities.Get(&secondToken).object;
    auto hideFirst=identities.PreparePresence(&firstToken,false), hideSecond=identities.PreparePresence(&secondToken,false);
    CHECK(hideSecond.Publish() && hideFirst.Publish());
    CHECK(!hideFirst.Publish() && !identities.Retained(&firstToken)->present && !identities.Retained(&secondToken)->present);
    auto showFirst=identities.PreparePresence(&firstToken,true), showSecond=identities.PreparePresence(&secondToken,true);
    CHECK(showFirst.Publish() && showSecond.Publish());
    CHECK(!Same(firstRef,identities.Get(&firstToken).object) && !Same(secondRef,identities.Get(&secondToken).object));
    auto retiredPresence=identities.PreparePresence(&firstToken,false);identities.Forget(&firstToken);
    CHECK(!retiredPresence.Publish());
    std::shared_ptr<HistoryObject> slot;
    auto object = std::make_shared<HistoryObject>(); object->destroyed = &destroyed; object->value = 123;
    const auto initial = identities.Get(object.get()).object;
    const auto bone = identities.Element(object.get(), MMS_CHANNEL_BONES, 0);
    auto absent = identities.PreparePresence(object.get(), false); CHECK(absent.Publish());
    auto imported = std::make_shared<HistoryPresence>(identities, slot, object, true);
    native("import object", imported);
    const auto importedRef = identities.Get(object.get()).object;
    CHECK(slot == object && Same(initial.id, importedRef.id) && !Same(initial, importedRef));
    plugin(41, 50);
    native("delete object", std::make_shared<HistoryPresence>(identities, slot, object, false));
    CHECK(!slot && identities.Known(initial.id) && Throws([&] { identities.Get(object.get()); }));
    CHECK(api.undo(api.context) == MMS_OK && slot == object && slot->value == 123);
    CHECK(!Same(identities.Get(object.get()).object, importedRef) && Same(identities.Element(object.get(), MMS_CHANNEL_BONES, 0), bone));
    CHECK(api.undo(api.context) == MMS_OK && state.value == 41 && slot == object);
    CHECK(api.undo(api.context) == MMS_OK && !slot);
    CHECK(api.redo(api.context) == MMS_OK && slot == object && slot->value == 123);
    CHECK(api.redo(api.context) == MMS_OK && state.value == 50);
    CHECK(api.redo(api.context) == MMS_OK && !slot);
    imported.reset(); object.reset(); CHECK(destroyed == 0);
    CHECK(history.Reset() == MMS_OK && destroyed == 0);
    history.Collect(); CHECK(destroyed == 1);

    // Budget eviction and redo truncation defer resource destruction until idle.
    int evicted = 0, value = 0;
    SharedHistory bounded(storage, {}, 2, 1024);
    for (int i = 0; i < 3; ++i) {
        auto change = std::make_shared<HistoryValue>(value, i, i + 1); change->destroyed = &evicted;
        CHECK(bounded.PrepareNative("bounded", change, tx) == MMS_OK && bounded.CommitNative(std::move(tx)) == MMS_OK);
    }
    CHECK(bounded.UndoCount() == 2 && evicted == 0); bounded.Collect(); CHECK(evicted == 1);
    CHECK(bounded.History(false) == MMS_OK && value == 2);
    auto huge = std::make_shared<HistoryValue>(value, 2, 4); huge->bytes = UINT64_MAX;
    CHECK(bounded.PrepareNative("large", huge, tx) == MMS_E_MEMORY && bounded.RedoCount() == 1 && value == 2);
    CHECK(bounded.Reset() == MMS_OK && evicted == 1); bounded.Collect(); CHECK(evicted == 3);
    bool busy = true; SharedHistory gated(storage, [&] { return busy ? MMS_E_BUSY : MMS_OK; });
    CHECK(gated.PrepareNative("gate", std::make_shared<HistoryValue>(value, 2, 3), tx) == MMS_E_BUSY);
    busy = false; CHECK(gated.PrepareNative("gate", std::make_shared<HistoryValue>(value, 2, 3), tx) == MMS_OK);
    busy = true; CHECK(gated.CommitNative(std::move(tx)) == MMS_E_BUSY && value == 2 && !gated.UndoCount());
    // A throwing commit freezes the common journal, including native entry
    // points. It cannot report the partly changed plugin state as undoable.
    EditParticipantFixture violation{&state, 50, 60}; violation.throwCommit = true;
    auto violating = violation.Api("example.shared");
    CHECK(api.begin(api.context, S("bad commit"), revision(), &handle) == MMS_OK);
    CHECK(api.join(api.context, handle, &violating) == MMS_OK && api.prepare(api.context, handle) == MMS_OK);
    CHECK(api.commit(api.context, handle) == MMS_E_FAILED && violation.commits == 1 && !violation.cancels);
    CHECK(history.History(false) == MMS_E_NOT_READY && history.Reset() == MMS_E_NOT_READY && !history.UndoCount());
    CHECK(history.PrepareNative("after poison", std::make_shared<HistoryValue>(a, 11, 12), tx) == MMS_E_NOT_READY);
    std::cout << "shared history journal and transaction granularity PASS (native scene lifecycle integration pending)\n";
}
