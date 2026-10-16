// Include after the shared Fixture/Check definitions in native_edit_history_tests.
namespace {
bool SameResourceReport(const plugins::resource_budget::Report& a,
                        const plugins::resource_budget::Report& b) {
    return a.cpuBytes == b.cpuBytes && a.d3d9Bytes == b.d3d9Bytes &&
        a.totalBytes == b.totalBytes && a.missing == b.missing;
}
void SceneResourceBudgetChecks() {
    namespace rb = plugins::resource_budget;
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
        Check((undo.Result().missing & rb::UndoCapacity) && undo.Result().cpuBytes > grown.Result().cpuBytes,
            "legacy undo payload is charged and unknown reserved capacity stays explicit");
        wchar_t text[] = L"model"; f.m->pmxTextBuffers[0] = text;
        rb::Collector named; rb::Model(*f.m, named);
        Check((named.Result().missing & rb::RawTextCapacity) && named.Result().cpuBytes > undo.Result().cpuBytes,
            "raw text payload does not pretend visible string length is allocation capacity");
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
