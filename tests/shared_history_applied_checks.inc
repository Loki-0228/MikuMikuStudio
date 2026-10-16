namespace {
struct AppliedValue final : plugins::HistoryChange {
    int& live;
    int before, after, forwardPrepares = 0, reversePrepares = 0, publications = 0;
    uint64_t bytes = 80;
    bool stale = false, empty = false;
    int* destroyed = nullptr;
    std::function<void()> preparing;
    std::function<void()> destroyingCandidate;
    AppliedValue(int& value, int a, int b) : live(value), before(a), after(b) {}
    ~AppliedValue() { if (destroyed) ++*destroyed; }
    struct Candidate final : plugins::PreparedHistoryChange {
        AppliedValue& value;
        int expected, target;
        Candidate(AppliedValue& v, bool forward) : value(v),
            expected(forward ? v.before : v.after), target(forward ? v.after : v.before) {}
        ~Candidate() { if (value.destroyingCandidate) value.destroyingCandidate(); }
        bool IsCurrent() const noexcept override { return !value.stale && value.live == expected; }
        void Publish() noexcept override { value.live = target; ++value.publications; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        if (forward) ++forwardPrepares; else ++reversePrepares;
        if (preparing) preparing();
        if (!empty) out = std::make_unique<Candidate>(*this, forward);
        return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override { return bytes; }
};
void SharedHistoryAppliedChecks() {
    using plugins::SharedHistory;
    plugins::StorageStore storage;
    SharedHistory history(storage), foreign(storage);
    int value = 0;
    auto change = std::make_shared<AppliedValue>(value, 0, 7);
    std::unique_ptr<SharedHistory::NativeTransaction> legacy;
    Check(history.PrepareNative("old branch", change, legacy) == MMS_OK &&
          history.CommitNative(std::move(legacy)) == MMS_OK && history.History(false) == MMS_OK,
          "applied fixture has redo branch");
    change->forwardPrepares = change->reversePrepares = change->publications = 0;
    uint64_t revision = 0, current = 0;
    Check(history.Revision(revision) == MMS_OK, "applied revision available");
    std::unique_ptr<SharedHistory::AppliedTransaction> transaction, second;
    Check(history.ReserveAppliedNative("huge", UINT64_MAX, transaction) == MMS_E_MEMORY && !transaction,
          "applied maximum is checked before authoring");
    Check(history.ReserveAppliedNative("", 80, transaction) == MMS_E_ARGUMENT && !transaction,
          "applied empty label releases gate");
    Check(history.RedoCount() == 1 && !history.UndoCount() && value == 0,
          "failed applied reservation keeps redo and live state");
    bool reserved = false;
    for (long allocation = 0; allocation < 32; ++allocation) {
        failAfter = allocation;
        const auto result = history.ReserveAppliedNative("applied", 80, transaction);
        failAfter = -1;
        if (result == MMS_OK) { transaction.reset(); reserved = true; break; }
        Check(result == MMS_E_MEMORY && !transaction && history.Enter() == MMS_OK,
              "applied reservation allocation failures release gate");
        history.Leave();
        Check(history.RedoCount() == 1 && !history.UndoCount() && value == 0,
              "applied reservation failures do not trim redo");
    }
    Check(reserved, "applied reservation allocation sweep reaches success");
    Check(history.ReserveAppliedNative("applied", 80, transaction) == MMS_OK,
          "reserve applied before authoring");
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    Check(history.Enter() == MMS_E_BUSY && history.Reset() == MMS_E_BUSY &&
          history.PrepareReset(reset) == MMS_E_BUSY && history.History(false) == MMS_E_BUSY &&
          history.ReserveAppliedNative("second", 80, second) == MMS_E_BUSY && !second,
          "applied token excludes edits and scene reset");
    Check(history.CommitAppliedNative(*transaction) == MMS_E_CONFLICT &&
          history.PrepareAppliedNative(*transaction, {}) == MMS_E_ARGUMENT,
          "applied requires prepared nonempty change");
    Check(foreign.PrepareAppliedNative(*transaction, change) == MMS_E_ARGUMENT &&
          foreign.CommitAppliedNative(*transaction) == MMS_E_ARGUMENT,
          "applied token belongs to one journal");
    MmsResult prepareWorker = MMS_OK, commitWorker = MMS_OK;
    std::thread worker([&] {
        prepareWorker = history.PrepareAppliedNative(*transaction, change);
        commitWorker = history.CommitAppliedNative(*transaction);
    });
    worker.join();
    Check(prepareWorker == MMS_E_THREAD && commitWorker == MMS_E_THREAD,
          "applied preparation and commit reject worker threads");
    value = 7;
    change->preparing = [&] {
        Check(history.PrepareAppliedNative(*transaction, change) == MMS_E_BUSY &&
              history.CommitAppliedNative(*transaction) == MMS_E_BUSY,
              "applied callbacks cannot recursively prepare or commit");
    };
    Check(history.PrepareAppliedNative(*transaction, change) == MMS_OK && value == 7,
          "applied prepare validates after state without replay");
    change->preparing = {};
    Check(history.PrepareAppliedNative(*transaction, change) == MMS_E_CONFLICT,
          "applied prepare accepted once");
    failAfter = 0;
    const auto committed = history.CommitAppliedNative(*transaction);
    failAfter = -1;
    Check(committed == MMS_OK && value == 7 && change->reversePrepares == 1 &&
          change->forwardPrepares == 0 && change->publications == 0,
          "applied commit allocates nothing and never publishes inverse or forward");
    Check(history.Revision(current) == MMS_OK && current == revision + 1 &&
          history.UndoCount() == 1 && !history.RedoCount(), "applied commit advances cursor once");
    Check(history.CommitAppliedNative(*transaction) == MMS_E_CONFLICT && history.History(false) == MMS_E_BUSY,
          "committed applied token continues excluding reentry until cleanup");
    auto* releasing = transaction.get();
    change->destroyingCandidate = [&] {
        Check(history.PrepareAppliedNative(*releasing, change) == MMS_E_BUSY &&
              history.CommitAppliedNative(*releasing) == MMS_E_BUSY,
              "applied candidate destruction cannot reenter its releasing token");
    };
    transaction.reset();
    change->destroyingCandidate = {};
    Check(history.History(false) == MMS_OK && value == 0 && history.History(true) == MMS_OK && value == 7,
          "adopted native change supports normal undo redo");
    Check(history.History(false) == MMS_OK && value == 0, "applied failures start from redo branch");
    Check(history.Revision(revision) == MMS_OK, "failure revision available");
    // Every rejected preparation/commit leaves the reservation owned by the
    // caller, who can restore captured pose before reopening the journal gate.
    for (int failure = 0; failure < 7; ++failure) {
        auto rejected = std::make_shared<AppliedValue>(value, 0, 9);
        Check(history.ReserveAppliedNative("failure", 80, transaction) == MMS_OK, "reserve failure scope");
        value = 9;
        MmsResult result = MMS_OK;
        if (failure == 0) {
            rejected->bytes = 81; result = history.PrepareAppliedNative(*transaction, rejected);
            Check(result == MMS_E_MEMORY, "applied actual bytes cannot exceed reservation");
        } else if (failure == 1) {
            failAfter = 0; result = history.PrepareAppliedNative(*transaction, rejected); failAfter = -1;
            Check(result == MMS_E_MEMORY, "applied candidate allocation failure is recoverable");
        } else if (failure == 2) {
            rejected->empty = true; result = history.PrepareAppliedNative(*transaction, rejected);
            Check(result == MMS_E_FAILED, "applied candidate must exist");
        } else if (failure == 3) {
            rejected->stale = true; result = history.PrepareAppliedNative(*transaction, rejected);
            Check(result == MMS_E_STALE, "applied preparation rejects stale live state");
        } else if (failure == 4) {
            rejected->preparing = [&] { ++rejected->bytes; };
            result = history.PrepareAppliedNative(*transaction, rejected);
            Check(result == MMS_E_STALE, "applied candidate cannot change retained byte count");
        } else {
            Check(history.PrepareAppliedNative(*transaction, rejected) == MMS_OK, "prepare later rejection");
            if (failure == 5) value = 10; else ++rejected->bytes;
            failAfter = 0; result = history.CommitAppliedNative(*transaction); failAfter = -1;
            Check(result == MMS_E_STALE, "applied commit rechecks source and retained bytes");
        }
        Check(transaction && history.Enter() == MMS_E_BUSY && history.RedoCount() == 1 && !history.UndoCount(),
              "applied failure retains lock and existing branch");
        Check(history.Revision(current) == MMS_OK && current == revision, "failed applied edit preserves revision");
        value = 0; // stand-in for the native capture's no-allocation Rollback
        transaction.reset();
        Check(history.Enter() == MMS_OK, "rollback then release reopens journal"); history.Leave();
    }
    Check(history.ReserveAppliedNative("prepared storage", 80, transaction) == MMS_OK,
          "reserve before serializer prepare");
    value = 7;
    Check(storage.Prepare({}) == MMS_OK && history.PrepareAppliedNative(*transaction, change) == MMS_E_BUSY,
          "applied preparation refuses prepared storage");
    storage.Cancel();
    Check(history.PrepareAppliedNative(*transaction, change) == MMS_OK && storage.Prepare({}) == MMS_OK &&
          history.CommitAppliedNative(*transaction) == MMS_E_BUSY, "applied commit refuses prepared storage");
    storage.Cancel(); value = 0; transaction.reset();
    // Cancel itself is intentionally not an implicit native rollback.
    Check(history.ReserveAppliedNative("cancel contract", 80, transaction) == MMS_OK, "reserve cancel contract");
    value = 11; transaction.reset();
    Check(value == 11 && history.RedoCount() == 1, "token only cancels reservation; capture owns rollback"); value = 0;
    // Even an outstanding adopted candidate cannot retain a scene after its
    // journal dies. Native captures themselves must be closed before teardown.
    int destroyed = 0;
    {
        auto shortLived = std::make_unique<SharedHistory>(storage);
        auto owned = std::make_shared<AppliedValue>(value, 0, 12); owned->destroyed = &destroyed;
        Check(shortLived->ReserveAppliedNative("orphan", 80, transaction) == MMS_OK, "reserve orphan scope");
        value = 12;
        Check(shortLived->PrepareAppliedNative(*transaction, owned) == MMS_OK, "prepare orphan scope");
        value = 0; owned.reset(); shortLived.reset();
    }
    Check(destroyed == 1 && foreign.PrepareAppliedNative(*transaction, change) == MMS_E_STALE &&
          foreign.CommitAppliedNative(*transaction) == MMS_E_STALE, "journal releases orphan applied owners before scene destruction");
    transaction.reset();
}
}
