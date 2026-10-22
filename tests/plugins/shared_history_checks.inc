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
// Configurable effect/physics participant. The default stays conservative and
// keeps the inherited physics behavior, so effects and physics are independent.
struct HistoryEffectsValue : HistoryValue {
    HistoryEffects effects = HistoryEffects::All;
    bool physicsReset = true;
    mutable int effectsQueries = 0;
    using HistoryValue::HistoryValue;
    HistoryEffects Effects() const noexcept override { ++effectsQueries; return effects; }
    bool RequiresPhysicsReset() const noexcept override { return physicsReset; }
};
// Preparation-time Effects() that rewrites the caller's mutable record. The
// journal must copy the record first and freeze its mask from that copy.
struct RecordMutatingEffects : HistoryValue {
    EditRecord* record = nullptr;
    mutable int effectsReads = 0;
    using HistoryValue::HistoryValue;
    HistoryEffects Effects() const noexcept override {
        ++effectsReads;
        if (record) {
            record->label = "mutated payload";
            record->before.clear();
            record->after.clear();
        }
        return HistoryEffects::Viewport;
    }
};
// Captures what the synchronous publication signal observes.
struct HistoryEffectsSignal {
    SharedHistory* history = nullptr;
    HistoryEffects seen = HistoryEffects::None;
    int calls = 0;
    uint64_t revision = 0;
    void Install(SharedHistory& target) {
        history = &target;
        CHECK(target.SetPublicationSignal(HistoryPublicationSignal{this,
            [](void* context, uint64_t revision) noexcept {
                auto& state = *static_cast<HistoryEffectsSignal*>(context);
                ++state.calls; state.revision = revision;
                state.seen = state.history->LastPublicationEffects();
            }}) == MMS_OK);
    }
};
struct HistoryCleanupValue : HistoryValue {
    std::function<void()> candidateDestroyed, ownerDestroyed;
    using HistoryValue::HistoryValue;
    ~HistoryCleanupValue() override { if (ownerDestroyed) ownerDestroyed(); }
    struct Prepared final : HistoryValue::Candidate {
        HistoryCleanupValue& value;
        Prepared(HistoryCleanupValue& c, bool forward) : Candidate(c, forward), value(c) {}
        ~Prepared() override { if (value.candidateDestroyed) value.candidateDestroyed(); }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (throwPrepare) throw std::bad_alloc();
        if (fail) return MMS_E_NOT_READY;
        auto candidate = std::make_unique<Prepared>(*this, forward);
        if (!candidate->IsCurrent()) return MMS_E_STALE;
        out = std::move(candidate); return MMS_OK;
    }
};
static void SharedHistoryCleanupBoundaryChecks() {
    // Native token cancellation and stale by-value commit both release their
    // final resource owners after the normal prepare/commit gate has ended.
    for (bool staleCommit : {false, true}) {
        StorageStore storage; SharedHistory history(storage);
        int value = 0, candidateCalls = 0, ownerCalls = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        CHECK(history.PrepareNative("seed", std::make_shared<HistoryValue>(value, 0, 1), token) == MMS_OK);
        CHECK(history.CommitNative(std::move(token)) == MMS_OK);
        uint64_t revision = 0, after = 0;
        CHECK(history.Revision(revision) == MMS_OK);
        const auto bytes = history.RetainedBytes();
        MmsResult candidateUndo = MMS_OK, candidateReset = MMS_OK, ownerUndo = MMS_OK, ownerReset = MMS_OK;
        auto change = std::make_shared<HistoryCleanupValue>(value, 1, 2);
        change->candidateDestroyed = [&] {
            ++candidateCalls;
            candidateUndo = history.History(false); candidateReset = history.Reset();
        };
        change->ownerDestroyed = [&] {
            ++ownerCalls;
            ownerUndo = history.History(false); ownerReset = history.Reset();
        };
        std::weak_ptr<HistoryChange> weak = change;
        CHECK(history.PrepareNative("cancel cleanup", change, token) == MMS_OK);
        change->stale = staleCommit; change.reset();
        if (staleCommit) CHECK(history.CommitNative(std::move(token)) == MMS_E_STALE);
        else token.reset();
        CHECK(!token && weak.expired() && candidateCalls == 1 && ownerCalls == 1);
        CHECK(candidateUndo == MMS_E_BUSY && candidateReset == MMS_E_BUSY &&
            ownerUndo == MMS_E_BUSY && ownerReset == MMS_E_BUSY);
        CHECK(history.Revision(after) == MMS_OK && after == revision && value == 1 &&
            history.UndoCount() == 1 && !history.RedoCount() && history.RetainedBytes() == bytes);
        CHECK(history.History(false) == MMS_OK && value == 0);
    }

    // A failed consumed edit is no longer in transactions_. Neither its
    // cancel callback nor its last native owner may traverse/clear history.
    for (unsigned failure = 0; failure != 4; ++failure) {
        StorageStore storage;
        MmsResult rejectEnter = MMS_OK; bool throwEnter = false;
        SharedHistory history(storage, [&]() -> MmsResult {
            if (throwEnter) { throwEnter = false; throw std::bad_alloc(); }
            const auto result = rejectEnter; rejectEnter = MMS_OK; return result;
        }, 2);
        EditStore edit(history, storage);
        StorageClient sc; sc.id = "example.cleanup"; storage.Api(sc);
        SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
        CHECK(storage.Register(sc, S("example.cleanup"), &serializer, sizeof(serializer), &provider) == MMS_OK);
        EditClient client; client.id = sc.id; auto api = edit.Api(client);
        int value = 0, retired = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        for (int next = 1; next <= 3; ++next) {
            auto seed = std::make_shared<HistoryValue>(value, next - 1, next);
            if (next == 1) seed->destroyed = &retired;
            CHECK(history.PrepareNative("seed cleanup", seed, token) == MMS_OK);
            CHECK(history.CommitNative(std::move(token)) == MMS_OK);
        }
        CHECK(history.History(false) == MMS_OK && value == 2 && retired == 0);
        uint64_t revision = 0, after = 0;
        CHECK(history.Revision(revision) == MMS_OK);
        const auto bytes = history.RetainedBytes();
        int calls[3]{}; bool busy[3]{};
        MmsResult undo[3]{}, redo[3]{}, reset[3]{};
        auto observe = [&](unsigned stage) {
            ++calls[stage]; busy[stage] = edit.InCallback();
            undo[stage] = history.History(false); redo[stage] = history.History(true);
            reset[stage] = history.Reset(); history.Collect();
        };
        EditParticipantFixture participant{&state, 0, 7}; auto table = participant.Api("example.cleanup");
        participant.onCancel = [&] { observe(0); };
        auto change = std::make_shared<HistoryCleanupValue>(value, 2, 8);
        change->candidateDestroyed = [&] { observe(1); };
        change->ownerDestroyed = [&] { observe(2); };
        std::weak_ptr<HistoryChange> weak = change;
        MmsHandle transaction = 0;
        CHECK(api.begin(api.context, S("rejected mixed cleanup"), revision, &transaction) == MMS_OK);
        CHECK(api.join(api.context, transaction, &table) == MMS_OK && edit.JoinNative(client, transaction, change) == MMS_OK);
        CHECK(api.prepare(api.context, transaction) == MMS_OK);
        MmsResult expected = MMS_E_STALE;
        if (failure == 0) change->stale = true;
        else if (failure == 1) {
            CHECK(storage.Unregister(sc, provider) == MMS_OK); expected = MMS_E_NOT_READY;
        } else if (failure == 2) rejectEnter = expected = MMS_E_BUSY;
        else { throwEnter = true; expected = MMS_E_MEMORY; }
        change.reset();
        CHECK(api.commit(api.context, transaction) == expected);
        CHECK(!edit.HasPending() && !edit.InCallback() && weak.expired());
        CHECK(participant.cancels == 1 && participant.commits == 0 && participant.releases == 1);
        for (unsigned stage = 0; stage != 3; ++stage)
            CHECK(calls[stage] == 1 && busy[stage] && undo[stage] == MMS_E_BUSY &&
                redo[stage] == MMS_E_BUSY && reset[stage] == MMS_E_BUSY);
        CHECK(retired == 0 && value == 2 && state.value == 0 && history.RetainedBytes() == bytes &&
            history.UndoCount() == 1 && history.RedoCount() == 1);
        CHECK(history.Revision(after) == MMS_OK && after == revision);
        CHECK(api.commit(api.context, transaction) == MMS_E_STALE);
        history.Collect(); CHECK(retired == 1);
        CHECK(api.undo(api.context) == MMS_OK && value == 1);
        CHECK(api.redo(api.context) == MMS_OK && value == 2);
    }

    // A forwarding host can perform post-traversal cleanup before returning
    // to EditStore. It must not inherit a reusable "busy is allowed" flag.
    struct Forwarder final : EditHost {
        SharedHistory& target;
        std::function<void()> after;
        explicit Forwarder(SharedHistory& h) : target(h) {}
        void AttachStore(EditStore* store) override { target.AttachStore(store); }
        void DetachStore(EditStore* store) noexcept override { target.DetachStore(store); }
        MmsResult Revision(uint64_t& r) override { return target.Revision(r); }
        MmsResult Enter() override { return target.Enter(); }
        void Leave() noexcept override { target.Leave(); }
        MmsResult Reserve(std::shared_ptr<const EditRecord> r, std::unique_ptr<EditPublication>& p) override {
            return target.Reserve(std::move(r), p);
        }
        MmsResult History(bool redo) override {
            const auto result = target.History(redo); if (after) after(); return result;
        }
        void Poison() noexcept override { target.Poison(); }
    };
    StorageStore storage; SharedHistory history(storage); Forwarder host(history); EditStore edit(host, storage);
    EditClient client; client.id = "example.forwarding"; auto api = edit.Api(client);
    int value = 0;
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    for (int next : {1, 2}) {
        CHECK(history.PrepareNative("forwarded", std::make_shared<HistoryValue>(value, next - 1, next), token) == MMS_OK);
        CHECK(history.CommitNative(std::move(token)) == MMS_OK);
    }
    MmsResult nested = MMS_OK;
    host.after = [&] { CHECK(edit.InCallback()); nested = history.History(false); };
    CHECK(api.undo(api.context) == MMS_OK && nested == MMS_E_BUSY && value == 1);
    CHECK(history.UndoCount() == 1 && history.RedoCount() == 1);
    CHECK(api.redo(api.context) == MMS_OK && nested == MMS_E_BUSY && value == 2);
    host.after = {};

    // The public EditStore boundary must enforce pending exclusion even for
    // a host that does not inspect EditStore itself.
    {
        StorageStore isolatedStorage; EditHostFixture isolatedHost(isolatedStorage);
        EditStore isolatedEdit(isolatedHost, isolatedStorage);
        StorageClient sc; sc.id = "example.pending.host"; isolatedStorage.Api(sc);
        SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
        CHECK(isolatedStorage.Register(sc, S("example.pending.host"), &serializer, sizeof(serializer), &provider) == MMS_OK);
        EditClient owner; owner.id = sc.id; auto ownApi = isolatedEdit.Api(owner);
        EditParticipantFixture participant{&state, 0, 5}; auto table = participant.Api("example.pending.host");
        MmsHandle transaction = 0;
        CHECK(ownApi.begin(ownApi.context, S("seed"), isolatedHost.revision, &transaction) == MMS_OK);
        CHECK(ownApi.join(ownApi.context, transaction, &table) == MMS_OK &&
            ownApi.prepare(ownApi.context, transaction) == MMS_OK && ownApi.commit(ownApi.context, transaction) == MMS_OK);
        CHECK(ownApi.begin(ownApi.context, S("open but unprepared"), isolatedHost.revision, &transaction) == MMS_OK);
        const auto revision = isolatedHost.revision;
        CHECK(ownApi.undo(ownApi.context) == MMS_E_BUSY && ownApi.redo(ownApi.context) == MMS_E_BUSY &&
            state.value == 5 && isolatedHost.cursor == 1 && isolatedHost.revision == revision);
        ownApi.cancel(ownApi.context, transaction);
        CHECK(ownApi.undo(ownApi.context) == MMS_OK && state.value == 0);
    }
}
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
static void SharedHistoryNavigationChecks() {
    StorageStore storage; bool playing=true;
    SharedHistory history(storage,[&]{return playing?MMS_E_BUSY:MMS_OK;});
    CHECK(history.CheckNativeEdit()==MMS_E_BUSY && history.CheckNativeNavigation()==MMS_OK);
    playing=false;
    CHECK(history.Enter()==MMS_OK && history.CheckNativeNavigation()==MMS_E_BUSY);history.Leave();
    int value=0;std::unique_ptr<SharedHistory::NativeTransaction> native;
    CHECK(history.PrepareNative("navigation pending",std::make_shared<HistoryValue>(value,0,1),native)==MMS_OK);
    CHECK(history.CheckNativeNavigation()==MMS_E_BUSY);
    CHECK(history.CommitNative(std::move(native))==MMS_OK && history.CheckNativeNavigation()==MMS_OK);
    std::unique_ptr<SharedHistory::AppliedTransaction> applied;
    CHECK(history.ReserveAppliedNative("navigation applied",1024,applied)==MMS_OK && history.CheckNativeNavigation()==MMS_E_BUSY);
    applied.reset();CHECK(history.CheckNativeNavigation()==MMS_OK);
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    CHECK(history.PrepareReset(reset)==MMS_OK && history.CheckNativeNavigation()==MMS_E_BUSY);
    reset.reset();CHECK(history.CheckNativeNavigation()==MMS_OK);
    StorageClient sc;sc.id="example.navigation";storage.Api(sc);
    MmsSerializerV1 serializer{Header<MmsSerializerV1>(),&history};
    serializer.prepareLoad=[](void* p,MmsBytes,uint32_t,MmsHandle* out)->MmsResult{
        CHECK(static_cast<SharedHistory*>(p)->CheckNativeNavigation()==MMS_E_BUSY);*out=17;return MMS_OK;};
    serializer.commitLoad=[](void* p,MmsHandle){CHECK(static_cast<SharedHistory*>(p)->CheckNativeNavigation()==MMS_E_BUSY);};
    serializer.cancelLoad=serializer.commitLoad;
    serializer.save=[](void* p,MmsBytes* bytes,uint32_t* schema,MmsHandle* lease)->MmsResult{
        CHECK(static_cast<SharedHistory*>(p)->CheckNativeNavigation()==MMS_E_BUSY);
        *bytes={};*schema=7;*lease=19;return MMS_OK;};
    serializer.releaseBytes=[](void*,MmsHandle){};
    MmsHandle registered=0;
    CHECK(storage.Register(sc,S("example.navigation"),&serializer,sizeof(serializer),&registered)==MMS_OK);
    CHECK(storage.Prepare({})==MMS_OK && history.CheckNativeNavigation()==MMS_E_BUSY);
    storage.Cancel();CHECK(history.CheckNativeNavigation()==MMS_OK);
    StoredRecords records;CHECK(storage.Capture(records)==MMS_OK && history.CheckNativeNavigation()==MMS_OK);
    {
        EditStore edit(history,storage);EditClient client;client.id=sc.id;auto api=edit.Api(client);
        MmsHandle transaction=0;uint64_t revision=0;CHECK(history.Revision(revision)==MMS_OK);
        CHECK(api.begin(api.context,S("navigation plugin pending"),revision,&transaction)==MMS_OK);
        CHECK(history.CheckNativeNavigation()==MMS_E_BUSY);api.cancel(api.context,transaction);
        CHECK(history.CheckNativeNavigation()==MMS_OK);
        SerializerFixture state;EditParticipantFixture participant{&state,0,1};auto table=participant.Api("example.navigation");
        participant.onCommit=[&]{CHECK(!edit.HasPending() && edit.InCallback() && history.CheckNativeNavigation()==MMS_E_BUSY);};
        CHECK(api.begin(api.context,S("navigation callback"),revision,&transaction)==MMS_OK);
        CHECK(api.join(api.context,transaction,&table)==MMS_OK && api.prepare(api.context,transaction)==MMS_OK);
        CHECK(history.CheckNativeNavigation()==MMS_E_BUSY);
        CHECK(api.commit(api.context,transaction)==MMS_OK && history.CheckNativeNavigation()==MMS_OK);
    }
    MmsResult worker=MMS_OK;std::thread thread([&]{worker=history.CheckNativeNavigation();});thread.join();
    CHECK(worker==MMS_E_THREAD);
}
static void SharedHistoryEffectsChecks() {
    // The mask helpers are constexpr bit operations; no label guessing.
    CHECK(static_cast<uint32_t>(HistoryEffects::None) == 0 &&
        static_cast<uint32_t>(HistoryEffects::ProjectData) == 1 &&
        static_cast<uint32_t>(HistoryEffects::SceneReadout) == 2 &&
        static_cast<uint32_t>(HistoryEffects::Viewport) == 4 &&
        static_cast<uint32_t>(HistoryEffects::All) == 7);
    CHECK((HistoryEffects::ProjectData | HistoryEffects::Viewport) == static_cast<HistoryEffects>(5));
    CHECK((HistoryEffects::All & HistoryEffects::SceneReadout) == HistoryEffects::SceneReadout);
    CHECK(HasHistoryEffect(HistoryEffects::All, HistoryEffects::ProjectData) &&
        !HasHistoryEffect(HistoryEffects::Viewport, HistoryEffects::ProjectData));

    StorageStore storage; SharedHistory history(storage); EditStore edit(history, storage);
    StorageClient sc; sc.id = "example.effects"; storage.Api(sc);
    SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
    CHECK(storage.Register(sc, S("example.effects"), &serializer, sizeof(serializer), &provider) == MMS_OK);
    EditClient client; client.id = sc.id; auto api = edit.Api(client);
    const auto revision = [&] { uint64_t r = 0; CHECK(history.Revision(r) == MMS_OK); return r; };
    std::unique_ptr<SharedHistory::NativeTransaction> native;
    const auto commit = [&](const char* label, std::shared_ptr<HistoryChange> change) {
        CHECK(history.PrepareNative(label, std::move(change), native) == MMS_OK);
        CHECK(history.CommitNative(std::move(native)) == MMS_OK);
    };
    int value = 0;
    // An unknown native participant keeps the conservative default.
    auto plain = std::make_shared<HistoryValue>(value, 0, 1);
    CHECK(plain->Effects() == HistoryEffects::All);
    commit("default native", plain);
    CHECK(value == 1 && history.LastPublicationEffects() == HistoryEffects::All);

    // Renderer-only native work declares a viewport mask without project data.
    auto renderer = std::make_shared<HistoryEffectsValue>(value, 1, 2);
    renderer->effects = HistoryEffects::Viewport;
    commit("renderer native", renderer);
    CHECK(value == 2 && history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(history.LastPublicationRequiresPhysicsReset()); // physics stays independent

    auto metadata = std::make_shared<HistoryEffectsValue>(value, 2, 3);
    metadata->effects = HistoryEffects::None;
    commit("metadata native", metadata);
    CHECK(value == 3 && history.LastPublicationEffects() == HistoryEffects::None);
    CHECK(history.LastPublicationRequiresPhysicsReset());

    auto readout = std::make_shared<HistoryEffectsValue>(value, 3, 4);
    readout->effects = HistoryEffects::All; readout->physicsReset = false;
    commit("readout native", readout);
    CHECK(value == 4 && history.LastPublicationEffects() == HistoryEffects::All);
    CHECK(!history.LastPublicationRequiresPhysicsReset());

    auto combined = std::make_shared<HistoryEffectsValue>(value, 4, 5);
    combined->effects = HistoryEffects::ProjectData | HistoryEffects::Viewport;
    commit("combined native", combined);
    CHECK(value == 5 && history.LastPublicationEffects() == static_cast<HistoryEffects>(5));

    // A plugin record always widens a renderer-only native mask to All.
    EditParticipantFixture participant{&state, 0, 9}; auto table = participant.Api("example.effects");
    auto joined = std::make_shared<HistoryEffectsValue>(value, 5, 9);
    joined->effects = HistoryEffects::Viewport;
    MmsHandle handle = 0;
    CHECK(api.begin(api.context, S("plugin and renderer native"), revision(), &handle) == MMS_OK);
    CHECK(api.join(api.context, handle, &table) == MMS_OK);
    CHECK(edit.JoinNative(client, handle, joined) == MMS_OK);
    CHECK(api.prepare(api.context, handle) == MMS_OK);
    CHECK(api.commit(api.context, handle) == MMS_OK && state.value == 9 && value == 9);
    CHECK(history.LastPublicationEffects() == HistoryEffects::All);

    // Every undo/redo step reports the mask frozen on the node it traversed.
    const HistoryEffects undoMasks[] = {HistoryEffects::All, static_cast<HistoryEffects>(5),
        HistoryEffects::All, HistoryEffects::None, HistoryEffects::Viewport, HistoryEffects::All};
    bool firstUndo = true;
    for (auto effects : undoMasks) {
        CHECK(api.undo(api.context) == MMS_OK && history.LastPublicationEffects() == effects);
        // The mixed participant fixture must restore the serializer value that
        // was live before the commit, not a fabricated before snapshot.
        if (firstUndo) { CHECK(state.value == 0); firstUndo = false; }
    }
    const HistoryEffects redoMasks[] = {HistoryEffects::All, HistoryEffects::Viewport,
        HistoryEffects::None, HistoryEffects::All, static_cast<HistoryEffects>(5), HistoryEffects::All};
    for (auto effects : redoMasks)
        CHECK(api.redo(api.context) == MMS_OK && history.LastPublicationEffects() == effects);

    // Mutating the participant after preparation cannot change the frozen mask,
    // and publication/traversal never queries Effects again.
    auto frozen = std::make_shared<HistoryEffectsValue>(value, 9, 10);
    frozen->effects = HistoryEffects::SceneReadout;
    CHECK(history.PrepareNative("frozen", frozen, native) == MMS_OK);
    CHECK(frozen->effectsQueries == 1);
    frozen->effects = HistoryEffects::ProjectData;
    CHECK(history.CommitNative(std::move(native)) == MMS_OK && value == 10);
    CHECK(frozen->effectsQueries == 1 && history.LastPublicationEffects() == HistoryEffects::SceneReadout);
    CHECK(api.undo(api.context) == MMS_OK && value == 9);
    CHECK(frozen->effectsQueries == 1 && history.LastPublicationEffects() == HistoryEffects::SceneReadout);
    CHECK(api.redo(api.context) == MMS_OK && value == 10);
    CHECK(frozen->effectsQueries == 1 && history.LastPublicationEffects() == HistoryEffects::SceneReadout);
    // A stale traversal is rejected without republishing the previous mask.
    frozen->stale = true;
    CHECK(api.undo(api.context) == MMS_E_STALE && value == 10 &&
        history.LastPublicationEffects() == HistoryEffects::SceneReadout);
    frozen->stale = false;

    // Applied native work refreezes the placeholder when the real participant
    // is accepted; reserve/failure/cancellation/stale never publish a mask.
    const auto previous = history.LastPublicationEffects(); // SceneReadout
    HistoryEffectsSignal signal; signal.Install(history);
    std::unique_ptr<SharedHistory::AppliedTransaction> applied;
    CHECK(history.ReserveAppliedNative("applied effects", 4096, applied) == MMS_OK);
    CHECK(!signal.calls && history.LastPublicationEffects() == previous);
    auto authored = std::make_shared<HistoryEffectsValue>(value, 10, 12);
    authored->effects = HistoryEffects::Viewport; // deliberately different from previous
    value = 12;
    CHECK(history.PrepareAppliedNative(*applied, authored) == MMS_OK);
    CHECK(authored->effectsQueries == 1);
    authored->effects = HistoryEffects::ProjectData;
    CHECK(!signal.calls && history.LastPublicationEffects() == previous);
    CHECK(history.CommitAppliedNative(*applied) == MMS_OK);
    CHECK(authored->effectsQueries == 1 &&
        history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(signal.calls == 1 && signal.seen == HistoryEffects::Viewport);
    CHECK(history.CommitAppliedNative(*applied) == MMS_E_CONFLICT &&
        signal.calls == 1 && history.LastPublicationEffects() == HistoryEffects::Viewport);
    applied.reset();
    CHECK(history.History(false) == MMS_OK && value == 10 &&
        history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(authored->effectsQueries == 1 &&
        signal.calls == 2 && signal.seen == HistoryEffects::Viewport);
    CHECK(history.History(true) == MMS_OK && value == 12 &&
        history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(authored->effectsQueries == 1 &&
        signal.calls == 3 && signal.seen == HistoryEffects::Viewport);
    const auto kept = history.LastPublicationEffects();
    CHECK(history.ReserveAppliedNative("applied failure", 4096, applied) == MMS_OK);
    auto failingApplied = std::make_shared<HistoryEffectsValue>(value, 12, 13);
    failingApplied->effects = HistoryEffects::ProjectData;
    failingApplied->fail = true;
    CHECK(history.PrepareAppliedNative(*applied, failingApplied) == MMS_E_NOT_READY);
    CHECK(history.LastPublicationEffects() == kept && signal.calls == 3);
    applied.reset();
    CHECK(history.LastPublicationEffects() == kept && signal.calls == 3);
    CHECK(history.ReserveAppliedNative("applied cancel", 4096, applied) == MMS_OK);
    applied.reset();
    CHECK(history.LastPublicationEffects() == kept && signal.calls == 3);
    CHECK(history.ReserveAppliedNative("applied stale", 4096, applied) == MMS_OK);
    auto staleApplied = std::make_shared<HistoryEffectsValue>(value, 12, 14);
    staleApplied->effects = HistoryEffects::ProjectData;
    value = 14;
    CHECK(history.PrepareAppliedNative(*applied, staleApplied) == MMS_OK);
    value = 15;
    CHECK(history.CommitAppliedNative(*applied) == MMS_E_STALE);
    CHECK(history.LastPublicationEffects() == kept && signal.calls == 3);
    applied.reset(); value = 12;

    // Prepare failures, cancellation, stale commits and budget rejection keep
    // the previous mask.
    auto failingNative = std::make_shared<HistoryEffectsValue>(value, 12, 13);
    failingNative->effects = HistoryEffects::SceneReadout; failingNative->fail = true;
    CHECK(history.PrepareNative("prepare failure", failingNative, native) == MMS_E_NOT_READY && !native);
    CHECK(history.LastPublicationEffects() == kept);
    auto cancelled = std::make_shared<HistoryEffectsValue>(value, 12, 16);
    cancelled->effects = HistoryEffects::ProjectData;
    CHECK(history.PrepareNative("cancelled", cancelled, native) == MMS_OK);
    native.reset();
    CHECK(history.LastPublicationEffects() == kept);
    auto tooLarge = std::make_shared<HistoryEffectsValue>(value, 12, 17);
    tooLarge->effects = HistoryEffects::All; tooLarge->bytes = UINT64_MAX;
    CHECK(history.PrepareNative("budget", tooLarge, native) == MMS_E_MEMORY && !native);
    CHECK(history.LastPublicationEffects() == kept);
    auto staleNative = std::make_shared<HistoryEffectsValue>(value, 12, 18);
    staleNative->effects = HistoryEffects::ProjectData;
    CHECK(history.PrepareNative("stale commit", staleNative, native) == MMS_OK);
    staleNative->stale = true;
    CHECK(history.CommitNative(std::move(native)) == MMS_E_STALE);
    CHECK(history.LastPublicationEffects() == kept);

    // Zero plugin participants: a native-only EditStore transaction keeps the
    // renderer-only mask through the real prepare/commit/undo/redo path.
    {
        StorageStore nativeStorage; SharedHistory nativeHistory(nativeStorage);
        EditStore nativeEdit(nativeHistory, nativeStorage);
        EditClient nativeClient; nativeClient.id = "example.effects.native";
        auto nativeApi = nativeEdit.Api(nativeClient);
        int nativeValue = 0;
        auto renderer = std::make_shared<HistoryEffectsValue>(nativeValue, 0, 1);
        renderer->effects = HistoryEffects::Viewport;
        MmsHandle nativeTransaction = 0;
        uint64_t nativeRevision = 0; CHECK(nativeHistory.Revision(nativeRevision) == MMS_OK);
        CHECK(nativeApi.begin(nativeApi.context, S("native only"), nativeRevision, &nativeTransaction) == MMS_OK);
        CHECK(nativeEdit.JoinNative(nativeClient, nativeTransaction, renderer) == MMS_OK);
        CHECK(nativeApi.prepare(nativeApi.context, nativeTransaction) == MMS_OK);
        CHECK(nativeApi.commit(nativeApi.context, nativeTransaction) == MMS_OK && nativeValue == 1);
        CHECK(nativeHistory.LastPublicationEffects() == HistoryEffects::Viewport);
        CHECK(nativeApi.undo(nativeApi.context) == MMS_OK && nativeValue == 0 &&
            nativeHistory.LastPublicationEffects() == HistoryEffects::Viewport);
        CHECK(nativeApi.redo(nativeApi.context) == MMS_OK && nativeValue == 1 &&
            nativeHistory.LastPublicationEffects() == HistoryEffects::Viewport);
    }

    // An empty journal and its rejected undo/redo keep the last mask; a None
    // mask still publishes and restores the live value normally.
    {
        StorageStore emptyStorage; SharedHistory empty(emptyStorage);
        CHECK(empty.LastPublicationEffects() == HistoryEffects::All);
        CHECK(empty.History(false) == MMS_E_NOT_FOUND && empty.History(true) == MMS_E_NOT_FOUND);
        CHECK(empty.LastPublicationEffects() == HistoryEffects::All);
        int emptyValue = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> onlyNative;
        auto only = std::make_shared<HistoryEffectsValue>(emptyValue, 0, 1);
        only->effects = HistoryEffects::None;
        CHECK(empty.PrepareNative("only", only, onlyNative) == MMS_OK &&
            empty.CommitNative(std::move(onlyNative)) == MMS_OK);
        CHECK(emptyValue == 1 && empty.LastPublicationEffects() == HistoryEffects::None);
        CHECK(empty.History(false) == MMS_OK && emptyValue == 0 &&
            empty.LastPublicationEffects() == HistoryEffects::None);
        CHECK(empty.History(false) == MMS_E_NOT_FOUND && emptyValue == 0 &&
            empty.LastPublicationEffects() == HistoryEffects::None);
        CHECK(empty.History(true) == MMS_OK && emptyValue == 1 &&
            empty.LastPublicationEffects() == HistoryEffects::None);
        CHECK(empty.History(true) == MMS_E_NOT_FOUND &&
            empty.LastPublicationEffects() == HistoryEffects::None);
    }

    // The publication signal observes the new mask synchronously on commit,
    // undo and redo.
    {
        StorageStore signalStorage; SharedHistory signalHistory(signalStorage);
        HistoryEffectsSignal signal; signal.Install(signalHistory);
        int signalValue = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> signalNative;
        auto only = std::make_shared<HistoryEffectsValue>(signalValue, 0, 1);
        only->effects = HistoryEffects::Viewport;
        CHECK(signalHistory.PrepareNative("signal", only, signalNative) == MMS_OK);
        CHECK(signal.calls == 0 && signalHistory.LastPublicationEffects() == HistoryEffects::All);
        CHECK(signalHistory.CommitNative(std::move(signalNative)) == MMS_OK);
        uint64_t signalRevision = 0; CHECK(signalHistory.Revision(signalRevision) == MMS_OK);
        CHECK(signal.calls == 1 && signal.seen == HistoryEffects::Viewport && signal.revision == signalRevision);
        CHECK(signalHistory.History(false) == MMS_OK);
        CHECK(signal.calls == 2 && signal.seen == HistoryEffects::Viewport);
        CHECK(signalHistory.History(true) == MMS_OK);
        CHECK(signal.calls == 3 && signal.seen == HistoryEffects::Viewport);
    }

    // ExplicitOnly adjustments add no journal step, yet a successful
    // publication still updates the mask and emits the same signal.
    {
        StorageStore modeStorage; SharedHistory modeHistory(modeStorage);
        CHECK(modeHistory.SetMode(SharedHistory::RecordingMode::ExplicitOnly) == MMS_OK);
        HistoryEffectsSignal signal; signal.Install(modeHistory);
        int modeValue = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> adjustment;
        auto change = std::make_shared<HistoryEffectsValue>(modeValue, 0, 1);
        change->effects = HistoryEffects::ProjectData | HistoryEffects::SceneReadout;
        change->physicsReset = false;
        CHECK(modeHistory.PrepareNative("adjustment", change, adjustment,
            SharedHistory::NativeIntent::Adjustment) == MMS_OK);
        CHECK(signal.calls == 0 && modeHistory.LastPublicationEffects() == HistoryEffects::All);
        CHECK(modeHistory.CommitNative(std::move(adjustment)) == MMS_OK && modeValue == 1);
        CHECK(!modeHistory.UndoCount() && !modeHistory.RedoCount());
        CHECK(modeHistory.LastPublicationEffects() == (HistoryEffects::ProjectData | HistoryEffects::SceneReadout));
        CHECK(signal.calls == 1 && signal.seen == modeHistory.LastPublicationEffects());
        uint64_t modeRevision = 0; CHECK(modeHistory.Revision(modeRevision) == MMS_OK);
        CHECK(signal.revision == modeRevision && !modeHistory.LastPublicationRequiresPhysicsReset());
    }

    // A None mask cannot bypass the application edit gate, and a rejected
    // publication never changes the last mask.
    {
        bool open = false; StorageStore gatedStorage;
        SharedHistory gated(gatedStorage, [&] { return open ? MMS_OK : MMS_E_BUSY; });
        int gatedValue = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> gatedNative;
        open = true;
        auto seed = std::make_shared<HistoryEffectsValue>(gatedValue, 0, 1);
        seed->effects = HistoryEffects::Viewport;
        CHECK(gated.PrepareNative("gate seed", seed, gatedNative) == MMS_OK);
        CHECK(gated.CommitNative(std::move(gatedNative)) == MMS_OK);
        CHECK(gated.LastPublicationEffects() == HistoryEffects::Viewport);
        open = false;
        auto blocked = std::make_shared<HistoryEffectsValue>(gatedValue, 1, 2);
        blocked->effects = HistoryEffects::None;
        CHECK(gated.PrepareNative("gate blocked", blocked, gatedNative) == MMS_E_BUSY && !gatedNative);
        CHECK(gated.LastPublicationEffects() == HistoryEffects::Viewport);
        open = true;
        CHECK(gated.PrepareNative("gate pending", blocked, gatedNative) == MMS_OK);
        open = false;
        CHECK(gated.CommitNative(std::move(gatedNative)) == MMS_E_BUSY && gatedValue == 1);
        CHECK(gated.LastPublicationEffects() == HistoryEffects::Viewport);
        CHECK(gated.History(false) == MMS_E_BUSY && gatedValue == 1 &&
            gated.LastPublicationEffects() == HistoryEffects::Viewport);
        open = true;
    }

    // Reset restores the conservative default without an extra signal.
    {
        StorageStore resetStorage; SharedHistory resetHistory(resetStorage);
        HistoryEffectsSignal signal; signal.Install(resetHistory);
        int resetValue = 0;
        std::unique_ptr<SharedHistory::NativeTransaction> resetNative;
        auto renderer = std::make_shared<HistoryEffectsValue>(resetValue, 0, 1);
        renderer->effects = HistoryEffects::Viewport;
        CHECK(resetHistory.PrepareNative("reset seed", renderer, resetNative) == MMS_OK);
        CHECK(resetHistory.CommitNative(std::move(resetNative)) == MMS_OK);
        CHECK(resetHistory.LastPublicationEffects() == HistoryEffects::Viewport && signal.calls == 1);
        CHECK(resetHistory.Reset() == MMS_OK);
        CHECK(resetHistory.LastPublicationEffects() == HistoryEffects::All && signal.calls == 1);
    }
}
static void SharedHistoryReserveCopyChecks() {
    // Direct Reserve/publication path: a native Effects() rewrites the caller's
    // mutable record during preparation. The retained copy and its frozen mask
    // must come from the aggregate copied before that call.
    StorageStore storage; SharedHistory history(storage);
    StorageClient sc; sc.id = "example.copy"; storage.Api(sc);
    SerializerFixture state; auto serializer = state.Api(); MmsHandle provider = 0;
    CHECK(storage.Register(sc, S("example.copy"), &serializer, sizeof(serializer), &provider) == MMS_OK);
    int live = 0;
    auto record = std::make_shared<EditRecord>();
    record->label = "original payload";
    record->before.emplace("example.copy", StoredInt(11));
    record->after.emplace("example.copy", StoredInt(22));
    auto change = std::make_shared<RecordMutatingEffects>(live, 0, 1);
    change->record = record.get();
    record->native = change;
    CHECK(history.Enter() == MMS_OK);
    std::unique_ptr<EditPublication> publication;
    CHECK(history.Reserve(record, publication) == MMS_OK && publication);
    CHECK(change->effectsReads == 1 && record->label == "mutated payload" &&
        record->before.empty() && record->after.empty());
    publication->Publish();
    history.Leave();
    CHECK(history.UndoCount() == 1 && !history.RedoCount());
    CHECK(*history.NextLabel(false) == "original payload");
    CHECK(history.LastPublicationEffects() == HistoryEffects::All);
    CHECK(history.History(false) == MMS_OK && live == 0 && state.value == 11 &&
        history.LastPublicationEffects() == HistoryEffects::All);
    CHECK(history.History(true) == MMS_OK && live == 1 && state.value == 22 &&
        history.LastPublicationEffects() == HistoryEffects::All);
    // The caller's source stays mutated; only the retained copy is stable.
    CHECK(record->label == "mutated payload" && record->before.empty());
}
static void SharedHistoryEffectsPeekChecks() {
    // Empty journals preview the conservative fallback on both cursors.
    StorageStore emptyStorage; SharedHistory empty(emptyStorage);
    CHECK(empty.NextPublicationEffects(false) == HistoryEffects::All &&
        empty.NextPublicationEffects(true) == HistoryEffects::All);

    StorageStore storage; SharedHistory history(storage);
    int value = 0;
    std::unique_ptr<SharedHistory::NativeTransaction> native;
    auto first = std::make_shared<HistoryEffectsValue>(value, 0, 1);
    first->effects = HistoryEffects::Viewport;
    CHECK(history.PrepareNative("peek first", first, native) == MMS_OK);
    CHECK(history.CommitNative(std::move(native)) == MMS_OK);
    auto second = std::make_shared<HistoryEffectsValue>(value, 1, 2);
    second->effects = HistoryEffects::None;
    CHECK(history.PrepareNative("peek second", second, native) == MMS_OK);
    CHECK(history.CommitNative(std::move(native)) == MMS_OK);
    CHECK(first->effectsQueries == 1 && second->effectsQueries == 1);
    // The preview reads the next real node without consuming or re-querying it.
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::None);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::All);
    CHECK(history.UndoCount() == 2 && !history.RedoCount());
    CHECK(first->effectsQueries == 1 && second->effectsQueries == 1);

    // A successful traversal reports exactly the mask announced by the preview.
    CHECK(history.History(false) == MMS_OK && value == 1 &&
        history.LastPublicationEffects() == HistoryEffects::None);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::None);
    CHECK(history.History(false) == MMS_OK && value == 0 &&
        history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::All);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::Viewport);

    // Rejected and cancelled work keeps the cursor and both previews intact.
    first->stale = true;
    CHECK(history.History(true) == MMS_E_STALE && value == 0);
    CHECK(!history.UndoCount() && history.RedoCount() == 2);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::All);
    first->stale = false;
    auto cancelled = std::make_shared<HistoryEffectsValue>(value, 0, 3);
    cancelled->effects = HistoryEffects::ProjectData;
    CHECK(history.PrepareNative("peek cancel", cancelled, native) == MMS_OK);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::All);
    native.reset();
    CHECK(!history.UndoCount() && history.RedoCount() == 2);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::Viewport);
    auto failed = std::make_shared<HistoryEffectsValue>(value, 0, 4);
    failed->effects = HistoryEffects::SceneReadout; failed->fail = true;
    CHECK(history.PrepareNative("peek failure", failed, native) == MMS_E_NOT_READY && !native);
    CHECK(!history.UndoCount() && history.RedoCount() == 2);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::Viewport);

    // Redo switches the preview to the next frozen node, and a failed undo
    // leaves the order and preview untouched.
    CHECK(history.History(true) == MMS_OK && value == 1 &&
        history.LastPublicationEffects() == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::Viewport);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::None);
    CHECK(history.History(true) == MMS_OK && value == 2 &&
        history.LastPublicationEffects() == HistoryEffects::None);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::None);
    CHECK(history.NextPublicationEffects(true) == HistoryEffects::All);
    CHECK(history.UndoCount() == 2 && !history.RedoCount());
    second->stale = true;
    CHECK(history.History(false) == MMS_E_STALE && value == 2);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::None &&
        history.NextPublicationEffects(true) == HistoryEffects::All);
    CHECK(history.UndoCount() == 2 && !history.RedoCount());
    second->stale = false;

    // The preview never enters the gate or asks the participant again.
    int gateCalls = 0; bool open = true;
    StorageStore gatedStorage;
    SharedHistory gated(gatedStorage, [&] { ++gateCalls; return open ? MMS_OK : MMS_E_BUSY; });
    int gatedValue = 0;
    std::unique_ptr<SharedHistory::NativeTransaction> gatedNative;
    auto rendered = std::make_shared<HistoryEffectsValue>(gatedValue, 0, 1);
    rendered->effects = HistoryEffects::SceneReadout;
    CHECK(gated.PrepareNative("gate peek", rendered, gatedNative) == MMS_OK);
    CHECK(gated.CommitNative(std::move(gatedNative)) == MMS_OK);
    const auto commitGateCalls = gateCalls;
    CHECK(commitGateCalls == 2 && rendered->effectsQueries == 1);
    CHECK(gated.NextPublicationEffects(false) == HistoryEffects::SceneReadout);
    CHECK(gateCalls == commitGateCalls && rendered->effectsQueries == 1);
    open = false;
    CHECK(gated.NextPublicationEffects(false) == HistoryEffects::SceneReadout);
    CHECK(gated.NextPublicationEffects(true) == HistoryEffects::All);
    CHECK(gateCalls == commitGateCalls);
    open = true;
    HistoryEffects gatedWorker = HistoryEffects::None;
    std::thread gatedThread([&] { gatedWorker = gated.NextPublicationEffects(false); });
    gatedThread.join();
    CHECK(gatedWorker == HistoryEffects::All && gateCalls == commitGateCalls &&
        rendered->effectsQueries == 1);

    // A NotReady journal and non-UI callers conservatively report All.
    int poisonValue = 0;
    StorageStore poisonStorage; SharedHistory poisoned(poisonStorage);
    std::unique_ptr<SharedHistory::NativeTransaction> poisonNative;
    auto poisonSeed = std::make_shared<HistoryEffectsValue>(poisonValue, 0, 1);
    poisonSeed->effects = HistoryEffects::Viewport;
    CHECK(poisoned.PrepareNative("poison peek", poisonSeed, poisonNative) == MMS_OK);
    CHECK(poisoned.CommitNative(std::move(poisonNative)) == MMS_OK);
    CHECK(poisoned.NextPublicationEffects(false) == HistoryEffects::Viewport);
    poisoned.Poison();
    CHECK(poisoned.NextPublicationEffects(false) == HistoryEffects::All &&
        poisoned.NextPublicationEffects(true) == HistoryEffects::All);

    HistoryEffects workerUndo = HistoryEffects::None, workerRedo = HistoryEffects::None;
    std::thread worker([&] {
        workerUndo = history.NextPublicationEffects(false);
        workerRedo = history.NextPublicationEffects(true);
    });
    worker.join();
    CHECK(workerUndo == HistoryEffects::All && workerRedo == HistoryEffects::All);
    CHECK(history.NextPublicationEffects(false) == HistoryEffects::None);
    HistoryEffects emptyWorker = HistoryEffects::None;
    std::thread emptyThread([&] { emptyWorker = empty.NextPublicationEffects(true); });
    emptyThread.join();
    CHECK(emptyWorker == HistoryEffects::All);
}
static void SharedHistoryChecks() {
    SharedHistoryEffectsChecks();
    SharedHistoryEffectsPeekChecks();
    SharedHistoryReserveCopyChecks();
    SharedHistoryNavigationChecks();
    SharedHistoryCleanupBoundaryChecks();
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
