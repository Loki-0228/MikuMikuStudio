#include "mikumikustudio/morph_evaluation.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <cmath>
#include <stdexcept>

// Include after morph_definition_host_checks.inc: its source-byte and direct
// native-definition oracle helpers are shared, never another evaluated frame.
#if !MMD_ORIGINAL_PROFILE
namespace morph_evaluation_probe {
namespace {
struct Expected {
    bool pmx = false;
    uint8_t physicsMode = 0;
    mdl::MaterialMorphChannels baseMaterial{};
    std::array<float, 4> baseUv0{};
};
bool Near(float a, float b) { return std::isfinite(a) && std::abs(a - b) < 1e-5f; }
float Phase(double frame) { return float(frame <= 10 ? frame / 10 : (20 - frame) / 10); }
float Peak(uint32_t morph, bool pmx, bool revised) {
    if (!pmx) return morph == 0 ? 100.f : morph == 1 ? (revised ? 4.f : 2.f) : 0.f;
    if (morph == 0) return 2.f;
    if (morph == 1) return revised ? 2.f : 1.f;
    if (morph == 2 || morph == 8) return .5f;
    return 2.f;
}
bool VerifyFrame(const MorphEvaluationFrame& result, const Expected& expected, float phase, bool revised) {
    if (result.PhysicsMode() != expected.physicsMode || !result.RetainedBytes()) return false;
    if (expected.pmx) {
        uint32_t index = 999; std::array<float, 3> position{};
        const float vertexWeight = (revised ? 1.5f : .5f) * phase; // Direct weight plus 2 * (.25 - .5).
        if (result.PositionCount() != 1 || !result.PositionIndexAt(0, index) || index != 2 ||
            !result.PositionAt(2, position) || !Near(position[0], .25f * vertexWeight) ||
            !Near(position[1], 3.f - .5f * vertexWeight) || !Near(position[2], .75f * vertexWeight)) return false;
        std::array<float, 3> absent{{77, 88, 99}};
        if (result.PositionAt(0, absent) || absent != std::array<float, 3>{{77, 88, 99}}) return false;
        for (uint32_t family = 0; family < 5; ++family) {
            std::array<float, 4> uv{};
            if (result.UvCount(family) != 1 || !result.UvIndexAt(family, 0, index) || index != 1 || !result.UvAt(family, 1, uv)) return false;
            const std::array<float, 4> base = family ? std::array<float, 4>{{float(family), float(family) + .125f,
                float(family) + .25f, float(family) + .375f}} : expected.baseUv0;
            if (!Near(uv[0], base[0] + float(family + 3) * .1f * 2.f * phase) ||
                !Near(uv[1], base[1] - .2f * 2.f * phase)) return false;
            if (family) {
                if (!Near(uv[2], base[2] + .3f * 2.f * phase) || !Near(uv[3], base[3] - .4f * 2.f * phase)) return false;
            } else if (std::memcmp(&uv[2], &base[2], 2 * sizeof(float))) return false;
        }
        mdl::BoneMorphOffsetRecord bone{};
        if (result.BoneCount() != 1 || !result.BoneAt(0, bone) || bone.boneIndex != 0 ||
            !Near(bone.translation[0], .25f * phase) || !Near(bone.translation[1], .5f * phase) ||
            !Near(bone.translation[2], -.125f * phase) || !Near(bone.rotation[0], 0) ||
            !Near(bone.rotation[1], 0) || !Near(bone.rotation[2], 0) || !Near(bone.rotation[3], 1)) return false;
        MorphMaterialEvaluation material{};
        if (result.MaterialCount() != 1 || !result.MaterialAt(0, material) ||
            std::memcmp(&material.base, &expected.baseMaterial, sizeof(material.base))) return false;
        for (uint32_t i = 0; i < 4; ++i) {
            // File material operation 197 follows the native additive path.
            const float diffuse = .01f * float(i + 1) * .5f * phase;
            if (!Near(material.additive.diffuse[i], diffuse) || !Near(material.multiplicative.diffuse[i], 1) ||
                !Near(material.properties.diffuse[i], expected.baseMaterial.diffuse[i] + diffuse) ||
                !Near(material.additive.textureTint[i], .01f * float(17 + i) * .5f * phase) ||
                !Near(material.multiplicative.textureTint[i], 1)) return false;
        }
    } else {
        static const float positions[4][3] = {{-6, 4, 0}, {-6, 16, 0}, {6, 4, 0}, {6, 16, 0}};
        if (result.PositionCount() != 4 || result.BoneCount() || result.MaterialCount()) return false;
        for (uint32_t vertex = 0; vertex < 4; ++vertex) {
            uint32_t index = 999; std::array<float, 3> position{};
            if (!result.PositionIndexAt(vertex, index) || index != vertex || !result.PositionAt(vertex, position)) return false;
            const float delta = vertex == 3 ? .125f * (revised ? 4.f : 2.f) * phase : 0;
            if (!Near(position[0], positions[vertex][0] + delta) || !Near(position[1], positions[vertex][1]) ||
                !Near(position[2], positions[vertex][2])) return false;
        }
        for (uint32_t family = 0; family < 5; ++family) if (result.UvCount(family)) return false;
    }
    return true;
}
std::unique_ptr<MorphEvaluationFrame> EvaluateAt(const MorphDefinitionSnapshot& definitions,
    const morph_curve::SamplingState& motion, double frame, bool pmx, bool revised) {
    std::vector<float> weights(motion.MorphCount());
    for (uint32_t i = 0; i < motion.MorphCount(); ++i)
        if (!motion.Sample(int(i), frame, weights[i]) || !Near(weights[i], Peak(i, pmx, revised) * Phase(frame)))
            throw std::runtime_error("registered morph sampling disagrees with linear key oracle");
    return MorphEvaluationFrame::Evaluate(definitions, weights.data(), uint32_t(weights.size()));
}
void Workers(const MorphDefinitionSnapshot& definitions, const morph_curve::SamplingState& old,
    const morph_curve::SamplingState& newer, const Expected& expected) {
    bool firstOk = true, secondOk = true;
    const auto run = [&](bool& ok, bool revised) {
        try {
            const auto& motion = revised ? newer : old;
            for (unsigned repeat = 0; repeat < 4; ++repeat) for (const double frame : {0., 5., 10., 15., 20.}) {
                auto result = EvaluateAt(definitions, motion, frame, expected.pmx, revised);
                ok = result && VerifyFrame(*result, expected, Phase(frame), revised) && ok;
            }
        } catch (...) { ok = false; }
    };
    std::thread first([&] { run(firstOk, false); }); std::thread second;
    try { second = std::thread([&] { run(secondOk, true); }); } catch (...) { first.join(); throw; }
    first.join(); second.join();
    Check(firstOk && secondOk, "worker sampled registered motion produces independently calculated morph output");
}
std::vector<unsigned char> SourceBytes(const mdl::ModelRecord& model) {
    auto bytes = morph_definition_probe::SourceBytes(model);
    const auto append = [&](const void* source, size_t size) {
        if (!size) return;
        const auto* begin = static_cast<const unsigned char*>(source); bytes.insert(bytes.end(), begin, begin + size);
    };
    append(model.morphKeys, mdl::MorphKeyCapacity(model) * sizeof(mdl::MorphKey));
    if (model.morphKeyCursors) append(model.morphKeyCursors, model.morphCount * sizeof(uint32_t));
    if (model.morphTrackActive) append(model.morphTrackActive, model.morphCount);
    append(model.boneTable, model.boneCount * sizeof(mdl::BoneRecord));
    append(model.materials, model.materialCount * sizeof(mdl::ModelMaterialRecord));
    if (model.rawVertices) append(model.rawVertices, model.vertexCount * sizeof(mdl::PmdVertex));
    if (model.pmxVertices) append(model.pmxVertices, model.vertexCount * sizeof(mdl::PmxVertex));
    return bytes;
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{}; const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "morph evaluation fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load actual morph evaluation model");
    auto& model = *mdl::Mdl(raw);
    Expected expected; expected.pmx = model.physicsMode == 2; expected.physicsMode = model.physicsMode;
    Check(model.morphCount == (expected.pmx ? 9u : 5u) && model.morph0Count, "fixture has all expected registered morph types and restore vertices");
    if (expected.pmx) {
        Check(model.materialMorphPools.base && model.uvMorphCounts.byFamily[0] == 1 && model.pmxAdditionalUvCount == 4,
            "PMX contains material and all UV restore definitions");
        expected.baseMaterial = model.materialMorphPools.base[0].channels;
        std::copy(model.uvMorphTables.byFamily[0][0].offset, model.uvMorphTables.byFamily[0][0].offset + 4, expected.baseUv0.begin());
    }
    Check(mdl::MorphKeyPoolEnsure(model, model.morphCount * 3ull), "reserve real registered motion keys");
    for (uint32_t i = 0; i < model.morphCount; ++i) {
        const auto middle = model.morphCount + i * 2, last = middle + 1;
        model.morphKeys[i] = {}; model.morphKeys[middle] = {}; model.morphKeys[last] = {};
        model.morphKeys[i].next = middle; model.morphKeys[i].allocated = 1;
        model.morphKeys[middle].frame = 10; model.morphKeys[middle].previous = i; model.morphKeys[middle].next = last;
        model.morphKeys[middle].value = Peak(i, expected.pmx, false); model.morphKeys[middle].allocated = 1;
        model.morphKeys[last].frame = 20; model.morphKeys[last].previous = middle; model.morphKeys[last].allocated = 1;
        if (model.morphKeyCursors) model.morphKeyCursors[i] = i;
        if (model.morphTrackActive) model.morphTrackActive[i] = 0;
        model.morphs[i].value = -.375f; // Unregistered edit must not supply sampled weights.
    }
    model.maxFrame = (std::max)(model.maxFrame, 20u);
    app->CurrentFrame() = 7; app->SceneModified() = 0;
    const auto originalDefinitions = morph_definition_probe::ReadExpected(model);
    const auto before = SourceBytes(model);
    morph_curve::AuthoredLayers layers; std::string error;
    Check(morph_curve::CaptureLayers(raw, layers, error), "capture authoring layers for read-only proof");
    auto definitions = MorphDefinitionSnapshot::Capture(raw);
    auto old = morph_curve::SamplingState::Capture(raw);
    Check(definitions && old && morph_definition_probe::Matches(*definitions, originalDefinitions), "freeze actual definitions and registered weights");
    Check(before == SourceBytes(model) && morph_curve::LayersUnchanged(raw, layers) && app->CurrentFrame() == 7 && !app->SceneModified(),
        "both captures preserve native state and authoring layers");
    const auto changedKey = model.morphCount + 2;
    const auto savedWeight = model.morphKeys[changedKey].value;
    model.morphKeys[changedKey].value = Peak(1, expected.pmx, true);
    const auto pending = SourceBytes(model);
    std::unique_ptr<morph_curve::SamplingState> newer;
    try { newer = morph_curve::SamplingState::Capture(raw); }
    catch (...) { model.morphKeys[changedKey].value = savedWeight; throw; }
    const bool unchanged = pending == SourceBytes(model); model.morphKeys[changedKey].value = savedWeight;
    Check(newer && unchanged && before == SourceBytes(model), "native key edit yields a new frozen motion and is restored without side effects");
    Workers(*definitions, *old, *newer, expected);
    auto retainedOld = EvaluateAt(*definitions, *old, 10, expected.pmx, false);
    auto retainedNew = EvaluateAt(*definitions, *newer, 10, expected.pmx, true);
    Check(retainedOld && retainedNew && VerifyFrame(*retainedOld, expected, 1, false) && VerifyFrame(*retainedNew, expected, 1, true),
        "old and new registered motion produce distinct hand-calculated positions");
    Check(before == SourceBytes(model) && morph_curve::LayersUnchanged(raw, layers) && app->CurrentFrame() == 7 && !app->SceneModified() &&
        morph_definition_probe::Matches(*definitions, originalDefinitions), "background evaluation changes neither source nor immutable definitions");
    const auto project = std::filesystem::absolute(L"morph-evaluation.pmm"); app->CurrentFrame() = 0;
    for (uint32_t i = 0; i < model.morphCount; ++i) model.morphs[i].value = 0;
    Check(SaveSceneCopy(app, project.c_str()), "save restored registered motion in a real PMM");
    ResetAppState(app); Check(!app->ModelSlot(0), "destroy registered motion source");
    Workers(*definitions, *old, *newer, expected);
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0), "reload actual PMM containing the registered tracks");
    Workers(*definitions, *old, *newer, expected);
    auto reloadDefinitions = MorphDefinitionSnapshot::Capture(app->ModelSlot(0));
    auto reloadMotion = morph_curve::SamplingState::Capture(app->ModelSlot(0));
    auto reloadFrame = EvaluateAt(*reloadDefinitions, *reloadMotion, 10, expected.pmx, false);
    Check(reloadFrame && VerifyFrame(*reloadFrame, expected, 1, false), "saved original native keys reproduce the original frozen result");
    ResetAppState(app); definitions.reset(); old.reset(); newer.reset(); reloadDefinitions.reset(); reloadMotion.reset();
    bool stillValid = false;
    std::thread resultReader([&] {
        stillValid = VerifyFrame(*retainedOld, expected, 1, false) && VerifyFrame(*retainedNew, expected, 1, true) &&
            VerifyFrame(*reloadFrame, expected, 1, false);
    });
    resultReader.join(); Check(stillValid, "owned results remain readable after all source models and input snapshots are destroyed");
    std::puts("Frozen morph evaluation host: registered motion, hand-calculated output, read-only workers and independent lifetime passed");
}
}
#endif
