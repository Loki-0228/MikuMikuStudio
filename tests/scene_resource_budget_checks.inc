// Include after the shared Fixture/Check definitions in native_edit_history_tests.
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/extended_names.hpp"
namespace {
void OwnedTextResourceChecks() {
    namespace rb = plugins::resource_budget;
    const auto baseline = owned_text::Count();
    {
        owned_text::Buffer text(owned_text::AllocateBytes(100));
        text.get()[0] = L'A'; text.get()[1] = 0;
        owned_text::Allocation allocation;
        Check(owned_text::Find(text.get(), allocation) && allocation.bytes == 100,
            "owned text retains allocation capacity beyond early NUL");
        rb::Collector c; c.RawText(text.get()); c.RawText(text.get());
        Check(c.Result().Complete() && c.Result().cpuBytes == 100 + allocation.registryBytes,
            "raw text counts capacity and registration node once");
        rb::Collector unknown; unknown.RawText(reinterpret_cast<const wchar_t*>(std::uintptr_t(1)));
        Check(unknown.Result().cpuBytes == 0 && (unknown.Result().missing & rb::RawTextCapacity),
            "unknown text pointer is never dereferenced to invent capacity");
        owned_text::Forget(text.get());
        Check(!owned_text::Find(text.get(), allocation), "forgotten text capacity is inaccessible");
        owned_text::Remember(text.get(), 40);
        Check(owned_text::Find(text.get(), allocation) && allocation.bytes == 40,
            "reused text address receives only its new capacity");
        failAfter = 0; text.reset(); failAfter = -1;
        Check(owned_text::Count() == baseline, "text release unregisters without allocation");
    }
    bool succeeded = false;
    for (long limit = 0; limit < 8 && !succeeded; ++limit) {
        wchar_t* text = nullptr; bool memory = false;
        failAfter = limit;
        try { text = owned_text::AllocateBytes(40); } catch (const std::bad_alloc&) { memory = true; }
        failAfter = -1;
        if (memory) Check(owned_text::Count() == baseline, "allocation or registration failure leaves no text entry");
        else { succeeded = true; owned_text::Delete(text); }
    }
    Check(succeeded && owned_text::Count() == baseline, "text allocation succeeds after all allocation failures");
    {
        wchar_t* destination = nullptr;
        Check(extended_names::Assign(destination, L"original"), "create tracked extended name");
        auto* before = destination;
        const std::wstring replacement(L"A\0rest", 6);
        failAfter = 0; const bool assigned = extended_names::Assign(destination, replacement); failAfter = -1;
        Check(!assigned && destination == before && wcscmp(destination, L"original") == 0 &&
            owned_text::Count() == baseline + 1, "name registration failure preserves old owning field");
        Check(extended_names::Assign(destination, replacement), "replace tracked extended name");
        owned_text::Allocation allocation;
        Check(owned_text::Find(destination, allocation) && allocation.bytes == 7 * sizeof(wchar_t) &&
            !owned_text::Find(before, allocation), "name replacement tracks full input capacity and unregisters old pointer");
        owned_text::Free(destination);
    }
    {
        auto model = std::make_unique<mdl::ModelRecord>();
        model->boneTable = static_cast<mdl::BoneRecord*>(std::calloc(1, sizeof(mdl::BoneRecord))); model->boneCount = 1;
        model->morphs = static_cast<mdl::MorphRecord*>(std::calloc(1, sizeof(mdl::MorphRecord))); model->morphCount = 1;
        model->rigidTable = static_cast<mdl::RigidRecord*>(std::calloc(1, sizeof(mdl::RigidRecord))); model->rigidCount = 1;
        model->jointTable = static_cast<mdl::JointRecord*>(std::calloc(1, sizeof(mdl::JointRecord))); model->jointCount = 1;
        Check(model->boneTable && model->morphs && model->rigidTable && model->jointTable, "allocate text ownership fixture");
        wchar_t** fields[] = {&model->pmxTextBuffers[0], &model->pmxTextBuffers[1], &model->pmxTextBuffers[2],
            &model->pmxTextBuffers[3], &model->boneTable[0].jpText, &model->boneTable[0].enText,
            &model->morphs[0].jpText, &model->morphs[0].enText, &model->rigidTable[0].jpText,
            &model->rigidTable[0].enText, &model->jointTable[0].jpText, &model->jointTable[0].enText};
        for (auto field : fields) { *field = owned_text::AllocateBytes(40); **field = 0; }
        Check(owned_text::Count() == baseline + 12, "all native owned text fields register capacity");
        ModelDispose(reinterpret_cast<unsigned char*>(model.get()));
        Check(owned_text::Count() == baseline, "native model disposal unregisters every owning text family");
    }
    Check(owned_text::Count() == baseline, "owned text resource checks leave registry unchanged");
}
bool SameResourceReport(const plugins::resource_budget::Report& a,
                        const plugins::resource_budget::Report& b) {
    return a.cpuBytes == b.cpuBytes && a.d3d9Bytes == b.d3d9Bytes &&
        a.totalBytes == b.totalBytes && a.missing == b.missing;
}
void SceneResourceBudgetChecks() {
    namespace rb = plugins::resource_budget;
    OwnedTextResourceChecks();
    {
        rb::Collector c;
        int a = 0, b = 0; c.Add(&a, 128, 1);
        const auto initial = c.Result();
        failAfter = 0;
        const auto covered = c.Covers([&](rb::Collector& out) { out.Add(&a, 64, 1); out.Add(&a, 128, 1); });
        failAfter = -1;
        Check(covered && SameResourceReport(initial, c.Result()), "resource ledger validates identities without allocation or changing its charge");
        Check(!c.Covers([&](rb::Collector& out) { out.Add(&a, 129, 1); }) &&
            !c.Covers([&](rb::Collector& out) { out.Add(&b, 1, 1); }) &&
            !c.Covers([&](rb::Collector& out) { out.MissingResource(rb::RawTextCapacity); }),
            "resource ledger rejects growth new identities and newly incomplete measurements");
        Check(c.Covers([&](rb::Collector& out) { out.Add(&a, 128, 1); }) && SameResourceReport(initial, c.Result()),
            "failed capacity validation preserves the prepared ledger for retry");
    }
    {
        int a = 0, b = 0;
        rb::Collector c;
        c.Add(&a, 8, 4); c.Add(&a, 4, 4); c.Add(&a, 16, 4);
        c.Add(&b, 3, 8, true); c.Add(&b, 2, 8, true);
        const auto before = c.Result();
        Check(before.cpuBytes == 64 && before.d3d9Bytes == 24 && before.totalBytes == 88 && before.Complete(),
              "resource collector deduplicates allocations at largest observed capacity and separates DX9");
        bool failed = false;
        try { c.Add(&a, UINT64_MAX, 2); } catch (const std::bad_alloc&) { failed = true; }
        Check(failed && SameResourceReport(c.Result(), before), "resource product overflow preserves report");
        rb::Collector maximum; maximum.Add(&a, UINT64_MAX, 1);
        const auto full = maximum.Result(); failed = false;
        try { maximum.Add(&b, 1, 1); } catch (const std::bad_alloc&) { failed = true; }
        Check(failed && SameResourceReport(maximum.Result(), full), "resource sum overflow preserves report");
        c.MissingResource(rb::NativeOpaque);
        Check(!c.Result().Complete() && (c.Result().missing & rb::NativeOpaque), "resource report exposes incomplete category");
    }
    {
        Fixture f;
        rb::Collector initial; rb::Model(*f.m, initial);
        const auto first = initial.Result();
        Check(first.Complete() && first.d3d9Bytes == 0 && first.cpuBytes >= sizeof(mdl::ModelRecord) +
            32 * sizeof(mdl::BoneKey) + 16 * sizeof(mdl::MorphKey) + 4 * (sizeof(mdl::DisplayKey) + 1 + sizeof(mdl::BoneReference)),
            "native fixture counts allocated key capacities and every display key payload without graphics");
        auto* separateIk = f.m->displayKeys[1].ikStates;
        f.m->displayKeys[1].ikStates = f.m->displayKeys[0].ikStates;
        rb::Collector alias; rb::Model(*f.m, alias);
        Check(alias.Result().cpuBytes + 1 == first.cpuBytes, "shared nested display allocation is charged once");
        f.m->displayKeys[1].ikStates = separateIk;
        const auto oldCapacity = mdl::BoneKeyCapacity(*f.m);
        Check(mdl::BoneKeyPoolEnsure(*f.m, oldCapacity + 1), "grow real bone-key pool for resource measurement");
        rb::Collector grown; rb::Model(*f.m, grown);
        Check(grown.Result().cpuBytes >= first.cpuBytes +
            (mdl::BoneKeyCapacity(*f.m) - oldCapacity) * sizeof(mdl::BoneKey),
            "native resource charge follows grown key-pool capacity");
        f.Entry(1, 2, 3);
        rb::Collector undo; rb::Model(*f.m, undo);
        Check((undo.Result().missing & rb::UndoCapacity) && undo.Result().cpuBytes == grown.Result().cpuBytes,
            "unregistered legacy undo reports unknown capacity without dereferencing or guessing size");
        wchar_t text[] = L"model"; f.m->pmxTextBuffers[0] = text;
        rb::Collector named; rb::Model(*f.m, named);
        Check((named.Result().missing & rb::RawTextCapacity) && named.Result().cpuBytes == undo.Result().cpuBytes,
            "unknown raw text reports missing capacity without inventing a payload size");
        f.m->pmxTextBuffers[0] = nullptr;
    }
    {
        struct UnknownShape final : btSphereShape { UnknownShape() : btSphereShape(btScalar(1)) {} } shape;
        rb::Collector c; rb::Shape(&shape, c);
        Check((c.Result().missing & rb::PhysicsStorage) && c.Result().cpuBytes != 0,
            "unknown derived Bullet shape exposes incomplete storage");
    }
    {
        Fixture f;
        auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->ModelSlot(0) = f.raw();
        auto object = plugins::RetainSceneObject(*app, MMS_OBJECT_MODEL, 0);
        rb::Report single{}, duplicate{};
        const auto digest = f.Digest();
        Check(object->MeasureResources(single) == MMS_OK && single.Complete(), "retained model exposes native resource measurement");
        Check(plugins::RetainedSceneObject::MeasureResourcesBatch({object, object}, duplicate) == MMS_OK &&
            SameResourceReport(single, duplicate), "batch resource measurement deduplicates repeated retained owner");
        Check(f.Digest() == digest, "resource measurement does not mutate native model");
        rb::Report output{11, 22, 33, rb::NativeOpaque}; const auto unchanged = output;
        failAfter = 0; const auto failed = object->MeasureResources(output); failAfter = -1;
        Check(failed == MMS_E_MEMORY && SameResourceReport(output, unchanged), "failed resource allocation leaves caller output intact");
        Check(plugins::RetainedSceneObject::MeasureResourcesBatch({}, output) == MMS_E_ARGUMENT && SameResourceReport(output, unchanged),
            "invalid resource batch leaves caller output intact");
        app->ModelSlot(0) = nullptr;
        Check(object->MeasureResources(output) == MMS_E_STALE && SameResourceReport(output, unchanged),
            "stale active resource owner rejects without replacing output");
        object.reset(); plugins::ForgetSceneObject(f.raw());
    }
}
}
