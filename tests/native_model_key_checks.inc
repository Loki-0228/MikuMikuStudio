// Include after the bone/display/morph key checks. Run NativeModelKeyChecks().
#include "../src/plugins/native_model_keys.hpp"
namespace {
struct ModelKeyFixture : BoneRegistrationFixture {
    std::array<mdl::IkChain, 1> firstIk{}, secondIk{};
    std::array<mdl::BoneOrderEntry, 1> firstOrder{}, secondOrder{};
    ModelKeyFixture() {
        Seed(first, firstIk, firstOrder); Seed(second, secondIk, secondOrder);
        app->CurrentFrame() = 10; app->PlaybackPhysicsMode() = 0;
        app->SceneModified() = 0; app->state.lastRegisteredFrame = 37;
    }
    ~ModelKeyFixture() {
        history.Reset(); history.Collect();
        for (auto* f : {&first, &second}) { f->m->ikChains = nullptr; f->m->boneOrderTable = nullptr; }
    }
    static void Seed(Fixture& f, std::array<mdl::IkChain, 1>& ik, std::array<mdl::BoneOrderEntry, 1>& order) {
        f.m->ikChainCount = 1; f.m->ikChains = ik.data(); ik[0].enabled = 1;
        f.m->boneOrderCount = 1; f.m->boneOrderTable = order.data(); order[0] = {0, 0, 0, -1, 0};
        f.m->loadComplete = 1;
        for (unsigned index = 0; index < 2; ++index) {
            f.bones[index].type = mdl::BoneType::RotateMove; f.bones[index].slotIndex = -1;
            f.bones[index].trans[0] = float(100 + index); f.dirty[index] = 0;
            f.m->boneKeys[index].rotation[3] = 1;
        }
        f.m->boneKeys[0].next = 2;
        for (unsigned slot : {2u, 3u}) {
            auto& key = f.m->boneKeys[slot]; key.frame = slot == 2 ? 10 : 30;
            key.previous = slot == 2 ? 0 : 2; key.next = slot == 2 ? 3 : 0;
            key.position[0] = slot == 2 ? 12.f : 30.f; key.rotation[3] = 1;
            for (unsigned lane = 0; lane < 4; ++lane) {
                key.interpolation[lane] = key.interpolation[lane + 4] = 20;
                key.interpolation[lane + 8] = key.interpolation[lane + 12] = 107;
            }
        }
        f.m->morphKeys[0].next = 1;
        f.m->morphKeys[1] = {10, 0, 2, .25f, 0, {}};
        f.m->morphKeys[2] = {30, 1, 0, .75f, 0, {}};
        f.morphs[0].value = .91f;
        for (unsigned slot = 0; slot < 4; ++slot) {
            auto& key = f.m->displayKeys[slot]; key.visible = slot == 1 ? 0 : 1;
            key.ikStates[0] = slot == 1 ? 0 : 1; key.selectorStates[0] = {-1, 0};
        }
        f.m->displayKeys[0].next = 1;
        f.m->displayKeys[1].frame = 10; f.m->displayKeys[1].next = 2;
        f.m->displayKeys[2].frame = 30; f.m->displayKeys[2].previous = 1;
    }
    void Marks(bool bone = true, bool morph = true, bool display = true) {
        for (auto* f : {&first, &second}) {
            f->m->boneKeys[2].allocated = bone ? 3 : 0;
            f->m->morphKeys[1].allocated = morph ? 3 : 0;
            f->m->displayKeys[1].allocated = display ? 3 : 0;
        }
    }
};
struct ModelKeyState {
    std::vector<mdl::BoneKey> bones;
    std::vector<mdl::MorphKey> morphs;
    DisplayTestState display;
    explicit ModelKeyState(const Fixture& f) : bones(RegistrationKeys(f)),
        morphs(f.m->morphKeys, f.m->morphKeys + mdl::MorphKeyCapacity(f.m->morphKeys)), display(f) {}
    bool Matches(const Fixture& f) const {
        return SameRegistrationKeys(f, bones) && mdl::MorphKeyCapacity(f.m->morphKeys) >= morphs.size() &&
            !std::memcmp(f.m->morphKeys, morphs.data(), morphs.size() * sizeof(mdl::MorphKey)) && display.Matches(f);
    }
    void Restore(Fixture& f) const {
        std::memcpy(f.m->boneKeys, bones.data(), bones.size() * sizeof(mdl::BoneKey));
        std::memcpy(f.m->morphKeys, morphs.data(), morphs.size() * sizeof(mdl::MorphKey));
        display.Restore(f);
    }
};
struct ModelKeyDerivedState {
    std::array<mdl::MorphRecord, 1> morphs;
    std::array<mdl::IkChain, 1> ik;
    std::array<mdl::BoneOrderEntry, 1> order;
    explicit ModelKeyDerivedState(const ModelKeyFixture& f) : morphs(f.first.morphs), ik(f.firstIk), order(f.firstOrder) {}
    bool Matches(const ModelKeyFixture& f) const {
        return !std::memcmp(morphs.data(), f.first.morphs.data(), sizeof(morphs)) &&
            !std::memcmp(ik.data(), f.firstIk.data(), sizeof(ik)) &&
            !std::memcmp(order.data(), f.firstOrder.data(), sizeof(order));
    }
};
std::shared_ptr<plugins::HistoryChange> ModelKeyPlan(ModelKeyFixture& f, bool fallback = false, bool automatic = false) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanModelKeyDeletion(*f.app, 0, automatic, change, fallback) == MMS_OK,
          "three-family deletion planning succeeds"); return change;
}
void ModelKeyCommit(ModelKeyFixture& f, const std::shared_ptr<plugins::HistoryChange>& change) {
    Check(bool(change), "three-family deletion contains an authored change");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("model keys", change, transaction) == MMS_OK,
          "three-family journal prepares every candidate");
    Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
          "three-family deletion publishes without allocation");
}
void ModelKeyDerivedMatches(ModelKeyFixture& f, const char* reason) {
    SeekModelFrame(f.second.raw(), f.app->CurrentFrame(), 0);
    Check(std::fabs(f.first.bones[0].trans[0] - f.second.bones[0].trans[0]) < 0.0001f &&
          std::fabs(f.first.morphs[0].value - f.second.morphs[0].value) < 0.0001f &&
          f.first.m->loadComplete == f.second.m->loadComplete &&
          f.firstIk[0].enabled == f.secondIk[0].enabled &&
          !std::memcmp(f.firstOrder.data(), f.secondOrder.data(), sizeof(f.firstOrder)), reason);
}
void ModelKeyMixedHistory() {
    for (bool automatic : {false, true}) {
        ModelKeyFixture f; f.Automatic(automatic); f.Marks(); const auto before = ModelKeyState(f.first);
        const auto digest = f.first.Digest(), ring = PoseRingDigest(f.first);
        const auto change = ModelKeyPlan(f, false, automatic);
        Check(before.Matches(f.first) && f.first.Digest() == digest && !f.history.UndoCount(),
              "mixed-family planning has no live mutations");
        ModelKeyCommit(f, change);
        DeleteMarkedModelKeys(f.second.raw(), f.app->CurrentFrame());
        Check(ModelKeyState(f.second).Matches(f.first), "mixed-family payloads match the actual legacy deletion writer");
        ModelKeyDerivedMatches(f, "mixed deletion evaluates current bone morph visibility and IK");
        Check(f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring && f.SelectionUnchanged() &&
              f.app->SceneModified() == 0 && f.app->state.lastRegisteredFrame == 37,
              "three families use one journal node and preserve legacy rings selection and unrelated metadata");
        const auto after = ModelKeyState(f.first);
        f.app->CurrentFrame() = 20;
        Check(f.history.History(false) == MMS_OK && before.Matches(f.first) && f.history.RedoCount() == 1,
              "one undo restores all three key families at a changed playhead");
        // Restore the oracle's authored keys without invoking its old ring.
        before.Restore(f.second);
        ModelKeyDerivedMatches(f, "mixed undo samples every affected family at the current frame rather than restoring old pose");
        f.app->CurrentFrame() = 30;
        Check(f.history.History(true) == MMS_OK && after.Matches(f.first) && f.history.UndoCount() == 1,
              "one redo restores the complete target key state at another playhead");
        DeleteMarkedModelKeys(f.second.raw(), f.app->CurrentFrame());
        ModelKeyDerivedMatches(f, "mixed redo reevaluates all affected families at the new playhead");
    }
}
void ModelKeyFallbackAndSelection() {
    {
        ModelKeyFixture f;
        Check(!ModelKeyPlan(f), "button deletion has no selected-bone fallback");
        auto change = ModelKeyPlan(f, true); ModelKeyCommit(f, change);
        Check(f.first.m->boneKeys[2].frame == 0 && f.first.m->morphKeys[1].frame == 10 &&
              f.first.m->displayKeys[1].frame == 10 && f.first.selected[0] == 1 && f.SelectionUnchanged(),
              "keyboard fallback deletes only the selected bone at current nonzero frame");
        Check(f.history.History(false) == MMS_OK && f.first.m->boneKeys[2].allocated == 0,
              "fallback undo restores original unmarked selection state");
        Check(!ModelKeyPlan(f) && f.history.RedoCount() == 1,
              "an unmarked button no-op preserves the existing redo branch");
    }
    for (bool display : {false, true}) {
        ModelKeyFixture f; f.Marks(false, !display, display);
        ModelKeyCommit(f, ModelKeyPlan(f, true));
        Check(f.first.m->boneKeys[2].frame == 10 && f.first.m->boneKeys[0].next == 2,
              "an explicit auxiliary-family mark suppresses selected-bone keyboard fallback");
    }
    for (bool display : {false, true}) {
        ModelKeyFixture f;
        if (display) f.first.m->displayKeys[0].allocated = 3;
        else f.first.m->morphKeys[0].allocated = 3;
        ModelKeyCommit(f, ModelKeyPlan(f, true));
        Check(f.first.m->boneKeys[2].frame == 10 && f.first.m->boneKeys[0].next == 2,
              "an explicit frame-zero auxiliary mark also suppresses keyboard fallback");
    }
    {
        ModelKeyFixture f; f.app->CurrentFrame() = 0;
        Check(!ModelKeyPlan(f, true), "keyboard fallback never implicitly deletes frame-zero seeds");
    }
}
void ModelKeySelectorWindows() {
    for (bool mixed : {false, true}) {
        ModelKeyFixture f; f.app->CurrentFrame() = 15; f.Marks(mixed, mixed, true);
        for (auto* fixture : {&f.first, &f.second}) {
            fixture->bones[1].slotIndex = 0;
            fixture->m->boneKeys[1].position[0] = 9;
            fixture->m->displayKeys[1].selectorStates[0] = {1, 0};
            static_cast<mdl::BoneOrderEntry*>(fixture->m->boneOrderTable)[0].boneIndex = 1;
            SeekModelFrame(fixture->raw(), f.app->CurrentFrame(), 0);
        }
        Check(f.first.bones[1].trans[0] == 0 && f.firstOrder[0].windowStart == 10 &&
              f.firstOrder[0].linkedModel == 1,
              "source external-parent window resets the terminal bone pose before deletion");
        const ModelKeyState before(f.first); const auto untouched = f.first.m->boneKeys[1];
        auto change = ModelKeyPlan(f); ModelKeyCommit(f, change);
        DeleteMarkedModelKeys(f.second.raw(), f.app->CurrentFrame());
        ModelKeyDerivedMatches(f, "display and mixed deletion agree with the real seek selector evaluation");
        Check(f.first.bones[1].trans[0] == 9 && f.first.bones[1].trans[0] == f.second.bones[1].trans[0] &&
              f.firstOrder[0].windowStart == 0 && f.firstOrder[0].linkedModel == -1 &&
              !std::memcmp(&f.first.m->boneKeys[1], &untouched, sizeof(untouched)) && f.history.UndoCount() == 1,
              "target windows reevaluate an externally parented bone whose key track was not edited");
        const ModelKeyState after(f.first);
        f.app->CurrentFrame() = 20;
        SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(false) == MMS_OK && before.Matches(f.first),
              "selector-aware aggregate undo restores all authored families");
        before.Restore(f.second); ModelKeyDerivedMatches(f, "selector-aware undo matches native seek at changed frame");
        Check(f.first.bones[1].trans[0] == 0 && f.first.bones[1].trans[0] == f.second.bones[1].trans[0] &&
              f.firstOrder[0].windowStart == 10 && f.firstOrder[0].linkedModel == 1,
              "undo samples the restored external-parent window instead of storing a stale pose");
        f.app->CurrentFrame() = 35;
        SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(true) == MMS_OK && after.Matches(f.first),
              "selector-aware aggregate redo restores the complete deletion");
        DeleteMarkedModelKeys(f.second.raw(), f.app->CurrentFrame());
        ModelKeyDerivedMatches(f, "selector-aware redo matches native seek beyond the terminal display key");
        Check(f.first.bones[1].trans[0] == 9 && f.first.bones[1].trans[0] == f.second.bones[1].trans[0],
              "redo reevaluates the unedited terminal bone with the target parent window");
    }
}
void ModelKeyAtomicFailures() {
    ModelKeyFixture f; f.Marks(); const auto before = ModelKeyState(f.first); const auto digest = f.first.Digest();
    const ModelKeyDerivedState derived(f);
    unsigned planningFailures = 0; bool planned = false;
    for (long limit = 0; limit < 1024; ++limit) {
        std::shared_ptr<plugins::HistoryChange> attempt;
        failAfter = limit; const auto result = plugins::PlanModelKeyDeletion(*f.app, 0, false, attempt); failAfter = -1;
        Check(before.Matches(f.first) && derived.Matches(f) && f.first.Digest() == digest && !f.history.UndoCount(),
              "every aggregate planning allocation failure leaves all native families untouched");
        if (result == MMS_OK) { Check(bool(attempt), "successful fault-sweep plan retains an edit"); planned = true; break; }
        Check(result == MMS_E_MEMORY && !attempt, "failed aggregate planning never returns a partial record"); ++planningFailures;
    }
    Check(planned && planningFailures > 8, "planning allocation sweep traverses all three key families");
    const auto change = ModelKeyPlan(f); Check(bool(change), "failure fixture plans three families");
    const float saved = f.first.m->morphKeys[1].value; f.first.m->morphKeys[1].value = .375f;
    const auto external = f.first.Digest(); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("later family stale", change, transaction) != MMS_OK && !transaction &&
          f.first.Digest() == external && derived.Matches(f) && !f.history.UndoCount(),
          "a later family preparation failure publishes none of the earlier prepared families");
    f.first.m->morphKeys[1].value = saved;
    unsigned failures = 0; bool ready = false;
    for (long limit = 0; limit < 1024; ++limit) {
        failAfter = limit; const auto result = f.history.PrepareNative("mixed OOM", change, transaction); failAfter = -1;
        Check(before.Matches(f.first) && derived.Matches(f) && f.first.Digest() == digest && !f.history.UndoCount(),
              "every aggregate preparation allocation failure preserves all live families");
        if (result == MMS_OK) { ready = true; break; }
        Check(result == MMS_E_MEMORY && !transaction, "mixed preparation reports allocation failure without a transaction"); ++failures;
    }
    Check(ready && failures > 8, "aggregate allocation sweep reaches all candidate families");
    f.first.m->displayKeys[1].ikStates[0] ^= 1; const auto stale = f.first.Digest();
    Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_E_STALE &&
          f.first.Digest() == stale && derived.Matches(f) && !f.history.UndoCount(),
          "late nested display mutation invalidates the whole aggregate before any key is published");
    f.first.m->displayKeys[1].ikStates[0] ^= 1; ModelKeyCommit(f, change);
}
void ModelKeyAliasing() {
    for (bool selectors : {false, true}) {
        ModelKeyFixture f; f.Marks(); auto& display = f.first.m->displayKeys[1];
        auto* ik = display.ikStates; auto* parents = display.selectorStates;
        if (selectors) display.selectorStates = reinterpret_cast<mdl::BoneReference*>(f.first.m->boneKeys);
        else display.ikStates = reinterpret_cast<unsigned char*>(f.first.m->morphKeys);
        const auto digest = f.first.Digest(); std::shared_ptr<plugins::HistoryChange> change;
        const auto result = plugins::PlanModelKeyDeletion(*f.app, 0, false, change);
        Check(result != MMS_OK && !change && f.first.Digest() == digest && !f.history.UndoCount(),
              "cross-family nested storage aliases are rejected before planning or publication");
        display.ikStates = ik; display.selectorStates = parents;
    }
    {
        ModelKeyFixture f; f.Marks(); auto change = ModelKeyPlan(f);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("late alias", change, transaction) == MMS_OK,
              "cross-family alias test prepares against independent storage");
        auto& display = f.first.m->displayKeys[3]; auto* saved = display.selectorStates;
        display.selectorStates = reinterpret_cast<mdl::BoneReference*>(f.first.m->boneOrderTable);
        const auto digest = f.first.Digest();
        Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_E_STALE &&
              f.first.Digest() == digest && !f.history.UndoCount(),
              "replacing an unmodified nested buffer with a live selector alias invalidates the complete prepared write set");
        display.selectorStates = saved;
    }
}
void ModelKeyBorrowedPoolReplacement() {
    for (bool mixed : {false, true}) {
        ModelKeyFixture f; f.Marks(mixed, mixed, true);
        const ModelKeyState before(f.first); const ModelKeyDerivedState derived(f);
        const auto digest = f.first.Digest(); auto change = ModelKeyPlan(f);
        // Put the one-record borrowed array against an uncommitted page. A
        // fallback-capacity traversal would fault on the very next record.
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        auto* reservation = static_cast<unsigned char*>(VirtualAlloc(nullptr,
            std::size_t(system.dwPageSize) * 2, MEM_RESERVE, PAGE_NOACCESS));
        Check(reservation != nullptr, "borrowed display pool guard pages reserved");
        Check(VirtualAlloc(reservation, system.dwPageSize, MEM_COMMIT, PAGE_READWRITE) == reservation,
              "borrowed display pool first page committed");
        auto* original = f.first.m->displayKeys;
        auto* borrowed = ::new(static_cast<void*>(reservation + system.dwPageSize - sizeof(mdl::DisplayKey)))
            mdl::DisplayKey(original[0]);
        const auto saved = *borrowed;
        Check(!mdl::KeyPoolIdentity(borrowed), "replacement one-record pool is deliberately unregistered");
        f.first.m->displayKeys = borrowed;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        const auto result = f.history.PrepareNative("borrowed pool", change, transaction);
        f.first.m->displayKeys = original;
        Check((result == MMS_E_STALE || result == MMS_E_NOT_READY) && !transaction &&
              !std::memcmp(borrowed, &saved, sizeof(saved)) && before.Matches(f.first) && derived.Matches(f) &&
              f.first.Digest() == digest && !f.history.UndoCount(),
              "plan-to-prepare borrowed display replacement is rejected before assumed-capacity reads or writes");
        Check(VirtualFree(reservation, 0, MEM_RELEASE) != FALSE, "borrowed display guard pages released after pointer restoration");
        ModelKeyCommit(f, change);
    }
}
void ModelKeySelectorMembershipStale() {
    for (bool mixed : {false, true}) for (bool existingParent : {false, true}) for (bool prepared : {false, true}) {
        ModelKeyFixture f; f.Marks(mixed, mixed, true);
        if (existingParent) f.first.bones[0].slotIndex = 0;
        const ModelKeyState before(f.first); const ModelKeyDerivedState derived(f);
        auto change = ModelKeyPlan(f);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if (prepared) Check(f.history.PrepareNative("selector guard", change, transaction) == MMS_OK,
                            "selector membership candidate prepares with the original complete bone index vector");
        // Bone 1 has no selected key and was not in the sampling set. With
        // display-only/no existing parent, the original set was entirely empty.
        f.first.bones[1].slotIndex = 0;
        const auto external = f.first.Digest();
        const auto result = prepared ? CommitRegistrationNoAllocation(f.history, std::move(transaction)) :
            f.history.PrepareNative("selector guard", change, transaction);
        Check(result == MMS_E_STALE && !transaction && before.Matches(f.first) && derived.Matches(f) &&
              f.first.Digest() == external && !f.history.UndoCount(),
              "adding an unedited bone to a selector after planning or preparation invalidates the entire aggregate");
        f.first.bones[1].slotIndex = -1;
        ModelKeyCommit(f, change);
    }
}
void NativeModelKeyChecks() {
    std::puts("RUN model key mixed history"); ModelKeyMixedHistory();
    std::puts("RUN model key selection fallback"); ModelKeyFallbackAndSelection();
    std::puts("RUN model key target selector windows"); ModelKeySelectorWindows();
    std::puts("RUN model key aggregate failures"); ModelKeyAtomicFailures();
    std::puts("RUN model key cross-family alias"); ModelKeyAliasing();
    std::puts("RUN model key borrowed pool replacement"); ModelKeyBorrowedPoolReplacement();
    std::puts("RUN model key selector membership stale"); ModelKeySelectorMembershipStale();
}
}
