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
static void SharedHistoryResetChecks() {
    StorageStore storage; SharedHistory history(storage);
    int value = 0, destroyed = 0;
    std::unique_ptr<SharedHistory::NativeTransaction> pending;
    const auto add = [&](int before, int after) {
        auto change = std::make_shared<HistoryValue>(value, before, after);
        change->destroyed = &destroyed;
        CHECK(history.PrepareNative("old scene", change, pending) == MMS_OK);
        CHECK(history.CommitNative(std::move(pending)) == MMS_OK);
    };
    add(0, 1); add(1, 2);
    CHECK(history.History(false) == MMS_OK && value == 1);
    uint64_t revision = 0, unchanged = 0;
    CHECK(history.Revision(revision) == MMS_OK);
    {
        std::unique_ptr<SharedHistory::ResetTransaction> reset;
        CHECK(history.PrepareReset(reset) == MMS_OK);
        CHECK(storage.Prepare({}) == MMS_OK);
        CHECK(history.Reset() == MMS_E_BUSY && history.History(false) == MMS_E_BUSY);
        storage.Cancel();
    }
    CHECK(history.Revision(unchanged) == MMS_OK && unchanged == revision);
    CHECK(history.UndoCount() == 1 && history.RedoCount() == 1 && destroyed == 0);
    {
        auto change = std::make_shared<HistoryValue>(value, 1, 3);
        change->destroyed = &destroyed;
        CHECK(history.PrepareNative("pending old scene", change, pending) == MMS_OK);
    }
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    CHECK(history.PrepareReset(reset) == MMS_OK && storage.Prepare({}) == MMS_OK);
    MmsResult fromWorker = MMS_OK;
    std::thread worker([&] { fromWorker = reset->Commit(); }); worker.join();
    CHECK(fromWorker == MMS_E_THREAD && history.UndoCount() == 1 && destroyed == 0);
    CHECK(reset->Commit() == MMS_OK && destroyed == 3);
    CHECK(!history.UndoCount() && !history.RedoCount() && !history.RetainedBytes());
    CHECK(history.Revision(unchanged) == MMS_OK && unchanged == revision + 1);
    CHECK(reset->Commit() == MMS_E_ARGUMENT && history.History(false) == MMS_E_BUSY);
    CHECK(storage.Commit()); reset.reset();
    CHECK(history.CommitNative(std::move(pending)) == MMS_E_STALE && value == 1);
    CHECK(history.History(false) == MMS_E_NOT_FOUND);
    // Native tokens may outlive the journal itself, but none of their scene
    // candidates or resource owners may do so.
    std::unique_ptr<SharedHistory::ResetTransaction> orphanReset;
    {
        auto shortLived = std::make_unique<SharedHistory>(storage);
        auto change = std::make_shared<HistoryValue>(value, 1, 4); change->destroyed = &destroyed;
        CHECK(shortLived->PrepareNative("orphan", change, pending) == MMS_OK);
        change.reset();
        CHECK(shortLived->PrepareReset(orphanReset) == MMS_OK);
    }
    CHECK(destroyed == 4 && orphanReset->Commit() == MMS_E_STALE);
    pending.reset(); orphanReset.reset();
}
static void SharedHistoryEditBoundaryChecks();
static void SharedHistoryDynamicBudgetChecks() {
    struct Dynamic final : HistoryValue {
        uint64_t forwardBytes = 160, reverseBytes = 80;
        bool dynamic = true;
        using HistoryValue::HistoryValue;
        struct Prepared final : HistoryValue::Candidate {
            uint64_t targetBytes;
            bool dynamic;
            Prepared(Dynamic& c, bool forward) : Candidate(c, forward),
                targetBytes(forward ? c.forwardBytes : c.reverseBytes), dynamic(c.dynamic) {}
            bool TargetRetainedBytes(uint64_t& out) const noexcept override { out = targetBytes; return dynamic; }
        };
        MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
            if (fail) return MMS_E_NOT_READY;
            out = std::make_unique<Prepared>(*this, forward); return MMS_OK;
        }
    };
    StorageStore storage; SharedHistory history(storage, {}, 8, 1000);
    int value = 0;
    auto change = std::make_shared<Dynamic>(value, 0, 1);
    change->bytes = 1; // A static placeholder cannot authorize the larger target.
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    change->forwardBytes = UINT64_MAX;
    CHECK(history.PrepareNative("dynamic", change, tx) == MMS_E_MEMORY && !tx && !history.RetainedBytes());
    change->forwardBytes = 160;
    change->bytes = UINT64_MAX;
    CHECK(history.PrepareNative("dynamic", change, tx) == MMS_OK && !history.RetainedBytes());
    tx.reset();
    change->bytes = 1;
    CHECK(value == 0 && !history.UndoCount() && !history.RetainedBytes());
    CHECK(history.PrepareNative("dynamic", change, tx) == MMS_OK && history.CommitNative(std::move(tx)) == MMS_OK);
    const auto full = history.RetainedBytes(), overhead = full - 160;
    CHECK(full > 160 && value == 1);
    CHECK(history.History(false) == MMS_OK && value == 0 && history.RetainedBytes() == overhead + 80);
    change->forwardBytes = 320;
    CHECK(history.History(true) == MMS_OK && value == 1 && history.RetainedBytes() == overhead + 320);
    uint64_t revision = 0, current = 0;
    CHECK(history.Revision(revision) == MMS_OK);
    change->reverseBytes = UINT64_MAX;
    CHECK(history.History(false) == MMS_E_MEMORY && value == 1 && history.RetainedBytes() == overhead + 320);
    CHECK(history.Revision(current) == MMS_OK && current == revision && history.UndoCount() == 1 && !history.RedoCount());
    change->reverseBytes = 80;
    CHECK(history.History(false) == MMS_OK);
    change->forwardBytes = 160;
    CHECK(history.History(true) == MMS_OK);
    int secondValue = 0;
    auto second = std::make_shared<Dynamic>(secondValue, 0, 2);
    second->bytes = 1;
    CHECK(history.PrepareNative("second", second, tx) == MMS_OK && history.CommitNative(std::move(tx)) == MMS_OK);
    const auto beforeGrowth = history.RetainedBytes();
    // Individually below the cap, but it cannot evict the first node on undo.
    second->reverseBytes = 1000 - overhead - 1;
    CHECK(history.History(false) == MMS_E_MEMORY && secondValue == 2 && value == 1 &&
        history.UndoCount() == 2 && history.RetainedBytes() == beforeGrowth);
    second->reverseBytes = 80;
    CHECK(history.History(false) == MMS_OK && secondValue == 0 && history.UndoCount() == 1 && history.RedoCount() == 1);
    CHECK(history.Reset() == MMS_OK && !history.RetainedBytes()); history.Collect();

    // An unavailable dynamic result may leave output unspecified. It cannot
    // override the fixed charge, even if the getter writes zero before false.
    value = 0; change->dynamic = false; change->forwardBytes = change->reverseBytes = 0;
    change->bytes = UINT64_MAX;
    CHECK(history.PrepareNative("dynamic", change, tx) == MMS_E_MEMORY && !tx && !history.RetainedBytes());
    change->bytes = 160;
    CHECK(history.PrepareNative("dynamic", change, tx) == MMS_OK && history.CommitNative(std::move(tx)) == MMS_OK);
    CHECK(history.RetainedBytes() == overhead + 160 && value == 1);
    CHECK(history.History(false) == MMS_OK && value == 0 && history.RetainedBytes() == overhead + 160);
    CHECK(history.History(true) == MMS_OK && value == 1 && history.RetainedBytes() == overhead + 160);
    CHECK(history.Reset() == MMS_OK); history.Collect();
    change->dynamic = true; change->forwardBytes = 160; change->reverseBytes = 80;

    // Serializer failures must not publish the budget or the native target.
    StorageClient sc; sc.id = "example.dynamic"; storage.Api(sc);
    SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
    CHECK(storage.Register(sc, S("example.dynamic"), &serializer, sizeof(serializer), &provider) == MMS_OK);
    EditStore edit(history, storage); EditClient client; client.id = sc.id; auto api = edit.Api(client);
    auto mixed = std::make_shared<Dynamic>(value, 1, 4);
    MmsHandle handle = 0; EditParticipantFixture participant{&state, 0, 5}; auto p = participant.Api("example.dynamic");
    CHECK(history.Revision(revision) == MMS_OK && api.begin(api.context, S("mixed"), revision, &handle) == MMS_OK);
    CHECK(api.join(api.context, handle, &p) == MMS_OK && edit.JoinNative(client, handle, mixed) == MMS_OK);
    CHECK(api.prepare(api.context, handle) == MMS_OK && api.commit(api.context, handle) == MMS_OK);
    const auto mixedBytes = history.RetainedBytes(); state.fail = true;
    CHECK(api.undo(api.context) == MMS_E_NOT_READY && value == 4 && state.value == 5 && history.RetainedBytes() == mixedBytes);
    state.fail = false;
    CHECK(api.undo(api.context) == MMS_OK && value == 1 && state.value == 0 && history.RetainedBytes() == mixedBytes - 80);
    CHECK(api.redo(api.context) == MMS_OK && history.RetainedBytes() == mixedBytes);
    CHECK(history.Reset() == MMS_OK); history.Collect();
    // AppliedNative verifies the inverse without publishing it. Its charge
    // cannot be mistaken for the live authored state being recorded.
    std::unique_ptr<SharedHistory::AppliedTransaction> applied;
    CHECK(history.ReserveAppliedNative("applied", 500, applied) == MMS_OK);
    CHECK(history.PrepareAppliedNative(*applied, mixed) == MMS_E_NOT_READY);
    applied.reset();
    CHECK(!history.RetainedBytes() && !history.UndoCount());
}
static void SharedHistoryPendingPluginChecks() {
    // Both an open edit and a prepared edit exclude independent native work.
    // Also cover a native candidate created before the plugin transaction.
    for (bool prepared : {false, true}) for (bool nativeFirst : {false, true}) {
        StorageStore storage; SharedHistory history(storage); EditStore edit(history, storage);
        StorageClient sc; sc.id = "example.pending"; storage.Api(sc);
        SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
        CHECK(storage.Register(sc, S("example.pending"), &serializer, sizeof(serializer), &provider) == MMS_OK);
        EditClient client; client.id = sc.id; auto api = edit.Api(client);
        int value = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> native;
        for (int next : {1, 2}) {
            CHECK(history.PrepareNative("seed", std::make_shared<HistoryValue>(value, next - 1, next), native) == MMS_OK);
            CHECK(history.CommitNative(std::move(native)) == MMS_OK);
        }
        CHECK(api.undo(api.context) == MMS_OK && value == 1);
        uint64_t revision = 0;
        CHECK(history.Revision(revision) == MMS_OK);
        const auto bytes = history.RetainedBytes();
        const auto unchanged = [&] {
            uint64_t current = 0;
            CHECK(history.Revision(current) == MMS_OK && current == revision);
            CHECK(value == 1 && state.value == 0 && history.UndoCount() == 1 && history.RedoCount() == 1);
            CHECK(history.RetainedBytes() == bytes);
        };
        auto change = std::make_shared<HistoryValue>(value, 1, 7);
        if (nativeFirst) CHECK(history.PrepareNative("earlier native", change, native) == MMS_OK);
        EditParticipantFixture participant{&state, 0, 9}; auto p = participant.Api("example.pending");
        MmsHandle plugin = 0;
        CHECK(api.begin(api.context, S("pending plugin"), revision, &plugin) == MMS_OK);
        CHECK(api.join(api.context, plugin, &p) == MMS_OK);
        if (prepared) CHECK(api.prepare(api.context, plugin) == MMS_OK);
        CHECK(edit.HasPending());
        std::unique_ptr<SharedHistory::NativeTransaction> blocked;
        CHECK(history.PrepareNative("blocked native", change, blocked) == MMS_E_BUSY && !blocked);
        CHECK(history.History(false) == MMS_E_BUSY && history.History(true) == MMS_E_BUSY);
        CHECK(api.undo(api.context) == MMS_E_BUSY && api.redo(api.context) == MMS_E_BUSY);
        if (nativeFirst) CHECK(history.CommitNative(std::move(native)) == MMS_E_BUSY && !native);
        unchanged();

        // Ready errors retain priority over the pending-edit check.
        MmsResult wrongThread = MMS_OK;
        std::thread worker([&] { wrongThread = history.History(false); }); worker.join();
        CHECK(wrongThread == MMS_E_THREAD);
        participant.onCancel = [&] {
            CHECK(edit.InCallback());
            CHECK(history.PrepareNative("cancel callback", change, blocked) == MMS_E_BUSY && !blocked);
            CHECK(history.History(false) == MMS_E_BUSY);
        };
        api.cancel(api.context, plugin);
        CHECK(!edit.HasPending() && participant.cancels == 1 && !participant.commits);
        unchanged();

        // A refused native commit consumes only that native token. After
        // plugin cancellation, a fresh prepare may replace the redo branch.
        CHECK(history.PrepareNative("after cancel", change, native) == MMS_OK);
        CHECK(history.CommitNative(std::move(native)) == MMS_OK && value == 7 && state.value == 0);
        CHECK(history.UndoCount() == 2 && !history.RedoCount());

        // Plugin commit and its normal History forwarding must still work:
        // both hold EditStore's callback guard without a pending native edit.
        CHECK(history.Revision(revision) == MMS_OK);
        participant.onCommit = [&] {
            CHECK(edit.InCallback() && !edit.HasPending());
            CHECK(history.PrepareNative("commit callback", change, blocked) == MMS_E_BUSY && !blocked);
        };
        CHECK(api.begin(api.context, S("successful plugin"), revision, &plugin) == MMS_OK);
        CHECK(api.join(api.context, plugin, &p) == MMS_OK && api.prepare(api.context, plugin) == MMS_OK);
        CHECK(api.commit(api.context, plugin) == MMS_OK && state.value == 9 && value == 7);
        CHECK(api.undo(api.context) == MMS_OK && state.value == 0 && value == 7);
        CHECK(api.undo(api.context) == MMS_OK && value == 1);
        CHECK(api.redo(api.context) == MMS_OK && value == 7);
        CHECK(api.redo(api.context) == MMS_OK && state.value == 9 && value == 7);
    }
}
static void SharedHistoryChecks() {
    SharedHistoryPendingPluginChecks();
    SharedHistoryDynamicBudgetChecks();
    SharedHistoryEditBoundaryChecks();
    SharedHistoryResetChecks();
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
    // An authored native scope shares the same order with serializer-backed
    // plugin steps and excludes both pending and newly begun plugin edits.
    {
        MmsHandle pending = 0;
        std::unique_ptr<SharedHistory::AppliedTransaction> applied;
        CHECK(api.begin(api.context, S("pending plugin"), revision(), &pending) == MMS_OK);
        CHECK(history.ReserveAppliedNative("native scope", 1024, applied) == MMS_E_BUSY && !applied);
        api.cancel(api.context, pending);
        CHECK(history.ReserveAppliedNative("native scope", 1024, applied) == MMS_OK);
        MmsHandle unchanged = 99;
        CHECK(api.begin(api.context, S("during native scope"), revision(), &unchanged) == MMS_E_BUSY && unchanged == 99);
        auto authored = std::make_shared<HistoryValue>(a, 1, 5); a = 5;
        CHECK(history.PrepareAppliedNative(*applied, authored) == MMS_OK);
        CHECK(history.CommitAppliedNative(*applied) == MMS_OK && a == 5);
        applied.reset();
        plugin(10, 20);
        CHECK(api.undo(api.context) == MMS_OK && state.value == 10 && a == 5);
        CHECK(api.undo(api.context) == MMS_OK && a == 1 && state.value == 10);
        CHECK(api.redo(api.context) == MMS_OK && a == 5);
        CHECK(api.redo(api.context) == MMS_OK && state.value == 20);
        CHECK(api.undo(api.context) == MMS_OK && api.undo(api.context) == MMS_OK && a == 1 && state.value == 10);
    }
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
static void SharedHistoryEditBoundaryChecks() {
    StorageStore storage;SharedHistory history(storage);
    StorageClient sc;sc.id="example.boundary";storage.Api(sc);
    SerializerFixture state;auto serializer=state.Api();MmsHandle provider=0;
    CHECK(storage.Register(sc,S("example.boundary"),&serializer,sizeof(serializer),&provider)==MMS_OK);
    int a=0,b=0,destroyed=0;
    const auto revision=[&]{uint64_t r=0;CHECK(history.Revision(r)==MMS_OK);return r;};
    const auto initial=revision();
    {
        EditStore edit(history,storage);EditClient client;client.id=sc.id;auto api=edit.Api(client);
        CHECK(Throws([&]{EditStore duplicate(history,storage);}));
        EditParticipantFixture unprepared{&state,0,11},prepared{&state,0,22};
        auto u=unprepared.Api("example.boundary"),p=prepared.Api("example.boundary");
        MmsHandle first=0,second=0;
        auto nativeA=std::make_shared<HistoryValue>(a,0,1),nativeB=std::make_shared<HistoryValue>(b,0,2);
        nativeA->destroyed=&destroyed;nativeB->destroyed=&destroyed;
        std::weak_ptr<HistoryChange> weakA=nativeA,weakB=nativeB;
        CHECK(api.begin(api.context,S("unprepared mixed"),initial,&first)==MMS_OK);
        CHECK(api.join(api.context,first,&u)==MMS_OK && edit.JoinNative(client,first,nativeA)==MMS_OK);
        nativeA.reset();
        std::unique_ptr<SharedHistory::ResetTransaction> reset;
        CHECK(edit.HasPending() && history.Reset()==MMS_E_BUSY && history.PrepareReset(reset)==MMS_E_BUSY && !reset);
        CHECK(api.begin(api.context,S("prepared mixed"),initial,&second)==MMS_OK);
        CHECK(api.join(api.context,second,&p)==MMS_OK && edit.JoinNative(client,second,nativeB)==MMS_OK);
        nativeB.reset();
        CHECK(api.prepare(api.context,second)==MMS_OK);
        CHECK(history.Reset()==MMS_E_BUSY && history.PrepareReset(reset)==MMS_E_BUSY && !reset);
        CHECK(revision()==initial && !history.UndoCount() && !weakA.expired() && !weakB.expired() && !destroyed);
        prepared.onCancel=[&]{
            CHECK(edit.InCallback() && edit.CancelPending()==MMS_E_BUSY);
            CHECK(history.Reset()==MMS_E_BUSY && history.PrepareReset(reset)==MMS_E_BUSY && !reset);
        };
        CHECK(edit.CancelPending()==MMS_OK && !edit.HasPending());
        CHECK(unprepared.cancels==1 && prepared.cancels==1 && !unprepared.commits && !prepared.commits);
        CHECK(weakA.expired() && weakB.expired() && destroyed==2 && a==0 && b==0 && state.value==0);
        CHECK(api.prepare(api.context,first)==MMS_E_STALE && api.commit(api.context,second)==MMS_E_STALE);
        CHECK(edit.CancelPending()==MMS_OK && unprepared.cancels==1 && prepared.cancels==1);
        CHECK(history.PrepareReset(reset)==MMS_OK);
        MmsHandle unchanged=733;
        CHECK(api.begin(api.context,S("during reset"),initial,&unchanged)==MMS_E_BUSY && unchanged==733 && !edit.HasPending());
        CHECK(reset->Commit()==MMS_OK);
        CHECK(api.begin(api.context,S("after reset publication"),revision(),&unchanged)==MMS_E_BUSY && unchanged==733);
        reset.reset();
        // Commit removes its transaction from the map before callbacks. Reset
        // must also reject this interval, when InCallback is the only edit flag.
        EditParticipantFixture committing{&state,0,33};auto c=committing.Api("example.boundary");
        committing.onCommit=[&]{
            CHECK(!edit.HasPending() && edit.InCallback());
            CHECK(history.Reset()==MMS_E_BUSY && history.PrepareReset(reset)==MMS_E_BUSY && !reset);
        };
        CHECK(api.begin(api.context,S("commit callback boundary"),revision(),&first)==MMS_OK);
        CHECK(api.join(api.context,first,&c)==MMS_OK && api.prepare(api.context,first)==MMS_OK);
        CHECK(api.commit(api.context,first)==MMS_OK && committing.commits==1 && !committing.cancels && state.value==33);
        CHECK(history.Reset()==MMS_OK);history.Collect();
    }
    // Destruction unregisters the first store; a replacement can bind safely.
    {
        EditStore replacement(history,storage);
        CHECK(!replacement.HasPending() && history.Reset()==MMS_OK);
    }
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    CHECK(history.PrepareReset(reset)==MMS_OK);
    CHECK(Throws([&]{EditStore duringReset(history,storage);}));
    reset.reset();
}
