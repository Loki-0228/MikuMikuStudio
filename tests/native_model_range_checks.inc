// Include after native_model_key_checks.inc and native_bone_transform_checks.inc.
// The oracle follows the old bone -> morph -> display registrar sequence, using
// native writers but omitting their renderer/UI wrapper and intermediate seeks.
#include "../src/plugins/native_model_keys.hpp"
namespace {
struct ModelRangeFixture : ModelKeyFixture {
    ModelRangeFixture() {
        app->CurrentFrame() = 20; app->state.lastRegisteredFrame = 25;
        app->FrameRangeStartFrame() = 123; Marks();
        for (auto* fixture : {&first, &second}) {
            fixture->m->maxFrame = 30; fixture->m->searchCursor = 9;
            fixture->m->boneKeys[0].allocated = 2;
            fixture->m->morphKeys[0].allocated = 4;
            fixture->m->displayKeys[0].allocated = 5;
            strcpy_s(fixture->bones[0].name, "rangeBone");
            SeekModelFrame(fixture->raw(), app->CurrentFrame(), 0);
        }
    }
};
plugins::ModelRangeScaleOptions ModelRangeOptions(unsigned families, unsigned start = 10,
    unsigned end = 30, double scale = 2, bool automatic = false) {
    plugins::ModelRangeScaleOptions options;
    options.start = start; options.end = end; options.scale = scale;
    options.bones = (families & 1) != 0; options.morphs = (families & 2) != 0;
    options.display = (families & 4) != 0; options.automaticInterpolation = automatic;
    return options;
}
std::shared_ptr<plugins::HistoryChange> ModelRangePlan(ModelRangeFixture& f,
    const plugins::ModelRangeScaleOptions& options) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanModelRangeScale(*f.app, 0, options, change) == MMS_OK,
          "model range plans the selected families without live mutation");
    return change;
}
struct ModelRangeState {
    ModelKeyState keys;
    std::uint32_t maximum, last;
    int cursor;
    explicit ModelRangeState(const ModelRangeFixture& f) : keys(f.first), maximum(f.first.m->maxFrame),
        last(f.app->state.lastRegisteredFrame), cursor(f.first.m->searchCursor) {}
    bool Matches(const ModelRangeFixture& f) const {
        return keys.Matches(f.first) && maximum == f.first.m->maxFrame &&
            cursor == f.first.m->searchCursor && last == f.app->state.lastRegisteredFrame;
    }
    void RestoreOracle(ModelRangeFixture& f) const {
        keys.Restore(f.second); f.second.m->maxFrame = maximum; f.second.m->searchCursor = cursor;
    }
};
template<class Key> void ModelRangeClearMarks(Key* keys, std::size_t capacity) {
    for (std::size_t slot = 0; slot < capacity; ++slot) keys[slot].allocated = 0;
}
std::uint32_t ModelRangeRelative(std::uint32_t frame, const plugins::ModelRangeScaleOptions& options) {
    if (frame <= options.end) return std::uint32_t(std::int64_t(double(frame - options.start) * options.scale));
    return frame + std::uint32_t(std::int64_t(double(options.end - options.start) * options.scale)) - options.end;
}
void ModelRangeOracle(ModelRangeFixture& f, const plugins::ModelRangeScaleOptions& options) {
    auto& fixture = f.second; auto& model = *fixture.m;
    if (options.bones) {
        const plugins::BoneRangeScaleOptions bone{options.start, options.end, options.scale, options.automaticInterpolation};
        TransformRangeOracle(f, bone);
    }
    if (options.morphs) {
        struct Copy { unsigned owner, frame; float value; };
        std::vector<Copy> copies; auto* keys = model.morphKeys;
        for (std::size_t slot = 0; slot < mdl::MorphKeyCapacity(keys); ++slot) {
            auto& key = keys[slot]; key.allocated = 0;
            if ((!key.frame && slot >= model.morphCount) || key.frame < options.start) continue;
            key.allocated = 1; unsigned owner = unsigned(slot);
            while (owner >= model.morphCount) owner = keys[owner].previous;
            copies.push_back({owner, options.start + ModelRangeRelative(key.frame, options), key.value});
        }
        if (!copies.empty()) {
            ModelRangeClearMarks(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys));
            ModelRangeClearMarks(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys));
            DeleteMarkedModelKeys(fixture.raw(), f.app->CurrentFrame()); ResetMorphKeyCursor(fixture.raw());
            for (const auto& copy : copies)
                Check(RegisterMorphKeyAtIndex(fixture.raw(), int(copy.owner), copy.frame, copy.value),
                      "legacy range oracle registers a saved morph key");
        }
    }
    if (options.display) {
        struct Copy { unsigned frame; unsigned char visible; std::array<unsigned char, 21> ik{};
            std::array<unsigned char, 28> parent{}; };
        std::vector<Copy> copies; auto* keys = model.displayKeys;
        for (std::size_t slot = 0; slot < mdl::KeyPoolCapacity(keys); ++slot) {
            auto& key = keys[slot]; key.allocated = 0;
            if (!key.frame || key.frame < options.start) continue;
            key.allocated = 1; Copy copy{}; copy.frame = ModelRangeRelative(key.frame, options); copy.visible = key.visible;
            std::memcpy(copy.ik.data(), fixture.bones[model.ikChains[0].boneIndex].name, 20);
            copy.ik[20] = key.ikStates[0];
            std::memcpy(copy.parent.data() + 20, &key.selectorStates[0].modelIndex, sizeof(int));
            std::memcpy(copy.parent.data() + 24, &key.selectorStates[0].boneIndex, sizeof(int));
            copies.push_back(copy);
        }
        if (!copies.empty()) {
            ModelRangeClearMarks(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys));
            ModelRangeClearMarks(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys));
            DeleteMarkedModelKeys(fixture.raw(), f.app->CurrentFrame()); ResetDisplayKeyCursor(fixture.raw());
            for (const auto& copy : copies)
                Check(RegisterDisplayKeyFromRecord(fixture.raw(), int(copy.frame), copy.visible, 1,
                    copy.ik.data(), 1, copy.parent.data(), int(options.start)),
                    "legacy range oracle registers saved display IK and parent payloads");
        }
    }
}
void ModelRangeOracleMatches(ModelRangeFixture& f, const plugins::ModelRangeScaleOptions& options, unsigned previousLast) {
    Check(ModelKeyState(f.second).Matches(f.first),
          "all range key bytes and final marks match the old ordered registrar sequence");
    Check(f.first.m->maxFrame == f.second.m->maxFrame && f.first.m->searchCursor == f.second.m->searchCursor &&
          f.app->state.lastRegisteredFrame == (std::max)(previousLast, f.second.m->maxFrame),
          "aggregate range publishes the final native maximum cursor and last registered frame once");
    SeekModelFrame(f.second.raw(), f.app->CurrentFrame(), 0);
    Check((!options.bones || std::fabs(f.first.bones[0].trans[0] - f.second.bones[0].trans[0]) < .0001f) &&
          (!options.morphs || std::fabs(f.first.morphs[0].value - f.second.morphs[0].value) < .0001f) &&
          (!options.display || (f.first.m->loadComplete == f.second.m->loadComplete &&
              f.firstIk[0].enabled == f.secondIk[0].enabled &&
              !std::memcmp(f.firstOrder.data(), f.secondOrder.data(), sizeof(f.firstOrder)))),
          "range samples the edited families at the current playhead");
}
void ModelRangeSevenFamilies() {
    for (unsigned families = 1; families < 8; ++families) for (bool automatic : {false, true})
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        std::printf("  model range families=%u auto=%u case=%u\n", families, unsigned(automatic), scenario);
        ModelRangeFixture f; f.Automatic(automatic);
        const auto options = scenario == 0 ? ModelRangeOptions(families, 10, 30, 2, automatic) :
            scenario == 1 ? ModelRangeOptions(families, 0, 30, .01, automatic) :
                            ModelRangeOptions(families, 15, 30, .5, automatic);
        const ModelRangeState before(f); const auto digest = f.first.Digest(), ring = PoseRingDigest(f.first);
        auto change = ModelRangePlan(f, options);
        Check(before.Matches(f) && f.first.Digest() == digest && !f.history.UndoCount(),
              "seven checkbox combinations are private until common publication");
        ModelKeyCommit(f, change); ModelRangeOracle(f, options); ModelRangeOracleMatches(f, options, before.last);
        Check(f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring && f.SelectionUnchanged() &&
              !f.app->SceneModified() && f.app->FrameRangeStartFrame() == 123,
              "range uses one common node and leaves old rings selection and UI metadata untouched");
        const ModelRangeState after(f);
        f.app->CurrentFrame() = 25; SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(false) == MMS_OK && before.Matches(f) && f.history.RedoCount() == 1,
              "one range undo restores all payloads marks and shared metadata at a changed playhead");
        before.RestoreOracle(f); SeekModelFrame(f.second.raw(), f.app->CurrentFrame(), 0);
        Check((!options.bones || std::fabs(f.first.bones[0].trans[0] - f.second.bones[0].trans[0]) < .0001f) &&
              (!options.morphs || std::fabs(f.first.morphs[0].value - f.second.morphs[0].value) < .0001f),
              "range undo evaluates restored bone and morph tracks at the new frame");
        f.app->CurrentFrame() = 35; SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(true) == MMS_OK && after.Matches(f) && f.history.UndoCount() == 1,
              "one range redo restores all selected families and their final shared metadata");
        ModelRangeOracle(f, options); ModelRangeOracleMatches(f, options, before.last);
    }
}
void ModelRangeEmptyLaterFamily() {
    for (unsigned families : {3u, 5u, 7u}) {
        ModelRangeFixture f;
        for (auto* fixture : {&f.first, &f.second}) {
            if (families == 3) {
                fixture->m->morphKeys[0].next = 0;
                fixture->m->morphKeys[1] = {}; fixture->m->morphKeys[2] = {};
            }
            fixture->m->displayKeys[0].next = 0;
            for (unsigned slot : {1u, 2u}) {
                auto& key = fixture->m->displayKeys[slot]; key.frame = key.previous = key.next = 0; key.allocated = 0;
            }
            // A selected empty display family must clear its own old root mark
            // without discarding marks of the preceding nonempty family.
            fixture->m->displayKeys[0].allocated = 5;
            SeekModelFrame(fixture->raw(), f.app->CurrentFrame(), 0);
        }
        const auto options = ModelRangeOptions(families); const auto previousLast = f.app->state.lastRegisteredFrame;
        ModelKeyCommit(f, ModelRangePlan(f, options)); ModelRangeOracle(f, options);
        ModelRangeOracleMatches(f, options, previousLast);
        const auto marked = [](const auto* keys, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) if (keys[i].allocated) return true; return false;
        };
        Check(families == 7 ? marked(f.first.m->morphKeys, mdl::MorphKeyCapacity(f.first.m->morphKeys)) :
                             marked(f.first.m->boneKeys, mdl::BoneKeyCapacity(f.first.m->boneKeys)),
              "the last nonempty selected family retains its inserted marks when a later selected family is empty");
    }
    {
        ModelRangeFixture f;
        for (auto* fixture : {&f.first, &f.second}) {
            fixture->m->boneKeys[2].next = 0; fixture->m->boneKeys[3] = {};
            SeekModelFrame(fixture->raw(), f.app->CurrentFrame(), 0);
        }
        const auto options = ModelRangeOptions(3); const auto previousLast = f.app->state.lastRegisteredFrame;
        ModelKeyCommit(f, ModelRangePlan(f, options)); ModelRangeOracle(f, options);
        ModelRangeOracleMatches(f, options, previousLast);
        Check(f.first.m->boneKeys[2].frame == 10 && f.first.m->boneKeys[2].allocated == 0 &&
              f.first.m->morphKeys[2].frame == 50 && f.history.UndoCount() == 1,
              "a selected family with unchanged mapped frames still follows the old sequence when another family moves");
    }
}
void ModelRangeTargetWindows() {
    for (unsigned families : {4u, 7u}) {
        ModelRangeFixture f; f.app->CurrentFrame() = 15;
        for (auto* fixture : {&f.first, &f.second}) {
            fixture->bones[1].slotIndex = 0; fixture->m->boneKeys[1].position[0] = 9;
            fixture->m->displayKeys[1].selectorStates[0] = {1, 0};
            static_cast<mdl::BoneOrderEntry*>(fixture->m->boneOrderTable)[0].boneIndex = 1;
            SeekModelFrame(fixture->raw(), f.app->CurrentFrame(), 0);
        }
        Check(f.first.bones[1].trans[0] == 0 && f.firstOrder[0].windowStart == 10,
              "range source parent transition resets the terminal unedited bone");
        const ModelRangeState before(f); const auto options = ModelRangeOptions(families, 0, 30, 2);
        ModelKeyCommit(f, ModelRangePlan(f, options)); ModelRangeOracle(f, options);
        ModelRangeOracleMatches(f, options, before.last);
        Check(f.first.bones[1].trans[0] == 9 && f.first.bones[1].trans[0] == f.second.bones[1].trans[0] &&
              f.firstOrder[0].windowStart == 0 && f.firstOrder[0].windowEnd == 20,
              "range uses the retimed target selector window for an otherwise unedited bone");
        f.app->CurrentFrame() = 16; SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(false) == MMS_OK && before.Matches(f) && f.first.bones[1].trans[0] == 0 &&
              f.firstOrder[0].windowStart == 10, "range undo derives the original external-parent window at the new frame");
        f.app->CurrentFrame() = 18; SeekModelFrame(f.first.raw(), f.app->CurrentFrame(), 0);
        Check(f.history.History(true) == MMS_OK && f.first.bones[1].trans[0] == 9 &&
              f.firstOrder[0].windowEnd == 20, "range redo derives the target window rather than replaying a saved pose");
    }
}
void ModelRangeZeroBoneFamily() {
    for (unsigned families : {3u, 5u, 7u}) {
        ModelRangeFixture f;
        struct EmptyBoneFamily {
            ModelRangeFixture& fixture;
            std::unique_ptr<mdl::ModelRecord> original;
            std::vector<mdl::BoneKey> keys;
            explicit EmptyBoneFamily(ModelRangeFixture& value) : fixture(value),
                original(std::make_unique<mdl::ModelRecord>(*value.first.m)), keys(RegistrationKeys(value.first)) {
                auto& model = *fixture.first.m;
                std::fill_n(model.boneKeys, keys.size(), mdl::BoneKey{});
                model.boneCount = model.ikChainCount = model.boneOrderCount = 0;
                model.boneTable = nullptr; model.bonePhysicsState = model.boneSelection = nullptr;
                model.ikChains = nullptr; model.boneOrderTable = nullptr; model.selectedBone = -1;
            }
            ~EmptyBoneFamily() {
                // Retire every record while its empty-family structure is still
                // current, then restore the fixture's owned table and pool data.
                fixture.history.Reset(); fixture.history.Collect();
                auto& model = *fixture.first.m;
                model.boneCount = original->boneCount; model.boneTable = original->boneTable;
                model.bonePhysicsState = original->bonePhysicsState; model.boneSelection = original->boneSelection;
                model.ikChainCount = original->ikChainCount; model.ikChains = original->ikChains;
                model.boneOrderCount = original->boneOrderCount; model.boneOrderTable = original->boneOrderTable;
                model.selectedBone = original->selectedBone;
                std::memcpy(model.boneKeys, keys.data(), keys.size() * sizeof(mdl::BoneKey));
            }
        } empty(f);
        Check(mdl::BoneKeyPoolIdentity(f.first.m->boneKeys) != 0 && !f.first.m->boneCount &&
              !f.first.m->ikChainCount && !f.first.m->boneOrderCount,
              "zero-bone range fixture retains a registered empty key pool and no dangling IK or parent entries");
        const ModelRangeState before(f); const auto digest = f.first.Digest(), ring = PoseRingDigest(f.first);
        Check(!ModelRangePlan(f, ModelRangeOptions(1)) && before.Matches(f) && f.first.Digest() == digest,
              "selecting only a zero-bone family is a no-op rather than not ready");
        auto change = ModelRangePlan(f, ModelRangeOptions(families));
        Check(before.Matches(f) && f.first.Digest() == digest,
              "zero-bone mixed range planning leaves the scene unchanged");
        ModelKeyCommit(f, change);
        Check((!(families & 2) || f.first.m->morphKeys[2].frame == 50) &&
              (!(families & 4) || f.first.m->displayKeys[2].frame == 50) &&
              f.first.m->maxFrame == 50 && f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring &&
              SameRegistrationKeys(f.first, before.keys.bones),
              "an empty selected bone family permits morph display or mixed range scaling in one common node");
        const ModelRangeState after(f);
        f.app->CurrentFrame() = 25;
        Check(f.history.History(false) == MMS_OK && before.Matches(f),
              "zero-bone mixed range undo restores all key payloads marks and metadata");
        f.app->CurrentFrame() = 35;
        Check(f.history.History(true) == MMS_OK && after.Matches(f) && f.history.UndoCount() == 1,
              "zero-bone mixed range redo remains valid at another playhead");
    }
}
void ModelRangeAtomicFailures() {
    ModelRangeFixture f; const auto options = ModelRangeOptions(7);
    const ModelRangeState before(f); const ModelKeyDerivedState derived(f); const auto digest = f.first.Digest();
    bool planned = false; unsigned planningFailures = 0;
    for (long limit = 0; limit < 2048; ++limit) {
        std::shared_ptr<plugins::HistoryChange> attempt;
        failAfter = limit; const auto result = plugins::PlanModelRangeScale(*f.app, 0, options, attempt); failAfter = -1;
        Check(before.Matches(f) && derived.Matches(f) && f.first.Digest() == digest && !f.history.UndoCount(),
              "range planning allocation failures preserve all families metadata and current evaluation");
        if (result == MMS_OK) { Check(bool(attempt), "range fault sweep eventually plans a change"); planned = true; break; }
        Check(result == MMS_E_MEMORY && !attempt, "range allocation failure returns no partial history record"); ++planningFailures;
    }
    Check(planned && planningFailures > 20, "range planning fault sweep covers all participants");
    auto change = ModelRangePlan(f, options); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    f.first.m->morphKeys[1].value = .375f; const auto staleSource = f.first.Digest();
    Check(f.history.PrepareNative("range stale", change, transaction) == MMS_E_STALE && !transaction &&
          f.first.Digest() == staleSource && derived.Matches(f) && !f.history.UndoCount(),
          "a stale later range family publishes none of the already prepared families");
    f.first.m->morphKeys[1].value = before.keys.morphs[1].value;
    bool prepared = false; unsigned prepareFailures = 0;
    for (long limit = 0; limit < 2048; ++limit) {
        failAfter = limit; const auto result = f.history.PrepareNative("range OOM", change, transaction); failAfter = -1;
        Check(before.Matches(f) && derived.Matches(f) && f.first.Digest() == digest && !f.history.UndoCount(),
              "range preparation allocation failures leave the entire scene and journal unchanged");
        if (result == MMS_OK) { prepared = true; break; }
        Check(result == MMS_E_MEMORY && !transaction, "failed range preparation leaves no pending transaction"); ++prepareFailures;
    }
    Check(prepared && prepareFailures > 20, "range preparation fault sweep reaches the complete aggregate");
    f.first.m->displayKeys[1].ikStates[0] ^= 1; const auto stalePrepared = f.first.Digest();
    Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_E_STALE &&
          f.first.Digest() == stalePrepared && derived.Matches(f) && !f.history.UndoCount(),
          "late display payload changes reject the full prepared range before any native publication");
    f.first.m->displayKeys[1].ikStates[0] ^= 1; ModelKeyCommit(f, change);
}
void ModelRangeNoopsAndRoutes() {
    for (unsigned families = 1; families < 8; ++families) {
        ModelRangeFixture f; TransformRangeDialog dialog(f);
        CheckDlgButton(dialog.window, panel::kScaleBoneCheckbox, families & 1 ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog.window, panel::kScaleMorphCheckbox, families & 2 ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog.window, panel::kScaleDispIkCheckbox, families & 4 ? BST_CHECKED : BST_UNCHECKED);
        const auto digest = f.first.Digest();
        Check(!plugins::TryScaleBoundBoneRange(f.app.get(), dialog.window) && f.first.Digest() == digest,
              "unbound range helper preserves ownership of the original profile path");
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        const ModelRangeState before(f); const auto ring = PoseRingDigest(f.first);
        ApplyFrameRangeScale(dialog.window);
        Check(f.history.UndoCount() == 1 && PoseRingDigest(f.first) == ring && f.app->SceneModified() == 1 &&
              f.app->RecordedFrameCountByte1() == 1 && f.app->FrameRangeStartFrame() == 10 && f.SelectionUnchanged(),
              "actual range dialog routes every checkbox combination to one shared node and publishes UI metadata");
        ModelRangeOracle(f, ModelRangeOptions(families)); ModelRangeOracleMatches(f, ModelRangeOptions(families), before.last);
        Check(f.history.History(false) == MMS_OK && before.Matches(f), "route undo restores the complete authored state");
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; f.app->FrameRangeStartFrame() = 123;
        const auto unchanged = f.first.Digest();
        for (auto options : {ModelRangeOptions(families, 10, 30, 1), ModelRangeOptions(families, 40, 50, 2),
                             ModelRangeOptions(families, 10, 30, 1.00001), ModelRangeOptions(0)})
            Check(!ModelRangePlan(f, options) && before.Matches(f) && f.first.Digest() == unchanged && f.history.RedoCount() == 1,
                  "unit scale empty range integer truncation and unchecked families preserve marks and redo");
        dialog.Text("10", "30", "1"); ApplyFrameRangeScale(dialog.window);
        Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified() &&
              !f.app->RecordedFrameCountByte1() && f.app->FrameRangeStartFrame() == 123,
              "range UI no-op does not dirty the scene or rewrite the selected range start");
        dialog.Text("10", "30", "2");
        Check(f.history.Enter() == MMS_OK, "range route journal gate acquired");
        ApplyFrameRangeScale(dialog.window); f.history.Leave();
        Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
              "busy mixed range never falls through to the old writer");
    }
}
void NativeModelRangeChecks() {
    std::puts("RUN model range seven family combinations"); ModelRangeSevenFamilies();
    std::puts("RUN model range later empty family"); ModelRangeEmptyLaterFamily();
    std::puts("RUN model range target selector windows"); ModelRangeTargetWindows();
    std::puts("RUN model range zero bone family"); ModelRangeZeroBoneFamily();
    std::puts("RUN model range atomic failures"); ModelRangeAtomicFailures();
    std::puts("RUN model range no-op and UI routes"); ModelRangeNoopsAndRoutes();
}
}
