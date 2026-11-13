// Include at file scope after native_negative_copy_source_checks.inc.
// The parent TU owns routing to NativeModelCopySnapshotChecks(). These checks
// exercise preparation and dependency guards, not production batch admission.
#include "../src/plugins/native_model_copy.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
namespace model_copy_snapshot_checks {
namespace result = negative_copy_result_checks;
namespace negative = negative_copy_source_checks;
namespace receipt = global_copy_snapshot_checks;
using Snapshot = plugins::ModelCopySnapshot;
using Source = std::shared_ptr<const Snapshot>;
using Mode = plugins::SharedHistory::RecordingMode;
constexpr long allowanceMax = (std::numeric_limits<long>::max)();
static_assert(std::is_same<decltype(plugins::CaptureModelCopySnapshot(
    std::declval<MMDApp&>(), std::uint32_t{}, std::declval<Source&>())), MmsResult>::value,
    "model snapshot capture returns MmsResult");
static_assert(noexcept(plugins::CaptureModelCopySnapshot(std::declval<MMDApp&>(), 0, std::declval<Source&>())),
    "capture contains failures");
static_assert(noexcept(plugins::ModelCopySnapshotCurrent(std::declval<const MMDApp&>(), std::declval<const Snapshot&>())),
    "dependency checks never throw");

bool SamePayload(const plugins::ModelPasteSource& a, const plugins::ModelPasteSource& b) {
    if (a.frameOffset != b.frameOffset || a.advanceLastRegisteredFrame != b.advanceLastRegisteredFrame ||
        a.resetDisplayCursor != b.resetDisplayCursor || a.curveTargetsResolved != b.curveTargetsResolved ||
        a.boneCurveBlock != b.boneCurveBlock || a.morphCurveBlock != b.morphCurveBlock ||
        a.boneCurveTargets != b.boneCurveTargets || a.morphCurveTargets != b.morphCurveTargets ||
        a.bones.size() != b.bones.size() || a.morphs.size() != b.morphs.size() || a.displays.size() != b.displays.size()) return false;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const auto& x = a.bones[i]; const auto& y = b.bones[i];
        if (std::memcmp(&x.key, &y.key, sizeof(x.key)) || !(x.scale == y.scale) || x.scaleCurve != y.scaleCurve ||
            x.targetResolved != y.targetResolved || x.targetIndex != y.targetIndex) return false;
    }
    for (std::size_t i = 0; i < a.morphs.size(); ++i) {
        const auto& x = a.morphs[i]; const auto& y = b.morphs[i];
        if (std::memcmp(&x.key, &y.key, sizeof(x.key)) || x.targetResolved != y.targetResolved || x.targetIndex != y.targetIndex) return false;
    }
    for (std::size_t i = 0; i < a.displays.size(); ++i) {
        const auto& x = a.displays[i]; const auto& y = b.displays[i];
        if (x.frame != y.frame || x.visible != y.visible || x.ikTargetsResolved != y.ikTargetsResolved ||
            x.ikTargets != y.ikTargets || !receipt::Equal(x.ik, y.ik) || !receipt::Equal(x.selectors, y.selectors)) return false;
    }
    return true;
}
bool SameTrack(const plugins::ModelCopyTrack& a, const plugins::ModelCopyTrack& b) {
    return a.index == b.index && a.name == b.name && a.occurrence == b.occurrence && a.available == b.available;
}
bool SameMapping(const plugins::ModelCopyMapping& a, const plugins::ModelCopyMapping& b) {
    return a.tableIndex == b.tableIndex && a.mappedBone == b.mappedBone && a.name == b.name &&
        a.occurrence == b.occurrence && a.available == b.available;
}
bool SameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.owner != b.owner || a.sceneGeneration != b.sceneGeneration || a.nativeFirst != b.nativeFirst ||
        a.modelSlot != b.modelSlot || a.referencesAvailable != b.referencesAvailable || !SamePayload(a.payload, b.payload) ||
        a.bones.size() != b.bones.size() || a.morphs.size() != b.morphs.size() || a.displays.size() != b.displays.size()) return false;
    for (std::size_t i = 0; i < a.models.size(); ++i) {
        const auto& x = a.models[i]; const auto& y = b.models[i];
        if (x.slot != y.slot || x.pointer != y.pointer || !negative::SameIdentity(x.identity, y.identity) ||
            x.bones != y.bones || x.morphs != y.morphs || x.ikChains != y.ikChains || x.selectors != y.selectors ||
            x.boneCount != y.boneCount || x.morphCount != y.morphCount || x.ikCount != y.ikCount ||
            x.selectorCount != y.selectorCount || x.referenced != y.referenced || x.available != y.available || x.tracks != y.tracks) return false;
    }
    for (std::size_t i = 0; i < a.bones.size(); ++i) if (!SameTrack(a.bones[i], b.bones[i])) return false;
    for (std::size_t i = 0; i < a.morphs.size(); ++i) if (!SameTrack(a.morphs[i], b.morphs[i])) return false;
    for (std::size_t i = 0; i < a.displays.size(); ++i) {
        const auto& x = a.displays[i]; const auto& y = b.displays[i];
        if (x.ik.size() != y.ik.size() || x.selectors.size() != y.selectors.size()) return false;
        for (std::size_t j = 0; j < x.ik.size(); ++j) if (!SameMapping(x.ik[j], y.ik[j])) return false;
        for (std::size_t j = 0; j < x.selectors.size(); ++j) if (!SameMapping(x.selectors[j], y.selectors[j])) return false;
    }
    return true;
}

struct NativeFixture : result::ModelFixture {
    negative::Parent parent;
    std::array<mdl::MorphRecord,2> morphs{};
    std::array<mdl::BoneOrderEntry,2> selectors{};
    bone_scale::Matrix scale;
    bone_scale::Curve curve{{12,24,91,115}};
    explicit NativeFixture(Mode mode = Mode::AllOperations) : result::ModelFixture(mode) {
        plugins::SceneObjectIdentity(model.raw());
        app->ModelSlot(19) = parent.model.raw();
        model.m->morphs = morphs.data(); model.m->morphCount = 2;
        model.m->boneOrderTable = selectors.data(); model.m->boneOrderCount = 2;
        // Different bytes after NUL verify exact name20 ownership, while the
        // shared text must count unselected earlier duplicates as occurrence 0.
        for (unsigned i = 0; i < 2; ++i) {
            std::memset(model.bones[i].name, int('A' + i), 20); std::memcpy(model.bones[i].name, "dup", 4);
            std::memset(morphs[i].name, int('C' + i), 20); std::memcpy(morphs[i].name, "face", 5);
            morphs[i].panel = static_cast<mdl::MorphPanel>(1);
        }
        selectors[0].boneIndex = -1; selectors[1].boneIndex = 1;
        selectors[0].linkedModel = 19; selectors[0].linkedBone = 1;
        selectors[1].linkedModel = 0; selectors[1].linkedBone = 1;
        chains[0].boneIndex = 1;
        for (unsigned i = 0; i < 4; ++i) {
            delete[] model.m->displayKeys[i].selectorStates;
            model.m->displayKeys[i].selectorStates = new mdl::BoneReference[2]{};
        }
        ClearRealCopyMarks(model);
        auto& bone = model.m->boneKeys[1]; bone.allocated = 1; bone.frame = 9; bone.physicsDisabled = 1;
        for (unsigned i = 0; i < 3; ++i) bone.position[i] = float(i) + .25f;
        for (unsigned i = 0; i < 4; ++i) bone.rotation[i] = float(i + 1) / 8;
        for (unsigned i = 0; i < 16; ++i) bone.interpolation[i] = static_cast<unsigned char>(i * 3 + 1);
        for (unsigned i = 0; i < 9; ++i) scale.v[i] = float(i + 1) / 16;
        scale.v[0] = 2; scale.v[4] = 3; scale.v[8] = 4;
        bone_scale::SetKey(model.raw(), 1, scale); bone_scale::SetKeyCurve(model.raw(), 1, curve);
        auto& morph = model.m->morphKeys[1]; morph.allocated = 1; morph.frame = 14; morph.value = .625f;
        auto& display = model.m->displayKeys[0]; display.allocated = 1; display.frame = 18; display.visible = 0;
        display.ikStates[0] = 0;
        display.selectorStates[0].modelIndex = 19; display.selectorStates[0].boneIndex = 1;
        display.selectorStates[1].modelIndex = 0; display.selectorStates[1].boneIndex = 1;
        RealCopy();
        Check(app->ClipboardCounts().bones == 1 && app->ClipboardCounts().morphs == 1 && app->ClipboardCounts().displays == 1 &&
            frame_clipboard::BoneTarget(app.get(), model.raw(), 0) == 1 && frame_clipboard::MorphTarget(app.get(), model.raw(), 0) == 1,
            "real 420 copies only the second duplicate and installs original-track mappings");
    }
    void RealCopy() {
        const auto window = app->MainWindow(); app->MainWindow() = controls.host;
        CmdControl400(app.get(), controls.host, 420, 0);
        app->MainWindow() = window;
    }
    ~NativeFixture() {
        history.Reset(); history.Collect(); negative_frames::Reset(app.get());
        app->ModelSlot(19) = nullptr;
        model.m->morphs = model.morphs.data(); model.m->morphCount = 1;
        model.m->boneOrderTable = nullptr; model.m->boneOrderCount = 1;
    }
};

Source Capture(MMDApp& app, std::uint32_t first = 9) {
    Source out;
    Check(plugins::CaptureModelCopySnapshot(app, first, out) == MMS_OK && out,
        "preparation succeeds and returns immutable owned payload");
    return out;
}
void Current(MMDApp& app, const Source& source, bool expected) {
    Check(source && failAfter == -1 && !allocationAudit, "Current starts outside other allocation audits");
    AllocationAudit audit; allocationAudit = &audit; failAfter = 0;
    const bool value = plugins::ModelCopySnapshotCurrent(app, *source);
    const long remaining = failAfter; failAfter = -1; allocationAudit = nullptr;
    Check(value == expected && remaining == 0 && !audit.outstanding && !audit.overflow,
        "source guard returns the expected answer with allocation forbidden");
}
void RawMatches(NativeFixture& f, const Snapshot& source) {
    plugins::ModelPasteSource raw;
    Check(plugins::CaptureModelPastePayload(*f.app, raw) == MMS_OK && SamePayload(raw, source.payload),
        "snapshot owns every payload field and nested IK/selector byte from the existing payload capture");
    Check(source.payload.bones.size() == source.bones.size() && source.payload.morphs.size() == source.morphs.size() &&
        source.payload.displays.size() == source.displays.size(), "unavailable metadata never filters payload rows");
}
void Stamp(const plugins::ModelCopyModelReference& ref, int slot, Fixture& f, bool tracks) {
    MmsObjectRef identity{};
    Check(plugins::FindSceneObjectIdentity(f.raw(), identity) && negative::SameIdentity(identity, ref.identity) &&
        ref.referenced && ref.available && ref.slot == slot && ref.pointer == f.raw() && ref.tracks == tracks &&
        ref.bones == f.m->boneTable && ref.boneCount == f.m->boneCount && ref.morphs == f.m->morphs &&
        ref.morphCount == f.m->morphCount && ref.ikChains == f.m->ikChains && ref.ikCount == f.m->ikChainCount &&
        ref.selectors == f.m->boneOrderTable && ref.selectorCount == f.m->boneOrderCount,
        "model stamp retains an existing identity and all copy-time table tokens/counts");
}

void FieldsAndOwnership(Mode mode) {
    NativeFixture f(mode); const result::ModelLive live(f); const negative::RawClip raw(*f.app);
    auto source = Capture(*f.app); live.Expect(f); raw.Expect(*f.app); RawMatches(f, *source);
    Check(source->owner == f.app.get() && source->sceneGeneration == f.app->SceneGeneration() && source->nativeFirst == 9 &&
        source->modelSlot == 0 && source->referencesAvailable && source->payload.frameOffset == 0,
        "snapshot records source owner scene slot and shared native origin separately from destination");
    Stamp(source->models[0], 0, f.model, true); Stamp(source->models[19], 19, f.parent.model, false);
    for (unsigned i = 0; i < source->models.size(); ++i)
        Check(source->models[i].referenced == (i == 0 || i == 19), "only source and actual selector parents are referenced");
    const auto& bone = source->bones[0]; const auto& morph = source->morphs[0];
    Check(bone.index == 1 && bone.available && bone.occurrence == 1 && !std::memcmp(bone.name.data(), f.model.bones[1].name, 20) &&
        morph.index == 1 && morph.available && morph.occurrence == 1 && !std::memcmp(morph.name.data(), f.morphs[1].name, 20),
        "original indices name20 and full-table ordinals survive when only the second duplicate was copied");
    const auto& display = source->displays[0];
    Check(display.ik.size() == 1 && display.selectors.size() == 2 && display.ik[0].tableIndex == 0 &&
        display.ik[0].mappedBone == 1 && display.ik[0].available && display.ik[0].occurrence == 1 && display.ik[0].name == bone.name &&
        display.selectors[1].tableIndex == 1 && display.selectors[1].mappedBone == 1 && display.selectors[1].available &&
        display.selectors[1].occurrence == 1 && display.selectors[1].name == bone.name,
        "IK and nonroot selector retain their exact source table-to-bone mappings and duplicate ordinals");
    Check(display.selectors[0].tableIndex == 0 && display.selectors[0].available && display.selectors[0].mappedBone == -1 &&
        display.selectors[0].name == std::array<char,20>{} && display.selectors[0].occurrence == 0,
        "selector zero accepts the whole-model sentinel without inventing a bone name");
    Check(source->payload.bones[0].scale == f.scale && source->payload.bones[0].scaleCurve == f.curve &&
        source->payload.bones[0].key.frame == 0 && source->payload.morphs[0].key.frame == 5 && source->payload.displays[0].frame == 9,
        "payload includes every scale component and scale curve and preserves one shared frame origin");
    const auto saved = *source; Current(*f.app, source, true);
    f.app->CurrentFrame() = -123; f.app->SelectedModelSlot() = 19; f.app->CameraMode() = 1;
    ClearRealCopyMarks(f.model); negative_frames::SelectRange(f.app.get(), 1, 2);
    std::memset(f.model.bones[1].name, 'X', 20); std::memset(f.morphs[1].name, 'Y', 20);
    f.model.m->boneKeys[1].position[0] = 123; f.morphs[1].value = .125f;
    f.model.m->displayKeys[0].selectorStates[0].boneIndex = 999;
    f.selectors[0].linkedModel = -1; f.selectors[0].linkedBone = 999; f.chains[0].enabled = 1;
    Current(*f.app, source, true);
    Check(SameSnapshot(*source, saved), "later names values selection mode and playhead do not rewrite copy-time metadata");
    f.app->BoneClipboard()[0].position[0] = -77; f.app->BoneClipboard()[0].reserved1[2] = 71;
    f.app->MorphClipboard()[0].value = -.75f; f.app->DisplayClipboard()[0].visible = 1;
    f.app->DisplayClipboard()[0].ikStates[0].enabled = 1;
    f.app->DisplayClipboard()[0].selectorStates[0].modelIndex = 254;
    bone_scale::CopyClipboard(f.app.get(), 0, bone_scale::Matrix{}, false);
    frame_clipboard::Reset(f.app.get()); Current(*f.app, source, true);
    Check(SameSnapshot(*source, saved), "legacy payload scale and mapping replacement cannot mutate a held snapshot");
    f.app->SelectedModelSlot() = 0; f.app->CameraMode() = 0; f.RealCopy();
    auto empty = Capture(*f.app, 77); Current(*f.app, empty, true); Current(*f.app, source, true);
    Check(empty->payload.bones.empty() && empty->payload.morphs.empty() && empty->payload.displays.empty() &&
        empty->bones.empty() && empty->morphs.empty() && empty->displays.empty() && empty->nativeFirst == 77 && SameSnapshot(*source, saved),
        "actual empty copy releases native buffers but this preparation guard is independent of production batches");
    auto other = std::make_unique<MMDApp>(); Current(*other, source, false);
    f.app->AdvanceSceneGeneration(); Current(*f.app, source, false);
    Check(SameSnapshot(*source, saved), "scene retirement leaves all owned bytes readable");
}

void Structure(MMDApp& app, const Source& source, Fixture& f, bool tracks) {
    auto& m = *f.m;
    const auto bones = m.boneTable; m.boneTable = nullptr; Current(app, source, false); m.boneTable = bones;
    const auto boneCount = m.boneCount; ++m.boneCount; Current(app, source, false); m.boneCount = boneCount;
    const auto morphs = m.morphs; m.morphs = nullptr; Current(app, source, !tracks); m.morphs = morphs;
    const auto morphCount = m.morphCount; ++m.morphCount; Current(app, source, !tracks); m.morphCount = morphCount;
    const auto ik = m.ikChains; m.ikChains = nullptr; Current(app, source, !tracks); m.ikChains = ik;
    const auto ikCount = m.ikChainCount; ++m.ikChainCount; Current(app, source, !tracks); m.ikChainCount = ikCount;
    const auto selectors = m.boneOrderTable; m.boneOrderTable = nullptr; Current(app, source, !tracks); m.boneOrderTable = selectors;
    const auto selectorCount = m.boneOrderCount; ++m.boneOrderCount; Current(app, source, !tracks); m.boneOrderCount = selectorCount;
    Current(app, source, true);
}
void IdentityAndMappings() {
    NativeFixture f; auto source = Capture(*f.app); const auto saved = *source;
    Structure(*f.app, source, f.model, true); Structure(*f.app, source, f.parent.model, false);
    for (int changed : {0, 99}) {
        f.chains[0].boneIndex = changed; Current(*f.app, source, false); f.chains[0].boneIndex = 1; Current(*f.app, source, true);
        f.selectors[1].boneIndex = changed; Current(*f.app, source, false); f.selectors[1].boneIndex = 1; Current(*f.app, source, true);
    }
    f.selectors[0].boneIndex = 0; Current(*f.app, source, false); f.selectors[0].boneIndex = -1; Current(*f.app, source, true);
    for (int slot : {0, 19}) {
        auto* pointer = f.app->ModelSlot(slot);
        f.app->ModelSlot(slot) = nullptr; Current(*f.app, source, false);
        f.app->ModelSlot(slot) = reinterpret_cast<unsigned char*>(std::uintptr_t(1)); Current(*f.app, source, false);
        f.app->ModelSlot(slot) = pointer; Current(*f.app, source, true);
        plugins::ForgetSceneObject(pointer);
        // An absent identity must be rejected before any table read.
        auto* model = reinterpret_cast<mdl::ModelRecord*>(pointer); const auto bones = model->boneTable;
        model->boneTable = reinterpret_cast<mdl::BoneRecord*>(std::uintptr_t(1)); Current(*f.app, source, false);
        model->boneTable = bones; plugins::SceneObjectIdentity(pointer); Current(*f.app, source, false);
        source = Capture(*f.app); Current(*f.app, source, true);
    }
    auto transient = std::make_unique<negative::Parent>(); f.app->ModelSlot(19) = transient->model.raw();
    auto removed = Capture(*f.app); const auto removedBytes = *removed; Current(*f.app, removed, true);
    f.app->ModelSlot(19) = nullptr; transient.reset(); Current(*f.app, removed, false);
    Check(SameSnapshot(*removed, removedBytes), "destroyed parent is checked through slot and identity without touching cached storage");
    f.app->ModelSlot(19) = f.parent.model.raw();
    Check(SamePayload(source->payload, saved.payload), "identity refresh never modifies owned source payload");
}

void UnavailableDependencies() {
    NativeFixture f; auto live = Capture(*f.app); const auto saved = *live;
    const auto unavailable = [&]() {
        auto source = Capture(*f.app); RawMatches(f, *source); Current(*f.app, source, false);
        Check(!source->referencesAvailable && source->bones.size() == 1 && source->morphs.size() == 1 &&
            source->displays[0].ik.size() == 1 && source->displays[0].selectors.size() == 2,
            "unavailable dependency keeps all owned payload and parallel metadata rows");
        return source;
    };
    frame_clipboard::Reset(f.app.get()); auto noMapping = unavailable();
    Check(noMapping->bones[0].index == -1 && !noMapping->bones[0].available && noMapping->morphs[0].index == -1 &&
        !noMapping->morphs[0].available, "missing original mappings do not fall back to the first duplicate");
    f.RealCopy(); Current(*f.app, noMapping, false); Current(*f.app, live, true);
    const auto bones = f.model.m->boneTable; f.model.m->boneTable = nullptr;
    auto noBones = unavailable(); Stamp(noBones->models[19], 19, f.parent.model, false);
    f.model.m->boneTable = bones; Current(*f.app, noBones, false);
    f.model.m->morphCount = 1; auto shortMorphs = unavailable();
    Check(!shortMorphs->morphs[0].available && shortMorphs->bones[0].available, "missing morph does not erase other family metadata");
    f.model.m->morphCount = 2; Current(*f.app, shortMorphs, false);
    f.model.m->boneOrderTable = nullptr; auto noSelectors = unavailable();
    Stamp(noSelectors->models[19], 19, f.parent.model, false);
    f.model.m->boneOrderTable = f.selectors.data(); Current(*f.app, noSelectors, false);
    f.model.m->ikChains = nullptr; auto noIk = unavailable(); f.model.m->ikChains = f.chains.data(); Current(*f.app, noIk, false);
    f.chains[0].boneIndex = 99; auto invalidIk = unavailable();
    Check(invalidIk->displays[0].ik[0].mappedBone == 99 && !invalidIk->displays[0].ik[0].available,
        "out-of-range mapped bone remains explicit metadata"); f.chains[0].boneIndex = 1;
    f.selectors[1].boneIndex = -1; auto invalidSelector = unavailable(); f.selectors[1].boneIndex = 1;
    Check(!invalidSelector->displays[0].selectors[1].available, "whole-model sentinel is special only for selector zero");
    f.app->ModelSlot(19) = nullptr; auto missingParent = unavailable(); f.app->ModelSlot(19) = f.parent.model.raw();
    Check(missingParent->models[19].referenced && !missingParent->models[19].available,
        "a missing parent is retained as an unavailable copy-time reference"); Current(*f.app, missingParent, false);
    for (int slot : {0, 19}) {
        auto* pointer = f.app->ModelSlot(slot); plugins::ForgetSceneObject(pointer); MmsObjectRef absent{};
        auto missing = unavailable();
        Check(!plugins::FindSceneObjectIdentity(pointer, absent), "capture only finds identities and never creates a missing one");
        if (!slot) Stamp(missing->models[19], 19, f.parent.model, false);
        plugins::SceneObjectIdentity(pointer); Current(*f.app, missing, false);
    }
    auto& parentIndex = f.app->DisplayClipboard()[0].selectorStates[0].modelIndex;
    for (int invalid : {-2, 255, (std::numeric_limits<int>::max)()}) {
        parentIndex = invalid; auto source = unavailable();
        Check(source->payload.displays[0].selectors[0].modelIndex == invalid,
            "invalid parent slots remain byte-exact in payload instead of being filtered");
    }
    parentIndex = -1; auto detached = Capture(*f.app); Current(*f.app, detached, true);
    Check(!detached->models[19].referenced, "parent -1 is a deliberate detached reference");
    // Parent bone validity and numeric values belong to semantic admission.
    parentIndex = 19; f.app->DisplayClipboard()[0].selectorStates[0].boneIndex = 999;
    f.app->BoneClipboard()[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    auto semantic = Capture(*f.app); Current(*f.app, semantic, true);
    Check(std::isnan(semantic->payload.bones[0].key.position[0]) && semantic->payload.displays[0].selectors[0].boneIndex == 999,
        "raw numeric and parent-bone values are retained without claiming complete semantic admission");
    Check(SameSnapshot(*live, saved), "all failed dependencies leave the caller's earlier snapshot unchanged");
}

struct Attempt { MmsResult code = MMS_E_FAILED; long spent = 0; bool exhausted = false, overflow = false; std::size_t outstanding = 0; };
Attempt CaptureAt(MMDApp& app, Source& out, long allowance) {
    Check(failAfter == -1 && !allocationAudit, "capture audit starts outside other injections");
    AllocationAudit audit; Attempt attempt; allocationAudit = &audit; failAfter = allowance;
    attempt.code = plugins::CaptureModelCopySnapshot(app, 9, out);
    attempt.spent = allowance - failAfter; attempt.exhausted = failAfter == 0;
    failAfter = -1; allocationAudit = nullptr;
    attempt.outstanding = audit.outstanding; attempt.overflow = audit.overflow; return attempt;
}
void AllocationFailures(NativeFixture& f) {
    const result::ModelLive live(f); const negative::RawClip raw(*f.app);
    auto held = Capture(*f.app); const auto saved = *held; Source measured;
    const auto success = CaptureAt(*f.app, measured, allowanceMax);
    Check(success.code == MMS_OK && measured && success.spent > 0 && !success.exhausted && !success.overflow &&
        SameSnapshot(*held, *measured), "measure the actual successful model snapshot allocation path");
    live.Expect(f); raw.Expect(*f.app);
    for (long allowance = 0; allowance < success.spent; ++allowance) {
        Source out = held; const auto failure = CaptureAt(*f.app, out, allowance);
        Check(failure.code == MMS_E_MEMORY && !out && failure.exhausted && !failure.outstanding && !failure.overflow,
            "every measured OOM stage clears prepopulated out and releases partial owned payload and metadata");
        live.Expect(f); raw.Expect(*f.app); Current(*f.app, held, true);
        Check(SameSnapshot(*held, saved), "every allocation failure preserves the caller-held snapshot");
    }
    auto recovered = Capture(*f.app); Check(SameSnapshot(*recovered, saved), "preparation recovers after the full measured OOM sweep");
    live.Expect(f); raw.Expect(*f.app);
}
void Oom(Mode mode) {
    NativeFixture f(mode); AllocationFailures(f);
    ClearRealCopyMarks(f.model); f.RealCopy(); AllocationFailures(f);
}
void InvalidContextAndPayload() {
    NativeFixture f; auto previous = Capture(*f.app);
    f.app->ModelSlot(0) = nullptr;
    for (unsigned slot : {0u, 254u, 255u}) {
        f.app->SelectedModelSlot() = static_cast<std::uint8_t>(slot); Source out = previous;
        const negative::RawClip raw(*f.app); const auto result = CaptureAt(*f.app, out, 0);
        Check(result.code == MMS_E_NOT_READY && !out && !result.spent && !result.outstanding && !result.overflow,
            "missing or out-of-range model is NOT_READY before allocation and clears old out"); raw.Expect(*f.app);
    }
    f.app->SelectedModelSlot() = 0; f.app->ModelSlot(0) = f.model.raw();
    const auto clip = f.app->BoneClipboard(); f.app->BoneClipboard() = nullptr;
    Source out = previous; auto malformed = CaptureAt(*f.app, out, allowanceMax);
    Check(malformed.code == MMS_E_ARGUMENT && !out && !malformed.outstanding && !malformed.overflow,
        "malformed nonempty payload clears output and releases its partial envelope"); f.app->BoneClipboard() = clip;
    auto& display = f.app->DisplayClipboard()[0]; const auto count = display.selectorCount; display.selectorCount = -1;
    out = previous; malformed = CaptureAt(*f.app, out, allowanceMax);
    Check(malformed.code == MMS_E_ARGUMENT && !out && !malformed.outstanding && !malformed.overflow,
        "nested payload failure clears output after earlier families have allocated"); display.selectorCount = count;
    Current(*f.app, previous, true);
}
void OwnerDestruction() {
    Source source; Snapshot saved;
    { NativeFixture f; source = Capture(*f.app); saved = *source; }
    auto other = std::make_unique<MMDApp>(); Current(*other, source, false);
    Check(SameSnapshot(*source, saved), "snapshot owns its complete payload and metadata after app and models are destroyed");
}
}

void NativeModelCopySnapshotChecks() {
    using namespace model_copy_snapshot_checks;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) { FieldsAndOwnership(mode); Oom(mode); }
    IdentityAndMappings(); UnavailableDependencies(); InvalidContextAndPayload(); OwnerDestruction();
}
}
