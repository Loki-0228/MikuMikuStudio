#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "../../src/plugins/history_runtime.hpp"
#include <cmath>

// Include after bone_definition_host_checks.inc for its raw static-table copy.
#if !MMD_ORIGINAL_PROFILE
namespace bone_kinematic_probe {
namespace {
constexpr int kFrames[] = {0, 5, 10, 15, 20, 25};
struct Oracle {
    std::array<std::vector<BoneMotionSample>, 6> samples;
};
bool Near(float a, float b) { return std::isfinite(a) && std::abs(a - b) < 2e-5f; }
bool Same(const BoneMotionSample& a, const BoneMotionSample& b) {
    for (unsigned i = 0; i < 3; ++i) if (!Near(a.position[i], b.position[i])) return false;
    for (unsigned i = 0; i < 4; ++i) if (!Near(a.rotation[i], b.rotation[i])) return false;
    for (unsigned i = 0; i < 9; ++i) if (!Near(a.scale.v[i], b.scale.v[i])) return false;
    return a.physicsDisabled == b.physicsDisabled;
}
Oracle ReadNativeOracle(unsigned char* raw) {
    auto& model = *mdl::Mdl(raw); Oracle result;
    for (unsigned t = 0; t < 6; ++t) {
        // Seek deliberately skips native type 7/9. A fresh independent pose
        // starts at bind locals instead of inheriting an edited live value.
        for (uint32_t b = 0; b < model.boneCount; ++b) {
            auto& bone = model.boneTable[b];
            std::memset(bone.trans, 0, sizeof(bone.trans)); std::memset(bone.rotQuat, 0, sizeof(bone.rotQuat));
            bone.rotQuat[3] = 1; bone.physicsDisabled = 0;
        }
        SeekModelFrame(raw, kFrames[t], 0);
        for (uint32_t b = 0; b < model.boneCount; ++b) {
            const auto& bone = model.boneTable[b]; BoneMotionSample sample;
            std::copy(bone.trans, bone.trans + 3, sample.position.begin());
            std::copy(bone.rotQuat, bone.rotQuat + 4, sample.rotation.begin());
            sample.scale = bone_scale::Pose(raw, int(b)); sample.physicsDisabled = bone.physicsDisabled != 0;
            result.samples[t].push_back(sample);
        }
    }
    return result;
}
bool Matches(const BoneMotionSnapshot& motion, const DisplayMotionSnapshot& display, const Oracle& oracle) {
    for (unsigned t = 0; t < 6; ++t) {
        if (motion.BoneCount() != oracle.samples[t].size()) return false;
        for (uint32_t b = 0; b < motion.BoneCount(); ++b) {
            BoneMotionSample sample;
            if (motion.PrepareKinematicChannels(int(b), kFrames[t], display, sample) != BoneMotionStatus::Ok ||
                !Same(sample, oracle.samples[t][b])) return false;
        }
    }
    return true;
}
void Workers(const BoneMotionSnapshot& old, const BoneMotionSnapshot& newer, const DisplayMotionSnapshot& display,
    const Oracle& original, const Oracle& edited) {
    bool a = true, b = true;
    std::thread first([&] { for (unsigned i = 0; i < 48; ++i) a = Matches(old, display, original) && a; });
    std::thread second;
    try { second = std::thread([&] { for (unsigned i = 0; i < 48; ++i) b = Matches(newer, display, edited) && b; }); }
    catch (...) { first.join(); throw; }
    first.join(); second.join(); Check(a && b, "workers preserve two registered local revisions and native seek parity");
}
std::vector<unsigned char> SourceBytes(const mdl::ModelRecord& model) {
    auto bytes = bone_definition_host_checks::SourceBytes(model);
    const auto append = [&](const void* data, size_t size) {
        if (!size) return;
        const auto* start = static_cast<const unsigned char*>(data); bytes.insert(bytes.end(), start, start + size);
    };
    append(model.boneKeys, mdl::BoneKeyCapacity(model) * sizeof(mdl::BoneKey));
    append(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys) * sizeof(mdl::DisplayKey));
    append(model.displayKeys[0].selectorStates, model.boneOrderCount * sizeof(mdl::BoneReference));
    append(model.displayKeys[0].ikStates, model.ikChainCount);
    if (model.boneKeyCursors) append(model.boneKeyCursors, model.boneCount * sizeof(uint32_t));
    if (model.boneTrackActive) append(model.boneTrackActive, model.boneCount);
    append(model.morphs, model.morphCount * sizeof(mdl::MorphRecord));
    return bytes;
}
struct Proof {
    MMDApp* app; unsigned char* raw;
    std::vector<unsigned char> bytes;
    std::unique_ptr<bone_curve::AuthoringState> curves;
    bone_scale::AuthoringState scale;
    bone_scale::Values pose;
    uint64_t scaleRevision = 0, historyRevision = 0, historyEpoch = 0, historyBytes = 0;
    size_t undo = 0, redo = 0;
    int frame = 0, modified = 0;
    Proof(MMDApp* a, unsigned char* source) : app(a), raw(source), bytes(SourceBytes(*mdl::Mdl(source))),
        curves(bone_curve::AuthoringState::Capture(source)), scale(bone_scale::AuthoringState::Capture(source)),
        pose(bone_scale::PoseValues(source)), scaleRevision(bone_scale::Revision(source)), frame(a->CurrentFrame()), modified(a->SceneModified()) {
        auto* h = plugins::NativeHistory(app); Check(h && h->Revision(historyRevision) == MMS_OK, "real common history is available for read-only proof");
        historyEpoch = h->NativeEpoch(); historyBytes = h->RetainedBytes(); undo = h->UndoCount(); redo = h->RedoCount();
    }
    void Verify() const {
        auto* h = plugins::NativeHistory(app); uint64_t revision = 0;
        Check(h && h->Revision(revision) == MMS_OK && revision == historyRevision && h->NativeEpoch() == historyEpoch &&
            h->RetainedBytes() == historyBytes && h->UndoCount() == undo && h->RedoCount() == redo, "sampling does not publish or discard history");
        Check(bytes == SourceBytes(*mdl::Mdl(raw)) && frame == app->CurrentFrame() && modified == app->SceneModified(),
            "capture and workers preserve current frame full native keys selectors and edited pose");
        Check(curves->Equals(*bone_curve::AuthoringState::Capture(raw)) && scale.Equals(bone_scale::AuthoringState::Capture(raw)) &&
            bone_scale::PoseValuesEqual(raw, pose) && bone_scale::Revision(raw) == scaleRevision, "capture preserves all scale and free-curve authors");
    }
};
}
void Run(MMDApp* app) {
    wchar_t path[32768]{}; const auto length = GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768);
    Check(length && length < 32768, "kinematic local fixture supplied"); LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw, "load actual PMX or PMD for local preparation");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount >= 5 && model.boneTable && model.ikChainCount && model.boneOrderCount > 1, "fixture has real IK and derived-type bones");
    int move = -1, ik = -1, tip = -1, corotate = -1;
    for (uint32_t b = 0; b < model.boneCount; ++b) {
        const auto& bone = model.boneTable[b];
        if (bone.type == mdl::BoneType::Move && bone.slotIndex >= 0) move = int(b);
        if (bone.type == mdl::BoneType::Ik && bone.slotIndex >= 0) ik = int(b);
        if (bone.type == mdl::BoneType::InertTip) tip = int(b);
        if (bone.type == mdl::BoneType::CoRotate) corotate = int(b);
    }
    Check(move >= 0 && ik >= 0 && tip >= 0 && (model.physicsMode == 2 || corotate >= 0), "loader preserved Move IK and fresh type7/9 coverage");
    Check(mdl::BoneKeyPoolEnsure(model, model.boneCount * 3ull), "reserve registered three-key tracks");
    for (uint32_t b = 0; b < model.boneCount; ++b) {
        const auto middle = model.boneCount + b * 2, last = middle + 1;
        const uint32_t slots[] = {b, middle, last};
        for (uint32_t keyIndex = 0; keyIndex < 3; ++keyIndex) {
            auto& key = model.boneKeys[slots[keyIndex]]; key = {};
            key.frame = keyIndex * 10; key.previous = keyIndex ? slots[keyIndex - 1] : 0;
            key.next = keyIndex < 2 ? slots[keyIndex + 1] : 0; key.allocated = 1;
            const float factor = keyIndex == 1 ? 1.f : keyIndex == 2 ? -.25f : 0.f;
            key.position[0] = float(b + 1) * factor; key.position[1] = -.5f * factor; key.position[2] = .25f * factor;
            const float half = float(b + 1) * .03f * factor;
            key.rotation[2] = std::sin(half); key.rotation[3] = std::cos(half);
            for (unsigned lane = 0; lane < 4; ++lane) {
                key.interpolation[lane] = key.interpolation[lane + 4] = 20;
                key.interpolation[lane + 8] = key.interpolation[lane + 12] = 107;
            }
            const float sx = keyIndex == 1 ? 1.5f : 1.f;
            bone_scale::SetKey(raw, int(slots[keyIndex]), {{sx, 0, 0, 0, 1, 0, 0, 0, 1}});
        }
    }
    auto& displayKey = model.displayKeys[0];
    displayKey.frame = displayKey.previous = displayKey.next = 0; displayKey.visible = 1;
    for (uint32_t i = 0; i < model.ikChainCount; ++i) displayKey.ikStates[i] = 1;
    for (uint32_t i = 0; i < model.boneOrderCount; ++i) displayKey.selectorStates[i] = {-1, 0};
    model.maxFrame = (std::max)(model.maxFrame, 20u);
    const auto original = ReadNativeOracle(raw);
    Check(std::abs(original.samples[2][ik].position[0]) > .5f, "native IK control key has nonzero local motion");
    const auto changedKey = model.boneCount + uint32_t(ik) * 2;
    const auto savedKey = model.boneKeys[changedKey]; model.boneKeys[changedKey].position[0] += 7;
    const auto edited = ReadNativeOracle(raw); model.boneKeys[changedKey] = savedKey;
    Check(!Same(original.samples[2][ik], edited.samples[2][ik]), "native oracle distinguishes the edited registered key");
    // Preserve a visibly different unregistered pose at capture time.
    for (uint32_t b = 0; b < model.boneCount; ++b) { model.boneTable[b].trans[0] = 123; model.boneTable[b].rotQuat[0] = .125f; }
    app->CurrentFrame() = 7; app->SceneModified() = 0;
    std::unique_ptr<BoneMotionSnapshot> old, newer; std::unique_ptr<DisplayMotionSnapshot> display;
    {
        const Proof proof(app, raw);
        display = DisplayMotionSnapshot::Capture(raw); old = BoneMotionSnapshot::Capture(raw);
        Check(display && old && Matches(*old, *display, original), "private local preparation matches independent native direct seeks");
        for (const int bone : {move, ik}) {
            BoneMotionSample refused; refused.position[0] = 456;
            Check(old->SampleChannels(bone, 10, refused) == BoneMotionStatus::ExternalParent && refused.position[0] == 456,
                "existing public local sampler still refuses selector-dependent bones without changing output");
        }
        proof.Verify();
    }
    model.boneKeys[changedKey].position[0] += 7;
    {
        const Proof proof(app, raw); newer = BoneMotionSnapshot::Capture(raw);
        Check(newer && Matches(*newer, *display, edited), "new capture sees registered edit while old capture stays frozen");
        Workers(*old, *newer, *display, original, edited); proof.Verify();
    }
    model.boneKeys[changedKey] = savedKey;
    const auto project = std::filesystem::absolute(L"bone-kinematic.pmm"); app->CurrentFrame() = 0;
    for (uint32_t b = 0; b < model.boneCount; ++b) {
        std::memset(model.boneTable[b].trans, 0, sizeof(model.boneTable[b].trans));
        std::memset(model.boneTable[b].rotQuat, 0, sizeof(model.boneTable[b].rotQuat)); model.boneTable[b].rotQuat[3] = 1;
    }
    SeekModelFrame(raw, 0, 0);
    Check(SaveSceneCopy(app, project.c_str()), "save original registered local tracks");
    ResetAppState(app); Check(!app->ModelSlot(0), "destroy original local-motion source");
    Workers(*old, *newer, *display, original, edited);
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile(); Check(app->ModelSlot(0), "reload actual saved local-motion project");
    {
        const Proof proof(app, app->ModelSlot(0)); Workers(*old, *newer, *display, original, edited);
        auto reloadedMotion = BoneMotionSnapshot::Capture(app->ModelSlot(0)); auto reloadedDisplay = DisplayMotionSnapshot::Capture(app->ModelSlot(0));
        Check(reloadedMotion && reloadedDisplay && Matches(*reloadedMotion, *reloadedDisplay, original), "reloaded original keys reproduce local-channel oracle");
        proof.Verify();
    }
    ResetAppState(app); Workers(*old, *newer, *display, original, edited);
    std::puts("Private bone local preparation: loaded selectors, native-seek parity, unchanged editing and frozen lifetime passed");
}
}
#endif
