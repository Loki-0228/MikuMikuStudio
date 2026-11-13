// Include after native_model_paste_family_checks.inc (and the model deletion
// target/range checks). The parent TU registers NativeModelDeleteRecordOwnerChecks.
#include "../src/plugins/scene_object_history.hpp"
namespace model_delete_record_owner_checks {
using Owner = std::shared_ptr<plugins::RetainedSceneObject>;

// Fixed storage keeps the traversal check independent of Collector allocation.
// These shared_ptr copies are test observations, never owner-budget payload.
struct Owners {
    std::array<Owner, 16> values{};
    std::size_t size = 0, calls = 0;
    bool invalid = false;
    static void Visit(void* context, const Owner& owner) {
        auto& out = *static_cast<Owners*>(context); ++out.calls;
        if (!owner) { out.invalid = true; return; }
        for (std::size_t i = 0; i < out.size; ++i) if (out.values[i] == owner) return;
        if (out.size == out.values.size()) { out.invalid = true; return; }
        out.values[out.size++] = owner;
    }
    bool Contains(const Owners& other) const noexcept {
        for (std::size_t i = 0; i < other.size; ++i) {
            bool found = false;
            for (std::size_t j = 0; j < size; ++j) found |= values[j] == other.values[i];
            if (!found) return false;
        }
        return true;
    }
};
struct NoAllocation {
    long previous = failAfter;
    NoAllocation() { failAfter = 0; }
    ~NoAllocation() { failAfter = previous; }
};
template<class T> Owners Read(const T& value) {
    Owners out;
    { NoAllocation guard; value.VisitRetainedModels(&out, Owners::Visit); }
    Check(!out.invalid && out.size, "owner traversal has valid holders and allocates nothing at failAfter zero");
    return out;
}
struct Ledger {
    plugins::resource_budget::Collector collector;
    MmsResult status = MMS_OK;
    static void Visit(void* context, const Owner& owner) {
        auto& out = *static_cast<Ledger*>(context);
        if (out.status == MMS_OK) out.status = owner->MeasureBookkeeping(out.collector);
    }
    std::uint64_t Bytes() const { return collector.Result().totalBytes; }
    void Complete() const {
        Check(status == MMS_OK && collector.Result().Complete(), "visited holders have complete bookkeeping measurements");
    }
};
template<class Record, class Candidate> void CheckUnion(const Record& record, const Candidate& candidate) {
    const auto payload = record.RetainedBytes();
    const auto existing = Read(record), prepared = Read(candidate);
    Check(prepared.Contains(existing), "candidate owners include every holder retained by the persistent record");
    Ledger combined;
    record.VisitRetainedModels(&combined, Ledger::Visit);
    candidate.VisitRetainedModels(&combined, Ledger::Visit);
    combined.Complete();
    const auto bytes = combined.Bytes();
    Ledger unique;
    for (std::size_t i = 0; i < prepared.size; ++i) Ledger::Visit(&unique, prepared.values[i]);
    unique.Complete();
    Check(bytes == unique.Bytes() && bytes > 0,
        "one collector deduplicates record/candidate model and parent holders without charging shared_ptr copies");
    record.VisitRetainedModels(&combined, Ledger::Visit);
    candidate.VisitRetainedModels(&combined, Ledger::Visit);
    combined.Complete();
    Check(combined.Bytes() == bytes && record.RetainedBytes() == payload,
        "repeated visits preserve owner charge and the record's original payload accounting");
    bool covered = true;
    {
        NoAllocation guard;
        for (std::size_t i = 0; i < prepared.size; ++i)
            covered &= prepared.values[i]->CoversBookkeeping(combined.collector);
    }
    Check(covered, "one shared ledger covers all visited holders without allocation");
}
void Aggregates() {
    using namespace model_delete_target_checks;
    for (auto family : {Families::All, Families::Bone, Families::Morph, Families::Display})
    for (bool privateSelection : {false, true}) {
        ModelKeyFixture f; const auto selection = Seed(f, family, privateSelection);
        f.app->CurrentFrame() = -17;
        const auto change = Plan(f, privateSelection ? &selection : nullptr);
        const auto original = Read(*change);
        const auto edited = plugins::RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 0);
        Check(original.size == 1 && original.values[0] == edited,
            "deletion records retain only the edited model; raw selector indices are not parent holders");
        if (family == Families::All)
            Check(original.calls == (privateSelection ? 5u : 4u),
                "aggregate visits bone, attached display, morph, display and optional metadata holders");
        // A visitor must enumerate captured holders even while live slots are
        // unavailable; it must not reconstruct them by calling RetainSceneObject.
        struct HiddenSlots {
            MMDApp& app; unsigned char* first; unsigned char* second;
            explicit HiddenSlots(MMDApp& a) : app(a), first(a.ModelSlot(0)), second(a.ModelSlot(1)) {
                app.ModelSlot(0) = app.ModelSlot(1) = nullptr;
            }
            ~HiddenSlots() { app.ModelSlot(0) = first; app.ModelSlot(1) = second; }
        };
        {
            HiddenSlots hidden(*f.app); const auto detached = Read(*change);
            Check(detached.Contains(original) && original.Contains(detached) && detached.calls == original.calls,
                "record enumeration is independent of current scene membership and unrelated models");
        }
        struct VisitorFailure {};
        bool propagated = false;
        try {
            NoAllocation guard;
            change->VisitRetainedModels(nullptr, [](void*, const Owner&) { throw VisitorFailure{}; });
        } catch (const VisitorFailure&) { propagated = true; }
        Check(propagated, "record visitor preserves callback exceptions for coordinator budget refusal");
        for (bool forward : {true, false, true}) {
            std::unique_ptr<Prepared> candidate;
            Check(change->PrepareSigned(forward, candidate) == MMS_OK && candidate && candidate->IsCurrent(),
                "prepare real signed aggregate for owner containment in both directions");
            CheckUnion(*change, *candidate);
            const auto owners = Read(*candidate);
            Check(owners.size == (family == Families::Morph ? 1u : 2u),
                "candidate adds only relevant parent holders including unchanged display reads");
            Check(Read(*change).size == original.size,
                "candidate-only parent leases never migrate into persistent deletion records");
            Publish(*candidate);
        }
    }
}
void FamiliesAndSourceParents() {
    ModelKeyFixture f;
    model_delete_target_checks::Seed(f, model_delete_target_checks::Families::All, false);
    f.app->CurrentFrame() = -17;
    std::shared_ptr<plugins::DisplayKeyDeletion> display;
    std::shared_ptr<plugins::BoneKeyDeletion> bone;
    std::shared_ptr<plugins::MorphKeyDeletion> morph;
    Check(plugins::PlanSignedDisplayKeyDeletion(*f.app, 0, display, true, false) == MMS_OK && display &&
        plugins::PlanSignedBoneKeyDeletion(*f.app, 0, false, bone, false, true, display) == MMS_OK && bone &&
        plugins::PlanSignedMorphKeyDeletion(*f.app, 0, morph, true) == MMS_OK && morph,
        "plan all three persistent deletion interfaces independently");
    std::unique_ptr<plugins::PreparedDisplayKeyDeletion> d;
    std::unique_ptr<plugins::PreparedBoneKeyDeletion> b;
    std::unique_ptr<plugins::PreparedMorphKeyDeletion> m;
    Check(display->PrepareSigned(true, d) == MMS_OK && d &&
        bone->PrepareSigned(true, &d->ZeroTarget().selectors, b) == MMS_OK && b &&
        morph->PrepareSigned(true, m) == MMS_OK && m, "prepare the actual family candidate hierarchy");
    CheckUnion(*display, *d); CheckUnion(*bone, *b); CheckUnion(*morph, *m);
    Check(Read(*bone).calls == 2 && Read(*bone).size == 1,
        "bone record recursively visits its attached display holder without double charging the model");
    Check(Read(*morph).size == 1 && Read(*m).size == 1, "morph enumeration excludes unrelated scene parents");
}
void SourceAndTargetOnlyParents() {
    ModelKeyFixture f; f.Marks(false, false, false); f.app->CurrentFrame() = -9;
    auto& root = f.first.m->displayKeys[0]; root.allocated = 1; root.selectorStates[0] = {1, 0};
    std::shared_ptr<plugins::DisplayKeyDeletion> record;
    Check(plugins::PlanSignedDisplayKeyDeletion(*f.app, 0, record, false, false) == MMS_OK && record,
        "plan deletion whose only registered parent belongs to the removed root payload");
    for (bool forward : {true, false}) {
        std::unique_ptr<plugins::PreparedDisplayKeyDeletion> candidate;
        Check(record->PrepareSigned(forward, candidate) == MMS_OK && candidate && candidate->IsCurrent(),
            "prepare source-only forward or target-only inverse parent lease");
        Check(Read(*record).size == 1 && Read(*candidate).size == 2,
            "source-only and target-only parents belong to candidates, not unretained record indices");
        CheckUnion(*record, *candidate);
        { NoAllocation guard; candidate->Publish(); }
    }
}
template<class Record, class Candidate> void Ordinary(const std::shared_ptr<plugins::HistoryChange>& base,
    std::size_t expected) {
    auto record = std::dynamic_pointer_cast<Record>(base);
    Check(bool(record), "shared non-deletion implementation still exposes its actual record interface");
    Check(Read(*record).size == expected, "non-deletion record visits all and only its existing holders");
    std::unique_ptr<plugins::PreparedHistoryChange> prepared;
    Check(base->Prepare(true, prepared) == MMS_OK && prepared && prepared->IsCurrent(),
        "ordinary preparation keeps the shared implementation concrete and usable");
    const auto* candidate = dynamic_cast<const Candidate*>(prepared.get());
    Check(candidate != nullptr, "ordinary candidate keeps the expected typed hierarchy");
    CheckUnion(*record, *candidate);
}
void SharedImplementations() {
    {
        ModelKeyFixture f;
        auto bone = RegistrationPlan(f, 0, 12);
        Ordinary<plugins::BoneKeyDeletion, plugins::PreparedBoneKeyDeletion>(bone, 1);
        std::shared_ptr<plugins::HistoryChange> display;
        Check(plugins::PlanDisplayRegistration(*f.app, 0, 12, display) == MMS_OK && display,
            "display registration still instantiates the shared deletion implementation");
        Ordinary<plugins::DisplayKeyDeletion, plugins::PreparedDisplayKeyDeletion>(display, 1);
    }
    {
        ModelPasteFamilyFixture f; ModelPasteFamilyPlans plans(f);
        Ordinary<plugins::DisplayKeyDeletion, plugins::PreparedDisplayKeyDeletion>(plans.displays, 2);
        Ordinary<plugins::BoneKeyDeletion, plugins::PreparedBoneKeyDeletion>(plans.bones, 2);
        auto bone = std::dynamic_pointer_cast<plugins::BoneKeyDeletion>(plans.bones);
        auto display = std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(plans.displays);
        Check(Read(*bone).Contains(Read(*display)), "bone paste transitively retains both display source/target parent holders");
        Ledger combined;
        bone->VisitRetainedModels(&combined, Ledger::Visit); combined.Complete(); const auto bytes = combined.Bytes();
        display->VisitRetainedModels(&combined, Ledger::Visit); combined.Complete();
        Check(combined.Bytes() == bytes, "separate bone/display records share one model and parent bookkeeping charge");
    }
    {
        ModelRangeFixture f;
        auto range = ModelRangePlan(f, ModelRangeOptions(7));
        Ordinary<plugins::ModelKeyDeletion, plugins::PreparedModelKeyDeletion>(range, 1);
    }
}
void DistinctPasteParents() {
    ModelPasteFamilyFixture f;
    struct ExtraModels {
        MMDApp& app; Fixture target, unrelated;
        explicit ExtraModels(MMDApp& a) : app(a) {
            app.ModelSlot(2) = target.raw(); app.ModelSlot(3) = unrelated.raw();
            plugins::SceneObjectIdentity(target.raw()); plugins::SceneObjectIdentity(unrelated.raw());
        }
        ~ExtraModels() {
            plugins::ForgetSceneObject(target.raw()); plugins::ForgetSceneObject(unrelated.raw());
            app.ModelSlot(2) = app.ModelSlot(3) = nullptr;
        }
    } extra(*f.app);
    // Registered source parents remain model 1; repeated clipboard parents
    // target model 2. Model 3 is present but unrelated to either direction.
    for (auto& frame : f.parents) for (auto& parent : frame)
        if (parent.modelIndex == 1) parent.modelIndex = 2;
    ModelPasteFamilyPlans plans(f);
    Ordinary<plugins::DisplayKeyDeletion, plugins::PreparedDisplayKeyDeletion>(plans.displays, 3);
    auto bone = std::dynamic_pointer_cast<plugins::BoneKeyDeletion>(plans.bones);
    auto display = std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(plans.displays);
    const auto owners = Read(*bone);
    Check(owners.size == 3 && owners.Contains(Read(*display)),
        "persistent bone/display records include distinct source and target parent holders");
    Owners expected;
    for (unsigned slot : {0u, 1u, 2u}) Owners::Visit(&expected, plugins::RetainSceneObject(*f.app, MMS_OBJECT_MODEL, slot));
    Check(owners.Contains(expected) && expected.Contains(owners),
        "persistent owner traversal excludes an unrelated published model");
    Ledger combined;
    bone->VisitRetainedModels(&combined, Ledger::Visit); combined.Complete(); const auto bytes = combined.Bytes();
    display->VisitRetainedModels(&combined, Ledger::Visit); combined.Complete();
    Check(combined.Bytes() == bytes, "one collector deduplicates shared edited model and both persistent parents");
}
}
void NativeModelDeleteRecordOwnerChecks() {
    model_delete_record_owner_checks::Aggregates();
    model_delete_record_owner_checks::FamiliesAndSourceParents();
    model_delete_record_owner_checks::SourceAndTargetOnlyParents();
    model_delete_record_owner_checks::SharedImplementations();
    model_delete_record_owner_checks::DistinctPasteParents();
}
