#if !MMD_ORIGINAL_PROFILE
// Include after bone_evaluation_host_checks.inc and the scene API helpers.
namespace scene_unsupported_abi_probe {
using namespace mikumikustudio::plugins;

// Restore before assertions or deep proof reads. The original allocation must
// never reach the model destructor with a sentinel or invented active count.
struct MorphRestore {
    mdl::ModelRecord& model;
    uint32_t count;
    mdl::MorphRecord* table;
    mdl::MorphRecord first;
    explicit MorphRestore(mdl::ModelRecord& value)
        : model(value), count(value.morphCount), table(value.morphs) {
        std::memcpy(&first, table, sizeof(first));
    }
    ~MorphRestore() {
        model.morphCount = count;
        model.morphs = table;
        std::memcpy(table, &first, sizeof(first));
    }
};

void Run(MMDApp* app) {
    using Getter = MmsResult(MMS_CALL*)(MmsEvaluationApiV1_4*);
    const auto module = GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    const auto getter = module ? reinterpret_cast<Getter>(GetProcAddress(module, "MmsEvaluationHostGetApi14")) : nullptr;
    MmsEvaluationApiV1_4 api{};
    Check(getter && getter(&api) == MMS_OK, "unsupported-morph probe negotiates real plugin evaluation 1.4");
    MmsEvaluationSceneCapsV1 caps{};
    Check(api.getSceneCapabilities(api.context, &caps) == MMS_OK && (caps.flags & MMS_EVALUATION_SCENE_SKELETON),
        "real scene capture capability is available");
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "unsupported-morph fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0);
    Check(raw && !app->ModelSlot(1), "load one isolated real PMX or PMD model");
    auto& model = *mdl::Mdl(raw);
    Check(model.morphCount && model.morphs && model.boneCount, "fixture has actual morph and bone definitions");
    const bool pmx = model.physicsMode == 2;
    std::printf("MODEL %s\n", pmx ? "PMX" : "PMD");
    bone_evaluation_probe::Seed(raw);

    // Publish object and bone identities through the same BIND API used by
    // plugins, before any intentionally invalid live header is installed.
    auto sceneStore = CreateSceneStore(app);
    SceneClient sceneClient;
    auto scene = ([&]{ MmsSceneApiV1 value{}; if (sceneStore->Api(sceneClient, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    MmsSceneQueryV1 bindQuery{};
    bindQuery.h = Header<MmsSceneQueryV1>();
    bindQuery.stage = MMS_STAGE_BIND;
    bindQuery.flags = MMS_SCENE_CURRENT_TIME;
    bindQuery.channels = MMS_CHANNEL_BONES;
    MmsSnapshotV1 bind{};
    MmsSpan objects{};
    Check(scene.acquire(scene.context, &bindQuery, &bind) == MMS_OK &&
        scene.objects(scene.context, bind.handle, &objects) == MMS_OK && objects.count == 1,
        "BIND publishes one real model identity");
    const auto object = static_cast<const MmsObjectV1*>(objects.data)[0].object;
    MmsSpan bones{};
    Check(scene.readChannel(scene.context, bind.handle, object, MMS_CHANNEL_BONES, &bones) == MMS_OK &&
        bones.count == model.boneCount, "BIND publishes real bone identities");
    scene.release(scene.context, bind.handle);

    app->CurrentFrame() = 7;
    app->SceneModified() = 0;
    MmsEvaluationSceneQueryV1 query{};
    query.h = Header<MmsEvaluationSceneQueryV1>();
    MmsHandle baseline = 0;
    Check(api.createSceneSource(api.context, &query, &baseline) == MMS_OK,
        "unmodified loaded model is a valid skeleton scene source");
    MmsEvaluationSceneInfoV1 baselineInfo{};
    Check(api.describeSceneSource(api.context, baseline, &baselineInfo) == MMS_OK && baselineInfo.modelCount == 1,
        "baseline source contains the real model");

    constexpr MmsHandle sentinel = 0x3141592653589793ULL;
    const auto runCase = [&](const char* label, MmsResult expected, const auto& mutate, uint64_t budget = 0) {
        const bone_evaluation_probe::Proof proof(app, raw);
        MmsHandle output = sentinel;
        MmsResult result = MMS_E_FAILED;
        bool headersUnchanged = false;
        {
            MorphRestore restore(model);
            mutate(model, model.morphs[0]);
            const auto count = model.morphCount;
            const auto* table = model.morphs;
            mdl::MorphRecord first;
            // restore.table remains the original valid allocation even when
            // the tested model.morphs field is null or a sentinel pointer.
            std::memcpy(&first, restore.table, sizeof(first));
            auto request = query;
            request.maxRetainedBytes = budget;
            result = api.createSceneSource(api.context, &request, &output);
            headersUnchanged = model.morphCount == count && model.morphs == table &&
                !std::memcmp(restore.table, &first, sizeof(first));
        }
        // Proof walks valid, restored tables only, and includes native poses,
        // key pools, authoring state, history, playback time and modified flag.
        proof.Verify();
        Check(headersUnchanged, "capture preserved intentionally modified source headers before restoration");
        const bool outputOk = expected == MMS_OK ? output != 0 && output != sentinel : output == sentinel;
        MmsResult released = MMS_OK;
        if (result == MMS_OK && output != 0 && output != sentinel)
            released = api.release(api.context, output);
        std::printf("CASE %s result=%d expected=%d output-preserved=%d\n", label,
            int(result), int(expected), int(outputOk));
        Check(result == expected && outputOk && released == MMS_OK, label);
        proof.Verify();
    };

    if (pmx) {
        for (const unsigned type : {9u, 10u, 255u}) {
            const auto label = std::string("PMX unsupported type ") + std::to_string(type);
            runCase(label.c_str(), MMS_E_UNSUPPORTED, [=](auto&, auto& morph) { morph.type = uint8_t(type); });
        }
    } else {
        runCase("PMD inactive type 255 remains capturable", MMS_OK, [](auto&, auto& morph) {
            morph.type = 255;
            morph.impulseEntries = morph.impulse2Entries = nullptr;
        });
    }
    runCase("opaque impulse pointer is unsupported without dereferencing", MMS_E_UNSUPPORTED, [](auto&, auto& morph) {
        morph.impulseEntries = reinterpret_cast<unsigned char*>(uintptr_t(1));
    });
    runCase("opaque second impulse pointer is unsupported without dereferencing", MMS_E_UNSUPPORTED, [](auto&, auto& morph) {
        morph.impulse2Entries = reinterpret_cast<unsigned char*>(uintptr_t(1));
    });
    runCase("zero morph count does not dereference sentinel table", MMS_OK, [](auto& value, auto&) {
        value.morphCount = 0;
        value.morphs = reinterpret_cast<mdl::MorphRecord*>(uintptr_t(1));
    });
    runCase("morph count beyond signed range is stale before table walk", MMS_E_STALE, [](auto& value, auto&) {
        value.morphCount = 0x80000000u;
        value.morphs = reinterpret_cast<mdl::MorphRecord*>(uintptr_t(1));
    });
    runCase("nonempty null morph table is stale", MMS_E_STALE, [](auto& value, auto&) {
        value.morphCount = 1;
        value.morphs = nullptr;
    });
    runCase("huge morph count exceeds small budget before sentinel walk", MMS_E_MEMORY, [](auto& value, auto&) {
        value.morphCount = 0x7fffffffu;
        value.morphs = reinterpret_cast<mdl::MorphRecord*>(uintptr_t(1));
    }, 65536);

    // Exercise every PMX active payload family, not unrelated inactive fields.
    // PMD uses vertex offsets regardless of the raw type byte.
    const unsigned firstType = pmx ? 0u : 1u, lastType = pmx ? 8u : 1u;
    for (unsigned type = firstType; type <= lastType; ++type) {
        for (const bool missing : {false, true}) {
            const auto label = std::string(pmx ? "PMX" : "PMD") + " active type " + std::to_string(type) +
                (missing ? " positive count with null table is stale" : " negative count is stale");
            runCase(label.c_str(), MMS_E_STALE, [=](auto&, auto& morph) {
                morph.type = uint8_t(type);
                const int32_t count = missing ? 1 : -1;
                switch (type) {
                case 0: morph.groupCount = count; morph.groupEntries = nullptr; break;
                case 1: morph.offsetCount = count; morph.vertexEntries = nullptr; break;
                case 2: morph.boneCount = count; morph.boneEntries = nullptr; break;
                case 8: morph.materialCount = count; morph.materialEntries = nullptr; break;
                default: morph.uvCounts[type - 3] = count; morph.uvEntries[type - 3] = nullptr; break;
                }
            });
        }
    }

    // One known source plus 127 retained aliases must still fill exactly the
    // service's 128 client slots. Any hidden failed-publication handle reduces
    // this capacity. Retain shares payload bytes, avoiding a memory-quota test.
    const bone_evaluation_probe::Proof finalProof(app, raw);
    std::vector<MmsHandle> aliases;
    aliases.reserve(127);
    for (unsigned i = 0; i < 127; ++i) {
        MmsHandle alias = sentinel;
        Check(api.retain(api.context, baseline, &alias) == MMS_OK && alias != sentinel && alias != 0,
            "failed capture left no partial handle in client capacity");
        aliases.push_back(alias);
    }
    MmsHandle excess = sentinel;
    Check(api.retain(api.context, baseline, &excess) == MMS_E_BUSY && excess == sentinel,
        "client capacity boundary rejects atomically");
    for (const auto alias : aliases) Check(api.release(api.context, alias) == MMS_OK, "release capacity-check alias");
    Check(api.release(api.context, baseline) == MMS_OK, "release baseline source");
    MmsHandle restored = 0;
    Check(api.createSceneSource(api.context, &query, &restored) == MMS_OK && api.release(api.context, restored) == MMS_OK,
        "restored model remains capturable after all rejected requests");
    finalProof.Verify();
    ResetAppState(app);
    std::puts("Production scene unsupported-morph checks passed");
}
}
#endif
