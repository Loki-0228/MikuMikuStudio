#include "mikumikustudio/bone_key_pool.hpp"
#if !MMD_ORIGINAL_PROFILE
namespace evaluation_display_host_checks {
void Run(MMDApp* app) {
    using namespace mikumikustudio::plugins;
    const auto module = GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    using Getter = MmsResult(MMS_CALL*)(MmsEvaluationApiV1_2*);
    const auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "MmsEvaluationHostGetApi12"));
    MmsEvaluationApiV1_2 api{};
    Check(getter && getter(&api) == MMS_OK, "production plugin negotiates evaluation1.2");
    MmsEvaluationCapsV1 caps{};
    Check(api.getCapabilities(api.context, &caps) == MMS_OK && caps.flags == 7, "production host supports all registered channels");
    wchar_t path[32768]{}; const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "display evaluation PMX fixture supplied");
    const auto loadPair = [&] {
        LoadModelFile(app, path, false); LoadModelFile(app, path, false);
        Check(app->ModelSlot(0) && app->ModelSlot(1), "load source and external parent models");
    };
    loadPair();
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    Check(model.boneCount && model.morphCount && model.boneOrderCount, "fixture has bone morph and global selector");
    const auto prepare = [](mdl::ModelRecord& m) {
        Check(mdl::DisplayKeyPoolEnsure(m, 3), "prepare display evaluation key pool");
        for (uint32_t i = 0; i < 3; ++i) {
            auto& key = m.displayKeys[i];
            Check(key.selectorStates && (!m.ikChainCount || key.ikStates), "display payloads are owned");
            key.frame = i * 4; key.previous = i ? i - 1 : 0; key.next = i < 2 ? i + 1 : 0;
            key.visible = i == 1 ? 197 : 255; key.allocated = 0;
            for (uint32_t j = 0; j < m.ikChainCount; ++j) key.ikStates[j] = i == 1 ? 197 : 255;
            for (uint32_t j = 0; j < m.boneOrderCount; ++j) key.selectorStates[j] = {-1, 0};
            key.selectorStates[0] = i == 1 ? mdl::BoneReference{1, 0} : mdl::BoneReference{-1, int32_t(i + 9)};
        }
        m.maxFrame = (std::max)(m.maxFrame, 8u);
    };
    prepare(model);
    auto scene = CreateSceneStore(app); SceneClient client; auto bindApi = scene->Api(client);
    const auto acquire = [&] {
        MmsSceneQueryV1 q{}; q.h = Header<MmsSceneQueryV1>(); q.flags = MMS_SCENE_CURRENT_TIME; q.channels = MMS_CHANNEL_BONES;
        MmsSnapshotV1 bind{}; Check(bindApi.acquire(bindApi.context, &q, &bind) == MMS_OK, "publish both stable model identities"); return bind;
    };
    auto bind = acquire(); MmsSpan objects{}, targetBones{};
    Check(bindApi.objects(bindApi.context, bind.handle, &objects) == MMS_OK && objects.count == 2, "two model identities available");
    const auto sourceObject = static_cast<const MmsObjectV1*>(objects.data)[0].object;
    const auto targetObject = static_cast<const MmsObjectV1*>(objects.data)[1].object;
    Check(bindApi.readChannel(bindApi.context, bind.handle, targetObject, MMS_CHANNEL_BONES, &targetBones) == MMS_OK && targetBones.count,
        "target bone has public identity");
    const auto targetBone = static_cast<const MmsBoneV1*>(targetBones.data)[0].id;
    MmsEvaluationSourceQueryV1 query{}; query.h = Header<MmsEvaluationSourceQueryV1>(); query.object = sourceObject;
    app->CurrentFrame() = 3; app->SceneModified() = 0;
    const auto before = display_sampling_host_checks::SourceBytes(model);
    const auto targetBefore = display_sampling_host_checks::SourceBytes(*mdl::Mdl(app->ModelSlot(1)));
    MmsHandle display = 0, combined = 0;
    Check(api.createSourceChannels(api.context, &query, 4, &display) == MMS_OK &&
        api.createSourceChannels(api.context, &query, 7, &combined) == MMS_OK, "capture display-only and combined production sources");
    MmsEvaluationSourceInfoV1_2 info{}; MmsEvaluationSelectorV1 selector{};
    Check(api.describeSourceDisplay(api.context, display, &info) == MMS_OK && info.flags == 4 &&
        !info.boneCount && !info.morphCount && info.ikCount == model.ikChainCount && info.selectorCount == model.boneOrderCount,
        "display-only retains descriptors without capturing bone or morph motion");
    Check(api.selectorAt(api.context, display, 0, &selector) == MMS_OK && Empty(selector.bone) && selector.flags == MMS_EVALUATION_SELECTOR_GLOBAL,
        "global parent descriptor is not confused with bone zero");
    const auto read = [&](MmsHandle source) {
        MmsDisplayStateV1 state{}; MmsParentSelectionV1 parent{}, inactive{}, later{};
        return api.sampleVisibility(api.context, source, {5, 30}, &state) == MMS_OK && state.visible == 197 &&
            api.sampleParent(api.context, source, 0, {5, 30}, &parent) == MMS_OK && Same(parent.target, targetObject) &&
            Same(parent.bone, targetBone) && parent.flags == MMS_EVALUATION_PARENT_ACTIVE && parent.windowStart == 4 && parent.windowEnd == 8 &&
            api.sampleParent(api.context, source, 0, {0, 1}, &inactive) == MMS_OK && !inactive.flags && Empty(inactive.target.id) &&
            Empty(inactive.bone) && inactive.windowStart == 0 && inactive.windowEnd == 4 &&
            api.sampleParent(api.context, source, 0, {8, 30}, &later) == MMS_OK && !later.flags && later.windowStart == 8 && !later.windowEnd;
    };
    Check(read(display) && read(combined), "parent samples carry stable target identities and original byte/window values");
    Check(before == display_sampling_host_checks::SourceBytes(model) && targetBefore == display_sampling_host_checks::SourceBytes(*mdl::Mdl(app->ModelSlot(1))) &&
        app->CurrentFrame() == 3 && app->SceneModified() == 0, "capture leaves both models playback and edit state untouched");
    // A future bad reference is rejected even when the current frame is inactive.
    const auto savedParent = model.displayKeys[2].selectorStates[0]; model.displayKeys[2].selectorStates[0] = {100000, 0};
    MmsHandle rejected = 12345, unaffected = 0;
    const auto badDisplay = api.createSourceChannels(api.context, &query, 4, &rejected);
    const auto badCombined = api.createSourceChannels(api.context, &query, 7, &rejected);
    const auto goodOther = api.createSourceChannels(api.context, &query, 3, &unaffected);
    model.displayKeys[2].selectorStates[0] = savedParent;
    Check(badDisplay == MMS_E_STALE && badCombined == MMS_E_STALE && rejected == 12345 && goodOther == MMS_OK,
        "future stale target rejects selected display atomically without breaking other channels");
    Check(api.release(api.context, unaffected) == MMS_OK, "release unaffected source");
    const auto boneNext = model.boneKeys[0].next, morphNext = model.morphKeys[0].next;
    model.boneKeys[0].next = uint32_t(mdl::BoneKeyCapacity(model)); model.morphKeys[0].next = uint32_t(mdl::MorphKeyCapacity(model));
    const auto goodDisplay = api.createSourceChannels(api.context, &query, 4, &unaffected);
    const auto badOther = api.createSourceChannels(api.context, &query, 7, &rejected);
    model.boneKeys[0].next = boneNext; model.morphKeys[0].next = morphNext;
    Check(goodDisplay == MMS_OK && badOther == MMS_E_ARGUMENT && rejected == 12345, "display-only ignores malformed unrequested motion");
    Check(api.release(api.context, unaffected) == MMS_OK, "release isolated display source");
    const auto workerRead = [&] { bool valid = true; std::thread worker([&] { for (int i = 0; i < 128; ++i) valid = read(display) && read(combined) && valid; });
        worker.join(); Check(valid, "background parent reads never re-resolve model slots"); };
    workerRead();
    const auto project = std::filesystem::absolute(L"evaluation-display.pmm"); app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save project before destroying source and target");
    ResetAppState(app); workerRead();
    loadPair(); prepare(*mdl::Mdl(app->ModelSlot(0)));
    auto replacement = acquire(); MmsSpan changed{}; Check(bindApi.objects(bindApi.context, replacement.handle, &changed) == MMS_OK && changed.count == 2, "replacement slots have published identities");
    const auto newSource = static_cast<const MmsObjectV1*>(changed.data)[0].object, newTarget = static_cast<const MmsObjectV1*>(changed.data)[1].object;
    Check(!Same(newTarget, targetObject), "reused target slot gets a different identity");
    query.object = newSource; MmsHandle fresh = 0;
    Check(api.createSourceChannels(api.context, &query, 4, &fresh) == MMS_OK, "capture replacement model slots");
    MmsParentSelectionV1 freshParent{};
    Check(api.sampleParent(api.context, fresh, 0, {5, 30}, &freshParent) == MMS_OK && Same(freshParent.target, newTarget), "new source binds replacement target identity");
    workerRead(); Check(api.release(api.context, fresh) == MMS_OK, "release replacement source"); bindApi.release(bindApi.context, replacement.handle);
    ResetAppState(app); wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0) && app->ModelSlot(1), "reload actual saved project"); workerRead();
    Check(api.release(api.context, display) == MMS_OK && api.release(api.context, combined) == MMS_OK, "release original display sources");
    bindApi.release(bindApi.context, bind.handle); ResetAppState(app);
    std::puts("Production evaluation1.2: stable parent identities, channel isolation and replacement lifetime passed");
}
}
#endif
