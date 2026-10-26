#include <mms/evaluation.h>
#include "mikumikustudio/bone_key_pool.hpp"
namespace evaluation_morph_host_checks {
void Run(MMDApp* app) {
    using namespace mikumikustudio::plugins;
    const auto module = GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    Check(module != nullptr, "production runtime loads evaluation1.1 probe");
    using Getter = MmsResult(MMS_CALL*)(MmsEvaluationApiV1_1*);
    const auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "MmsEvaluationHostGetApi11"));
    MmsEvaluationApiV1_1 api{};
    Check(getter && getter(&api) == MMS_OK && api.h.minor == 1, "real plugin receives evaluation1.1 table");
    MmsEvaluationCapsV1 caps{};
    Check(api.getCapabilities(api.context, &caps) == MMS_OK && caps.flags == 3, "production host advertises bone and morph channels");
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "combined evaluation PMX fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load actual combined evaluation model");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount && model.morphCount, "fixture has both channel kinds");
    auto scene = CreateSceneStore(app); SceneClient client; auto bindApi = scene->Api(client);
    MmsSceneQueryV1 request{}; request.h = Header<MmsSceneQueryV1>();
    request.flags = MMS_SCENE_CURRENT_TIME; request.channels = MMS_CHANNEL_BONES | MMS_CHANNEL_MORPHS;
    MmsSnapshotV1 bind{};
    Check(bindApi.acquire(bindApi.context, &request, &bind) == MMS_OK, "acquire both stable channel identities");
    MmsSpan objects{}, morphs{}, bones{};
    Check(bindApi.objects(bindApi.context, bind.handle, &objects) == MMS_OK && objects.count == 1, "read model identity");
    const auto object = static_cast<const MmsObjectV1*>(objects.data)[0].object;
    Check(bindApi.readChannel(bindApi.context, bind.handle, object, MMS_CHANNEL_MORPHS, &morphs) == MMS_OK && morphs.count,
        "read public morph identities");
    Check(bindApi.readChannel(bindApi.context, bind.handle, object, MMS_CHANNEL_BONES, &bones) == MMS_OK && bones.count,
        "read public bone identities");
    const auto morph = static_cast<const MmsMorphV1*>(morphs.data)[0];
    const auto bone = static_cast<const MmsBoneV1*>(bones.data)[0];
    animation_curve::NamedTrack track;
    track.id = morph_curve::StableId(0, model.morphs[0].name);
    animation_curve::Key a, b; b.frame = 4; a.outSlope = 4; b.inSlope = -4;
    a.tangent = b.tangent = animation_curve::Tangent::Free; track.track.keys = {a, b};
    std::string error;
    Check(morph_curve::Write(raw, {track}, error), "author real morph curve for public evaluation");
    model.morphs[0].value = -.375f; app->CurrentFrame() = 7; app->SceneModified() = 0;
    const morph_sampling_host_checks::Unchanged original(app, raw);
    MmsEvaluationSourceQueryV1 query{Header<MmsEvaluationSourceQueryV1>(), object, 0, 0, 0};
    MmsHandle combined = 0, onlyMorph = 0, legacy = 0;
    Check(api.createSourceChannels(api.context, &query, 3, &combined) == MMS_OK, "capture both channels in one public source");
    Check(api.createSourceChannels(api.context, &query, 2, &onlyMorph) == MMS_OK, "capture morph-only source");
    Check(api.createSource(api.context, &query, &legacy) == MMS_OK, "legacy prefix still captures only bones");
    MmsEvaluationSourceInfoV1_1 info{};
    Check(api.describeSourceChannels(api.context, combined, &info) == MMS_OK && info.flags == 3 &&
        info.boneCount == model.boneCount && info.morphCount == model.morphCount && info.h.minor == 1,
        "combined source describes both channel counts");
    const auto firstCapture = info.captureId;
    MmsEvaluationMorphV1 descriptor{};
    Check(api.morphAt(api.context, combined, 0, &descriptor) == MMS_OK && Same(descriptor.id, morph.id) &&
        descriptor.kind == morph.kind && descriptor.flags == morph.flags, "evaluation morph metadata equals public BIND descriptor");
    MmsMorphWeightV1 value{};
    Check(api.sampleMorph(api.context, combined, morph.id, {2, 30}, &value) == MMS_OK && std::abs(value.value - 4) < 1e-4f,
        "public morph sampling returns actual free-curve overshoot");
    MmsBoneChannelsV1 boneValue{};
    Check(api.sampleBone(api.context, onlyMorph, bone.id, {0, 1}, &boneValue) == MMS_E_UNSUPPORTED &&
        api.sampleMorph(api.context, legacy, morph.id, {0, 1}, &value) == MMS_E_UNSUPPORTED, "unrequested channels are explicitly unsupported");
    original.Verify();

    // A broken unrequested channel must not alter the old API's capture scope.
    const auto oldMorphNext = model.morphKeys[0].next;
    model.morphKeys[0].next = uint32_t(mdl::MorphKeyCapacity(model));
    MmsHandle unaffected = 0, rejected = 12345;
    const auto boneOnly = api.createSource(api.context, &query, &unaffected);
    const auto failedCombined = api.createSourceChannels(api.context, &query, 3, &rejected);
    model.morphKeys[0].next = oldMorphNext;
    Check(boneOnly == MMS_OK && failedCombined == MMS_E_ARGUMENT && rejected == 12345,
        "bad morph chain does not break legacy bone capture; combined publication is atomic");
    Check(api.release(api.context, unaffected) == MMS_OK, "release unaffected bone-only source");
    const auto oldBoneNext = model.boneKeys[0].next;
    model.boneKeys[0].next = uint32_t(mdl::BoneKeyCapacity(model));
    const auto morphOnly = api.createSourceChannels(api.context, &query, 2, &unaffected);
    const auto badBoth = api.createSourceChannels(api.context, &query, 3, &rejected);
    model.boneKeys[0].next = oldBoneNext;
    Check(morphOnly == MMS_OK && badBoth == MMS_E_ARGUMENT && rejected == 12345,
        "bad bone chain does not break morph-only capture");
    Check(api.release(api.context, unaffected) == MMS_OK, "release unaffected morph-only source");
    original.Verify();

    const auto endpoint = model.morphKeys[0].next;
    Check(endpoint && model.morphKeys[endpoint].frame == 4, "native morph endpoint exists");
    model.morphKeys[endpoint].value = 12;
    const morph_sampling_host_checks::Unchanged pending(app, raw);
    MmsHandle newer = 0;
    Check(api.createSourceChannels(api.context, &query, 3, &newer) == MMS_OK, "capture pending native edit through public source");
    Check(api.describeSourceChannels(api.context, newer, &info) == MMS_OK && info.captureId != firstCapture,
        "fresh combined capture has a distinct identity");
    const auto readOldAndNew = [&] {
        bool valid = true;
        std::thread worker([&] {
            for (int i = 0; i < 128; ++i) {
                MmsMorphWeightV1 oldWeight{}, newWeight{};
                if (api.sampleMorph(api.context, combined, morph.id, {2, 30}, &oldWeight) != MMS_OK ||
                    api.sampleMorph(api.context, newer, morph.id, {2, 30}, &newWeight) != MMS_OK ||
                    std::abs(oldWeight.value - 4) > 1e-4f || std::abs(newWeight.value - 6) > 1e-4f) valid = false;
            }
        });
        worker.join(); Check(valid, "worker reads distinct immutable morph captures through C ABI");
    };
    readOldAndNew(); pending.Verify();
    const auto project = std::filesystem::absolute(L"evaluation-morph.pmm");
    app->CurrentFrame() = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save actual project before source deletion");
    ResetAppState(app); readOldAndNew();
    Check(api.createSourceChannels(api.context, &query, 2, &rejected) == MMS_E_STALE && rejected == 12345,
        "deleted object cannot create a new morph source");
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0) != nullptr, "reload actual PMM source generation");
    readOldAndNew();
    Check(api.createSourceChannels(api.context, &query, 3, &rejected) == MMS_E_STALE,
        "old object generation cannot capture replacement source");
    Check(api.release(api.context, combined) == MMS_OK && api.release(api.context, onlyMorph) == MMS_OK &&
        api.release(api.context, legacy) == MMS_OK && api.release(api.context, newer) == MMS_OK,
        "release all production channel leases");
    bindApi.release(bindApi.context, bind.handle);
    ResetAppState(app);
    std::puts("Production evaluation1.1: channels, identities, isolated capture and morph lifetime passed");
}
}
