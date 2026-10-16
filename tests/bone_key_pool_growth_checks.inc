// Include in native_edit_history_tests.cpp after Check/failAfter, then call
// BoneKeyPoolGrowthChecks(). These checks use only CPU-owned pool fixtures.
struct BoneGrowthFixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    explicit BoneGrowthFixture(std::size_t capacity = 64) {
        model->boneKeys = mdl::BoneKeyPoolCreate(capacity);
    }
    ~BoneGrowthFixture() { mdl::BoneKeyPoolDestroy(model->boneKeys); }
};

void BoneKeyPoolGrowthChecks() {
    using Growth = mdl::BoneKeyPoolGrowth;
    {
        BoneGrowthFixture f;
        auto* old = f.model->boneKeys;
        const auto identity = mdl::BoneKeyPoolIdentity(old);
        old[7].frame = 41; f.model->keyVisitMap[7] = 3;
        f.model->keyVisitMap[80] = 9;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, 65, stage) && stage->IsCurrent() && stage->Grows(),
              "bone growth prepares a current candidate");
        auto* candidate = stage->CandidatePool();
        const auto capacity = stage->CandidateCapacity();
        Check(candidate != old && capacity == mdl::BoneKeyPlanCapacity(64, 65) &&
                  stage->CandidateIdentity() == identity && f.model->boneKeys == old &&
                  mdl::BoneKeyCapacity(old) == 64 && mdl::BoneKeyPoolIdentity(old) == identity,
              "prepare preserves public pointer capacity and identity");
        Check(mdl::BoneKeyPoolIdentity(candidate) == 0 && mdl::BoneKeyCapacity(candidate) == 0,
              "private bone growth candidate is not a public pool");
        auto alias = std::make_unique<mdl::ModelRecord>();
        alias->boneKeys = candidate;
        const auto hidden = mdl::BoneKeyVisitMapOf(*alias, capacity);
        Check(hidden.bytes == nullptr && hidden.size == 0 && mdl::BoneKeyVisitMapSize(*alias) == 0,
              "private candidate never falls back to the borrowed visit map");
        auto* sameStage = stage.get();
        Check(!Growth::Prepare(*f.model, 65, stage) && stage.get() == sameStage && stage->IsCurrent(),
              "nonempty output is preserved on prepare refusal");
        failAfter = 0; stage.reset(); failAfter = -1;
        Check(f.model->boneKeys == old && old[7].frame == 41 &&
                  f.model->keyVisitMap[7] == 3 && f.model->keyVisitMap[80] == 9 &&
                  mdl::BoneKeyPoolIdentity(old) == identity && mdl::BoneKeyPoolIdentity(candidate) == 0,
              "unpublished cancellation neither allocates nor alters the source");

        Check(Growth::Prepare(*f.model, 65, stage), "prepare exact bone growth rollback");
        candidate = stage->CandidatePool();
        // Key authorship is checked by the edit, not this allocation token.
        old[7].frame = 42;
        Check(stage->IsCurrent(), "authored changes alone do not invalidate an allocation token");
        failAfter = 0; const bool published = stage->Publish(); failAfter = -1;
        Check(published && stage->IsCurrent() && f.model->boneKeys == candidate &&
                  candidate[7].frame == 42 && mdl::BoneKeyPoolIdentity(candidate) == identity &&
                  mdl::BoneKeyCapacity(candidate) == capacity &&
                  mdl::BoneKeyPoolIdentity(old) == 0 && mdl::BoneKeyCapacity(old) == 0 &&
                  f.model->keyVisitMap[7] == 3 && f.model->keyVisitMap[80] == 0,
              "no-allocation publish refreshes keys and makes only the grown pool public");
        alias->boneKeys = old;
        const auto dormant = mdl::BoneKeyVisitMapOf(*alias, 64);
        Check(dormant.bytes == nullptr && dormant.size == 0,
              "dormant source visit storage remains private");
        candidate[7].frame = 99; f.model->keyVisitMap[7] = 8; f.model->keyVisitMap[80] = 6;
        failAfter = 0; const bool rolledBack = stage->Rollback(); failAfter = -1;
        Check(rolledBack && f.model->boneKeys == old && old[7].frame == 42 &&
                  mdl::BoneKeyCapacity(old) == 64 && mdl::BoneKeyPoolIdentity(old) == identity &&
                  f.model->keyVisitMap[7] == 3 && f.model->keyVisitMap[80] == 9 &&
                  mdl::BoneKeyPoolIdentity(candidate) == 0,
              "no-allocation rollback restores the exact source and entire affected inline visit range");
        Check(!stage->IsCurrent() && !stage->Publish() && !stage->Rollback() && !stage->Accept() &&
                  !stage->CandidatePool() && !stage->CandidateCapacity() && !stage->CandidateIdentity(),
              "a rolled-back growth token is consumed");
        stage.reset();

        Check(Growth::Prepare(*f.model, 65, stage) && stage->Publish(), "prepare accepted bone growth");
        candidate = f.model->boneKeys; candidate[7].frame = 67;
        failAfter = 0; const bool accepted = stage->Accept(); failAfter = -1;
        Check(accepted && f.model->boneKeys == candidate && candidate[7].frame == 67 &&
                  mdl::BoneKeyPoolIdentity(candidate) == identity && mdl::BoneKeyPoolIdentity(old) == 0,
              "accept frees the dormant owned allocation and keeps authored edits");
        failAfter = 0;
        const bool fitted = mdl::BoneKeyPoolEnsure(*f.model, capacity);
        stage.reset();
        failAfter = -1;
        Check(fitted && f.model->boneKeys == candidate && mdl::BoneKeyPoolIdentity(candidate) == identity,
              "accepted-token cleanup and fitted legacy ensure require no allocation");
    }
    {
        BoneGrowthFixture f;
        auto* old = f.model->boneKeys;
        const auto identity = mdl::BoneKeyPoolIdentity(old);
        old[3].frame = 12; f.model->keyVisitMap[3] = 4;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, 64, stage) && !stage->Grows() &&
                  stage->CandidatePool() == old && stage->CandidateCapacity() == 64 &&
                  stage->CandidateIdentity() == identity, "no-growth token keeps the source allocation");
        failAfter = 0; const bool published = stage->Publish(); failAfter = -1;
        Check(published && stage->IsCurrent(), "no-growth publish needs no allocation");
        old[3].frame = 13; f.model->keyVisitMap[3] = 7;
        failAfter = 0; const bool rollback = stage->Rollback(); failAfter = -1;
        Check(rollback && old[3].frame == 13 && f.model->keyVisitMap[3] == 4 &&
                  f.model->boneKeys == old && mdl::BoneKeyPoolIdentity(old) == identity,
              "no-growth rollback restores visits and leaves authored-key rollback to the caller");
        stage.reset();
        Check(Growth::Prepare(*f.model, 64, stage) && stage->Publish(), "prepare no-growth accept");
        f.model->keyVisitMap[3] = 5;
        failAfter = 0; const bool accepted = stage->Accept(); stage.reset(); failAfter = -1;
        Check(accepted && f.model->keyVisitMap[3] == 5 && mdl::BoneKeyPoolIdentity(old) == identity,
              "no-growth accept keeps visit edits and releases the reservation");
    }
    {
        BoneGrowthFixture f;
        auto* old = f.model->boneKeys;
        f.model->keyVisitMap[7] = 4;
        const auto identity = mdl::BoneKeyPoolIdentity(old);
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, std::uint64_t(mdl::kBoneKeyCapacity) + 1, stage),
              "prepare inline-to-side visit growth");
        failAfter = 0; const bool published = stage->Publish(); failAfter = -1;
        auto visit = mdl::BoneKeyVisitMapOf(*f.model);
        Check(published && visit.bytes != f.model->keyVisitMap && visit.size == stage->CandidateCapacity() &&
                  visit.bytes[7] == 4 && visit.bytes[64] == 0,
              "published grown side visit map preserves source marks and clears new slots");
        visit.bytes[7] = 9;
        failAfter = 0; stage.reset(); failAfter = -1;
        visit = mdl::BoneKeyVisitMapOf(*f.model);
        Check(f.model->boneKeys == old && visit.bytes == f.model->keyVisitMap && visit.size == 64 &&
                  visit.bytes[7] == 4 && mdl::BoneKeyPoolIdentity(old) == identity,
              "destructor rollback restores the exact inline allocation without allocating");
    }
    {
        BoneGrowthFixture f(mdl::kBoneKeyCapacity + 1);
        auto* old = f.model->boneKeys;
        const auto identity = mdl::BoneKeyPoolIdentity(old);
        const auto before = mdl::BoneKeyVisitMapOf(*f.model);
        before.bytes[before.size - 1] = 6;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, std::uint64_t(before.size) + 1, stage) && stage->Publish(),
              "prepare side-to-side visit growth");
        const auto after = mdl::BoneKeyVisitMapOf(*f.model);
        Check(after.bytes != before.bytes && after.bytes[before.size - 1] == 6 &&
                  after.bytes[before.size] == 0, "side-to-side growth preserves the private old map");
        after.bytes[before.size - 1] = 2;
        failAfter = 0; const bool rollback = stage->Rollback(); failAfter = -1;
        const auto restored = mdl::BoneKeyVisitMapOf(*f.model);
        Check(rollback && f.model->boneKeys == old && restored.bytes == before.bytes &&
                  restored.size == before.size && restored.bytes[before.size - 1] == 6 &&
                  mdl::BoneKeyPoolIdentity(old) == identity,
              "side-map rollback restores the exact buffer capacity contents and identity");
    }
    {
        auto model = std::make_unique<mdl::ModelRecord>();
        std::vector<mdl::BoneKey> borrowed(mdl::kBoneKeyCapacity);
        model->boneKeys = borrowed.data(); borrowed[7].frame = 91; model->keyVisitMap[7] = 3;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*model, std::uint64_t(borrowed.size()) + 1, stage) &&
                  stage->CandidateIdentity() != 0 && mdl::BoneKeyPoolIdentity(borrowed.data()) == 0,
              "borrowed growth stages fresh ownership without claiming the source");
        const auto cancelledIdentity = stage->CandidateIdentity();
        Check(stage->Publish(), "publish borrowed growth");
        model->boneKeys[7].frame = 22;
        failAfter = 0; const bool rollback = stage->Rollback(); failAfter = -1;
        Check(rollback && model->boneKeys == borrowed.data() && borrowed[7].frame == 91 &&
                  mdl::BoneKeyPoolIdentity(model->boneKeys) == 0 && model->keyVisitMap[7] == 3,
              "borrowed rollback restores weak identity and never frees caller storage");
        stage.reset();
        Check(Growth::Prepare(*model, std::uint64_t(borrowed.size()) + 1, stage) &&
                  stage->CandidateIdentity() != cancelledIdentity && stage->Publish(),
              "a cancelled borrowed adoption identity is never reused");
        failAfter = 0; const bool accepted = stage->Accept(); failAfter = -1;
        Check(accepted && mdl::BoneKeyPoolIdentity(model->boneKeys) != 0 && borrowed[7].frame == 91,
              "accepting borrowed adoption keeps the caller-owned source alive");
        mdl::BoneKeyPoolDestroy(model->boneKeys);
        Check(borrowed[7].frame == 91, "destroying accepted borrowed growth cannot release the borrowed source");
    }
    {
        BoneGrowthFixture f;
        auto* old = f.model->boneKeys;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, 65, stage), "prepare visit-stale growth");
        f.model->keyVisitMap[3] = 1;
        Check(!stage->IsCurrent() && !stage->Publish() && f.model->boneKeys == old,
              "visit changes invalidate preparation without partially publishing");
        failAfter = 0; stage.reset(); failAfter = -1;
        Check(f.model->keyVisitMap[3] == 1, "cancelling stale preparation preserves later visit edits");
        Check(Growth::Prepare(*f.model, 65, stage), "prepare pointer-stale growth");
        auto* replacement = mdl::BoneKeyPoolCreate(32);
        f.model->boneKeys = replacement;
        Check(!stage->IsCurrent() && !stage->Publish(), "replacement pointer invalidates preparation");
        failAfter = 0; stage.reset(); failAfter = -1;
        Check(f.model->boneKeys == replacement && mdl::BoneKeyPoolIdentity(old) != 0,
              "stale unpublished cleanup does not overwrite or release the source owner");
        mdl::BoneKeyPoolDestroy(old);

        Check(Growth::Prepare(*f.model, 33, stage) && stage->Publish(), "prepare published stale growth");
        auto* grown = f.model->boneKeys;
        auto* foreign = mdl::BoneKeyPoolCreate(16);
        f.model->boneKeys = foreign;
        Check(!stage->Rollback() && !stage->Accept(), "stale publication refuses explicit rollback and accept");
        failAfter = 0; stage.reset(); failAfter = -1;
        Check(f.model->boneKeys == foreign && mdl::BoneKeyPoolIdentity(foreign) != 0 &&
                  mdl::BoneKeyPoolIdentity(replacement) == 0 && mdl::BoneKeyPoolIdentity(grown) == 0,
              "stale published cleanup retires token-owned blocks without overwriting a replacement");

        Check(Growth::Prepare(*f.model, 16, stage) && stage->Publish(), "prepare stale no-growth token");
        replacement = mdl::BoneKeyPoolCreate(8); f.model->boneKeys = replacement;
        failAfter = 0; stage.reset(); failAfter = -1;
        Check(f.model->boneKeys == replacement && mdl::BoneKeyPoolIdentity(foreign) != 0,
              "stale no-growth cleanup releases only its reservation, not original ownership");
        mdl::BoneKeyPoolDestroy(foreign);
    }
    {
        BoneGrowthFixture f;
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, 65, stage), "prepare destruction identity guard");
        const auto oldIdentity = mdl::BoneKeyPoolIdentity(f.model->boneKeys);
        mdl::BoneKeyPoolDestroy(f.model->boneKeys);
        f.model->boneKeys = mdl::BoneKeyPoolCreate(64);
        const auto replacementIdentity = mdl::BoneKeyPoolIdentity(f.model->boneKeys);
        Check(replacementIdentity != oldIdentity && !stage->IsCurrent() && !stage->Publish(),
              "destroy and recreate cannot revive an old allocation guard even at a recycled address");
        auto* replacement = f.model->boneKeys;
        stage.reset();
        Check(f.model->boneKeys == replacement && mdl::BoneKeyPoolIdentity(replacement) == replacementIdentity,
              "old-token cleanup cannot retire a replacement allocation identity");
    }
    {
        BoneGrowthFixture f;
        auto* old = f.model->boneKeys;
        const auto identity = mdl::BoneKeyPoolIdentity(old);
        old[7].frame = 29; f.model->keyVisitMap[7] = 4;
        unsigned refused = 0; bool reachedSuccess = false;
        for (long allowance = 0; allowance < 32 && !reachedSuccess; ++allowance) {
            std::unique_ptr<Growth> stage;
            failAfter = allowance;
            const bool prepared = Growth::Prepare(*f.model, 65, stage);
            failAfter = -1;
            Check(f.model->boneKeys == old && mdl::BoneKeyCapacity(old) == 64 &&
                      mdl::BoneKeyPoolIdentity(old) == identity && old[7].frame == 29 &&
                      f.model->keyVisitMap[7] == 4,
                  "every injected prepare allocation failure preserves source state");
            if (prepared) {
                reachedSuccess = true;
                Check(stage && stage->IsCurrent(), "successful allocation sweep returns a valid token");
                failAfter = 0; stage.reset(); failAfter = -1;
            } else { ++refused; Check(!stage, "failed prepare leaves output empty"); }
        }
        Check(reachedSuccess && refused >= 4, "fault sweep reaches token visit registry and wrapper allocations");
        std::unique_ptr<Growth> stage;
        // Impl and inline before-image consume two new allocations; the grown
        // side-map allocation then fails after calloc has created the new pool.
        failAfter = 2;
        const bool sidePrepared = Growth::Prepare(*f.model, std::uint64_t(mdl::kBoneKeyCapacity) + 1, stage);
        failAfter = -1;
        Check(!sidePrepared && !stage && f.model->boneKeys == old &&
                  mdl::BoneKeyPoolIdentity(old) == identity && old[7].frame == 29,
              "side-map allocation failure frees only private storage");
        Check(!Growth::Prepare(*f.model, std::uint64_t(mdl::kBoneKeyCapacityLimit) + 1, stage) && !stage &&
                  f.model->boneKeys == old && mdl::BoneKeyPoolIdentity(old) == identity,
              "structurally impossible growth leaves output and ownership untouched");
        Check(mdl::BoneKeyPoolEnsure(*f.model, 65) && mdl::BoneKeyPoolIdentity(f.model->boneKeys) == identity,
              "legacy ensure still succeeds after cancelled and failed preparations");
    }
    {
        BoneGrowthFixture f(0);
        std::unique_ptr<Growth> stage;
        Check(Growth::Prepare(*f.model, 0, stage) && !stage->Grows() &&
                  !stage->CandidatePool() && !stage->CandidateIdentity() && stage->Publish() && stage->Accept(),
              "an empty no-growth token completes without inventing ownership");
        stage.reset(); f.model->keyVisitMap[7] = 5;
        Check(Growth::Prepare(*f.model, 1, stage) && stage->CandidateIdentity() != 0 && stage->Publish(),
              "empty pool growth establishes fresh staged ownership");
        Check(f.model->keyVisitMap[7] == 0 && stage->Rollback() && !f.model->boneKeys &&
                  f.model->keyVisitMap[7] == 5,
              "empty pool rollback restores its null pointer and affected inline visit bytes");
    }
}
